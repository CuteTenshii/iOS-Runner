#include "stdlib_internal.h"

#include <type_traits>
#include "symbols.h"

using namespace stdlib;

SWIFTCC void* dictionary_literal(void* elements, void* key_type, void* value_type, void** hashable);

template <typename Integer>
SWIFTCC SwiftString integer_description(SWIFT_CONTEXT const Integer* self) {
    return swift_string_from_utf8(std::to_string(static_cast<std::conditional_t<std::is_signed_v<Integer>, int64_t, uint64_t>>(*self)));
}

SWIFTCC bool native_dictionary_ensure_unique(bool is_unique, int64_t capacity, void* type, SWIFT_CONTEXT char** self);

// As Date prints: UTC, whatever the device's time zone.
SWIFTCC SwiftString date_description(SWIFT_CONTEXT const double* self) {
    const auto seconds = static_cast<time_t>(*self + kSecondsFrom1970To2001);
    tm parts{};
    gmtime_r(&seconds, &parts);
    char text[40];
    strftime(text, sizeof text, "%Y-%m-%d %H:%M:%S +0000", &parts);
    return swift_string_from_utf8(text);
}

static SymbolTable all_stdlib_symbols() {
    SymbolTable all = {
        {"$ss17_NativeDictionaryV12ensureUnique02isD08capacityS2b_SitF", reinterpret_cast<void*>(native_dictionary_ensure_unique)},
        {"$sSD17dictionaryLiteralSDyxq_Gx_q_td_tcfC", reinterpret_cast<void*>(dictionary_literal)},
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
        register_foreign_witness("_$sSus23CustomStringConvertiblesWP", "_$ss23CustomStringConvertibleP11descriptionSSvgTq", reinterpret_cast<const void*>(integer_description<uint64_t>));
        register_foreign_witness("_$ss4Int8Vs23CustomStringConvertiblesWP", "_$ss23CustomStringConvertibleP11descriptionSSvgTq", reinterpret_cast<const void*>(integer_description<int8_t>));
        register_foreign_witness("_$ss5Int16Vs23CustomStringConvertiblesWP", "_$ss23CustomStringConvertibleP11descriptionSSvgTq", reinterpret_cast<const void*>(integer_description<int16_t>));
        register_foreign_witness("_$ss5Int32Vs23CustomStringConvertiblesWP", "_$ss23CustomStringConvertibleP11descriptionSSvgTq", reinterpret_cast<const void*>(integer_description<int32_t>));
        register_foreign_witness("_$ss5Int64Vs23CustomStringConvertiblesWP", "_$ss23CustomStringConvertibleP11descriptionSSvgTq", reinterpret_cast<const void*>(integer_description<int64_t>));
        register_foreign_witness("_$ss5UInt8Vs23CustomStringConvertiblesWP", "_$ss23CustomStringConvertibleP11descriptionSSvgTq", reinterpret_cast<const void*>(integer_description<uint8_t>));
        register_foreign_witness("_$ss6UInt16Vs23CustomStringConvertiblesWP", "_$ss23CustomStringConvertibleP11descriptionSSvgTq", reinterpret_cast<const void*>(integer_description<uint16_t>));
        register_foreign_witness("_$ss6UInt32Vs23CustomStringConvertiblesWP", "_$ss23CustomStringConvertibleP11descriptionSSvgTq", reinterpret_cast<const void*>(integer_description<uint32_t>));
        register_foreign_witness("_$ss6UInt64Vs23CustomStringConvertiblesWP", "_$ss23CustomStringConvertibleP11descriptionSSvgTq", reinterpret_cast<const void*>(integer_description<uint64_t>));
        register_foreign_witness("_$s10Foundation4DateVs23CustomStringConvertibleAAMc", "_$ss23CustomStringConvertibleP11descriptionSSvgTq", reinterpret_cast<const void*>(date_description));
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

using RawHashThunk = SWIFTCC int64_t (*)(int64_t, void*, void*, SWIFT_CONTEXT const void*);

// String keys must land where the guest's specialized lookups hash them, through Hasher.
int64_t hash_for_bucket(const void* key, int64_t seed, void* key_type, void* hashable) {
    if (key_type == type_system_symbol("$sSSN")) {
        HasherState hasher;
        hasher_seed(&hasher, seed);
        string_hash_into(&hasher, *static_cast<const SwiftString*>(key));
        return hasher_finalize(&hasher);
    }
    static const auto thunk = reinterpret_cast<RawHashThunk>(resolve_symbol("$sSH13_rawHashValue4seedS2i_tFTj"));
    return thunk(seed, key_type, hashable, key);
}

// Takes the array, as a variadic literal hands over its only reference.
SWIFTCC void* dictionary_literal(void* elements, void* key_type, void* value_type, void** hashable) {
    int64_t count;
    memcpy(&count, static_cast<char*>(elements) + kArrayCountOffset, sizeof count);
    void* arguments[] = {key_type, value_type, hashable};
    void* metadata = storage_class_metadata("_$ss18_DictionaryStorageCMn", arguments, 3);
    auto* raw = static_cast<char*>(allocate_dictionary(metadata, scale_for_capacity(count), 0, 0, false));
    auto* header = reinterpret_cast<HashedHeader*>(raw);
    char* keys;
    char* values;
    memcpy(&keys, raw + kDictionaryKeysOffset, sizeof keys);
    memcpy(&values, raw + kDictionaryValuesOffset, sizeof values);
    auto* bitmap = reinterpret_cast<uint64_t*>(raw + kDictionaryBitmapOffset);
    const uint64_t mask = (1ULL << header->scale) - 1;
    const auto* key_witnesses = witnesses_of(key_type);
    const auto* value_witnesses = witnesses_of(value_type);
    const size_t value_mask = value_witnesses->flags & 0xff;
    const size_t value_offset = (key_witnesses->size + value_mask) & ~value_mask;
    const size_t tuple_mask = (key_witnesses->flags & 0xff) > value_mask ? (key_witnesses->flags & 0xff) : value_mask;
    const size_t stride = (value_offset + value_witnesses->size + tuple_mask) & ~tuple_mask;
    char* element = static_cast<char*>(elements) + ((kArrayElementsOffset + tuple_mask) & ~tuple_mask);
    for (int64_t i = 0; i < count; ++i, element += stride) {
        uint64_t bucket = static_cast<uint64_t>(hash_for_bucket(element, header->seed, key_type, hashable)) & mask;
        while ((bitmap[bucket / kWordBits] >> (bucket % kWordBits)) & 1) bucket = (bucket + 1) & mask;
        bitmap[bucket / kWordBits] |= 1ULL << (bucket % kWordBits);
        key_witnesses->initialize_with_copy(keys + bucket * key_witnesses->stride, element, key_type);
        value_witnesses->initialize_with_copy(values + bucket * value_witnesses->stride, element + value_offset, value_type);
    }
    header->count = count;
    using Release = void (*)(void*);
    reinterpret_cast<Release>(resolve_symbol("swift_release"))(elements);
    return raw;
}

// Grows or unshares the storage in place, returning whether its buckets moved, in which case callers must find theirs again.
SWIFTCC bool native_dictionary_ensure_unique(bool is_unique, int64_t capacity, void* type, SWIFT_CONTEXT char** self) {
    auto* old_raw = *self;
    auto* old_header = reinterpret_cast<HashedHeader*>(old_raw);
    const bool grow = capacity > old_header->capacity;
    if (!grow && is_unique) return false;
    void** generics = arguments_of(type);
    void* key_type = generics[0];
    void* value_type = generics[1];
    // Metadata resolved from a mangled name holds only the two types, so the conformance has to be looked up.
    static const void* const hashable_protocol = unresolved_symbol_named("_$sSHMp");
    void** hashable = hashable_protocol != nullptr ? static_cast<void**>(swift_runtime_find_conformance(key_type, hashable_protocol)) : nullptr;
    void* arguments[] = {key_type, value_type, hashable};
    void* metadata = storage_class_metadata("_$ss18_DictionaryStorageCMn", arguments, 3);
    const int scale = grow ? scale_for_capacity(capacity) : old_header->scale;
    auto* raw = static_cast<char*>(allocate_dictionary(metadata, scale, old_header->age, old_header->seed, !grow));
    auto* header = reinterpret_cast<HashedHeader*>(raw);
    char* keys;
    char* values;
    memcpy(&keys, raw + kDictionaryKeysOffset, sizeof keys);
    memcpy(&values, raw + kDictionaryValuesOffset, sizeof values);
    auto* bitmap = reinterpret_cast<uint64_t*>(raw + kDictionaryBitmapOffset);
    const uint64_t mask = (1ULL << header->scale) - 1;
    const auto* key_witnesses = witnesses_of(key_type);
    const auto* value_witnesses = witnesses_of(value_type);
    if (old_header->count > 0) {
        char* old_keys;
        char* old_values;
        memcpy(&old_keys, old_raw + kDictionaryKeysOffset, sizeof old_keys);
        memcpy(&old_values, old_raw + kDictionaryValuesOffset, sizeof old_values);
        const auto* old_bitmap = reinterpret_cast<const uint64_t*>(old_raw + kDictionaryBitmapOffset);
        const uint64_t old_buckets = 1ULL << old_header->scale;
        for (uint64_t i = 0; i < old_buckets; ++i) {
            if (!((old_bitmap[i / kWordBits] >> (i % kWordBits)) & 1)) continue;
            const char* key = old_keys + i * key_witnesses->stride;
            uint64_t bucket = i;
            if (grow) {
                bucket = static_cast<uint64_t>(hash_for_bucket(key, header->seed, key_type, hashable)) & mask;
                while ((bitmap[bucket / kWordBits] >> (bucket % kWordBits)) & 1) bucket = (bucket + 1) & mask;
            }
            bitmap[bucket / kWordBits] |= 1ULL << (bucket % kWordBits);
            key_witnesses->initialize_with_copy(keys + bucket * key_witnesses->stride, const_cast<char*>(key), key_type);
            value_witnesses->initialize_with_copy(values + bucket * value_witnesses->stride, old_values + i * value_witnesses->stride, value_type);
        }
    }
    header->count = old_header->count;
    *self = raw;
    using Release = void (*)(void*);
    reinterpret_cast<Release>(resolve_symbol("swift_release"))(old_raw);
    return grow;
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
