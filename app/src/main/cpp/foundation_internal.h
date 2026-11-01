#pragma once
#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <algorithm>
#include <atomic>
#include <cctype>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>
#include <functional>
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
#include "foundation_shim.h"
#include "json.h"
#include "localization.h"
#include "objc_runtime.h"
#include "stdlib_shim.h"
#include "swift_abi.h"
#include "type_system.h"

// Private to the foundation shim sources.
namespace foundation {

extern "C" void* darwin_block_copy(const void*);

constexpr uint64_t kSmallStringBit = 1ULL << 61;
constexpr uint64_t kSmallAsciiBit = 1ULL << 62;
constexpr uint64_t kImmortalBit = 1ULL << 63;
constexpr uint64_t kObjectPointerMask = 0x0fffffffffffffffULL;
constexpr uint64_t kAsciiFlag = 1ULL << 63;
constexpr uint64_t kNfcFlag = 1ULL << 62;
constexpr size_t kSmallStringCapacity = 15;
constexpr size_t kSmallCountShift = 56;
constexpr uint64_t kSmallCountMask = 0x0f;
constexpr size_t kNativeBias = 32;
constexpr const char* kDefaultsFileName = ".defaults";

struct AnyValue {
    uint64_t buffer[3];
    void* type;
};

struct NSStringObject {
    HeapObject header;
    std::string* text;
};

constexpr size_t kStorageCapacityOffset = 16;

extern const HeapMetadata kStringHeapMetadata;

extern void* g_nsstring_class;

extern void* g_defaults_class;

void destroy_nsstring(void* raw);
void* new_nsstring(const std::string& text);

// Imports resolve before classes register, so these get their class later.
std::vector<void*>& string_constants();
const std::string& nsstring_text(void* object);

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

UserDefaults& defaults();
extern "C" void* rt_swift_bridgeObjectRetain(void*);

struct NSNumberObject {
    HeapObject header;
    int64_t value;
    bool is_bool;
};

extern void* g_nsnumber_class;

void* defaults_string_for_key(void*, const char*, void* key);
void* defaults_object_for_key(void*, const char*, void* key);
bool defaults_bool_for_key(void*, const char*, void* key);
int64_t defaults_integer_for_key(void*, const char*, void* key);
void defaults_set_bool(void*, const char*, bool value, void* key);
void defaults_set_integer(void*, const char*, int64_t value, void* key);
void defaults_set_object(void*, const char*, void* value, void* key);
void* framework_singleton(const char* class_name);

extern const double kLocationAccuracyKilometer;

// NSArray keeps retained references; the app only receives arrays from this runtime.
struct NSArrayObject {
    HeapObject header;
    std::vector<void*>* items;
};

extern void* g_nsarray_class;

void destroy_nsarray(void* raw);
void* new_nsarray();

struct BlockHeader {
    void* isa;
    int32_t flags;
    int32_t reserved;
    void (*invoke)(void*, void*);
};

struct URLBox;

constexpr size_t kArrayHeaderBytes = 32;

extern void* g_url_session_delegate;

// Foundation.URL is resilient, so the guest only ever holds this object through one reference.
struct URLBox {
    void* metadata;
    std::atomic<uint64_t> references;
    std::string* text;
};

URLBox* new_url(const std::string& text);

constexpr const char* kFileScheme = "file://";

bool make_directories(const std::string& path);
std::string url_path(const std::string& text);
std::string path_last_component(std::string path);
std::string text_without_query(const std::string& text);

struct OptionalSwiftString {
    uint64_t count_and_flags;
    uint64_t object;
};

extern void* g_nsurl_class;

URLBox* make_url_box(const std::string& text);

extern const int32_t kDirectoryHintInferFromPath;

constexpr uint64_t kLocaleImmediate = 1ULL << 63;

extern const int32_t kBundleDescriptionForClass;

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

struct DataValue {
    uint64_t range;
    uint64_t storage;
};

DataValue make_data(const std::string& bytes);

// An Error existential is one reference to a box; this box carries a domain, a code and a message.
struct ErrorBox {
    void* metadata;
    std::atomic<uint64_t> references;
    std::string* message;
    int64_t code;
};

void* make_error(const std::string& message, int64_t code);
const char* value_witnesses(void* type);

// Foundation.URLRequest is resilient here too: one reference to this box.
struct URLRequestBox {
    void* metadata;
    std::atomic<uint64_t> references;
    std::string* url;
    std::string* method;
    std::string* headers;
    std::string* body;
};

std::string data_bytes(uint64_t range, uint64_t storage);

extern void* g_http_response_class;

struct AsyncDataResult {
    DataValue data;
    void* response;
    void* error;
};

AsyncDataResult perform_request(const std::string& url, const std::string& method, const std::string& headers, const std::string& body);
extern "C" void rt_url_session_data_url();
extern "C" void rt_url_session_data_request();

// An async function pointer: a relative address, then the callee's context size.
struct AsyncFunctionRecord {
    int32_t function;
    uint32_t context_size;
};

extern AsyncFunctionRecord g_url_session_url_record;

extern AsyncFunctionRecord g_url_session_request_record;

// A bridged collection only needs to stay alive until framework code uses it, so it wraps the Swift value.
struct BridgedCollection {
    HeapObject header;
    void* value;
    // Set when the guest bridged it, and null for collections the runtime made.
    void* key_type;
    void* element_type;
};

extern void* g_bridged_collection_class;

extern "C" void rt_swift_bridgeObjectRelease(void*);
void destroy_bridged_collection(void* raw);
SWIFTCC void* collection_bridge_to_objc(void* value);
bool bridged_collection_value(void* object, void*& value);

}  // namespace foundation

