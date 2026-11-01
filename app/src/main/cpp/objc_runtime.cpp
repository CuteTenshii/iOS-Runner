#include "objc_runtime.h"

#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

#include "engine.h"
#include "macho.h"
#include "swift_abi.h"
#include "symbols.h"

namespace {

constexpr uint32_t kRelativeMethodList = 0x80000000;
constexpr uint32_t kMethodListEntrySizeMask = 0xffff;
constexpr size_t kRoBaseMethodsOffset = 32;
constexpr size_t kClassSymbolPrefixLength = sizeof("_OBJC_CLASS_$_") - 1;
constexpr size_t kMetaclassSymbolPrefixLength = sizeof("_OBJC_METACLASS_$_") - 1;

std::map<const void*, std::string>& native_classes() {
    static std::map<const void*, std::string> classes;
    return classes;
}

std::string framework_class_name(const void* object) {
    if (const auto found = native_classes().find(object); found != native_classes().end()) return found->second;
    const char* symbol = unresolved_symbol_at(reinterpret_cast<uintptr_t>(object));
    if (symbol == nullptr) return {};
    if (strncmp(symbol, "_OBJC_CLASS_$_", kClassSymbolPrefixLength) == 0) return symbol + kClassSymbolPrefixLength;
    if (strncmp(symbol, "_OBJC_METACLASS_$_", kMetaclassSymbolPrefixLength) == 0) return symbol + kMetaclassSymbolPrefixLength;
    return symbol;
}

const void* find_in_method_list(const char* list, const char* selector) {
    if (list == nullptr) return nullptr;
    uint32_t entry_flags, count;
    memcpy(&entry_flags, list, sizeof entry_flags);
    memcpy(&count, list + 4, sizeof count);
    const size_t entry_size = entry_flags & kMethodListEntrySizeMask;
    const char* entries = list + 8;
    for (uint32_t i = 0; i < count; ++i) {
        const char* entry = entries + i * entry_size;
        const char* name;
        const void* implementation;
        if (entry_flags & kRelativeMethodList) {
            int32_t name_offset, implementation_offset;
            memcpy(&name_offset, entry, sizeof name_offset);
            memcpy(&implementation_offset, entry + 8, sizeof implementation_offset);
            name = *reinterpret_cast<const char* const*>(entry + name_offset);
            implementation = entry + 8 + implementation_offset;
        } else {
            memcpy(&name, entry, sizeof name);
            memcpy(&implementation, entry + 16, sizeof implementation);
        }
        if (strcmp(name, selector) == 0) return implementation;
    }
    return nullptr;
}

// Category methods on guest classes, newest last. Like the real runtime, they win over the class's own.
std::unordered_map<const void*, std::vector<const char*>>& category_method_lists() {
    static std::unordered_map<const void*, std::vector<const char*>> lists;
    return lists;
}

const void* find_guest_method(const char* cls, const char* selector, std::string& framework_root) {
    while (cls != nullptr) {
        const std::string framework = framework_class_name(cls);
        if (!framework.empty()) {
            framework_root = framework;
            return nullptr;
        }
        if (const auto extra = category_method_lists().find(cls); extra != category_method_lists().end()) {
            for (auto list = extra->second.rbegin(); list != extra->second.rend(); ++list) {
                if (const void* found = find_in_method_list(*list, selector)) return found;
            }
        }
        uintptr_t data;
        memcpy(&data, cls + kClassDataOffset, sizeof data);
        const auto* read_only = reinterpret_cast<const char*>(data & kClassDataMask);
        const char* methods;
        memcpy(&methods, read_only + kRoBaseMethodsOffset, sizeof methods);
        if (const void* found = find_in_method_list(methods, selector)) return found;
        memcpy(&cls, cls + kClassSuperclassOffset, sizeof cls);
    }
    return nullptr;
}

constexpr uint32_t kRoFlagMeta = 1;
constexpr size_t kRoNameOffset = 24;

// A framework metaclass is only a name, so a guest subclass's own metaclass is the one to ask.
bool is_metaclass(const char* cls) {
    if (cls == nullptr || !framework_class_name(cls).empty()) return false;
    uintptr_t data;
    memcpy(&data, cls + kClassDataOffset, sizeof data);
    uint32_t flags;
    memcpy(&flags, reinterpret_cast<const char*>(data & kClassDataMask), sizeof flags);
    return (flags & kRoFlagMeta) != 0;
}

std::unordered_map<std::string, void*>& guest_classes() {
    static std::unordered_map<std::string, void*> classes;
    return classes;
}

// "_TtC9SideStore11AppDelegate" reads "SideStore.AppDelegate"; nested or generic names keep their mangled form.
std::string readable_class_name(const std::string& raw) {
    if (raw.rfind("_TtC", 0) != 0) return raw;
    std::string out;
    size_t at = 4;
    for (int part = 0; part < 2; ++part) {
        size_t length = 0;
        const size_t digits = at;
        while (at < raw.size() && isdigit(static_cast<unsigned char>(raw[at]))) length = length * 10 + static_cast<size_t>(raw[at++] - '0');
        if (at == digits || at + length > raw.size()) return raw;
        if (part == 1) out += '.';
        out += raw.substr(at, length);
        at += length;
    }
    return at == raw.size() ? out : raw;
}

std::string raw_class_name(const void* cls) {
    uintptr_t data;
    memcpy(&data, static_cast<const char*>(cls) + kClassDataOffset, sizeof data);
    const char* name;
    memcpy(&name, reinterpret_cast<const char*>(data & kClassDataMask) + kRoNameOffset, sizeof name);
    return name;
}

std::unordered_map<std::string, const void*>& framework_methods() {
    static std::unordered_map<std::string, const void*> table;
    return table;
}

std::unordered_map<std::string, const void* (*)(const char*)>& framework_fallbacks() {
    static std::unordered_map<std::string, const void* (*)(const char*)> table;
    return table;
}

[[noreturn]] void stop_unknown_message(const std::string& description) {
    engine_log("STOP: unimplemented message %s", description.c_str());
    abort();
}

bool g_uikit_active = false;
const void* (*g_uikit_fallback)(const char*) = nullptr;

// Every framework class inherits NSObject's root methods.
const void* lookup_framework_method(const std::string& description) {
    const auto found = framework_methods().find(description);
    if (found != framework_methods().end()) return found->second;
    const size_t space = description.find(' ');
    const auto fallback = framework_fallbacks().find(description.substr(2, space - 2));
    if (fallback != framework_fallbacks().end()) {
        const std::string selector = description.substr(space + 1, description.size() - space - 2);
        if (const void* generated = fallback->second(selector.c_str())) return generated;
    }
    const std::string root = description.substr(0, 2) + "NSObject" + description.substr(space);
    const auto inherited = framework_methods().find(root);
    if (inherited != framework_methods().end()) return inherited->second;
    // Location and notification objects keep their state here in every app; SwiftUI apps don't use this for UIKit.
    const bool generic_class = description.compare(2, 2, "UN") == 0 || description.compare(2, 2, "CL") == 0 || (g_uikit_active && (description.compare(2, 2, "UI") == 0 || description.compare(2, 2, "CA") == 0 || description.compare(2, 8, "NSLayout") == 0 || description.compare(2, 12, "NSPersistent") == 0 || description.compare(2, 12, "NSAttributed") == 0 || description.compare(2, 14, "NSMutableAttri") == 0 || description.compare(2, 17, "NSMutableParagraph") == 0));
    if (generic_class && g_uikit_fallback != nullptr) {
        const std::string selector = description.substr(space + 1, description.size() - space - 2);
        if (const void* generic = g_uikit_fallback(selector.c_str())) return generic;
    }
    // UIKit only describes presentation, which the host draws itself: nil is a valid answer to all of it.
    if (description.compare(2, 2, "UI") == 0 || generic_class) {
        static std::set<std::string> logged;
        if (logged.insert(description).second) engine_log("UIKIT: ignoring %s", description.c_str());
        return reinterpret_cast<const void*>(+[](void*) -> void* { return nullptr; });
    }
    stop_unknown_message(description);
}

// A class message reaches instance methods too, as the root metaclass's superclass is the root class itself.
const void* lookup_inherited_method(const char* cls, const std::string& framework_root, const char* selector) {
    const std::string instance = "-[" + framework_root + " " + selector + "]";
    if (is_metaclass(cls)) {
        const std::string class_method = "+[" + framework_root + " " + selector + "]";
        if (framework_methods().count(class_method) != 0 || framework_methods().count(instance) == 0) return lookup_framework_method(class_method);
    }
    return lookup_framework_method(instance);
}

extern "C" const void* rt_objc_lookup(void* receiver, const char* selector) {
    static std::recursive_mutex lock;
    const std::lock_guard guard(lock);
    const std::string direct = framework_class_name(receiver);
    if (!direct.empty()) return lookup_framework_method("+[" + direct + " " + selector + "]");

    const char* cls;
    memcpy(&cls, receiver, sizeof cls);
    std::string framework_root;
    if (const void* method = find_guest_method(cls, selector, framework_root)) return method;
    if (!framework_root.empty()) return lookup_inherited_method(cls, framework_root, selector);
    char detail[96];
    snprintf(detail, sizeof detail, "receiver %p, class word %p", receiver, static_cast<const void*>(cls));
    stop_unknown_message(std::string("-[? ") + selector + "] on " + detail);
}

extern "C" const void* rt_objc_super_lookup(void* const* super_info, const char* selector) {
    // objc_msgSendSuper2 receives the current class: its superclass is where the search starts.
    const char* current;
    memcpy(&current, &super_info[1], sizeof current);
    const char* start;
    memcpy(&start, current + kClassSuperclassOffset, sizeof start);
    std::string framework_root;
    if (const void* method = find_guest_method(start, selector, framework_root)) return method;
    if (!framework_root.empty()) return lookup_inherited_method(current, framework_root, selector);
    stop_unknown_message(std::string("unknown superclass -[? ") + selector + "]");
}

constexpr size_t kRoInstanceStartOffset = 4;
constexpr size_t kRoInstanceVariablesOffset = 48;
constexpr size_t kIvarListHeaderSize = 8;
constexpr size_t kSwiftInstanceSizeOffset = 0x30;
constexpr uintptr_t kClassIsSwiftBits = 3;
constexpr uint32_t kRefcountWordSize = sizeof(void*);

bool has_objc_root(const char* cls) {
    while (cls != nullptr) {
        const std::string framework = framework_class_name(cls);
        if (!framework.empty()) return framework.find("SwiftObject") == std::string::npos;
        memcpy(&cls, cls + kClassSuperclassOffset, sizeof cls);
    }
    return false;
}

void add_to_u32(char* address, uint32_t amount) {
    uint32_t value;
    memcpy(&value, address, sizeof value);
    value += amount;
    memcpy(address, &value, sizeof value);
}

// Objects keep their refcount in the word after the class, where real ObjC instances start their ivars.
// Like the ObjC runtime does when a superclass grows, this moves every ivar past it.
void slide_instance_variables(char* cls) {
    uintptr_t data;
    memcpy(&data, cls + kClassDataOffset, sizeof data);
    auto* read_only = reinterpret_cast<char*>(data & kClassDataMask);
    add_to_u32(read_only + kRoInstanceStartOffset, kRefcountWordSize);
    add_to_u32(read_only + kRoInstanceSizeOffset, kRefcountWordSize);
    if (data & kClassIsSwiftBits) add_to_u32(cls + kSwiftInstanceSizeOffset, kRefcountWordSize);
    char* ivars;
    memcpy(&ivars, read_only + kRoInstanceVariablesOffset, sizeof ivars);
    if (ivars == nullptr) return;
    uint32_t entry_size, count;
    memcpy(&entry_size, ivars, sizeof entry_size);
    memcpy(&count, ivars + sizeof entry_size, sizeof count);
    for (uint32_t i = 0; i < count; ++i) {
        char* offset_variable;
        memcpy(&offset_variable, ivars + kIvarListHeaderSize + i * entry_size, sizeof offset_variable);
        if (offset_variable != nullptr) add_to_u32(offset_variable, kRefcountWordSize);
    }
}

extern "C" void rt_objc_msgSend();
extern "C" void rt_objc_msgSendSuper2();

constexpr size_t kFrameworkInstanceSize = 64;

void* framework_alloc(const std::string& name) {
    static std::map<std::string, void*> classes;
    auto& cls = classes[name];
    if (cls == nullptr) cls = objc_native_class(strdup(name.c_str()), free);
    auto** object = static_cast<void**>(calloc(1, kFrameworkInstanceSize));
    object[0] = cls;
    object[1] = reinterpret_cast<void*>(static_cast<uintptr_t>(1));
    return object;
}

// A Swift class with fields of resilient types learns its ivar offsets only when its metadata initializes, which
// the real runtime triggers on first use of the class. Generic classes arrive here already instantiated.
void initialize_swift_class(void* cls) {
    constexpr size_t kClassDescriptionOffset = 0x40, kDescriptorAccessorOffset = 12;
    constexpr uint32_t kDescriptorIsGeneric = 0x80;
    static std::set<void*> initialized;
    if (!initialized.insert(cls).second) return;
    uintptr_t data;
    memcpy(&data, static_cast<char*>(cls) + kClassDataOffset, sizeof data);
    if (!(data & kClassIsSwiftBits)) return;
    const char* descriptor;
    memcpy(&descriptor, static_cast<char*>(cls) + kClassDescriptionOffset, sizeof descriptor);
    if (descriptor == nullptr) return;
    uint32_t flags;
    memcpy(&flags, descriptor, sizeof flags);
    if (flags & kDescriptorIsGeneric) return;
    int32_t offset;
    memcpy(&offset, descriptor + kDescriptorAccessorOffset, sizeof offset);
    if (offset == 0) return;
    using Accessor = void* (*)(size_t);
    reinterpret_cast<Accessor>(const_cast<char*>(descriptor + kDescriptorAccessorOffset + offset))(0);
}

extern "C" void* rt_objc_allocWithZone(void* cls, void*) {
    const std::string framework = framework_class_name(cls);
    if (!framework.empty()) return framework_alloc(framework);
    initialize_swift_class(cls);
    uintptr_t data;
    memcpy(&data, static_cast<char*>(cls) + kClassDataOffset, sizeof data);
    uint32_t size;
    memcpy(&size, reinterpret_cast<char*>(data & kClassDataMask) + kRoInstanceSizeOffset, sizeof size);
    auto** object = static_cast<void**>(calloc(1, size));
    object[0] = cls;
    object[1] = reinterpret_cast<void*>(static_cast<uintptr_t>(1));
    return object;
}

}  // namespace

