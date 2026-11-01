#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

#include "engine.h"
#include "foundation_internal.h"
#include "json.h"
#include "stdlib_shim.h"
#include "symbols.h"
#include "uikit_internal.h"

using namespace foundation;

// Security.framework's Keychain, stored by the host. See Keychain.kt for what keeps apps apart.
namespace {

constexpr int32_t kStatusSuccess = 0, kStatusParam = -50, kStatusNotFound = -25300;
constexpr size_t kAnyTypeWordIndex = 3;

const char kBase64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

std::string base64_encode(const std::string& bytes) {
    std::string out;
    uint32_t bits = 0;
    int count = 0;
    for (const char c : bytes) {
        bits = (bits << 8) | static_cast<uint8_t>(c);
        count += 8;
        while (count >= 6) {
            count -= 6;
            out += kBase64[(bits >> count) & 63];
        }
    }
    if (count > 0) out += kBase64[(bits << (6 - count)) & 63];
    while (out.size() % 4 != 0) out += '=';
    return out;
}

std::string base64_decode(const std::string& text) {
    std::string out;
    uint32_t bits = 0;
    int count = 0;
    for (const char c : text) {
        const char* found = c != '\0' ? strchr(kBase64, c) : nullptr;
        if (found == nullptr) continue;
        bits = (bits << 6) | static_cast<uint32_t>(found - kBase64);
        count += 6;
        if (count >= 8) {
            count -= 8;
            out += static_cast<char>((bits >> count) & 0xff);
        }
    }
    return out;
}

Json object_of(std::vector<std::pair<std::string, Json>> members) {
    Json out;
    out.kind = Json::Kind::Object;
    out.members = std::move(members);
    return out;
}

Json typed(const char* tag, Json value) { return object_of({{tag, std::move(value)}}); }

Json integer_json(int64_t value) {
    Json out;
    out.kind = Json::Kind::Integer;
    out.integer = value;
    return out;
}

// Values travel tagged, so that a data item and a string with the same text never match.
bool typed_from_object(void* object, Json& out) {
    if (object == nullptr) return false;
    const void* isa = static_cast<HeapObject*>(object)->metadata;
    if (isa == g_nsstring_class) {
        out = typed("s", json_text(nsstring_text(object)));
    } else if (isa == g_nsdata_class) {
        out = typed("d", json_text(base64_encode(*static_cast<NSDataObject*>(object)->bytes)));
    } else if (isa == g_nsnumber_class) {
        const auto* number = static_cast<NSNumberObject*>(object);
        out = typed(number->is_bool ? "b" : "i", integer_json(number->value));
    } else {
        return false;
    }
    return true;
}

bool typed_from_any(void* entry, Json& out) {
    auto** words = static_cast<void**>(entry);
    void* type = words[kAnyTypeWordIndex];
    if (type == type_system_symbol("$sSSN")) {
        out = typed("s", json_text(swift_string_to_utf8(*static_cast<SwiftString*>(entry))));
    } else if (type == type_system_symbol("$sSbN")) {
        out = typed("b", integer_json(*static_cast<bool*>(entry) ? 1 : 0));
    } else if (type == type_system_symbol("$sSiN")) {
        out = typed("i", integer_json(*static_cast<int64_t*>(entry)));
    } else if (type == resolve_symbol("$s10Foundation4DataVN")) {
        out = typed("d", json_text(base64_encode(foundation_data_bytes(*static_cast<FoundationData*>(entry)))));
    } else {
        return typed_from_object(words[0], out);
    }
    return true;
}

struct ParsedQuery {
    Json attributes;
    bool return_data = false, return_attributes = false, return_ref = false, return_all = false;
};

bool flag_value(const Json& value) {
    const Json* flag = value.member("b");
    return flag != nullptr && flag->integer != 0;
}

bool parse_query(void* dictionary, ParsedQuery& out) {
    void* storage = nullptr;
    if (dictionary == nullptr || !bridged_collection_value(dictionary, storage)) return false;
    out.attributes.kind = Json::Kind::Object;
    visit_string_dictionary(storage, [&](const std::string& key, void* entry) {
        Json value;
        if (!typed_from_any(entry, value)) {
            engine_log("KEYCHAIN: ignoring a value of an unsupported type for %s", key.c_str());
            return;
        }
        if (key == "kSecReturnData")
            out.return_data = flag_value(value);
        else if (key == "kSecReturnAttributes")
            out.return_attributes = flag_value(value);
        else if (key == "kSecReturnRef" || key == "kSecReturnPersistentRef")
            out.return_ref = flag_value(value);
        else if (key == "kSecMatchLimit") {
            const Json* text = value.member("s");
            out.return_all = text != nullptr && text->text == "kSecMatchLimitAll";
        } else if (key.rfind("kSecUse", 0) != 0 && key.rfind("kSecMatch", 0) != 0) {
            out.attributes.members.emplace_back(key, std::move(value));
        }
    });
    return true;
}

bool call_keychain(Json request, Json& reply, int32_t& status) {
    const std::string answer = engine_host_query("keychain", json_write(request));
    status = kStatusParam;
    if (!json_parse(answer, reply)) return false;
    const Json* code = reply.member("status");
    if (code == nullptr) return false;
    status = static_cast<int32_t>(code->integer);
    return true;
}

// As plist data, so that Swift sees attributes the way it sees any bridged dictionary.
Json plain_value(const Json& value) {
    if (const Json* text = value.member("s")) return *text;
    if (const Json* number = value.member("i")) return *number;
    if (const Json* flag = value.member("b")) {
        Json out;
        out.kind = Json::Kind::Bool;
        out.flag = flag->integer != 0;
        return out;
    }
    if (const Json* data = value.member("d")) {
        Json out;
        out.kind = Json::Kind::Data;
        out.text = base64_decode(data->text);
        return out;
    }
    return {};
}

void* item_data(const Json& item) {
    const Json* value = item.member("kSecValueData");
    const Json* data = value != nullptr ? value->member("d") : nullptr;
    return make_nsdata(data != nullptr ? base64_decode(data->text) : std::string());
}

void* item_result(const ParsedQuery& query, const Json& item) {
    if (!query.return_attributes) return item_data(item);
    Json attributes;
    attributes.kind = Json::Kind::Object;
    for (const auto& [key, value] : item.members) {
        if (key == "kSecValueData" && !query.return_data) continue;
        attributes.members.emplace_back(key, plain_value(value));
    }
    return foundation_new_json_object(std::move(attributes));
}

int32_t sec_item_add(void* attributes, void** result) {
    ParsedQuery query;
    if (!parse_query(attributes, query)) return kStatusParam;
    Json reply;
    int32_t status = kStatusParam;
    call_keychain(object_of({{"op", json_text("add")}, {"query", query.attributes}}), reply, status);
    if (result != nullptr) *result = nullptr;
    return status;
}

int32_t sec_item_copy_matching(void* query_object, void** result) {
    ParsedQuery query;
    if (result != nullptr) *result = nullptr;
    if (!parse_query(query_object, query)) return kStatusParam;
    Json all;
    all.kind = Json::Kind::Bool;
    all.flag = query.return_all;
    Json reply;
    int32_t status = kStatusParam;
    if (!call_keychain(object_of({{"op", json_text("find")}, {"all", all}, {"query", query.attributes}}), reply, status) || status != kStatusSuccess) return status;
    const Json* items = reply.member("items");
    if (items == nullptr || items->items.empty()) return kStatusNotFound;
    if (result == nullptr || (!query.return_data && !query.return_attributes && !query.return_ref)) return kStatusSuccess;
    if (query.return_ref) engine_log("KEYCHAIN: references aren't supported; answering with the item's data");
    if (!query.return_all) {
        *result = item_result(query, items->items[0]);
        return kStatusSuccess;
    }
    std::vector<void*> results;
    for (const Json& item : items->items) results.push_back(item_result(query, item));
    *result = foundation_new_array(results);
    return kStatusSuccess;
}

int32_t sec_item_update(void* query_object, void* changes_object) {
    ParsedQuery query, changes;
    if (!parse_query(query_object, query) || !parse_query(changes_object, changes)) return kStatusParam;
    Json reply;
    int32_t status = kStatusParam;
    call_keychain(object_of({{"op", json_text("update")}, {"query", query.attributes}, {"changes", changes.attributes}}), reply, status);
    return status;
}

int32_t sec_item_delete(void* query_object) {
    ParsedQuery query;
    if (!parse_query(query_object, query)) return kStatusParam;
    Json reply;
    int32_t status = kStatusParam;
    call_keychain(object_of({{"op", json_text("delete")}, {"query", query.attributes}}), reply, status);
    return status;
}

}  // namespace

void* security_symbol(std::string_view name) {
    if (name == "SecItemAdd") return reinterpret_cast<void*>(sec_item_add);
    if (name == "SecItemCopyMatching") return reinterpret_cast<void*>(sec_item_copy_matching);
    if (name == "SecItemUpdate") return reinterpret_cast<void*>(sec_item_update);
    if (name == "SecItemDelete") return reinterpret_cast<void*>(sec_item_delete);
    return nullptr;
}
