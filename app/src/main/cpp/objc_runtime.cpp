#include "objc_runtime.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <unordered_map>

#include "engine.h"
#include "macho.h"

namespace {

constexpr uint32_t kRelativeMethodList = 0x80000000;
constexpr uint32_t kMethodListEntrySizeMask = 0xffff;
constexpr uintptr_t kClassDataMask = ~static_cast<uintptr_t>(7);
constexpr size_t kClassSuperclassOffset = 8;
constexpr size_t kClassDataOffset = 32;
constexpr size_t kRoInstanceSizeOffset = 8;
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

const void* find_guest_method(const char* cls, const char* selector, std::string& framework_root) {
    while (cls != nullptr) {
        const std::string framework = framework_class_name(cls);
        if (!framework.empty()) {
            framework_root = framework;
            return nullptr;
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
    // UIKit only describes presentation, which the host draws itself: nil is a valid answer to all of it.
    if (description.compare(2, 2, "UI") == 0) {
        static std::set<std::string> logged;
        if (logged.insert(description).second) engine_log("UIKIT: ignoring %s", description.c_str());
        return reinterpret_cast<const void*>(+[](void*) -> void* { return nullptr; });
    }
    stop_unknown_message(description);
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
    if (!framework_root.empty()) return lookup_framework_method("-[" + framework_root + " " + selector + "]");
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
    if (!framework_root.empty()) return lookup_framework_method("-[" + framework_root + " " + selector + "]");
    stop_unknown_message(std::string("unknown superclass -[? ") + selector + "]");
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

extern "C" void* rt_objc_allocWithZone(void* cls, void*) {
    const std::string framework = framework_class_name(cls);
    if (!framework.empty()) return framework_alloc(framework);
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
#define OBJC_DISPATCH(name, lookup, nil_check, finish)  \
    ".globl " #name "\n"                        \
    #name ":\n"                                 \
    nil_check                                   \
    "  stp x29, x30, [sp, #-208]!\n"            \
    "  mov x29, sp\n"                           \
    "  stp x0, x1, [sp, #16]\n"                 \
    "  stp x2, x3, [sp, #32]\n"                 \
    "  stp x4, x5, [sp, #48]\n"                 \
    "  stp x6, x7, [sp, #64]\n"                 \
    "  str x8, [sp, #80]\n"                     \
    "  stp d0, d1, [sp, #96]\n"                 \
    "  stp d2, d3, [sp, #112]\n"                \
    "  stp d4, d5, [sp, #128]\n"                \
    "  stp d6, d7, [sp, #144]\n"                \
    "  bl " #lookup "\n"                        \
    "  mov x16, x0\n"                           \
    "  ldp x0, x1, [sp, #16]\n"                 \
    "  ldp x2, x3, [sp, #32]\n"                 \
    "  ldp x4, x5, [sp, #48]\n"                 \
    "  ldp x6, x7, [sp, #64]\n"                 \
    "  ldr x8, [sp, #80]\n"                     \
    "  ldp d0, d1, [sp, #96]\n"                 \
    "  ldp d2, d3, [sp, #112]\n"                \
    "  ldp d4, d5, [sp, #128]\n"                \
    "  ldp d6, d7, [sp, #144]\n"                \
    "  ldp x29, x30, [sp], #208\n"              \
    finish                                      \
    "  br x16\n"

__asm__(
    ".text\n"
    OBJC_DISPATCH(rt_objc_msgSend, rt_objc_lookup,
                  "  cbnz x0, 1f\n"
                  "  mov x1, xzr\n"
                  "  movi d0, #0\n"
                  "  movi d1, #0\n"
                  "  ret\n"
                  "1:\n",
                  "")
    // x0 points at {receiver, class}; the method itself must see the receiver.
    OBJC_DISPATCH(rt_objc_msgSendSuper2, rt_objc_super_lookup, "", "  ldr x0, [x0]\n"));

void* objc_runtime_symbol(std::string_view name) {
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
