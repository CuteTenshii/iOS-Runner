#include "foundation_internal.h"

#include <algorithm>
#include <cstdio>
#include <initializer_list>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "symbols.h"
#include "uikit_internal.h"
#include "uuid.h"

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

// Values from registerDefaults: are never saved, and only answer for keys the app hasn't set.
std::map<std::string, std::string>& registered_defaults() {
    static std::map<std::string, std::string> values;
    return values;
}

const std::string* stored_value(void* key) {
    const std::string name = nsstring_text(key);
    const auto found = defaults().values.find(name);
    if (found != defaults().values.end() && !found->second.empty()) return &found->second;
    const auto registered = registered_defaults().find(name);
    return registered == registered_defaults().end() ? nullptr : &registered->second;
}

void defaults_register(void*, const char*, void* dictionary) {
    void* storage = nullptr;
    if (dictionary == nullptr || !bridged_collection_value(dictionary, storage)) return;
    constexpr size_t kExistentialTypeWord = 3;
    visit_string_dictionary(storage, [](const std::string& name, void* entry) {
        auto** words = static_cast<void**>(entry);
        void* type = words[kExistentialTypeWord];
        if (type == type_system_symbol("$sSbN")) {
            registered_defaults()[name] = std::string(1, kStoredBool) + (*reinterpret_cast<bool*>(entry) ? "1" : "0");
        } else if (type == type_system_symbol("$sSiN")) {
            registered_defaults()[name] = kStoredInteger + std::to_string(*reinterpret_cast<int64_t*>(entry));
        } else if (type == type_system_symbol("$sSSN")) {
            registered_defaults()[name] = kStoredString + swift_string_to_utf8(*reinterpret_cast<SwiftString*>(entry));
        } else {
            engine_log("DEFAULTS: not registering a value of an unsupported type for %s", name.c_str());
        }
    });
}

double defaults_double_for_key(void*, const char*, void* key) {
    const auto* stored = stored_value(key);
    return stored == nullptr ? 0 : strtod(stored->c_str() + 1, nullptr);
}

bool defaults_synchronize(void*, const char*) { return true; }

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

void defaults_set_double(void*, const char*, void* key, double value) {
    char text[64];
    snprintf(text, sizeof text, "%.17g", value);
    defaults().values[nsstring_text(key)] = kStoredInteger + std::string(text);
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

// Answers as the app's deployment target, like __isPlatformVersionAtLeast.
bool process_info_at_least(void*, const char*, const OperatingSystemVersion* version) {
    using Check = int32_t (*)(uint32_t, uint32_t, uint32_t, uint32_t);
    static const auto check = reinterpret_cast<Check>(resolve_symbol("__isPlatformVersionAtLeast"));
    return check(0, static_cast<uint32_t>(version->major), static_cast<uint32_t>(version->minor), static_cast<uint32_t>(version->patch)) != 0;
}

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
    if (element_type == any_type()) {
        char* elements;
        void* converted = make_array_storage(element_type, count, &elements);
        auto* values = reinterpret_cast<AnyValue*>(elements);
        for (size_t i = 0; i < count; ++i) {
            void* item = (*array->items)[i];
            memset(&values[i], 0, sizeof values[i]);
            values[i].buffer[0] = reinterpret_cast<uint64_t>(rt_swift_retain(item));
            values[i].type = *static_cast<void**>(item);
        }
        return converted;
    }
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
    void* object;
    void* block;
    void* target;
    std::string selector;
};

std::vector<NotificationObserver>& notification_observers() {
    static std::vector<NotificationObserver> observers;
    return observers;
}

// Hash tables and map tables keep their entries strongly: nothing here frees an object a guest still points at.
struct ObjectTable {
    std::vector<void*> values;
    std::vector<void*> keys;
};

std::unordered_map<void*, ObjectTable>& object_tables() {
    static std::unordered_map<void*, ObjectTable> tables;
    return tables;
}

void* hash_table_make(void*, const char*) { return objc_framework_object("NSHashTable"); }
void* map_table_make(void*, const char*) { return objc_framework_object("NSMapTable"); }