// Saves every argument register around the lookup, so the method sees exactly the caller's call.
#define OBJC_DISPATCH(name, lookup, nil_check, finish) \
    ".globl " #name "\n" #name ":\n" nil_check         \
    "  stp x29, x30, [sp, #-208]!\n"                   \
    "  mov x29, sp\n"                                  \
    "  stp x0, x1, [sp, #16]\n"                        \
    "  stp x2, x3, [sp, #32]\n"                        \
    "  stp x4, x5, [sp, #48]\n"                        \
    "  stp x6, x7, [sp, #64]\n"                        \
    "  str x8, [sp, #80]\n"                            \
    "  stp d0, d1, [sp, #96]\n"                        \
    "  stp d2, d3, [sp, #112]\n"                       \
    "  stp d4, d5, [sp, #128]\n"                       \
    "  stp d6, d7, [sp, #144]\n"                       \
    "  bl " #lookup                                    \
    "\n"                                               \
    "  mov x16, x0\n"                                  \
    "  ldp x0, x1, [sp, #16]\n"                        \
    "  ldp x2, x3, [sp, #32]\n"                        \
    "  ldp x4, x5, [sp, #48]\n"                        \
    "  ldp x6, x7, [sp, #64]\n"                        \
    "  ldr x8, [sp, #80]\n"                            \
    "  ldp d0, d1, [sp, #96]\n"                        \
    "  ldp d2, d3, [sp, #112]\n"                       \
    "  ldp d4, d5, [sp, #128]\n"                       \
    "  ldp d6, d7, [sp, #144]\n"                       \
    "  ldp x29, x30, [sp], #208\n" finish              \
    "  br x16\n"

