#include "foundation_shim.h"

#include <unistd.h>

#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <mutex>
#include <sstream>
#include <unordered_map>
#include <vector>

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

constexpr size_t kStorageCapacityOffset = 16;
constexpr size_t kStorageCountOffset = 24;

struct StorageMetadata {
    void (*destroyer)(void*);
    const void* witnesses;
    uint64_t kind;
};

const StorageMetadata kStringStorageMetadata = {free, nullptr, 0};

void* g_nsstring_class = nullptr;
void* g_defaults_class = nullptr;

void destroy_nsstring(void* raw) {
    auto* object = static_cast<NSStringObject*>(raw);
    delete object->text;
    free(object);
}

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

void* defaults_object_for_key(void* self, const char* selector, void* key) { return defaults_string_for_key(self, selector, key); }

// Arrays are not persisted yet, so a missing value is the only answer this can give truthfully.
void* defaults_string_array_for_key(void*, const char*, void*) { return nullptr; }

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

void* return_self(void* self, const char*) { return self; }
void* return_self_with_argument(void* self, const char*, void*) { return self; }
void ignore_message(void*, const char*) {}
void ignore_message_with_word(void*, const char*, uint64_t) {}
void ignore_message_with_double(void*, const char*, double) {}
int32_t location_authorization_status(void*, const char*) { return 0; }

constexpr int64_t kReportedOsMajorVersion = 27;

struct OperatingSystemVersion {
    int64_t major;
    int64_t minor;
    int64_t patch;
};

// Returned indirectly: AAPCS64 passes the result pointer in x8, which a C++ struct return also uses here.
OperatingSystemVersion process_info_version(void*, const char*) { return {kReportedOsMajorVersion, 0, 0}; }

void* framework_singleton(const char* class_name) {
    static std::map<std::string, NativeObject*> singletons;
    auto*& instance = singletons[class_name];
    if (instance == nullptr) {
        instance = static_cast<NativeObject*>(calloc(1, sizeof(NativeObject)));
        instance->isa = objc_native_class(class_name, free);
        instance->references = 1ULL << 63;
    }
    return instance;
}

void* process_info_shared(void*, const char*) { return framework_singleton("NSProcessInfo"); }
void* main_bundle(void*, const char*) { return framework_singleton("NSBundle"); }
void* bundle_for_class(void*, const char*, void*) { return framework_singleton("NSBundle"); }
void* bundle_info_value(void*, const char*, void*) { return nullptr; }
void* file_manager_default(void*, const char*) { return framework_singleton("NSFileManager"); }
void* shared_application(void*, const char*) { return framework_singleton("UIApplication"); }

bool file_exists_at_path(void*, const char*, void* path) { return access(nsstring_text(path).c_str(), F_OK) == 0; }

// iOS's coarsest accuracy constant, in metres; the value only needs to be the one the app passes back.
const double kLocationAccuracyKilometer = 1000.0;

std::vector<std::string>& user_scripts() {
    static std::vector<std::string> scripts;
    return scripts;
}

void* user_script_init(void* self, const char*, void* source, int64_t, bool) {
    user_scripts().push_back(nsstring_text(source));
    engine_log("WEBKIT: registered user script, %zu bytes", nsstring_text(source).size());
    return self;
}

void* web_view_layer(void*, const char*) { return framework_singleton("CALayer"); }

void* web_view_init(void* self, const char*, double, double, double, double, void*) { return self; }

