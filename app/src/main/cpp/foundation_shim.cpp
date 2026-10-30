#include "foundation_shim.h"

#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <mutex>
#include <sstream>
#include <unordered_map>

#include "engine.h"
#include "objc_runtime.h"

#define SWIFTCC __attribute__((swiftcall))

namespace {

constexpr uint64_t kSmallStringBit = 1ULL << 61;
constexpr uint64_t kSmallAsciiBit = 1ULL << 62;
constexpr uint64_t kImmortalBit = 1ULL << 63;
constexpr uint64_t kObjectPointerMask = 0x0fffffffffffffffULL;
constexpr uint64_t kCountMask = 0x0000ffffffffffffULL;
constexpr uint64_t kAsciiFlag = 1ULL << 63;
constexpr uint64_t kNfcFlag = 1ULL << 62;
constexpr size_t kSmallStringCapacity = 15;
constexpr size_t kSmallCountShift = 56;
constexpr uint64_t kSmallCountMask = 0x0f;
constexpr size_t kNativeBias = 32;
constexpr const char* kDefaultsFileName = ".defaults";

struct NativeObject {
    void* isa;
    uint64_t references;
};

struct NSStringObject {
    NativeObject header;
    std::string* text;
};

void* g_nsstring_class = nullptr;
void* g_defaults_class = nullptr;

void* new_nsstring(const std::string& text) {
    auto* object = static_cast<NSStringObject*>(calloc(1, sizeof(NSStringObject)));
    object->header.isa = g_nsstring_class;
    object->header.references = 1;
    object->text = new std::string(text);
    return object;
}

const std::string& nsstring_text(void* object) { return *static_cast<NSStringObject*>(object)->text; }

struct UserDefaults {
    std::map<std::string, std::string> values;

    std::string path() const { return engine_bundle_dir() + "/" + kDefaultsFileName; }

    void load() {
        std::ifstream input(path());
        std::string key, value;
        while (std::getline(input, key) && std::getline(input, value)) values[unescape(key)] = unescape(value);
    }

    void save() const {
        std::ofstream output(path(), std::ios::trunc);
        for (const auto& [key, value] : values) output << escape(key) << '\n' << escape(value) << '\n';
    }

    static std::string escape(const std::string& text) {
        std::string out;
        for (const char c : text) {
            if (c == '\n') out += "\\n";
            else if (c == '\\') out += "\\\\";
            else out += c;
        }
        return out;
    }

    static std::string unescape(const std::string& text) {
        std::string out;
        for (size_t i = 0; i < text.size(); ++i) {
            if (text[i] == '\\' && i + 1 < text.size()) out += text[++i] == 'n' ? '\n' : text[i];
            else out += text[i];
        }
        return out;
    }
};

UserDefaults& defaults() {
    static UserDefaults* instance = [] {
        auto* created = new UserDefaults();
        created->load();
        return created;
    }();
    return *instance;
}

void* defaults_standard(void*, const char*) {
    static NativeObject* singleton = [] {
        auto* object = static_cast<NativeObject*>(calloc(1, sizeof(NativeObject)));
        object->isa = g_defaults_class;
        object->references = 1ULL << 63;
        return object;
    }();
    return singleton;
}

void* defaults_string_for_key(void*, const char*, void* key) {
    const auto found = defaults().values.find(nsstring_text(key));
    return found == defaults().values.end() ? nullptr : new_nsstring(found->second);
}

bool defaults_bool_for_key(void*, const char*, void* key) {
    const auto found = defaults().values.find(nsstring_text(key));
    return found != defaults().values.end() && (found->second == "1" || found->second == "true");
}

int64_t defaults_integer_for_key(void*, const char*, void* key) {
    const auto found = defaults().values.find(nsstring_text(key));
    return found == defaults().values.end() ? 0 : strtoll(found->second.c_str(), nullptr, 10);
}

void defaults_set_bool(void*, const char*, bool value, void* key) {
    defaults().values[nsstring_text(key)] = value ? "1" : "0";
    defaults().save();
}

void defaults_set_integer(void*, const char*, int64_t value, void* key) {
    defaults().values[nsstring_text(key)] = std::to_string(value);
    defaults().save();
}

void defaults_set_object(void*, const char*, void* value, void* key) {
    if (value == nullptr) defaults().values.erase(nsstring_text(key));
    else defaults().values[nsstring_text(key)] = nsstring_text(value);
    defaults().save();
}

void defaults_remove(void*, const char*, void* key) {
    defaults().values.erase(nsstring_text(key));
    defaults().save();
}

SWIFTCC void* string_bridge_to_objc(SwiftString value) { return new_nsstring(swift_string_to_utf8(value)); }

SWIFTCC SwiftString string_bridge_from_objc(void* object) {
    if (object == nullptr) return swift_string_from_utf8("");
    return swift_string_from_utf8(nsstring_text(object));
}

}  // namespace