__asm__(
    ".text\n" OBJC_DISPATCH(rt_objc_msgSend, rt_objc_lookup,
                            "  cbnz x0, 1f\n"
                            "  mov x1, xzr\n"
                            "  movi d0, #0\n"
                            "  movi d1, #0\n"
                            "  ret\n"
                            "1:\n",
                            "")
    // x0 points at {receiver, class}; the method itself must see the receiver.
    OBJC_DISPATCH(rt_objc_msgSendSuper2, rt_objc_super_lookup, "", "  ldr x0, [x0]\n"));

namespace {

struct CategoryRecord {
    const char* name;
    char* cls;
    const char* instance_methods;
    const char* class_methods;
};

// Copies a list's methods into a framework class's table, which is searched by full name.
void register_framework_category_methods(const std::string& class_name, const char* list, const char* prefix) {
    if (list == nullptr) return;
    uint32_t entry_flags, count;
    memcpy(&entry_flags, list, sizeof entry_flags);
    memcpy(&count, list + 4, sizeof count);
    const size_t entry_size = entry_flags & kMethodListEntrySizeMask;
    for (uint32_t i = 0; i < count; ++i) {
        const char* entry = list + 8 + i * entry_size;
        const char* name;
        const void* implementation;
        if (entry_flags & kRelativeMethodList) {
            int32_t name_offset, implementation_offset;
            memcpy(&name_offset, entry, sizeof name_offset);
            memcpy(&implementation_offset, entry + 8, sizeof implementation_offset);
            name = *reinterpret_cast<const char* const*>(entry + name_offset);
            implementation = entry + 8 + implementation_offset;
        } else {
            memcpy(&name, entry, sizeof name);
            memcpy(&implementation, entry + 16, sizeof implementation);
        }
        framework_methods()[std::string(prefix) + class_name + " " + name + "]"] = implementation;
    }
}

const MachSection* objc_section(const MachImage& image, const char* name) {
    const MachSection* section = image.section("__DATA_CONST", name);
    return section != nullptr ? section : image.section("__DATA", name);
}

void attach_categories(const MachImage& image) {
    const MachSection* list = objc_section(image, "__objc_catlist");
    if (list == nullptr) return;
    for (size_t offset = 0; offset + sizeof(void*) <= list->size; offset += sizeof(void*)) {
        const CategoryRecord* category;
        memcpy(&category, reinterpret_cast<const char*>(list->address + offset), sizeof category);
        if (category->cls == nullptr) continue;
        const std::string framework = framework_class_name(category->cls);
        if (!framework.empty()) {
            register_framework_category_methods(framework, category->instance_methods, "-[");
            register_framework_category_methods(framework, category->class_methods, "+[");
            continue;
        }
        char* metaclass;
        memcpy(&metaclass, category->cls, sizeof metaclass);
        if (category->instance_methods != nullptr) category_method_lists()[category->cls].push_back(category->instance_methods);
        if (category->class_methods != nullptr) category_method_lists()[metaclass].push_back(category->class_methods);
    }
}

const char* class_method_list(const char* cls) {
    const char* metaclass;
    memcpy(&metaclass, cls, sizeof metaclass);
    uintptr_t data;
    memcpy(&data, metaclass + kClassDataOffset, sizeof data);
    const char* methods;
    memcpy(&methods, reinterpret_cast<const char*>(data & kClassDataMask) + kRoBaseMethodsOffset, sizeof methods);
    return methods;
}

}  // namespace

