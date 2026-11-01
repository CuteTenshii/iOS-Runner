#include "foundation_internal.h"
#include "uikit_internal.h"
#include "symbols.h"

#include <cmath>

using namespace foundation;

void* g_nsdate_class = nullptr;

void* new_date_object(double seconds) {
    auto* object = static_cast<NSDateObject*>(calloc(1, sizeof(NSDateObject)));
    object->header.metadata = g_nsdate_class;
    object->header.references = 1;
    object->seconds = seconds;
    return object;
}

bool date_from_object(void* object, double& seconds) {
    if (static_cast<HeapObject*>(object)->metadata != g_nsdate_class) return false;
    seconds = static_cast<NSDateObject*>(object)->seconds;
    return true;
}

SWIFTCC void* date_bridge_to_objc(SWIFT_CONTEXT const double* self) { return new_date_object(*self); }

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
    object->header.metadata = g_json_value_class;
    object->header.references = 1;
    object->value = value;
    return object;
}

SWIFTCC void* dictionary_bridge_from_objc(void* object) {
    if (void* value; bridged_collection_value(object, value)) return rt_swift_bridgeObjectRetain(value);
    if (object != nullptr && static_cast<HeapObject*>(object)->metadata == g_json_value_class) {
        AnyValue any;
        json_to_any(*static_cast<JSONValueObject*>(object)->value, &any);
        return reinterpret_cast<void*>(any.buffer[0]);
    }
    return make_string_dictionary(any_type(), {});
}

// A Calendar is an immediate word: only the Gregorian calendar in the device's time zone is implemented.
constexpr uint64_t kCalendarImmediate = 1ULL << 63;

SWIFTCC void calendar_current(SWIFT_OUT uint64_t* out) { *out = kCalendarImmediate; }
SWIFTCC void calendar_with_identifier(SWIFT_OUT uint64_t* out, const void*) { *out = kCalendarImmediate; }

constexpr size_t kComponentCount = 7;
enum ComponentField { kYear,
                      kMonth,
                      kDay,
                      kHour,
                      kMinute,
                      kSecond,
                      kNanosecond };

struct DateComponentsObject {
    HeapObject header;
    int64_t fields[kComponentCount];
};

void* g_date_components_class = nullptr;

time_t seconds_of(const double* date) { return static_cast<time_t>(floor(*date + kSecondsFrom1970To2001)); }

void fill_components(int64_t* fields, double date) {
    const time_t seconds = seconds_of(&date);
    tm parts{};
    localtime_r(&seconds, &parts);
    fields[kYear] = parts.tm_year + 1900;
    fields[kMonth] = parts.tm_mon + 1;
    fields[kDay] = parts.tm_mday;
    fields[kHour] = parts.tm_hour;
    fields[kMinute] = parts.tm_min;
    fields[kSecond] = parts.tm_sec;
    const double whole = floor(date);
    fields[kNanosecond] = static_cast<int64_t>((date - whole) * 1e9);
}

DateComponentsObject* new_date_components() {
    if (g_date_components_class == nullptr) g_date_components_class = objc_native_class("NSDateComponents", free);
    auto* object = static_cast<DateComponentsObject*>(calloc(1, sizeof(DateComponentsObject)));
    object->header.metadata = g_date_components_class;
    object->header.references = 1;
    return object;
}

// The requested set is ignored: every field is filled, and code only reads the ones it asked for.
SWIFTCC void calendar_components_from(SWIFT_OUT DateComponentsObject** out, void*, const double* date, SWIFT_CONTEXT void*) {
    *out = new_date_components();
    fill_components((*out)->fields, *date);
}

SWIFTCC void calendar_components_between(SWIFT_OUT DateComponentsObject** out, void*, const double* from, const double* to, SWIFT_CONTEXT void*) {
    *out = new_date_components();
    auto delta = static_cast<int64_t>(*to - *from);
    (*out)->fields[kDay] = delta / 86400;
    delta %= 86400;
    (*out)->fields[kHour] = delta / 3600;
    delta %= 3600;
    (*out)->fields[kMinute] = delta / 60;
    (*out)->fields[kSecond] = delta % 60;
}

