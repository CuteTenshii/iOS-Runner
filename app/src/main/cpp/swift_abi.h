#pragma once
#include <atomic>
#include <cstddef>
#include <cstdint>

// What every shim shares about the Swift ABI on arm64: calling convention, object layout and value witnesses.
#define SWIFTCC __attribute__((swiftcall))
#define SWIFT_OUT __attribute__((swift_indirect_result))
#define SWIFT_CONTEXT __attribute__((swift_context))
#define SWIFT_ERROR __attribute__((swift_error_result))

extern "C" void* rt_swift_retain(void*);
extern "C" void rt_swift_release(void*);
// Calls a Swift function with x8 = out, x20 = self, x0 = type, x1 = table.
extern "C" void* rt_call_witness(const void* function, void* out, void* self, void* type, void* table);
// Swift destructors take self in x20, this runtime's in x0, so the object goes in both.
extern "C" void rt_call_destroyer(void* destroyer, void* object);

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
    uint32_t (*get_enum_tag)(const void*, void*);
    void (*destructive_project_enum_data)(void*, void*);
    void (*destructive_inject_enum_tag)(void*, uint32_t, void*);
};

inline ValueWitnessTable* witnesses_of(const void* metadata) { return static_cast<ValueWitnessTable*>(static_cast<void* const*>(metadata)[-1]); }

struct HeapObject {
    void* metadata;
    std::atomic<uint64_t> references;
};

// Metadata of a runtime-owned object: release finds the destroyer two words before the kind.
struct HeapMetadata {
    void (*destroyer)(void*);
    const void* witnesses;
    uint64_t kind;
};

// Null, tagged immediates, and small words: framework enums kept in one word hold plain case numbers.
inline bool is_not_object(const void* raw) {
    constexpr uintptr_t kFirstValidAddress = 0x1000;
    return reinterpret_cast<uintptr_t>(raw) < kFirstValidAddress || reinterpret_cast<intptr_t>(raw) < 0;
}

// A symbolic reference in a mangled name is a marker byte and a 4-byte offset.
inline size_t mangled_name_length(const char* name) {
    size_t length = 0;
    while (name[length] != 0) {
        const auto byte = static_cast<uint8_t>(name[length]);
        length += byte >= 1 && byte <= 0x17 ? 5 : 1;
    }
    return length;
}

constexpr uint64_t kImmortal = 1ULL << 63;
constexpr uint64_t kBridgePointerMask = 0x00fffffffffffff8ULL;
constexpr uint64_t kBridgeImmediateBit = 1ULL << 63;
// A framework value kept in one word, tagged so refcounting skips it.
constexpr uint64_t kImmediate = 1ULL << 63;
constexpr uint32_t kFlagNonPod = 0x10000;
constexpr uint32_t kFlagNonInline = 0x20000;
constexpr size_t kValueWitnessSizeOffset = 0x40;
constexpr size_t kWitnessStoreEnumTagOffset = 0x38;
constexpr size_t kBodyBufferSize = 64 * 1024;
constexpr size_t kMetadataDescriptorWord = 1;
constexpr size_t kMetadataArgumentsWord = 2;
constexpr size_t kDescriptorFieldsOffset = 16;
constexpr size_t kFieldRecordBytes = 12;
constexpr size_t kConformanceTypeOffset = 4;
constexpr uint32_t kTypeReferenceKindMask = 0x7u << 3;
constexpr uint32_t kTypeReferenceIndirect = 0x1u << 3;
constexpr uint8_t kMarkerAccessFunction = 0xff;
constexpr uint8_t kSymbolicAccessFunction = 0x09;
constexpr size_t kGenericRequirementSize = 12;

constexpr size_t kClassSuperclassOffset = 8;
constexpr size_t kClassDataOffset = 32;
constexpr uintptr_t kClassDataMask = ~static_cast<uintptr_t>(7);
constexpr size_t kRoInstanceSizeOffset = 8;

constexpr uint64_t kCountMask = 0x0000ffffffffffffULL;
constexpr size_t kArrayCountOffset = 16;
constexpr size_t kArrayElementsOffset = 32;
constexpr size_t kStorageCountOffset = 24;
constexpr size_t kDictionaryKeysOffset = 48;
constexpr size_t kDictionaryValuesOffset = 56;
constexpr size_t kDictionaryBitmapOffset = 64;

constexpr double kSecondsFrom1970To2001 = 978307200.0;

// A shim for a framework constant that is one tagged word.
SWIFTCC inline void immediate_indirect(SWIFT_OUT uint64_t* out) { *out = kImmediate; }