void hash_table_add(void* self, const char*, void* object) {
    auto& values = object_tables()[self].values;
    if (std::find(values.begin(), values.end(), object) == values.end()) {
        rt_swift_retain(object);
        values.push_back(object);
    }
}

void hash_table_remove(void* self, const char*, void* object) {
    auto& values = object_tables()[self].values;
    values.erase(std::remove(values.begin(), values.end(), object), values.end());
}

bool hash_table_contains(void* self, const char*, void* object) {
    const auto& values = object_tables()[self].values;
    return std::find(values.begin(), values.end(), object) != values.end();
}

void* hash_table_all(void* self, const char*) {
    auto* array = static_cast<NSArrayObject*>(new_nsarray());
    *array->items = object_tables()[self].values;
    return array;
}

int64_t hash_table_count(void* self, const char*) { return static_cast<int64_t>(object_tables()[self].values.size()); }
void table_remove_all(void* self, const char*) { object_tables().erase(self); }

void map_table_set(void* self, const char*, void* value, void* key) {
    auto& table = object_tables()[self];
    const auto found = std::find(table.keys.begin(), table.keys.end(), key);
    rt_swift_retain(value);
    if (found != table.keys.end()) {
        table.values[static_cast<size_t>(found - table.keys.begin())] = value;
        return;
    }
    rt_swift_retain(key);
    table.keys.push_back(key);
    table.values.push_back(value);
}

void* map_table_get(void* self, const char*, void* key) {
    auto& table = object_tables()[self];
    const auto found = std::find(table.keys.begin(), table.keys.end(), key);
    return found == table.keys.end() ? nullptr : table.values[static_cast<size_t>(found - table.keys.begin())];
}

void map_table_remove(void* self, const char*, void* key) {
    auto& table = object_tables()[self];
    const auto found = std::find(table.keys.begin(), table.keys.end(), key);
    if (found == table.keys.end()) return;
    table.values.erase(table.values.begin() + (found - table.keys.begin()));
    table.keys.erase(found);
}

void* notification_center_default(void*, const char*) { return framework_singleton("NSNotificationCenter"); }

// An NSNotification keeps its parts in the words after its object header.
constexpr size_t kNotificationNameWord = 2, kNotificationObjectWord = 3, kNotificationInfoWord = 4;

void* new_notification(void* name, void* object, void* info) {
    auto** notification = static_cast<void**>(objc_framework_object("NSNotification"));
    notification[kNotificationNameWord] = name;
    notification[kNotificationObjectWord] = object;
    notification[kNotificationInfoWord] = info;
    return notification;
}

void* notification_name(void* self, const char*) { return static_cast<void**>(self)[kNotificationNameWord]; }
void* notification_object(void* self, const char*) { return static_cast<void**>(self)[kNotificationObjectWord]; }
void* notification_info(void* self, const char*) { return static_cast<void**>(self)[kNotificationInfoWord]; }

// The block runs synchronously when posted, as it would with a nil queue; the returned token is the block.
void* notification_add_block(void*, const char*, void* name, void* object, void*, void* block) {
    void* copy = darwin_block_copy(block);
    notification_observers().push_back({name == nullptr ? std::string() : nsstring_text(name), object, copy, nullptr, {}});
    return copy;
}

void notification_add_observer(void*, const char*, void* target, const char* selector, void* name, void* object) {
    notification_observers().push_back({name == nullptr ? std::string() : nsstring_text(name), object, nullptr, target, selector});
}

void notification_remove(void*, const char*, void* token) {
    auto& observers = notification_observers();
    observers.erase(std::remove_if(observers.begin(), observers.end(), [token](const NotificationObserver& o) { return o.block == token || o.target == token; }), observers.end());
}

void notification_remove_named(void*, const char*, void* token, void* name, void*) {
    auto& observers = notification_observers();
    const std::string text = name == nullptr ? std::string() : nsstring_text(name);
    observers.erase(std::remove_if(observers.begin(), observers.end(), [&](const NotificationObserver& o) { return (o.block == token || o.target == token) && (text.empty() || o.name == text); }), observers.end());
}