struct OptionalInt {
    int64_t value;
    uint64_t is_nil;
};

template <ComponentField field>
SWIFTCC OptionalInt components_field(SWIFT_CONTEXT DateComponentsObject** self) { return {(*self)->fields[field], 0}; }

int component_field_of(const uint64_t* component) {
    static const char* const kNames[kComponentCount] = {"$s10Foundation8CalendarV9ComponentO4yearyA2EmFWC", "$s10Foundation8CalendarV9ComponentO5monthyA2EmFWC", "$s10Foundation8CalendarV9ComponentO3dayyA2EmFWC", "$s10Foundation8CalendarV9ComponentO4houryA2EmFWC", "$s10Foundation8CalendarV9ComponentO6minuteyA2EmFWC", "$s10Foundation8CalendarV9ComponentO6secondyA2EmFWC", "$s10Foundation8CalendarV9ComponentO10nanosecondyA2EmFWC"};
    for (size_t i = 0; i < kComponentCount; ++i) {
        const auto* tag = static_cast<const int32_t*>(resolve_symbol(kNames[i]));
        if (static_cast<uint64_t>(*tag) == *component) return static_cast<int>(i);
    }
    return -1;
}

SWIFTCC int64_t calendar_component(const uint64_t* component, const double* date, SWIFT_CONTEXT void*) {
    int64_t fields[kComponentCount];
    fill_components(fields, *date);
    const int field = component_field_of(component);
    return field < 0 ? 0 : fields[field];
}

double start_of_day(double date) {
    const time_t seconds = seconds_of(&date);
    tm parts{};
    localtime_r(&seconds, &parts);
    parts.tm_hour = parts.tm_min = parts.tm_sec = 0;
    return static_cast<double>(mktime(&parts)) - kSecondsFrom1970To2001;
}

SWIFTCC void calendar_start_of_day(SWIFT_OUT double* out, const double* date, SWIFT_CONTEXT void*) { *out = start_of_day(*date); }

SWIFTCC bool calendar_is_today(const double* date, SWIFT_CONTEXT void*) {
    const double now = static_cast<double>(time(nullptr)) - kSecondsFrom1970To2001;
    return start_of_day(*date) == start_of_day(now);
}

struct OptionalDate {
    double value;
    uint8_t is_nil;
};

// Normalizes through mktime, which carries overflowing fields into the larger ones.
SWIFTCC void calendar_date_adding(SWIFT_OUT OptionalDate* out, const uint64_t* component, int64_t value, const double* date, bool, SWIFT_CONTEXT void*) {
    const time_t seconds = seconds_of(date);
    tm parts{};
    localtime_r(&seconds, &parts);
    const double fraction = *date - floor(*date);
    double nanoseconds = 0;
    switch (component_field_of(component)) {
        case kYear:
            parts.tm_year += static_cast<int>(value);
            break;
        case kMonth:
            parts.tm_mon += static_cast<int>(value);
            break;
        case kDay:
            parts.tm_mday += static_cast<int>(value);
            break;
        case kHour:
            parts.tm_hour += static_cast<int>(value);
            break;
        case kMinute:
            parts.tm_min += static_cast<int>(value);
            break;
        case kSecond:
            parts.tm_sec += static_cast<int>(value);
            break;
        case kNanosecond:
            nanoseconds = static_cast<double>(value) / 1e9;
            break;
        default:
            out->is_nil = 1;
            return;
    }
    parts.tm_isdst = -1;
    out->value = static_cast<double>(mktime(&parts)) - kSecondsFrom1970To2001 + fraction + nanoseconds;
    out->is_nil = 0;
}

