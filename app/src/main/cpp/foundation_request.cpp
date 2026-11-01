#include "base64.h"
#include "foundation_internal.h"
#include "symbols.h"

using namespace foundation;

namespace foundation {

// Errors the app throws itself: a box with the value's type and Error witness table, then the value.
struct GuestErrorBox {
    void* metadata;
    std::atomic<uint64_t> references;
    void* type;
    void* witnesses;
};

constexpr size_t kGuestErrorValueOffset = sizeof(GuestErrorBox);

constexpr size_t kWitnessDestroyOffset = sizeof(void*);

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

// A class like any other, so that a Swift error bridged to NSError can receive messages.
void* guest_error_class() {
    static void* cls = objc_native_class("NSError", destroy_guest_error);
    return cls;
}

struct AllocatedError {
    void* box;
    void* value;
};

SWIFTCC AllocatedError swift_alloc_error(void* type, void* witnesses, void* initial, bool is_take) {
    size_t size;
    memcpy(&size, value_witnesses(type) + kValueWitnessSizeOffset, sizeof size);
    const size_t offset = guest_error_value_offset(type);
    auto* box = static_cast<GuestErrorBox*>(calloc(1, offset + size + 1));
    box->metadata = guest_error_class();
    if (const char* name = foreign_type_name(type)) {
        engine_log("ERROR: thrown value of type %s", name);
    } else {
        // A value type's descriptor follows its kind, and holds its name eight bytes in.
        const char* descriptor = static_cast<const char* const*>(type)[1];
        int32_t name_offset;
        memcpy(&name_offset, descriptor + 8, sizeof name_offset);
        engine_log("ERROR: thrown value of type %s", descriptor + 8 + name_offset);
    }
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
    if (box->metadata == guest_error_class()) {
        *out = {reinterpret_cast<char*>(box) + guest_error_value_offset(box->type), box->type, box->witnesses};
    } else {
        *out = {error, nullptr, nullptr};
    }
}

void* nserror_domain(void* self, const char*) {
    auto* box = static_cast<GuestErrorBox*>(self);
    if (box->metadata != guest_error_class()) return new_nsstring("NSCocoaErrorDomain");
    const char* name = foreign_type_name(box->type);
    return new_nsstring(name != nullptr ? name : "SwiftError");
}

int64_t nserror_code(void* self, const char*) {
    auto* box = static_cast<GuestErrorBox*>(self);
    return box->metadata == guest_error_class() ? 1 : static_cast<ErrorBox*>(self)->code;
}

void* nserror_user_info(void*, const char*) { return collection_bridge_to_objc(resolve_symbol("_swiftEmptyDictionarySingleton")); }

void* nserror_description(void* self, const char*) {
    auto* box = static_cast<GuestErrorBox*>(self);
    return new_nsstring(box->metadata == guest_error_class() ? "The operation could not be completed." : *static_cast<ErrorBox*>(self)->message);
}

void* nserror_nothing(void*, const char*) { return nullptr; }

}  // namespace foundation

SwiftString foundation_error_message(void* error) { return swift_string_from_utf8(*static_cast<foundation::ErrorBox*>(error)->message); }

