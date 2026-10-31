#include "foundation_internal.h"

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
        {"$s10Foundation22_convertNSErrorToErrorys0E0_pSo0C0CSgF", reinterpret_cast<void*>(error_identity)},
        {"$s10Foundation22_convertErrorToNSErrorySo0E0Cs0C0_pF", reinterpret_cast<void*>(error_identity)},
        {"$ss018_bridgeAnyObjectToB0yypyXlSgF", reinterpret_cast<void*>(bridge_any_object_to_any)},
    });
}

void register_foundation_date_classes() {
    objc_register_method("-[NSUserDefaults dictionaryRepresentation]", reinterpret_cast<const void*>(defaults_dictionary_representation));
    objc_register_method("+[NSRegularExpression escapedPatternForString:]", reinterpret_cast<const void*>(regular_expression_escaped_pattern));
    objc_register_method("+[NSJSONSerialization JSONObjectWithData:options:error:]", reinterpret_cast<const void*>(json_object_with_data));
    objc_register_method("+[NSJSONSerialization dataWithJSONObject:options:error:]", reinterpret_cast<const void*>(json_data_with_object));
}

}  // namespace foundation
