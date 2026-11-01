#include <sys/uio.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <unordered_map>

#include "engine.h"
#include "foundation_internal.h"
#include "swift_abi.h"
#include "symbols.h"
#include "uikit_internal.h"
#include "uikit_shim.h"

namespace uikit {

namespace {

// Arguments and results use x0 for words and d0-d3 for floats and rectangles, so one record serves every type.
struct PropertyValue {
    uint64_t word = 0;
    double floats[4] = {0, 0, 0, 0};
};

std::mutex& property_lock() {
    static std::mutex lock;
    return lock;
}

std::unordered_map<void*, std::map<std::string, PropertyValue>>& property_store() {
    static std::unordered_map<void*, std::map<std::string, PropertyValue>> store;
    return store;
}

bool read_word(uint64_t address, uint64_t& out) {
    iovec local{&out, sizeof out};
    iovec remote{reinterpret_cast<void*>(address), sizeof out};
    return process_vm_readv(getpid(), &local, 1, &remote, 1, 0) == sizeof out;
}

// A word may be an integer as easily as an object: it counts as one only if it starts with a known class.
bool looks_like_object(uint64_t word) {
    constexpr uint64_t kLowestHeapAddress = 0x100000000ULL;
    constexpr uint64_t kPointerAlignmentMask = 7;
    if (word < kLowestHeapAddress || (word >> 48) != 0 || (word & kPointerAlignmentMask) != 0) return false;
    uint64_t cls = 0;
    return read_word(word, cls) && objc_is_class_address(reinterpret_cast<void*>(cls));
}

void retain_word(uint64_t word) {
    using Function = void* (*)(void*);
    static const auto retain = reinterpret_cast<Function>(resolve_symbol("swift_retain"));
    if (looks_like_object(word)) retain(reinterpret_cast<void*>(word));
}

std::string key_from_setter(const char* selector) {
    constexpr size_t kSetPrefixLength = 3;
    std::string key = selector + kSetPrefixLength;
    if (!key.empty() && key.back() == ':') key.pop_back();
    if (!key.empty()) key[0] = static_cast<char>(tolower(static_cast<unsigned char>(key[0])));
    return key;
}

void generic_setter(void* self, const char* selector, uint64_t word, double d0, double d1, double d2, double d3) {
    const std::lock_guard guard(property_lock());
    retain_word(word);
    property_store()[self][key_from_setter(selector)] = {word, {d0, d1, d2, d3}};
}

}  // namespace

}  // namespace uikit

extern "C" const void* uikit_property_record(void* self, const char* selector) {
    using namespace uikit;
    static const PropertyValue kEmpty;
    const std::lock_guard guard(property_lock());
    const auto object = property_store().find(self);
    if (object == property_store().end()) return &kEmpty;
    auto value = object->second.find(selector);
    if (value == object->second.end() && strncmp(selector, "is", 2) == 0 && isupper(static_cast<unsigned char>(selector[2]))) {
        std::string plain = selector + 2;
        plain[0] = static_cast<char>(tolower(static_cast<unsigned char>(plain[0])));
        value = object->second.find(plain);
    }
    return value == object->second.end() ? &kEmpty : &value->second;
}

extern "C" void uikit_generic_getter();