void objc_run_load_methods(const MachImage& image) {
    using LoadMethod = void (*)(void*, const char*);
    if (const MachSection* classes = objc_section(image, "__objc_nlclslist")) {
        for (size_t offset = 0; offset + sizeof(void*) <= classes->size; offset += sizeof(void*)) {
            char* cls;
            memcpy(&cls, reinterpret_cast<const char*>(classes->address + offset), sizeof cls);
            if (const void* load = find_in_method_list(class_method_list(cls), "load")) reinterpret_cast<LoadMethod>(const_cast<void*>(load))(cls, "load");
        }
    }
    if (const MachSection* categories = objc_section(image, "__objc_nlcatlist")) {
        for (size_t offset = 0; offset + sizeof(void*) <= categories->size; offset += sizeof(void*)) {
            const CategoryRecord* category;
            memcpy(&category, reinterpret_cast<const char*>(categories->address + offset), sizeof category);
            if (category->cls == nullptr) continue;
            if (const void* load = find_in_method_list(category->class_methods, "load")) reinterpret_cast<LoadMethod>(const_cast<void*>(load))(category->cls, "load");
        }
    }
}

void objc_attach_image(const MachImage& image) {
    attach_categories(image);
    const MachSection* list = image.section("__DATA_CONST", "__objc_classlist");
    if (list == nullptr) list = image.section("__DATA", "__objc_classlist");
    if (list == nullptr) return;
    for (size_t offset = 0; offset + sizeof(void*) <= list->size; offset += sizeof(void*)) {
        char* cls;
        memcpy(&cls, reinterpret_cast<const char*>(list->address + offset), sizeof cls);
        if (has_objc_root(cls)) slide_instance_variables(cls);
        const std::string raw = raw_class_name(cls);
        guest_classes()[raw] = cls;
        guest_classes()[readable_class_name(raw)] = cls;
    }
}