void deliver_notification(void* notification, void* name, void* object) {
    constexpr size_t kBlockInvokeWord = 2;
    const std::string text = nsstring_text(name);
    const auto observers = notification_observers();
    for (const auto& observer : observers) {
        if (!observer.name.empty() && observer.name != text) continue;
        if (observer.object != nullptr && observer.object != object) continue;
        if (observer.block != nullptr) {
            using Invoke = void (*)(void*, void*);
            reinterpret_cast<Invoke>(static_cast<void**>(observer.block)[kBlockInvokeWord])(observer.block, notification);
        } else {
            objc_send(observer.target, observer.selector.c_str(), notification);
        }
    }
}

void notification_post_info(void*, const char*, void* name, void* object, void* info) { deliver_notification(new_notification(name, object, info), name, object); }
void notification_post_name(void* self, const char* selector, void* name, void* object) { notification_post_info(self, selector, name, object, nullptr); }

void notification_post(void*, const char*, void* notification) {
    deliver_notification(notification, notification_name(notification, nullptr), notification_object(notification, nullptr));
}

SWIFTCC void notification_bridge_out(SWIFT_OUT void** out, void* object) { *out = object != nullptr ? rt_swift_retain(object) : nullptr; }

SWIFTCC void* notification_value_name(SWIFT_CONTEXT void** self) { return notification_name(*self, nullptr); }

SWIFTCC void* notification_value_info(SWIFT_CONTEXT void** self) {
    void* info = notification_info(*self, nullptr);
    if (void* value; info != nullptr && bridged_collection_value(info, value)) return rt_swift_bridgeObjectRetain(value);
    return nullptr;
}

// Optional<Any>: an empty existential means nil, and a posted object is always one reference.
SWIFTCC void notification_value_object(SWIFT_OUT AnyValue* out, SWIFT_CONTEXT void** self) {
    void* object = notification_object(*self, nullptr);
    memset(out, 0, sizeof *out);
    if (object == nullptr) return;
    out->buffer[0] = reinterpret_cast<uint64_t>(rt_swift_retain(object));
    out->type = *static_cast<void**>(object);
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
        {"$s10Foundation12NotificationV36_unconditionallyBridgeFromObjectiveCyACSo14NSNotificationCSgFZ", reinterpret_cast<void*>(notification_bridge_out)},
        {"$s10Foundation12NotificationV4nameSo18NSNotificationNameavg", reinterpret_cast<void*>(notification_value_name)},
        {"$s10Foundation12NotificationV6objectypSgvg", reinterpret_cast<void*>(notification_value_object)},
        {"$s10Foundation12NotificationV8userInfoSDys11AnyHashableVypGSgvg", reinterpret_cast<void*>(notification_value_info)},
        {"$s10Foundation3URLV15fileURLWithPathACSSh_tcfC", reinterpret_cast<void*>(url_file_with_path)},
        {"$sSa10FoundationE36_unconditionallyBridgeFromObjectiveCySayxGSo7NSArrayCSgFZ", reinterpret_cast<void*>(array_bridge_from_objc)},
    });
}

