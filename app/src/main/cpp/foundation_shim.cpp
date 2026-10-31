#include "foundation_shim.h"

#include <algorithm>

#include <dirent.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <ctime>

#include <atomic>
#include <cerrno>

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <sstream>
#include <unordered_map>
#include <vector>

#include "concurrency_shim.h"
#include "dispatch_shim.h"
#include "engine.h"
#include "json.h"
#include "localization.h"
#include "objc_runtime.h"
#include "stdlib_shim.h"
#include "type_system.h"

#define SWIFTCC __attribute__((swiftcall))
#define SWIFT_OUT __attribute__((swift_indirect_result))
#define SWIFT_SELF __attribute__((swift_context))
#define SWIFT_CTX __attribute__((swift_context))

uint64_t* empty_array_singleton();

// Defaults values that are defined after the types they wrap.
void* strings_object_from_json(const std::string& json);
bool strings_json_from_object(void* object, std::string& json);
bool date_from_object(void* object, double& seconds);
void* new_date_object(double seconds);

extern "C" void* rt_swift_retain(void*);
extern "C" void* darwin_block_copy(const void*);

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

struct AnyValue {
    uint64_t buffer[3];
    void* type;
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

// Imports resolve before classes register, so these get their class later.
std::vector<void*>& string_constants() {
    static std::vector<void*> constants;
    return constants;
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
        for (const auto& [key, value] : values) output << escape(key) << '\n'
                                                       << escape(value) << '\n';
    }

    static std::string escape(const std::string& text) {
        std::string out;
        for (const char c : text) {
            if (c == '\n')
                out += "\\n";
            else if (c == '\\')
                out += "\\\\";
            else
                out += c;
        }
        return out;
    }