std::string swift_string_to_utf8(SwiftString value) {
    if (value.object & kSmallStringBit) {
        const size_t count = (value.object >> kSmallCountShift) & kSmallCountMask;
        char bytes[16];
        memcpy(bytes, &value.count_and_flags, 8);
        memcpy(bytes + 8, &value.object, 8);
        return std::string(bytes, count);
    }
    const uint64_t pointer = value.object & kObjectPointerMask;
    if (pointer == 0) return {};
    return std::string(reinterpret_cast<const char*>(pointer + kNativeBias), value.count_and_flags & kCountMask);
}

SwiftString swift_string_from_utf8(const std::string& text) {
    bool ascii = true;
    for (const char c : text) {
        if (static_cast<unsigned char>(c) >= 0x80) ascii = false;
    }
    SwiftString result{0, 0};
    if (text.size() <= kSmallStringCapacity) {
        char bytes[16] = {};
        memcpy(bytes, text.data(), text.size());
        memcpy(&result.count_and_flags, bytes, 8);
        memcpy(&result.object, bytes + 8, 8);
        result.object |= kImmortalBit | kSmallStringBit | (ascii ? kSmallAsciiBit : 0) | (static_cast<uint64_t>(text.size()) << kSmallCountShift);
        return result;
    }
    // Immortal storage: the guest never frees it, as with Swift's own string literals.
    auto* storage = static_cast<char*>(malloc(text.size() + 1));
    memcpy(storage, text.c_str(), text.size() + 1);
    result.count_and_flags = text.size() | kNfcFlag | (ascii ? kAsciiFlag : 0);
    result.object = (reinterpret_cast<uint64_t>(storage) - kNativeBias) | kImmortalBit;
    return result;
}

void foundation_register_classes() {
    g_nsstring_class = objc_native_class("NSString");
    g_defaults_class = objc_native_class("NSUserDefaults");
    objc_register_method("+[NSUserDefaults standardUserDefaults]", reinterpret_cast<const void*>(defaults_standard));
    objc_register_method("-[NSUserDefaults stringForKey:]", reinterpret_cast<const void*>(defaults_string_for_key));
    objc_register_method("-[NSUserDefaults boolForKey:]", reinterpret_cast<const void*>(defaults_bool_for_key));
    objc_register_method("-[NSUserDefaults integerForKey:]", reinterpret_cast<const void*>(defaults_integer_for_key));
    objc_register_method("-[NSUserDefaults setBool:forKey:]", reinterpret_cast<const void*>(defaults_set_bool));
    objc_register_method("-[NSUserDefaults setInteger:forKey:]", reinterpret_cast<const void*>(defaults_set_integer));
    objc_register_method("-[NSUserDefaults setObject:forKey:]", reinterpret_cast<const void*>(defaults_set_object));
    objc_register_method("-[NSUserDefaults removeObjectForKey:]", reinterpret_cast<const void*>(defaults_remove));
}

void* foundation_symbol(std::string_view name) {
    static const std::unordered_map<std::string_view, void*> table = {
        {"$sSS10FoundationE19_bridgeToObjectiveCSo8NSStringCyF", reinterpret_cast<void*>(string_bridge_to_objc)},
        {"$sSS10FoundationE36_unconditionallyBridgeFromObjectiveCySSSo8NSStringCSgFZ", reinterpret_cast<void*>(string_bridge_from_objc)},
    };
    const auto found = table.find(name);
    return found == table.end() ? nullptr : found->second;
}
