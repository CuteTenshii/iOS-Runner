#include "codable_shim.h"

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <new>
#include <unordered_map>
#include <vector>

#include "base64.h"
#include "engine.h"
#include "foundation_internal.h"
#include "foundation_shim.h"
#include "json.h"
#include "macho.h"
#include "stdlib_shim.h"
#include "swift_abi.h"
#include "swift_runtime.h"
#include "symbols.h"
#include "type_system.h"

extern "C" void rt_swift_bridgeObjectRelease(void*);

extern "C" void* rt_swift_bridgeObjectRetain(void*);

namespace {
constexpr size_t kAnyBufferWords = 3;
constexpr uint64_t kBridgeObjectAddressMask = 0x00FFFFFFFFFFFFF8ULL;

size_t size_of(const void* type) {
    size_t size;
    memcpy(&size, static_cast<const char*>(static_cast<void* const*>(type)[-1]) + kValueWitnessSizeOffset, sizeof size);
    return size;
}

// A Decoder or Encoder existential: the value is just the node it reads or fills, held as an Int so nothing owns it.
struct CoderExistential {
    Json* node;
    void* padding[kAnyBufferWords - 1];
    void* type;
    void* witnesses;
};

CoderExistential coder_for(Json* node) { return {node, {nullptr, nullptr}, type_system_symbol("$sSiN"), nullptr}; }

// KeyedDecodingContainer and KeyedEncodingContainer are one reference at runtime; this is what it refers to.
const HeapMetadata kContainerMetadata = {free, nullptr, 0};

struct ContainerBox {
    void* metadata;
    std::atomic<uint64_t> references;
    Json* node;
    void* key_witnesses;
};

ContainerBox* make_container(Json* node, void* key_witnesses) {
    auto* box = static_cast<ContainerBox*>(calloc(1, sizeof(ContainerBox)));
    box->metadata = const_cast<uint64_t*>(&kContainerMetadata.kind);
    new (&box->references) std::atomic<uint64_t>(1);
    box->node = node;
    box->key_witnesses = key_witnesses;
    return box;
}

const void* witness_in(void* table, const char* requirement) {
    // A framework conformance is only a placeholder: it has no witnesses to find.
    if (unresolved_symbol_at(reinterpret_cast<uintptr_t>(static_cast<void**>(table)[0])) != nullptr) return nullptr;
    return swift_runtime_find_witness_named(static_cast<const char*>(static_cast<void**>(table)[0]), requirement);
}

using StringValueGetter = SWIFTCC SwiftString (*)(void*, void*, SWIFT_CONTEXT const void*);

std::string key_name(const ContainerBox* box, const void* key, void* key_type) {
    const auto getter = reinterpret_cast<StringValueGetter>(const_cast<void*>(witness_in(box->key_witnesses, "_$ss9CodingKeyP11stringValueSSvgTq")));
    if (getter == nullptr) {
        engine_log("STOP: a CodingKey has no stringValue");
        abort();
    }
    const SwiftString name = getter(key_type, box->key_witnesses, key);
    std::string text = swift_string_to_utf8(name);
    rt_swift_bridgeObjectRelease(reinterpret_cast<void*>(name.object));
    return text;
}

// A container's metadata has the key type as its first generic argument.
void* key_type_of(const void* container_metadata) { return static_cast<void* const*>(container_metadata)[2]; }

void* decoding_error(const std::string& what) {
    engine_log("CODABLE: %s", what.c_str());
    return foundation_make_error("The data couldn't be read: " + what, 4864);
}

using InitFromDecoder = SWIFTCC void (*)(SWIFT_OUT void*, CoderExistential*, void*, void*, SWIFT_CONTEXT void*, SWIFT_ERROR void**);
using EncodeToEncoder = SWIFTCC void (*)(CoderExistential*, void*, void*, SWIFT_CONTEXT const void*, SWIFT_ERROR void**);

constexpr uint64_t kOptionalKind = 0x202;
constexpr size_t kFirstGenericWord = 2;

// Optional's tag goes through its payload's witness, which knows the spare bits it can use.
void store_optional_tag(void* out, void* payload, bool none) {
    // Compiled code reads a nil String? as all zeros, wherever the witness puts its own tag.
    if (none) memset(out, 0, size_of(payload));
    using Store = void (*)(void*, uint32_t, uint32_t, void*);
    Store store;
    memcpy(&store, static_cast<const char*>(static_cast<void* const*>(payload)[-1]) + kWitnessStoreEnumTagOffset, sizeof store);
    store(out, none ? 1 : 0, 1, payload);
}

bool json_seconds(const Json& node, double& seconds) {
    if (node.kind == Json::Kind::Integer)
        seconds = static_cast<double>(node.integer);
    else if (node.kind == Json::Kind::Number || node.kind == Json::Kind::Date)
        seconds = node.number;
    else
        return false;
    return true;
}

// One plain object serves every coder, and only JSONEncoder's formatting is honoured.
struct CoderObject {
    void* metadata;
    uint64_t references;
    uint64_t output_formatting;
    void* user_info;
    const void* custom_date_function;
    void* custom_date_context;
};

// What the decode in progress was configured with, for the containers it passes through.
CoderObject* g_current_decoder = nullptr;

// The value type comes from the caller, as a dictionary's storage doesn't always record it.
void visit_typed_dictionary(void* storage, void* value_type, const std::function<void(const std::string&, void*)>& visit) {
    constexpr size_t kCountOffset = 16;
    int64_t count;
    memcpy(&count, static_cast<char*>(storage) + kCountOffset, sizeof count);
    if (count == 0) return;
    const size_t stride = witnesses_of(value_type)->stride;
    visit_dictionary_buckets(static_cast<const char*>(storage), [&](const char* keys, const char* values, size_t bucket) {
        visit(swift_string_to_utf8(reinterpret_cast<const SwiftString*>(keys)[bucket]), const_cast<char*>(values) + bucket * stride);
    });
}

void decode_value(void* type, Json* node, void* out, void** error) {
    if (type == type_system_symbol("$sSSN")) {
        if (node->kind != Json::Kind::String) {
            *error = decoding_error("expected a string");
            return;
        }
        const SwiftString text = swift_string_from_utf8(node->text);
        memcpy(out, &text, sizeof text);
        return;
    }
    if (type == type_system_symbol("$sSiN") || type == type_system_symbol("$sSuN")) {
        if (node->kind != Json::Kind::Integer) {
            *error = decoding_error("expected an integer");
            return;
        }
        memcpy(out, &node->integer, sizeof node->integer);
        return;
    }
    if (type == type_system_symbol("$sSbN")) {
        if (node->kind != Json::Kind::Bool) {
            *error = decoding_error("expected a boolean");
            return;
        }
        *static_cast<uint8_t*>(out) = node->flag ? 1 : 0;
        return;
    }
    if (type == type_system_symbol("$sSdN")) {
        if (node->kind != Json::Kind::Number && node->kind != Json::Kind::Integer) {
            *error = decoding_error("expected a number");
            return;
        }
        const double number = node->kind == Json::Kind::Integer ? static_cast<double>(node->integer) : node->number;
        memcpy(out, &number, sizeof number);
        return;
    }
    if (*static_cast<const uint64_t*>(type) == kOptionalKind) {
        void* payload = static_cast<void**>(type)[kFirstGenericWord];
        if (node->kind == Json::Kind::Null) {
            store_optional_tag(out, payload, true);
            return;
        }
        decode_value(payload, node, out, error);
        if (*error == nullptr) store_optional_tag(out, payload, false);
        return;
    }
    if (const char* name = foreign_type_name(type)) {
        if (strcmp(name, "_$s10Foundation3URLVMn") == 0) {
            if (node->kind != Json::Kind::String) {
                *error = decoding_error("expected a URL string");
                return;
            }
            void* url = foundation_make_url(node->text);
            memcpy(out, &url, sizeof url);
            return;
        }
        if (strcmp(name, "_$s10Foundation4DateVMn") == 0) {
            if (g_current_decoder != nullptr && g_current_decoder->custom_date_function != nullptr) {
                using DateClosure = SWIFTCC void (*)(SWIFT_OUT double*, const void*, SWIFT_CONTEXT void*, SWIFT_ERROR void**);
                CoderExistential coder = coder_for(node);
                double decoded = 0;
                reinterpret_cast<DateClosure>(const_cast<void*>(g_current_decoder->custom_date_function))(&decoded, &coder, g_current_decoder->custom_date_context, error);
                if (*error == nullptr) memcpy(out, &decoded, sizeof decoded);
                return;
            }
            double seconds = 0;
            if (node->kind == Json::Kind::String ? !foundation_parse_iso8601(node->text, seconds) : !json_seconds(*node, seconds)) {
                *error = decoding_error("expected a date");
                return;
            }
            memcpy(out, &seconds, sizeof seconds);
            return;
        }
        if (strcmp(name, "_$s10Foundation4DataVMn") == 0) {
            std::string bytes = node->text;
            if (node->kind != Json::Kind::Data && (node->kind != Json::Kind::String || !base64_decode_strict(node->text, bytes))) {
                *error = decoding_error("expected data");
                return;
            }
            const FoundationData data = foundation_make_data(bytes);
            memcpy(out, &data, sizeof data);
            return;
        }
    }
    if (void* element = array_element_type(type)) {
        if (node->kind != Json::Kind::Array) {
            *error = decoding_error("expected an array");
            return;
        }
        char* elements;
        void* storage = make_array_storage(element, node->items.size(), &elements);
        const size_t stride = (size_of(element) + 7) & ~size_t{7};
        for (size_t i = 0; i < node->items.size(); ++i) {
            decode_value(element, &node->items[i], elements + i * stride, error);
            if (*error != nullptr) return;
        }
        memcpy(out, &storage, sizeof storage);
        return;
    }
    if (void* value = dictionary_value_type(type)) {
        if (node->kind != Json::Kind::Object) {
            *error = decoding_error("expected an object");
            return;
        }
        std::vector<char> bytes(node->members.size() * ((size_of(value) + 7) & ~size_t{7}) + 1);
        const size_t stride = (size_of(value) + 7) & ~size_t{7};
        std::vector<std::pair<std::string, const void*>> entries;
        for (size_t i = 0; i < node->members.size(); ++i) {
            decode_value(value, &node->members[i].second, bytes.data() + i * stride, error);
            if (*error != nullptr) return;
            entries.emplace_back(node->members[i].first, bytes.data() + i * stride);
        }
        void* storage = make_string_dictionary(value, entries);
        memcpy(out, &storage, sizeof storage);
        return;
    }
    static const void* decodable = unresolved_symbol_named("_$sSeMp");
    void* table = swift_runtime_find_conformance(type, decodable);
    const auto init = reinterpret_cast<InitFromDecoder>(const_cast<void*>(witness_in(table, "_$sSe4fromxs7Decoder_p_tKcfCTq")));
    if (init == nullptr) {
        const char* name = foreign_type_name(type);
        engine_log("CODABLE: no decoder for %s, kind %llx, from JSON kind %d", name != nullptr ? name : "(app type)", static_cast<unsigned long long>(*static_cast<uint64_t*>(type)), static_cast<int>(node->kind));
        *error = decoding_error("this type is not Decodable");
        return;
    }
    CoderExistential coder = coder_for(node);
    init(out, &coder, type, table, type, error);
}

void encode_value(void* type, const void* value, Json& out, void** error) {
    if (type == type_system_symbol("$sSSN")) {
        out.kind = Json::Kind::String;
        out.text = swift_string_to_utf8(*static_cast<const SwiftString*>(value));
        return;
    }
    if (type == type_system_symbol("$sSiN")) {
        out.kind = Json::Kind::Integer;
        memcpy(&out.integer, value, sizeof out.integer);
        return;
    }
    if (type == type_system_symbol("$sSbN")) {
        out.kind = Json::Kind::Bool;
        out.flag = *static_cast<const uint8_t*>(value) != 0;
        return;
    }
    if (type == type_system_symbol("$sSdN")) {
        out.kind = Json::Kind::Number;
        memcpy(&out.number, value, sizeof out.number);
        return;
    }
    if (const char* name = foreign_type_name(type)) {
        if (strcmp(name, "_$s10Foundation4DateVMn") == 0) {
            out.kind = Json::Kind::Date;
            memcpy(&out.number, value, sizeof out.number);
            return;
        }
        if (strcmp(name, "_$s10Foundation3URLVMn") == 0) {
            void* url = *static_cast<void* const*>(value);
            out = json_text(foundation_url_text(url));
            return;
        }
        if (strcmp(name, "_$s10Foundation4DataVMn") == 0) {
            out.kind = Json::Kind::Data;
            out.text = foundation_data_bytes(*static_cast<const FoundationData*>(value));
            return;
        }
    }
    if (void* element = array_element_type(type)) {
        out.kind = Json::Kind::Array;
        uint64_t word;
        memcpy(&word, value, sizeof word);
        char* elements;
        const size_t count = array_storage_contents(reinterpret_cast<void*>(word & kBridgeObjectAddressMask), element, &elements);
        const size_t stride = (size_of(element) + 7) & ~size_t{7};
        out.items.resize(count);
        for (size_t i = 0; i < count; ++i) {
            encode_value(element, elements + i * stride, out.items[i], error);
            if (*error != nullptr) return;
        }
        return;
    }
    if (void* element = set_element_type(type)) {
        out.kind = Json::Kind::Array;
        uint64_t word;
        memcpy(&word, value, sizeof word);
        visit_set_elements(reinterpret_cast<void*>(word & kBridgeObjectAddressMask), [&](void* item) {
            if (*error != nullptr) return;
            out.items.emplace_back();
            encode_value(element, item, out.items.back(), error);
        });
        return;
    }
    if (void* value_type = dictionary_value_type(type)) {
        out.kind = Json::Kind::Object;
        uint64_t word;
        memcpy(&word, value, sizeof word);
        visit_typed_dictionary(reinterpret_cast<void*>(word & kBridgeObjectAddressMask), value_type, [&](const std::string& key, void* member) {
            if (*error != nullptr) return;
            out.members.emplace_back(key, Json{});
            encode_value(value_type, member, out.members.back().second, error);
        });
        return;
    }
    if (type == any_type()) {
        constexpr uint64_t kLargestMetadataKind = 0x7ff;
        const auto* any = static_cast<const AnyValue*>(value);
        if (any->type == nullptr) return;
        // A class's first word is 0 or an isa pointer, where every other type has a small kind.
        uint64_t kind;
        memcpy(&kind, any->type, sizeof kind);
        if (kind != 0 && kind <= kLargestMetadataKind) {
            encode_value(any->type, any->buffer, out, error);
        } else if (!json_from_object(reinterpret_cast<void*>(any->buffer[0]), out)) {
            *error = foundation_make_error("This value could not be encoded.", 4866);
        }
        return;
    }
    static const void* encodable = unresolved_symbol_named("_$sSEMp");
    void* table = swift_runtime_find_conformance(type, encodable);
    if (table == nullptr) {
        *error = foundation_make_error("This value could not be encoded.", 4866);
        return;
    }
    const auto encode = reinterpret_cast<EncodeToEncoder>(const_cast<void*>(witness_in(table, "_$sSE6encode2toys7Encoder_p_tKFTq")));
    if (encode == nullptr) {
        *error = foundation_make_error("This value could not be encoded.", 4866);
        return;
    }
    CoderExistential coder = coder_for(&out);
    encode(&coder, type, table, value, error);
}

constexpr uint64_t kFormattingPretty = 1, kFormattingSortedKeys = 2, kFormattingWithoutEscapingSlashes = 8;

void* coder_class_metadata() {
    static void* block[12] = {};
    return sized_class_metadata(block, reinterpret_cast<void*>(free), sizeof(CoderObject));
}

struct MetadataResponse {
    void* metadata;
    size_t state;
};

SWIFTCC MetadataResponse coder_access(size_t) { return {coder_class_metadata(), 0}; }
SWIFTCC void* coder_init(SWIFT_CONTEXT void* self) {
    auto* coder = static_cast<CoderObject*>(self);
    coder->output_formatting = 0;
    coder->user_info = resolve_symbol("_swiftEmptyDictionarySingleton");
    coder->custom_date_function = nullptr;
    coder->custom_date_context = nullptr;
    return self;
}

SWIFTCC void formatting_pretty(SWIFT_OUT uint64_t* out) { *out = kFormattingPretty; }
SWIFTCC void formatting_sorted_keys(SWIFT_OUT uint64_t* out) { *out = kFormattingSortedKeys; }
SWIFTCC void formatting_without_escaping_slashes(SWIFT_OUT uint64_t* out) { *out = kFormattingWithoutEscapingSlashes; }
SWIFTCC void encoder_set_formatting(const uint64_t* value, SWIFT_CONTEXT void* self) { static_cast<CoderObject*>(self)->output_formatting = *value; }

SWIFTCC void json_decoder_decode(SWIFT_OUT void* out, void*, uint64_t range, uint64_t storage, void* type, void*, SWIFT_CONTEXT void* self, SWIFT_ERROR void** error) {
    auto* root = new Json;
    if (!json_parse(foundation_data_bytes({range, storage}), *root)) {
        delete root;
        *error = decoding_error("it isn't valid JSON");
        return;
    }
    CoderObject* const outer = g_current_decoder;
    g_current_decoder = static_cast<CoderObject*>(self);
    decode_value(type, root, out, error);
    g_current_decoder = outer;
    delete root;
}

SWIFTCC FoundationData json_encoder_encode(const void* value, void* type, void*, SWIFT_CONTEXT void* self, SWIFT_ERROR void** error) {
    Json root;
    encode_value(type, value, root, error);
    if (*error != nullptr) return foundation_make_data("");
    const uint64_t formatting = static_cast<CoderObject*>(self)->output_formatting;
    return foundation_make_data(json_write(root, {(formatting & kFormattingPretty) != 0, (formatting & kFormattingSortedKeys) != 0}));
}

bool convert_bridged_collection(void* value, void* key_type, void* element_type, Json& out) {
    if (element_type == nullptr || (key_type != nullptr && key_type != type_system_symbol("$sSSN"))) return false;
    void* storage = reinterpret_cast<void*>(reinterpret_cast<uint64_t>(value) & kBridgeObjectAddressMask);
    void* error = nullptr;
    if (key_type != nullptr) {
        out.kind = Json::Kind::Object;
        visit_typed_dictionary(storage, element_type, [&](const std::string& key, void* member) {
            if (error != nullptr) return;
            out.members.emplace_back(key, Json{});
            encode_value(element_type, member, out.members.back().second, &error);
        });
    } else {
        out.kind = Json::Kind::Array;
        char* elements;
        const size_t count = array_storage_contents(storage, element_type, &elements);
        const size_t stride = (size_of(element_type) + 7) & ~size_t{7};
        out.items.resize(count);
        for (size_t i = 0; i < count && error == nullptr; ++i) encode_value(element_type, elements + i * stride, out.items[i], &error);
    }
    return error == nullptr;
}

SWIFTCC void plist_decoder_decode(SWIFT_OUT void* out, void*, uint64_t range, uint64_t storage, void* type, void*, SWIFT_CONTEXT void*, SWIFT_ERROR void** error) {
    auto* root = new Json;
    if (!json_from_plist_bytes(foundation_data_bytes({range, storage}), *root)) {
        delete root;
        *error = decoding_error("it isn't a valid property list");
        return;
    }
    decode_value(type, root, out, error);
    delete root;
}

SWIFTCC FoundationData plist_encoder_encode(const void* value, void* type, void*, SWIFT_CONTEXT void*, SWIFT_ERROR void** error) {
    Json root;
    encode_value(type, value, root, error);
    if (*error != nullptr) return foundation_make_data("");
    return foundation_make_data(plist_bytes_from_json(root));
}

SWIFTCC void coder_ignore_flag(bool, SWIFT_CONTEXT void*) {}

SWIFTCC void* coder_get_user_info(SWIFT_CONTEXT void* self) { return rt_swift_bridgeObjectRetain(static_cast<CoderObject*>(self)->user_info); }

struct UserInfoYield {
    void (*resume)(void*, bool) SWIFTCC;
    void** value;
};

SWIFTCC void user_info_modify_finish(void*, bool) {}

// A yield-once coroutine: the app edits the dictionary where it's stored.
SWIFTCC UserInfoYield coder_modify_user_info(void*, SWIFT_CONTEXT void* self) { return {user_info_modify_finish, &static_cast<CoderObject*>(self)->user_info}; }

// The enum arrives with its closure in the first two words, which is the only case that needs keeping.
SWIFTCC void decoder_set_date_strategy(void** strategy, SWIFT_CONTEXT void* self) {
    auto* coder = static_cast<CoderObject*>(self);
    coder->custom_date_function = strategy[0];
    coder->custom_date_context = strategy[1] != nullptr ? rt_swift_retain(strategy[1]) : nullptr;
}

// Same representation as the String it wraps, so dictionaries keyed by it hash and compare as strings.
SWIFTCC MetadataResponse coding_user_info_key_access(size_t) { return {type_system_symbol("$sSSN"), 0}; }

SWIFTCC void coding_user_info_key_init(SWIFT_OUT SwiftString* out, SwiftString raw_value) { *out = raw_value; }

SWIFTCC void decoder_container(SWIFT_OUT ContainerBox** out, void*, void*, void* key_witnesses, void*, void*, SWIFT_CONTEXT Json** self, SWIFT_ERROR void** error) {
    if ((*self)->kind != Json::Kind::Object) {
        *error = decoding_error("expected an object");
        return;
    }
    *out = make_container(*self, key_witnesses);
}

SWIFTCC void encoder_container(SWIFT_OUT ContainerBox** out, void*, void*, void* key_witnesses, void*, void*, SWIFT_CONTEXT Json** self) {
    (*self)->kind = Json::Kind::Object;
    *out = make_container(*self, key_witnesses);
}

Json* required_member(const ContainerBox* box, const void* key, void* container_type, void** error) {
    const std::string name = key_name(box, key, key_type_of(container_type));
    auto* found = const_cast<Json*>(box->node->member(name));
    if (found == nullptr) *error = decoding_error("no value for key " + name);
    return found;
}

struct OptionalInt {
    int64_t value;
    uint64_t is_nil;
};

SWIFTCC SwiftString keyed_decode_string(const void* key, void* container_type, SWIFT_CONTEXT ContainerBox** self, SWIFT_ERROR void** error) {
    SwiftString out{0, 0};
    if (Json* node = required_member(*self, key, container_type, error)) decode_value(type_system_symbol("$sSSN"), node, &out, error);
    return out;
}

SWIFTCC int64_t keyed_decode_int(const void* key, void* container_type, SWIFT_CONTEXT ContainerBox** self, SWIFT_ERROR void** error) {
    int64_t out = 0;
    if (Json* node = required_member(*self, key, container_type, error)) decode_value(type_system_symbol("$sSiN"), node, &out, error);
    return out;
}

SWIFTCC bool keyed_decode_bool(const void* key, void* container_type, SWIFT_CONTEXT ContainerBox** self, SWIFT_ERROR void** error) {
    uint8_t out = 0;
    if (Json* node = required_member(*self, key, container_type, error)) decode_value(type_system_symbol("$sSbN"), node, &out, error);
    return out != 0;
}

SWIFTCC void keyed_decode_generic(SWIFT_OUT void* out, void*, const void* key, void* container_type, void* type, void*, SWIFT_CONTEXT ContainerBox** self, SWIFT_ERROR void** error) {
    if (Json* node = required_member(*self, key, container_type, error)) decode_value(type, node, out, error);
}

Json* optional_member(const ContainerBox* box, const void* key, void* container_type) {
    auto* found = const_cast<Json*>(box->node->member(key_name(box, key, key_type_of(container_type))));
    return found == nullptr || found->kind == Json::Kind::Null ? nullptr : found;
}

SWIFTCC SwiftString keyed_decode_optional_string(const void* key, void* container_type, SWIFT_CONTEXT ContainerBox** self, SWIFT_ERROR void** error) {
    SwiftString out{0, 0};
    if (Json* node = optional_member(*self, key, container_type)) decode_value(type_system_symbol("$sSSN"), node, &out, error);
    return out;
}

SWIFTCC OptionalInt keyed_decode_optional_int(const void* key, void* container_type, SWIFT_CONTEXT ContainerBox** self, SWIFT_ERROR void** error) {
    Json* node = optional_member(*self, key, container_type);
    if (node == nullptr) return {0, 1};
    OptionalInt out{0, 0};
    decode_value(type_system_symbol("$sSiN"), node, &out.value, error);
    return out;
}

// decodeIfPresent<T>: absent and null both give nil, and the result is T? in place.
SWIFTCC void keyed_decode_if_present_generic(SWIFT_OUT void* out, void*, const void* key, void* container_type, void* type, void*, SWIFT_CONTEXT ContainerBox** self, SWIFT_ERROR void** error) {
    Json* node = optional_member(*self, key, container_type);
    if (node == nullptr) {
        store_optional_tag(out, type, true);
        return;
    }
    decode_value(type, node, out, error);
    if (*error == nullptr) store_optional_tag(out, type, false);
}

constexpr uint8_t kOptionalBoolNone = 2;

SWIFTCC uint8_t keyed_decode_optional_bool(const void* key, void* container_type, SWIFT_CONTEXT ContainerBox** self, SWIFT_ERROR void** error) {
    Json* node = optional_member(*self, key, container_type);
    if (node == nullptr) return kOptionalBoolNone;
    uint8_t out = 0;
    decode_value(type_system_symbol("$sSbN"), node, &out, error);
    return out;
}

SWIFTCC double keyed_decode_double(const void* key, void* container_type, SWIFT_CONTEXT ContainerBox** self, SWIFT_ERROR void** error) {
    double out = 0;
    if (Json* node = required_member(*self, key, container_type, error)) decode_value(type_system_symbol("$sSdN"), node, &out, error);
    return out;
}

SWIFTCC bool keyed_contains(const void* key, void* container_type, SWIFT_CONTEXT ContainerBox** self) { return (*self)->node->member(key_name(*self, key, key_type_of(container_type))) != nullptr; }

SWIFTCC bool keyed_decode_nil(const void* key, void* container_type, SWIFT_CONTEXT ContainerBox** self, SWIFT_ERROR void**) { return optional_member(*self, key, container_type) == nullptr; }

SWIFTCC void keyed_nested(SWIFT_OUT ContainerBox** out, void*, const void* key, void* container_type, void*, void* key_witnesses, SWIFT_CONTEXT ContainerBox** self, SWIFT_ERROR void** error) {
    Json* node = required_member(*self, key, container_type, error);
    if (node != nullptr) *out = make_container(node, key_witnesses);
}

// Single-value and unkeyed containers are existentials over a node; unkeyed keeps its position in the buffer.
SWIFTCC void decoder_value_container(SWIFT_OUT CoderExistential* out, void*, void*, SWIFT_CONTEXT Json** self, SWIFT_ERROR void**) { *out = coder_for(*self); }

SWIFTCC void decoder_unkeyed_container(SWIFT_OUT CoderExistential* out, void*, void*, SWIFT_CONTEXT Json** self, SWIFT_ERROR void** error) {
    if ((*self)->kind != Json::Kind::Array) {
        *error = decoding_error("expected an array");
        return;
    }
    *out = coder_for(*self);
}

SWIFTCC void* coding_path_empty(void*, void*, SWIFT_CONTEXT void*) { return resolve_symbol("_swiftEmptyArrayStorage"); }
SWIFTCC void* decoder_user_info(void*, void*, SWIFT_CONTEXT void*) { return g_current_decoder != nullptr ? rt_swift_bridgeObjectRetain(g_current_decoder->user_info) : resolve_symbol("_swiftEmptyDictionarySingleton"); }

SWIFTCC bool value_decode_nil(void*, void*, SWIFT_CONTEXT Json** self) { return (*self)->kind == Json::Kind::Null; }

SWIFTCC bool value_decode_bool(void*, void*, SWIFT_CONTEXT Json** self, SWIFT_ERROR void** error) {
    uint8_t out = 0;
    decode_value(type_system_symbol("$sSbN"), *self, &out, error);
    return out != 0;
}

SWIFTCC int64_t value_decode_int(void*, void*, SWIFT_CONTEXT Json** self, SWIFT_ERROR void** error) {
    int64_t out = 0;
    decode_value(type_system_symbol("$sSiN"), *self, &out, error);
    return out;
}

SWIFTCC double value_decode_double(void*, void*, SWIFT_CONTEXT Json** self, SWIFT_ERROR void** error) {
    double out = 0;
    decode_value(type_system_symbol("$sSdN"), *self, &out, error);
    return out;
}

SWIFTCC SwiftString value_decode_string(void*, void*, SWIFT_CONTEXT Json** self, SWIFT_ERROR void** error) {
    SwiftString out{0, 0};
    decode_value(type_system_symbol("$sSSN"), *self, &out, error);
    return out;
}

SWIFTCC void value_decode_generic(SWIFT_OUT void* out, void*, void* type, void*, void*, void*, SWIFT_CONTEXT Json** self, SWIFT_ERROR void** error) { decode_value(type, *self, out, error); }

// The position lives in the existential's second buffer word, which the guest passes back by address.
int64_t& unkeyed_position(CoderExistential* self) { return *reinterpret_cast<int64_t*>(&self->padding[0]); }

SWIFTCC bool unkeyed_is_at_end(void*, void*, SWIFT_CONTEXT CoderExistential* self) { return unkeyed_position(self) >= static_cast<int64_t>(self->node->items.size()); }
SWIFTCC int64_t unkeyed_current_index(void*, void*, SWIFT_CONTEXT CoderExistential* self) { return unkeyed_position(self); }

struct OptionalCount {
    int64_t value;
    uint64_t is_nil;
};

SWIFTCC OptionalCount unkeyed_count(void*, void*, SWIFT_CONTEXT CoderExistential* self) { return {static_cast<int64_t>(self->node->items.size()), 0}; }

SWIFTCC void unkeyed_decode_generic(SWIFT_OUT void* out, void*, void* type, void*, void*, void*, SWIFT_CONTEXT CoderExistential* self, SWIFT_ERROR void** error) {
    int64_t& position = unkeyed_position(self);
    if (position >= static_cast<int64_t>(self->node->items.size())) {
        *error = decoding_error("the array has no more values");
        return;
    }
    decode_value(type, &self->node->items[static_cast<size_t>(position)], out, error);
    if (*error == nullptr) ++position;
}

Json& new_member(const ContainerBox* box, const void* key, void* container_type) {
    box->node->members.emplace_back(key_name(box, key, key_type_of(container_type)), Json{});
    return box->node->members.back().second;
}

SWIFTCC void keyed_encode_string(SwiftString value, const void* key, void* container_type, SWIFT_CONTEXT ContainerBox** self, SWIFT_ERROR void** error) {
    encode_value(type_system_symbol("$sSSN"), &value, new_member(*self, key, container_type), error);
}

SWIFTCC void keyed_encode_int(int64_t value, const void* key, void* container_type, SWIFT_CONTEXT ContainerBox** self, SWIFT_ERROR void** error) {
    encode_value(type_system_symbol("$sSiN"), &value, new_member(*self, key, container_type), error);
}

SWIFTCC void keyed_encode_bool(bool value, const void* key, void* container_type, SWIFT_CONTEXT ContainerBox** self, SWIFT_ERROR void** error) {
    const uint8_t flag = value ? 1 : 0;
    encode_value(type_system_symbol("$sSbN"), &flag, new_member(*self, key, container_type), error);
}

SWIFTCC void keyed_encode_generic(const void* value, const void* key, void* container_type, void* type, void*, SWIFT_CONTEXT ContainerBox** self, SWIFT_ERROR void** error) {
    encode_value(type, value, new_member(*self, key, container_type), error);
}

SWIFTCC void keyed_encode_generic_if_present(const void* value, const void* key, void* container_type, void* type, void* witnesses, SWIFT_CONTEXT ContainerBox** self, SWIFT_ERROR void** error) {
    if (witnesses_of(type)->get_enum_tag_single_payload(value, 1, type) == 0) keyed_encode_generic(value, key, container_type, type, witnesses, self, error);
}

SWIFTCC void keyed_encode_optional_string(SwiftString value, const void* key, void* container_type, SWIFT_CONTEXT ContainerBox** self, SWIFT_ERROR void** error) {
    if (value.object != 0) keyed_encode_string(value, key, container_type, self, error);
}

SWIFTCC void keyed_encode_optional_int(int64_t value, uint8_t is_nil, const void* key, void* container_type, SWIFT_CONTEXT ContainerBox** self, SWIFT_ERROR void** error) {
    if ((is_nil & 1) == 0) keyed_encode_int(value, key, container_type, self, error);
}

using InitRawValue = SWIFTCC void (*)(SWIFT_OUT void*, const SwiftString*, void*, void*, SWIFT_CONTEXT void*);
using GetRawValue = SWIFTCC void (*)(SWIFT_OUT SwiftString*, void*, void*, SWIFT_CONTEXT const void*);

// RawRepresentable's default init(from:) and encode(to:), where RawValue is String.
SWIFTCC void raw_representable_init(SWIFT_OUT void* out, CoderExistential* decoder, void* type, void*, void* raw_witnesses, SWIFT_CONTEXT void*, SWIFT_ERROR void** error) {
    if (decoder->node->kind != Json::Kind::String) {
        *error = decoding_error("expected a string");
        return;
    }
    const auto init = reinterpret_cast<InitRawValue>(const_cast<void*>(witness_in(raw_witnesses, "_$sSY8rawValuexSg03RawB0Qz_tcfCTq")));
    const SwiftString raw = swift_string_from_utf8(decoder->node->text);
    void* optional = optional_metadata(type);
    std::vector<char> result(size_of(optional) + 8);
    init(result.data(), &raw, type, raw_witnesses, type);
    TypeField payload{};
    if (!optional_payload(optional, result.data(), payload) || payload.type == nullptr) {
        *error = decoding_error("unknown value " + decoder->node->text);
        return;
    }
    memcpy(out, result.data() + payload.offset, size_of(type));
}

SWIFTCC void raw_representable_encode(CoderExistential* encoder, void* type, void*, void* raw_witnesses, SWIFT_CONTEXT const void* self, SWIFT_ERROR void**) {
    const auto get = reinterpret_cast<GetRawValue>(const_cast<void*>(witness_in(raw_witnesses, "_$sSY8rawValue03RawB0QzvgTq")));
    SwiftString raw{0, 0};
    get(&raw, type, raw_witnesses, self);
    encoder->node->kind = Json::Kind::String;
    encoder->node->text = swift_string_to_utf8(raw);
    rt_swift_bridgeObjectRelease(reinterpret_cast<void*>(raw.object));
}

}  // namespace

