#include "stdlib_internal.h"

using namespace stdlib;

static SymbolTable all_stdlib_symbols() {
    SymbolTable all = {
        {"$s2os12OSSignpostIDV3logACSo03OS_a1_D0C_tcfC", reinterpret_cast<void*>(+[](void*) -> uint64_t { return 0; })},
        {"$s2os12OSSignpostIDV3log6objectACSo03OS_a1_D0C_yXltcfC", reinterpret_cast<void*>(+[](void*, void*) -> uint64_t { return 0; })},
        {"$s2os0A9_signpost_3dso3log4name0B2IDySo0a1_B7_type_ta_SVSo03OS_a1_D0Cs12StaticStringVAA010OSSignpostF0VtF", reinterpret_cast<void*>(+[] {})},
        {"$s2os0A9_signpost_3dso3log4name0B2ID__ySo0a1_B7_type_ta_SVSo03OS_a1_D0Cs12StaticStringVAA010OSSignpostF0VALs7CVarArg_pdtF", reinterpret_cast<void*>(+[] {})},
        {"swift_unownedRetain", reinterpret_cast<void*>(rt_swift_retain)},
        {"swift_unownedRelease", reinterpret_cast<void*>(rt_swift_release)},
        {"swift_unownedRetainStrong", reinterpret_cast<void*>(rt_swift_retain)},
        {"swift_dynamicCast", reinterpret_cast<void*>(swift_cast)},
        {"swift_getKeyPath", reinterpret_cast<void*>(key_path_create)},
        {"swift_modifyAtReferenceWritableKeyPath", reinterpret_cast<void*>(key_path_begin_modify)},
        {"$sSo8NSObjectC10ObjectiveCE2eeoiySbAB_ABtFZ", reinterpret_cast<void*>(+[](void* left, void* right) { return left == right; })},
        {"$sSO9hashValueSivg", reinterpret_cast<void*>(+[](uint64_t identifier) -> uint64_t { return identifier >> 4; })},
    };
    add_stdlib_runtime_symbols(all);
    add_stdlib_strings_symbols(all);
    add_stdlib_collections_symbols(all);
    return all;
}

void* stdlib_symbol(std::string_view name) {
    static const bool registered = [] {
        register_foreign_witness("_$sSis23CustomStringConvertiblesWP", "_$ss23CustomStringConvertibleP11descriptionSSvgTq", reinterpret_cast<const void*>(int_description));
        return true;
    }();
    (void)registered;
    static const SymbolTable table = all_stdlib_symbols();
    return lookup_symbol(table, name);
}

extern "C" void (*storage_destroyer_for(const char* symbol))(void*) {
    const std::string_view name(symbol);
    if (name == "_$ss11_SetStorageCMn") return destroy_set_storage;
    if (name == "_$ss18_DictionaryStorageCMn") return destroy_dictionary_storage;
    if (name == "_$ss23_ContiguousArrayStorageCMn") return destroy_array_storage;
    return nullptr;
}

void* make_string_dictionary(void* value_type, const std::vector<std::pair<std::string, const void*>>& entries) {
    void* arguments[] = {type_system_symbol("$sSSN"), value_type};
    void* metadata = storage_class_metadata("_$ss18_DictionaryStorageCMn", arguments, 2);
    auto* raw = static_cast<char*>(allocate_dictionary(metadata, scale_for_capacity(static_cast<int64_t>(entries.size())), 0, 0, false));
    auto* header = reinterpret_cast<HashedHeader*>(raw);
    char* keys;
    char* values;
    memcpy(&keys, raw + kDictionaryKeysOffset, sizeof keys);
    memcpy(&values, raw + kDictionaryValuesOffset, sizeof values);
    auto* bitmap = reinterpret_cast<uint64_t*>(raw + kDictionaryBitmapOffset);
    const uint64_t mask = (1ULL << header->scale) - 1;
    const size_t stride = witnesses_of(value_type)->stride;
    const size_t size = witnesses_of(value_type)->size;
    for (const auto& [key, value] : entries) {
        const SwiftString text = swift_string_from_utf8(key);
        HasherState hasher;
        hasher_seed(&hasher, header->seed);
        string_hash_into(&hasher, text);
        uint64_t bucket = static_cast<uint64_t>(hasher_finalize(&hasher)) & mask;
        while ((bitmap[bucket / kWordBits] >> (bucket % kWordBits)) & 1) bucket = (bucket + 1) & mask;
        bitmap[bucket / kWordBits] |= 1ULL << (bucket % kWordBits);
        memcpy(keys + bucket * sizeof(SwiftString), &text, sizeof text);
        memcpy(values + bucket * stride, value, size);
    }
    header->count = static_cast<int64_t>(entries.size());
    return raw;
}

void* make_string_bool_dictionary(const std::vector<std::pair<std::string, bool>>& entries) {
    std::vector<uint8_t> flags;
    flags.reserve(entries.size());
    std::vector<std::pair<std::string, const void*>> values;
    for (const auto& [key, value] : entries) {
        flags.push_back(value ? 1 : 0);
        values.emplace_back(key, &flags.back());
    }
    return make_string_dictionary(type_system_symbol("$sSbN"), values);
}

void visit_string_dictionary(void* storage, const std::function<void(const std::string&, void*)>& visit) {
    auto* raw = static_cast<char*>(storage);
    auto* header = reinterpret_cast<HashedHeader*>(raw);
    if (header->count == 0) return;
    char* keys;
    char* values;
    memcpy(&keys, raw + kDictionaryKeysOffset, sizeof keys);
    memcpy(&values, raw + kDictionaryValuesOffset, sizeof values);
    const size_t stride = witnesses_of(arguments_of(header->metadata)[1])->stride;
    for_each_occupied(raw + kDictionaryBitmapOffset, header->scale, [&](size_t bucket) {
        SwiftString key;
        memcpy(&key, keys + bucket * sizeof key, sizeof key);
        visit(swift_string_to_utf8(key), values + bucket * stride);
    });
}

void* make_array_storage(void* element_type, size_t count, char** elements) {
    const auto* witnesses = witnesses_of(element_type);
    const size_t elements_at = align_up(kArrayElementsOffset, alignment_mask(witnesses));
    void* arguments[] = {element_type};
    auto* raw = static_cast<char*>(calloc(1, elements_at + count * witnesses->stride));
    auto* header = reinterpret_cast<HashedHeader*>(raw);
    header->metadata = storage_class_metadata("_$ss23_ContiguousArrayStorageCMn", arguments, 1);
    new (&header->references) std::atomic<uint64_t>(1);
    header->count = static_cast<int64_t>(count);
    header->capacity = static_cast<int64_t>(count * 2);
    *elements = raw + elements_at;
    return raw;
}

size_t array_storage_contents(void* storage, void* element_type, char** elements) {
    auto* raw = static_cast<char*>(storage);
    *elements = raw + align_up(kArrayElementsOffset, alignment_mask(witnesses_of(element_type)));
    return static_cast<size_t>(reinterpret_cast<HashedHeader*>(raw)->count);
}