    static std::string unescape(const std::string& text) {
        std::string out;
        for (size_t i = 0; i < text.size(); ++i) {
            if (text[i] == '\\' && i + 1 < text.size())
                out += text[++i] == 'n' ? '\n' : text[i];
            else
                out += text[i];
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

// Stored values carry their type in a one-letter prefix, so objects come back as what was saved.
constexpr char kStoredString = 's';
constexpr char kStoredBool = 'b';
constexpr char kStoredInteger = 'i';
constexpr char kStoredStrings = 'a';
constexpr char kStoredDate = 'd';

// Defined with the bridged collections further down.
bool bridged_collection_value(void* object, void*& value);

extern "C" void* rt_swift_bridgeObjectRetain(void*);

struct NSNumberObject {
    NativeObject header;
    int64_t value;
    bool is_bool;
};

void* g_nsnumber_class = nullptr;

void* new_nsnumber(int64_t value, bool is_bool) {
    auto* number = static_cast<NSNumberObject*>(calloc(1, sizeof(NSNumberObject)));
    number->header.isa = g_nsnumber_class;
    number->header.references = 1;
    number->value = value;
    number->is_bool = is_bool;
    return number;
}

const std::string* stored_value(void* key) {
    const auto found = defaults().values.find(nsstring_text(key));
    return found == defaults().values.end() || found->second.empty() ? nullptr : &found->second;
}

void* defaults_string_for_key(void*, const char*, void* key) {
    const auto* stored = stored_value(key);
    return stored == nullptr || (*stored)[0] != kStoredString ? nullptr : new_nsstring(stored->substr(1));
}

void* defaults_object_for_key(void*, const char*, void* key) {
    const auto* stored = stored_value(key);
    if (stored == nullptr) return nullptr;
    const std::string body = stored->substr(1);
    switch ((*stored)[0]) {
        case kStoredBool:
            return new_nsnumber(body == "1", true);
        case kStoredInteger:
            return new_nsnumber(strtoll(body.c_str(), nullptr, 10), false);
        case kStoredStrings:
            return strings_object_from_json(body);
        case kStoredDate:
            return new_date_object(strtod(body.c_str(), nullptr));
        default:
            return new_nsstring(body);
    }
}

void* defaults_string_array_for_key(void*, const char*, void* key) {
    const auto* stored = stored_value(key);
    return stored == nullptr || (*stored)[0] != kStoredStrings ? nullptr : strings_object_from_json(stored->substr(1));
}

bool defaults_bool_for_key(void*, const char*, void* key) {
    const auto* stored = stored_value(key);
    return stored != nullptr && stored->substr(1) == "1";
}

int64_t defaults_integer_for_key(void*, const char*, void* key) {
    const auto* stored = stored_value(key);
    return stored == nullptr ? 0 : strtoll(stored->c_str() + 1, nullptr, 10);
}

void defaults_set_bool(void*, const char*, bool value, void* key) {
    defaults().values[nsstring_text(key)] = std::string(1, kStoredBool) + (value ? "1" : "0");
    defaults().save();
}

void defaults_set_integer(void*, const char*, int64_t value, void* key) {
    defaults().values[nsstring_text(key)] = kStoredInteger + std::to_string(value);
    defaults().save();
}

void defaults_set_object(void*, const char*, void* value, void* key) {
    const void* isa = value != nullptr ? static_cast<NativeObject*>(value)->isa : nullptr;
    if (value == nullptr) {
        defaults().values.erase(nsstring_text(key));
    } else if (isa == g_nsstring_class) {
        defaults().values[nsstring_text(key)] = kStoredString + nsstring_text(value);
    } else if (isa == g_nsnumber_class) {
        const auto* number = static_cast<NSNumberObject*>(value);
        defaults().values[nsstring_text(key)] = (number->is_bool ? kStoredBool : kStoredInteger) + std::to_string(number->value);
    } else {
        std::string json;
        double seconds;
        if (strings_json_from_object(value, json)) {
            defaults().values[nsstring_text(key)] = kStoredStrings + json;
        } else if (date_from_object(value, seconds)) {
            defaults().values[nsstring_text(key)] = kStoredDate + std::to_string(seconds);
        } else {
            engine_log("DEFAULTS: not saving a value of an unsupported type for %s", nsstring_text(key).c_str());
        }
    }
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
void* file_manager_default(void*, const char*) { return framework_singleton("NSFileManager"); }
void* shared_application(void*, const char*) { return framework_singleton("UIApplication"); }

bool file_exists_at_path(void*, const char*, void* path) { return access(nsstring_text(path).c_str(), F_OK) == 0; }

// iOS's coarsest accuracy constant, in metres; the value only needs to be the one the app passes back.
const double kLocationAccuracyKilometer = 1000.0;

// NSArray keeps retained references; the app only receives arrays from this runtime.
struct NSArrayObject {
    NativeObject header;
    std::vector<void*>* items;
};

void* g_nsarray_class = nullptr;

void destroy_nsarray(void* raw) {
    auto* array = static_cast<NSArrayObject*>(raw);
    delete array->items;
    free(array);
}

void* new_nsarray() {
    auto* array = static_cast<NSArrayObject*>(calloc(1, sizeof(NSArrayObject)));
    array->header.isa = g_nsarray_class;
    array->header.references = 1;
    array->items = new std::vector<void*>();
    return array;
}

struct BlockHeader {
    void* isa;
    int32_t flags;
    int32_t reserved;
    void (*invoke)(void*, void*);
};

struct URLBox;
URLBox* make_url_box(const std::string& text);

bool is_url_type(void* type) {
    const char* name = foreign_type_name(type);
    return name != nullptr && strcmp(name, "_$s10Foundation3URLVMn") == 0;
}

constexpr size_t kArrayHeaderBytes = 32;
constexpr size_t kArrayCountOffset = 16;
constexpr size_t kArrayCapacityOffset = 24;

SWIFTCC void* array_bridge_from_objc(void* object, void* element_type) {
    if (void* value; bridged_collection_value(object, value)) return rt_swift_bridgeObjectRetain(value);
    auto* array = static_cast<NSArrayObject*>(object);
    if (array == nullptr || array->items->empty()) return empty_array_singleton();
    const size_t count = array->items->size();
    // Strings and URLs change representation across the bridge; everything else is one object reference.
    if (element_type == type_system_symbol("$sSSN") || is_url_type(element_type)) {
        char* elements;
        void* converted = make_array_storage(element_type, count, &elements);
        for (size_t i = 0; i < count; ++i) {
            void* item = (*array->items)[i];
            if (element_type == type_system_symbol("$sSSN")) {
                const SwiftString text = swift_string_from_utf8(nsstring_text(item));
                memcpy(elements + i * sizeof text, &text, sizeof text);
            } else {
                URLBox* url = make_url_box(nsstring_text(item));
                memcpy(elements + i * sizeof url, &url, sizeof url);
            }
        }
        return converted;
    }
    auto* storage = static_cast<char*>(calloc(1, kArrayHeaderBytes + count * sizeof(void*)));
    auto* header = reinterpret_cast<NativeObject*>(storage);
    header->isa = storage_class_metadata("_$ss23_ContiguousArrayStorageCMn", &element_type, 1);
    header->references = 1;
    const uint64_t words[] = {count, count * 2};
    memcpy(storage + kArrayCountOffset, words, sizeof words);
    static_assert(kArrayCapacityOffset == kArrayCountOffset + sizeof(uint64_t));
    memcpy(storage + kArrayHeaderBytes, array->items->data(), count * sizeof(void*));
    return storage;
}

void* cookie_store(void*, const char*) { return framework_singleton("WKHTTPCookieStore"); }

void* web_view_layer(void*, const char*) { return framework_singleton("CALayer"); }

void* web_data_store_default(void*, const char*) { return framework_singleton("WKWebsiteDataStore"); }
void* content_world_page(void*, const char*) { return framework_singleton("WKContentWorld"); }
void* g_url_session_delegate = nullptr;

void* no_object(void*, const char*) { return nullptr; }

void* url_session_shared(void*, const char*) { return framework_singleton("NSURLSession"); }
void* device_current(void*, const char*) { return framework_singleton("UIDevice"); }
void* device_system_name(void*, const char*) { return new_nsstring("iOS"); }
void* device_model(void*, const char*) { return new_nsstring("iPhone"); }
int64_t device_idiom_phone(void*, const char*) { return 0; }

struct NotificationObserver {
    std::string name;
    void* block;
};

std::vector<NotificationObserver>& notification_observers() {
    static std::vector<NotificationObserver> observers;
    return observers;
}

void* notification_center_default(void*, const char*) { return framework_singleton("NSNotificationCenter"); }

// The block runs synchronously when posted, as it would with a nil queue; the returned token is the block.
void* notification_add_block(void*, const char*, void* name, void*, void*, void* block) {
    void* copy = darwin_block_copy(block);
    notification_observers().push_back({name == nullptr ? std::string() : nsstring_text(name), copy});
    return copy;
}

void notification_remove(void*, const char*, void* token) {
    auto& observers = notification_observers();
    observers.erase(std::remove_if(observers.begin(), observers.end(), [token](const NotificationObserver& o) { return o.block == token; }), observers.end());
}

void notification_post_name(void*, const char*, void* name, void*) {
    using Invoke = void (*)(void*, void*);
    struct Literal {
        void* isa;
        int32_t flags, reserved;
        Invoke invoke;
    };
    const std::string text = nsstring_text(name);
    const auto observers = notification_observers();
    for (const auto& observer : observers) {
        if (observer.name.empty() || observer.name == text) static_cast<Literal*>(observer.block)->invoke(observer.block, framework_singleton("NSNotification"));
    }
}
void* url_session_configuration_default(void*, const char*) { return framework_singleton("NSURLSessionConfiguration"); }
void* url_session_with_configuration(void*, const char*, void*) { return framework_singleton("NSURLSession"); }
void* url_session_with_delegate(void*, const char*, void*, void* delegate, void*) {
    void* session = framework_singleton("NSURLSession");
    g_url_session_delegate = delegate;
    if (delegate != nullptr) rt_swift_retain(delegate);
    return session;
}

// Foundation.URL is resilient, so the guest only ever holds this object through one reference.
struct URLBox {
    void* metadata;
    std::atomic<uint64_t> references;
    std::string* text;
};

void destroy_url(void* raw) {
    auto* box = static_cast<URLBox*>(raw);
    delete box->text;
    free(box);
}

const StorageMetadata kUrlMetadata = {destroy_url, nullptr, 0};

URLBox* new_url(const std::string& text) {
    auto* box = static_cast<URLBox*>(calloc(1, sizeof(URLBox)));
    box->metadata = const_cast<uint64_t*>(&kUrlMetadata.kind);
    new (&box->references) std::atomic<uint64_t>(1);
    box->text = new std::string(text);
    return box;
}

constexpr const char* kFileScheme = "file://";

bool make_directories(const std::string& path) {
    for (size_t i = 1; i <= path.size(); ++i) {
        if (i == path.size() || path[i] == '/') {
            const std::string prefix = path.substr(0, i);
            if (mkdir(prefix.c_str(), 0700) != 0 && errno != EEXIST) return false;
        }
    }
    return true;
}

std::string url_path(const std::string& text) {
    size_t start = 0;
    const size_t scheme_end = text.find("://");
    if (scheme_end != std::string::npos) {
        start = text.find('/', scheme_end + 3);
        if (start == std::string::npos) return {};
    }
    const size_t end = text.find_first_of("?#", start);
    return text.substr(start, end == std::string::npos ? std::string::npos : end - start);
}

std::string path_last_component(std::string path) {
    while (path.size() > 1 && path.back() == '/') path.pop_back();
    const size_t slash = path.rfind('/');
    return slash == std::string::npos ? path : path.substr(slash + 1);
}

std::string text_without_query(const std::string& text) { return text.substr(0, text.find_first_of("?#")); }

SWIFTCC void url_file_with_path(__attribute__((swift_indirect_result)) URLBox** out, SwiftString path) { *out = new_url("file://" + swift_string_to_utf8(path)); }

SWIFTCC void url_documents(__attribute__((swift_indirect_result)) URLBox** out) { *out = new_url("file://" + engine_bundle_dir() + "/.Documents/"); }

std::string appended_component(const std::string& text, const std::string& component, bool directory) {
    const std::string base = text_without_query(text);
    std::string joined = base;
    if (joined.empty() || joined.back() != '/') joined += '/';
    joined += component;
    if (directory && (joined.empty() || joined.back() != '/')) joined += '/';
    return joined;
}

SWIFTCC void url_appending_component(__attribute__((swift_indirect_result)) URLBox** out, SwiftString component, __attribute__((swift_context)) URLBox** self) {
    *out = new_url(appended_component(*(*self)->text, swift_string_to_utf8(component), false));
}

SWIFTCC void url_appending_directory(__attribute__((swift_indirect_result)) URLBox** out, SwiftString component, bool directory, __attribute__((swift_context)) URLBox** self) {
    *out = new_url(appended_component(*(*self)->text, swift_string_to_utf8(component), directory));
}

SWIFTCC void url_appending_extension(__attribute__((swift_indirect_result)) URLBox** out, SwiftString extension, __attribute__((swift_context)) URLBox** self) {
    std::string base = text_without_query(*(*self)->text);
    while (base.size() > 1 && base.back() == '/') base.pop_back();
    *out = new_url(base + "." + swift_string_to_utf8(extension));
}

SWIFTCC void url_deleting_extension(__attribute__((swift_indirect_result)) URLBox** out, __attribute__((swift_context)) URLBox** self) {
    std::string base = text_without_query(*(*self)->text);
    while (base.size() > 1 && base.back() == '/') base.pop_back();
    const size_t dot = base.rfind('.');
    const size_t slash = base.rfind('/');
    if (dot != std::string::npos && (slash == std::string::npos || dot > slash + 1)) base.erase(dot);
    *out = new_url(base);
}

SWIFTCC SwiftString url_last_component(__attribute__((swift_context)) URLBox** self) { return swift_string_from_utf8(path_last_component(url_path(*(*self)->text))); }

SWIFTCC SwiftString url_path_extension(__attribute__((swift_context)) URLBox** self) {
    const std::string name = path_last_component(url_path(*(*self)->text));
    const size_t dot = name.rfind('.');
    return swift_string_from_utf8(dot == std::string::npos || dot == 0 ? std::string() : name.substr(dot + 1));
}

SWIFTCC SwiftString url_path_percent(bool, __attribute__((swift_context)) URLBox** self) { return swift_string_from_utf8(url_path(*(*self)->text)); }
SWIFTCC bool url_is_file(__attribute__((swift_context)) URLBox** self) { return (*self)->text->rfind("file:", 0) == 0; }
SWIFTCC bool url_has_directory_path(__attribute__((swift_context)) URLBox** self) {
    const std::string base = text_without_query(*(*self)->text);
    return !base.empty() && base.back() == '/';
}
SWIFTCC bool url_equal(URLBox** left, URLBox** right) { return *(*left)->text == *(*right)->text; }
SWIFTCC bool url_start_security_scope(__attribute__((swift_context)) void*) { return true; }
SWIFTCC void url_absolute(__attribute__((swift_indirect_result)) URLBox** out, __attribute__((swift_context)) URLBox** self) { *out = new_url(*(*self)->text); }
SWIFTCC void url_no_base(__attribute__((swift_indirect_result)) URLBox** out, __attribute__((swift_context)) void*) { *out = nullptr; }

struct UrlAuthority {
    std::string host;
    int64_t port = -1;
};

UrlAuthority url_authority(const std::string& text) {
    UrlAuthority result;
    const size_t scheme = text.find("://");
    if (scheme == std::string::npos) return result;
    const size_t start = scheme + 3;
    std::string authority = text.substr(start, text.find_first_of("/?#", start) - start);
    if (const size_t at = authority.rfind('@'); at != std::string::npos) authority.erase(0, at + 1);
    const size_t colon = authority.rfind(':');
    if (colon != std::string::npos && authority.find(']', colon) == std::string::npos) {
        result.port = strtol(authority.c_str() + colon + 1, nullptr, 10);
        authority.erase(colon);
    }
    result.host = authority;
    return result;
}

SWIFTCC void url_string_relative(__attribute__((swift_indirect_result)) URLBox** out, SwiftString value, URLBox** base) {
    const std::string relative = swift_string_to_utf8(value);
    if (base == nullptr || *base == nullptr || relative.find("://") != std::string::npos) {
        *out = new_url(relative);
        return;
    }
    *out = new_url(appended_component(*(*base)->text, relative, false));
}

struct OptionalIntResult {
    int64_t value;
    bool is_none;
};

SWIFTCC OptionalIntResult url_port(__attribute__((swift_context)) URLBox** self) {
    const int64_t port = url_authority(*(*self)->text).port;
    return {port < 0 ? 0 : port, port < 0};
}

SWIFTCC void url_application_support(__attribute__((swift_indirect_result)) URLBox** out) {
    const std::string directory = engine_bundle_dir() + "/.Library/Application Support";
    make_directories(directory);
    *out = new_url(kFileScheme + directory + "/");
}

SWIFTCC void url_from_string(__attribute__((swift_indirect_result)) URLBox** out, SwiftString value) {
    const std::string text = swift_string_to_utf8(value);
    *out = text.empty() ? nullptr : new_url(text);
}

SWIFTCC SwiftString url_absolute_string(__attribute__((swift_context)) URLBox** self) { return swift_string_from_utf8(*(*self)->text); }
SWIFTCC SwiftString url_path_getter(__attribute__((swift_context)) URLBox** self) { return swift_string_from_utf8(url_path(*(*self)->text)); }

struct OptionalSwiftString {
    uint64_t count_and_flags;
    uint64_t object;
};

SWIFTCC OptionalSwiftString url_host(__attribute__((swift_context)) URLBox** self) {
    const std::string host = url_authority(*(*self)->text).host;
    if (host.empty()) return {0, 0};
    const SwiftString value = swift_string_from_utf8(host);
    return {value.count_and_flags, value.object};
}

SWIFTCC OptionalSwiftString url_query(__attribute__((swift_context)) URLBox** self) {
    const std::string& text = *(*self)->text;
    const size_t question = text.find('?');
    if (question == std::string::npos) return {0, 0};
    const SwiftString value = swift_string_from_utf8(text.substr(question + 1, text.find('#', question) - question - 1));
    return {value.count_and_flags, value.object};
}

SWIFTCC OptionalSwiftString url_scheme(__attribute__((swift_context)) URLBox** self) {
    const size_t end = (*self)->text->find(':');
    if (end == std::string::npos) return {0, 0};
    const SwiftString scheme = swift_string_from_utf8((*self)->text->substr(0, end));
    return {scheme.count_and_flags, scheme.object};
}

SWIFTCC void url_appending_path(__attribute__((swift_indirect_result)) URLBox** out, const SwiftString* component, const void*, void*, void*, __attribute__((swift_context)) URLBox** self) {
    std::string base = *(*self)->text;
    if (!base.empty() && base.back() != '/') base += '/';
    *out = new_url(base + swift_string_to_utf8(*component));
}

SWIFTCC void url_deleting_last_component(__attribute__((swift_indirect_result)) URLBox** out, __attribute__((swift_context)) URLBox** self) {
    std::string text = *(*self)->text;
    if (!text.empty() && text.back() == '/') text.pop_back();
    const size_t slash = text.rfind('/');
    *out = new_url(slash == std::string::npos ? text : text.substr(0, slash + 1));
}

void* g_nsurl_class = nullptr;

URLBox* make_url_box(const std::string& text) { return new_url(text); }

void* new_nsurl(const std::string& text) {
    auto* object = static_cast<NSStringObject*>(new_nsstring(text));
    object->header.isa = g_nsurl_class;
    return object;
}

std::string file_url_text(const std::string& path) { return "file://" + path; }

std::string nsobject_path(void* object) {
    const std::string& text = nsstring_text(object);
    return static_cast<NativeObject*>(object)->isa == g_nsurl_class ? url_path(text) : text;
}

void set_cocoa_error(void** error, const std::string& message) {
    if (error != nullptr) *error = foundation_make_error(message, errno);
}

void* bundle_url(void*, const char*) { return new_nsurl(file_url_text(engine_bundle_dir() + "/")); }
void* bundle_path(void*, const char*) { return new_nsstring(engine_bundle_dir()); }

void* bundle_url_for_resource(void*, const char*, void* name, void* extension) {
    std::string path = engine_bundle_dir() + "/" + (name == nullptr ? std::string() : nsstring_text(name));
    if (extension != nullptr && !nsstring_text(extension).empty()) path += "." + nsstring_text(extension);
    return access(path.c_str(), F_OK) == 0 ? new_nsurl(file_url_text(path)) : nullptr;
}

constexpr int64_t kDocumentDirectory = 9;
constexpr int64_t kCachesDirectory = 13;

std::string sandbox_directory(int64_t directory) {
    switch (directory) {
        case kDocumentDirectory:
            return engine_bundle_dir() + "/.Documents";
        case kCachesDirectory:
            return engine_bundle_dir() + "/.Library/Caches";
        default:
            return engine_bundle_dir() + "/.Library/Application Support";
    }
}

void make_directory_tree(const std::string& path) {
    for (size_t at = path.find('/', 1); at != std::string::npos; at = path.find('/', at + 1)) mkdir(path.substr(0, at).c_str(), 0700);
    mkdir(path.c_str(), 0700);
}

void* file_manager_urls(void*, const char*, int64_t directory, uint64_t) {
    const std::string path = sandbox_directory(directory);
    make_directory_tree(path);
    auto* array = static_cast<NSArrayObject*>(new_nsarray());
    array->items->push_back(new_nsurl(file_url_text(path + "/")));
    return array;
}

void* file_manager_temporary_directory(void*, const char*) {
    const std::string path = engine_bundle_dir() + "/.tmp";
    make_directory_tree(path);
    return new_nsurl(file_url_text(path + "/"));
}

bool file_manager_exists_directory(void*, const char*, void* path, bool* is_directory) {
    struct stat info{};
    if (stat(nsstring_text(path).c_str(), &info) != 0) return false;
    if (is_directory != nullptr) *is_directory = S_ISDIR(info.st_mode);
    return true;
}

bool file_manager_create_path(void*, const char*, void* path, bool, void*, void**) {
    make_directory_tree(nsobject_path(path));
    return true;
}

bool remove_recursively(const std::string& path) {
    struct stat info{};
    if (lstat(path.c_str(), &info) != 0) return false;
    if (S_ISDIR(info.st_mode)) {
        if (DIR* directory = opendir(path.c_str())) {
            while (const dirent* entry = readdir(directory)) {
                const std::string name = entry->d_name;
                if (name != "." && name != "..") remove_recursively(path + "/" + name);
            }
            closedir(directory);
        }
        return rmdir(path.c_str()) == 0;
    }
    return unlink(path.c_str()) == 0;
}

bool file_manager_remove(void*, const char*, void* item, void** error) {
    if (remove_recursively(nsobject_path(item))) return true;
    set_cocoa_error(error, "Could not remove " + nsobject_path(item));
    return false;
}

bool file_manager_move(void*, const char*, void* from, void* to, void** error) {
    if (rename(nsobject_path(from).c_str(), nsobject_path(to).c_str()) == 0) return true;
    set_cocoa_error(error, "Could not move " + nsobject_path(from));
    return false;
}

bool copy_recursively(const std::string& from, const std::string& to) {
    struct stat info{};
    if (stat(from.c_str(), &info) != 0) return false;
    if (S_ISDIR(info.st_mode)) {
        make_directory_tree(to);
        DIR* directory = opendir(from.c_str());
        if (directory == nullptr) return false;
        bool copied = true;
        while (const dirent* entry = readdir(directory)) {
            const std::string name = entry->d_name;
            if (name != "." && name != "..") copied = copy_recursively(from + "/" + name, to + "/" + name) && copied;
        }
        closedir(directory);
        return copied;
    }
    FILE* input = fopen(from.c_str(), "rb");
    if (input == nullptr) return false;
    FILE* output = fopen(to.c_str(), "wb");
    if (output == nullptr) {
        fclose(input);
        return false;
    }
    char chunk[65536];
    for (size_t got; (got = fread(chunk, 1, sizeof chunk, input)) > 0;) fwrite(chunk, 1, got, output);
    fclose(input);
    return fclose(output) == 0;
}

bool file_manager_copy(void*, const char*, void* from, void* to, void** error) {
    if (copy_recursively(nsobject_path(from), nsobject_path(to))) return true;
    set_cocoa_error(error, "Could not copy " + nsobject_path(from));
    return false;
}

void* directory_listing(const std::string& path, bool as_urls) {
    auto* array = static_cast<NSArrayObject*>(new_nsarray());
    if (DIR* directory = opendir(path.c_str())) {
        while (const dirent* entry = readdir(directory)) {
            const std::string name = entry->d_name;
            if (name == "." || name == "..") continue;
            array->items->push_back(as_urls ? new_nsurl(file_url_text(path + "/" + name)) : new_nsstring(name));
        }
        closedir(directory);
    }
    return array;
}

void* file_manager_contents_path(void*, const char*, void* path, void**) { return directory_listing(nsobject_path(path), false); }
void* file_manager_contents_url(void*, const char*, void* url, void*, uint64_t, void**) {
    std::string path = nsobject_path(url);
    while (path.size() > 1 && path.back() == '/') path.pop_back();
    return directory_listing(path, true);
}

SWIFTCC void* url_bridge_to_objc(__attribute__((swift_context)) URLBox** self) {
    auto* object = static_cast<NSStringObject*>(new_nsstring(*(*self)->text));
    object->header.isa = g_nsurl_class;
    return object;
}

SWIFTCC void url_bridge_from_objc(__attribute__((swift_indirect_result)) URLBox** out, void* object) { *out = new_url(nsstring_text(object)); }

const int32_t kDirectoryHintInferFromPath = 2;

bool file_manager_create_directory(void*, const char*, void* url, bool, void*, void** error) {
    if (error != nullptr) *error = nullptr;
    return make_directories(url_path(nsstring_text(url)));
}

SWIFTCC void localization_value_literal(SWIFT_OUT LocalizedBox** out, SwiftString literal) { *out = new_localized_box(swift_string_to_utf8(literal)); }
SWIFTCC void localization_interpolation_init(SWIFT_OUT LocalizedBox** out, int64_t, int64_t) { *out = new_localized_box(""); }
SWIFTCC void localization_append_literal(SwiftString literal, SWIFT_SELF LocalizedBox** self) { *(*self)->key += swift_string_to_utf8(literal); }

SWIFTCC void localization_append_string(SwiftString value, SWIFT_SELF LocalizedBox** self) {
    *(*self)->key += "%@";
    (*self)->arguments->push_back(swift_string_to_utf8(value));
}

SWIFTCC void localization_append_integer(const int64_t* value, SwiftString specifier, void*, void*, SWIFT_SELF LocalizedBox** self) {
    *(*self)->key += swift_string_to_utf8(specifier);
    (*self)->arguments->push_back(std::to_string(*value));
}

// Interpolations are passed indirectly and consumed: ownership of the box moves to the new value.
SWIFTCC void localization_value_interpolation(SWIFT_OUT LocalizedBox** out, LocalizedBox** interpolation) { *out = *interpolation; }

LocalizedBox* copy_localized(const LocalizedBox* source) {
    auto* copy = new_localized_box(*source->key);
    *copy->arguments = *source->arguments;
    return copy;
}

SWIFTCC void localized_resource_init(SWIFT_OUT LocalizedBox** out, LocalizedBox** value) { *out = copy_localized(*value); }
SWIFTCC void localized_resource_init_default(SWIFT_OUT LocalizedBox** out, const char* key, int64_t length, uint64_t, LocalizedBox** default_value) {
    *out = copy_localized(*default_value);
    *(*out)->key = std::string(key, static_cast<size_t>(length));
}

SWIFTCC SwiftString string_localized(LocalizedBox** resource) { return swift_string_from_utf8(localized_text(*resource)); }

constexpr uint64_t kLocaleImmediate = 1ULL << 63;
SWIFTCC void locale_current(SWIFT_OUT uint64_t* out) { *out = kLocaleImmediate; }

const int32_t kBundleDescriptionForClass = 0;

constexpr uint32_t kUrlSessionContextBytes = 64;
constexpr uint64_t kDataEmptyTag = 3ULL << 62;
constexpr uint64_t kDataSliceTag = 1ULL << 62;
constexpr uint64_t kDataLargeTag = 2ULL << 62;

// The storage behind a slice-form Data. The app only reaches its fields through imported accessors.
struct DataStorage {
    void* metadata;
    std::atomic<uint64_t> references;
    uint8_t* bytes;
    int64_t length;
    int64_t capacity;
    int64_t offset;
};

void destroy_data_storage(void* raw) {
    auto* storage = static_cast<DataStorage*>(raw);
    free(storage->bytes);
    free(storage);
}

const StorageMetadata kDataStorageMetadata = {destroy_data_storage, nullptr, 0};

struct DataValue {
    uint64_t range;
    uint64_t storage;
};

DataValue make_data(const std::string& bytes) {
    if (bytes.empty()) return {0, kDataEmptyTag};
    auto* storage = static_cast<DataStorage*>(calloc(1, sizeof(DataStorage)));
    storage->metadata = const_cast<uint64_t*>(&kDataStorageMetadata.kind);
    new (&storage->references) std::atomic<uint64_t>(1);
    storage->bytes = static_cast<uint8_t*>(malloc(bytes.size()));
    memcpy(storage->bytes, bytes.data(), bytes.size());
    storage->length = static_cast<int64_t>(bytes.size());
    storage->capacity = storage->length;
    // Range<Int32>: lower bound in the low half, upper bound in the high half.
    const uint64_t range = static_cast<uint64_t>(bytes.size()) << 32;
    return {range, reinterpret_cast<uint64_t>(storage) | kDataSliceTag};
}

// __DataStorage allocated by the guest's inlined Data initializers, which read its size from the class.
void* data_storage_class_metadata() {
    static void* block[12] = {};
    if (block[0] == nullptr) {
        block[0] = reinterpret_cast<void*>(destroy_data_storage);
        const uint32_t instance_size = sizeof(DataStorage);
        const uint16_t alignment_mask = 7;
        memcpy(reinterpret_cast<char*>(block + 2) + 0x30, &instance_size, sizeof instance_size);
        memcpy(reinterpret_cast<char*>(block + 2) + 0x34, &alignment_mask, sizeof alignment_mask);
    }
    return block + 2;
}

struct DataMetadataResponse {
    void* metadata;
    size_t state;
};

SWIFTCC DataMetadataResponse data_storage_access(size_t) { return {data_storage_class_metadata(), 0}; }

void data_storage_fill(DataStorage* self, const void* bytes, int64_t length, int64_t offset) {
    self->bytes = static_cast<uint8_t*>(calloc(1, length > 0 ? static_cast<size_t>(length) : 1));
    if (bytes != nullptr && length > 0) memcpy(self->bytes, bytes, static_cast<size_t>(length));
    self->length = length;
    self->capacity = length;
    self->offset = offset;
}

SWIFTCC DataStorage* data_storage_init_bytes(const void* bytes, int64_t length, __attribute__((swift_context)) DataStorage* self) {
    data_storage_fill(self, bytes, length, 0);
    return self;
}

SWIFTCC DataStorage* data_storage_init_length(int64_t length, __attribute__((swift_context)) DataStorage* self) {
    data_storage_fill(self, nullptr, length, 0);
    return self;
}

using DataDeallocator = SWIFTCC void (*)(void*, int64_t, __attribute__((swift_context)) void*);

SWIFTCC DataStorage* data_storage_init_full(void* bytes, int64_t length, bool, DataDeallocator deallocator, void* deallocator_context, int64_t offset, __attribute__((swift_context)) DataStorage* self) {
    data_storage_fill(self, bytes, length, offset);
    if (deallocator != nullptr) deallocator(bytes, length, deallocator_context);
    return self;
}

SWIFTCC uint8_t* data_storage_bytes(__attribute__((swift_context)) DataStorage* self) { return self->bytes; }
SWIFTCC int64_t data_storage_length(__attribute__((swift_context)) DataStorage* self) { return self->length; }
SWIFTCC int64_t data_storage_offset(__attribute__((swift_context)) DataStorage* self) { return self->offset; }

// An Error existential is one reference to a box; this box carries a domain, a code and a message.
struct ErrorBox {
    void* metadata;
    std::atomic<uint64_t> references;
    std::string* message;
    int64_t code;
};

void destroy_error_box(void* raw) {
    auto* box = static_cast<ErrorBox*>(raw);
    delete box->message;
    free(box);
}

const StorageMetadata kErrorBoxMetadata = {destroy_error_box, nullptr, 0};

void* make_error(const std::string& message, int64_t code) {
    auto* box = static_cast<ErrorBox*>(calloc(1, sizeof(ErrorBox)));
    box->metadata = const_cast<uint64_t*>(&kErrorBoxMetadata.kind);
    new (&box->references) std::atomic<uint64_t>(1);
    box->message = new std::string(message);
    box->code = code;
    return box;
}

// Errors the app throws itself: a box with the value's type and Error witness table, then the value.
struct GuestErrorBox {
    void* metadata;
    std::atomic<uint64_t> references;
    void* type;
    void* witnesses;
};

constexpr size_t kGuestErrorValueOffset = sizeof(GuestErrorBox);
constexpr size_t kWitnessDestroyOffset = sizeof(void*);
constexpr size_t kWitnessSizeOffset = 0x40;
constexpr size_t kWitnessFlagsOffset = 0x50;
constexpr size_t kWitnessInitTakeOffset = 4 * sizeof(void*);
constexpr size_t kWitnessInitCopyOffset = 2 * sizeof(void*);

const char* value_witnesses(void* type) { return static_cast<const char*>(static_cast<void**>(type)[-1]); }

size_t guest_error_value_offset(void* type) {
    uint32_t flags;
    memcpy(&flags, value_witnesses(type) + kWitnessFlagsOffset, sizeof flags);
    const size_t mask = flags & 0xff;
    return (kGuestErrorValueOffset + mask) & ~mask;
}

void destroy_guest_error(void* raw) {
    auto* box = static_cast<GuestErrorBox*>(raw);
    void (*destroy)(void*, void*);
    memcpy(&destroy, value_witnesses(box->type) + kWitnessDestroyOffset, sizeof destroy);
    destroy(static_cast<char*>(raw) + guest_error_value_offset(box->type), box->type);
    free(raw);
}

const StorageMetadata kGuestErrorMetadata = {destroy_guest_error, nullptr, 0};

struct AllocatedError {
    void* box;
    void* value;
};

SWIFTCC AllocatedError swift_alloc_error(void* type, void* witnesses, void* initial, bool is_take) {
    size_t size;
    memcpy(&size, value_witnesses(type) + kWitnessSizeOffset, sizeof size);
    const size_t offset = guest_error_value_offset(type);
    auto* box = static_cast<GuestErrorBox*>(calloc(1, offset + size + 1));
    box->metadata = const_cast<uint64_t*>(&kGuestErrorMetadata.kind);
    new (&box->references) std::atomic<uint64_t>(1);
    box->type = type;
    box->witnesses = witnesses;
    char* value = reinterpret_cast<char*>(box) + offset;
    if (initial != nullptr) {
        void* (*initialize)(void*, void*, void*);
        memcpy(&initialize, value_witnesses(type) + (is_take ? kWitnessInitTakeOffset : kWitnessInitCopyOffset), sizeof initialize);
        initialize(value, initial, type);
    }
    return {box, value};
}

struct ErrorValue {
    void* value;
    void* type;
    void* witnesses;
};

// Runtime errors report a null type, which localizedDescription uses to tell them apart.
void swift_get_error_value(void* error, void**, ErrorValue* out) {
    auto* box = static_cast<GuestErrorBox*>(error);
    if (box->metadata == &kGuestErrorMetadata.kind) {
        *out = {reinterpret_cast<char*>(box) + guest_error_value_offset(box->type), box->type, box->witnesses};
    } else {
        *out = {error, nullptr, nullptr};
    }
}

SWIFTCC SwiftString error_localized_description(void* type, void*, __attribute__((swift_context)) void* value) {
    if (type == nullptr) return swift_string_from_utf8(*static_cast<ErrorBox*>(value)->message);
    return swift_string_from_utf8("The operation could not be completed.");
}

SWIFTCC DataValue data_contents_of(URLBox** url, uint64_t, __attribute__((swift_context)) void*, __attribute__((swift_error_result)) void** error) {
    const std::string path = url_path(*(*url)->text);
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        *error = make_error("The file could not be read: " + path, ENOENT);
        return {0, kDataEmptyTag};
    }
    std::string bytes((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    return make_data(bytes);
}

SWIFTCC void data_write(URLBox** url, uint64_t, uint64_t range, uint64_t storage, __attribute__((swift_context)) void*, __attribute__((swift_error_result)) void** error) {
    const std::string path = url_path(*(*url)->text);
    const size_t slash = path.rfind('/');
    if (slash != std::string::npos) make_directories(path.substr(0, slash));
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    if (!file) {
        *error = make_error("The file could not be written: " + path, EACCES);
        return;
    }
    if ((storage >> 62) == (kDataSliceTag >> 62)) {
        const auto* backing = reinterpret_cast<const DataStorage*>(storage & ~(3ULL << 62));
        const auto low = static_cast<uint32_t>(range);
        const auto high = static_cast<uint32_t>(range >> 32);
        file.write(reinterpret_cast<const char*>(backing->bytes) + low, high - low);
    }
}

// Foundation.URLRequest is resilient here too: one reference to this box.
struct URLRequestBox {
    void* metadata;
    std::atomic<uint64_t> references;
    std::string* url;
    std::string* method;
    std::string* headers;
    std::string* body;
};

void destroy_url_request(void* raw) {
    auto* box = static_cast<URLRequestBox*>(raw);
    delete box->url;
    delete box->method;
    delete box->headers;
    delete box->body;
    free(box);
}

const StorageMetadata kUrlRequestMetadata = {destroy_url_request, nullptr, 0};

SWIFTCC void url_request_init(SWIFT_OUT URLRequestBox** out, URLBox** url, uint64_t, double) {
    auto* box = static_cast<URLRequestBox*>(calloc(1, sizeof(URLRequestBox)));
    box->metadata = const_cast<uint64_t*>(&kUrlRequestMetadata.kind);
    new (&box->references) std::atomic<uint64_t>(1);
    box->url = new std::string(*(*url)->text);
    box->method = new std::string("GET");
    box->headers = new std::string();
    box->body = new std::string();
    *out = box;
}

SWIFTCC void url_request_set_method(SwiftString method, SWIFT_CTX URLRequestBox** self) {
    if (method.object != 0) *(*self)->method = swift_string_to_utf8(method);
}

SWIFTCC void url_request_set_header(SwiftString value, SwiftString field, SWIFT_CTX URLRequestBox** self) {
    if (value.object != 0) *(*self)->headers += swift_string_to_utf8(field) + ": " + swift_string_to_utf8(value) + "\n";
}

std::string data_bytes(uint64_t range, uint64_t storage) {
    const uint64_t tag = storage >> 62;
    if (tag == 0) {
        // Inline form: up to 14 bytes across the two words, with the count in the 15th.
        char inline_bytes[2 * sizeof(uint64_t)];
        memcpy(inline_bytes, &range, sizeof range);
        memcpy(inline_bytes + sizeof range, &storage, sizeof storage);
        return std::string(inline_bytes, static_cast<uint8_t>(storage >> 48));
    }
    if (tag == (kDataLargeTag >> 62)) {
        // Large form: the first word is a RangeReference holding a Range<Int> after its object header.
        const auto* reference = reinterpret_cast<const int64_t*>(range);
        const auto* backing = reinterpret_cast<const DataStorage*>(storage & ~(3ULL << 62));
        return std::string(reinterpret_cast<const char*>(backing->bytes) + reference[2], static_cast<size_t>(reference[3] - reference[2]));
    }
    if (tag != (kDataSliceTag >> 62)) return {};
    const auto* backing = reinterpret_cast<const DataStorage*>(storage & ~(3ULL << 62));
    const auto low = static_cast<uint32_t>(range);
    const auto high = static_cast<uint32_t>(range >> 32);
    return std::string(reinterpret_cast<const char*>(backing->bytes) + low, high - low);
}

void store_data(DataValue* self, const std::string& bytes) { *self = make_data(bytes); }

SWIFTCC void data_append(const char* start, const char* end, __attribute__((swift_context)) DataValue* self) {
    std::string bytes = data_bytes(self->range, self->storage);
    if (start != nullptr) bytes.append(start, static_cast<size_t>(end - start));
    store_data(self, bytes);
}

SWIFTCC void data_replace_subrange(int64_t lower, int64_t upper, const char* replacement, int64_t count, __attribute__((swift_context)) DataValue* self) {
    std::string bytes = data_bytes(self->range, self->storage);
    bytes.replace(static_cast<size_t>(lower), static_cast<size_t>(upper - lower), replacement != nullptr ? std::string(replacement, static_cast<size_t>(count)) : std::string());
    store_data(self, bytes);
}

SWIFTCC void url_request_set_body(uint64_t range, uint64_t storage, SWIFT_CTX URLRequestBox** self) { *(*self)->body = data_bytes(range, storage); }
SWIFTCC void url_request_get_url(SWIFT_OUT URLBox** out, SWIFT_CTX URLRequestBox** self) { *out = new_url(*(*self)->url); }

// NSHTTPURLResponse: the only part the app reads is the status.
struct HTTPResponseObject {
    NativeObject header;
    int64_t status;
};

void* g_http_response_class = nullptr;

int64_t http_response_status(void* self, const char*) { return static_cast<HTTPResponseObject*>(self)->status; }

struct AsyncDataResult {
    DataValue data;
    void* response;
    void* error;
};

std::string g_session_headers;

AsyncDataResult perform_request(const std::string& url, const std::string& method, const std::string& headers, const std::string& body) {
    const HttpResult result = engine_http(url, method, g_session_headers + headers, body);
    engine_log("HTTP: %s %s -> %d, %zu bytes", method.c_str(), url.c_str(), result.status, result.body.size());
    if (result.status < 0) return {{0, kDataEmptyTag}, nullptr, make_error("The network request could not be completed.", 1)};
    auto* response = static_cast<HTTPResponseObject*>(calloc(1, sizeof(HTTPResponseObject)));
    response->header.isa = g_http_response_class;
    response->header.references = 1;
    response->status = result.status;
    return {make_data(result.body), response, nullptr};
}

extern "C" void rt_url_session_fetch_url(AsyncDataResult* out, URLBox** url) { *out = perform_request(*(*url)->text, "GET", "", ""); }
extern "C" void rt_url_session_fetch_request(AsyncDataResult* out, URLRequestBox** request) {
    *out = perform_request(*(*request)->url, *(*request)->method, *(*request)->headers, *(*request)->body);
}

extern "C" void rt_url_session_data_url();
extern "C" void rt_url_session_data_request();

// An async function pointer: a relative address, then the callee's context size.
struct AsyncFunctionRecord {
    int32_t function;
    uint32_t context_size;
};

AsyncFunctionRecord g_url_session_url_record;
AsyncFunctionRecord g_url_session_request_record;

// A bridged collection only needs to stay alive until framework code uses it, so it wraps the Swift value.
struct BridgedCollection {
    NativeObject header;
    void* value;
};

void* g_bridged_collection_class = nullptr;
extern "C" void rt_swift_release(void*);
extern "C" void* rt_swift_retain(void*);
extern "C" void* rt_swift_bridgeObjectRetain(void*);
extern "C" void rt_swift_bridgeObjectRelease(void*);

void destroy_bridged_collection(void* raw) {
    rt_swift_bridgeObjectRelease(static_cast<BridgedCollection*>(raw)->value);
    free(raw);
}

SWIFTCC void* collection_bridge_to_objc(void* value) {
    auto* wrapper = static_cast<BridgedCollection*>(calloc(1, sizeof(BridgedCollection)));
    wrapper->header.isa = g_bridged_collection_class;
    wrapper->header.references = 1;
    wrapper->value = rt_swift_bridgeObjectRetain(value);
    return wrapper;
}

bool bridged_collection_value(void* object, void*& value) {
    if (object == nullptr || static_cast<NativeObject*>(object)->isa != g_bridged_collection_class) return false;
    value = static_cast<BridgedCollection*>(object)->value;
    return true;
}

// Format styles are one tagged word here; the only option this app sets is compact notation.
constexpr uint64_t kStyleWord = kLocaleImmediate;
constexpr uint64_t kCompactNotationFlag = 1;

SWIFTCC void integer_style_init(SWIFT_OUT uint64_t* out, const void*, void*) { *out = kStyleWord; }
SWIFTCC void notation_compact_name(SWIFT_OUT uint64_t* out) { *out = kStyleWord | kCompactNotationFlag; }
SWIFTCC void integer_style_notation(SWIFT_OUT uint64_t* out, const uint64_t* notation, void*, SWIFT_CTX const uint64_t* self) { *out = *self | (*notation & kCompactNotationFlag); }

std::string group_thousands(int64_t value) {
    std::string digits = std::to_string(value < 0 ? -value : value);
    std::string out;
    for (size_t i = 0; i < digits.size(); ++i) {
        if (i != 0 && (digits.size() - i) % 3 == 0) out += ',';
        out += digits[i];
    }
    return value < 0 ? "-" + out : out;
}

std::string compact_number(int64_t value) {
    const double magnitude = static_cast<double>(value < 0 ? -value : value);
    static constexpr struct {
        double scale;
        const char* suffix;
    } kUnits[] = {{1e9, "B"}, {1e6, "M"}, {1e3, "K"}};
    for (const auto& unit : kUnits) {
        if (magnitude >= unit.scale) {
            char text[32];
            snprintf(text, sizeof text, "%s%.1f", value < 0 ? "-" : "", magnitude / unit.scale);
            std::string number = text;
            if (number.size() > 2 && number.compare(number.size() - 2, 2, ".0") == 0) number.resize(number.size() - 2);
            return number + unit.suffix;
        }
    }
    return std::to_string(value);
}

// FormatOutput is String for every style this app uses, written through the indirect result.
SWIFTCC void integer_formatted_style(SWIFT_OUT SwiftString* out, const uint64_t* style, void*, void*, SWIFT_CTX const int64_t* self) {
    const int64_t value = *self;
    *out = swift_string_from_utf8((*style & kCompactNotationFlag) ? compact_number(value) : group_thousands(value));
}

SWIFTCC SwiftString integer_formatted(void*, void*, SWIFT_CTX const int64_t* self) { return swift_string_from_utf8(group_thousands(*self)); }

SWIFTCC void date_now(SWIFT_OUT double* out) {
    struct timespec now{};
    clock_gettime(CLOCK_REALTIME, &now);
    constexpr double kSecondsFrom1970To2001 = 978307200.0;
    *out = static_cast<double>(now.tv_sec) - kSecondsFrom1970To2001 + static_cast<double>(now.tv_nsec) * 1e-9;
}

SWIFTCC void date_style_word(SWIFT_OUT uint64_t* out) { *out = kStyleWord; }

SWIFTCC SwiftString date_formatted(const void*, const void*, SWIFT_CTX const double* self) {
    constexpr double kSecondsFrom1970To2001 = 978307200.0;
    const auto seconds = static_cast<time_t>(*self + kSecondsFrom1970To2001);
    struct tm local{};
    localtime_r(&seconds, &local);
    char text[64];
    strftime(text, sizeof text, "%b %e, %Y at %H:%M", &local);
    return swift_string_from_utf8(text);
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

// Async throwing functions return values in x0-x2 and the error in x20, through the context's resume word.
#define URL_SESSION_ENTRY(name, fetch) \
    ".globl " #name "\n" #name         \
    ":\n"                              \
    "  stp x29, x30, [sp, #-48]!\n"    \
    "  mov x29, sp\n"                  \
    "  mov x1, x0\n"                   \
    "  add x0, sp, #16\n"              \
    "  bl " #fetch                     \
    "\n"                               \
    "  ldp x0, x1, [sp, #16]\n"        \
    "  ldp x2, x20, [sp, #32]\n"       \
    "  ldp x29, x30, [sp], #48\n"      \
    "  ldr x3, [x22, #8]\n"            \
    "  br x3\n"

// Suspends: control returns to the scheduler until the host reports the script's result.
__asm__(
    ".text\n"
    ".globl rt_web_view_call_script_entry\n"
    "rt_web_view_call_script_entry:\n"
    "  stp x29, x30, [sp, #-16]!\n"
    "  mov x29, sp\n"
    "  mov x6, x22\n"
    "  mov x7, x20\n"
    "  bl rt_web_view_call_script\n"
    "  ldp x29, x30, [sp], #16\n"
    "  ret\n");

__asm__(
    ".text\n" URL_SESSION_ENTRY(rt_url_session_data_url, rt_url_session_fetch_url)
        URL_SESSION_ENTRY(rt_url_session_data_request, rt_url_session_fetch_request));

constexpr size_t kDictionaryKeysOffset = 48;
constexpr size_t kDictionaryValuesOffset = 56;
constexpr size_t kDictionaryBitmapOffset = 64;
constexpr size_t kDictionaryScaleOffset = 32;
constexpr size_t kAnyHashableStride = 40;
constexpr size_t kAnyStride = 32;
constexpr uint64_t kCollectionPointerMask = 0x00fffffffffffff8ULL;
constexpr size_t kBitsPerWord = 64;

// The session's headers are a [AnyHashable: Any] of strings, read directly from its native storage.
void session_set_headers(void*, const char*, void* wrapper) {
    g_session_headers.clear();
    if (wrapper == nullptr) return;
    const auto bits = reinterpret_cast<uint64_t>(static_cast<BridgedCollection*>(wrapper)->value);
    const auto* storage = reinterpret_cast<const char*>(bits & kCollectionPointerMask);
    const int scale = static_cast<int8_t>(storage[kDictionaryScaleOffset]);
    const char* keys;
    const char* values;
    memcpy(&keys, storage + kDictionaryKeysOffset, sizeof keys);
    memcpy(&values, storage + kDictionaryValuesOffset, sizeof values);
    const char* bitmap = storage + kDictionaryBitmapOffset;
    for (size_t bucket = 0; bucket < (1ULL << scale); ++bucket) {
        uint64_t word;
        memcpy(&word, bitmap + bucket / kBitsPerWord * sizeof word, sizeof word);
        if (!((word >> (bucket % kBitsPerWord)) & 1)) continue;
        const std::string name = swift_string_to_utf8(*reinterpret_cast<const SwiftString*>(keys + bucket * kAnyHashableStride));
        const std::string value = swift_string_to_utf8(*reinterpret_cast<const SwiftString*>(values + bucket * kAnyStride));
        g_session_headers += name + ": " + value + "\n";
    }
}

SWIFTCC void url_error_code(SWIFT_OUT uint64_t* out) { *out = kStyleWord; }
SWIFTCC void url_error_init(SWIFT_OUT uint64_t* out, const void*, void*) { *out = kStyleWord; }
SWIFTCC void* url_error_ns_error(void*) { return make_error("The server's response could not be used.", 1); }

struct WebConfiguration {
    std::vector<std::string> scripts;
    std::string application_name;
    NativeObject* controller = nullptr;
};

std::map<const void*, WebConfiguration*>& web_configurations() {
    static std::map<const void*, WebConfiguration*> table;
    return table;
}

WebConfiguration* configuration_of(void* object) {
    auto*& entry = web_configurations()[object];
    if (entry == nullptr) entry = new WebConfiguration();
    return entry;
}

std::map<const void*, std::string>& user_script_sources() {
    static std::map<const void*, std::string> table;
    return table;
}

struct WebViewState {
    int id;
    void* configuration;
    std::string url;
};

std::map<const void*, WebViewState>& web_views() {
    static std::map<const void*, WebViewState> table;
    return table;
}

std::string json_quote(const std::string& text) {
    std::string out = "\"";
    for (const char c : text) {
        const auto byte = static_cast<unsigned char>(c);
        if (c == '"' || c == '\\') {
            out += '\\';
            out += c;
        } else if (c == '\n') {
            out += "\\n";
        } else if (byte < 0x20) {
            char escaped[8];
            snprintf(escaped, sizeof escaped, "\\u%04x", byte);
            out += escaped;
        } else {
            out += c;
        }
    }
    return out + "\"";
}

void* user_script_init(void* self, const char*, void* source, int64_t, bool) {
    user_script_sources()[self] = nsstring_text(source);
    return self;
}

void* configuration_controller(void* self, const char*) {
    auto* configuration = configuration_of(self);
    if (configuration->controller == nullptr) {
        configuration->controller = static_cast<NativeObject*>(calloc(1, sizeof(NativeObject)));
        static void* controller_class = objc_native_class("WKUserContentController", free);
        configuration->controller->isa = controller_class;
        configuration->controller->references = 1ULL << 63;
        web_configurations()[configuration->controller] = configuration;
    }
    return configuration->controller;
}

void controller_add_script(void* self, const char*, void* script) { configuration_of(self)->scripts.push_back(user_script_sources()[script]); }
void configuration_set_application_name(void* self, const char*, void* name) { configuration_of(self)->application_name = nsstring_text(name); }

void* web_view_init(void* self, const char*, double, double, double, double, void* configuration) {
    static int next_id = 0;
    const int id = next_id++;
    // The web view owns its configuration, as WKWebView does.
    web_views()[self] = {id, rt_swift_retain(configuration), {}};
    auto* config = configuration_of(configuration);
    std::string scripts;
    for (const auto& script : config->scripts) {
        if (!scripts.empty()) scripts += ',';
        scripts += json_quote(script);
    }
    engine_emit("webnew", "{\"id\":" + std::to_string(id) + ",\"name\":" + json_quote(config->application_name) + ",\"scripts\":[" + scripts + "]}");
    return self;
}

void* web_view_configuration(void* self, const char*) { return web_views()[self].configuration; }

// Bridged requests only need to carry their URL into loadRequest:.
// A bridged request reads as its URL; the full request stays here for URLSession to find.
std::unordered_map<void*, URLRequestBox*>& bridged_requests() {
    static std::unordered_map<void*, URLRequestBox*> requests;
    return requests;
}

SWIFTCC void* url_request_bridge_to_objc(SWIFT_CTX URLRequestBox** self) {
    auto* object = static_cast<NSStringObject*>(new_nsstring(*(*self)->url));
    object->header.isa = g_nsurl_class;
    rt_swift_retain(*self);
    bridged_requests()[object] = *self;
    return object;
}

void* web_view_load(void* self, const char*, void* request) {
    engine_emit("webload", "{\"id\":" + std::to_string(web_views()[self].id) + ",\"url\":" + json_quote(nsstring_text(request)) + "}");
    return nullptr;
}

void* web_view_url(void* self, const char*) {
    const std::string& url = web_views()[self].url;
    if (url.empty()) return nullptr;
    auto* object = static_cast<NSStringObject*>(new_nsstring(url));
    object->header.isa = g_nsurl_class;
    return object;
}

bool is_any_type(void* type) { return static_cast<char*>(type) == static_cast<char*>(type_system_symbol("$sypN")) + sizeof(void*); }

// Swift values to JSON, for the arguments of a script call.
std::string value_to_json(void* value, void* type) {
    if (type == type_system_symbol("$sSSN")) return json_quote(swift_string_to_utf8(*static_cast<SwiftString*>(value)));
    if (type == type_system_symbol("$sSbN")) return *static_cast<uint8_t*>(value) ? "true" : "false";
    if (type == type_system_symbol("$sSiN")) return std::to_string(*static_cast<int64_t*>(value));
    if (is_any_type(type)) {
        auto* any = static_cast<AnyValue*>(value);
        return any->type == nullptr ? "null" : value_to_json(any->buffer, any->type);
    }
    constexpr size_t kWitnessStrideOffset = 0x48;
    const auto stride_of = [&](void* element) {
        size_t stride;
        memcpy(&stride, value_witnesses(element) + kWitnessStrideOffset, sizeof stride);
        return stride;
    };
    const auto storage_of = [&]() { return reinterpret_cast<const char*>(*static_cast<uint64_t*>(value) & kCollectionPointerMask); };
    if (void* element = array_element_type(type)) {
        const char* storage = storage_of();
        int64_t count;
        memcpy(&count, storage + kArrayCountOffset, sizeof count);
        std::string out = "[";
        const size_t stride = stride_of(element);
        for (int64_t i = 0; i < count; ++i) {
            if (i != 0) out += ',';
            out += value_to_json(const_cast<char*>(storage) + kArrayHeaderBytes + static_cast<size_t>(i) * stride, element);
        }
        return out + "]";
    }
    if (void* value_type = dictionary_value_type(type)) {
        const char* storage = storage_of();
        const int scale = static_cast<int8_t>(storage[kDictionaryScaleOffset]);
        const char* keys;
        const char* values;
        memcpy(&keys, storage + kDictionaryKeysOffset, sizeof keys);
        memcpy(&values, storage + kDictionaryValuesOffset, sizeof values);
        const char* bitmap = storage + kDictionaryBitmapOffset;
        const size_t stride = stride_of(value_type);
        std::string out = "{";
        for (size_t bucket = 0; bucket < (1ULL << scale); ++bucket) {
            uint64_t word;
            memcpy(&word, bitmap + bucket / kBitsPerWord * sizeof word, sizeof word);
            if (!((word >> (bucket % kBitsPerWord)) & 1)) continue;
            if (out.size() > 1) out += ',';
            out += json_quote(swift_string_to_utf8(*reinterpret_cast<const SwiftString*>(keys + bucket * sizeof(SwiftString)))) + ":";
            out += value_to_json(const_cast<char*>(values) + bucket * stride, value_type);
        }
        return out + "}";
    }
    engine_log("STOP: a script argument of a type this runtime cannot convert");
    abort();
}

void json_to_any(const Json& json, AnyValue* out);

struct PendingScriptCall {
    AnyValue* out;
    void* context;
};

std::map<int, PendingScriptCall>& pending_script_calls() {
    static std::map<int, PendingScriptCall> table;
    return table;
}

extern "C" void rt_web_view_call_script(AnyValue* out, uint64_t body_bits, uint64_t body_object, void* arguments, void*, void*, void* context, void* self) {
    static int next_call = 0;
    const int call = next_call++;
    pending_script_calls()[call] = {out, context};
    void* type = resolve_mangled_type("SDySSypG", nullptr);
    const std::string json = value_to_json(&arguments, type);
    engine_emit("webjs", "{\"id\":" + std::to_string(web_views()[self].id) + ",\"call\":" + std::to_string(call) + ",\"body\":" + json_quote(swift_string_to_utf8({body_bits, body_object})) + ",\"args\":" + json + "}");
}

extern "C" void rt_web_view_call_script_entry();
AsyncFunctionRecord g_web_script_record;

struct NSCookieObject {
    NativeObject header;
    std::string* domain;
    std::string* name;
    std::string* value;
};

void* g_cookie_class = nullptr;
void* cookie_name(void* self, const char*) { return new_nsstring(*static_cast<NSCookieObject*>(self)->name); }
void* cookie_value(void* self, const char*) { return new_nsstring(*static_cast<NSCookieObject*>(self)->value); }
void* cookie_domain(void* self, const char*) { return new_nsstring(*static_cast<NSCookieObject*>(self)->domain); }

void* g_cookie_observer = nullptr;
void cookie_store_add_observer(void*, const char*, void* observer) { g_cookie_observer = observer; }

void* bundle_info_value_string(void*, const char*, void* key) {
    const std::string value = bundle_info_string(nsstring_text(key));
    return value.empty() ? nullptr : new_nsstring(value);
}

void cookie_store_get_all(void*, const char*, BlockHeader* completion) {
    auto* array = static_cast<NSArrayObject*>(new_nsarray());
    std::istringstream lines(engine_cookies());
    std::string line;
    while (std::getline(lines, line)) {
        const size_t first = line.find('\t');
        const size_t second = first == std::string::npos ? first : line.find('\t', first + 1);
        if (second == std::string::npos) continue;
        auto* cookie = static_cast<NSCookieObject*>(calloc(1, sizeof(NSCookieObject)));
        cookie->header.isa = g_cookie_class;
        cookie->header.references = 1;
        cookie->domain = new std::string(line.substr(0, first));
        cookie->name = new std::string(line.substr(first + 1, second - first - 1));
        cookie->value = new std::string(line.substr(second + 1));
        array->items->push_back(cookie);
    }
    completion->invoke(completion, array);
}

// Only what the script results here need: an object of booleans, or a single value.
void write_script_result(AnyValue* out, const std::string& json) {
    memset(out, 0, sizeof *out);
    std::string text = json;
    text.erase(0, text.find_first_not_of(" \n\t"));
    if (text.empty() || text == "null") return;
    if (text == "true" || text == "false") {
        const bool flag = text == "true";
        memcpy(out->buffer, &flag, sizeof flag);
        out->type = type_system_symbol("$sSbN");
        return;
    }
    Json parsed;
    if (json_parse(text, parsed)) {
        json_to_any(parsed, out);
        return;
    }
    engine_log("SCRIPT: ignoring a result this runtime does not convert: %.60s", text.c_str());
}

void* any_type() { return static_cast<char*>(type_system_symbol("$sypN")) + sizeof(void*); }

// A JSON value as the Swift value Foundation would bridge it to; null stays an object no `as?` accepts.
void json_to_any(const Json& json, AnyValue* out) {
    memset(out, 0, sizeof *out);
    switch (json.kind) {
        case Json::Kind::Null:
            out->buffer[0] = reinterpret_cast<uint64_t>(framework_singleton("NSNull"));
            out->type = any_object_metadata();
            return;
        case Json::Kind::Bool:
            memcpy(out->buffer, &json.flag, sizeof json.flag);
            out->type = type_system_symbol("$sSbN");
            return;
        case Json::Kind::Integer:
            memcpy(out->buffer, &json.integer, sizeof json.integer);
            out->type = type_system_symbol("$sSiN");
            return;
        case Json::Kind::Number:
            memcpy(out->buffer, &json.number, sizeof json.number);
            out->type = type_system_symbol("$sSdN");
            return;
        case Json::Kind::String: {
            const SwiftString text = swift_string_from_utf8(json.text);
            memcpy(out->buffer, &text, sizeof text);
            out->type = type_system_symbol("$sSSN");
            return;
        }
        case Json::Kind::Array: {
            char* elements;
            void* storage = make_array_storage(any_type(), json.items.size(), &elements);
            for (size_t i = 0; i < json.items.size(); ++i) json_to_any(json.items[i], reinterpret_cast<AnyValue*>(elements) + i);
            out->buffer[0] = reinterpret_cast<uint64_t>(storage);
            out->type = resolve_mangled_type("SayypG", nullptr);
            return;
        }
        case Json::Kind::Object: {
            std::vector<AnyValue> values(json.members.size());
            std::vector<std::pair<std::string, const void*>> entries;
            for (size_t i = 0; i < json.members.size(); ++i) {
                json_to_any(json.members[i].second, &values[i]);
                entries.emplace_back(json.members[i].first, &values[i]);
            }
            out->buffer[0] = reinterpret_cast<uint64_t>(make_string_dictionary(any_type(), entries));
            out->type = resolve_mangled_type("SDySSypG", nullptr);
            return;
        }
    }
}

// NSData and the object JSONSerialization returns: thin holders that only live until the guest bridges them.
struct NSDataObject {
    NativeObject header;
    std::string* bytes;
};

struct JSONValueObject {
    NativeObject header;
    Json* value;
};

void* g_nsdata_class = nullptr;
void* g_json_value_class = nullptr;

void destroy_nsdata(void* raw) {
    delete static_cast<NSDataObject*>(raw)->bytes;
    free(raw);
}

void destroy_json_value(void* raw) {
    delete static_cast<JSONValueObject*>(raw)->value;
    free(raw);
}

void* make_nsdata(const std::string& bytes) {
    auto* object = static_cast<NSDataObject*>(calloc(1, sizeof(NSDataObject)));
    object->header.isa = g_nsdata_class;
    object->header.references = 1;
    object->bytes = new std::string(bytes);
    return object;
}

SWIFTCC void* data_bridge_to_objc(uint64_t range, uint64_t storage) { return make_nsdata(data_bytes(range, storage)); }
SWIFTCC DataValue data_bridge_from_objc(NSDataObject* object) {
    return object == nullptr ? DataValue{0, kDataEmptyTag} : make_data(*object->bytes);
}

constexpr int64_t kCocoaPropertyListReadCorrupt = 3840;

void* strings_object_from_json(const std::string& json) {
    Json parsed;
    json_parse(json, parsed);
    void* element = type_system_symbol("$sSSN");
    char* elements;
    void* storage = make_array_storage(element, parsed.items.size(), &elements);
    for (size_t i = 0; i < parsed.items.size(); ++i) reinterpret_cast<SwiftString*>(elements)[i] = swift_string_from_utf8(parsed.items[i].text);
    void* object = collection_bridge_to_objc(storage);
    rt_swift_bridgeObjectRelease(storage);
    return object;
}

// Every collection the app stores in defaults is a [String].
bool strings_json_from_object(void* object, std::string& json) {
    if (static_cast<NativeObject*>(object)->isa != g_bridged_collection_class) return false;
    json = value_to_json(&static_cast<BridgedCollection*>(object)->value, resolve_mangled_type("SaySSG", nullptr));
    return true;
}

struct NSDateObject {
    NativeObject header;
    double seconds;
};

void* g_nsdate_class = nullptr;

void* new_date_object(double seconds) {
    auto* object = static_cast<NSDateObject*>(calloc(1, sizeof(NSDateObject)));
    object->header.isa = g_nsdate_class;
    object->header.references = 1;
    object->seconds = seconds;
    return object;
}

bool date_from_object(void* object, double& seconds) {
    if (static_cast<NativeObject*>(object)->isa != g_nsdate_class) return false;
    seconds = static_cast<NSDateObject*>(object)->seconds;
    return true;
}

SWIFTCC void* date_bridge_to_objc(double seconds) { return new_date_object(seconds); }

// Only the keys of dictionaryRepresentation are read, so stored values all surface as their text.
void* defaults_dictionary_representation(void*, const char*) {
    auto* value = new Json;
    value->kind = Json::Kind::Object;
    for (const auto& [key, stored] : defaults().values) {
        Json member;
        member.kind = Json::Kind::String;
        member.text = stored.empty() ? "" : stored.substr(1);
        value->members.emplace_back(key, std::move(member));
    }
    auto* object = static_cast<JSONValueObject*>(calloc(1, sizeof(JSONValueObject)));
    object->header.isa = g_json_value_class;
    object->header.references = 1;
    object->value = value;
    return object;
}

SWIFTCC void* dictionary_bridge_from_objc(void* object) {
    if (void* value; bridged_collection_value(object, value)) return rt_swift_bridgeObjectRetain(value);
    if (object != nullptr && static_cast<NativeObject*>(object)->isa == g_json_value_class) {
        AnyValue any;
        json_to_any(*static_cast<JSONValueObject*>(object)->value, &any);
        return reinterpret_cast<void*>(any.buffer[0]);
    }
    return make_string_dictionary(any_type(), {});
}

void* regular_expression_escaped_pattern(void*, const char*, void* text) {
    static const std::string kSpecial = "\\*?+[(){}^$|./";
    std::string out;
    for (const char c : nsstring_text(text)) {
        if (kSpecial.find(c) != std::string::npos) out += '\\';
        out += c;
    }
    return new_nsstring(out);
}

void* json_object_with_data(void*, const char*, NSDataObject* data, uint64_t, void** error) {
    auto parsed = std::make_unique<Json>();
    if (data == nullptr || !json_parse(*data->bytes, *parsed)) {
        if (error != nullptr) *error = make_error("The data couldn't be read because it isn't in the correct format.", kCocoaPropertyListReadCorrupt);
        return nullptr;
    }
    auto* object = static_cast<JSONValueObject*>(calloc(1, sizeof(JSONValueObject)));
    object->header.isa = g_json_value_class;
    object->header.references = 1;
    object->value = parsed.release();
    return object;
}

void* json_data_with_object(void*, const char*, BridgedCollection* object, uint64_t, void**) {
    // The app's only JSON bodies are bridged [String: Any] literals.
    return make_nsdata(value_to_json(&object->value, resolve_mangled_type("SDySSypG", nullptr)));
}

SWIFTCC void* error_identity(void* error) { return error != nullptr ? error : make_error("The operation could not be completed.", 1); }

// URLQueryItem and URLComponents are one retained reference each at runtime.
struct QueryItemBox {
    void* metadata;
    std::atomic<uint64_t> references;
    std::string* name;
    std::string* value;
};

void destroy_query_item(void* raw) {
    auto* box = static_cast<QueryItemBox*>(raw);
    delete box->name;
    delete box->value;
    free(box);
}

const StorageMetadata kQueryItemMetadata = {destroy_query_item, nullptr, 0};

QueryItemBox* new_query_item(const std::string& name, const std::string* value) {
    auto* box = static_cast<QueryItemBox*>(calloc(1, sizeof(QueryItemBox)));
    box->metadata = const_cast<uint64_t*>(&kQueryItemMetadata.kind);
    new (&box->references) std::atomic<uint64_t>(1);
    box->name = new std::string(name);
    box->value = value != nullptr ? new std::string(*value) : nullptr;
    return box;
}

SWIFTCC void query_item_init(SWIFT_OUT QueryItemBox** out, SwiftString name, SwiftString value) {
    if (value.object == 0) {
        *out = new_query_item(swift_string_to_utf8(name), nullptr);
        return;
    }
    const std::string text = swift_string_to_utf8(value);
    *out = new_query_item(swift_string_to_utf8(name), &text);
}

SWIFTCC SwiftString query_item_name(SWIFT_CTX QueryItemBox** self) { return swift_string_from_utf8(*(*self)->name); }
SWIFTCC SwiftString query_item_value(SWIFT_CTX QueryItemBox** self) {
    return (*self)->value != nullptr ? swift_string_from_utf8(*(*self)->value) : SwiftString{0, 0};
}

struct ComponentsBox {
    void* metadata;
    std::atomic<uint64_t> references;
    std::string* scheme;
    std::string* authority;
    std::string* path;
    std::string* query;
    std::string* fragment;
};

void destroy_components(void* raw) {
    auto* box = static_cast<ComponentsBox*>(raw);
    delete box->scheme;
    delete box->authority;
    delete box->path;
    delete box->query;
    delete box->fragment;
    free(box);
}

const StorageMetadata kComponentsMetadata = {destroy_components, nullptr, 0};

std::string* copied(const std::string* text) { return text != nullptr ? new std::string(*text) : nullptr; }

ComponentsBox* new_components() {
    auto* box = static_cast<ComponentsBox*>(calloc(1, sizeof(ComponentsBox)));
    box->metadata = const_cast<uint64_t*>(&kComponentsMetadata.kind);
    new (&box->references) std::atomic<uint64_t>(1);
    box->path = new std::string;
    return box;
}

ComponentsBox* parse_components(std::string text) {
    for (const char c : text) {
        if (static_cast<unsigned char>(c) <= ' ' || c == '"' || c == '<' || c == '>' || c == '\\' || c == '^' || c == '`' || c == '{' || c == '|' || c == '}') return nullptr;
    }
    auto* box = new_components();
    if (const size_t hash = text.find('#'); hash != std::string::npos) {
        box->fragment = new std::string(text.substr(hash + 1));
        text.resize(hash);
    }
    if (const size_t mark = text.find('?'); mark != std::string::npos) {
        box->query = new std::string(text.substr(mark + 1));
        text.resize(mark);
    }
    const size_t colon = text.find(':');
    if (colon != std::string::npos && text.find('/') > colon && colon > 0) {
        box->scheme = new std::string(text.substr(0, colon));
        text.erase(0, colon + 1);
    }
    if (text.compare(0, 2, "//") == 0) {
        const size_t slash = text.find('/', 2);
        box->authority = new std::string(text.substr(2, slash == std::string::npos ? std::string::npos : slash - 2));
        text.erase(0, slash == std::string::npos ? text.size() : slash);
    }
    *box->path = text;
    return box;
}

std::string percent_decoded(const std::string& text) {
    std::string out;
    for (size_t i = 0; i < text.size(); ++i) {
        if (text[i] == '%' && i + 2 < text.size() && isxdigit(static_cast<unsigned char>(text[i + 1])) && isxdigit(static_cast<unsigned char>(text[i + 2]))) {
            out += static_cast<char>(strtol(text.substr(i + 1, 2).c_str(), nullptr, 16));
            i += 2;
        } else {
            out += text[i];
        }
    }
    return out;
}

// RFC 3986 query characters, minus the separators a value must not introduce.
std::string percent_encoded_query(const std::string& text) {
    static const std::string kAllowed = "-._~!$'()*,;:@/?";
    std::string out;
    for (const char c : text) {
        if (isalnum(static_cast<unsigned char>(c)) || kAllowed.find(c) != std::string::npos) {
            out += c;
        } else {
            char escaped[4];
            snprintf(escaped, sizeof escaped, "%%%02X", static_cast<unsigned>(static_cast<unsigned char>(c)));
            out += escaped;
        }
    }
    return out;
}

SWIFTCC void components_from_url(SWIFT_OUT ComponentsBox** out, URLBox** url, bool) { *out = parse_components(*(*url)->text); }
SWIFTCC void components_from_string(SWIFT_OUT ComponentsBox** out, SwiftString text) { *out = parse_components(swift_string_to_utf8(text)); }

SWIFTCC void components_url(SWIFT_OUT URLBox** out, SWIFT_CTX ComponentsBox** self) {
    const ComponentsBox* box = *self;
    std::string text;
    if (box->scheme != nullptr) text += *box->scheme + ":";
    if (box->authority != nullptr) text += "//" + *box->authority;
    text += *box->path;
    if (box->query != nullptr) text += "?" + *box->query;
    if (box->fragment != nullptr) text += "#" + *box->fragment;
    *out = text.empty() ? nullptr : new_url(text);
}

SwiftString optional_text(const std::string* text) { return text != nullptr ? swift_string_from_utf8(*text) : SwiftString{0, 0}; }

SWIFTCC SwiftString components_scheme(SWIFT_CTX ComponentsBox** self) { return optional_text((*self)->scheme); }
SWIFTCC SwiftString components_path(SWIFT_CTX ComponentsBox** self) { return swift_string_from_utf8(percent_decoded(*(*self)->path)); }
SWIFTCC SwiftString components_host(SWIFT_CTX ComponentsBox** self) {
    if ((*self)->authority == nullptr) return {0, 0};
    std::string host = *(*self)->authority;
    if (const size_t at = host.rfind('@'); at != std::string::npos) host.erase(0, at + 1);
    if (const size_t colon = host.rfind(':'); colon != std::string::npos) host.resize(colon);
    return swift_string_from_utf8(percent_decoded(host));
}

SWIFTCC void* components_query_items(SWIFT_CTX ComponentsBox** self) {
    if ((*self)->query == nullptr) return nullptr;
    std::vector<QueryItemBox*> items;
    const std::string& query = *(*self)->query;
    for (size_t start = 0; start <= query.size();) {
        size_t end = query.find('&', start);
        if (end == std::string::npos) end = query.size();
        const std::string pair = query.substr(start, end - start);
        const size_t equals = pair.find('=');
        if (equals == std::string::npos) {
            items.push_back(new_query_item(percent_decoded(pair), nullptr));
        } else {
            const std::string value = percent_decoded(pair.substr(equals + 1));
            items.push_back(new_query_item(percent_decoded(pair.substr(0, equals)), &value));
        }
        start = end + 1;
    }
    char* elements;
    void* storage = make_array_storage(any_object_metadata(), items.size(), &elements);
    memcpy(elements, items.data(), items.size() * sizeof(void*));
    return storage;
}

// A new box each time, since URLComponents is a value and a copy may share the old one.
SWIFTCC void components_set_query_items(void* items, SWIFT_CTX ComponentsBox** self) {
    ComponentsBox* old = *self;
    auto* box = new_components();
    box->scheme = copied(old->scheme);
    box->authority = copied(old->authority);
    *box->path = *old->path;
    box->fragment = copied(old->fragment);
    if (items != nullptr) {
        char* elements;
        const size_t count = array_storage_contents(items, any_object_metadata(), &elements);
        std::string query;
        for (size_t i = 0; i < count; ++i) {
            const auto* item = reinterpret_cast<QueryItemBox**>(elements)[i];
            if (i != 0) query += '&';
            query += percent_encoded_query(*item->name);
            if (item->value != nullptr) query += "=" + percent_encoded_query(*item->value);
        }
        box->query = new std::string(query);
    }
    rt_swift_release(old);
    *self = box;
}

// CharacterSet is an immediate naming one of the two sets the app uses; retain and release skip small values.
constexpr uint64_t kCharacterSetWhitespaces = 1;
constexpr uint64_t kCharacterSetWhitespacesAndNewlines = 2;

SWIFTCC void character_set_whitespaces(SWIFT_OUT uint64_t* out) { *out = kCharacterSetWhitespaces; }
SWIFTCC void character_set_whitespaces_and_newlines(SWIFT_OUT uint64_t* out) { *out = kCharacterSetWhitespacesAndNewlines; }

// Decodes the scalar at `at`, setting `length`; malformed bytes count as themselves.
uint32_t utf8_scalar(const std::string& text, size_t at, size_t& length) {
    const auto lead = static_cast<unsigned char>(text[at]);
    length = lead < 0x80 ? 1 : lead < 0xE0 ? 2
                           : lead < 0xF0   ? 3
                                           : 4;
    if (at + length > text.size()) {
        length = 1;
        return lead;
    }
    if (length == 1) return lead;
    uint32_t scalar = lead & (0xFF >> (length + 1));
    for (size_t i = 1; i < length; ++i) scalar = (scalar << 6) | (static_cast<unsigned char>(text[at + i]) & 0x3F);
    return scalar;
}

bool in_character_set(uint32_t scalar, uint64_t set) {
    const bool whitespace = scalar == ' ' || scalar == '\t' || scalar == 0xA0 || scalar == 0x1680 || (scalar >= 0x2000 && scalar <= 0x200B) || scalar == 0x202F || scalar == 0x205F || scalar == 0x3000;
    if (whitespace) return true;
    if (set != kCharacterSetWhitespacesAndNewlines) return false;
    return (scalar >= 0x0A && scalar <= 0x0D) || scalar == 0x85 || scalar == 0x2028 || scalar == 0x2029;
}

SWIFTCC SwiftString string_trimming(const uint64_t* set, void* type, void*, SWIFT_CTX const SwiftString* self) {
    if (type != type_system_symbol("$sSSN")) {
        engine_log("STOP: trimmingCharacters on a type other than String");
        abort();
    }
    const std::string text = swift_string_to_utf8(*self);
    size_t start = 0;
    size_t end = text.size();
    while (start < end) {
        size_t length;
        if (!in_character_set(utf8_scalar(text, start, length), *set)) break;
        start += length;
    }
    while (end > start) {
        size_t at = end - 1;
        while (at > start && (static_cast<unsigned char>(text[at]) & 0xC0) == 0x80) --at;
        size_t length;
        if (!in_character_set(utf8_scalar(text, at, length), *set)) break;
        end = at;
    }
    return swift_string_from_utf8(text.substr(start, end - start));
}

// UUID is kept as its string, in the same box as a URL.
SWIFTCC void uuid_init(SWIFT_OUT URLBox** out) {
    uint8_t bytes[16];
    arc4random_buf(bytes, sizeof bytes);
    bytes[6] = (bytes[6] & 0x0F) | 0x40;
    bytes[8] = (bytes[8] & 0x3F) | 0x80;
    char text[37];
    snprintf(text, sizeof text, "%02X%02X%02X%02X-%02X%02X-%02X%02X-%02X%02X-%02X%02X%02X%02X%02X%02X", bytes[0], bytes[1], bytes[2], bytes[3], bytes[4], bytes[5], bytes[6], bytes[7], bytes[8], bytes[9], bytes[10], bytes[11], bytes[12], bytes[13], bytes[14], bytes[15]);
    *out = new_url(text);
}
SWIFTCC SwiftString uuid_string(SWIFT_CTX URLBox** self) { return swift_string_from_utf8(*(*self)->text); }

void stdlib_random(void* buffer, size_t count) { arc4random_buf(buffer, count); }

// Puts an Objective-C object into an Any as its Swift value, as the runtime's bridging does.
SWIFTCC void bridge_any_object_to_any(SWIFT_OUT AnyValue* out, void* object) {
    memset(out, 0, sizeof *out);
    const void* isa = object != nullptr ? static_cast<NativeObject*>(object)->isa : nullptr;
    if (isa == g_json_value_class) {
        json_to_any(*static_cast<JSONValueObject*>(object)->value, out);
    } else if (isa == g_nsdate_class) {
        const double seconds = static_cast<NSDateObject*>(object)->seconds;
        memcpy(out->buffer, &seconds, sizeof seconds);
        out->type = date_metadata();
    } else if (isa == g_nsstring_class) {
        const SwiftString text = swift_string_from_utf8(nsstring_text(object));
        memcpy(out->buffer, &text, sizeof text);
        out->type = type_system_symbol("$sSSN");
    } else if (isa == g_nsnumber_class) {
        const auto* number = static_cast<NSNumberObject*>(object);
        if (number->is_bool) {
            const bool flag = number->value != 0;
            memcpy(out->buffer, &flag, sizeof flag);
            out->type = type_system_symbol("$sSbN");
        } else {
            memcpy(out->buffer, &number->value, sizeof number->value);
            out->type = type_system_symbol("$sSiN");
        }
    } else {
        out->buffer[0] = reinterpret_cast<uint64_t>(object);
        out->type = any_object_metadata();
    }
}

std::string foundation_data_bytes(FoundationData data) { return data_bytes(data.range, data.storage); }
FoundationData foundation_make_data(const std::string& bytes) {
    const DataValue data = make_data(bytes);
    return {data.range, data.storage};
}
void* foundation_make_error(const std::string& message, int64_t code) { return make_error(message, code); }

int foundation_web_view_id(const void* object) {
    const auto found = web_views().find(object);
    return found == web_views().end() ? -1 : found->second.id;
}

void foundation_js_finished(int call, const std::string& json, bool failed) {
    engine_post_event([call, json, failed] {
        const auto found = pending_script_calls().find(call);
        if (found == pending_script_calls().end()) return;
        const PendingScriptCall pending = found->second;
        pending_script_calls().erase(found);
        void* error = nullptr;
        if (failed)
            error = make_error("The script could not run.", 1);
        else
            write_script_result(pending.out, json);
        concurrency_resume_call(pending.context, error);
    });
}

void foundation_web_url_changed(int web_view, const std::string& url) {
    engine_post_event([web_view, url] {
        for (auto& [object, state] : web_views()) {
            if (state.id == web_view) state.url = url;
        }
    });
}

void foundation_cookies_changed() {
    engine_post_event([] {
        if (g_cookie_observer != nullptr) objc_send(g_cookie_observer, "cookiesDidChangeInCookieStore:", framework_singleton("WKHTTPCookieStore"));
    });
}

std::string foundation_url_text(const void* url) { return *static_cast<const URLBox*>(url)->text; }

constexpr double kSecondsFrom1970To2001 = 978307200;

bool foundation_parse_iso8601(const std::string& text, double& seconds) {
    tm parts{};
    int consumed = 0;
    if (sscanf(text.c_str(), "%d-%d-%dT%d:%d:%d%n", &parts.tm_year, &parts.tm_mon, &parts.tm_mday, &parts.tm_hour, &parts.tm_min, &parts.tm_sec, &consumed) < 3) return false;
    parts.tm_year -= 1900;
    parts.tm_mon -= 1;
    double result = static_cast<double>(timegm(&parts));
    const char* rest = text.c_str() + consumed;
    if (consumed > 0 && *rest == '.') {
        char* end;
        result += strtod(rest, &end);
        rest = end;
    }
    int hours = 0, minutes = 0;
    if ((*rest == '+' || *rest == '-') && sscanf(rest + 1, "%d:%d", &hours, &minutes) >= 1) {
        const double offset = hours * 3600.0 + minutes * 60.0;
        result += *rest == '+' ? -offset : offset;
    }
    seconds = result - kSecondsFrom1970To2001;
    return true;
}

void* foundation_make_url(const std::string& text) { return make_url_box(text); }

void* foundation_native_array(void* object) {
    void* value = nullptr;
    return object != nullptr && bridged_collection_value(object, value) ? value : nullptr;
}

struct FormatArgument {
    void* buffer[3];
    void* type;
    void* table;
};

std::string format_argument_text(const FormatArgument& argument, const std::string& specifier) {
    char text[64];
    if (argument.type == type_system_symbol("$sSSN")) return swift_string_to_utf8(*reinterpret_cast<const SwiftString*>(argument.buffer));
    if (argument.type == type_system_symbol("$sSdN")) {
        snprintf(text, sizeof text, specifier.c_str(), *reinterpret_cast<const double*>(argument.buffer));
        return text;
    }
    if (specifier.back() == '@') {
        if (argument.type == type_system_symbol("$sSiN")) return std::to_string(reinterpret_cast<int64_t>(argument.buffer[0]));
        const std::string object_text = foundation_object_text(argument.buffer[0]);
        return object_text;
    }
    std::string wide = specifier;
    wide.erase(std::remove_if(wide.begin(), wide.end(), [](char c) { return c == 'l' || c == 'z' || c == 'q'; }), wide.end());
    wide.insert(wide.size() - 1, "ll");
    snprintf(text, sizeof text, wide.c_str(), reinterpret_cast<long long>(argument.buffer[0]));
    return text;
}

// printf-style formatting over [CVarArg], whose elements are existentials holding their value inline.
SWIFTCC SwiftString string_format_arguments(SwiftString format, void* arguments) {
    const std::string pattern = swift_string_to_utf8(format);
    int64_t count = 0;
    memcpy(&count, static_cast<char*>(arguments) + kArrayCountOffset, sizeof count);
    const auto* values = reinterpret_cast<const FormatArgument*>(static_cast<char*>(arguments) + kArrayHeaderBytes);
    std::string out;
    int64_t next = 0;
    for (size_t i = 0; i < pattern.size(); ++i) {
        if (pattern[i] != '%') {
            out += pattern[i];
            continue;
        }
        if (i + 1 < pattern.size() && pattern[i + 1] == '%') {
            out += '%';
            ++i;
            continue;
        }
        size_t end = i + 1;
        while (end < pattern.size() && strchr("0123456789.+-# lzq$", pattern[end]) != nullptr) ++end;
        if (end >= pattern.size()) break;
        const std::string specifier = pattern.substr(i, end - i + 1);
        if (next < count) out += format_argument_text(values[next++], specifier);
        i = end;
    }
    return swift_string_from_utf8(out);
}

// StringProtocol's Foundation API: Self and its generic arguments are only ever String here, passed by address.
SWIFTCC bool string_contains(const SwiftString* other, void*, void*, void*, void*, __attribute__((swift_context)) const SwiftString* self) {
    return swift_string_to_utf8(*self).find(swift_string_to_utf8(*other)) != std::string::npos;
}

std::string ascii_lowered(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return static_cast<char>(tolower(c)); });
    return text;
}

SWIFTCC bool string_contains_any_case(const SwiftString* other, void*, void*, void*, void*, __attribute__((swift_context)) const SwiftString* self) {
    return ascii_lowered(swift_string_to_utf8(*self)).find(ascii_lowered(swift_string_to_utf8(*other))) != std::string::npos;
}

constexpr int64_t kOrderedAscending = -1;
constexpr int64_t kOrderedDescending = 1;

SWIFTCC int64_t string_compare_any_case(const SwiftString* other, void*, void*, void*, void*, __attribute__((swift_context)) const SwiftString* self) {
    const int order = ascii_lowered(swift_string_to_utf8(*self)).compare(ascii_lowered(swift_string_to_utf8(*other)));
    return order < 0 ? kOrderedAscending : order > 0 ? kOrderedDescending
                                                     : 0;
}

SWIFTCC SwiftString string_replacing(const SwiftString* target, const SwiftString* replacement, uint64_t, uint64_t, uint64_t, uint64_t, void*, void*, __attribute__((swift_context)) const SwiftString* self) {
    std::string text = swift_string_to_utf8(*self);
    const std::string from = swift_string_to_utf8(*target);
    const std::string to = swift_string_to_utf8(*replacement);
    if (!from.empty()) {
        for (size_t at = text.find(from); at != std::string::npos; at = text.find(from, at + to.size())) text.replace(at, from.size(), to);
    }
    return swift_string_from_utf8(text);
}

// CharacterSet is one reference to this set of bytes; the framework's sets are all ASCII as far as this runtime goes.
struct CharacterSetBox {
    NativeObject header;
    std::string* members;
};

const StorageMetadata kCharacterSetMetadata = {free, nullptr, 0};

CharacterSetBox* new_character_set(std::string members) {
    auto* box = static_cast<CharacterSetBox*>(calloc(1, sizeof(CharacterSetBox)));
    box->header.isa = const_cast<uint64_t*>(&kCharacterSetMetadata.kind);
    box->header.references = 1;
    box->members = new std::string(std::move(members));
    return box;
}

constexpr const char* kAlphanumerics = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";

SWIFTCC void set_whitespace(__attribute__((swift_indirect_result)) CharacterSetBox** out) { *out = new_character_set(" \t"); }
SWIFTCC void set_newlines(__attribute__((swift_indirect_result)) CharacterSetBox** out) { *out = new_character_set("\n\r"); }
SWIFTCC void set_whitespace_newlines(__attribute__((swift_indirect_result)) CharacterSetBox** out) { *out = new_character_set(" \t\n\r"); }
SWIFTCC void set_alphanumerics(__attribute__((swift_indirect_result)) CharacterSetBox** out) { *out = new_character_set(kAlphanumerics); }
SWIFTCC void set_url_query(__attribute__((swift_indirect_result)) CharacterSetBox** out) { *out = new_character_set(std::string(kAlphanumerics) + "-._~!$&'()*+,;=:@/?"); }
SWIFTCC void set_characters_in(__attribute__((swift_indirect_result)) CharacterSetBox** out, SwiftString text) { *out = new_character_set(swift_string_to_utf8(text)); }
SWIFTCC void set_insert_characters(SwiftString text, __attribute__((swift_context)) CharacterSetBox** self) { *(*self)->members += swift_string_to_utf8(text); }
SWIFTCC void set_remove_characters(SwiftString text, __attribute__((swift_context)) CharacterSetBox** self) {
    for (const char c : swift_string_to_utf8(text)) {
        std::string& members = *(*self)->members;
        members.erase(std::remove(members.begin(), members.end(), c), members.end());
    }
}

SWIFTCC SwiftString string_trimming_set(CharacterSetBox** set, void*, void*, __attribute__((swift_context)) const SwiftString* self) {
    const std::string text = swift_string_to_utf8(*self);
    const size_t first = text.find_first_not_of(*(*set)->members);
    if (first == std::string::npos) return swift_string_from_utf8("");
    return swift_string_from_utf8(text.substr(first, text.find_last_not_of(*(*set)->members) - first + 1));
}

void* string_array(const std::vector<std::string>& parts) {
    char* elements;
    void* storage = make_array_storage(type_system_symbol("$sSSN"), parts.size(), &elements);
    for (size_t i = 0; i < parts.size(); ++i) {
        const SwiftString part = swift_string_from_utf8(parts[i]);
        memcpy(elements + i * sizeof part, &part, sizeof part);
    }
    return storage;
}

SWIFTCC void* string_components_string(const SwiftString* separator, void*, void*, void*, void*, __attribute__((swift_context)) const SwiftString* self) {
    const std::string text = swift_string_to_utf8(*self);
    const std::string by = swift_string_to_utf8(*separator);
    std::vector<std::string> parts;
    size_t start = 0;
    for (size_t at = by.empty() ? std::string::npos : text.find(by); at != std::string::npos; at = text.find(by, start)) {
        parts.push_back(text.substr(start, at - start));
        start = at + by.size();
    }
    parts.push_back(text.substr(start));
    return string_array(parts);
}

SWIFTCC void* string_components_set(CharacterSetBox** set, void*, void*, __attribute__((swift_context)) const SwiftString* self) {
    const std::string text = swift_string_to_utf8(*self);
    std::vector<std::string> parts;
    size_t start = 0;
    for (size_t at = text.find_first_of(*(*set)->members); at != std::string::npos; at = text.find_first_of(*(*set)->members, start)) {
        parts.push_back(text.substr(start, at - start));
        start = at + 1;
    }
    parts.push_back(text.substr(start));
    return string_array(parts);
}

SWIFTCC OptionalSwiftString string_adding_percent(CharacterSetBox** set, void*, void*, __attribute__((swift_context)) const SwiftString* self) {
    std::string out;
    for (const char c : swift_string_to_utf8(*self)) {
        if ((*set)->members->find(c) != std::string::npos) {
            out += c;
        } else {
            char escaped[4];
            snprintf(escaped, sizeof escaped, "%%%02X", static_cast<unsigned char>(c));
            out += escaped;
        }
    }
    const SwiftString value = swift_string_from_utf8(out);
    return {value.count_and_flags, value.object};
}

SWIFTCC OptionalSwiftString string_removing_percent(void*, void*, __attribute__((swift_context)) const SwiftString* self) {
    const std::string text = swift_string_to_utf8(*self);
    std::string out;
    for (size_t i = 0; i < text.size(); ++i) {
        if (text[i] == '%' && i + 2 < text.size() && isxdigit(static_cast<unsigned char>(text[i + 1])) && isxdigit(static_cast<unsigned char>(text[i + 2]))) {
            out += static_cast<char>(strtol(text.substr(i + 1, 2).c_str(), nullptr, 16));
            i += 2;
        } else {
            out += text[i];
        }
    }
    const SwiftString value = swift_string_from_utf8(out);
    return {value.count_and_flags, value.object};
}

SWIFTCC SwiftString string_capitalized(void*, void*, __attribute__((swift_context)) const SwiftString* self) {
    std::string text = ascii_lowered(swift_string_to_utf8(*self));
    bool start = true;
    for (char& c : text) {
        if (start) c = static_cast<char>(toupper(static_cast<unsigned char>(c)));
        start = isspace(static_cast<unsigned char>(c)) != 0;
    }
    return swift_string_from_utf8(text);
}

// String.Encoding is a raw NSStringEncoding; only UTF-8 compatible text passes through this runtime.
constexpr uint64_t kEncodingAscii = 1;
constexpr uint64_t kEncodingUtf8 = 4;
constexpr uint64_t kEncodingLatin1 = 5;

template <uint64_t Encoding>
SWIFTCC void encoding_getter(__attribute__((swift_indirect_result)) uint64_t* out) { *out = Encoding; }

SWIFTCC void encoding_from_raw(__attribute__((swift_indirect_result)) uint64_t* out, uint64_t raw) { *out = raw; }

// `Data?`'s none has this tag in its second word, where Data's own storage tags never reach.
constexpr uint64_t kOptionalDataNoneTag = 0xF000000000000000ULL;

SWIFTCC DataValue string_data_using(const uint64_t*, bool, void*, void*, __attribute__((swift_context)) const SwiftString* self) { return make_data(swift_string_to_utf8(*self)); }

SWIFTCC OptionalSwiftString string_from_data(uint64_t range, uint64_t storage, const uint64_t*) {
    if (storage == kOptionalDataNoneTag) return {0, 0};
    const SwiftString value = swift_string_from_utf8(data_bytes(range, storage));
    return {value.count_and_flags, value.object};
}

SWIFTCC SwiftString string_contents_of(URLBox** url, const uint64_t*, __attribute__((swift_context)) void*, __attribute__((swift_error_result)) void** error) {
    const std::string path = url_path(*(*url)->text);
    FILE* file = fopen(path.c_str(), "rb");
    if (file == nullptr) {
        *error = foundation_make_error("The file couldn't be opened.", 260);
        return {0, 0};
    }
    std::string text;
    char chunk[4096];
    for (size_t got; (got = fread(chunk, 1, sizeof chunk, file)) > 0;) text.append(chunk, got);
    fclose(file);
    return swift_string_from_utf8(text);
}

SWIFTCC void string_write_to(URLBox** url, bool, const uint64_t*, void*, void*, __attribute__((swift_context)) const SwiftString* self, __attribute__((swift_error_result)) void** error) {
    FILE* file = fopen(url_path(*(*url)->text).c_str(), "wb");
    if (file == nullptr) {
        *error = foundation_make_error("The file couldn't be saved.", 512);
        return;
    }
    const std::string text = swift_string_to_utf8(*self);
    fwrite(text.data(), 1, text.size(), file);
    fclose(file);
}

SWIFTCC SwiftString ns_localized_string(SwiftString key) { return swift_string_from_utf8(localization_lookup(swift_string_to_utf8(key))); }

// A task from dataTask(with:completionHandler:): the request happens once resume is called.
struct DataTask {
    NativeObject header;
    std::string* url;
    std::string* method;
    std::string* headers;
    std::string* body;
    void* completion;
    void* response;
};

void* new_data_task(std::string url, std::string method, std::string headers, std::string body, void* completion) {
    auto* task = static_cast<DataTask*>(objc_framework_object("NSURLSessionDataTask"));
    task->url = new std::string(std::move(url));
    task->method = new std::string(std::move(method));
    task->headers = new std::string(std::move(headers));
    task->body = new std::string(std::move(body));
    task->completion = darwin_block_copy(completion);
    return task;
}

void* session_task_url(void*, const char*, void* url, void* completion) { return new_data_task(nsstring_text(url), "GET", "", "", completion); }

void* task_for_request(void* request, void* completion) {
    const auto found = bridged_requests().find(request);
    if (found == bridged_requests().end()) return new_data_task(nsstring_text(request), "GET", "", "", completion);
    const URLRequestBox* box = found->second;
    return new_data_task(*box->url, *box->method, *box->headers, *box->body, completion);
}

void* session_task_request(void*, const char*, void* request, void* completion) { return task_for_request(request, completion); }

void disposition_handler(void*, int64_t) {}

// A completion block for didReceiveResponse: the transfer has already finished, so the answer changes nothing.
struct GlobalBlock {
    void* isa;
    int32_t flags, reserved;
    void* invoke;
    const void* descriptor;
};

constexpr int32_t kBlockGlobalFlag = 1 << 28;
const uintptr_t kBlockDescriptor[2] = {0, sizeof(GlobalBlock)};
GlobalBlock g_disposition_block = {nullptr, kBlockGlobalFlag, 0, reinterpret_cast<void*>(disposition_handler), kBlockDescriptor};

using MessageSend = void (*)(void*, const char*, void*, void*, void*, void*);

void run_delegate_task(DataTask* task, const AsyncDataResult& result) {
    auto send = reinterpret_cast<MessageSend>(objc_runtime_symbol("objc_msgSend"));
    void* session = framework_singleton("NSURLSession");
    void* delegate = g_url_session_delegate;
    if (result.response != nullptr) {
        send(delegate, "URLSession:dataTask:didReceiveResponse:completionHandler:", session, task, result.response, &g_disposition_block);
        send(delegate, "URLSession:dataTask:didReceiveData:", session, task, make_nsdata(data_bytes(result.data.range, result.data.storage)), nullptr);
    }
    send(delegate, "URLSession:task:didCompleteWithError:", session, task, result.error, nullptr);
}

void* session_task_request_delegate(void*, const char*, void* request) { return task_for_request(request, nullptr); }

void run_data_task(void* raw) {
    auto* task = static_cast<DataTask*>(raw);
    const AsyncDataResult result = perform_request(*task->url, *task->method, *task->headers, *task->body);
    task->response = result.response;
    if (task->completion == nullptr) {
        run_delegate_task(task, result);
        return;
    }
    struct Literal {
        void* isa;
        int32_t flags, reserved;
        void (*invoke)(void*, void*, void*, void*);
    };
    void* data = result.error == nullptr ? make_nsdata(data_bytes(result.data.range, result.data.storage)) : nullptr;
    static_cast<Literal*>(task->completion)->invoke(task->completion, data, result.response, result.error);
}

void session_ignore_word(void*, const char*, void*) {}
int64_t http_response_expected_length(void*, const char*) { return -1; }

void session_task_resume(void* self, const char*) { dispatch_enqueue(run_data_task, self); }
void session_task_cancel(void*, const char*) {}
void* session_task_response(void* self, const char*) { return static_cast<DataTask*>(self)->response; }

SWIFTCC void date_unbridge(__attribute__((swift_indirect_result)) double* out, void* object) {
    double seconds = 0;
    if (object != nullptr) foundation_object_date(object, seconds);
    *out = seconds;
}

double seconds_now_since_2001() {
    timespec now{};
    clock_gettime(CLOCK_REALTIME, &now);
    return static_cast<double>(now.tv_sec) + static_cast<double>(now.tv_nsec) / 1e9 - kSecondsFrom1970To2001;
}

constexpr double kDistantPast = -63114076800.0;

SWIFTCC void date_distant_past(__attribute__((swift_indirect_result)) double* out) { *out = kDistantPast; }
SWIFTCC double date_interval_since(const double* other, __attribute__((swift_context)) const double* self) { return *self - *other; }
SWIFTCC void date_adding(__attribute__((swift_indirect_result)) double* out, double interval, __attribute__((swift_context)) const double* self) { *out = *self + interval; }
SWIFTCC void date_plus(__attribute__((swift_indirect_result)) double* out, const double* date, double interval) { *out = *date + interval; }
SWIFTCC void date_from_interval_since(__attribute__((swift_indirect_result)) double* out, double interval, const double* date) { *out = *date + interval; }
SWIFTCC bool date_greater(const double* left, const double* right) { return *left > *right; }
SWIFTCC bool date_less(const double* left, const double* right) { return *left < *right; }
SWIFTCC bool date_equal(const double* left, const double* right) { return *left == *right; }
SWIFTCC void date_from_since_now(__attribute__((swift_indirect_result)) double* out, double interval) { *out = seconds_now_since_2001() + interval; }
SWIFTCC double date_since_now(__attribute__((swift_context)) const double* self) { return *self - seconds_now_since_2001(); }
SWIFTCC void date_from_1970(__attribute__((swift_indirect_result)) double* out, double interval) { *out = interval - kSecondsFrom1970To2001; }
SWIFTCC double date_since_1970(__attribute__((swift_context)) const double* self) { return *self + kSecondsFrom1970To2001; }

// `result` is a String? written in place.
SWIFTCC bool string_conditional_bridge(void* object, SwiftString* result, void*) {
    *result = swift_string_from_utf8(nsstring_text(object));
    return true;
}

SWIFTCC void string_force_bridge(void* object, SwiftString* result, void*) { *result = swift_string_from_utf8(nsstring_text(object)); }

void* bundle_identifier_object(void*, const char*) { return new_nsstring(bundle_info_string("CFBundleIdentifier")); }

struct ScannerObject {
    NativeObject header;
    std::string* text;
};

void* scanner_init(void* self, const char*, void* text) {
    static_cast<ScannerObject*>(self)->text = new std::string(nsstring_text(text));
    return self;
}

void* scanner_with_string(void*, const char*, void* text) { return scanner_init(objc_framework_object("NSScanner"), nullptr, text); }

bool scanner_scan_hex(void* self, const char*, uint64_t* out) {
    const std::string& text = *static_cast<ScannerObject*>(self)->text;
    char* end;
    const uint64_t value = strtoull(text.c_str(), &end, 16);
    if (end == text.c_str()) return false;
    if (out != nullptr) *out = value;
    return true;
}

// The host's HTTP stack does its own caching, so a URL cache is an object nothing is ever stored in.
void* url_cache_init(void* self, const char*, uint64_t, uint64_t, void*) { return self; }
void* url_cache_shared(void*, const char*) { return framework_singleton("NSURLCache"); }
void url_cache_ignore(void*, const char*) {}
void url_cache_set_word(void*, const char*, uint64_t) {}

// Operation queues run on the guest thread, like every Dispatch queue: operations go through its run loop.
void operation_ignore_word(void*, const char*, int64_t) {}

struct Block0 {
    void* isa;
    int32_t flags, reserved;
    void (*invoke)(void*);
};

void run_operation_block(void* block) { static_cast<Block0*>(block)->invoke(block); }
void queue_add_block(void*, const char*, void* block) { dispatch_enqueue(run_operation_block, darwin_block_copy(block)); }

void run_operation(void* operation) {
    objc_send(operation, "start", nullptr);
}

void queue_add_operation(void*, const char*, void* operation) {
    rt_swift_retain(operation);
    dispatch_enqueue(run_operation, operation);
}

void* session_delegate_queue(void*, const char*) { return framework_singleton("NSOperationQueue"); }

// A guest subclass's instance has no room for the base class's state, so cancellation lives here.
std::set<const void*>& cancelled_operations() {
    static std::set<const void*> operations;
    return operations;
}

void* operation_init(void* self, const char*) {
    cancelled_operations().erase(self);
    return self;
}
void operation_mark_cancelled(void* self, const char*) { cancelled_operations().insert(self); }
bool operation_is_cancelled(void* self, const char*) { return cancelled_operations().count(self) != 0; }

// Nothing observes key paths here: these calls only exist to notify observers.
void object_key_change_ignored(void*, const char*, void*) {}

struct BlockOperation {
    NativeObject header;
    void* block;
};

void* block_operation_with_block(void*, const char*, void* block) {
    auto* operation = static_cast<BlockOperation*>(objc_framework_object("NSBlockOperation"));
    operation->block = darwin_block_copy(block);
    return operation;
}

void block_operation_start(void* self, const char*) { run_operation_block(static_cast<BlockOperation*>(self)->block); }

// Every guest task runs on one thread, so a lock is never contended.
void lock_nothing(void*, const char*) {}

// NSCache that keeps everything: nothing here reacts to memory pressure.
struct CacheObject {
    NativeObject header;
    std::unordered_map<void*, void*>* entries;
};

std::unordered_map<void*, void*>& cache_entries(void* self) {
    auto* cache = static_cast<CacheObject*>(self);
    if (cache->entries == nullptr) cache->entries = new std::unordered_map<void*, void*>();
    return *cache->entries;
}

void* cache_object_for_key(void* self, const char*, void* key) {
    auto& entries = cache_entries(self);
    const auto found = entries.find(key);
    return found == entries.end() ? nullptr : found->second;
}

void cache_set_object(void* self, const char*, void* value, void* key) {
    rt_swift_retain(value);
    rt_swift_retain(key);
    cache_entries(self)[key] = value;
}

void cache_set_object_cost(void* self, const char* selector, void* value, void* key, uint64_t) { cache_set_object(self, selector, value, key); }
void cache_remove_object(void* self, const char*, void* key) { cache_entries(self).erase(key); }
void cache_remove_all(void* self, const char*) { cache_entries(self).clear(); }
void cache_ignore_word(void*, const char*, uint64_t) {}

void* iso_formatter_date(void*, const char*, void* text) {
    double seconds = 0;
    return text != nullptr && foundation_parse_iso8601(nsstring_text(text), seconds) ? new_date_object(seconds) : nullptr;
}

void formatter_ignore_word(void*, const char*, uint64_t) {}

// A date format pattern's fields, as strftime spells them; only numeric UTC formats reach this runtime.
std::string strftime_pattern(const std::string& format) {
    static const std::pair<const char*, const char*> kFields[] = {{"yyyy", "%Y"}, {"MMMM", "%B"}, {"MMM", "%b"}, {"MM", "%m"}, {"dd", "%d"}, {"HH", "%H"}, {"hh", "%I"}, {"mm", "%M"}, {"ss", "%S"}, {"a", "%p"}};
    std::string out;
    for (size_t i = 0; i < format.size();) {
        if (format[i] == '\'') {
            const size_t end = format.find('\'', i + 1);
            if (end == std::string::npos) break;
            out += format.substr(i + 1, end - i - 1);
            i = end + 1;
            continue;
        }
        bool matched = false;
        for (const auto& [field, spelled] : kFields) {
            if (format.compare(i, strlen(field), field) == 0) {
                out += spelled;
                i += strlen(field);
                matched = true;
                break;
            }
        }
        if (!matched) out += format[i++];
    }
    return out;
}

struct DateFormatterObject {
    NativeObject header;
    std::string* format;
};

void date_formatter_set_format(void* self, const char*, void* format) {
    auto* formatter = static_cast<DateFormatterObject*>(self);
    delete formatter->format;
    formatter->format = new std::string(format == nullptr ? std::string() : nsstring_text(format));
}

std::string date_formatter_pattern(void* self) {
    const auto* format = static_cast<DateFormatterObject*>(self)->format;
    return strftime_pattern(format == nullptr || format->empty() ? "yyyy-MM-dd HH:mm" : *format);
}

constexpr double kFormatterEpochOffset = 978307200;

void* date_formatter_date(void* self, const char*, void* text) {
    if (text == nullptr) return nullptr;
    tm parts{};
    if (strptime(nsstring_text(text).c_str(), date_formatter_pattern(self).c_str(), &parts) == nullptr) return nullptr;
    return new_date_object(static_cast<double>(timegm(&parts)) - kFormatterEpochOffset);
}

void* date_formatter_string(void* self, const char*, void* date) {
    double seconds = 0;
    if (date == nullptr || !foundation_object_date(date, seconds)) return new_nsstring("");
    const time_t clock = static_cast<time_t>(seconds + kFormatterEpochOffset);
    tm parts{};
    gmtime_r(&clock, &parts);
    char text[128];
    strftime(text, sizeof text, date_formatter_pattern(self).c_str(), &parts);
    return new_nsstring(text);
}

std::string foundation_object_text(void* object) {
    const void* isa = static_cast<NativeObject*>(object)->isa;
    return isa == g_nsstring_class || isa == g_nsurl_class ? nsstring_text(object) : std::string();
}

bool foundation_object_date(void* object, double& seconds) {
    if (static_cast<NativeObject*>(object)->isa != g_nsdate_class) return false;
    seconds = static_cast<NSDateObject*>(object)->seconds;
    return true;
}

std::string foundation_encode_object(void* object) {
    const void* isa = static_cast<NativeObject*>(object)->isa;
    if (isa == g_nsstring_class) return "s" + nsstring_text(object);
    if (isa == g_nsurl_class) return "u" + nsstring_text(object);
    if (isa == g_nsdate_class) return "d" + std::to_string(static_cast<NSDateObject*>(object)->seconds);
    return {};
}

void* foundation_decode_object(const std::string& encoded) {
    if (encoded.empty()) return nullptr;
    const std::string body = encoded.substr(1);
    if (encoded[0] == 'd') return new_date_object(strtod(body.c_str(), nullptr));
    auto* object = static_cast<NSStringObject*>(new_nsstring(body));
    if (encoded[0] == 'u') object->header.isa = g_nsurl_class;
    return object;
}

bool foundation_defaults_lookup_bool(const std::string& key, bool fallback) {
    void* name = new_nsstring(key);
    const bool known = defaults_object_for_key(nullptr, nullptr, name) != nullptr;
    return known ? defaults_bool_for_key(nullptr, nullptr, name) : fallback;
}

int64_t foundation_defaults_lookup_integer(const std::string& key, int64_t fallback) {
    void* name = new_nsstring(key);
    const bool known = defaults_object_for_key(nullptr, nullptr, name) != nullptr;
    return known ? defaults_integer_for_key(nullptr, nullptr, name) : fallback;
}

std::string foundation_defaults_lookup_string(const std::string& key, const std::string& fallback) {
    void* value = defaults_string_for_key(nullptr, nullptr, new_nsstring(key));
    return value == nullptr ? fallback : nsstring_text(value);
}

void foundation_defaults_store_bool(const std::string& key, bool value) { defaults_set_bool(nullptr, nullptr, value, new_nsstring(key)); }
void foundation_defaults_store_integer(const std::string& key, int64_t value) { defaults_set_integer(nullptr, nullptr, value, new_nsstring(key)); }
void foundation_defaults_store_string(const std::string& key, const std::string& value) { defaults_set_object(nullptr, nullptr, new_nsstring(value), new_nsstring(key)); }

void foundation_register_classes() {
    if (g_nsstring_class == nullptr) g_nsstring_class = objc_native_class("NSString", destroy_nsstring);
    for (void* constant : string_constants()) static_cast<NativeObject*>(constant)->isa = g_nsstring_class;
    const auto make_record = [](AsyncFunctionRecord& record, void (*entry)()) {
        record.context_size = kUrlSessionContextBytes;
        record.function = static_cast<int32_t>(reinterpret_cast<intptr_t>(entry) - reinterpret_cast<intptr_t>(&record));
    };
    make_record(g_url_session_url_record, rt_url_session_data_url);
    make_record(g_url_session_request_record, rt_url_session_data_request);
    g_http_response_class = objc_native_class("NSHTTPURLResponse", free);
    objc_register_method("-[NSHTTPURLResponse statusCode]", reinterpret_cast<const void*>(http_response_status));
    g_defaults_class = objc_native_class("NSUserDefaults", free);
    objc_register_method("-[WKUserScript initWithSource:injectionTime:forMainFrameOnly:]", reinterpret_cast<const void*>(user_script_init));
    objc_register_method("-[WKWebView initWithFrame:configuration:]", reinterpret_cast<const void*>(web_view_init));
    objc_register_method("-[WKWebsiteDataStore httpCookieStore]", reinterpret_cast<const void*>(cookie_store));
    g_nsarray_class = objc_native_class("NSArray", destroy_nsarray);
    g_bridged_collection_class = objc_native_class("NSDictionary", destroy_bridged_collection);
    objc_register_method("-[WKHTTPCookieStore getAllCookies:]", reinterpret_cast<const void*>(cookie_store_get_all));
    objc_register_method("-[WKHTTPCookieStore addObserver:]", reinterpret_cast<const void*>(cookie_store_add_observer));
    g_cookie_class = objc_native_class("NSHTTPCookie", free);
    objc_register_method("-[NSHTTPCookie name]", reinterpret_cast<const void*>(cookie_name));
    objc_register_method("-[NSHTTPCookie value]", reinterpret_cast<const void*>(cookie_value));
    objc_register_method("-[NSHTTPCookie domain]", reinterpret_cast<const void*>(cookie_domain));
    g_web_script_record.context_size = kUrlSessionContextBytes;
    g_web_script_record.function = static_cast<int32_t>(reinterpret_cast<intptr_t>(rt_web_view_call_script_entry) - reinterpret_cast<intptr_t>(&g_web_script_record));
    objc_register_method("-[WKWebView configuration]", reinterpret_cast<const void*>(web_view_configuration));
    objc_register_method("-[WKWebView layer]", reinterpret_cast<const void*>(web_view_layer));
    objc_register_method("-[CALayer setCornerRadius:]", reinterpret_cast<const void*>(ignore_message_with_double));
    objc_register_method("-[CALayer setMasksToBounds:]", reinterpret_cast<const void*>(ignore_message_with_word));
    objc_register_method("-[WKWebView loadRequest:]", reinterpret_cast<const void*>(web_view_load));
    objc_register_method("-[WKWebView URL]", reinterpret_cast<const void*>(web_view_url));
    objc_register_method("-[WKWebView setNavigationDelegate:]", reinterpret_cast<const void*>(ignore_message_with_word));
    objc_register_method("-[WKWebView setInspectable:]", reinterpret_cast<const void*>(ignore_message_with_word));
    objc_register_method("-[WKWebViewConfiguration userContentController]", reinterpret_cast<const void*>(configuration_controller));
    objc_register_method("-[WKWebViewConfiguration websiteDataStore]", reinterpret_cast<const void*>(web_data_store_default));
    objc_register_method("-[WKWebViewConfiguration setWebsiteDataStore:]", reinterpret_cast<const void*>(ignore_message_with_word));
    objc_register_method("-[WKWebViewConfiguration setApplicationNameForUserAgent:]", reinterpret_cast<const void*>(configuration_set_application_name));
    objc_register_method("-[WKUserContentController addUserScript:]", reinterpret_cast<const void*>(controller_add_script));
    objc_register_method("+[WKWebsiteDataStore defaultDataStore]", reinterpret_cast<const void*>(web_data_store_default));
    objc_register_method("+[WKContentWorld pageWorld]", reinterpret_cast<const void*>(content_world_page));
    g_nsdata_class = objc_native_class("NSData", destroy_nsdata);
    g_nsdate_class = objc_native_class("NSDate", free);
    objc_register_method("-[NSUserDefaults dictionaryRepresentation]", reinterpret_cast<const void*>(defaults_dictionary_representation));
    g_json_value_class = objc_native_class("__NSJSONValue", destroy_json_value);
    objc_register_method("+[NSRegularExpression escapedPatternForString:]", reinterpret_cast<const void*>(regular_expression_escaped_pattern));
    objc_register_method("+[NSJSONSerialization JSONObjectWithData:options:error:]", reinterpret_cast<const void*>(json_object_with_data));
    objc_register_method("+[NSJSONSerialization dataWithJSONObject:options:error:]", reinterpret_cast<const void*>(json_data_with_object));
    objc_register_method("+[NSURLSessionConfiguration defaultSessionConfiguration]", reinterpret_cast<const void*>(url_session_configuration_default));
    objc_register_method("-[NSURLSessionConfiguration setTimeoutIntervalForRequest:]", reinterpret_cast<const void*>(ignore_message_with_double));
    objc_register_method("-[NSURLSessionConfiguration setHTTPAdditionalHeaders:]", reinterpret_cast<const void*>(session_set_headers));
    objc_register_method("+[NSURLSession sessionWithConfiguration:]", reinterpret_cast<const void*>(url_session_with_configuration));
    objc_register_method("-[NSObject init]", reinterpret_cast<const void*>(return_self));
    objc_register_method("-[NSObject self]", reinterpret_cast<const void*>(return_self));
    objc_register_method("-[NSObject retain]", reinterpret_cast<const void*>(return_self));
    objc_register_method("-[NSObject autorelease]", reinterpret_cast<const void*>(return_self));
    objc_register_method("-[NSUserDefaults initWithSuiteName:]", reinterpret_cast<const void*>(return_self_with_argument));
    objc_register_method("+[NSProcessInfo processInfo]", reinterpret_cast<const void*>(process_info_shared));
    objc_register_method("-[NSProcessInfo physicalMemory]", reinterpret_cast<const void*>(+[](void*, const char*) -> uint64_t { return static_cast<uint64_t>(sysconf(_SC_PHYS_PAGES)) * static_cast<uint64_t>(sysconf(_SC_PAGESIZE)); }));
    objc_register_method("-[NSProcessInfo operatingSystemVersion]", reinterpret_cast<const void*>(process_info_version));
    objc_register_method("-[NSBundle bundleURL]", reinterpret_cast<const void*>(bundle_url));
    objc_register_method("-[NSBundle resourceURL]", reinterpret_cast<const void*>(bundle_url));
    objc_register_method("-[NSBundle bundlePath]", reinterpret_cast<const void*>(bundle_path));
    objc_register_method("-[NSBundle URLForResource:withExtension:]", reinterpret_cast<const void*>(bundle_url_for_resource));
    objc_register_method("-[NSFileManager URLsForDirectory:inDomains:]", reinterpret_cast<const void*>(file_manager_urls));
    objc_register_method("-[NSFileManager temporaryDirectory]", reinterpret_cast<const void*>(file_manager_temporary_directory));
    objc_register_method("-[NSFileManager fileExistsAtPath:isDirectory:]", reinterpret_cast<const void*>(file_manager_exists_directory));
    objc_register_method("-[NSFileManager createDirectoryAtPath:withIntermediateDirectories:attributes:error:]", reinterpret_cast<const void*>(file_manager_create_path));
    objc_register_method("-[NSFileManager removeItemAtPath:error:]", reinterpret_cast<const void*>(file_manager_remove));
    objc_register_method("-[NSFileManager removeItemAtURL:error:]", reinterpret_cast<const void*>(file_manager_remove));
    objc_register_method("-[NSFileManager moveItemAtURL:toURL:error:]", reinterpret_cast<const void*>(file_manager_move));
    objc_register_method("-[NSFileManager copyItemAtURL:toURL:error:]", reinterpret_cast<const void*>(file_manager_copy));
    objc_register_method("-[NSFileManager contentsOfDirectoryAtPath:error:]", reinterpret_cast<const void*>(file_manager_contents_path));
    objc_register_method("-[NSFileManager contentsOfDirectoryAtURL:includingPropertiesForKeys:options:error:]", reinterpret_cast<const void*>(file_manager_contents_url));
    objc_register_method("-[NSURLSession setSessionDescription:]", reinterpret_cast<const void*>(session_ignore_word));
    objc_register_method("-[NSURLSession dataTaskWithRequest:]", reinterpret_cast<const void*>(session_task_request_delegate));
    objc_register_method("-[NSHTTPURLResponse expectedContentLength]", reinterpret_cast<const void*>(http_response_expected_length));
    objc_register_method("-[NSURLSession dataTaskWithURL:completionHandler:]", reinterpret_cast<const void*>(session_task_url));
    objc_register_method("-[NSURLSession dataTaskWithRequest:completionHandler:]", reinterpret_cast<const void*>(session_task_request));
    objc_register_method("-[NSURLSessionDataTask resume]", reinterpret_cast<const void*>(session_task_resume));
    objc_register_method("-[NSURLSessionDataTask cancel]", reinterpret_cast<const void*>(session_task_cancel));
    objc_register_method("-[NSURLSessionDataTask response]", reinterpret_cast<const void*>(session_task_response));
    objc_register_method("-[NSURLSession delegateQueue]", reinterpret_cast<const void*>(session_delegate_queue));
    objc_register_method("-[NSURLSessionDataTask setPrefersIncrementalDelivery:]", reinterpret_cast<const void*>(url_cache_set_word));
    objc_register_method("-[NSURLSessionDataTask setPriority:]", reinterpret_cast<const void*>(url_cache_set_word));
    objc_register_method("-[NSISO8601DateFormatter setFormatOptions:]", reinterpret_cast<const void*>(formatter_ignore_word));
    objc_register_method("-[NSISO8601DateFormatter dateFromString:]", reinterpret_cast<const void*>(iso_formatter_date));
    objc_register_method("-[NSDateFormatter setDateFormat:]", reinterpret_cast<const void*>(date_formatter_set_format));
    objc_register_method("-[NSDateFormatter setLocale:]", reinterpret_cast<const void*>(formatter_ignore_word));
    objc_register_method("-[NSDateFormatter setTimeZone:]", reinterpret_cast<const void*>(formatter_ignore_word));
    objc_register_method("-[NSDateFormatter setCalendar:]", reinterpret_cast<const void*>(formatter_ignore_word));
    objc_register_method("-[NSDateFormatter setDateStyle:]", reinterpret_cast<const void*>(formatter_ignore_word));
    objc_register_method("-[NSDateFormatter setTimeStyle:]", reinterpret_cast<const void*>(formatter_ignore_word));
    objc_register_method("-[NSDateFormatter dateFromString:]", reinterpret_cast<const void*>(date_formatter_date));
    objc_register_method("-[NSDateFormatter stringFromDate:]", reinterpret_cast<const void*>(date_formatter_string));
    objc_register_method("-[NSBundle bundleIdentifier]", reinterpret_cast<const void*>(bundle_identifier_object));
    objc_register_method("-[NSScanner initWithString:]", reinterpret_cast<const void*>(scanner_init));
    objc_register_method("+[NSScanner scannerWithString:]", reinterpret_cast<const void*>(scanner_with_string));
    objc_register_method("-[NSScanner scanHexLongLong:]", reinterpret_cast<const void*>(scanner_scan_hex));
    objc_register_method("-[NSURLCache initWithMemoryCapacity:diskCapacity:diskPath:]", reinterpret_cast<const void*>(url_cache_init));
    objc_register_method("+[NSURLCache sharedURLCache]", reinterpret_cast<const void*>(url_cache_shared));
    objc_register_method("-[NSURLCache removeAllCachedResponses]", reinterpret_cast<const void*>(url_cache_ignore));
    objc_register_method("-[NSURLCache setDiskCapacity:]", reinterpret_cast<const void*>(url_cache_set_word));
    objc_register_method("-[NSURLCache setMemoryCapacity:]", reinterpret_cast<const void*>(url_cache_set_word));
    objc_register_method("-[NSURLSessionConfiguration setURLCache:]", reinterpret_cast<const void*>(url_cache_set_word));
    objc_register_method("-[NSOperationQueue setMaxConcurrentOperationCount:]", reinterpret_cast<const void*>(operation_ignore_word));
    objc_register_method("-[NSOperationQueue setQualityOfService:]", reinterpret_cast<const void*>(operation_ignore_word));
    objc_register_method("-[NSOperationQueue setUnderlyingQueue:]", reinterpret_cast<const void*>(operation_ignore_word));
    objc_register_method("-[NSOperationQueue setName:]", reinterpret_cast<const void*>(operation_ignore_word));
    objc_register_method("-[NSOperationQueue addOperationWithBlock:]", reinterpret_cast<const void*>(queue_add_block));
    objc_register_method("-[NSOperationQueue addOperation:]", reinterpret_cast<const void*>(queue_add_operation));
    objc_register_method("+[NSBlockOperation blockOperationWithBlock:]", reinterpret_cast<const void*>(block_operation_with_block));
    objc_register_method("-[NSBlockOperation start]", reinterpret_cast<const void*>(block_operation_start));
    objc_register_method("-[NSOperation setQueuePriority:]", reinterpret_cast<const void*>(operation_ignore_word));
    objc_register_method("-[NSOperation init]", reinterpret_cast<const void*>(operation_init));
    objc_register_method("-[NSOperation cancel]", reinterpret_cast<const void*>(operation_mark_cancelled));
    objc_register_method("-[NSOperation isCancelled]", reinterpret_cast<const void*>(operation_is_cancelled));
    objc_register_method("-[NSOperationQueue setSuspended:]", reinterpret_cast<const void*>(operation_ignore_word));
    objc_register_method("-[NSObject willChangeValueForKey:]", reinterpret_cast<const void*>(object_key_change_ignored));
    objc_register_method("-[NSObject didChangeValueForKey:]", reinterpret_cast<const void*>(object_key_change_ignored));
    objc_register_method("-[NSOperation setQualityOfService:]", reinterpret_cast<const void*>(operation_ignore_word));
    objc_register_method("-[NSLock lock]", reinterpret_cast<const void*>(lock_nothing));
    objc_register_method("-[NSLock unlock]", reinterpret_cast<const void*>(lock_nothing));
    objc_register_method("-[NSRecursiveLock lock]", reinterpret_cast<const void*>(lock_nothing));
    objc_register_method("-[NSRecursiveLock unlock]", reinterpret_cast<const void*>(lock_nothing));
    objc_register_method("-[NSCondition lock]", reinterpret_cast<const void*>(lock_nothing));
    objc_register_method("-[NSCondition unlock]", reinterpret_cast<const void*>(lock_nothing));
    objc_register_method("-[NSCache objectForKey:]", reinterpret_cast<const void*>(cache_object_for_key));
    objc_register_method("-[NSCache setObject:forKey:]", reinterpret_cast<const void*>(cache_set_object));
    objc_register_method("-[NSCache setObject:forKey:cost:]", reinterpret_cast<const void*>(cache_set_object_cost));
    objc_register_method("-[NSCache removeObjectForKey:]", reinterpret_cast<const void*>(cache_remove_object));
    objc_register_method("-[NSCache removeAllObjects]", reinterpret_cast<const void*>(cache_remove_all));
    objc_register_method("-[NSCache setTotalCostLimit:]", reinterpret_cast<const void*>(cache_ignore_word));
    objc_register_method("-[NSCache setCountLimit:]", reinterpret_cast<const void*>(cache_ignore_word));
    objc_register_method("+[NSURLSession sharedSession]", reinterpret_cast<const void*>(url_session_shared));
    objc_register_method("+[UIDevice currentDevice]", reinterpret_cast<const void*>(device_current));
    objc_register_method("-[UIDevice systemName]", reinterpret_cast<const void*>(device_system_name));
    objc_register_method("-[UIDevice model]", reinterpret_cast<const void*>(device_model));
    objc_register_method("-[UIDevice userInterfaceIdiom]", reinterpret_cast<const void*>(device_idiom_phone));
    objc_register_method("-[UIApplication keyWindow]", reinterpret_cast<const void*>(no_object));
    objc_register_method("+[NSNotificationCenter defaultCenter]", reinterpret_cast<const void*>(notification_center_default));
    objc_register_method("-[NSNotificationCenter addObserverForName:object:queue:usingBlock:]", reinterpret_cast<const void*>(notification_add_block));
    objc_register_method("-[NSNotificationCenter removeObserver:]", reinterpret_cast<const void*>(notification_remove));
    objc_register_method("-[NSNotificationCenter postNotificationName:object:]", reinterpret_cast<const void*>(notification_post_name));
    objc_register_method("+[NSURLSession sessionWithConfiguration:delegate:delegateQueue:]", reinterpret_cast<const void*>(url_session_with_delegate));
    objc_register_method("+[NSBundle mainBundle]", reinterpret_cast<const void*>(main_bundle));
    objc_register_method("+[NSBundle bundleForClass:]", reinterpret_cast<const void*>(bundle_for_class));
    objc_register_method("-[NSBundle objectForInfoDictionaryKey:]", reinterpret_cast<const void*>(bundle_info_value_string));
    g_nsurl_class = objc_native_class("NSURL", destroy_nsstring);
    g_nsnumber_class = objc_native_class("NSNumber", free);
    objc_register_method("-[NSFileManager createDirectoryAtURL:withIntermediateDirectories:attributes:error:]", reinterpret_cast<const void*>(file_manager_create_directory));
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
    // UIKit and Foundation name their notifications with NSString constants holding the same text.
    constexpr std::string_view kNotificationSuffix = "Notification";
    if ((name.rfind("UI", 0) == 0 || name.rfind("NS", 0) == 0) && name.size() > kNotificationSuffix.size() && name.substr(name.size() - kNotificationSuffix.size()) == kNotificationSuffix) {
        static std::unordered_map<std::string, void*> constants;
        void*& slot = constants[std::string(name)];
        if (slot == nullptr) {
            slot = new_nsstring(std::string(name));
            string_constants().push_back(slot);
        }
        return &slot;
    }
    static const std::unordered_map<std::string_view, void*> table = {
        {"$sSS10FoundationE6format9argumentsS2Sh_Says7CVarArg_pGhtcfC", reinterpret_cast<void*>(string_format_arguments)},
        {"$sSS10FoundationE6format_S2Sh_s7CVarArg_pdtcfC", reinterpret_cast<void*>(string_format_arguments)},
        {"$s10Foundation3URLV15fileURLWithPathACSSh_tcfC", reinterpret_cast<void*>(url_file_with_path)},
        {"$s10Foundation3URLV18documentsDirectoryACvgZ", reinterpret_cast<void*>(url_documents)},
        {"$s10Foundation3URLV22appendingPathComponentyACSSF", reinterpret_cast<void*>(url_appending_component)},
        {"$s10Foundation3URLV22appendingPathComponent_11isDirectoryACSS_SbtF", reinterpret_cast<void*>(url_appending_directory)},
        {"$s10Foundation3URLV22appendingPathExtensionyACSSF", reinterpret_cast<void*>(url_appending_extension)},
        {"$s10Foundation3URLV21deletingPathExtensionACyF", reinterpret_cast<void*>(url_deleting_extension)},
        {"$s10Foundation3URLV17lastPathComponentSSvg", reinterpret_cast<void*>(url_last_component)},
        {"$s10Foundation3URLV13pathExtensionSSvg", reinterpret_cast<void*>(url_path_extension)},
        {"$s10Foundation3URLV12relativePathSSvg", reinterpret_cast<void*>(url_path_getter)},
        {"$s10Foundation3URLV4path14percentEncodedSSSb_tF", reinterpret_cast<void*>(url_path_percent)},
        {"$s10Foundation3URLV06isFileB0Sbvg", reinterpret_cast<void*>(url_is_file)},
        {"$s10Foundation3URLV16hasDirectoryPathSbvg", reinterpret_cast<void*>(url_has_directory_path)},
        {"$s10Foundation3URLV2eeoiySbAC_ACtFZ", reinterpret_cast<void*>(url_equal)},
        {"$s10Foundation3URLV36startAccessingSecurityScopedResourceSbyF", reinterpret_cast<void*>(url_start_security_scope)},
        {"$s10Foundation3URLV08absoluteB0ACvg", reinterpret_cast<void*>(url_absolute)},
        {"$s10Foundation3URLV04baseB0ACSgvg", reinterpret_cast<void*>(url_no_base)},
        {"$s10Foundation3URLV4hostSSSgvg", reinterpret_cast<void*>(url_host)},
        {"$s10Foundation3URLV5querySSSgvg", reinterpret_cast<void*>(url_query)},
        {"$s10Foundation3URLV4portSiSgvg", reinterpret_cast<void*>(url_port)},
        {"$s10Foundation3URLV6string10relativeToACSgSSh_AFhtcfC", reinterpret_cast<void*>(url_string_relative)},
        {"$s10Foundation4DateV36_unconditionallyBridgeFromObjectiveCyACSo6NSDateCSgFZ", reinterpret_cast<void*>(date_unbridge)},
        {"$s10Foundation4DateV11distantPastACvgZ", reinterpret_cast<void*>(date_distant_past)},
        {"$s10Foundation4DateV17timeIntervalSinceySdACF", reinterpret_cast<void*>(date_interval_since)},
        {"$s10Foundation4DateV18addingTimeIntervalyACSdF", reinterpret_cast<void*>(date_adding)},
        {"$s10Foundation4DateV1poiyA2C_SdtFZ", reinterpret_cast<void*>(date_plus)},
        {"$s10Foundation4DateV12timeInterval5sinceACSd_ACtcfC", reinterpret_cast<void*>(date_from_interval_since)},
        {"$s10Foundation4DateV1goiySbAC_ACtFZ", reinterpret_cast<void*>(date_greater)},
        {"$s10Foundation4DateV1loiySbAC_ACtFZ", reinterpret_cast<void*>(date_less)},
        {"$s10Foundation4DateV2eeoiySbAC_ACtFZ", reinterpret_cast<void*>(date_equal)},
        {"$s10Foundation4DateV20timeIntervalSinceNowACSd_tcfC", reinterpret_cast<void*>(date_from_since_now)},
        {"$s10Foundation4DateV20timeIntervalSinceNowSdvg", reinterpret_cast<void*>(date_since_now)},
        {"$s10Foundation4DateV21timeIntervalSince1970ACSd_tcfC", reinterpret_cast<void*>(date_from_1970)},
        {"$s10Foundation4DateV21timeIntervalSince1970Sdvg", reinterpret_cast<void*>(date_since_1970)},
        {"$s10Foundation4DateV3nowACvgZ", reinterpret_cast<void*>(date_now)},
        {"$sSS10FoundationE34_conditionallyBridgeFromObjectiveC_6resultSbSo8NSStringC_SSSgztFZ", reinterpret_cast<void*>(string_conditional_bridge)},
        {"$sSS10FoundationE26_forceBridgeFromObjectiveC_6resultySo8NSStringC_SSSgztFZ", reinterpret_cast<void*>(string_force_bridge)},
        {"$sSy10FoundationE8containsySbqd__SyRd__lF", reinterpret_cast<void*>(string_contains)},
        {"$sSy10FoundationE32localizedCaseInsensitiveContainsySbqd__SyRd__lF", reinterpret_cast<void*>(string_contains_any_case)},
        {"$sSy10FoundationE31localizedCaseInsensitiveCompareySo18NSComparisonResultVqd__SyRd__lF", reinterpret_cast<void*>(string_compare_any_case)},
        {"$sSy10FoundationE22caseInsensitiveCompareySo18NSComparisonResultVqd__SyRd__lF", reinterpret_cast<void*>(string_compare_any_case)},
        {"$sSy10FoundationE20replacingOccurrences2of4with7options5rangeSSqd___qd_0_So22NSStringCompareOptionsVSnySS5IndexVGSgtSyRd__SyRd_0_r0_lF", reinterpret_cast<void*>(string_replacing)},
        {"$sSy10FoundationE18trimmingCharacters2inSSAA12CharacterSetV_tF", reinterpret_cast<void*>(string_trimming_set)},
        {"$sSy10FoundationE10components11separatedBySaySSGqd___tSyRd__lF", reinterpret_cast<void*>(string_components_string)},
        {"$sSy10FoundationE10components11separatedBySaySSGAA12CharacterSetV_tF", reinterpret_cast<void*>(string_components_set)},
        {"$sSy10FoundationE21addingPercentEncoding21withAllowedCharactersSSSgAA12CharacterSetV_tF", reinterpret_cast<void*>(string_adding_percent)},
        {"$sSy10FoundationE23removingPercentEncodingSSSgvg", reinterpret_cast<void*>(string_removing_percent)},
        {"$sSy10FoundationE11capitalizedSSvg", reinterpret_cast<void*>(string_capitalized)},
        {"$sSy10FoundationE4data5using20allowLossyConversionAA4DataVSgSSAAE8EncodingV_SbtF", reinterpret_cast<void*>(string_data_using)},
        {"$sSS10FoundationE4data8encodingSSSgAA4DataVh_SSAAE8EncodingVtcfC", reinterpret_cast<void*>(string_from_data)},
        {"$sSS10FoundationE10contentsOf8encodingSSAA3URLVh_SSAAE8EncodingVtKcfC", reinterpret_cast<void*>(string_contents_of)},
        {"$sSy10FoundationE5write2to10atomically8encodingyAA3URLV_SbSSAAE8EncodingVtKF", reinterpret_cast<void*>(string_write_to)},
        {"$sSS10FoundationE8EncodingV4utf8ACvgZ", reinterpret_cast<void*>(encoding_getter<kEncodingUtf8>)},
        {"$sSS10FoundationE8EncodingV5asciiACvgZ", reinterpret_cast<void*>(encoding_getter<kEncodingAscii>)},
        {"$sSS10FoundationE8EncodingV9isoLatin1ACvgZ", reinterpret_cast<void*>(encoding_getter<kEncodingLatin1>)},
        {"$sSS10FoundationE8EncodingV8rawValueACSu_tcfC", reinterpret_cast<void*>(encoding_from_raw)},
        {"$s10Foundation12CharacterSetV11whitespacesACvgZ", reinterpret_cast<void*>(set_whitespace)},
        {"$s10Foundation12CharacterSetV8newlinesACvgZ", reinterpret_cast<void*>(set_newlines)},
        {"$s10Foundation12CharacterSetV22whitespacesAndNewlinesACvgZ", reinterpret_cast<void*>(set_whitespace_newlines)},
        {"$s10Foundation12CharacterSetV13alphanumericsACvgZ", reinterpret_cast<void*>(set_alphanumerics)},
        {"$s10Foundation12CharacterSetV15urlQueryAllowedACvgZ", reinterpret_cast<void*>(set_url_query)},
        {"$s10Foundation12CharacterSetV12charactersInACSSh_tcfC", reinterpret_cast<void*>(set_characters_in)},
        {"$s10Foundation12CharacterSetV6insert12charactersInySS_tF", reinterpret_cast<void*>(set_insert_characters)},
        {"$s10Foundation12CharacterSetV6remove12charactersInySS_tF", reinterpret_cast<void*>(set_remove_characters)},
        {"$s10Foundation17NSLocalizedString_9tableName6bundle5value7commentS2S_SSSgSo8NSBundleCS2StF", reinterpret_cast<void*>(ns_localized_string)},
        {"$s10Foundation3URLV27applicationSupportDirectoryACvgZ", reinterpret_cast<void*>(url_application_support)},
        {"$s10Foundation3URLV6stringACSgSSh_tcfC", reinterpret_cast<void*>(url_from_string)},
        {"$s10Foundation3URLV14absoluteStringSSvg", reinterpret_cast<void*>(url_absolute_string)},
        {"$s10Foundation3URLV4pathSSvg", reinterpret_cast<void*>(url_path_getter)},
        {"$s10Foundation3URLV6schemeSSSgvg", reinterpret_cast<void*>(url_scheme)},
        {"$s10Foundation3URLV9appending4path13directoryHintACx_AC09DirectoryF0OtSyRzlF", reinterpret_cast<void*>(url_appending_path)},
        {"$s10Foundation3URLV25deletingLastPathComponentACyF", reinterpret_cast<void*>(url_deleting_last_component)},
        {"$s10Foundation3URLV19_bridgeToObjectiveCSo5NSURLCyF", reinterpret_cast<void*>(url_bridge_to_objc)},
        {"$s10Foundation3URLV36_unconditionallyBridgeFromObjectiveCyACSo5NSURLCSgFZ", reinterpret_cast<void*>(url_bridge_from_objc)},
        {"$s10Foundation3URLV13DirectoryHintO13inferFromPathyA2EmFWC", const_cast<int32_t*>(&kDirectoryHintInferFromPath)},
        {"$sSS10FoundationE17LocalizationValueV13stringLiteralACSS_tcfC", reinterpret_cast<void*>(localization_value_literal)},
        {"$sSS10FoundationE17LocalizationValueV19StringInterpolationV15literalCapacity18interpolationCountAESi_SitcfC", reinterpret_cast<void*>(localization_interpolation_init)},
        {"$sSS10FoundationE17LocalizationValueV19StringInterpolationV13appendLiteralyySSF", reinterpret_cast<void*>(localization_append_literal)},
        {"$sSS10FoundationE17LocalizationValueV19StringInterpolationV06appendE0yySSF", reinterpret_cast<void*>(localization_append_string)},
        {"$sSS10FoundationE17LocalizationValueV19StringInterpolationV06appendE0_9specifieryx_SStAA18_FormatSpecifiableRzlF", reinterpret_cast<void*>(localization_append_integer)},
        {"$sSS10FoundationE17LocalizationValueV19stringInterpolationA2C06StringE0V_tcfC", reinterpret_cast<void*>(localization_value_interpolation)},
        {"$s10Foundation23LocalizedStringResourceV_5table6locale6bundle7commentACSSAAE17LocalizationValueV_SSSgAA6LocaleVAC17BundleDescriptionOs06StaticC0VSgtcfC", reinterpret_cast<void*>(localized_resource_init)},
        {"$s10Foundation23LocalizedStringResourceV_12defaultValue5table6locale6bundle7commentACs06StaticC0V_SSAAE012LocalizationF0VSSSgAA6LocaleVAC17BundleDescriptionOAJSgtcfC", reinterpret_cast<void*>(localized_resource_init_default)},
        {"$sSS10FoundationE9localizedSSAA23LocalizedStringResourceV_tcfC", reinterpret_cast<void*>(string_localized)},
        {"$s10Foundation6LocaleV7currentACvgZ", reinterpret_cast<void*>(locale_current)},
        {"$s10Foundation6LocaleV19autoupdatingCurrentACvgZ", reinterpret_cast<void*>(locale_current)},
        {"$s10Foundation23LocalizedStringResourceV17BundleDescriptionO8forClassyAEyXlXpcAEmFWC", const_cast<int32_t*>(&kBundleDescriptionForClass)},
        {"$sSa10FoundationE36_unconditionallyBridgeFromObjectiveCySayxGSo7NSArrayCSgFZ", reinterpret_cast<void*>(array_bridge_from_objc)},
        {"$s10Foundation4DataV10contentsOf7optionsAcA3URLVh_So20NSDataReadingOptionsVtKcfC", reinterpret_cast<void*>(data_contents_of)},
        {"$s10Foundation4DataV5write2to7optionsyAA3URLV_So20NSDataWritingOptionsVtKF", reinterpret_cast<void*>(data_write)},
        {"$s10Foundation12URLQueryItemV4name5valueACSSh_SSSghtcfC", reinterpret_cast<void*>(query_item_init)},
        {"$s10Foundation12URLQueryItemV4nameSSvg", reinterpret_cast<void*>(query_item_name)},
        {"$s10Foundation12URLQueryItemV5valueSSSgvg", reinterpret_cast<void*>(query_item_value)},
        {"$s10Foundation13URLComponentsV3url23resolvingAgainstBaseURLACSgAA0G0Vh_SbtcfC", reinterpret_cast<void*>(components_from_url)},
        {"$s10Foundation13URLComponentsV6stringACSgSSh_tcfC", reinterpret_cast<void*>(components_from_string)},
        {"$s10Foundation13URLComponentsV3urlAA3URLVSgvg", reinterpret_cast<void*>(components_url)},
        {"$s10Foundation13URLComponentsV6schemeSSSgvg", reinterpret_cast<void*>(components_scheme)},
        {"$s10Foundation13URLComponentsV4hostSSSgvg", reinterpret_cast<void*>(components_host)},
        {"$s10Foundation13URLComponentsV4pathSSvg", reinterpret_cast<void*>(components_path)},
        {"$s10Foundation13URLComponentsV10queryItemsSayAA12URLQueryItemVGSgvg", reinterpret_cast<void*>(components_query_items)},
        {"$s10Foundation13URLComponentsV10queryItemsSayAA12URLQueryItemVGSgvs", reinterpret_cast<void*>(components_set_query_items)},
        {"$s10Foundation4UUIDVACycfC", reinterpret_cast<void*>(uuid_init)},
        {"$s10Foundation4UUIDV10uuidStringSSvg", reinterpret_cast<void*>(uuid_string)},
        {"swift_stdlib_random", reinterpret_cast<void*>(stdlib_random)},
        {"$s10Foundation4DateV19_bridgeToObjectiveCSo6NSDateCyF", reinterpret_cast<void*>(date_bridge_to_objc)},
        {"$sSD10FoundationE36_unconditionallyBridgeFromObjectiveCySDyxq_GSo12NSDictionaryCSgFZ", reinterpret_cast<void*>(dictionary_bridge_from_objc)},
        {"$s10Foundation13__DataStorageCMa", reinterpret_cast<void*>(data_storage_access)},
        {"$s10Foundation13__DataStorageC5bytes6lengthACSVSg_Sitcfc", reinterpret_cast<void*>(data_storage_init_bytes)},
        {"$s10Foundation13__DataStorageC6lengthACSi_tcfc", reinterpret_cast<void*>(data_storage_init_length)},
        {"$s10Foundation13__DataStorageC5bytes6length4copy11deallocator6offsetACSvSg_SiSbySv_SitcSgSitcfc", reinterpret_cast<void*>(data_storage_init_full)},
        {"$s10Foundation4DataV15_RepresentationO6append10contentsOfySW_tF", reinterpret_cast<void*>(data_append)},
        {"$s10Foundation4DataV15_RepresentationO15replaceSubrange_4with5countySnySiG_SVSgSitF", reinterpret_cast<void*>(data_replace_subrange)},
        {"$s10Foundation4DataV19_bridgeToObjectiveCSo6NSDataCyF", reinterpret_cast<void*>(data_bridge_to_objc)},
        {"$s10Foundation4DataV36_unconditionallyBridgeFromObjectiveCyACSo6NSDataCSgFZ", reinterpret_cast<void*>(data_bridge_from_objc)},
        {"$s10Foundation22_convertNSErrorToErrorys0E0_pSo0C0CSgF", reinterpret_cast<void*>(error_identity)},
        {"$s10Foundation22_convertErrorToNSErrorySo0E0Cs0C0_pF", reinterpret_cast<void*>(error_identity)},
        {"$s10Foundation13__DataStorageC6_bytesSvSgvg", reinterpret_cast<void*>(data_storage_bytes)},
        {"$s10Foundation13__DataStorageC7_lengthSivg", reinterpret_cast<void*>(data_storage_length)},
        {"$s10Foundation13__DataStorageC7_offsetSivg", reinterpret_cast<void*>(data_storage_offset)},
        {"$ss5ErrorP10FoundationE20localizedDescriptionSSvg", reinterpret_cast<void*>(error_localized_description)},
        {"$sSo12NSURLSessionC10FoundationE4data3for8delegateAC4DataV_So13NSURLResponseCtAC10URLRequestV_So0A12TaskDelegate_pSgtYaKF", reinterpret_cast<void*>(rt_url_session_data_request)},
        {"$sSo12NSURLSessionC10FoundationE4data4from8delegateAC4DataV_So13NSURLResponseCtAC3URLV_So0A12TaskDelegate_pSgtYaKF", reinterpret_cast<void*>(rt_url_session_data_url)},
        {"$sSo12NSURLSessionC10FoundationE4data3for8delegateAC4DataV_So13NSURLResponseCtAC10URLRequestV_So0A12TaskDelegate_pSgtYaKFTu", &g_url_session_request_record},
        {"$sSo12NSURLSessionC10FoundationE4data4from8delegateAC4DataV_So13NSURLResponseCtAC3URLV_So0A12TaskDelegate_pSgtYaKFTu", &g_url_session_url_record},
        {"$s10Foundation10URLRequestV3url11cachePolicy15timeoutIntervalAcA3URLV_So017NSURLRequestCacheE0VSdtcfC", reinterpret_cast<void*>(url_request_init)},
        {"$s10Foundation10URLRequestV10httpMethodSSSgvs", reinterpret_cast<void*>(url_request_set_method)},
        {"$s10Foundation10URLRequestV8setValue_18forHTTPHeaderFieldySSSg_SStF", reinterpret_cast<void*>(url_request_set_header)},
        {"$s10Foundation10URLRequestV8httpBodyAA4DataVSgvs", reinterpret_cast<void*>(url_request_set_body)},
        {"$s10Foundation10URLRequestV3urlAA3URLVSgvg", reinterpret_cast<void*>(url_request_get_url)},
        {"$sSD10FoundationE19_bridgeToObjectiveCSo12NSDictionaryCyF", reinterpret_cast<void*>(collection_bridge_to_objc)},
        {"$sSa10FoundationE19_bridgeToObjectiveCSo7NSArrayCyF", reinterpret_cast<void*>(collection_bridge_to_objc)},
        {"swift_allocError", reinterpret_cast<void*>(swift_alloc_error)},
        {"swift_getErrorValue", reinterpret_cast<void*>(swift_get_error_value)},
        {"$s10Foundation18IntegerFormatStyleV6localeACyxGAA6LocaleV_tcfC", reinterpret_cast<void*>(integer_style_init)},
        {"$s10Foundation30NumberFormatStyleConfigurationO8NotationV11compactNameAEvgZ", reinterpret_cast<void*>(notation_compact_name)},
        {"$s10Foundation18IntegerFormatStyleV8notationyACyxGAA06NumbercD13ConfigurationO8NotationVF", reinterpret_cast<void*>(integer_style_notation)},
        {"$sSz10FoundationE9formattedy12FormatOutputQyd__qd__0C5InputQyd__RszAA0C5StyleRd__lF", reinterpret_cast<void*>(integer_formatted_style)},
        {"$sSz10FoundationE9formattedSSyF", reinterpret_cast<void*>(integer_formatted)},
        {"$s10Foundation4DateVACycfC", reinterpret_cast<void*>(date_now)},
        {"$s10Foundation4DateV11FormatStyleV0bD0V11abbreviatedAGvgZ", reinterpret_cast<void*>(date_style_word)},
        {"$s10Foundation4DateV11FormatStyleV04TimeD0V9shortenedAGvgZ", reinterpret_cast<void*>(date_style_word)},
        {"$s10Foundation4DateV9formatted4date4timeSSAC11FormatStyleV0bG0V_AH04TimeG0VtF", reinterpret_cast<void*>(date_formatted)},
        {"$ss018_bridgeAnyObjectToB0yypyXlSgF", reinterpret_cast<void*>(bridge_any_object_to_any)},
        {"$s10Foundation8URLErrorV4CodeV17badServerResponseAEvgZ", reinterpret_cast<void*>(url_error_code)},
        {"$s10Foundation8URLErrorV4CodeV19cannotParseResponseAEvgZ", reinterpret_cast<void*>(url_error_code)},
        {"$s10Foundation21_BridgedStoredNSErrorPAAE_8userInfox4CodeQz_SDySSypGtcfC", reinterpret_cast<void*>(url_error_init)},
        {"$s10Foundation8URLErrorV8_nsErrorSo7NSErrorCvg", reinterpret_cast<void*>(url_error_ns_error)},
        {"$s10Foundation10URLRequestV19_bridgeToObjectiveCSo12NSURLRequestCyF", reinterpret_cast<void*>(url_request_bridge_to_objc)},
        {"$sSo9WKWebViewC6WebKitE19callAsyncJavaScript_9arguments2in12contentWorldypSgSS_SDySSypGSo11WKFrameInfoCSgSo09WKContentL0CtYaKFTu", &g_web_script_record},
        {"$sSo9WKWebViewC6WebKitE19callAsyncJavaScript_9arguments2in12contentWorldypSgSS_SDySSypGSo11WKFrameInfoCSgSo09WKContentL0CtYaKF", reinterpret_cast<void*>(rt_web_view_call_script_entry)},
        {"kCLLocationAccuracyKilometer", const_cast<double*>(&kLocationAccuracyKilometer)},
        {"$sSS10FoundationE19_bridgeToObjectiveCSo8NSStringCyF", reinterpret_cast<void*>(string_bridge_to_objc)},
        {"$sSS10FoundationE36_unconditionallyBridgeFromObjectiveCySSSo8NSStringCSgFZ", reinterpret_cast<void*>(string_bridge_from_objc)},
    };
    const auto found = table.find(name);
    return found == table.end() ? nullptr : found->second;
}