bool json_from_bridged_collection(void* value, void* key_type, void* element_type, Json& out) { return convert_bridged_collection(value, key_type, element_type, out); }

bool json_from_any(const void* any, Json& out) {
    void* error = nullptr;
    encode_value(any_type(), any, out, &error);
    return error == nullptr;
}

void* codable_symbol(std::string_view name) {
    static const std::unordered_map<std::string_view, void*> table = {
        {"$ss22KeyedDecodingContainerV15decodeIfPresent_6forKeyqd__Sgqd__m_xtKSeRd__lF", reinterpret_cast<void*>(keyed_decode_if_present_generic)},
        {"$ss22KeyedDecodingContainerV15decodeIfPresent_6forKeySbSgSbm_xtKF", reinterpret_cast<void*>(keyed_decode_optional_bool)},
        {"$ss22KeyedDecodingContainerV15decodeIfPresent_6forKeySuSgSum_xtKF", reinterpret_cast<void*>(keyed_decode_optional_int)},
        {"$ss22KeyedDecodingContainerV15decodeIfPresent_6forKeys5Int64VSgAFm_xtKF", reinterpret_cast<void*>(keyed_decode_optional_int)},
        {"$ss22KeyedDecodingContainerV6decode_6forKeyS2dm_xtKF", reinterpret_cast<void*>(keyed_decode_double)},
        {"$ss22KeyedDecodingContainerV6decode_6forKeys5Int64VAFm_xtKF", reinterpret_cast<void*>(keyed_decode_int)},
        {"$ss22KeyedDecodingContainerV8containsySbxF", reinterpret_cast<void*>(keyed_contains)},
        {"$ss22KeyedDecodingContainerV9decodeNil6forKeySbx_tKF", reinterpret_cast<void*>(keyed_decode_nil)},
        {"$ss22KeyedDecodingContainerV06nestedC07keyedBy6forKeyAByqd__Gqd__m_xtKs06CodingH0Rd__lF", reinterpret_cast<void*>(keyed_nested)},
        {"$ss22KeyedDecodingContainerV10codingPathSays9CodingKey_pGvg", reinterpret_cast<void*>(coding_path_empty)},
        {"$ss7DecoderP20singleValueContainers06Singlec8DecodingD0_pyKFTj", reinterpret_cast<void*>(decoder_value_container)},
        {"$ss7DecoderP16unkeyedContainers015UnkeyedDecodingC0_pyKFTj", reinterpret_cast<void*>(decoder_unkeyed_container)},
        {"$ss7DecoderP10codingPathSays9CodingKey_pGvgTj", reinterpret_cast<void*>(coding_path_empty)},
        {"$ss7DecoderP8userInfoSDys010CodingUserC3KeyVypGvgTj", reinterpret_cast<void*>(decoder_user_info)},
        {"$ss28SingleValueDecodingContainerP10codingPathSays9CodingKey_pGvgTj", reinterpret_cast<void*>(coding_path_empty)},
        {"$ss28SingleValueDecodingContainerP9decodeNilSbyFTj", reinterpret_cast<void*>(value_decode_nil)},
        {"$ss28SingleValueDecodingContainerP6decodeyS2bmKFTj", reinterpret_cast<void*>(value_decode_bool)},
        {"$ss28SingleValueDecodingContainerP6decodeyS2imKFTj", reinterpret_cast<void*>(value_decode_int)},
        {"$ss28SingleValueDecodingContainerP6decodeyS2umKFTj", reinterpret_cast<void*>(value_decode_int)},
        {"$ss28SingleValueDecodingContainerP6decodeyS2dmKFTj", reinterpret_cast<void*>(value_decode_double)},
        {"$ss28SingleValueDecodingContainerP6decodeyS2SmKFTj", reinterpret_cast<void*>(value_decode_string)},
        {"$ss28SingleValueDecodingContainerP6decodeyqd__qd__mKSeRd__lFTj", reinterpret_cast<void*>(value_decode_generic)},
        {"$ss24UnkeyedDecodingContainerP7isAtEndSbvgTj", reinterpret_cast<void*>(unkeyed_is_at_end)},
        {"$ss24UnkeyedDecodingContainerP12currentIndexSivgTj", reinterpret_cast<void*>(unkeyed_current_index)},
        {"$ss24UnkeyedDecodingContainerP5countSiSgvgTj", reinterpret_cast<void*>(unkeyed_count)},
        {"$ss24UnkeyedDecodingContainerP10codingPathSays9CodingKey_pGvgTj", reinterpret_cast<void*>(coding_path_empty)},
        {"$ss24UnkeyedDecodingContainerP6decodeyqd__qd__mKSeRd__lFTj", reinterpret_cast<void*>(unkeyed_decode_generic)},
        {"$s10Foundation11JSONDecoderCMa", reinterpret_cast<void*>(coder_access)},
        {"$s10Foundation11JSONEncoderCMa", reinterpret_cast<void*>(coder_access)},
        {"$s10Foundation11JSONDecoderCACycfc", reinterpret_cast<void*>(coder_init)},
        {"$s10Foundation11JSONEncoderCACycfc", reinterpret_cast<void*>(coder_init)},
        {"$s10Foundation11JSONDecoderC6decode_4fromxxm_AA4DataVtKSeRzlFTj", reinterpret_cast<void*>(json_decoder_decode)},
        {"$s10Foundation11JSONEncoderC6encodeyAA4DataVxKSERzlFTj", reinterpret_cast<void*>(json_encoder_encode)},
        {"$s10Foundation11JSONEncoderC16OutputFormattingV13prettyPrintedAEvgZ", reinterpret_cast<void*>(formatting_pretty)},
        {"$s10Foundation11JSONEncoderC16OutputFormattingV10sortedKeysAEvgZ", reinterpret_cast<void*>(formatting_sorted_keys)},
        {"$s10Foundation11JSONEncoderC16OutputFormattingV22withoutEscapingSlashesAEvgZ", reinterpret_cast<void*>(formatting_without_escaping_slashes)},
        {"$s10Foundation11JSONEncoderC16outputFormattingAC06OutputD0VvsTj", reinterpret_cast<void*>(encoder_set_formatting)},
        {"$s10Foundation11JSONDecoderC11allowsJSON5SbvsTj", reinterpret_cast<void*>(coder_ignore_flag)},
        {"$s10Foundation11JSONDecoderC8userInfoSDys010CodingUserD3KeyVypGvgTj", reinterpret_cast<void*>(coder_get_user_info)},
        {"$s10Foundation11JSONDecoderC8userInfoSDys010CodingUserD3KeyVypGvMTj", reinterpret_cast<void*>(coder_modify_user_info)},
        {"$s10Foundation11JSONEncoderC8userInfoSDys010CodingUserD3KeyVypGvgTj", reinterpret_cast<void*>(coder_get_user_info)},
        {"$s10Foundation11JSONEncoderC8userInfoSDys010CodingUserD3KeyVypGvMTj", reinterpret_cast<void*>(coder_modify_user_info)},
        {"$s10Foundation11JSONDecoderC20dateDecodingStrategyAC04DatedE0OvsTj", reinterpret_cast<void*>(decoder_set_date_strategy)},
        {"$ss17CodingUserInfoKeyV8rawValueABSgSS_tcfC", reinterpret_cast<void*>(coding_user_info_key_init)},
        {"$ss17CodingUserInfoKeyVMa", reinterpret_cast<void*>(coding_user_info_key_access)},
        {"$s10Foundation19PropertyListDecoderCMa", reinterpret_cast<void*>(coder_access)},
        {"$s10Foundation19PropertyListEncoderCMa", reinterpret_cast<void*>(coder_access)},
        {"$s10Foundation19PropertyListDecoderCACycfc", reinterpret_cast<void*>(coder_init)},
        {"$s10Foundation19PropertyListEncoderCACycfc", reinterpret_cast<void*>(coder_init)},
        {"$s10Foundation19PropertyListDecoderC6decode_4fromxxm_AA4DataVtKSeRzlFTj", reinterpret_cast<void*>(plist_decoder_decode)},
        {"$s10Foundation19PropertyListEncoderC6encodeyAA4DataVxKSERzlFTj", reinterpret_cast<void*>(plist_encoder_encode)},
        {"$ss7DecoderP9container7keyedBys22KeyedDecodingContainerVyqd__Gqd__m_tKs9CodingKeyRd__lFTj", reinterpret_cast<void*>(decoder_container)},
        {"$ss7EncoderP9container7keyedBys22KeyedEncodingContainerVyqd__Gqd__m_ts9CodingKeyRd__lFTj", reinterpret_cast<void*>(encoder_container)},
        {"$ss22KeyedDecodingContainerV6decode_6forKeyS2Sm_xtKF", reinterpret_cast<void*>(keyed_decode_string)},
        {"$ss22KeyedDecodingContainerV6decode_6forKeyS2im_xtKF", reinterpret_cast<void*>(keyed_decode_int)},
        {"$ss22KeyedDecodingContainerV6decode_6forKeyS2bm_xtKF", reinterpret_cast<void*>(keyed_decode_bool)},
        {"$ss22KeyedDecodingContainerV6decode_6forKeyqd__qd__m_xtKSeRd__lF", reinterpret_cast<void*>(keyed_decode_generic)},
        {"$ss22KeyedDecodingContainerV15decodeIfPresent_6forKeySSSgSSm_xtKF", reinterpret_cast<void*>(keyed_decode_optional_string)},
        {"$ss22KeyedDecodingContainerV15decodeIfPresent_6forKeySiSgSim_xtKF", reinterpret_cast<void*>(keyed_decode_optional_int)},
        {"$ss22KeyedEncodingContainerV6encode_6forKeyySS_xtKF", reinterpret_cast<void*>(keyed_encode_string)},
        {"$ss22KeyedEncodingContainerV6encode_6forKeyySi_xtKF", reinterpret_cast<void*>(keyed_encode_int)},
        {"$ss22KeyedEncodingContainerV6encode_6forKeyySb_xtKF", reinterpret_cast<void*>(keyed_encode_bool)},
        {"$ss22KeyedEncodingContainerV6encode_6forKeyyqd___xtKSERd__lF", reinterpret_cast<void*>(keyed_encode_generic)},
        {"$ss22KeyedEncodingContainerV15encodeIfPresent_6forKeyyqd__Sg_xtKSERd__lF", reinterpret_cast<void*>(keyed_encode_generic_if_present)},
        {"$ss22KeyedEncodingContainerV15encodeIfPresent_6forKeyySSSg_xtKF", reinterpret_cast<void*>(keyed_encode_optional_string)},
        {"$ss22KeyedEncodingContainerV15encodeIfPresent_6forKeyySiSg_xtKF", reinterpret_cast<void*>(keyed_encode_optional_int)},
        {"$sSYsSeRzSS8RawValueSYRtzrlE4fromxs7Decoder_p_tKcfC", reinterpret_cast<void*>(raw_representable_init)},
        {"$sSYsSERzSS8RawValueSYRtzrlE6encode2toys7Encoder_p_tKF", reinterpret_cast<void*>(raw_representable_encode)},
    };
    const auto found = table.find(name);
    return found == table.end() ? nullptr : found->second;
}
