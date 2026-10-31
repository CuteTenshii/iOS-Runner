#pragma once
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
#include "stdlib_shim.h"
#include "swift_abi.h"
#include "swift_runtime.h"
#include "symbols.h"
#include "type_system.h"

// Private to the stdlib shim sources.
namespace stdlib {

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
static_assert(sizeof(HashedHeader) == kDictionaryKeysOffset);

constexpr size_t kWordBits = 64;
constexpr uint64_t kBridgeAddressMask = 0x00FFFFFFFFFFFFF8ULL;

void** arguments_of(const void* metadata);
int scale_for_capacity(int64_t capacity);
size_t align_up(size_t value, size_t mask);
size_t alignment_mask(const ValueWitnessTable* witnesses);
void* allocate_dictionary(void* metadata, int scale, int32_t age, int64_t seed, bool has_seed);

template <typename Visit>
void for_each_occupied(const char* bitmap, int scale, Visit visit) {
    const size_t buckets = 1ULL << scale;
    for (size_t bucket = 0; bucket < buckets; ++bucket) {
        uint64_t word;
        memcpy(&word, bitmap + bucket / kWordBits * sizeof word, sizeof word);
        if ((word >> (bucket % kWordBits)) & 1) visit(bucket);
    }
}

void destroy_set_storage(void* raw);
void destroy_dictionary_storage(void* raw);
void destroy_array_storage(void* raw);

// Hasher is frozen at 72 bytes; only its first word is used, since hash values are private to one process.
struct HasherState {
    uint64_t state;
    uint64_t unused[8];
};

SWIFTCC void hasher_seed(SWIFT_OUT HasherState* out, int64_t seed);
SWIFTCC int64_t hasher_finalize(SWIFT_CONTEXT HasherState* self);
SWIFTCC void string_hash_into(HasherState* hasher, SwiftString value);

constexpr size_t kMinimumGrownCapacity = 32;

uint64_t make_index(size_t offset);
size_t index_offset(uint64_t index);
size_t next_scalar(const std::string& text, size_t offset);
size_t previous_scalar(const std::string& text, size_t offset);

struct StringTriple {
    SwiftString value;
    uint64_t repaired;
};

SWIFTCC SwiftString int_description(SWIFT_CONTEXT const int64_t* self);

// AnyHashable is frozen: a three-word inline buffer, then the value's type and Hashable witness table.
struct AnyHashableValue {
    uint64_t buffer[3];
    void* type;
    void* witnesses;
};

constexpr size_t kBoxHeaderBytes = 16;

}  // namespace stdlib

void* make_array_storage(void* element_type, size_t count, char** elements);
size_t array_storage_contents(void* storage, void* element_type, char** elements);

namespace stdlib {

using SymbolTable = std::unordered_map<std::string_view, void*>;
void add_stdlib_runtime_symbols(SymbolTable& table);
void add_stdlib_strings_symbols(SymbolTable& table);
void add_stdlib_collections_symbols(SymbolTable& table);

}  // namespace stdlib