SWIFTCC void byte_count_units_all(SWIFT_OUT uint64_t* out) { *out = UINT64_MAX; }

SWIFTCC void byte_count_style(SWIFT_OUT uint64_t* out, const void*, const void*, bool, bool) { *out = 1ULL << 63; }

// Decimal units, as the .file style shows them: one digit once past bytes.
std::string byte_count_text(int64_t count) {
    static constexpr const char* kUnits[] = {"bytes", "KB", "MB", "GB", "TB", "PB"};
    constexpr double kUnitStep = 1000;
    double value = static_cast<double>(count);
    size_t unit = 0;
    while (fabs(value) >= kUnitStep && unit + 1 < std::size(kUnits)) {
        value /= kUnitStep;
        ++unit;
    }
    char text[64];
    if (unit == 0)
        snprintf(text, sizeof text, "%lld bytes", static_cast<long long>(count));
    else
        snprintf(text, sizeof text, "%.1f %s", value, kUnits[unit]);
    return text;
}

SWIFTCC void integer_formatted_byte_count(SWIFT_OUT SwiftString* out, const void*, void* type, void* style_type, void*, void*, void*, SWIFT_CONTEXT const void* self) {
    const char* style_name = foreign_type_name(style_type);
    if (style_name == nullptr || strcmp(style_name, "_$s10Foundation20ByteCountFormatStyleVMn") != 0) {
        engine_log("STOP: formatted(_:) with a format style other than ByteCountFormatStyle: %s", style_name != nullptr ? style_name : "(app type)");
        abort();
    }
    int64_t count = 0;
    memcpy(&count, self, witnesses_of(type)->size < sizeof count ? witnesses_of(type)->size : sizeof count);
    *out = swift_string_from_utf8(byte_count_text(count));
}

std::map<std::string, void*>& value_transformers() {
    static std::map<std::string, void*> transformers;
    return transformers;
}

void value_transformer_register(void*, const char*, void* transformer, void* name) {
    rt_swift_retain(transformer);
    value_transformers()[nsstring_text(name)] = transformer;
}

void* value_transformer_named(void*, const char*, void* name) {
    const auto found = value_transformers().find(nsstring_text(name));
    return found == value_transformers().end() ? nullptr : found->second;
}

// Ordered sets and sets are NSArrays underneath, without their duplicates.
void* unique_collection_from_array(void*, const char*, void* source) {
    std::vector<void*> unique;
    for (void* item : uikit::object_array_items(source)) {
        if (std::find(unique.begin(), unique.end(), item) == unique.end()) unique.push_back(item);
    }
    return foundation_new_array(unique);
}

void* collection_as_array(void* self, const char*) { return self; }

std::vector<void*>& array_items_of(void* self) { return *static_cast<NSArrayObject*>(self)->items; }

int64_t array_count(void* self, const char*) { return static_cast<int64_t>(array_items_of(self).size()); }
void* array_object_at(void* self, const char*, int64_t index) { return array_items_of(self).at(static_cast<size_t>(index)); }
void* array_first(void* self, const char*) { return array_items_of(self).empty() ? nullptr : array_items_of(self).front(); }
void* array_last(void* self, const char*) { return array_items_of(self).empty() ? nullptr : array_items_of(self).back(); }

bool array_contains(void* self, const char*, void* object) {
    const auto& items = array_items_of(self);
    return std::find(items.begin(), items.end(), object) != items.end();
}

int64_t array_index_of(void* self, const char*, void* object) {
    constexpr int64_t kNotFound = INT64_MAX;
    const auto& items = array_items_of(self);
    const auto found = std::find(items.begin(), items.end(), object);
    return found == items.end() ? kNotFound : static_cast<int64_t>(found - items.begin());
}

// NSFastEnumerationState: hands out the whole array at once from its own storage.
struct FastEnumerationState {
    uint64_t state;
    void** items;
    uint64_t* mutations;
    uint64_t extra[5];
};

