#include "foundation_internal.h"

using namespace foundation;

namespace foundation {

const HeapMetadata kStringHeapMetadata = {free, nullptr, 0};

void* g_nsstring_class = nullptr;

void* g_defaults_class = nullptr;

void destroy_nsstring(void* raw) {
    auto* object = static_cast<NSStringObject*>(raw);
    delete object->text;
    free(object);
}

void* new_nsstring(const std::string& text) {
    auto* object = static_cast<NSStringObject*>(calloc(1, sizeof(NSStringObject)));
    object->header.metadata = g_nsstring_class;
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

UserDefaults& defaults() {
    static UserDefaults* instance = [] {
        auto* created = new UserDefaults();
        created->load();
        return created;
    }();
    return *instance;
}

void* defaults_standard(void*, const char*) {
    static HeapObject* singleton = [] {
        auto* object = static_cast<HeapObject*>(calloc(1, sizeof(HeapObject)));
        object->metadata = g_defaults_class;
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

void* g_nsnumber_class = nullptr;

void* new_nsnumber(int64_t value, bool is_bool) {
    auto* number = static_cast<NSNumberObject*>(calloc(1, sizeof(NSNumberObject)));
    number->header.metadata = g_nsnumber_class;
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
    const void* isa = value != nullptr ? static_cast<HeapObject*>(value)->metadata : nullptr;
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
    static std::map<std::string, HeapObject*> singletons;
    auto*& instance = singletons[class_name];
    if (instance == nullptr) {
        instance = static_cast<HeapObject*>(calloc(1, sizeof(HeapObject)));
        instance->metadata = objc_native_class(class_name, free);
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

void* g_nsarray_class = nullptr;

void destroy_nsarray(void* raw) {
    auto* array = static_cast<NSArrayObject*>(raw);
    delete array->items;
    free(array);
}

void* new_nsarray() {
    auto* array = static_cast<NSArrayObject*>(calloc(1, sizeof(NSArrayObject)));
    array->header.metadata = g_nsarray_class;
    array->header.references = 1;
    array->items = new std::vector<void*>();
    return array;
}

bool is_url_type(void* type) {
    const char* name = foreign_type_name(type);
    return name != nullptr && strcmp(name, "_$s10Foundation3URLVMn") == 0;
}

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
    auto* header = reinterpret_cast<HeapObject*>(storage);
    header->metadata = storage_class_metadata("_$ss23_ContiguousArrayStorageCMn", &element_type, 1);
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

void destroy_url(void* raw) {
    auto* box = static_cast<URLBox*>(raw);
    delete box->text;
    free(box);
}

const HeapMetadata kUrlMetadata = {destroy_url, nullptr, 0};

URLBox* new_url(const std::string& text) {
    auto* box = static_cast<URLBox*>(calloc(1, sizeof(URLBox)));
    box->metadata = const_cast<uint64_t*>(&kUrlMetadata.kind);
    new (&box->references) std::atomic<uint64_t>(1);
    box->text = new std::string(text);
    return box;
}

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

SWIFTCC void url_file_with_path(SWIFT_OUT URLBox** out, SwiftString path) { *out = new_url("file://" + swift_string_to_utf8(path)); }

void add_foundation_objects_symbols(SymbolTable& table) {
    table.insert({
        {"$s10Foundation3URLV15fileURLWithPathACSSh_tcfC", reinterpret_cast<void*>(url_file_with_path)},
        {"$sSa10FoundationE36_unconditionallyBridgeFromObjectiveCySayxGSo7NSArrayCSgFZ", reinterpret_cast<void*>(array_bridge_from_objc)},
    });
}

void register_foundation_objects_classes() {
    objc_register_method("-[WKWebsiteDataStore httpCookieStore]", reinterpret_cast<const void*>(cookie_store));
    objc_register_method("-[WKWebView layer]", reinterpret_cast<const void*>(web_view_layer));
    objc_register_method("-[CALayer setCornerRadius:]", reinterpret_cast<const void*>(ignore_message_with_double));
    objc_register_method("-[CALayer setMasksToBounds:]", reinterpret_cast<const void*>(ignore_message_with_word));
    objc_register_method("-[WKWebView setNavigationDelegate:]", reinterpret_cast<const void*>(ignore_message_with_word));
    objc_register_method("-[WKWebView setInspectable:]", reinterpret_cast<const void*>(ignore_message_with_word));
    objc_register_method("-[WKWebViewConfiguration websiteDataStore]", reinterpret_cast<const void*>(web_data_store_default));
    objc_register_method("-[WKWebViewConfiguration setWebsiteDataStore:]", reinterpret_cast<const void*>(ignore_message_with_word));
    objc_register_method("+[WKWebsiteDataStore defaultDataStore]", reinterpret_cast<const void*>(web_data_store_default));
    objc_register_method("+[WKContentWorld pageWorld]", reinterpret_cast<const void*>(content_world_page));
    objc_register_method("+[NSURLSessionConfiguration defaultSessionConfiguration]", reinterpret_cast<const void*>(url_session_configuration_default));
    objc_register_method("-[NSURLSessionConfiguration setTimeoutIntervalForRequest:]", reinterpret_cast<const void*>(ignore_message_with_double));
    objc_register_method("+[NSURLSession sessionWithConfiguration:]", reinterpret_cast<const void*>(url_session_with_configuration));
    objc_register_method("-[NSObject init]", reinterpret_cast<const void*>(return_self));
    objc_register_method("-[NSObject self]", reinterpret_cast<const void*>(return_self));
    objc_register_method("-[NSObject retain]", reinterpret_cast<const void*>(return_self));
    objc_register_method("-[NSObject autorelease]", reinterpret_cast<const void*>(return_self));
    objc_register_method("-[NSUserDefaults initWithSuiteName:]", reinterpret_cast<const void*>(return_self_with_argument));
    objc_register_method("+[NSProcessInfo processInfo]", reinterpret_cast<const void*>(process_info_shared));
    objc_register_method("-[NSProcessInfo operatingSystemVersion]", reinterpret_cast<const void*>(process_info_version));
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

}  // namespace foundation