__asm__(R"(
.text
.globl uikit_generic_getter
uikit_generic_getter:
    stp x29, x30, [sp, #-16]!
    mov x29, sp
    bl uikit_property_record
    ldp d0, d1, [x0, #8]
    ldp d2, d3, [x0, #24]
    ldr x0, [x0]
    ldp x29, x30, [sp], #16
    ret
)");

namespace uikit {

namespace {

void* return_self(void* self, const char*) { return self; }

// Answers unknown UIKit messages by the shape of their selector; multi-argument calls log and return nil.
const void* resolve_generic_method(const char* selector) {
    const std::string name = selector;
    if (const void* shared = uikit_method_for_selector(name)) return shared;
    const size_t colons = static_cast<size_t>(std::count(name.begin(), name.end(), ':'));
    if (name == "alloc" || name == "new") return resolve_symbol(name == "alloc" ? "objc_alloc" : "objc_alloc_init");
    if (name.rfind("init", 0) == 0) return reinterpret_cast<const void*>(return_self);
    if (name.rfind("set", 0) == 0 && name.size() > 3 && isupper(static_cast<unsigned char>(name[3])) && colons == 1) return reinterpret_cast<const void*>(generic_setter);
    if (colons == 0) return reinterpret_cast<const void*>(uikit_generic_getter);
    return nullptr;
}

}  // namespace

void property_set_object(void* object, const std::string& key, void* value) {
    const std::lock_guard guard(property_lock());
    retain_word(reinterpret_cast<uint64_t>(value));
    property_store()[object][key] = {reinterpret_cast<uint64_t>(value), {0, 0, 0, 0}};
}

void* property_get_object(void* object, const std::string& key) {
    const std::lock_guard guard(property_lock());
    const auto found = property_store().find(object);
    if (found == property_store().end()) return nullptr;
    const auto value = found->second.find(key);
    return value == found->second.end() ? nullptr : reinterpret_cast<void*>(value->second.word);
}

void* object_retain(void* object) {
    using Function = void* (*)(void*);
    static const auto retain = reinterpret_cast<Function>(resolve_symbol("swift_retain"));
    return retain(object);
}

void property_forget(void* object) {
    const std::lock_guard guard(property_lock());
    property_store().erase(object);
}

void property_set_rect(void* object, const std::string& key, const double* values) {
    const std::lock_guard guard(property_lock());
    property_store()[object][key] = {0, {values[0], values[1], values[2], values[3]}};
}

void property_get_rect(void* object, const std::string& key, double* out) {
    const std::lock_guard guard(property_lock());
    out[0] = out[1] = out[2] = out[3] = 0;
    const auto found = property_store().find(object);
    if (found == property_store().end()) return;
    const auto value = found->second.find(key);
    if (value == found->second.end()) return;
    memcpy(out, value->second.floats, sizeof value->second.floats);
}

int64_t property_get_word(void* object, const std::string& key) {
    const std::lock_guard guard(property_lock());
    const auto found = property_store().find(object);
    if (found == property_store().end()) return 0;
    const auto value = found->second.find(key);
    return value == found->second.end() ? 0 : static_cast<int64_t>(value->second.word);
}

// Framework views keep their parts in this store, under the names their nib keys give them.
void finish_decoded_object(void* object, const std::string&) {
    auto* subviews = static_cast<foundation::NSArrayObject*>(property_get_object(object, "subviews"));
    if (subviews == nullptr) return;
    for (void* child : *subviews->items) property_set_object(child, "superview", object);
}

void object_set_value(void* object, const std::string& key, void* value) {
    std::string setter = "set" + key + ":";
    setter[3] = static_cast<char>(toupper(static_cast<unsigned char>(setter[3])));
    if (objc_responds(object, setter.c_str())) {
        using Setter = void (*)(void*, const char*, void*);
        reinterpret_cast<Setter>(const_cast<void*>(objc_method_for(object, setter.c_str())))(object, setter.c_str(), value);
        return;
    }
    if (objc_set_ivar_named(object, key, value)) return;
    property_set_object(object, key, value);
}

void control_add_target(void* control, void* target, const std::string& action, uint64_t events) {
    property_set_object(control, "actionTarget", target);
    property_set_object(control, "actionSelector", foundation::new_nsstring(action));
    property_set_word(control, "actionEvents", static_cast<int64_t>(events));
}

std::vector<void*> object_array_items(void* array) {
    if (array == nullptr) return {};
    void* bridged = nullptr;
    if (foundation::bridged_collection_value(array, bridged)) {
        constexpr size_t kArrayCountOffset = 16, kArrayElementsStart = 32;
        int64_t count;
        memcpy(&count, static_cast<char*>(bridged) + kArrayCountOffset, sizeof count);
        auto* elements = reinterpret_cast<void**>(static_cast<char*>(bridged) + kArrayElementsStart);
        return std::vector<void*>(elements, elements + count);
    }
    return *static_cast<foundation::NSArrayObject*>(array)->items;
}

bool object_is_kind(void* object, const char* framework_class) {
    static const std::map<std::string, std::string> kParents = {
        {"UITabBarController", "UIViewController"},
        {"UINavigationController", "UIViewController"},
        {"UITableViewController", "UIViewController"},
        {"UICollectionViewController", "UIViewController"},
        {"UIWindow", "UIView"},
        {"UILabel", "UIView"},
        {"UIImageView", "UIView"},
        {"UIStackView", "UIView"},
        {"UIScrollView", "UIView"},
        {"UITableView", "UIScrollView"},
        {"UICollectionView", "UIScrollView"},
        {"UITextView", "UIScrollView"},
        {"UIControl", "UIView"},
        {"UIButton", "UIControl"},
        {"UISwitch", "UIControl"},
        {"UITextField", "UIControl"},
        {"UITableViewCell", "UIView"},
        {"UICollectionReusableView", "UIView"},
        {"UICollectionViewCell", "UICollectionReusableView"},
    };
    std::string name = objc_framework_root_of(object);
    while (!name.empty()) {
        if (name == framework_class) return true;
        const auto parent = kParents.find(name);
        if (parent == kParents.end()) return false;
        name = parent->second;
    }
    return false;
}

void property_set_word(void* object, const std::string& key, int64_t value) {
    const std::lock_guard guard(property_lock());
    property_store()[object][key] = {static_cast<uint64_t>(value), {0, 0, 0, 0}};
}

void property_set_floats(void* object, const std::string& key, double first, double second) {
    const std::lock_guard guard(property_lock());
    property_store()[object][key] = {0, {first, second, 0, 0}};
}

void property_get_floats(void* object, const std::string& key, double* out) {
    const std::lock_guard guard(property_lock());
    out[0] = out[1] = 0;
    const auto found = property_store().find(object);
    if (found == property_store().end()) return;
    const auto value = found->second.find(key);
    if (value == found->second.end()) return;
    out[0] = value->second.floats[0];
    out[1] = value->second.floats[1];
}

double property_get_float(void* object, const std::string& key) {
    double values[2];
    property_get_floats(object, key, values);
    return values[0];
}

void register_property_fallback() { objc_set_uikit_fallback(resolve_generic_method); }

}  // namespace uikit

void register_device_classes();

void uikit_register_classes() {
    uikit::register_app_classes();
    uikit::register_property_fallback();
    uikit::register_nib_classes();
    uikit::register_view_classes();
    register_device_classes();
}

void* device_symbol(std::string_view name);

void* uikit_symbol(std::string_view name) {
    if (void* symbol = device_symbol(name)) return symbol;
    static const uikit::SymbolMap table = [] {
        uikit::SymbolMap all;
        uikit::add_app_symbols(all);
        uikit::add_constant_symbols(all);
        return all;
    }();
    const auto found = table.find(name);
    return found == table.end() ? nullptr : found->second;
}