using namespace foundation;

std::string swift_string_to_utf8(SwiftString value);
SwiftString swift_string_from_utf8(const std::string& text);

namespace foundation {

constexpr size_t kDictionaryScaleOffset = 32;
constexpr size_t kBitsPerWord = 64;

struct WebConfiguration {
    std::vector<std::string> scripts;
    std::string application_name;
    HeapObject* controller = nullptr;
};

}  // namespace foundation

using namespace foundation;

std::map<const void*, WebConfiguration*>& web_configurations();
WebConfiguration* configuration_of(void* object);
std::map<const void*, std::string>& user_script_sources();

namespace foundation {

struct WebViewState {
    int id;
    void* configuration;
    std::string url;
};

}  // namespace foundation

using namespace foundation;

std::map<const void*, WebViewState>& web_views();

// Bridged requests only need to carry their URL into loadRequest:.
// A bridged request reads as its URL; the full request stays here for URLSession to find.
std::unordered_map<void*, URLRequestBox*>& bridged_requests();

// Swift values to JSON, for the arguments of a script call.
std::string value_to_json(void* value, void* type);

namespace foundation {

struct PendingScriptCall {
    AnyValue* out;
    void* context;
};

}  // namespace foundation

using namespace foundation;

std::map<int, PendingScriptCall>& pending_script_calls();

namespace foundation {

extern "C" void rt_web_view_call_script_entry();

}  // namespace foundation

using namespace foundation;

extern AsyncFunctionRecord g_web_script_record;

extern void* g_cookie_class;

extern void* g_cookie_observer;

// Only what the script results here need: an object of booleans, or a single value.
void write_script_result(AnyValue* out, const std::string& json);
void* any_type();

// A JSON value as the Swift value Foundation would bridge it to; null stays an object no `as?` accepts.
void json_to_any(const Json& json, AnyValue* out);

namespace foundation {

// NSData and the object JSONSerialization returns: thin holders that only live until the guest bridges them.
struct NSDataObject {
    HeapObject header;
    std::string* bytes;
};

struct JSONValueObject {
    HeapObject header;
    Json* value;
};

}  // namespace foundation

using namespace foundation;

extern void* g_nsdata_class;

extern void* g_json_value_class;

void destroy_nsdata(void* raw);
void destroy_json_value(void* raw);
void* make_nsdata(const std::string& bytes);
void* property_list_object_from_file(const std::string& path, bool dictionary);
bool write_property_list_object(void* object, const std::string& path, bool xml);
std::string nsobject_path(void* object);

namespace foundation {

constexpr int64_t kCocoaPropertyListReadCorrupt = 3840;

}  // namespace foundation

using namespace foundation;

void* strings_object_from_json(const std::string& json);

// Every collection the app stores in defaults is a [String].
bool strings_json_from_object(void* object, std::string& json);

namespace foundation {

struct NSDateObject {
    HeapObject header;
    double seconds;
};

}  // namespace foundation

using namespace foundation;

extern void* g_nsdate_class;

void* new_date_object(double seconds);
bool date_from_object(void* object, double& seconds);
std::string* copied(const std::string* text);
void* foundation_make_error(const std::string& message, int64_t code);
bool foundation_parse_iso8601(const std::string& text, double& seconds);
std::string foundation_object_text(void* object);

namespace foundation {

void add_foundation_string_symbols(SymbolTable& table);
void add_foundation_objects_symbols(SymbolTable& table);
void add_foundation_url_symbols(SymbolTable& table);
void add_foundation_session_symbols(SymbolTable& table);
void add_foundation_request_symbols(SymbolTable& table);
void add_foundation_date_symbols(SymbolTable& table);
void add_foundation_web_symbols(SymbolTable& table);
void register_foundation_request_classes();
void register_foundation_web_classes();
void register_foundation_objects_classes();
void register_foundation_date_classes();
void register_foundation_url_classes();
void register_foundation_session_classes();

}  // namespace foundation

std::string percent_decoded(const std::string& text);
// Calls `visit` for each occupied bucket of a native dictionary's storage.
void visit_dictionary_buckets(const char* storage, const std::function<void(const char* keys, const char* values, size_t bucket)>& visit);
