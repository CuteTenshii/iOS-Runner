#include "foundation_shim.h"

#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <map>
#include <mutex>
#include <sstream>
#include <unordered_map>
#include <vector>

#include "engine.h"
#include "localization.h"
#include "objc_runtime.h"
#include "type_system.h"

#define SWIFTCC __attribute__((swiftcall))
#define SWIFT_OUT __attribute__((swift_indirect_result))
#define SWIFT_SELF __attribute__((swift_context))
#define SWIFT_CTX __attribute__((swift_context))

uint64_t* empty_array_singleton();

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

// Stored values carry their type in a one-letter prefix, so objects come back as what was saved.
constexpr char kStoredString = 's';
constexpr char kStoredBool = 'b';
constexpr char kStoredInteger = 'i';

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
        case kStoredBool: return new_nsnumber(body == "1", true);
        case kStoredInteger: return new_nsnumber(strtoll(body.c_str(), nullptr, 10), false);
        default: return new_nsstring(body);
    }
}

// Arrays are not persisted yet, so a missing value is the only answer this can give truthfully.
void* defaults_string_array_for_key(void*, const char*, void*) { return nullptr; }

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
        // Collections are not persisted yet; the app simply finds no saved value next launch.
        engine_log("DEFAULTS: not saving a collection for %s", nsstring_text(key).c_str());
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

void cookie_store_get_all(void*, const char*, BlockHeader* completion) { completion->invoke(completion, new_nsarray()); }

constexpr size_t kArrayHeaderBytes = 32;
constexpr size_t kArrayCountOffset = 16;
constexpr size_t kArrayCapacityOffset = 24;

SWIFTCC void* array_bridge_from_objc(void* object, void* element_type) {
    auto* array = static_cast<NSArrayObject*>(object);
    if (array == nullptr || array->items->empty()) return empty_array_singleton();
    const size_t count = array->items->size();
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

void* web_view_configuration(void*, const char*) { return framework_singleton("WKWebViewConfiguration"); }

void* cookie_store(void*, const char*) { return framework_singleton("WKHTTPCookieStore"); }

void* web_view_layer(void*, const char*) { return framework_singleton("CALayer"); }

void* web_view_init(void* self, const char*, double, double, double, double, void*) { return self; }

void* web_kit_child_singleton(void*, const char* selector) { return framework_singleton(selector); }
void* web_data_store_default(void*, const char*) { return framework_singleton("WKWebsiteDataStore"); }
void* content_world_page(void*, const char*) { return framework_singleton("WKContentWorld"); }
void* url_session_configuration_default(void*, const char*) { return framework_singleton("NSURLSessionConfiguration"); }
void* url_session_with_configuration(void*, const char*, void*) { return framework_singleton("NSURLSession"); }

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
    if ((storage >> 62) != (kDataSliceTag >> 62)) return {};
    const auto* backing = reinterpret_cast<const DataStorage*>(storage & ~(3ULL << 62));
    const auto low = static_cast<uint32_t>(range);
    const auto high = static_cast<uint32_t>(range >> 32);
    return std::string(reinterpret_cast<const char*>(backing->bytes) + low, high - low);
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
    static constexpr struct { double scale; const char* suffix; } kUnits[] = {{1e9, "B"}, {1e6, "M"}, {1e3, "K"}};
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
    struct timespec now {};
    clock_gettime(CLOCK_REALTIME, &now);
    constexpr double kSecondsFrom1970To2001 = 978307200.0;
    *out = static_cast<double>(now.tv_sec) - kSecondsFrom1970To2001 + static_cast<double>(now.tv_nsec) * 1e-9;
}

SWIFTCC void date_style_word(SWIFT_OUT uint64_t* out) { *out = kStyleWord; }