uint64_t array_fast_enumerate(void* self, const char*, FastEnumerationState* state, void**, uint64_t) {
    if (state->state != 0) return 0;
    auto& items = array_items_of(self);
    state->state = 1;
    state->items = items.data();
    state->mutations = state->extra;
    return items.size();
}

// A fast enumeration iterator is a box holding the collection and how far it has got.
struct FastIterator {
    HeapObject header;
    void* collection;
    size_t position;
};

SWIFTCC void fast_iterator_make(SWIFT_OUT FastIterator** out, SWIFT_CONTEXT void* self) {
    auto* iterator = static_cast<FastIterator*>(objc_framework_object("__FastEnumerationIterator"));
    iterator->collection = rt_swift_retain(self);
    *out = iterator;
}

SWIFTCC void fast_iterator_next(SWIFT_OUT AnyValue* out, SWIFT_CONTEXT FastIterator** self) {
    memset(out, 0, sizeof *out);
    FastIterator* iterator = *self;
    const auto& items = array_items_of(iterator->collection);
    if (iterator->position >= items.size()) return;
    void* object = items[iterator->position++];
    out->buffer[0] = reinterpret_cast<uint64_t>(rt_swift_retain(object));
    out->type = *static_cast<void**>(object);
}

std::vector<void*> objects_in_set(void* object) {
    if (object == nullptr || static_cast<HeapObject*>(object)->metadata != g_nsarray_class) return {};
    return *static_cast<NSArrayObject*>(object)->items;
}

// NSSets here are NSArrays underneath, and anything else, such as a scene's connection options, holds nothing.
SWIFTCC void* set_bridge_from_objc(void* object, void* type, void* hashable) { return set_from_objects(objects_in_set(object), type, hashable); }

SWIFTCC void set_force_bridge(void* object, void** result, void* type, void* hashable) { *result = set_from_objects(objects_in_set(object), type, hashable); }