namespace {

// Keys are compared by address, as in the real runtime. Values are retained unless the policy says assign.
std::map<std::pair<void*, const void*>, void*>& associated_objects() {
    static std::map<std::pair<void*, const void*>, void*> objects;
    return objects;
}

void set_associated_object(void* object, const void* key, void* value, uintptr_t policy) {
    constexpr uintptr_t kAssociationAssign = 0;
    using Retain = void* (*)(void*);
    if (value != nullptr && policy != kAssociationAssign) reinterpret_cast<Retain>(resolve_symbol("swift_retain"))(value);
    associated_objects()[{object, key}] = value;
}

void* get_associated_object(void* object, const void* key) {
    const auto found = associated_objects().find({object, key});
    return found == associated_objects().end() ? nullptr : found->second;
}

// A Method handle stands for one class-and-selector slot, whose implementation can be exchanged.
struct MethodHandle {
    const void* implementation;
};

MethodHandle* class_instance_method(void* cls, const char* selector) {
    std::string framework_root;
    const void* found = find_guest_method(static_cast<const char*>(cls), selector, framework_root);
    if (found == nullptr && !framework_root.empty()) {
        const auto registered = framework_methods().find("-[" + framework_root + " " + selector + "]");
        if (registered != framework_methods().end()) found = registered->second;
    }
    if (found == nullptr) return nullptr;
    static std::map<const void*, MethodHandle*> handles;
    MethodHandle*& handle = handles[found];
    if (handle == nullptr) handle = new MethodHandle{found};
    return handle;
}

}  // namespace

