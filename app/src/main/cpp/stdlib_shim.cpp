#include "stdlib_shim.h"

#include "symbols.h"

#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <new>
#include <string>
#include <unordered_map>

#include "engine.h"
#include "foundation_shim.h"
#include "key_path.h"
#include "macho.h"
#include "objc_runtime.h"
#include "swift_runtime.h"
#include "type_system.h"

#define SWIFTCC __attribute__((swiftcall))
#define SWIFT_OUT __attribute__((swift_indirect_result))
#define SWIFT_SELF __attribute__((swift_context))

uint64_t* empty_set_singleton();
uint64_t* empty_array_singleton();

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
constexpr uint64_t kBridgeAddressMask = 0x00FFFFFFFFFFFFF8ULL;

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

constexpr uint64_t kIndexFlags = 0x7;
constexpr unsigned kIndexOffsetShift = 16;
constexpr size_t kMinimumGrownCapacity = 32;

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

struct StringTriple {
    SwiftString value;
    uint64_t repaired;
};

struct SubstringValue {
    uint64_t start;
    uint64_t end;
    SwiftString guts;
};

std::string map_ascii_case(std::string text, bool upper) {
    for (char& c : text) {
        if (upper && c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
        if (!upper && c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    }
    return text;
}

SWIFTCC void string_append(SwiftString other, SWIFT_SELF SwiftString* self) {
    *self = swift_string_from_utf8(swift_string_to_utf8(*self) + swift_string_to_utf8(other));
}

SWIFTCC void string_guts_grow(int64_t capacity, SWIFT_SELF SwiftString* self) {
    // Rewrites the string into native storage with room for `capacity` bytes, as the guest's inline append expects.
    std::string text = swift_string_to_utf8(*self);
    const size_t requested = capacity < static_cast<int64_t>(kMinimumGrownCapacity) ? kMinimumGrownCapacity : static_cast<size_t>(capacity);
    const size_t size = text.size();
    text.resize(requested > kMinimumGrownCapacity ? requested : kMinimumGrownCapacity + 1);
    SwiftString grown = swift_string_from_utf8(text);
    constexpr uint64_t kCountMask = 0x0000ffffffffffffULL;
    grown.count_and_flags = (grown.count_and_flags & ~kCountMask) | size;
    constexpr size_t kStorageCountOffset = 24;
    memcpy(reinterpret_cast<char*>(grown.object) + kStorageCountOffset, &grown.count_and_flags, sizeof grown.count_and_flags);
    *self = grown;
}

SWIFTCC SwiftString string_lowercased(SwiftString self) { return swift_string_from_utf8(map_ascii_case(swift_string_to_utf8(self), false)); }
SWIFTCC SwiftString string_uppercased(SwiftString self) { return swift_string_from_utf8(map_ascii_case(swift_string_to_utf8(self), true)); }

SWIFTCC bool string_has_prefix(SwiftString prefix, SwiftString self) {
    const std::string text = swift_string_to_utf8(self);
    const std::string start = swift_string_to_utf8(prefix);
    return text.size() >= start.size() && text.compare(0, start.size(), start) == 0;
}

SWIFTCC bool string_has_suffix(SwiftString suffix, SwiftString self) {
    const std::string text = swift_string_to_utf8(self);
    const std::string end = swift_string_to_utf8(suffix);
    return text.size() >= end.size() && text.compare(text.size() - end.size(), end.size(), end) == 0;
}

SWIFTCC uint64_t string_index_after(uint64_t index, SwiftString self) { return make_index(next_scalar(swift_string_to_utf8(self), index_offset(index))); }
SWIFTCC uint64_t string_index_before(uint64_t index, SwiftString self) { return make_index(previous_scalar(swift_string_to_utf8(self), index_offset(index))); }

SWIFTCC SwiftString string_subscript_character(uint64_t index, SwiftString self) {
    const std::string text = swift_string_to_utf8(self);
    const size_t start = index_offset(index);
    return swift_string_from_utf8(text.substr(start, next_scalar(text, start) - start));
}

extern "C" void* rt_swift_bridgeObjectRetain(void*);

// `self` is borrowed but the Substring owns its guts, so it takes its own reference.
SWIFTCC SubstringValue string_subscript_range(uint64_t start, uint64_t end, SwiftString self) {
    rt_swift_bridgeObjectRetain(reinterpret_cast<void*>(self.object));
    return {start, end, self};
}

SWIFTCC SwiftString string_from_substring(uint64_t start, uint64_t end, SwiftString guts) {
    const std::string text = swift_string_to_utf8(guts);
    return swift_string_from_utf8(text.substr(index_offset(start), index_offset(end) - index_offset(start)));
}

SWIFTCC StringTriple string_from_utf8_repairing(const char* bytes, int64_t count) {
    return {swift_string_from_utf8(std::string(bytes, static_cast<size_t>(count))), 0};
}

constexpr size_t kArrayCountOffset = 16;
constexpr size_t kArrayStorageElements = 32;
constexpr size_t kStaticStringStride = 24;

SWIFTCC int64_t find_string_switch_case(const char* cases, SwiftString value) {
    const std::string text = swift_string_to_utf8(value);
    int64_t count;
    memcpy(&count, cases + kArrayCountOffset, sizeof count);
    for (int64_t i = 0; i < count; ++i) {
        const char* entry = cases + kArrayStorageElements + static_cast<size_t>(i) * kStaticStringStride;
        const char* start;
        uint64_t length;
        memcpy(&start, entry, sizeof start);
        memcpy(&length, entry + sizeof start, sizeof length);
        if (text.size() == length && memcmp(text.data(), start, length) == 0) return i;
    }
    return -1;
}

SWIFTCC SwiftString int_description(SWIFT_SELF const int64_t* self) { return swift_string_from_utf8(std::to_string(*self)); }

// Live Activities have no Android equivalent: the app sees none, and that they are switched off.
void* activity_list(void*) { return empty_array_singleton(); }
SWIFTCC void* activity_authorization_init(SWIFT_SELF void* self) { return self; }
SWIFTCC bool activity_enabled(SWIFT_SELF void*) { return false; }

// AnyHashable is frozen: a three-word inline buffer, then the value's type and Hashable witness table.
struct AnyHashableValue {
    uint64_t buffer[3];
    void* type;
    void* witnesses;
};

constexpr uint32_t kFlagNonInline = 0x20000;
constexpr size_t kBoxHeaderBytes = 16;

struct BoxMetadataWords {
    void (*destroyer)(void*);
    const void* witnesses;
    uint64_t kind;
};

const BoxMetadataWords kValueBoxMetadata = {free, nullptr, 0};

SWIFTCC void any_hashable_init(SWIFT_OUT AnyHashableValue* out, void* value, void* type, void* hashable) {
    const auto* witnesses = witnesses_of(type);
    memset(out, 0, sizeof *out);
    out->type = type;
    out->witnesses = hashable;
    if (witnesses->flags & kFlagNonInline) {
        const size_t mask = witnesses->flags & 0xff;
        const size_t offset = align_up(kBoxHeaderBytes, mask);
        auto* box = static_cast<char*>(calloc(1, offset + witnesses->size));
        auto* header = reinterpret_cast<HashedHeader*>(box);
        header->metadata = const_cast<uint64_t*>(&kValueBoxMetadata.kind);
        new (&header->references) std::atomic<uint64_t>(1);
        witnesses->initialize_with_take(box + offset, value, type);
        out->buffer[0] = reinterpret_cast<uint64_t>(box);
    } else {
        witnesses->initialize_with_take(out, value, type);
    }
}

// Headers and JSON keys are the only AnyHashable values this app builds: both are strings.
const SwiftString& any_hashable_string(const AnyHashableValue* value) {
    if (foreign_type_name(value->type) != nullptr) {
        engine_log("STOP: AnyHashable holding a non-string value");
        abort();
    }
    return *reinterpret_cast<const SwiftString*>(value);
}

SWIFTCC int64_t any_hashable_raw_hash(int64_t seed, SWIFT_SELF const AnyHashableValue* self) {
    HasherState hasher;
    hasher_seed(&hasher, seed);
    string_hash_into(&hasher, any_hashable_string(self));
    return hasher_finalize(&hasher);
}

SWIFTCC bool any_hashable_equal(const AnyHashableValue* left, const AnyHashableValue* right) {
    return swift_string_to_utf8(any_hashable_string(left)) == swift_string_to_utf8(any_hashable_string(right));
}

// The run length of the standard library's merge sort, computed as it does.
SWIFTCC int64_t minimum_merge_run_length(int64_t count) {
    constexpr int kBitsToUse = 6;
    constexpr int kWordBits = 64;
    if (count < (1 << kBitsToUse)) return count;
    const int offset = (kWordBits - kBitsToUse) - __builtin_clzll(static_cast<uint64_t>(count));
    const int64_t high = count >> offset;
    const int64_t low_mask = (1LL << offset) - 1;
    return (count & low_mask) == 0 ? high : high + 1;
}

// _HashTable is two words: the bitmap's address and the bucket mask.
SWIFTCC int64_t hash_table_start_bucket(const uint64_t* words, uint64_t mask) {
    for (uint64_t bucket = 0; bucket <= mask; ++bucket) {
        if ((words[bucket / kWordBits] >> (bucket % kWordBits)) & 1) return static_cast<int64_t>(bucket);
    }
    return static_cast<int64_t>(mask + 1);
}

SWIFTCC int64_t hash_table_previous_hole(int64_t before, const uint64_t* words, uint64_t mask) {
    uint64_t bucket = static_cast<uint64_t>(before);
    for (;;) {
        bucket = (bucket - 1) & mask;
        if (!((words[bucket / kWordBits] >> (bucket % kWordBits)) & 1)) return static_cast<int64_t>(bucket);
    }
}

constexpr size_t kOptionalIndexNil = 0;

struct OptionalIndex {
    uint64_t index;
    uint64_t is_nil;
};

SWIFTCC OptionalIndex string_index_offset_limited(uint64_t index, int64_t distance, uint64_t limit, SwiftString self) {
    const std::string text = swift_string_to_utf8(self);
    size_t offset = index_offset(index);
    const size_t bound = index_offset(limit);
    for (int64_t i = 0; i < distance; ++i) {
        if (offset == bound) return {kOptionalIndexNil, 1};
        offset = next_scalar(text, offset);
    }
    for (int64_t i = 0; i > distance; --i) {
        if (offset == bound) return {kOptionalIndexNil, 1};
        offset = previous_scalar(text, offset);
    }
    return {make_index(offset), 0};
}

SWIFTCC SwiftString string_remove_at(uint64_t index, SWIFT_SELF SwiftString* self) {
    std::string text = swift_string_to_utf8(*self);
    const size_t start = index_offset(index);
    const size_t end = next_scalar(text, start);
    const std::string removed = text.substr(start, end - start);
    text.erase(start, end - start);
    *self = swift_string_from_utf8(text);
    return swift_string_from_utf8(removed);
}

SWIFTCC void string_remove_subrange(uint64_t start, uint64_t end, SWIFT_SELF SwiftString* self) {
    std::string text = swift_string_to_utf8(*self);
    text.erase(index_offset(start), index_offset(end) - index_offset(start));
    *self = swift_string_from_utf8(text);
}

SWIFTCC uint64_t substring_index_after(uint64_t index, uint64_t, uint64_t, SwiftString guts) {
    return make_index(next_scalar(swift_string_to_utf8(guts), index_offset(index)));
}

SWIFTCC SwiftString substring_subscript(uint64_t index, uint64_t, uint64_t, SwiftString guts) {
    const std::string text = swift_string_to_utf8(guts);
    const size_t start = index_offset(index);
    return swift_string_from_utf8(text.substr(start, next_scalar(text, start) - start));
}

SWIFTCC int64_t substring_utf8_distance(uint64_t from, uint64_t to, uint64_t, uint64_t, SwiftString) {
    return static_cast<int64_t>(index_offset(to)) - static_cast<int64_t>(index_offset(from));
}

SWIFTCC bool character_is_whitespace(SwiftString character) {
    const std::string text = swift_string_to_utf8(character);
    return text == " " || text == "\n" || text == "\t" || text == "\r";
}

SWIFTCC SwiftString type_name(void* type, bool) {
    const char* name = foreign_type_name(type);
    return swift_string_from_utf8(name != nullptr ? name : "Type");
}

void* get_object_type(void* object) { return *static_cast<void**>(object); }

constexpr size_t kClassSuperOffset = 8;

void* dynamic_cast_objc_class(void* object, void* target) {
    if (object == nullptr) return nullptr;
    if (objc_is_framework_class(object, target)) return object;
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

// Framework enums have no descriptor in this runtime: their tag is simply the last byte.
bool is_framework_enum(void* type) { return static_cast<void**>(type)[kMetadataDescriptorWord] == nullptr; }

uint32_t get_enum_case_multi_payload(const char* value, void* type) {
    if (is_framework_enum(type)) return static_cast<uint8_t>(value[witnesses_of(type)->size - 1]);
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
    if (is_framework_enum(type)) {
        value[witnesses_of(type)->size - 1] = static_cast<char>(which);
        return;
    }
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
    return cases <= 0x100 ? 1 : cases <= 0x10000 ? 2
                                                 : 4;
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

// swift_getAtKeyPath receives the result pointer in x8, like any indirect Swift return.
SWIFTCC void get_at_key_path(SWIFT_OUT void* out, void* root, void* key_path, void* value_type) { key_path_get(key_path, root, out, value_type); }

void swift_will_throw(void*) {}

[[noreturn]] void deleted_method_error() {
    engine_log("STOP: guest called a deleted method");
    abort();
}

}  // namespace

// The case name reflection records hold for a value of an app enum, or empty for anything else.
std::string enum_case_name(const void* value, const void* type) {
    constexpr uint64_t kKindEnum = 0x201;
    constexpr size_t kGetEnumTagOffset = 0x58;
    constexpr size_t kDescriptorFieldsOffset = 16;
    constexpr size_t kFieldHeaderBytes = 16;
    constexpr size_t kFieldCountOffset = 12;
    constexpr size_t kFieldRecordBytes = 12;
    constexpr size_t kRecordNameOffset = 8;
    const auto* words = static_cast<void* const*>(type);
    if (reinterpret_cast<uint64_t>(words[0]) != kKindEnum) return {};
    const auto* descriptor = static_cast<const char*>(words[1]);
    const auto relative = [](const char* at) {
        int32_t offset;
        memcpy(&offset, at, sizeof offset);
        return offset == 0 ? nullptr : at + offset;
    };
    const char* fields = relative(descriptor + kDescriptorFieldsOffset);
    if (fields == nullptr) return {};
    uint32_t count;
    memcpy(&count, fields + kFieldCountOffset, sizeof count);
    using GetTag = uint32_t (*)(const void*, const void*);
    GetTag get_tag;
    memcpy(&get_tag, reinterpret_cast<const char*>(witnesses_of(const_cast<void*>(type))) + kGetEnumTagOffset, sizeof get_tag);
    const uint32_t tag = get_tag(value, type);
    if (tag >= count) return {};
    const char* name = relative(fields + kFieldHeaderBytes + tag * kFieldRecordBytes + kRecordNameOffset);
    return name != nullptr ? name : "";
}

constexpr size_t kValueWitnessStrideOffset = 72;
constexpr uint64_t kCollectionAddressMask = 0x00fffffffffffff8ULL;

// _print_unlocked into a DefaultStringInterpolation, which is a String at runtime.
SWIFTCC void print_unlocked(const void* value, SwiftString* stream, void* type) {
    std::string text;
    if (type == type_system_symbol("$sSSN"))
        text = swift_string_to_utf8(*static_cast<const SwiftString*>(value));
    else if (type == type_system_symbol("$sSiN"))
        text = std::to_string(*static_cast<const int64_t*>(value));
    else if (type == type_system_symbol("$sSbN"))
        text = *static_cast<const uint8_t*>(value) != 0 ? "true" : "false";
    else
        text = enum_case_name(value, type);
    if (text.empty() && type != type_system_symbol("$sSSN")) text = "(value)";
    *stream = swift_string_from_utf8(swift_string_to_utf8(*stream) + text);
}

SWIFTCC SwiftString array_description(void* storage, void* element_type) {
    char* elements;
    const size_t count = array_storage_contents(storage, element_type, &elements);
    const size_t stride = *reinterpret_cast<const size_t*>(reinterpret_cast<const char*>(witnesses_of(element_type)) + kValueWitnessStrideOffset);
    const bool strings = element_type == type_system_symbol("$sSSN");
    std::string text = "[";
    for (size_t i = 0; i < count; ++i) {
        if (i != 0) text += ", ";
        SwiftString part = swift_string_from_utf8("");
        print_unlocked(elements + i * stride, &part, element_type);
        text += strings ? "\"" + swift_string_to_utf8(part) + "\"" : swift_string_to_utf8(part);
    }
    return swift_string_from_utf8(text + "]");
}

// Array<AnyObject> entry points the compiler specialized: elements are single retained references.
extern "C" void* rt_swift_retain(void*);
extern "C" void rt_swift_release(void*);

void object_array_grow(void** self, int64_t minimum_capacity) {
    auto* old = static_cast<HashedHeader*>(*self);
    const int64_t old_capacity = old->capacity >> 1;
    int64_t capacity = old_capacity * 2;
    if (capacity < minimum_capacity) capacity = minimum_capacity;
    if (capacity < 1) capacity = 1;
    char* elements;
    auto* raw = make_array_storage(any_object_metadata(), static_cast<size_t>(capacity), &elements);
    auto* header = static_cast<HashedHeader*>(raw);
    header->count = old->count;
    char* old_elements;
    array_storage_contents(old, any_object_metadata(), &old_elements);
    for (int64_t i = 0; i < old->count; ++i) {
        void* element;
        memcpy(&element, old_elements + static_cast<size_t>(i) * sizeof element, sizeof element);
        rt_swift_retain(element);
        memcpy(elements + static_cast<size_t>(i) * sizeof element, &element, sizeof element);
    }
    rt_swift_release(old);
    *self = raw;
}

SWIFTCC void object_array_make_unique(SWIFT_SELF void** self) {
    auto* header = static_cast<HashedHeader*>(*self);
    const int64_t capacity = header->capacity >> 1;
    if (header->references.load(std::memory_order_relaxed) != 1 || header->count >= capacity) object_array_grow(self, header->count + 1);
}

SWIFTCC void object_array_create_new_buffer(bool, int64_t minimum_capacity, bool, SWIFT_SELF void** self) { object_array_grow(self, minimum_capacity); }

SWIFTCC void object_array_append(int64_t old_count, void* element, SWIFT_SELF void** self) {
    auto* header = static_cast<HashedHeader*>(*self);
    char* elements;
    array_storage_contents(header, any_object_metadata(), &elements);
    memcpy(elements + static_cast<size_t>(old_count) * sizeof element, &element, sizeof element);
    header->count = old_count + 1;
}

constexpr uint32_t kFlagNonPod = 0x10000;

// Equatable's dispatch thunk; framework types only reach it as String or URL.
SWIFTCC bool equatable_equal(const void* left, const void* right, void* type, void* table) {
    if (type == type_system_symbol("$sSSN")) return swift_string_to_utf8(*static_cast<const SwiftString*>(left)) == swift_string_to_utf8(*static_cast<const SwiftString*>(right));
    if (const char* name = foreign_type_name(type); name != nullptr && strcmp(name, "_$s10Foundation3URLVMn") == 0) {
        return foundation_url_text(*static_cast<void* const*>(left)) == foundation_url_text(*static_cast<void* const*>(right));
    }
    // Framework values with no references, such as enum tags, are equal when their bytes are.
    if (const auto* witnesses = witnesses_of(type); foreign_type_name(type) != nullptr && (witnesses->flags & kFlagNonPod) == 0) return memcmp(left, right, witnesses->size) == 0;
    using Equal = SWIFTCC bool (*)(const void*, const void*, void*, void*, SWIFT_SELF void*);
    const auto equal = reinterpret_cast<Equal>(const_cast<void*>(swift_runtime_find_witness_named(static_cast<const char*>(static_cast<void**>(table)[0]), "_$sSQ2eeoiySbx_xtFZTq")));
    if (equal == nullptr) {
        engine_log("STOP: == on a type with no witness");
        abort();
    }
    return equal(left, right, type, table, type);
}

// A framework OptionSet is its raw value, whatever the type's witness table says.
SWIFTCC void option_set_init(SWIFT_OUT void* out, const void* raw, void* type, void*) { memcpy(out, raw, witnesses_of(type)->size); }

// SetAlgebra(sequence): the only framework values built this way are detents and flags, which nothing reads back.
SWIFTCC void set_algebra_from_sequence(SWIFT_OUT void* out, void*, void* type, void*, void*, void*) {
    const size_t size = witnesses_of(type)->size;
    memset(out, 0, size);
    if (foreign_type_name(type) == nullptr && size == sizeof(void*)) {
        void* empty = resolve_symbol("_swiftEmptySetSingleton");
        memcpy(out, &empty, sizeof empty);
    }
}

// Characters here are Unicode scalars, which is what the app's ASCII strings need.
int64_t scalar_count(const std::string& text) {
    int64_t count = 0;
    for (const char c : text) count += (static_cast<unsigned char>(c) & 0xC0) != 0x80 ? 1 : 0;
    return count;
}

SWIFTCC int64_t string_count(SwiftString text) { return scalar_count(swift_string_to_utf8(text)); }

SWIFTCC SwiftString string_describing(const void* value, void* type) {
    SwiftString out = swift_string_from_utf8("");
    print_unlocked(value, &out, type);
    return out;
}

SWIFTCC SwiftString string_from_c_string(const char* text) { return swift_string_from_utf8(text); }

// os.Logger is one placeholder word: logging goes nowhere, and os_log_type_enabled already answers false.
SWIFTCC void logger_init(SWIFT_OUT uint64_t* out) { *out = 1ULL << 63; }
SWIFTCC uint64_t logger_object() { return 1ULL << 63; }
template <uint8_t Level>
SWIFTCC uint8_t log_type_getter() { return Level; }

void array_take_forward(char* to, char* from, size_t count, void* type) {
    const auto* witnesses = witnesses_of(type);
    for (size_t i = 0; i < count; ++i) witnesses->initialize_with_take(to + i * witnesses->stride, from + i * witnesses->stride, type);
}

void array_take_backward(char* to, char* from, size_t count, void* type) {
    const auto* witnesses = witnesses_of(type);
    for (size_t i = count; i-- > 0;) witnesses->initialize_with_take(to + i * witnesses->stride, from + i * witnesses->stride, type);
}

// A box holds its type, then the value at the alignment the type needs.
struct BoxHeader {
    void* metadata;
    std::atomic<uint64_t> references;
    void* type;
};

size_t box_value_offset(void* type) {
    const size_t mask = witnesses_of(type)->flags & 0xff;
    return (sizeof(BoxHeader) + mask) & ~mask;
}

void destroy_boxed(void* raw) {
    auto* box = static_cast<BoxHeader*>(raw);
    if (box->type != nullptr) witnesses_of(box->type)->destroy(static_cast<char*>(raw) + box_value_offset(box->type), box->type);
    free(raw);
}

struct BoxMetadataLayout {
    void (*destroyer)(void*);
    const void* witnesses;
    uint64_t kind;
};

const BoxMetadataLayout kBoxLayout = {destroy_boxed, nullptr, 0};

struct BoxPair {
    void* box;
    void* value;
};

BoxPair alloc_box(void* type) {
    const size_t offset = box_value_offset(type);
    auto* raw = static_cast<char*>(calloc(1, offset + witnesses_of(type)->size + 1));
    auto* box = reinterpret_cast<BoxHeader*>(raw);
    box->metadata = const_cast<uint64_t*>(&kBoxLayout.kind);
    new (&box->references) std::atomic<uint64_t>(1);
    box->type = type;
    return {box, raw + offset};
}

void* project_box(void* box) { return static_cast<char*>(box) + box_value_offset(static_cast<BoxHeader*>(box)->type); }

constexpr size_t kSuperclassOffset = 8;

// Walks a class's ancestry; a framework superclass is a guard address that can't be read, only compared.
bool class_inherits(const void* cls, const void* target) {
    while (cls != nullptr) {
        if (cls == target) return true;
        if (unresolved_symbol_at(reinterpret_cast<uintptr_t>(cls)) != nullptr) return false;
        memcpy(&cls, static_cast<const char*>(cls) + kSuperclassOffset, sizeof cls);
    }
    return false;
}

void* dynamic_cast_class(void* object, const void* target) {
    if (object == nullptr) return nullptr;
    const void* cls;
    memcpy(&cls, object, sizeof cls);
    return class_inherits(cls, target) ? object : nullptr;
}

void* dynamic_cast_class_unconditional(void* object, const void* target, const char*, uint32_t, uint32_t) {
    void* result = dynamic_cast_class(object, target);
    if (result == nullptr) {
        engine_log("STOP: an unconditional class cast failed");
        abort();
    }
    return result;
}

const void* dynamic_cast_metatype(const void* type, const void* target) { return class_inherits(type, target) ? type : nullptr; }
void* identity_cast(void* object) { return object; }

constexpr uint64_t kKindExistentialMetadata = 0x303;

// Only a class existential's value carries its own dynamic type here.
void* dynamic_type_of(void* value, void* type, bool) {
    if (*static_cast<uint64_t*>(type) == kKindExistentialMetadata) {
        void* object;
        memcpy(&object, value, sizeof object);
        void* cls;
        memcpy(&cls, object, sizeof cls);
        return cls;
    }
    return type;
}

bool is_uniquely_referenced(void* object) { return object != nullptr && static_cast<BoxHeader*>(object)->references.load(std::memory_order_relaxed) == 1; }

void release_n(void* object, uint32_t count) {
    for (uint32_t i = 0; i < count; ++i) rt_swift_release(object);
}

void* retain_n(void* object, uint32_t count) {
    for (uint32_t i = 0; i < count; ++i) rt_swift_retain(object);
    return object;
}

// Weak references are strong here: an object stays alive rather than a read finding it gone, which only costs memory.
void* weak_init(void** slot, void* object) {
    *slot = rt_swift_retain(object);
    return slot;
}

void* weak_assign(void** slot, void* object) {
    rt_swift_retain(object);
    rt_swift_release(*slot);
    *slot = object;
    return slot;
}

void* weak_load_strong(void** slot) { return rt_swift_retain(*slot); }
void weak_destroy(void** slot) { rt_swift_release(*slot); }

void* weak_copy_init(void** slot, void** from) {
    *slot = rt_swift_retain(*from);
    return slot;
}

void* weak_take_init(void** slot, void** from) {
    *slot = *from;
    return slot;
}

void* weak_copy_assign(void** slot, void** from) { return weak_assign(slot, *from); }

void* weak_take_assign(void** slot, void** from) {
    rt_swift_release(*slot);
    *slot = *from;
    return slot;
}

bool escaping_closure_check(const void*, const void*, int32_t, int32_t, int32_t) { return false; }

[[noreturn]] void unexpected_error(void*, const char*, int64_t, bool, const char*, int64_t, bool, const char*, int64_t, bool, uint64_t) {
    engine_log("STOP: an error reached a try! expression");
    abort();
}

// A sequence of Characters, which in this runtime means a String or an array of them.
SWIFTCC void string_append_sequence(const void* sequence, void* type, void*, SWIFT_SELF SwiftString* self) {
    std::string text = swift_string_to_utf8(*self);
    if (type == type_system_symbol("$sSSN")) {
        text += swift_string_to_utf8(*static_cast<const SwiftString*>(sequence));
    } else if (void* element = array_element_type(type)) {
        uint64_t word;
        memcpy(&word, sequence, sizeof word);
        char* elements;
        const size_t count = array_storage_contents(reinterpret_cast<void*>(word & kCollectionAddressMask), element, &elements);
        for (size_t i = 0; i < count; ++i) text += swift_string_to_utf8(*reinterpret_cast<const SwiftString*>(elements + i * sizeof(SwiftString)));
    } else {
        engine_log("STOP: append(contentsOf:) on a sequence other than String or Array");
        abort();
    }
    *self = swift_string_from_utf8(text);
}

// BidirectionalCollection<String>.joined: an Array is the only such collection this runtime hands out.
SWIFTCC SwiftString strings_joined(SwiftString separator, void* type, void*, SWIFT_SELF const uint64_t* self) {
    if (array_element_type(type) == nullptr) {
        engine_log("STOP: joined(separator:) on a collection other than Array");
        abort();
    }
    char* elements;
    const size_t count = array_storage_contents(reinterpret_cast<void*>(*self & kBridgeAddressMask), type_system_symbol("$sSSN"), &elements);
    const std::string glue = swift_string_to_utf8(separator);
    std::string out;
    for (size_t i = 0; i < count; ++i) {
        if (i != 0) out += glue;
        out += swift_string_to_utf8(reinterpret_cast<const SwiftString*>(elements)[i]);
    }
    return swift_string_from_utf8(out);
}

void* stdlib_symbol(std::string_view name) {
    static const bool registered = [] {
        register_foreign_witness("_$sSis23CustomStringConvertiblesWP", "_$ss23CustomStringConvertibleP11descriptionSSvgTq", reinterpret_cast<const void*>(int_description));
        return true;
    }();
    (void)registered;
    static const std::unordered_map<std::string_view, void*> table = {
        {"$sSa034_makeUniqueAndReserveCapacityIfNotB0yyFyXl_Ts5", reinterpret_cast<void*>(object_array_make_unique)},
        {"$sSa16_createNewBuffer14bufferIsUnique15minimumCapacity13growForAppendySb_SiSbtFyXl_Ts5", reinterpret_cast<void*>(object_array_create_new_buffer)},
        {"$sSa37_appendElementAssumeUniqueAndCapacity_03newB0ySi_xntFyXl_Ts5", reinterpret_cast<void*>(object_array_append)},
        {"$sSQ2eeoiySbx_xtFZTj", reinterpret_cast<void*>(equatable_equal)},
        {"$ss10SetAlgebraPyxqd__ncSTRd__7ElementQyd__ACRtzlufCTj", reinterpret_cast<void*>(set_algebra_from_sequence)},
        {"$ss9OptionSetP8rawValuex03RawD0Qz_tcfCTj", reinterpret_cast<void*>(option_set_init)},
        {"$ss15_print_unlockedyyx_q_zts16TextOutputStreamR_r0_lF", reinterpret_cast<void*>(print_unlocked)},
        {"$sSa11descriptionSSvg", reinterpret_cast<void*>(array_description)},
        {"$sSKsSS7ElementRtzrlE6joined9separatorS2S_tF", reinterpret_cast<void*>(strings_joined)},
        {"$s2os6LoggerV9subsystem8categoryACSS_SStcfC", reinterpret_cast<void*>(logger_init)},
        {"$s2os6LoggerV9logObjectSo03OS_a1_C0Cvg", reinterpret_cast<void*>(logger_object)},
        {"$sSo13os_log_type_ta0A0E5errorABvgZ", reinterpret_cast<void*>(log_type_getter<0x10>)},
        {"$sSo13os_log_type_ta0A0E4infoABvgZ", reinterpret_cast<void*>(log_type_getter<0x01>)},
        {"$sSo13os_log_type_ta0A0E5debugABvgZ", reinterpret_cast<void*>(log_type_getter<0x02>)},
        {"$sSo13os_log_type_ta0A0E7defaultABvgZ", reinterpret_cast<void*>(log_type_getter<0x00>)},
        {"$sSo13os_log_type_ta0A0E5faultABvgZ", reinterpret_cast<void*>(log_type_getter<0x11>)},
        {"$sSS6append10contentsOfyx_tSTRzSJ7ElementRtzlF", reinterpret_cast<void*>(string_append_sequence)},
        {"$sSS5countSivg", reinterpret_cast<void*>(string_count)},
        {"$sSS10describingSSx_tclufC", reinterpret_cast<void*>(string_describing)},
        {"$sSS7cStringSSSPys4Int8VG_tcfC", reinterpret_cast<void*>(string_from_c_string)},
        {"$sSS7cStringSSSPys5UInt8VG_tcfC", reinterpret_cast<void*>(string_from_c_string)},
        {"$sSS6appendyySSF", reinterpret_cast<void*>(string_append)},
        {"$ss11_StringGutsV4growyySiF", reinterpret_cast<void*>(string_guts_grow)},
        {"$sSS10lowercasedSSyF", reinterpret_cast<void*>(string_lowercased)},
        {"$sSS10uppercasedSSyF", reinterpret_cast<void*>(string_uppercased)},
        {"$sSS9hasPrefixySbSSF", reinterpret_cast<void*>(string_has_prefix)},
        {"$sSS9hasSuffixySbSSF", reinterpret_cast<void*>(string_has_suffix)},
        {"$sSS5index5afterSS5IndexVAD_tF", reinterpret_cast<void*>(string_index_after)},
        {"$sSS5index6beforeSS5IndexVAD_tF", reinterpret_cast<void*>(string_index_before)},
        {"$sSSySJSS5IndexVcig", reinterpret_cast<void*>(string_subscript_character)},
        {"$sSSySsSnySS5IndexVGcig", reinterpret_cast<void*>(string_subscript_range)},
        {"$sSS14_fromSubstringySSSshFZ", reinterpret_cast<void*>(string_from_substring)},
        {"$sSS18_fromUTF8RepairingySS6result_Sb11repairsMadetSRys5UInt8VGFZ", reinterpret_cast<void*>(string_from_utf8_repairing)},
        {"$ss21_findStringSwitchCase5cases6stringSiSays06StaticB0VG_SStF", reinterpret_cast<void*>(find_string_switch_case)},
        {"$s11ActivityKit0A0C10activitiesSayACyxGGvgZ", reinterpret_cast<void*>(activity_list)},
        {"$s11ActivityKit0A17AuthorizationInfoCACycfc", reinterpret_cast<void*>(activity_authorization_init)},
        {"$s11ActivityKit0A17AuthorizationInfoC20areActivitiesEnabledSbvg", reinterpret_cast<void*>(activity_enabled)},
        {"$ss11AnyHashableVyABxcSHRzlufC", reinterpret_cast<void*>(any_hashable_init)},
        {"$ss11AnyHashableV13_rawHashValue4seedS2i_tF", reinterpret_cast<void*>(any_hashable_raw_hash)},
        {"$ss11AnyHashableV2eeoiySbAB_ABtFZ", reinterpret_cast<void*>(any_hashable_equal)},
        {"$ss22_minimumMergeRunLengthyS2iF", reinterpret_cast<void*>(minimum_merge_run_length)},
        {"swift_arrayInitWithTakeFrontToBack", reinterpret_cast<void*>(array_take_forward)},
        {"swift_arrayInitWithTakeBackToFront", reinterpret_cast<void*>(array_take_backward)},
        {"swift_allocBox", reinterpret_cast<void*>(alloc_box)},
        {"swift_projectBox", reinterpret_cast<void*>(project_box)},
        {"swift_dynamicCastClass", reinterpret_cast<void*>(dynamic_cast_class)},
        {"swift_dynamicCastClassUnconditional", reinterpret_cast<void*>(dynamic_cast_class_unconditional)},
        {"swift_dynamicCastMetatype", reinterpret_cast<void*>(dynamic_cast_metatype)},
        {"swift_dynamicCastObjCClassUnconditional", reinterpret_cast<void*>(identity_cast)},
        {"swift_dynamicCastObjCProtocolConditional", reinterpret_cast<void*>(identity_cast)},
        {"swift_dynamicCastObjCProtocolUnconditional", reinterpret_cast<void*>(identity_cast)},
        {"swift_getDynamicType", reinterpret_cast<void*>(dynamic_type_of)},
        {"swift_isUniquelyReferenced_native", reinterpret_cast<void*>(is_uniquely_referenced)},
        {"swift_isUniquelyReferencedNonObjC_nonNull", reinterpret_cast<void*>(is_uniquely_referenced)},
        {"swift_release_n", reinterpret_cast<void*>(release_n)},
        {"swift_unknownObjectRelease_n", reinterpret_cast<void*>(release_n)},
        {"swift_unknownObjectRetain_n", reinterpret_cast<void*>(retain_n)},
        {"swift_unownedRetain", reinterpret_cast<void*>(rt_swift_retain)},
        {"swift_unownedRelease", reinterpret_cast<void*>(rt_swift_release)},
        {"swift_unownedRetainStrong", reinterpret_cast<void*>(rt_swift_retain)},
        {"swift_weakInit", reinterpret_cast<void*>(weak_init)},
        {"swift_weakAssign", reinterpret_cast<void*>(weak_assign)},
        {"swift_weakLoadStrong", reinterpret_cast<void*>(weak_load_strong)},
        {"swift_weakDestroy", reinterpret_cast<void*>(weak_destroy)},
        {"swift_unknownObjectWeakInit", reinterpret_cast<void*>(weak_init)},
        {"swift_unknownObjectWeakAssign", reinterpret_cast<void*>(weak_assign)},
        {"swift_unknownObjectWeakLoadStrong", reinterpret_cast<void*>(weak_load_strong)},
        {"swift_unknownObjectWeakDestroy", reinterpret_cast<void*>(weak_destroy)},
        {"swift_unknownObjectWeakCopyInit", reinterpret_cast<void*>(weak_copy_init)},
        {"swift_unknownObjectWeakTakeInit", reinterpret_cast<void*>(weak_take_init)},
        {"swift_unknownObjectWeakCopyAssign", reinterpret_cast<void*>(weak_copy_assign)},
        {"swift_unknownObjectWeakTakeAssign", reinterpret_cast<void*>(weak_take_assign)},
        {"swift_isEscapingClosureAtFileLocation", reinterpret_cast<void*>(escaping_closure_check)},
        {"swift_unexpectedError", reinterpret_cast<void*>(unexpected_error)},
        {"swift_dynamicCast", reinterpret_cast<void*>(swift_cast)},
        {"$ss10_HashTableV11startBucketAB0D0Vvg", reinterpret_cast<void*>(hash_table_start_bucket)},
        {"$ss10_HashTableV12previousHole6beforeAB6BucketVAF_tF", reinterpret_cast<void*>(hash_table_previous_hole)},
        {"$sSS5index_8offsetBy07limitedC0SS5IndexVSgAE_SiAEtF", reinterpret_cast<void*>(string_index_offset_limited)},
        {"$sSS6remove2atSJSS5IndexV_tF", reinterpret_cast<void*>(string_remove_at)},
        {"$sSS14removeSubrangeyySnySS5IndexVGF", reinterpret_cast<void*>(string_remove_subrange)},
        {"$sSs5index5afterSS5IndexVAD_tF", reinterpret_cast<void*>(substring_index_after)},
        {"$sSsySJSS5IndexVcig", reinterpret_cast<void*>(substring_subscript)},
        {"$sSs8UTF8ViewV8distance4from2toSiSS5IndexV_AGtF", reinterpret_cast<void*>(substring_utf8_distance)},
        {"$sSJ12isWhitespaceSbvg", reinterpret_cast<void*>(character_is_whitespace)},
        {"$ss9_typeName_9qualifiedSSypXp_SbtF", reinterpret_cast<void*>(type_name)},
        {"swift_getObjectType", reinterpret_cast<void*>(get_object_type)},
        {"swift_dynamicCastObjCClass", reinterpret_cast<void*>(dynamic_cast_objc_class)},
        {"swift_arrayDestroy", reinterpret_cast<void*>(array_destroy)},
        {"swift_arrayInitWithCopy", reinterpret_cast<void*>(array_init_with_copy)},
        {"swift_getEnumCaseMultiPayload", reinterpret_cast<void*>(get_enum_case_multi_payload)},
        {"swift_storeEnumTagMultiPayload", reinterpret_cast<void*>(store_enum_tag_multi_payload)},
        {"swift_getEnumTagSinglePayloadGeneric", reinterpret_cast<void*>(get_enum_tag_single_payload_generic)},
        {"swift_storeEnumTagSinglePayloadGeneric", reinterpret_cast<void*>(store_enum_tag_single_payload_generic)},
        {"swift_getKeyPath", reinterpret_cast<void*>(key_path_create)},
        {"swift_modifyAtReferenceWritableKeyPath", reinterpret_cast<void*>(key_path_begin_modify)},
        {"swift_getAtKeyPath", reinterpret_cast<void*>(get_at_key_path)},
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