// NSDictionary's literal, of (Any, Any) pairs. Keys here are always strings, in Swift or Objective-C form.
SWIFTCC void* nsdictionary_literal(void* pairs) {
    constexpr size_t kPairSize = 2 * sizeof(AnyValue), kCountOffset = 16;
    int64_t count;
    memcpy(&count, static_cast<char*>(pairs) + kCountOffset, sizeof count);
    std::vector<std::pair<std::string, const void*>> entries;
    auto* cursor = static_cast<char*>(pairs) + kArrayElementsOffset;
    for (int64_t i = 0; i < count; ++i, cursor += kPairSize) {
        const auto* key = reinterpret_cast<const AnyValue*>(cursor);
        std::string text;
        if (key->type == type_system_symbol("$sSSN"))
            text = swift_string_to_utf8(*reinterpret_cast<const SwiftString*>(key->buffer));
        else
            text = foundation_object_text(reinterpret_cast<void*>(key->buffer[0]));
        entries.emplace_back(text, cursor + sizeof(AnyValue));
    }
    void* storage = make_string_dictionary(any_type(), entries);
    return collection_bridge_to_objc(storage);
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
    object->header.metadata = g_json_value_class;
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

const HeapMetadata kQueryItemMetadata = {destroy_query_item, nullptr, 0};

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

SWIFTCC SwiftString query_item_name(SWIFT_CONTEXT QueryItemBox** self) { return swift_string_from_utf8(*(*self)->name); }

SWIFTCC SwiftString query_item_value(SWIFT_CONTEXT QueryItemBox** self) {
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

const HeapMetadata kComponentsMetadata = {destroy_components, nullptr, 0};

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

SWIFTCC void components_url(SWIFT_OUT URLBox** out, SWIFT_CONTEXT ComponentsBox** self) {
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

SWIFTCC SwiftString components_scheme(SWIFT_CONTEXT ComponentsBox** self) { return optional_text((*self)->scheme); }

SWIFTCC SwiftString components_path(SWIFT_CONTEXT ComponentsBox** self) { return swift_string_from_utf8(percent_decoded(*(*self)->path)); }

SWIFTCC SwiftString components_host(SWIFT_CONTEXT ComponentsBox** self) {
    if ((*self)->authority == nullptr) return {0, 0};
    std::string host = *(*self)->authority;
    if (const size_t at = host.rfind('@'); at != std::string::npos) host.erase(0, at + 1);
    if (const size_t colon = host.rfind(':'); colon != std::string::npos) host.resize(colon);
    return swift_string_from_utf8(percent_decoded(host));
}

SWIFTCC void* components_query_items(SWIFT_CONTEXT ComponentsBox** self) {
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

SWIFTCC void components_empty(SWIFT_OUT ComponentsBox** out) { *out = new_components(); }

struct ComponentsPort {
    int64_t value;
    uint64_t is_nil;
};

SWIFTCC ComponentsPort components_port(SWIFT_CONTEXT ComponentsBox** self) {
    if ((*self)->authority == nullptr) return {0, 1};
    const std::string& authority = *(*self)->authority;
    const size_t colon = authority.rfind(':');
    if (colon == std::string::npos || colon + 1 == authority.size() || authority.find('@', colon) != std::string::npos) return {0, 1};
    return {strtoll(authority.c_str() + colon + 1, nullptr, 10), 0};
}

SWIFTCC SwiftString components_query(SWIFT_CONTEXT ComponentsBox** self) {
    if ((*self)->query == nullptr) return {0, 0};
    return swift_string_from_utf8(percent_decoded(*(*self)->query));
}

// A copy, for the same reason as components_set_query_items.
ComponentsBox* copied_components(const ComponentsBox* old) {
    auto* box = new_components();
    box->scheme = copied(old->scheme);
    box->authority = copied(old->authority);
    *box->path = *old->path;
    box->query = copied(old->query);
    box->fragment = copied(old->fragment);
    return box;
}

void replace_components(ComponentsBox** self, ComponentsBox* box) {
    ComponentsBox* old = *self;
    *self = box;
    rt_swift_release(old);
}

SWIFTCC void components_set_scheme(SwiftString value, SWIFT_CONTEXT ComponentsBox** self) {
    auto* box = copied_components(*self);
    delete box->scheme;
    box->scheme = value.object != 0 ? new std::string(swift_string_to_utf8(value)) : nullptr;
    replace_components(self, box);
}

SWIFTCC void components_set_host(SwiftString value, SWIFT_CONTEXT ComponentsBox** self) {
    auto* box = copied_components(*self);
    const ComponentsPort port = components_port(self);
    delete box->authority;
    box->authority = nullptr;
    if (value.object != 0) {
        std::string authority = swift_string_to_utf8(value);
        if (!port.is_nil) authority += ":" + std::to_string(port.value);
        box->authority = new std::string(authority);
    }
    replace_components(self, box);
}

// A new box each time, since URLComponents is a value and a copy may share the old one.
SWIFTCC void components_set_query_items(void* items, SWIFT_CONTEXT ComponentsBox** self) {
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

SWIFTCC SwiftString string_trimming(const uint64_t* set, void* type, void*, SWIFT_CONTEXT const SwiftString* self) {
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
// Nil, as a null box, unless the text is 32 hex digits in 8-4-4-4-12 groups.
SWIFTCC void uuid_from_string(SWIFT_OUT URLBox** out, SwiftString text) {
    constexpr size_t kUuidLength = 36;
    std::string value = swift_string_to_utf8(text);
    bool valid = value.size() == kUuidLength;
    for (size_t i = 0; valid && i < value.size(); ++i) {
        const bool dash_here = i == 8 || i == 13 || i == 18 || i == 23;
        valid = dash_here ? value[i] == '-' : isxdigit(static_cast<unsigned char>(value[i])) != 0;
    }
    if (!valid) {
        *out = nullptr;
        return;
    }
    for (char& c : value) c = static_cast<char>(toupper(static_cast<unsigned char>(c)));
    *out = new_url(value);
}

SWIFTCC void uuid_init(SWIFT_OUT URLBox** out) {
    uint8_t bytes[16];
    arc4random_buf(bytes, sizeof bytes);
    bytes[6] = (bytes[6] & 0x0F) | 0x40;
    bytes[8] = (bytes[8] & 0x3F) | 0x80;
    char text[37];
    snprintf(text, sizeof text, "%02X%02X%02X%02X-%02X%02X-%02X%02X-%02X%02X-%02X%02X%02X%02X%02X%02X", bytes[0], bytes[1], bytes[2], bytes[3], bytes[4], bytes[5], bytes[6], bytes[7], bytes[8], bytes[9], bytes[10], bytes[11], bytes[12], bytes[13], bytes[14], bytes[15]);
    *out = new_url(text);
}

SWIFTCC SwiftString uuid_string(SWIFT_CONTEXT URLBox** self) { return swift_string_from_utf8(*(*self)->text); }

void stdlib_random(void* buffer, size_t count) { arc4random_buf(buffer, count); }

// Puts an Objective-C object into an Any as its Swift value, as the runtime's bridging does.
SWIFTCC void bridge_any_object_to_any(SWIFT_OUT AnyValue* out, void* object) {
    memset(out, 0, sizeof *out);
    const void* isa = object != nullptr ? static_cast<HeapObject*>(object)->metadata : nullptr;
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

namespace foundation {

void add_foundation_date_symbols(SymbolTable& table) {
    table.insert({
        {"$s10Foundation12URLQueryItemV4name5valueACSSh_SSSghtcfC", reinterpret_cast<void*>(query_item_init)},
        {"$s10Foundation12URLQueryItemV4nameSSvg", reinterpret_cast<void*>(query_item_name)},
        {"$s10Foundation12URLQueryItemV5valueSSSgvg", reinterpret_cast<void*>(query_item_value)},
        {"$s10Foundation13URLComponentsV3url23resolvingAgainstBaseURLACSgAA0G0Vh_SbtcfC", reinterpret_cast<void*>(components_from_url)},
        {"$s10Foundation13URLComponentsV6stringACSgSSh_tcfC", reinterpret_cast<void*>(components_from_string)},
        {"$s10Foundation13URLComponentsVACycfC", reinterpret_cast<void*>(components_empty)},
        {"$s10Foundation13URLComponentsV4portSiSgvg", reinterpret_cast<void*>(components_port)},
        {"$s10Foundation13URLComponentsV5querySSSgvg", reinterpret_cast<void*>(components_query)},
        {"$s10Foundation13URLComponentsV6schemeSSSgvs", reinterpret_cast<void*>(components_set_scheme)},
        {"$s10Foundation13URLComponentsV4hostSSSgvs", reinterpret_cast<void*>(components_set_host)},
        {"$s10Foundation13URLComponentsV3urlAA3URLVSgvg", reinterpret_cast<void*>(components_url)},
        {"$s10Foundation13URLComponentsV6schemeSSSgvg", reinterpret_cast<void*>(components_scheme)},
        {"$s10Foundation13URLComponentsV4hostSSSgvg", reinterpret_cast<void*>(components_host)},
        {"$s10Foundation13URLComponentsV4pathSSvg", reinterpret_cast<void*>(components_path)},
        {"$s10Foundation13URLComponentsV10queryItemsSayAA12URLQueryItemVGSgvg", reinterpret_cast<void*>(components_query_items)},
        {"$s10Foundation13URLComponentsV10queryItemsSayAA12URLQueryItemVGSgvs", reinterpret_cast<void*>(components_set_query_items)},
        {"$s10Foundation8CalendarV4date8byAdding5value2to18wrappingComponentsAA4DateVSgAC9ComponentO_SiAJSbtF", reinterpret_cast<void*>(calendar_date_adding)},
        {"$s10Foundation20ByteCountFormatStyleV5UnitsV3allAEvgZ", reinterpret_cast<void*>(byte_count_units_all)},
        {"$s10Foundation11FormatStylePA2A09ByteCountbC0VRszrlE04byteE05style12allowedUnits13spellsOutZero014includesActualdE0A2E0C0O_AE0I0VS2btFZ", reinterpret_cast<void*>(byte_count_style)},
        {"$sSz10FoundationE9formattedy12FormatOutputQyd__qd__AA0C5StyleRd__Sz0C5InputRpd__lF", reinterpret_cast<void*>(integer_formatted_byte_count)},
        {"$sSo12NSOrderedSetC10FoundationE12makeIteratorAC017NSFastEnumerationE0VyF", reinterpret_cast<void*>(fast_iterator_make)},
        {"$sSo5NSSetC10FoundationE12makeIteratorAC017NSFastEnumerationD0VyF", reinterpret_cast<void*>(fast_iterator_make)},
        {"$sSo7NSArrayC10FoundationE12makeIteratorAC017NSFastEnumerationD0VyF", reinterpret_cast<void*>(fast_iterator_make)},
        {"$s10Foundation25NSFastEnumerationIteratorV4nextypSgyF", reinterpret_cast<void*>(fast_iterator_next)},
        {"$sSh10FoundationE26_forceBridgeFromObjectiveC_6resultySo5NSSetC_ShyxGSgztFZ", reinterpret_cast<void*>(set_force_bridge)},
        {"$sSh10FoundationE36_unconditionallyBridgeFromObjectiveCyShyxGSo5NSSetCSgFZ", reinterpret_cast<void*>(set_bridge_from_objc)},
        {"$sSo12NSDictionaryC10FoundationE17dictionaryLiteralAByp_yptd_tcfC", reinterpret_cast<void*>(nsdictionary_literal)},
        {"$s10Foundation8CalendarV7currentACvgZ", reinterpret_cast<void*>(calendar_current)},
        {"$s10Foundation8CalendarV10identifierA2C10IdentifierOh_tcfC", reinterpret_cast<void*>(calendar_with_identifier)},
        {"$s10Foundation8CalendarV14dateComponents_4fromAA04DateD0VShyAC9ComponentOG_AA0F0VtF", reinterpret_cast<void*>(calendar_components_from)},
        {"$s10Foundation8CalendarV14dateComponents_4from2toAA04DateD0VShyAC9ComponentOG_AA0G0VAMtF", reinterpret_cast<void*>(calendar_components_between)},
        {"$s10Foundation8CalendarV9component_4fromSiAC9ComponentO_AA4DateVtF", reinterpret_cast<void*>(calendar_component)},
        {"$s10Foundation8CalendarV10startOfDay3forAA4DateVAG_tF", reinterpret_cast<void*>(calendar_start_of_day)},
        {"$s10Foundation8CalendarV13isDateInTodayySbAA0D0VF", reinterpret_cast<void*>(calendar_is_today)},
        {"$s10Foundation14DateComponentsV4yearSiSgvg", reinterpret_cast<void*>(components_field<kYear>)},
        {"$s10Foundation14DateComponentsV5monthSiSgvg", reinterpret_cast<void*>(components_field<kMonth>)},
        {"$s10Foundation14DateComponentsV3daySiSgvg", reinterpret_cast<void*>(components_field<kDay>)},
        {"$s10Foundation14DateComponentsV4hourSiSgvg", reinterpret_cast<void*>(components_field<kHour>)},
        {"$s10Foundation14DateComponentsV6minuteSiSgvg", reinterpret_cast<void*>(components_field<kMinute>)},
        {"$s10Foundation14DateComponentsV6secondSiSgvg", reinterpret_cast<void*>(components_field<kSecond>)},
        {"$s10Foundation14DateComponentsV10nanosecondSiSgvg", reinterpret_cast<void*>(components_field<kNanosecond>)},
        {"$s10Foundation4UUIDV10uuidStringACSgSSh_tcfC", reinterpret_cast<void*>(uuid_from_string)},
        {"$s10Foundation4UUIDVACycfC", reinterpret_cast<void*>(uuid_init)},
        {"$s10Foundation4UUIDV10uuidStringSSvg", reinterpret_cast<void*>(uuid_string)},
        {"swift_stdlib_random", reinterpret_cast<void*>(stdlib_random)},
        {"$s10Foundation4DateV19_bridgeToObjectiveCSo6NSDateCyF", reinterpret_cast<void*>(date_bridge_to_objc)},
        {"$sSD10FoundationE36_unconditionallyBridgeFromObjectiveCySDyxq_GSo12NSDictionaryCSgFZ", reinterpret_cast<void*>(dictionary_bridge_from_objc)},
        {"$s10Foundation22_convertNSErrorToErrorys0E0_pSo0C0CSgF", reinterpret_cast<void*>(error_identity)},
        {"$s10Foundation22_convertErrorToNSErrorySo0E0Cs0C0_pF", reinterpret_cast<void*>(error_identity)},
        {"$ss018_bridgeAnyObjectToB0yypyXlSgF", reinterpret_cast<void*>(bridge_any_object_to_any)},
    });
}

void register_foundation_date_classes() {
    objc_register_method("-[NSOrderedSet initWithArray:]", reinterpret_cast<const void*>(unique_collection_from_array));
    objc_register_method("+[NSOrderedSet orderedSetWithArray:]", reinterpret_cast<const void*>(unique_collection_from_array));
    objc_register_method("-[NSSet initWithArray:]", reinterpret_cast<const void*>(unique_collection_from_array));
    objc_register_method("+[NSSet setWithArray:]", reinterpret_cast<const void*>(unique_collection_from_array));
    objc_register_method("-[NSArray count]", reinterpret_cast<const void*>(array_count));
    objc_register_method("-[NSArray objectAtIndex:]", reinterpret_cast<const void*>(array_object_at));
    objc_register_method("-[NSArray objectAtIndexedSubscript:]", reinterpret_cast<const void*>(array_object_at));
    objc_register_method("-[NSArray firstObject]", reinterpret_cast<const void*>(array_first));
    objc_register_method("-[NSArray lastObject]", reinterpret_cast<const void*>(array_last));
    objc_register_method("-[NSArray containsObject:]", reinterpret_cast<const void*>(array_contains));
    objc_register_method("-[NSArray indexOfObject:]", reinterpret_cast<const void*>(array_index_of));
    objc_register_method("-[NSArray countByEnumeratingWithState:objects:count:]", reinterpret_cast<const void*>(array_fast_enumerate));
    objc_register_method("-[NSArray array]", reinterpret_cast<const void*>(collection_as_array));
    objc_register_method("-[NSArray allObjects]", reinterpret_cast<const void*>(collection_as_array));
    objc_register_method("+[NSValueTransformer setValueTransformer:forName:]", reinterpret_cast<const void*>(value_transformer_register));
    objc_register_method("+[NSValueTransformer valueTransformerForName:]", reinterpret_cast<const void*>(value_transformer_named));
    objc_register_method("-[NSUserDefaults dictionaryRepresentation]", reinterpret_cast<const void*>(defaults_dictionary_representation));
    objc_register_method("+[NSRegularExpression escapedPatternForString:]", reinterpret_cast<const void*>(regular_expression_escaped_pattern));
    objc_register_method("+[NSJSONSerialization JSONObjectWithData:options:error:]", reinterpret_cast<const void*>(json_object_with_data));
    objc_register_method("+[NSJSONSerialization dataWithJSONObject:options:error:]", reinterpret_cast<const void*>(json_data_with_object));
}

}  // namespace foundation