void* objc_runtime_symbol(std::string_view name) {
    if (name == "objc_setAssociatedObject") return reinterpret_cast<void*>(set_associated_object);
    if (name == "objc_getAssociatedObject") return reinterpret_cast<void*>(get_associated_object);
    if (name == "objc_getClass" || name == "objc_lookUpClass") return reinterpret_cast<void*>(+[](const char* class_name) -> void* { return objc_class_named(class_name); });
    if (name == "objc_setHook_getClass") return reinterpret_cast<void*>(+[](void*, void** previous) {
        if (previous != nullptr) *previous = nullptr;
    });
    if (name == "objc_getProtocol") return reinterpret_cast<void*>(+[](const char*) -> void* { return nullptr; });
    if (name == "protocol_getMethodDescription") return reinterpret_cast<void*>(+[]() -> void* { return nullptr; });
    if (name == "class_getInstanceMethod") return reinterpret_cast<void*>(class_instance_method);
    if (name == "objc_enumerationMutation") return reinterpret_cast<void*>(+[](void*) {
        engine_log("STOP: a collection was mutated while being enumerated");
        abort();
    });
    if (name == "objc_exception_throw") return reinterpret_cast<void*>(+[](void* exception) {
        engine_log("STOP: an Objective-C exception was thrown: %p", exception);
        abort();
    });
    if (name == "_objc_empty_cache") {
        static void* empty_cache[2];
        return empty_cache;
    }
    if (name == "_objc_autoreleasePoolPush") return reinterpret_cast<void*>(+[]() -> void* { return nullptr; });
    if (name == "_objc_autoreleasePoolPop") return reinterpret_cast<void*>(+[](void*) {});
    if (name == "objc_msgSend") return reinterpret_cast<void*>(rt_objc_msgSend);
    if (name == "objc_msgSendSuper2") return reinterpret_cast<void*>(rt_objc_msgSendSuper2);
    if (name == "objc_allocWithZone") return reinterpret_cast<void*>(rt_objc_allocWithZone);
    if (name == "objc_alloc") return reinterpret_cast<void*>(rt_objc_allocWithZone);
    if (name == "objc_alloc_init") return reinterpret_cast<void*>(objc_alloc_init);
    return nullptr;
}

void objc_register_method(const char* description, const void* implementation) {
    framework_methods()[description] = implementation;
}

void objc_register_fallback(const char* class_name, const void* (*resolve)(const char* selector)) { framework_fallbacks()[class_name] = resolve; }

std::string objc_framework_class_name(const void* cls) { return framework_class_name(cls); }

void* objc_framework_object(const char* class_name) { return framework_alloc(class_name); }

void* objc_native_class(const char* name, void (*destroyer)(void*)) {
    // Release finds the destroyer two words before the class, as it does in Swift metadata.
    auto** block = static_cast<void**>(calloc(3, sizeof(void*)));
    block[0] = reinterpret_cast<void*>(destroyer);
    void* cls = block + 2;
    native_classes()[cls] = name;
    return cls;
}