namespace foundation {

SWIFTCC SwiftString error_localized_description(void* type, void*, SWIFT_CONTEXT void* value) {
    if (type == nullptr) return swift_string_from_utf8(*static_cast<ErrorBox*>(value)->message);
    return swift_string_from_utf8("The operation could not be completed.");
}

SWIFTCC DataValue data_contents_of(URLBox** url, uint64_t, SWIFT_CONTEXT void*, SWIFT_ERROR void** error) {
    const std::string path = url_path(*(*url)->text);
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        *error = make_error("The file could not be read: " + path, ENOENT);
        return {0, kDataEmptyTag};
    }
    std::string bytes((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    return make_data(bytes);
}

SWIFTCC void data_write(URLBox** url, uint64_t, uint64_t range, uint64_t storage, SWIFT_CONTEXT void*, SWIFT_ERROR void** error) {
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

void destroy_url_request(void* raw) {
    auto* box = static_cast<URLRequestBox*>(raw);
    delete box->url;
    delete box->method;
    delete box->headers;
    delete box->body;
    free(box);
}

const HeapMetadata kUrlRequestMetadata = {destroy_url_request, nullptr, 0};

URLRequestBox* new_url_request(const std::string& url, uint64_t cache_policy, double timeout) {
    auto* box = static_cast<URLRequestBox*>(calloc(1, sizeof(URLRequestBox)));
    box->metadata = const_cast<uint64_t*>(&kUrlRequestMetadata.kind);
    new (&box->references) std::atomic<uint64_t>(1);
    box->url = new std::string(url);
    box->method = new std::string("GET");
    box->headers = new std::string();
    box->body = new std::string();
    box->cache_policy = cache_policy;
    box->timeout = timeout;
    return box;
}

SWIFTCC void url_request_init(SWIFT_OUT URLRequestBox** out, URLBox** url, uint64_t cache_policy, double timeout) { *out = new_url_request(*(*url)->text, cache_policy, timeout); }

// URLRequest is a value type: a copy has to stop sharing the box before it changes.
URLRequestBox* unique_request(URLRequestBox** self) {
    URLRequestBox* box = *self;
    if (box->references.load(std::memory_order_relaxed) == 1) return box;
    URLRequestBox* copy = new_url_request(*box->url, box->cache_policy, box->timeout);
    *copy->method = *box->method;
    *copy->headers = *box->headers;
    *copy->body = *box->body;
    copy->has_body = box->has_body;
    rt_swift_release(box);
    *self = copy;
    return copy;
}

std::string lowercased(std::string text) {
    for (char& c : text) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
    return text;
}

// Headers are kept as "Field: value" lines, the form the host's HTTP client reads.
std::vector<std::pair<std::string, std::string>> parsed_headers(const std::string& text) {
    std::vector<std::pair<std::string, std::string>> fields;
    size_t start = 0;
    while (start < text.size()) {
        const size_t end = text.find('\n', start);
        const std::string line = text.substr(start, end == std::string::npos ? std::string::npos : end - start);
        const size_t colon = line.find(": ");
        if (colon != std::string::npos) fields.emplace_back(line.substr(0, colon), line.substr(colon + 2));
        if (end == std::string::npos) break;
        start = end + 1;
    }
    return fields;
}

std::string joined_headers(const std::vector<std::pair<std::string, std::string>>& fields) {
    std::string text;
    for (const auto& [field, value] : fields) text += field + ": " + value + "\n";
    return text;
}

SWIFTCC void url_request_set_method(SwiftString method, SWIFT_CONTEXT URLRequestBox** self) { *unique_request(self)->method = method.object != 0 ? swift_string_to_utf8(method) : "GET"; }
SWIFTCC SwiftString url_request_get_method(SWIFT_CONTEXT URLRequestBox** self) { return swift_string_from_utf8(*(*self)->method); }

SWIFTCC void url_request_set_header(SwiftString value, SwiftString field, SWIFT_CONTEXT URLRequestBox** self) {
    URLRequestBox* box = unique_request(self);
    const std::string name = swift_string_to_utf8(field);
    auto fields = parsed_headers(*box->headers);
    const auto same = std::find_if(fields.begin(), fields.end(), [&](const auto& entry) { return lowercased(entry.first) == lowercased(name); });
    if (value.object == 0) {
        if (same != fields.end()) fields.erase(same);
    } else if (same != fields.end()) {
        same->second = swift_string_to_utf8(value);
    } else {
        fields.emplace_back(name, swift_string_to_utf8(value));
    }
    *box->headers = joined_headers(fields);
}

SWIFTCC SwiftString url_request_get_header(SwiftString field, SWIFT_CONTEXT URLRequestBox** self) {
    const std::string name = lowercased(swift_string_to_utf8(field));
    for (const auto& [key, value] : parsed_headers(*(*self)->headers)) {
        if (lowercased(key) == name) return swift_string_from_utf8(value);
    }
    return {0, 0};
}

SWIFTCC void* url_request_get_all_headers(SWIFT_CONTEXT URLRequestBox** self) {
    const auto fields = parsed_headers(*(*self)->headers);
    if (fields.empty()) return nullptr;
    std::vector<SwiftString> values;
    values.reserve(fields.size());
    std::vector<std::pair<std::string, const void*>> entries;
    for (const auto& [key, value] : fields) {
        values.push_back(swift_string_from_utf8(value));
        entries.emplace_back(key, &values.back());
    }
    return make_string_dictionary(type_system_symbol("$sSSN"), entries);
}

SWIFTCC void url_request_set_all_headers(void* dictionary, SWIFT_CONTEXT URLRequestBox** self) {
    URLRequestBox* box = unique_request(self);
    box->headers->clear();
    if (dictionary == nullptr) return;
    std::vector<std::pair<std::string, std::string>> fields;
    visit_string_dictionary(dictionary, [&](const std::string& key, void* value) { fields.emplace_back(key, swift_string_to_utf8(*static_cast<SwiftString*>(value))); });
    *box->headers = joined_headers(fields);
    rt_swift_bridgeObjectRelease(dictionary);
}

SWIFTCC uint64_t url_request_get_cache_policy(SWIFT_CONTEXT URLRequestBox** self) { return (*self)->cache_policy; }
SWIFTCC void url_request_set_cache_policy(uint64_t policy, SWIFT_CONTEXT URLRequestBox** self) { unique_request(self)->cache_policy = policy; }
SWIFTCC double url_request_get_timeout(SWIFT_CONTEXT URLRequestBox** self) { return (*self)->timeout; }
SWIFTCC void url_request_set_timeout(double timeout, SWIFT_CONTEXT URLRequestBox** self) { unique_request(self)->timeout = timeout; }
SWIFTCC bool url_request_allows_cellular(SWIFT_CONTEXT URLRequestBox**) { return true; }

SWIFTCC void url_request_bridge_from_objc(SWIFT_OUT URLRequestBox** out, void* object) {
    const auto found = bridged_requests().find(object);
    if (found != bridged_requests().end()) {
        rt_swift_retain(found->second);
        *out = found->second;
        return;
    }
    constexpr double kDefaultTimeoutSeconds = 60;
    *out = new_url_request(nsstring_text(object), 0, kDefaultTimeoutSeconds);
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

SWIFTCC void data_append(const char* start, const char* end, SWIFT_CONTEXT DataValue* self) {
    std::string bytes = data_bytes(self->range, self->storage);
    if (start != nullptr) bytes.append(start, static_cast<size_t>(end - start));
    store_data(self, bytes);
}

SWIFTCC void data_replace_subrange(int64_t lower, int64_t upper, const char* replacement, int64_t count, SWIFT_CONTEXT DataValue* self) {
    std::string bytes = data_bytes(self->range, self->storage);
    bytes.replace(static_cast<size_t>(lower), static_cast<size_t>(upper - lower), replacement != nullptr ? std::string(replacement, static_cast<size_t>(count)) : std::string());
    store_data(self, bytes);
}

SWIFTCC void url_request_set_body(uint64_t range, uint64_t storage, SWIFT_CONTEXT URLRequestBox** self) {
    constexpr uint64_t kOptionalDataNil = 0xf000000000000000ULL;
    URLRequestBox* box = unique_request(self);
    box->has_body = storage != kOptionalDataNil;
    *box->body = box->has_body ? data_bytes(range, storage) : std::string();
}

SWIFTCC DataValue url_request_get_body(SWIFT_CONTEXT URLRequestBox** self) {
    constexpr uint64_t kOptionalDataNil = 0xf000000000000000ULL;
    if (!(*self)->has_body) return {0, kOptionalDataNil};
    return make_data(*(*self)->body);
}

SWIFTCC void url_request_get_url(SWIFT_OUT URLBox** out, SWIFT_CONTEXT URLRequestBox** self) { *out = new_url(*(*self)->url); }

// NSHTTPURLResponse: the only part the app reads is the status.
struct HTTPResponseObject {
    HeapObject header;
    int64_t status;
};

void* g_http_response_class = nullptr;

int64_t http_response_status(void* self, const char*) { return static_cast<HTTPResponseObject*>(self)->status; }

std::string g_session_headers;

AsyncDataResult perform_request(const std::string& url, const std::string& method, const std::string& headers, const std::string& body) {
    const HttpResult result = engine_http(url, method, g_session_headers + headers, body);
    engine_log("HTTP: %s %s -> %d, %zu bytes", method.c_str(), url.c_str(), result.status, result.body.size());
    if (result.status < 0) return {{0, kDataEmptyTag}, nullptr, make_error("The network request could not be completed.", 1)};
    auto* response = static_cast<HTTPResponseObject*>(calloc(1, sizeof(HTTPResponseObject)));
    response->header.metadata = g_http_response_class;
    response->header.references = 1;
    response->status = result.status;
    return {make_data(result.body), response, nullptr};
}

extern "C" void rt_url_session_fetch_url(AsyncDataResult* out, URLBox** url) { *out = perform_request(*(*url)->text, "GET", "", ""); }

extern "C" void rt_url_session_fetch_request(AsyncDataResult* out, URLRequestBox** request) {
    *out = perform_request(*(*request)->url, *(*request)->method, *(*request)->headers, *(*request)->body);
}

AsyncFunctionRecord g_url_session_url_record;

AsyncFunctionRecord g_url_session_request_record;

void* g_bridged_collection_class = nullptr;

void destroy_bridged_collection(void* raw) {
    rt_swift_bridgeObjectRelease(static_cast<BridgedCollection*>(raw)->value);
    free(raw);
}

SWIFTCC void* collection_bridge_to_objc(void* value) {
    auto* wrapper = static_cast<BridgedCollection*>(calloc(1, sizeof(BridgedCollection)));
    wrapper->header.metadata = g_bridged_collection_class;
    wrapper->header.references = 1;
    wrapper->value = rt_swift_bridgeObjectRetain(value);
    return wrapper;
}

SWIFTCC void* dictionary_bridge_to_objc(void* value, void* key_type, void* element_type, void*) {
    auto* wrapper = static_cast<BridgedCollection*>(collection_bridge_to_objc(value));
    wrapper->key_type = key_type;
    wrapper->element_type = element_type;
    return wrapper;
}

SWIFTCC void* array_bridge_to_objc(void* value, void* element_type) {
    auto* wrapper = static_cast<BridgedCollection*>(collection_bridge_to_objc(value));
    wrapper->element_type = element_type;
    return wrapper;
}

bool bridged_collection_value(void* object, void*& value) {
    if (object == nullptr || static_cast<HeapObject*>(object)->metadata != g_bridged_collection_class) return false;
    value = static_cast<BridgedCollection*>(object)->value;
    return true;
}

// Format styles are one tagged word here; the only option this app sets is compact notation.
constexpr uint64_t kStyleWord = kLocaleImmediate;

constexpr uint64_t kCompactNotationFlag = 1;

SWIFTCC void integer_style_init(SWIFT_OUT uint64_t* out, const void*, void*) { *out = kStyleWord; }

SWIFTCC void notation_compact_name(SWIFT_OUT uint64_t* out) { *out = kStyleWord | kCompactNotationFlag; }

SWIFTCC void integer_style_notation(SWIFT_OUT uint64_t* out, const uint64_t* notation, void*, SWIFT_CONTEXT const uint64_t* self) { *out = *self | (*notation & kCompactNotationFlag); }

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
SWIFTCC void integer_formatted_style(SWIFT_OUT SwiftString* out, const uint64_t* style, void*, void*, SWIFT_CONTEXT const int64_t* self) {
    const int64_t value = *self;
    *out = swift_string_from_utf8((*style & kCompactNotationFlag) ? compact_number(value) : group_thousands(value));
}

SWIFTCC SwiftString integer_formatted(void*, void*, SWIFT_CONTEXT const int64_t* self) { return swift_string_from_utf8(group_thousands(*self)); }

SWIFTCC void date_now(SWIFT_OUT double* out) {
    struct timespec now{};
    clock_gettime(CLOCK_REALTIME, &now);
    *out = static_cast<double>(now.tv_sec) - kSecondsFrom1970To2001 + static_cast<double>(now.tv_nsec) * 1e-9;
}

SWIFTCC void date_style_word(SWIFT_OUT uint64_t* out) { *out = kStyleWord; }

SWIFTCC SwiftString date_formatted(const void*, const void*, SWIFT_CONTEXT const double* self) {
    const auto seconds = static_cast<time_t>(*self + kSecondsFrom1970To2001);
    struct tm local{};
    localtime_r(&seconds, &local);
    char text[64];
    strftime(text, sizeof text, "%b %e, %Y at %H:%M", &local);
    return swift_string_from_utf8(text);
}

SWIFTCC DataValue data_subdata(int64_t lower, int64_t upper, uint64_t range, uint64_t storage) {
    const std::string bytes = data_bytes(range, storage);
    if (lower < 0 || upper < lower || static_cast<size_t>(upper) > bytes.size()) return make_data({});
    return make_data(bytes.substr(static_cast<size_t>(lower), static_cast<size_t>(upper - lower)));
}

SWIFTCC int64_t data_count(uint64_t range, uint64_t storage) { return static_cast<int64_t>(data_bytes(range, storage).size()); }

SWIFTCC void data_append_data(uint64_t range, uint64_t storage, SWIFT_CONTEXT DataValue* self) {
    *self = make_data(data_bytes(self->range, self->storage) + data_bytes(range, storage));
}

char* copied_data_bytes(uint64_t range, uint64_t storage, size_t& size) {
    const std::string bytes = data_bytes(range, storage);
    size = bytes.size();
    auto* copy = static_cast<char*>(malloc(size + 1));
    memcpy(copy, bytes.data(), size);
    return copy;
}

using UnsafeBytesBody = SWIFTCC void (*)(SWIFT_OUT void*, const void*, const void*, SWIFT_CONTEXT void*, SWIFT_ERROR void**);

// The body sees a copy, which is fine for reading, the only thing this form of the call allows.
SWIFTCC void data_with_unsafe_bytes(SWIFT_OUT void* out, UnsafeBytesBody body, void* context, void*, uint64_t range, uint64_t storage, SWIFT_CONTEXT void*, SWIFT_ERROR void** error) {
    // Plain memory: a local std::string's cleanup around this Swift call doesn't assemble.
    size_t size = 0;
    char* copy = copied_data_bytes(range, storage, size);
    body(out, copy, copy + size, context, error);
    free(copy);
}

SWIFTCC SwiftString data_base64_string(uint64_t, uint64_t range, uint64_t storage) { return swift_string_from_utf8(base64_encode(data_bytes(range, storage))); }

// Nil as Optional<Data> spells it: a storage tag no real value has.
SWIFTCC DataValue data_from_base64(SwiftString text, uint64_t) {
    constexpr uint64_t kOptionalDataNil = 0xf000000000000000ULL;
    std::string out;
    if (!base64_decode_strict(swift_string_to_utf8(text), out)) return {0, kOptionalDataNil};
    return make_data(out);
}

SWIFTCC SwiftString string_from_nsstring(void* object) { return swift_string_from_utf8(nsstring_text(object)); }

SWIFTCC SwiftString string_from_utf8_pointer(const char* text) { return text != nullptr ? swift_string_from_utf8(text) : SwiftString{0, 0}; }

SWIFTCC void* nsstring_from_static(const char* text, int64_t length, uint8_t) { return new_nsstring(std::string(text, static_cast<size_t>(length))); }

// Literals of Any, whose objects are held directly and whose Strings become NSStrings, as bridging would make them.
SWIFTCC void* collection_from_any_literal(void* elements) {
    constexpr size_t kCountOffset = 16;
    int64_t count;
    memcpy(&count, static_cast<char*>(elements) + kCountOffset, sizeof count);
    const auto* values = reinterpret_cast<const AnyValue*>(static_cast<char*>(elements) + kArrayElementsOffset);
    std::vector<void*> items;
    for (int64_t i = 0; i < count; ++i) {
        if (values[i].type == type_system_symbol("$sSSN"))
            items.push_back(new_nsstring(swift_string_to_utf8(*reinterpret_cast<const SwiftString*>(values[i].buffer))));
        else
            items.push_back(reinterpret_cast<void*>(values[i].buffer[0]));
    }
    return foundation_new_array(items);
}

SWIFTCC void* string_bridge_to_objc(SwiftString value) { return new_nsstring(swift_string_to_utf8(value)); }

SWIFTCC SwiftString string_bridge_from_objc(void* object) {
    if (object == nullptr) return swift_string_from_utf8("");
    return swift_string_from_utf8(nsstring_text(object));
}

}  // namespace foundation

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
    auto* header = reinterpret_cast<HeapObject*>(storage);
    header->metadata = const_cast<uint64_t*>(&kStringHeapMetadata.kind);
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

constexpr size_t kAnyHashableStride = 40;

constexpr size_t kAnyStride = 32;

// The session's headers are a [AnyHashable: Any] of strings, read directly from its native storage.
void visit_dictionary_buckets(const char* storage, const std::function<void(const char* keys, const char* values, size_t bucket)>& visit) {
    const int scale = static_cast<int8_t>(storage[kDictionaryScaleOffset]);
    const char* keys;
    const char* values;
    memcpy(&keys, storage + kDictionaryKeysOffset, sizeof keys);
    memcpy(&values, storage + kDictionaryValuesOffset, sizeof values);
    const char* bitmap = storage + kDictionaryBitmapOffset;
    for (size_t bucket = 0; bucket < (1ULL << scale); ++bucket) {
        uint64_t word;
        memcpy(&word, bitmap + bucket / kBitsPerWord * sizeof word, sizeof word);
        if ((word >> (bucket % kBitsPerWord)) & 1) visit(keys, values, bucket);
    }
}

void session_set_headers(void*, const char*, void* wrapper) {
    g_session_headers.clear();
    if (wrapper == nullptr) return;
    const auto bits = reinterpret_cast<uint64_t>(static_cast<BridgedCollection*>(wrapper)->value);
    const auto* storage = reinterpret_cast<const char*>(bits & kBridgePointerMask);
    visit_dictionary_buckets(storage, [](const char* keys, const char* values, size_t bucket) {
        const std::string name = swift_string_to_utf8(*reinterpret_cast<const SwiftString*>(keys + bucket * kAnyHashableStride));
        const std::string value = swift_string_to_utf8(*reinterpret_cast<const SwiftString*>(values + bucket * kAnyStride));
        g_session_headers += name + ": " + value + "\n";
    });
}

SWIFTCC void url_error_code(SWIFT_OUT uint64_t* out) { *out = kStyleWord; }

SWIFTCC void url_error_init(SWIFT_OUT uint64_t* out, const void*, void*) { *out = kStyleWord; }

SWIFTCC void* url_error_ns_error(void*) { return make_error("The server's response could not be used.", 1); }

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

namespace foundation {

void add_foundation_request_symbols(SymbolTable& table) {
    table.insert({
        {"$s10Foundation4DateV3nowACvgZ", reinterpret_cast<void*>(date_now)},
        {"$s10Foundation4DataV10contentsOf7optionsAcA3URLVh_So20NSDataReadingOptionsVtKcfC", reinterpret_cast<void*>(data_contents_of)},
        {"$s10Foundation4DataV5write2to7optionsyAA3URLV_So20NSDataWritingOptionsVtKF", reinterpret_cast<void*>(data_write)},
        {"$s10Foundation4DataV15_RepresentationO6append10contentsOfySW_tF", reinterpret_cast<void*>(data_append)},
        {"$s10Foundation4DataV15_RepresentationO15replaceSubrange_4with5countySnySiG_SVSgSitF", reinterpret_cast<void*>(data_replace_subrange)},
        {"$ss5ErrorP10FoundationE20localizedDescriptionSSvg", reinterpret_cast<void*>(error_localized_description)},
        {"$sSo12NSURLSessionC10FoundationE4data3for8delegateAC4DataV_So13NSURLResponseCtAC10URLRequestV_So0A12TaskDelegate_pSgtYaKF", reinterpret_cast<void*>(rt_url_session_data_request)},
        {"$sSo12NSURLSessionC10FoundationE4data4from8delegateAC4DataV_So13NSURLResponseCtAC3URLV_So0A12TaskDelegate_pSgtYaKF", reinterpret_cast<void*>(rt_url_session_data_url)},
        {"$s10Foundation10URLRequestV3url11cachePolicy15timeoutIntervalAcA3URLV_So017NSURLRequestCacheE0VSdtcfC", reinterpret_cast<void*>(url_request_init)},
        {"$s10Foundation10URLRequestV10httpMethodSSSgvs", reinterpret_cast<void*>(url_request_set_method)},
        {"$s10Foundation10URLRequestV10httpMethodSSSgvg", reinterpret_cast<void*>(url_request_get_method)},
        {"$s10Foundation10URLRequestV11cachePolicySo017NSURLRequestCacheD0Vvg", reinterpret_cast<void*>(url_request_get_cache_policy)},
        {"$s10Foundation10URLRequestV11cachePolicySo017NSURLRequestCacheD0Vvs", reinterpret_cast<void*>(url_request_set_cache_policy)},
        {"$s10Foundation10URLRequestV15timeoutIntervalSdvg", reinterpret_cast<void*>(url_request_get_timeout)},
        {"$s10Foundation10URLRequestV15timeoutIntervalSdvs", reinterpret_cast<void*>(url_request_set_timeout)},
        {"$s10Foundation10URLRequestV19allHTTPHeaderFieldsSDyS2SGSgvg", reinterpret_cast<void*>(url_request_get_all_headers)},
        {"$s10Foundation10URLRequestV19allHTTPHeaderFieldsSDyS2SGSgvs", reinterpret_cast<void*>(url_request_set_all_headers)},
        {"$s10Foundation10URLRequestV20allowsCellularAccessSbvg", reinterpret_cast<void*>(url_request_allows_cellular)},
        {"$s10Foundation10URLRequestV36_unconditionallyBridgeFromObjectiveCyACSo12NSURLRequestCSgFZ", reinterpret_cast<void*>(url_request_bridge_from_objc)},
        {"$s10Foundation10URLRequestV5value18forHTTPHeaderFieldSSSgSS_tF", reinterpret_cast<void*>(url_request_get_header)},
        {"$s10Foundation10URLRequestV8httpBodyAA4DataVSgvg", reinterpret_cast<void*>(url_request_get_body)},
        {"$s10Foundation10URLRequestV8setValue_18forHTTPHeaderFieldySSSg_SStF", reinterpret_cast<void*>(url_request_set_header)},
        {"$s10Foundation10URLRequestV8httpBodyAA4DataVSgvs", reinterpret_cast<void*>(url_request_set_body)},
        {"$s10Foundation10URLRequestV3urlAA3URLVSgvg", reinterpret_cast<void*>(url_request_get_url)},
        {"$sSD10FoundationE19_bridgeToObjectiveCSo12NSDictionaryCyF", reinterpret_cast<void*>(dictionary_bridge_to_objc)},
        {"$sSa10FoundationE19_bridgeToObjectiveCSo7NSArrayCyF", reinterpret_cast<void*>(array_bridge_to_objc)},
        {"swift_allocError", reinterpret_cast<void*>(swift_alloc_error)},
        {"$s10Foundation4DataV7subdata2inACSnySiG_tF", reinterpret_cast<void*>(data_subdata)},
        {"$s10Foundation4DataV5countSivg", reinterpret_cast<void*>(data_count)},
        {"$s10Foundation4DataV6appendyyACF", reinterpret_cast<void*>(data_append_data)},
        {"$s10Foundation4DataV15withUnsafeBytesyxxSWKXEKlF", reinterpret_cast<void*>(data_with_unsafe_bytes)},
        {"$s10Foundation4DataV19base64EncodedString7optionsSSSo27NSDataBase64EncodingOptionsV_tF", reinterpret_cast<void*>(data_base64_string)},
        {"$s10Foundation4DataV13base64Encoded7optionsACSgSSh_So27NSDataBase64DecodingOptionsVtcfC", reinterpret_cast<void*>(data_from_base64)},
        {"$sSS10FoundationEySSSo8NSStringCcfC", reinterpret_cast<void*>(string_from_nsstring)},
        {"$sSS10FoundationE10utf8StringSSSgSPys4Int8VG_tcfC", reinterpret_cast<void*>(string_from_utf8_pointer)},
        {"$sSo8NSStringC10FoundationE13stringLiteralABs12StaticStringV_tcfC", reinterpret_cast<void*>(nsstring_from_static)},
        {"$sSo7NSArrayC10FoundationE12arrayLiteralABypd_tcfC", reinterpret_cast<void*>(collection_from_any_literal)},
        {"$sSo5NSSetC10FoundationE12arrayLiteralABypd_tcfC", reinterpret_cast<void*>(collection_from_any_literal)},
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
        {"$s10Foundation8URLErrorV4CodeV17badServerResponseAEvgZ", reinterpret_cast<void*>(url_error_code)},
        {"$s10Foundation8URLErrorV4CodeV19cannotParseResponseAEvgZ", reinterpret_cast<void*>(url_error_code)},
        {"$s10Foundation21_BridgedStoredNSErrorPAAE_8userInfox4CodeQz_SDySSypGtcfC", reinterpret_cast<void*>(url_error_init)},
        {"$s10Foundation8URLErrorV8_nsErrorSo7NSErrorCvg", reinterpret_cast<void*>(url_error_ns_error)},
        {"$sSS10FoundationE19_bridgeToObjectiveCSo8NSStringCyF", reinterpret_cast<void*>(string_bridge_to_objc)},
        {"$sSS10FoundationE36_unconditionallyBridgeFromObjectiveCySSSo8NSStringCSgFZ", reinterpret_cast<void*>(string_bridge_from_objc)},
    });
}

void register_foundation_request_classes() {
    objc_register_method("-[NSError domain]", reinterpret_cast<const void*>(nserror_domain));
    objc_register_method("-[NSError code]", reinterpret_cast<const void*>(nserror_code));
    objc_register_method("-[NSError userInfo]", reinterpret_cast<const void*>(nserror_user_info));
    objc_register_method("-[NSError description]", reinterpret_cast<const void*>(nserror_description));
    objc_register_method("-[NSError debugDescription]", reinterpret_cast<const void*>(nserror_description));
    objc_register_method("-[NSError localizedDescription]", reinterpret_cast<const void*>(nserror_description));
    objc_register_method("-[NSError localizedFailureReason]", reinterpret_cast<const void*>(nserror_nothing));
    objc_register_method("-[NSError localizedRecoverySuggestion]", reinterpret_cast<const void*>(nserror_nothing));
    objc_register_method("-[NSError underlyingErrors]", reinterpret_cast<const void*>(nserror_nothing));
    objc_register_method("-[NSHTTPURLResponse statusCode]", reinterpret_cast<const void*>(http_response_status));
    objc_register_method("-[NSURLSessionConfiguration setHTTPAdditionalHeaders:]", reinterpret_cast<const void*>(session_set_headers));
}

}  // namespace foundation