void* web_kit_child_singleton(void*, const char* selector) { return framework_singleton(selector); }
void* web_data_store_default(void*, const char*) { return framework_singleton("WKWebsiteDataStore"); }
void* content_world_page(void*, const char*) { return framework_singleton("WKContentWorld"); }
void* url_session_configuration_default(void*, const char*) { return framework_singleton("NSURLSessionConfiguration"); }
void* url_session_with_configuration(void*, const char*, void*) { return framework_singleton("NSURLSession"); }

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
    constexpr uint64_t kNativeFlag = 1ULL << 61;
    constexpr uint64_t kTailAllocatedFlag = 1ULL << 60;
    constexpr size_t kMinimumCapacity = 32;
    const size_t capacity = text.size() < kMinimumCapacity ? kMinimumCapacity : text.size();
    auto* storage = static_cast<char*>(calloc(1, kNativeBias + capacity + 1));
    auto* header = reinterpret_cast<NativeObject*>(storage);
    header->isa = const_cast<uint64_t*>(&kStringStorageMetadata.kind);
    header->references = 1;
    const uint64_t flags = kNfcFlag | kNativeFlag | kTailAllocatedFlag | (ascii ? kAsciiFlag : 0);
    const uint64_t count_and_flags = text.size() | flags;
    memcpy(storage + kStorageCapacityOffset, &capacity, sizeof capacity);
    memcpy(storage + kStorageCountOffset, &count_and_flags, sizeof count_and_flags);
    memcpy(storage + kNativeBias, text.data(), text.size());
    result.count_and_flags = count_and_flags;
    result.object = reinterpret_cast<uint64_t>(storage);
    return result;
}