bool objc_is_framework_class(const void* object, const void* cls) {
    const std::string name = framework_class_name(cls);
    return !name.empty() && name == framework_class_name(*static_cast<void* const*>(object));
}

void* objc_alloc_init(void* cls) {
    void* object = rt_objc_allocWithZone(cls, nullptr);
    using Init = void* (*)(void*, const char*);
    return reinterpret_cast<Init>(const_cast<void*>(rt_objc_lookup(object, "init")))(object, "init");
}

void objc_send(void* receiver, const char* selector, void* argument) {
    using Method = void (*)(void*, const char*, void*);
    reinterpret_cast<Method>(const_cast<void*>(rt_objc_lookup(receiver, selector)))(receiver, selector, argument);
}

bool objc_responds(void* receiver, const char* selector) {
    const std::string direct = framework_class_name(receiver);
    if (!direct.empty()) return framework_methods().count("+[" + direct + " " + selector + "]") != 0;
    const char* cls;
    memcpy(&cls, receiver, sizeof cls);
    std::string framework_root;
    if (find_guest_method(cls, selector, framework_root) != nullptr) return true;
    return !framework_root.empty() && framework_methods().count("-[" + framework_root + " " + selector + "]") != 0;
}

const void* objc_method_for(void* receiver, const char* selector) { return rt_objc_lookup(receiver, selector); }

std::string objc_class_name(const void* cls) {
    const std::string framework = framework_class_name(cls);
    return framework.empty() ? readable_class_name(raw_class_name(cls)) : framework;
}

void* objc_class_named(const std::string& name) {
    if (const auto found = guest_classes().find(name); found != guest_classes().end()) return found->second;
    const std::string symbol = "_OBJC_CLASS_$_" + name;
    if (const void* guard = unresolved_symbol_named(symbol.c_str())) return const_cast<void*>(guard);
    for (const auto& [cls, known] : native_classes()) {
        if (known == name) return const_cast<void*>(cls);
    }
    return nullptr;
}

bool objc_is_class_address(const void* address) {
    if (!framework_class_name(address).empty()) return true;
    for (const auto& [name, cls] : guest_classes()) {
        if (cls == address) return true;
    }
    return false;
}

void objc_set_uikit_fallback(const void* (*resolve)(const char*)) { g_uikit_fallback = resolve; }

void objc_set_uikit_active() { g_uikit_active = true; }

void* objc_allocate(void* cls) { return rt_objc_allocWithZone(cls, nullptr); }

std::string objc_framework_root_of(const void* object) {
    const char* cls;
    memcpy(&cls, object, sizeof cls);
    while (cls != nullptr) {
        const std::string framework = framework_class_name(cls);
        if (!framework.empty()) return framework;
        memcpy(&cls, cls + kClassSuperclassOffset, sizeof cls);
    }
    return {};
}

bool objc_set_ivar_named(void* object, const std::string& name, void* value) {
    const char* cls;
    memcpy(&cls, object, sizeof cls);
    const std::string underscored = "_" + name;
    while (cls != nullptr && framework_class_name(cls).empty()) {
        uintptr_t data;
        memcpy(&data, cls + kClassDataOffset, sizeof data);
        const char* ivars;
        memcpy(&ivars, reinterpret_cast<const char*>(data & kClassDataMask) + kRoInstanceVariablesOffset, sizeof ivars);
        if (ivars != nullptr) {
            uint32_t entry_size, count;
            memcpy(&entry_size, ivars, sizeof entry_size);
            memcpy(&count, ivars + sizeof entry_size, sizeof count);
            for (uint32_t i = 0; i < count; ++i) {
                const char* entry = ivars + kIvarListHeaderSize + i * entry_size;
                const uint32_t* offset;
                const char* ivar_name;
                memcpy(&offset, entry, sizeof offset);
                memcpy(&ivar_name, entry + sizeof offset, sizeof ivar_name);
                if (offset == nullptr || ivar_name == nullptr || (name != ivar_name && underscored != ivar_name)) continue;
                using Retain = void* (*)(void*);
                if (value != nullptr) reinterpret_cast<Retain>(resolve_symbol("swift_retain"))(value);
                memcpy(static_cast<char*>(object) + *offset, &value, sizeof value);
                return true;
            }
        }
        memcpy(&cls, cls + kClassSuperclassOffset, sizeof cls);
    }
    return false;
}