SWIFTCC SwiftString date_formatted(const void*, const void*, SWIFT_CTX const double* self) {
    constexpr double kSecondsFrom1970To2001 = 978307200.0;
    const auto seconds = static_cast<time_t>(*self + kSecondsFrom1970To2001);
    struct tm local {};
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
#define URL_SESSION_ENTRY(name, fetch)         \
    ".globl " #name "\n"                       \
    #name ":\n"                                \
    "  stp x29, x30, [sp, #-48]!\n"            \
    "  mov x29, sp\n"                          \
    "  mov x1, x0\n"                           \
    "  add x0, sp, #16\n"                      \
    "  bl " #fetch "\n"                        \
    "  ldp x0, x1, [sp, #16]\n"                \
    "  ldp x2, x20, [sp, #32]\n"               \
    "  ldp x29, x30, [sp], #48\n"              \
    "  ldr x3, [x22, #8]\n"                    \
    "  br x3\n"

__asm__(
    ".text\n"
    URL_SESSION_ENTRY(rt_url_session_data_url, rt_url_session_fetch_url)
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

// Puts an Objective-C object into an Any as its Swift value, as the runtime's bridging does.
SWIFTCC void bridge_any_object_to_any(SWIFT_OUT AnyValue* out, void* object) {
    memset(out, 0, sizeof *out);
    const void* isa = object != nullptr ? static_cast<NativeObject*>(object)->isa : nullptr;
    if (isa == g_nsstring_class) {
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

std::string foundation_url_text(const void* url) { return *static_cast<const URLBox*>(url)->text; }

void foundation_register_classes() {
    const auto make_record = [](AsyncFunctionRecord& record, void (*entry)()) {
        record.context_size = kUrlSessionContextBytes;
        record.function = static_cast<int32_t>(reinterpret_cast<intptr_t>(entry) - reinterpret_cast<intptr_t>(&record));
    };
    make_record(g_url_session_url_record, rt_url_session_data_url);
    make_record(g_url_session_request_record, rt_url_session_data_request);
    g_http_response_class = objc_native_class("NSHTTPURLResponse", free);
    objc_register_method("-[NSHTTPURLResponse statusCode]", reinterpret_cast<const void*>(http_response_status));
    g_nsstring_class = objc_native_class("NSString", destroy_nsstring);
    g_defaults_class = objc_native_class("NSUserDefaults", free);
    objc_register_method("-[WKUserScript initWithSource:injectionTime:forMainFrameOnly:]", reinterpret_cast<const void*>(user_script_init));
    objc_register_method("-[WKWebView initWithFrame:configuration:]", reinterpret_cast<const void*>(web_view_init));
    objc_register_method("-[WKWebsiteDataStore httpCookieStore]", reinterpret_cast<const void*>(cookie_store));
    g_nsarray_class = objc_native_class("NSArray", destroy_nsarray);
    g_bridged_collection_class = objc_native_class("NSDictionary", destroy_bridged_collection);
    objc_register_method("-[WKHTTPCookieStore getAllCookies:]", reinterpret_cast<const void*>(cookie_store_get_all));
    objc_register_method("-[WKHTTPCookieStore addObserver:]", reinterpret_cast<const void*>(ignore_message_with_word));
    objc_register_method("-[WKWebView configuration]", reinterpret_cast<const void*>(web_view_configuration));
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
    objc_register_method("-[NSURLSessionConfiguration setHTTPAdditionalHeaders:]", reinterpret_cast<const void*>(session_set_headers));
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
    static const std::unordered_map<std::string_view, void*> table = {
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
        {"kCLLocationAccuracyKilometer", const_cast<double*>(&kLocationAccuracyKilometer)},
        {"$sSS10FoundationE19_bridgeToObjectiveCSo8NSStringCyF", reinterpret_cast<void*>(string_bridge_to_objc)},
        {"$sSS10FoundationE36_unconditionallyBridgeFromObjectiveCySSSo8NSStringCSgFZ", reinterpret_cast<void*>(string_bridge_from_objc)},
    };
    const auto found = table.find(name);
    return found == table.end() ? nullptr : found->second;
}
