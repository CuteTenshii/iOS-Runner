#include "foundation_internal.h"
#include "plist.h"
#include "symbols.h"

using namespace foundation;

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
        configuration->controller = static_cast<HeapObject*>(calloc(1, sizeof(HeapObject)));
        static void* controller_class = objc_native_class("WKUserContentController", free);
        configuration->controller->metadata = controller_class;
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

SWIFTCC void* url_request_bridge_to_objc(SWIFT_CONTEXT URLRequestBox** self) {
    auto* object = static_cast<NSStringObject*>(new_nsstring(*(*self)->url));
    object->header.metadata = g_nsurl_class;
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
    object->header.metadata = g_nsurl_class;
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
    const auto storage_of = [&]() { return reinterpret_cast<const char*>(*static_cast<uint64_t*>(value) & kBridgePointerMask); };
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
        const size_t stride = stride_of(value_type);
        std::string out = "{";
        visit_dictionary_buckets(storage_of(), [&](const char* keys, const char* values, size_t bucket) {
            if (out.size() > 1) out += ',';
            out += json_quote(swift_string_to_utf8(*reinterpret_cast<const SwiftString*>(keys + bucket * sizeof(SwiftString)))) + ":";
            out += value_to_json(const_cast<char*>(values) + bucket * stride, value_type);
        });
        return out + "}";
    }
    engine_log("STOP: a script argument of a type this runtime cannot convert");
    abort();
}

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

AsyncFunctionRecord g_web_script_record;