namespace {

struct ProgressState {
    int64_t total = 0;
    int64_t completed = 0;
    bool cancelled = false;
    void* cancellation_handler = nullptr;
    std::vector<std::pair<void*, int64_t>> children;
};

std::unordered_map<void*, ProgressState>& progress_states() {
    static std::unordered_map<void*, ProgressState> states;
    return states;
}

double progress_fraction(void* progress) {
    const ProgressState& state = progress_states()[progress];
    if (state.total <= 0) return 0;
    double done = static_cast<double>(state.completed);
    for (const auto& [child, pending] : state.children) done += progress_fraction(child) * static_cast<double>(pending);
    return std::min(done / static_cast<double>(state.total), 1.0);
}

void* new_progress(int64_t total) {
    void* progress = objc_framework_object("NSProgress");
    progress_states()[progress].total = total;
    return progress;
}

void* new_real_number(double value) {
    auto* number = static_cast<NSNumberObject*>(new_nsnumber(static_cast<int64_t>(value), false));
    number->is_real = true;
    number->real = value;
    return number;
}

double number_as_double(const void* self) {
    const auto* number = static_cast<const NSNumberObject*>(self);
    return number->is_real ? number->real : static_cast<double>(number->value);
}

void register_number_methods() {
    const auto from_word = reinterpret_cast<const void*>(+[](void*, const char*, int64_t value) { return new_nsnumber(value, false); });
    const auto from_int32 = reinterpret_cast<const void*>(+[](void*, const char*, int32_t value) { return new_nsnumber(value, false); });
    const auto from_uint32 = reinterpret_cast<const void*>(+[](void*, const char*, uint32_t value) { return new_nsnumber(value, false); });
    const auto from_int16 = reinterpret_cast<const void*>(+[](void*, const char*, int16_t value) { return new_nsnumber(value, false); });
    const auto from_bool = reinterpret_cast<const void*>(+[](void*, const char*, bool value) { return new_nsnumber(value ? 1 : 0, true); });
    const auto from_double = reinterpret_cast<const void*>(+[](void*, const char*, double value) { return new_real_number(value); });
    const auto from_float = reinterpret_cast<const void*>(+[](void*, const char*, float value) { return new_real_number(value); });
    for (const std::string prefix : {"-[NSNumber initWith", "+[NSNumber numberWith"}) {
        for (const auto& [name, method] : std::initializer_list<std::pair<const char*, const void*>>{{"Integer:]", from_word}, {"LongLong:]", from_word}, {"Long:]", from_word}, {"UnsignedInteger:]", from_word}, {"UnsignedLongLong:]", from_word}, {"Int:]", from_int32}, {"UnsignedInt:]", from_uint32}, {"Short:]", from_int16}, {"Bool:]", from_bool}, {"Double:]", from_double}, {"Float:]", from_float}}) {
            objc_register_method((prefix + name).c_str(), method);
        }
    }
    const auto as_word = reinterpret_cast<const void*>(+[](void* self, const char*) { return static_cast<NSNumberObject*>(self)->value; });
    for (const char* selector : {"-[NSNumber integerValue]", "-[NSNumber longLongValue]", "-[NSNumber longValue]", "-[NSNumber unsignedIntegerValue]", "-[NSNumber unsignedLongLongValue]"}) objc_register_method(selector, as_word);
    objc_register_method("-[NSNumber intValue]", reinterpret_cast<const void*>(+[](void* self, const char*) { return static_cast<int32_t>(static_cast<NSNumberObject*>(self)->value); }));
    objc_register_method("-[NSNumber boolValue]", reinterpret_cast<const void*>(+[](void* self, const char*) { return number_as_double(self) != 0; }));
    objc_register_method("-[NSNumber doubleValue]", reinterpret_cast<const void*>(+[](void* self, const char*) { return number_as_double(self); }));
    objc_register_method("-[NSNumber floatValue]", reinterpret_cast<const void*>(+[](void* self, const char*) { return static_cast<float>(number_as_double(self)); }));
    objc_register_method("-[NSNumber stringValue]", reinterpret_cast<const void*>(+[](void* self, const char*) -> void* {
        const auto* number = static_cast<NSNumberObject*>(self);
        if (!number->is_real) return new_nsstring(std::to_string(number->value));
        char text[40];
        snprintf(text, sizeof text, "%.17g", number->real);
        return new_nsstring(text);
    }));
}

void register_progress_methods() {
    const auto with_total = reinterpret_cast<const void*>(+[](void*, const char*, int64_t total) { return new_progress(total); });
    objc_register_method("+[NSProgress discreteProgressWithTotalUnitCount:]", with_total);
    objc_register_method("+[NSProgress progressWithTotalUnitCount:]", with_total);
    objc_register_method("-[NSProgress totalUnitCount]", reinterpret_cast<const void*>(+[](void* self, const char*) { return progress_states()[self].total; }));
    objc_register_method("-[NSProgress setTotalUnitCount:]", reinterpret_cast<const void*>(+[](void* self, const char*, int64_t total) { progress_states()[self].total = total; }));
    objc_register_method("-[NSProgress completedUnitCount]", reinterpret_cast<const void*>(+[](void* self, const char*) { return progress_states()[self].completed; }));
    objc_register_method("-[NSProgress setCompletedUnitCount:]", reinterpret_cast<const void*>(+[](void* self, const char*, int64_t completed) { progress_states()[self].completed = completed; }));
    objc_register_method("-[NSProgress fractionCompleted]", reinterpret_cast<const void*>(+[](void* self, const char*) { return progress_fraction(self); }));
    objc_register_method("-[NSProgress isFinished]", reinterpret_cast<const void*>(+[](void* self, const char*) { return progress_fraction(self) >= 1.0; }));
    objc_register_method("-[NSProgress isCancelled]", reinterpret_cast<const void*>(+[](void* self, const char*) { return progress_states()[self].cancelled; }));
    objc_register_method("-[NSProgress cancel]", reinterpret_cast<const void*>(+[](void* self, const char*) {
        // Copied first: reaching a child's state can grow the table this reference points into.
        const auto children = progress_states()[self].children;
        progress_states()[self].cancelled = true;
        if (void* handler = progress_states()[self].cancellation_handler) {
            using Invoke = void (*)(void*);
            reinterpret_cast<Invoke>(static_cast<BlockHeader*>(handler)->invoke)(handler);
        }
        using Cancel = void (*)(void*, const char*);
        for (const auto& [child, pending] : children) reinterpret_cast<Cancel>(const_cast<void*>(objc_method_for(child, "cancel")))(child, "cancel");
    }));
    objc_register_method("-[NSProgress setCancellationHandler:]", reinterpret_cast<const void*>(+[](void* self, const char*, void* handler) { progress_states()[self].cancellation_handler = handler != nullptr ? darwin_block_copy(handler) : nullptr; }));
    objc_register_method("-[NSProgress addChild:withPendingUnitCount:]", reinterpret_cast<const void*>(+[](void* self, const char*, void* child, int64_t pending) {
        rt_swift_retain(child);
        progress_states()[self].children.emplace_back(child, pending);
    }));
}

}  // namespace

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
    objc_register_method("-[NSProcessInfo globallyUniqueString]", reinterpret_cast<const void*>(+[](void*, const char*) -> void* { return new_nsstring(uuid4()); }));
    register_progress_methods();
    register_number_methods();
    // One shared object: main-thread work runs from the guest's run loop, not from this one.
    objc_register_method("+[NSRunLoop mainRunLoop]", reinterpret_cast<const void*>(+[](void*, const char*) -> void* {
                             static void* const loop = objc_framework_object("NSRunLoop");
                             return loop;
                         }));
    objc_register_method("-[NSProcessInfo processName]", reinterpret_cast<const void*>(+[](void*, const char*) -> void* { return new_nsstring(bundle_info_string("CFBundleExecutable")); }));
    objc_register_method("-[NSProcessInfo processIdentifier]", reinterpret_cast<const void*>(+[](void*, const char*) -> int32_t { return getpid(); }));
    objc_register_method("-[NSProcessInfo activeProcessorCount]", reinterpret_cast<const void*>(+[](void*, const char*) -> int64_t { return sysconf(_SC_NPROCESSORS_ONLN); }));
    objc_register_method("-[NSProcessInfo processorCount]", reinterpret_cast<const void*>(+[](void*, const char*) -> int64_t { return sysconf(_SC_NPROCESSORS_CONF); }));
    objc_register_method("-[NSProcessInfo isLowPowerModeEnabled]", reinterpret_cast<const void*>(+[](void*, const char*) -> bool { return false; }));
    objc_register_method("-[NSProcessInfo isiOSAppOnMac]", reinterpret_cast<const void*>(+[](void*, const char*) -> bool { return false; }));
    objc_register_method("-[NSProcessInfo isMacCatalystApp]", reinterpret_cast<const void*>(+[](void*, const char*) -> bool { return false; }));
    objc_register_method("-[NSProcessInfo isOperatingSystemAtLeastVersion:]", reinterpret_cast<const void*>(process_info_at_least));
    // The end of every guest dealloc chain. A stale property set must not reach whoever reuses this address.
    objc_register_method("-[NSObject dealloc]", reinterpret_cast<const void*>(+[](void* self, const char*) {
                             uikit::property_forget(self);
                             free(self);
                         }));
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
    objc_register_method("-[NSNotificationCenter addObserver:selector:name:object:]", reinterpret_cast<const void*>(notification_add_observer));
    objc_register_method("-[NSNotificationCenter removeObserver:name:object:]", reinterpret_cast<const void*>(notification_remove_named));
    objc_register_method("-[NSNotificationCenter postNotificationName:object:userInfo:]", reinterpret_cast<const void*>(notification_post_info));
    objc_register_method("-[NSNotificationCenter postNotification:]", reinterpret_cast<const void*>(notification_post));
    objc_register_method("+[NSHashTable weakObjectsHashTable]", reinterpret_cast<const void*>(hash_table_make));
    objc_register_method("-[NSHashTable addObject:]", reinterpret_cast<const void*>(hash_table_add));
    objc_register_method("-[NSHashTable removeObject:]", reinterpret_cast<const void*>(hash_table_remove));
    objc_register_method("-[NSHashTable containsObject:]", reinterpret_cast<const void*>(hash_table_contains));
    objc_register_method("-[NSHashTable allObjects]", reinterpret_cast<const void*>(hash_table_all));
    objc_register_method("-[NSHashTable count]", reinterpret_cast<const void*>(hash_table_count));
    objc_register_method("-[NSHashTable removeAllObjects]", reinterpret_cast<const void*>(table_remove_all));
    for (const char* maker : {"+[NSMapTable strongToStrongObjectsMapTable]", "+[NSMapTable strongToWeakObjectsMapTable]", "+[NSMapTable weakToStrongObjectsMapTable]", "+[NSMapTable weakToWeakObjectsMapTable]"}) objc_register_method(maker, reinterpret_cast<const void*>(map_table_make));
    objc_register_method("-[NSMapTable setObject:forKey:]", reinterpret_cast<const void*>(map_table_set));
    objc_register_method("-[NSMapTable objectForKey:]", reinterpret_cast<const void*>(map_table_get));
    objc_register_method("-[NSMapTable removeObjectForKey:]", reinterpret_cast<const void*>(map_table_remove));
    objc_register_method("-[NSMapTable removeAllObjects]", reinterpret_cast<const void*>(table_remove_all));
    objc_register_method("-[NSNotification name]", reinterpret_cast<const void*>(notification_name));
    objc_register_method("-[NSNotification object]", reinterpret_cast<const void*>(notification_object));
    objc_register_method("-[NSNotification userInfo]", reinterpret_cast<const void*>(notification_info));
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
    objc_register_method("-[NSUserDefaults registerDefaults:]", reinterpret_cast<const void*>(defaults_register));
    objc_register_method("-[NSUserDefaults doubleForKey:]", reinterpret_cast<const void*>(defaults_double_for_key));
    objc_register_method("-[NSUserDefaults setDouble:forKey:]", reinterpret_cast<const void*>(defaults_set_double));
    objc_register_method("-[NSUserDefaults setValue:forKey:]", reinterpret_cast<const void*>(defaults_set_object));
    objc_register_method("-[NSUserDefaults valueForKey:]", reinterpret_cast<const void*>(defaults_object_for_key));
    objc_register_method("-[NSUserDefaults synchronize]", reinterpret_cast<const void*>(defaults_synchronize));
    objc_register_method("-[NSUserDefaults removeObjectForKey:]", reinterpret_cast<const void*>(defaults_remove));
}

}  // namespace foundation

void* object_from_any(const AnyValue& value) {
    constexpr uint64_t kLargestMetadataKind = 0x7ff;
    if (value.type == nullptr) return nullptr;
    uint64_t kind;
    memcpy(&kind, value.type, sizeof kind);
    if (kind == 0 || kind > kLargestMetadataKind) return reinterpret_cast<void*>(value.buffer[0]);
    if (value.type == type_system_symbol("$sSSN")) return new_nsstring(swift_string_to_utf8(*reinterpret_cast<const SwiftString*>(value.buffer)));
    if (value.type == type_system_symbol("$sSbN")) return new_nsnumber(static_cast<uint8_t>(value.buffer[0]) != 0 ? 1 : 0, true);
    if (value.type == type_system_symbol("$sSiN")) return new_nsnumber(static_cast<int64_t>(value.buffer[0]), false);
    engine_log("STOP: a value in an [Any] of a type that has no object form");
    abort();
}