void foundation_register_classes() {
    g_nsstring_class = objc_native_class("NSString", destroy_nsstring);
    g_defaults_class = objc_native_class("NSUserDefaults", free);
    objc_register_method("-[WKUserScript initWithSource:injectionTime:forMainFrameOnly:]", reinterpret_cast<const void*>(user_script_init));
    objc_register_method("-[WKWebView initWithFrame:configuration:]", reinterpret_cast<const void*>(web_view_init));
    objc_register_method("-[WKWebView layer]", reinterpret_cast<const void*>(web_view_layer));
    objc_register_method("-[CALayer setCornerRadius:]", reinterpret_cast<const void*>(ignore_message_with_double));
    objc_register_method("-[CALayer setMasksToBounds:]", reinterpret_cast<const void*>(ignore_message_with_word));
    objc_register_method("-[WKWebView setNavigationDelegate:]", reinterpret_cast<const void*>(ignore_message_with_word));
    objc_register_method("-[WKWebView setInspectable:]", reinterpret_cast<const void*>(ignore_message_with_word));
    objc_register_method("-[WKWebViewConfiguration userContentController]", reinterpret_cast<const void*>(web_kit_child_singleton));
    objc_register_method("-[WKWebViewConfiguration websiteDataStore]", reinterpret_cast<const void*>(web_data_store_default));
    objc_register_method("-[WKWebViewConfiguration setWebsiteDataStore:]", reinterpret_cast<const void*>(ignore_message_with_word));
    objc_register_method("-[WKWebViewConfiguration setApplicationNameForUserAgent:]", reinterpret_cast<const void*>(ignore_message_with_word));
    objc_register_method("-[userContentController addUserScript:]", reinterpret_cast<const void*>(ignore_message_with_word));
    objc_register_method("+[WKWebsiteDataStore defaultDataStore]", reinterpret_cast<const void*>(web_data_store_default));
    objc_register_method("+[WKContentWorld pageWorld]", reinterpret_cast<const void*>(content_world_page));
    objc_register_method("+[NSURLSessionConfiguration defaultSessionConfiguration]", reinterpret_cast<const void*>(url_session_configuration_default));
    objc_register_method("-[NSURLSessionConfiguration setTimeoutIntervalForRequest:]", reinterpret_cast<const void*>(ignore_message_with_double));
    objc_register_method("-[NSURLSessionConfiguration setHTTPAdditionalHeaders:]", reinterpret_cast<const void*>(ignore_message_with_word));
    objc_register_method("+[NSURLSession sessionWithConfiguration:]", reinterpret_cast<const void*>(url_session_with_configuration));
    objc_register_method("-[NSObject init]", reinterpret_cast<const void*>(return_self));
    objc_register_method("-[NSObject self]", reinterpret_cast<const void*>(return_self));
    objc_register_method("-[NSObject retain]", reinterpret_cast<const void*>(return_self));
    objc_register_method("-[NSObject autorelease]", reinterpret_cast<const void*>(return_self));
    objc_register_method("-[NSUserDefaults initWithSuiteName:]", reinterpret_cast<const void*>(return_self_with_argument));
    objc_register_method("+[NSProcessInfo processInfo]", reinterpret_cast<const void*>(process_info_shared));
    objc_register_method("-[NSProcessInfo operatingSystemVersion]", reinterpret_cast<const void*>(process_info_version));
    objc_register_method("+[NSBundle mainBundle]", reinterpret_cast<const void*>(main_bundle));
    objc_register_method("+[NSBundle bundleForClass:]", reinterpret_cast<const void*>(bundle_for_class));
    objc_register_method("-[NSBundle objectForInfoDictionaryKey:]", reinterpret_cast<const void*>(bundle_info_value));
    objc_register_method("+[NSFileManager defaultManager]", reinterpret_cast<const void*>(file_manager_default));
    objc_register_method("-[NSFileManager fileExistsAtPath:]", reinterpret_cast<const void*>(file_exists_at_path));
    objc_register_method("+[UIApplication sharedApplication]", reinterpret_cast<const void*>(shared_application));
    objc_register_method("-[UIApplication setIdleTimerDisabled:]", reinterpret_cast<const void*>(ignore_message_with_word));
    objc_register_method("+[CLLocationManager authorizationStatus]", reinterpret_cast<const void*>(location_authorization_status));
    objc_register_method("-[CLLocationManager authorizationStatus]", reinterpret_cast<const void*>(location_authorization_status));
    objc_register_method("-[CLLocationManager setDelegate:]", reinterpret_cast<const void*>(ignore_message_with_word));
    objc_register_method("-[CLLocationManager setDesiredAccuracy:]", reinterpret_cast<const void*>(ignore_message_with_double));
    objc_register_method("-[CLLocationManager setAllowsBackgroundLocationUpdates:]", reinterpret_cast<const void*>(ignore_message_with_word));
    objc_register_method("-[CLLocationManager setPausesLocationUpdatesAutomatically:]", reinterpret_cast<const void*>(ignore_message_with_word));
    objc_register_method("-[CLLocationManager requestWhenInUseAuthorization]", reinterpret_cast<const void*>(ignore_message));
    objc_register_method("-[CLLocationManager startUpdatingLocation]", reinterpret_cast<const void*>(ignore_message));
    objc_register_method("-[CLLocationManager stopUpdatingLocation]", reinterpret_cast<const void*>(ignore_message));
    objc_register_method("+[NSUserDefaults standardUserDefaults]", reinterpret_cast<const void*>(defaults_standard));
    objc_register_method("-[NSUserDefaults stringForKey:]", reinterpret_cast<const void*>(defaults_string_for_key));
    objc_register_method("-[NSUserDefaults objectForKey:]", reinterpret_cast<const void*>(defaults_object_for_key));
    objc_register_method("-[NSUserDefaults stringArrayForKey:]", reinterpret_cast<const void*>(defaults_string_array_for_key));
    objc_register_method("-[NSUserDefaults boolForKey:]", reinterpret_cast<const void*>(defaults_bool_for_key));
    objc_register_method("-[NSUserDefaults integerForKey:]", reinterpret_cast<const void*>(defaults_integer_for_key));
    objc_register_method("-[NSUserDefaults setBool:forKey:]", reinterpret_cast<const void*>(defaults_set_bool));
    objc_register_method("-[NSUserDefaults setInteger:forKey:]", reinterpret_cast<const void*>(defaults_set_integer));
    objc_register_method("-[NSUserDefaults setObject:forKey:]", reinterpret_cast<const void*>(defaults_set_object));
    objc_register_method("-[NSUserDefaults removeObjectForKey:]", reinterpret_cast<const void*>(defaults_remove));
}

void* foundation_symbol(std::string_view name) {
    static const std::unordered_map<std::string_view, void*> table = {
        {"kCLLocationAccuracyKilometer", const_cast<double*>(&kLocationAccuracyKilometer)},
        {"$sSS10FoundationE19_bridgeToObjectiveCSo8NSStringCyF", reinterpret_cast<void*>(string_bridge_to_objc)},
        {"$sSS10FoundationE36_unconditionallyBridgeFromObjectiveCySSSo8NSStringCSgFZ", reinterpret_cast<void*>(string_bridge_from_objc)},
    };
    const auto found = table.find(name);
    return found == table.end() ? nullptr : found->second;
}