struct NSCookieObject {
    HeapObject header;
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

void* new_json_object(Json value) { return foundation_new_json_object(std::move(value)); }

Json json_from_plist(const PlistValue& value) {
    Json out;
    switch (value.kind) {
        case PlistValue::Kind::Boolean:
            out.kind = Json::Kind::Bool;
            out.flag = value.boolean;
            break;
        case PlistValue::Kind::Integer:
            out.kind = Json::Kind::Integer;
            out.integer = value.integer;
            break;
        case PlistValue::Kind::Real:
            out.kind = Json::Kind::Number;
            out.number = value.real;
            break;
        case PlistValue::Kind::String:
            return json_text(value.text);
        case PlistValue::Kind::Data:
            out.kind = Json::Kind::Data;
            out.text = value.text;
            break;
        case PlistValue::Kind::Date:
            out.kind = Json::Kind::Date;
            out.number = value.real;
            break;
        case PlistValue::Kind::Uid:
            // XML plists spell a UID this way, and NSPropertyListSerialization's callers read it from either format.
            out.kind = Json::Kind::Object;
            out.members.emplace_back("CF$UID", Json{});
            out.members.back().second.kind = Json::Kind::Integer;
            out.members.back().second.integer = value.integer;
            break;
        case PlistValue::Kind::Array:
            out.kind = Json::Kind::Array;
            for (const auto& item : value.items) out.items.push_back(json_from_plist(item));
            break;
        case PlistValue::Kind::Dictionary:
            out.kind = Json::Kind::Object;
            for (const auto& [key, item] : value.entries) out.members.emplace_back(key, json_from_plist(item));
            break;
        case PlistValue::Kind::Null:
            break;
    }
    return out;
}

PlistValue plist_from_json(const Json& value) {
    PlistValue out;
    switch (value.kind) {
        case Json::Kind::Null:
            break;
        case Json::Kind::Bool:
            out.kind = PlistValue::Kind::Boolean;
            out.boolean = value.flag;
            break;
        case Json::Kind::Integer:
            out.kind = PlistValue::Kind::Integer;
            out.integer = value.integer;
            break;
        case Json::Kind::Number:
            out.kind = PlistValue::Kind::Real;
            out.real = value.number;
            break;
        case Json::Kind::String:
            out.kind = PlistValue::Kind::String;
            out.text = value.text;
            break;
        case Json::Kind::Date:
            out.kind = PlistValue::Kind::Date;
            out.real = value.number;
            break;
        case Json::Kind::Data:
            out.kind = PlistValue::Kind::Data;
            out.text = value.text;
            break;
        case Json::Kind::Array:
            out.kind = PlistValue::Kind::Array;
            for (const auto& item : value.items) out.items.push_back(plist_from_json(item));
            break;
        case Json::Kind::Object:
            out.kind = PlistValue::Kind::Dictionary;
            for (const auto& [key, item] : value.members) out.entries[key] = plist_from_json(item);
            break;
    }
    return out;
}

bool json_from_object(void* object, Json& out) {
    if (object == nullptr) return false;
    const void* cls = static_cast<HeapObject*>(object)->metadata;
    if (cls == g_nsstring_class || cls == g_nsurl_class) {
        out = json_text(nsstring_text(object));
    } else if (cls == g_nsnumber_class) {
        const auto* number = static_cast<NSNumberObject*>(object);
        out.kind = number->is_real ? Json::Kind::Number : number->is_bool ? Json::Kind::Bool
                                                                          : Json::Kind::Integer;
        out.flag = number->value != 0;
        out.integer = number->value;
        out.number = number->real;
    } else if (cls == g_nsdata_class) {
        out.kind = Json::Kind::Data;
        out.text = *static_cast<NSDataObject*>(object)->bytes;
    } else if (cls == g_nsdate_class) {
        out.kind = Json::Kind::Date;
        out.number = static_cast<NSDateObject*>(object)->seconds;
    } else if (cls == g_nsarray_class) {
        out.kind = Json::Kind::Array;
        for (void* item : *static_cast<NSArrayObject*>(object)->items) {
            out.items.emplace_back();
            if (!json_from_object(item, out.items.back())) return false;
        }
    } else if (cls == g_json_value_class) {
        out = *static_cast<JSONValueObject*>(object)->value;
    } else if (cls == g_bridged_collection_class) {
        const auto* bridged = static_cast<BridgedCollection*>(object);
        return json_from_bridged_collection(bridged->value, bridged->key_type, bridged->element_type, out);
    } else {
        return false;
    }
    return true;
}

const PlistValue& bundle_info() {
    static const PlistValue info = [] {
        PlistValue loaded;
        plist_read_file(engine_bundle_dir() + "/Info.plist", loaded);
        return loaded;
    }();
    return info;
}

constexpr uint64_t kPlistFormatXml = 100, kPlistFormatBinary = 200;

void* property_list_from_data(void*, const char*, void* data, uint64_t, uint64_t* format, void** error) {
    PlistValue value;
    if (data == nullptr || !plist_parse(*static_cast<NSDataObject*>(data)->bytes, value)) {
        if (error != nullptr) *error = foundation_make_error("The data isn't a property list", 0);
        return nullptr;
    }
    if (format != nullptr) *format = static_cast<NSDataObject*>(data)->bytes->compare(0, 6, "bplist") == 0 ? kPlistFormatBinary : kPlistFormatXml;
    return new_json_object(json_from_plist(value));
}

void* data_from_property_list(void*, const char*, void* object, uint64_t format, uint64_t, void** error) {
    Json json;
    if (!json_from_object(object, json)) {
        engine_log("PLIST: an object of a type this runtime cannot serialize");
        if (error != nullptr) *error = foundation_make_error("The object isn't a property list", 0);
        return nullptr;
    }
    const PlistValue value = plist_from_json(json);
    return make_nsdata(format == kPlistFormatXml ? plist_write_xml(value) : plist_write_binary(value));
}

void* property_list_object_from_file(const std::string& path, bool dictionary) {
    PlistValue value;
    if (!plist_read_file(path, value) || value.kind != (dictionary ? PlistValue::Kind::Dictionary : PlistValue::Kind::Array)) return nullptr;
    return new_json_object(json_from_plist(value));
}

// Written beside the target and renamed, so a failure leaves the old file whole.
bool write_property_list_object(void* object, const std::string& path, bool xml) {
    Json json;
    if (!json_from_object(object, json)) return false;
    const PlistValue value = plist_from_json(json);
    const std::string bytes = xml ? plist_write_xml(value) : plist_write_binary(value);
    const std::string partial = path + ".partial";
    FILE* file = fopen(partial.c_str(), "wb");
    if (file == nullptr) return false;
    const bool written = fwrite(bytes.data(), 1, bytes.size(), file) == bytes.size();
    if (fclose(file) == 0 && written && rename(partial.c_str(), path.c_str()) == 0) return true;
    remove(partial.c_str());
    return false;
}

void* bundle_info_dictionary(void*, const char*) { return new_json_object(json_from_plist(bundle_info())); }

void* bundle_info_value_string(void*, const char*, void* key) {
    const PlistValue* value = bundle_info().get(nsstring_text(key));
    if (value == nullptr) return nullptr;
    if (value->kind == PlistValue::Kind::String) return new_nsstring(value->text);
    return new_json_object(json_from_plist(*value));
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
        cookie->header.metadata = g_cookie_class;
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
        case Json::Kind::Date:
            memcpy(out->buffer, &json.number, sizeof json.number);
            out->type = date_metadata();
            return;
        case Json::Kind::Data: {
            const FoundationData data = foundation_make_data(json.text);
            memcpy(out->buffer, &data, sizeof data);
            out->type = resolve_symbol("$s10Foundation4DataVN");
            return;
        }
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
    object->header.metadata = g_nsdata_class;
    object->header.references = 1;
    object->bytes = new std::string(bytes);
    return object;
}

SWIFTCC void* data_bridge_to_objc(uint64_t range, uint64_t storage) { return make_nsdata(data_bytes(range, storage)); }

SWIFTCC DataValue data_bridge_from_objc(NSDataObject* object) {
    return object == nullptr ? DataValue{0, kDataEmptyTag} : make_data(*object->bytes);
}

void* strings_object_from_json(const std::string& json) {
    Json parsed;
    json_parse(json, parsed);
    if (parsed.kind == Json::Kind::Object) return new_json_object(parsed);
    const bool all_strings = std::all_of(parsed.items.begin(), parsed.items.end(), [](const Json& item) { return item.kind == Json::Kind::String; });
    if (!all_strings) {
        AnyValue any;
        json_to_any(parsed, &any);
        auto* object = static_cast<BridgedCollection*>(collection_bridge_to_objc(reinterpret_cast<void*>(any.buffer[0])));
        object->element_type = any_type();
        rt_swift_bridgeObjectRelease(reinterpret_cast<void*>(any.buffer[0]));
        return object;
    }
    void* element = type_system_symbol("$sSSN");
    char* elements;
    void* storage = make_array_storage(element, parsed.items.size(), &elements);
    for (size_t i = 0; i < parsed.items.size(); ++i) reinterpret_cast<SwiftString*>(elements)[i] = swift_string_from_utf8(parsed.items[i].text);
    auto* object = static_cast<BridgedCollection*>(collection_bridge_to_objc(storage));
    object->element_type = element;
    rt_swift_bridgeObjectRelease(storage);
    return object;
}

bool json_from_plist_bytes(const std::string& bytes, Json& out) {
    PlistValue value;
    if (!plist_parse(bytes, value)) return false;
    out = json_from_plist(value);
    return true;
}

std::string plist_bytes_from_json(const Json& value) { return plist_write_binary(plist_from_json(value)); }

// Defaults keep collections as JSON. A collection the runtime made has no element types, and those are [String].
bool strings_json_from_object(void* object, std::string& json) {
    if (static_cast<HeapObject*>(object)->metadata != g_bridged_collection_class) return false;
    auto* collection = static_cast<BridgedCollection*>(object);
    if (collection->element_type == nullptr) {
        json = value_to_json(&collection->value, resolve_mangled_type("SaySSG", nullptr));
        return true;
    }
    Json parsed;
    if (!json_from_object(object, parsed)) return false;
    json = json_write(parsed);
    return true;
}

namespace foundation {

void add_foundation_web_symbols(SymbolTable& table) {
    table.insert({
        {"$s10Foundation4DataV19_bridgeToObjectiveCSo6NSDataCyF", reinterpret_cast<void*>(data_bridge_to_objc)},
        {"$s10Foundation4DataV36_unconditionallyBridgeFromObjectiveCyACSo6NSDataCSgFZ", reinterpret_cast<void*>(data_bridge_from_objc)},
        {"$s10Foundation10URLRequestV19_bridgeToObjectiveCSo12NSURLRequestCyF", reinterpret_cast<void*>(url_request_bridge_to_objc)},
        {"$sSo9WKWebViewC6WebKitE19callAsyncJavaScript_9arguments2in12contentWorldypSgSS_SDySSypGSo11WKFrameInfoCSgSo09WKContentL0CtYaKF", reinterpret_cast<void*>(rt_web_view_call_script_entry)},
    });
}

void register_foundation_web_classes() {
    objc_register_method("-[WKUserScript initWithSource:injectionTime:forMainFrameOnly:]", reinterpret_cast<const void*>(user_script_init));
    objc_register_method("-[WKWebView initWithFrame:configuration:]", reinterpret_cast<const void*>(web_view_init));
    objc_register_method("-[WKHTTPCookieStore getAllCookies:]", reinterpret_cast<const void*>(cookie_store_get_all));
    objc_register_method("-[WKHTTPCookieStore addObserver:]", reinterpret_cast<const void*>(cookie_store_add_observer));
    objc_register_method("-[NSHTTPCookie name]", reinterpret_cast<const void*>(cookie_name));
    objc_register_method("-[NSHTTPCookie value]", reinterpret_cast<const void*>(cookie_value));
    objc_register_method("-[NSHTTPCookie domain]", reinterpret_cast<const void*>(cookie_domain));
    objc_register_method("-[WKWebView configuration]", reinterpret_cast<const void*>(web_view_configuration));
    objc_register_method("-[WKWebView loadRequest:]", reinterpret_cast<const void*>(web_view_load));
    objc_register_method("-[WKWebView URL]", reinterpret_cast<const void*>(web_view_url));
    objc_register_method("-[WKWebViewConfiguration userContentController]", reinterpret_cast<const void*>(configuration_controller));
    objc_register_method("-[WKWebViewConfiguration setApplicationNameForUserAgent:]", reinterpret_cast<const void*>(configuration_set_application_name));
    objc_register_method("-[WKUserContentController addUserScript:]", reinterpret_cast<const void*>(controller_add_script));
    objc_register_method("+[NSPropertyListSerialization propertyListWithData:options:format:error:]", reinterpret_cast<const void*>(property_list_from_data));
    objc_register_method("+[NSPropertyListSerialization dataWithPropertyList:format:options:error:]", reinterpret_cast<const void*>(data_from_property_list));
    objc_register_method("-[NSBundle infoDictionary]", reinterpret_cast<const void*>(bundle_info_dictionary));
    objc_register_method("-[NSBundle objectForInfoDictionaryKey:]", reinterpret_cast<const void*>(bundle_info_value_string));
}

}  // namespace foundation

void* foundation_new_json_object(Json value) {
    auto* object = static_cast<JSONValueObject*>(calloc(1, sizeof(JSONValueObject)));
    object->header.metadata = g_json_value_class;
    object->header.references = 1;
    object->value = new Json(std::move(value));
    return object;
}
