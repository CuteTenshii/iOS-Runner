#include "stdlib_internal.h"

using namespace stdlib;

uint64_t* empty_set_singleton();

namespace stdlib {

constexpr double kMaximumLoadFactor = 0.75;

constexpr size_t kSetElementsOffset = sizeof(HashedHeader);

constexpr size_t kSetBitmapOffset = kSetElementsOffset + sizeof(void*);

void** arguments_of(const void* metadata) { return const_cast<void**>(static_cast<void* const*>(metadata)) + kMetadataArgumentsWord; }

int scale_for_capacity(int64_t capacity) {
    int scale = 0;
    while (static_cast<double>(1LL << scale) * kMaximumLoadFactor < static_cast<double>(capacity)) ++scale;
    return scale;
}

size_t align_up(size_t value, size_t mask) { return (value + mask) & ~mask; }

size_t bitmap_bytes(int scale) {
    const size_t buckets = 1ULL << scale;
    return ((buckets + kWordBits - 1) / kWordBits) * sizeof(uint64_t);
}

size_t alignment_mask(const ValueWitnessTable* witnesses) { return witnesses->flags & 0xff; }

void init_hashed_header(char* raw, void* metadata, int scale, int32_t age, int64_t seed, bool has_seed) {
    auto* header = reinterpret_cast<HashedHeader*>(raw);
    header->metadata = metadata;
    new (&header->references) std::atomic<uint64_t>(1);
    header->capacity = static_cast<int64_t>(static_cast<double>(1ULL << scale) * kMaximumLoadFactor);
    header->scale = static_cast<int8_t>(scale);
    header->age = age;
    header->seed = has_seed ? seed : reinterpret_cast<int64_t>(raw);
}

// Guest code searches the bitmap for a free bucket inline, and it's the set bits past the last bucket that stop it.
void mark_buckets_past_end(char* bitmap, int scale) {
    constexpr int kScaleFillingOneWord = 6;
    if (scale >= kScaleFillingOneWord) return;
    const uint64_t past_end = ~0ULL << (1ULL << scale);
    memcpy(bitmap, &past_end, sizeof past_end);
}

void* allocate_set(void* metadata, int scale, int32_t age, int64_t seed, bool has_seed) {
    void* element = arguments_of(metadata)[0];
    const auto* witnesses = witnesses_of(element);
    const size_t buckets = 1ULL << scale;
    const size_t elements_at = align_up(kSetBitmapOffset + bitmap_bytes(scale), alignment_mask(witnesses));
    auto* raw = static_cast<char*>(calloc(1, elements_at + buckets * witnesses->stride));
    init_hashed_header(raw, metadata, scale, age, seed, has_seed);
    mark_buckets_past_end(raw + kSetBitmapOffset, scale);
    void* elements = raw + elements_at;
    memcpy(raw + kSetElementsOffset, &elements, sizeof elements);
    return raw;
}

void* allocate_dictionary(void* metadata, int scale, int32_t age, int64_t seed, bool has_seed) {
    void* key = arguments_of(metadata)[0];
    void* value = arguments_of(metadata)[1];
    const auto* key_witnesses = witnesses_of(key);
    const auto* value_witnesses = witnesses_of(value);
    const size_t buckets = 1ULL << scale;
    const size_t keys_at = align_up(kDictionaryBitmapOffset + bitmap_bytes(scale), alignment_mask(key_witnesses));
    const size_t values_at = align_up(keys_at + buckets * key_witnesses->stride, alignment_mask(value_witnesses));
    auto* raw = static_cast<char*>(calloc(1, values_at + buckets * value_witnesses->stride));
    init_hashed_header(raw, metadata, scale, age, seed, has_seed);
    mark_buckets_past_end(raw + kDictionaryBitmapOffset, scale);
    void* keys = raw + keys_at;
    void* values = raw + values_at;
    memcpy(raw + kDictionaryKeysOffset, &keys, sizeof keys);
    memcpy(raw + kDictionaryValuesOffset, &values, sizeof values);
    return raw;
}

void destroy_set_storage(void* raw) {
    auto* header = static_cast<HashedHeader*>(raw);
    void* element = arguments_of(header->metadata)[0];
    const auto* witnesses = witnesses_of(element);
    char* elements;
    memcpy(&elements, static_cast<char*>(raw) + kSetElementsOffset, sizeof elements);
    for_each_occupied(static_cast<char*>(raw) + kSetBitmapOffset, header->scale, [&](size_t bucket) {
        witnesses->destroy(elements + bucket * witnesses->stride, element);
    });
    free(raw);
}

void destroy_dictionary_storage(void* raw) {
    auto* header = static_cast<HashedHeader*>(raw);
    void* key = arguments_of(header->metadata)[0];
    void* value = arguments_of(header->metadata)[1];
    const auto* key_witnesses = witnesses_of(key);
    const auto* value_witnesses = witnesses_of(value);
    char* keys;
    char* values;
    memcpy(&keys, static_cast<char*>(raw) + kDictionaryKeysOffset, sizeof keys);
    memcpy(&values, static_cast<char*>(raw) + kDictionaryValuesOffset, sizeof values);
    for_each_occupied(static_cast<char*>(raw) + kDictionaryBitmapOffset, header->scale, [&](size_t bucket) {
        key_witnesses->destroy(keys + bucket * key_witnesses->stride, key);
        value_witnesses->destroy(values + bucket * value_witnesses->stride, value);
    });
    free(raw);
}

void destroy_array_storage(void* raw) {
    auto* header = static_cast<HashedHeader*>(raw);
    void* element = arguments_of(header->metadata)[0];
    const auto* witnesses = witnesses_of(element);
    char* elements = static_cast<char*>(raw) + align_up(kArrayElementsOffset, alignment_mask(witnesses));
    for (int64_t i = 0; i < header->count; ++i) witnesses->destroy(elements + static_cast<size_t>(i) * witnesses->stride, element);
    free(raw);
}

SWIFTCC void nsobject_hash_into(HasherState* hasher, SWIFT_CONTEXT void* self);

// A Set of object references, as bridging an NSSet makes. They hash as NSObjects do: by address.
void* build_object_set(const std::vector<void*>& objects, void* type, void* hashable) {
    if (objects.empty()) return resolve_symbol("_swiftEmptySetSingleton");
    void* arguments[] = {type, hashable};
    void* metadata = storage_class_metadata("_$ss11_SetStorageCMn", arguments, 2);
    auto* raw = static_cast<char*>(allocate_set(metadata, scale_for_capacity(static_cast<int64_t>(objects.size())), 0, 0, false));
    auto* header = reinterpret_cast<HashedHeader*>(raw);
    void** elements;
    memcpy(&elements, raw + kSetElementsOffset, sizeof elements);
    auto* bitmap = reinterpret_cast<uint64_t*>(raw + kSetBitmapOffset);
    const uint64_t mask = (1ULL << header->scale) - 1;
    for (void* object : objects) {
        HasherState hasher;
        hasher_seed(&hasher, header->seed);
        nsobject_hash_into(&hasher, object);
        uint64_t bucket = static_cast<uint64_t>(hasher_finalize(&hasher)) & mask;
        while ((bitmap[bucket / 64] >> (bucket % 64)) & 1) bucket = (bucket + 1) & mask;
        bitmap[bucket / 64] |= 1ULL << (bucket % 64);
        elements[bucket] = rt_swift_retain(object);
    }
    header->count = static_cast<int64_t>(objects.size());
    return raw;
}

SWIFTCC void* set_storage_allocate(int64_t capacity, SWIFT_CONTEXT void* metadata) {
    return allocate_set(metadata, scale_for_capacity(capacity), 0, 0, false);
}

SWIFTCC void* set_storage_copy(HashedHeader* original, SWIFT_CONTEXT void* metadata) {
    return allocate_set(metadata, original->scale, original->age, original->seed, true);
}

SWIFTCC void* set_storage_resize(HashedHeader* original, int64_t capacity, bool, SWIFT_CONTEXT void* metadata) {
    return allocate_set(metadata, scale_for_capacity(capacity), original->age, 0, false);
}

SWIFTCC void* dictionary_storage_allocate(int64_t capacity, SWIFT_CONTEXT void* metadata) {
    return allocate_dictionary(metadata, scale_for_capacity(capacity), 0, 0, false);
}

SWIFTCC void* dictionary_storage_copy(HashedHeader* original, SWIFT_CONTEXT void* metadata) {
    return allocate_dictionary(metadata, original->scale, original->age, original->seed, true);
}

SWIFTCC void* dictionary_storage_resize(HashedHeader* original, int64_t capacity, bool, SWIFT_CONTEXT void* metadata) {
    return allocate_dictionary(metadata, scale_for_capacity(capacity), original->age, 0, false);
}

SWIFTCC void* set_minimum_capacity(int64_t capacity, void* element, void*) {
    if (capacity == 0) return empty_set_singleton();
    void* arguments[] = {element};
    return allocate_set(storage_class_metadata("_$ss11_SetStorageCMn", arguments, 1), scale_for_capacity(capacity), 0, 0, false);
}

constexpr uint64_t kFnvOffset = 0xcbf29ce484222325ULL;

constexpr uint64_t kFnvPrime = 0x100000001b3ULL;

uint64_t mix_bytes(uint64_t state, const void* bytes, size_t count) {
    const auto* cursor = static_cast<const uint8_t*>(bytes);
    for (size_t i = 0; i < count; ++i) state = (state ^ cursor[i]) * kFnvPrime;
    return state;
}

SWIFTCC void hasher_seed(SWIFT_OUT HasherState* out, int64_t seed) {
    memset(out, 0, sizeof *out);
    out->state = mix_bytes(kFnvOffset, &seed, sizeof seed);
}

SWIFTCC void hasher_combine_word(uint64_t value, SWIFT_CONTEXT HasherState* self) { self->state = mix_bytes(self->state, &value, sizeof value); }

SWIFTCC void hasher_combine_uint16(uint16_t value, SWIFT_CONTEXT HasherState* self) { self->state = mix_bytes(self->state, &value, sizeof value); }

SWIFTCC void hasher_combine_uint32(uint32_t value, SWIFT_CONTEXT HasherState* self) { self->state = mix_bytes(self->state, &value, sizeof value); }

SWIFTCC void hasher_combine_byte(uint8_t value, SWIFT_CONTEXT HasherState* self) { self->state = mix_bytes(self->state, &value, sizeof value); }

SWIFTCC int64_t hasher_finalize(SWIFT_CONTEXT HasherState* self) {
    // Finalizing must also scramble high bits: bucket selection uses the low bits only.
    uint64_t value = self->state;
    value ^= value >> 33;
    value *= 0xff51afd7ed558ccdULL;
    value ^= value >> 33;
    return static_cast<int64_t>(value);
}

SWIFTCC int64_t hasher_hash_seed_word(int64_t seed, uint64_t value) {
    HasherState state;
    hasher_seed(&state, seed);
    hasher_combine_word(value, &state);
    return hasher_finalize(&state);
}

// NSObject's default `hash` is the identity, and no shim overrides it.
SWIFTCC void nsobject_hash_into(HasherState* hasher, SWIFT_CONTEXT void* self) { hasher_combine_word(reinterpret_cast<uint64_t>(self), hasher); }

SWIFTCC int64_t nsobject_raw_hash_value(int64_t seed, SWIFT_CONTEXT void* self) { return hasher_hash_seed_word(seed, reinterpret_cast<uint64_t>(self)); }

SWIFTCC void string_hash_into(HasherState* hasher, SwiftString value) {
    const std::string text = swift_string_to_utf8(value);
    hasher->state = mix_bytes(hasher->state, text.data(), text.size());
    const uint8_t terminator = 0xff;
    hasher->state = mix_bytes(hasher->state, &terminator, 1);
}

SWIFTCC bool string_equal_slow(SwiftString left, SwiftString right, uint8_t expecting) {
    const std::string a = swift_string_to_utf8(left);
    const std::string b = swift_string_to_utf8(right);
    return expecting == 0 ? a == b : a < b;
}

constexpr uint64_t kIndexFlags = 0x7;

constexpr unsigned kIndexOffsetShift = 16;

uint64_t make_index(size_t offset) { return (static_cast<uint64_t>(offset) << kIndexOffsetShift) | kIndexFlags; }

size_t index_offset(uint64_t index) { return static_cast<size_t>(index >> kIndexOffsetShift); }

size_t scalar_length(unsigned char lead) {
    if (lead < 0x80) return 1;
    if (lead < 0xe0) return 2;
    if (lead < 0xf0) return 3;
    return 4;
}

size_t next_scalar(const std::string& text, size_t offset) {
    return offset >= text.size() ? text.size() : offset + scalar_length(static_cast<unsigned char>(text[offset]));
}

size_t previous_scalar(const std::string& text, size_t offset) {
    if (offset == 0) return 0;
    --offset;
    while (offset > 0 && (static_cast<unsigned char>(text[offset]) & 0xc0) == 0x80) --offset;
    return offset;
}

void add_stdlib_collections_symbols(SymbolTable& table) {
    table.insert({
        {"$sSh15minimumCapacityShyxGSi_tcfC", reinterpret_cast<void*>(set_minimum_capacity)},
        {"$ss11_SetStorageC8allocate8capacityAByxGSi_tFZ", reinterpret_cast<void*>(set_storage_allocate)},
        {"$ss11_SetStorageC4copy8originalAByxGs05__RawaB0C_tFZ", reinterpret_cast<void*>(set_storage_copy)},
        {"$ss11_SetStorageC6resize8original8capacity4moveAByxGs05__RawaB0C_SiSbtFZ", reinterpret_cast<void*>(set_storage_resize)},
        {"$ss18_DictionaryStorageC8allocate8capacityAByxq_GSi_tFZ", reinterpret_cast<void*>(dictionary_storage_allocate)},
        {"$ss18_DictionaryStorageC4copy8originalAByxq_Gs05__RawaB0C_tFZ", reinterpret_cast<void*>(dictionary_storage_copy)},
        {"$ss18_DictionaryStorageC6resize8original8capacity4moveAByxq_Gs05__RawaB0C_SiSbtFZ", reinterpret_cast<void*>(dictionary_storage_resize)},
        {"$ss6HasherV5_seedABSi_tcfC", reinterpret_cast<void*>(hasher_seed)},
        {"$sSo8NSObjectC10ObjectiveCE4hash4intoys6HasherVz_tF", reinterpret_cast<void*>(nsobject_hash_into)},
        {"$sSo8NSObjectC10ObjectiveCE13_rawHashValue4seedS2i_tF", reinterpret_cast<void*>(nsobject_raw_hash_value)},
        {"$ss6HasherV8_combineyySuF", reinterpret_cast<void*>(hasher_combine_word)},
        {"$ss6HasherV8_combineyys5UInt8VF", reinterpret_cast<void*>(hasher_combine_byte)},
        {"$ss6HasherV8_combineyys6UInt16VF", reinterpret_cast<void*>(hasher_combine_uint16)},
        {"$ss6HasherV8_combineyys6UInt32VF", reinterpret_cast<void*>(hasher_combine_uint32)},
        {"$ss6HasherV8_combineyys6UInt64VF", reinterpret_cast<void*>(hasher_combine_word)},
        {"$ss6HasherV9_finalizeSiyF", reinterpret_cast<void*>(hasher_finalize)},
        {"$ss6HasherV5_hash4seed_S2i_s6UInt64VtFZ", reinterpret_cast<void*>(hasher_hash_seed_word)},
        {"$sSS4hash4intoys6HasherVz_tF", reinterpret_cast<void*>(string_hash_into)},
        {"$ss27_stringCompareWithSmolCheck__9expectingSbs11_StringGutsV_ADs01_G16ComparisonResultOtF", reinterpret_cast<void*>(string_equal_slow)},
    });
}

}  // namespace stdlib

void* set_from_objects(const std::vector<void*>& objects, void* type, void* hashable) { return stdlib::build_object_set(objects, type, hashable); }

void visit_set_elements(void* storage, const std::function<void(void*)>& visit) {
    using namespace stdlib;
    auto* raw = static_cast<char*>(storage);
    auto* header = reinterpret_cast<HashedHeader*>(raw);
    if (header->count == 0) return;
    char* elements;
    memcpy(&elements, raw + kSetElementsOffset, sizeof elements);
    const size_t stride = witnesses_of(arguments_of(header->metadata)[0])->stride;
    for_each_occupied(raw + kSetBitmapOffset, header->scale, [&](size_t bucket) { visit(elements + bucket * stride); });
}
