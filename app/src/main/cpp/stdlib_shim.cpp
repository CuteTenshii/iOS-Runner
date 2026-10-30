#include "stdlib_shim.h"

#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <new>
#include <string>
#include <unordered_map>

#include "engine.h"
#include "foundation_shim.h"
#include "macho.h"
#include "type_system.h"

#define SWIFTCC __attribute__((swiftcall))
#define SWIFT_OUT __attribute__((swift_indirect_result))
#define SWIFT_SELF __attribute__((swift_context))

uint64_t* empty_set_singleton();

namespace {

struct ValueWitnessTable {
    void* (*initialize_buffer_with_copy)(void*, void*, void*);
    void (*destroy)(void*, void*);
    void* (*initialize_with_copy)(void*, void*, void*);
    void* (*assign_with_copy)(void*, void*, void*);
    void* (*initialize_with_take)(void*, void*, void*);
    void* (*assign_with_take)(void*, void*, void*);
    uint32_t (*get_enum_tag_single_payload)(const void*, uint32_t, void*);
    void (*store_enum_tag_single_payload)(void*, uint32_t, uint32_t, void*);
    size_t size;
    size_t stride;
    uint32_t flags;
    uint32_t extra_inhabitants;
};

const ValueWitnessTable* witnesses_of(const void* type) {
    return static_cast<const ValueWitnessTable*>(static_cast<void* const*>(type)[-1]);
}

// Native hashed storage, laid out as the guest's inlined Set and Dictionary code expects.
struct HashedHeader {
    void* metadata;
    std::atomic<uint64_t> references;
    int64_t count;
    int64_t capacity;
    int8_t scale;
    int8_t reserved_scale;
    int16_t extra;
    int32_t age;
    int64_t seed;
};

constexpr size_t kWordBits = 64;
constexpr double kMaximumLoadFactor = 0.75;
constexpr size_t kSetElementsOffset = sizeof(HashedHeader);
constexpr size_t kSetBitmapOffset = kSetElementsOffset + sizeof(void*);
constexpr size_t kDictionaryKeysOffset = sizeof(HashedHeader);
constexpr size_t kDictionaryValuesOffset = kDictionaryKeysOffset + sizeof(void*);
constexpr size_t kDictionaryBitmapOffset = kDictionaryValuesOffset + sizeof(void*);
constexpr size_t kMetadataArgumentsWord = 2;
constexpr size_t kArrayElementsOffset = 32;

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

void* allocate_set(void* metadata, int scale, int32_t age, int64_t seed, bool has_seed) {
    void* element = arguments_of(metadata)[0];
    const auto* witnesses = witnesses_of(element);
    const size_t buckets = 1ULL << scale;
    const size_t elements_at = align_up(kSetBitmapOffset + bitmap_bytes(scale), alignment_mask(witnesses));
    auto* raw = static_cast<char*>(calloc(1, elements_at + buckets * witnesses->stride));
    auto* header = reinterpret_cast<HashedHeader*>(raw);
    header->metadata = metadata;
    new (&header->references) std::atomic<uint64_t>(1);
    header->capacity = static_cast<int64_t>(static_cast<double>(buckets) * kMaximumLoadFactor);
    header->scale = static_cast<int8_t>(scale);
    header->reserved_scale = 0;
    header->age = age;
    header->seed = has_seed ? seed : reinterpret_cast<int64_t>(raw);
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
    auto* header = reinterpret_cast<HashedHeader*>(raw);
    header->metadata = metadata;
    new (&header->references) std::atomic<uint64_t>(1);
    header->capacity = static_cast<int64_t>(static_cast<double>(buckets) * kMaximumLoadFactor);
    header->scale = static_cast<int8_t>(scale);
    header->age = age;
    header->seed = has_seed ? seed : reinterpret_cast<int64_t>(raw);
    void* keys = raw + keys_at;
    void* values = raw + values_at;
    memcpy(raw + kDictionaryKeysOffset, &keys, sizeof keys);
    memcpy(raw + kDictionaryValuesOffset, &values, sizeof values);
    return raw;
}

template <typename Visit>
void for_each_occupied(const char* bitmap, int scale, Visit visit) {
    const size_t buckets = 1ULL << scale;
    for (size_t bucket = 0; bucket < buckets; ++bucket) {
        uint64_t word;
        memcpy(&word, bitmap + bucket / kWordBits * sizeof word, sizeof word);
        if ((word >> (bucket % kWordBits)) & 1) visit(bucket);
    }
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

SWIFTCC void* set_storage_allocate(int64_t capacity, SWIFT_SELF void* metadata) {
    return allocate_set(metadata, scale_for_capacity(capacity), 0, 0, false);
}

SWIFTCC void* set_storage_copy(HashedHeader* original, SWIFT_SELF void* metadata) {
    return allocate_set(metadata, original->scale, original->age, original->seed, true);
}

SWIFTCC void* set_storage_resize(HashedHeader* original, int64_t capacity, bool, SWIFT_SELF void* metadata) {
    return allocate_set(metadata, scale_for_capacity(capacity), original->age, 0, false);
}

SWIFTCC void* dictionary_storage_allocate(int64_t capacity, SWIFT_SELF void* metadata) {
    return allocate_dictionary(metadata, scale_for_capacity(capacity), 0, 0, false);
}

SWIFTCC void* dictionary_storage_copy(HashedHeader* original, SWIFT_SELF void* metadata) {
    return allocate_dictionary(metadata, original->scale, original->age, original->seed, true);
}

SWIFTCC void* dictionary_storage_resize(HashedHeader* original, int64_t capacity, bool, SWIFT_SELF void* metadata) {
    return allocate_dictionary(metadata, scale_for_capacity(capacity), original->age, 0, false);
}

SWIFTCC void* set_minimum_capacity(int64_t capacity, void* element, void*) {
    if (capacity == 0) return empty_set_singleton();
    void* arguments[] = {element};
    return allocate_set(storage_class_metadata("_$ss11_SetStorageCMn", arguments, 1), scale_for_capacity(capacity), 0, 0, false);
}

constexpr uint64_t kFnvOffset = 0xcbf29ce484222325ULL;
constexpr uint64_t kFnvPrime = 0x100000001b3ULL;

// Hasher is frozen at 72 bytes; only its first word is used, since hash values are private to one process.
struct HasherState {
    uint64_t state;
    uint64_t unused[8];
};

uint64_t mix_bytes(uint64_t state, const void* bytes, size_t count) {
    const auto* cursor = static_cast<const uint8_t*>(bytes);
    for (size_t i = 0; i < count; ++i) state = (state ^ cursor[i]) * kFnvPrime;
    return state;
}

SWIFTCC void hasher_seed(SWIFT_OUT HasherState* out, int64_t seed) {
    memset(out, 0, sizeof *out);
    out->state = mix_bytes(kFnvOffset, &seed, sizeof seed);
}

SWIFTCC void hasher_combine_word(uint64_t value, SWIFT_SELF HasherState* self) { self->state = mix_bytes(self->state, &value, sizeof value); }
SWIFTCC void hasher_combine_byte(uint8_t value, SWIFT_SELF HasherState* self) { self->state = mix_bytes(self->state, &value, sizeof value); }

SWIFTCC int64_t hasher_finalize(SWIFT_SELF HasherState* self) {
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


void* get_object_type(void* object) { return *static_cast<void**>(object); }

constexpr size_t kClassSuperOffset = 8;

void* dynamic_cast_objc_class(void* object, void* target) {
    if (object == nullptr) return nullptr;
    const char* cls = static_cast<const char*>(*static_cast<void**>(object));
    while (cls != nullptr) {
        if (cls == target) return object;
        if (unresolved_symbol_at(reinterpret_cast<uintptr_t>(cls)) != nullptr) return nullptr;
        if (foreign_type_name(cls) != nullptr) return nullptr;
        memcpy(&cls, cls + kClassSuperOffset, sizeof cls);
    }
    return nullptr;
}

void array_destroy(char* start, size_t count, void* type) {
    const auto* witnesses = witnesses_of(type);
    for (size_t i = 0; i < count; ++i) witnesses->destroy(start + i * witnesses->stride, type);
}

void* array_init_with_copy(char* destination, char* source, size_t count, void* type) {
    const auto* witnesses = witnesses_of(type);
    for (size_t i = 0; i < count; ++i) witnesses->initialize_with_copy(destination + i * witnesses->stride, source + i * witnesses->stride, type);
    return destination;
}

constexpr size_t kEnumPayloadInfoOffset = 20;
constexpr uint32_t kEnumPayloadCaseMask = 0x00ffffff;
constexpr uint32_t kEnumPayloadOffsetShift = 24;
constexpr size_t kMetadataDescriptorWord = 1;
constexpr size_t kSmallPayloadLimit = 4;

struct EnumShape {
    uint32_t payload_cases;
    size_t payload_size;
    size_t tag_bytes;
};

EnumShape enum_shape(void* type) {
    auto** metadata = static_cast<void**>(type);
    const auto* descriptor = static_cast<const char*>(metadata[kMetadataDescriptorWord]);
    uint32_t info;
    memcpy(&info, descriptor + kEnumPayloadInfoOffset, sizeof info);
    const uint32_t offset = info >> kEnumPayloadOffsetShift;
    const size_t payload_size = reinterpret_cast<size_t>(metadata[offset]);
    return {info & kEnumPayloadCaseMask, payload_size, witnesses_of(type)->size - payload_size};
}

uint32_t get_enum_case_multi_payload(const char* value, void* type) {
    const auto shape = enum_shape(type);
    uint32_t tag = 0;
    memcpy(&tag, value + shape.payload_size, shape.tag_bytes < 4 ? shape.tag_bytes : 4);
    if (tag < shape.payload_cases) return tag;
    uint32_t low = 0;
    memcpy(&low, value, shape.payload_size < kSmallPayloadLimit ? shape.payload_size : kSmallPayloadLimit);
    if (shape.payload_size >= kSmallPayloadLimit) return shape.payload_cases + low;
    return shape.payload_cases + (((tag - shape.payload_cases) << (shape.payload_size * 8)) | low);
}

void store_enum_tag_multi_payload(char* value, void* type, uint32_t which) {
    const auto shape = enum_shape(type);
    uint32_t tag = which;
    if (which >= shape.payload_cases) {
        const uint32_t index = which - shape.payload_cases;
        if (shape.payload_size >= kSmallPayloadLimit) {
            tag = shape.payload_cases;
            memset(value, 0, shape.payload_size);
            memcpy(value, &index, kSmallPayloadLimit);
        } else {
            tag = shape.payload_cases + (index >> (shape.payload_size * 8));
            memcpy(value, &index, shape.payload_size);
        }
    }
    memcpy(value + shape.payload_size, &tag, shape.tag_bytes < 4 ? shape.tag_bytes : 4);
}

uint32_t tag_bytes_for(size_t payload_size, uint32_t empty_cases) {
    if (payload_size >= kSmallPayloadLimit) return 1;
    const uint64_t cases = ((empty_cases + (1ULL << (payload_size * 8)) - 1) >> (payload_size * 8)) + 1;
    return cases <= 0x100 ? 1 : cases <= 0x10000 ? 2 : 4;
}

using GetExtraInhabitant = uint32_t (*)(const void*, uint32_t, void*);
using StoreExtraInhabitant = void (*)(void*, uint32_t, uint32_t, void*);

uint32_t get_enum_tag_single_payload_generic(const char* value, uint32_t empty_cases, void* payload, GetExtraInhabitant get) {
    const auto* witnesses = witnesses_of(payload);
    const size_t size = witnesses->size;
    const uint32_t inhabitants = witnesses->extra_inhabitants;
    if (empty_cases > inhabitants) {
        const uint32_t count = tag_bytes_for(size, empty_cases - inhabitants);
        uint32_t extra = 0;
        memcpy(&extra, value + size, count);
        if (extra != 0) {
            const uint32_t high = size >= kSmallPayloadLimit ? 0 : (extra - 1) << (size * 8);
            uint32_t low = 0;
            memcpy(&low, value, size < kSmallPayloadLimit ? size : kSmallPayloadLimit);
            return (high | low) + inhabitants + 1;
        }
    }
    return inhabitants == 0 ? 0 : get(value, inhabitants, payload);
}

void store_enum_tag_single_payload_generic(char* value, uint32_t which, uint32_t empty_cases, void* payload, StoreExtraInhabitant store) {
    const auto* witnesses = witnesses_of(payload);
    const size_t size = witnesses->size;
    const uint32_t inhabitants = witnesses->extra_inhabitants;
    const uint32_t count = empty_cases > inhabitants ? tag_bytes_for(size, empty_cases - inhabitants) : 0;
    if (which <= inhabitants) {
        if (count != 0) memset(value + size, 0, count);
        if (which != 0) store(value, which, inhabitants, payload);
        return;
    }
    const uint32_t index = which - inhabitants - 1;
    uint32_t extra = 1;
    if (size < kSmallPayloadLimit) extra += index >> (size * 8);
    memcpy(value + size, &extra, count);
    memset(value, 0, size);
    memcpy(value, &index, size < kSmallPayloadLimit ? size : kSmallPayloadLimit);
}

struct KeyPathObject {
    void* metadata;
    std::atomic<uint64_t> references;
    const void* pattern;
    const void* arguments;
};

void destroy_key_path(void* object) { free(object); }

struct KeyPathMetadata {
    void (*destroyer)(void*);
    const void* witnesses;
    uint64_t kind;
};

const KeyPathMetadata kKeyPathMetadata = {destroy_key_path, nullptr, 0};

void* get_key_path(const void* pattern, const void* arguments) {
    auto* object = static_cast<KeyPathObject*>(calloc(1, sizeof(KeyPathObject)));
    object->metadata = const_cast<uint64_t*>(&kKeyPathMetadata.kind);
    new (&object->references) std::atomic<uint64_t>(1);
    object->pattern = pattern;
    object->arguments = arguments;
    return object;
}

void swift_will_throw(void*) {}

[[noreturn]] void deleted_method_error() {
    engine_log("STOP: guest called a deleted method");
    abort();
}

}  // namespace

void* stdlib_symbol(std::string_view name) {
    static const std::unordered_map<std::string_view, void*> table = {
        {"swift_getObjectType", reinterpret_cast<void*>(get_object_type)},
        {"swift_dynamicCastObjCClass", reinterpret_cast<void*>(dynamic_cast_objc_class)},
        {"swift_arrayDestroy", reinterpret_cast<void*>(array_destroy)},
        {"swift_arrayInitWithCopy", reinterpret_cast<void*>(array_init_with_copy)},
        {"swift_getEnumCaseMultiPayload", reinterpret_cast<void*>(get_enum_case_multi_payload)},
        {"swift_storeEnumTagMultiPayload", reinterpret_cast<void*>(store_enum_tag_multi_payload)},
        {"swift_getEnumTagSinglePayloadGeneric", reinterpret_cast<void*>(get_enum_tag_single_payload_generic)},
        {"swift_storeEnumTagSinglePayloadGeneric", reinterpret_cast<void*>(store_enum_tag_single_payload_generic)},
        {"swift_getKeyPath", reinterpret_cast<void*>(get_key_path)},
        {"swift_willThrow", reinterpret_cast<void*>(swift_will_throw)},
        {"swift_deletedMethodError", reinterpret_cast<void*>(deleted_method_error)},
        {"$sSh15minimumCapacityShyxGSi_tcfC", reinterpret_cast<void*>(set_minimum_capacity)},
        {"$ss11_SetStorageC8allocate8capacityAByxGSi_tFZ", reinterpret_cast<void*>(set_storage_allocate)},
        {"$ss11_SetStorageC4copy8originalAByxGs05__RawaB0C_tFZ", reinterpret_cast<void*>(set_storage_copy)},
        {"$ss11_SetStorageC6resize8original8capacity4moveAByxGs05__RawaB0C_SiSbtFZ", reinterpret_cast<void*>(set_storage_resize)},
        {"$ss18_DictionaryStorageC8allocate8capacityAByxq_GSi_tFZ", reinterpret_cast<void*>(dictionary_storage_allocate)},
        {"$ss18_DictionaryStorageC4copy8originalAByxq_Gs05__RawaB0C_tFZ", reinterpret_cast<void*>(dictionary_storage_copy)},
        {"$ss18_DictionaryStorageC6resize8original8capacity4moveAByxq_Gs05__RawaB0C_SiSbtFZ", reinterpret_cast<void*>(dictionary_storage_resize)},
        {"$ss6HasherV5_seedABSi_tcfC", reinterpret_cast<void*>(hasher_seed)},
        {"$ss6HasherV8_combineyySuF", reinterpret_cast<void*>(hasher_combine_word)},
        {"$ss6HasherV8_combineyys5UInt8VF", reinterpret_cast<void*>(hasher_combine_byte)},
        {"$ss6HasherV9_finalizeSiyF", reinterpret_cast<void*>(hasher_finalize)},
        {"$ss6HasherV5_hash4seed_S2i_s6UInt64VtFZ", reinterpret_cast<void*>(hasher_hash_seed_word)},
        {"$sSS4hash4intoys6HasherVz_tF", reinterpret_cast<void*>(string_hash_into)},
        {"$ss27_stringCompareWithSmolCheck__9expectingSbs11_StringGutsV_ADs01_G16ComparisonResultOtF", reinterpret_cast<void*>(string_equal_slow)},
    };
    const auto found = table.find(name);
    return found == table.end() ? nullptr : found->second;
}

extern "C" void (*storage_destroyer_for(const char* symbol))(void*) {
    const std::string_view name(symbol);
    if (name == "_$ss11_SetStorageCMn") return destroy_set_storage;
    if (name == "_$ss18_DictionaryStorageCMn") return destroy_dictionary_storage;
    if (name == "_$ss23_ContiguousArrayStorageCMn") return destroy_array_storage;
    return nullptr;
}
