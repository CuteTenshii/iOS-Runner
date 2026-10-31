#pragma once
#include <pthread.h>
#include <sys/mman.h>
#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include "engine.h"
#include "macho.h"
#include "stdlib_shim.h"
#include "swift_abi.h"
#include "swift_runtime.h"
#include "type_system.h"

// Private to the type system sources.
namespace type_system {

extern "C" void (*storage_destroyer_for(const char* symbol))(void*);

constexpr uint64_t kKindStruct = 0x200;
constexpr uint64_t kKindOptional = 0x202;
constexpr uint64_t kKindTuple = 0x301;
constexpr uint64_t kKindFunction = 0x302;
constexpr uint64_t kKindExistential = 0x303;
constexpr uint64_t kKindMetatype = 0x304;
constexpr size_t kInlineBufferSize = 24;
constexpr size_t kPointerSize = 8;
constexpr uint32_t kPointerExtraInhabitants = 0x1000;
constexpr uint32_t kDescriptorGeneric = 0x80;
constexpr uint32_t kRequirementKindMask = 0xf;
constexpr uint32_t kRequirementKindProtocol = 0;
constexpr uint32_t kRequirementHasKeyArgument = 0x80;
constexpr uint32_t kDescriptorKindMask = 0x1f;
constexpr uint32_t kDescriptorKindOpaque = 4;
constexpr size_t kGenericHeaderSize = 8;

enum class LayoutKind { Pod,
                        NativeReference,
                        BridgeReference,
                        Aggregate,
                        Optional,
                        OpaqueExistential,
                        StateStorage,
                        Conditional };

struct Metadata;

struct Field {
    Metadata* type;
    size_t offset;
};

struct Layout {
    LayoutKind kind = LayoutKind::Pod;
    std::vector<Field> fields;
    Metadata* payload = nullptr;
    // StateStorage: which case holds `payload`, and where the other case keeps its retained word.
    uint8_t value_case = 0;
    size_t reference_offset = 8;
};

// Runtime metadata plus its two prefix words: this runtime's layout, then the value witnesses.
struct MetadataAllocation {
    Layout* layout;
    ValueWitnessTable* witnesses;
    uint64_t kind;
    const void* descriptor;
    void* arguments[1];
};

struct Metadata {
    uint64_t kind;
    const void* descriptor;
    void* arguments[1];
};

Layout* layout_of(const void* metadata);
void* witness_take(void* destination, void* source, void* type);
void* witness_assign_copy(void* destination, void* source, void* type);
void* witness_assign_take(void* destination, void* source, void* type);
uint8_t state_storage_tag(const void* value, const void* type);
void* witness_copy(void* destination, void* source, void* type);
void witness_destroy(void* raw, void* type);
void* witness_buffer_copy(void* destination, void* source, void* type);

// App-defined types have no layout record here, only their own value witnesses.
std::unordered_set<const void*>& runtime_metadata();
uint32_t witness_get_tag(const void* value, uint32_t empty_cases, void* type);
void witness_store_tag(void* value, uint32_t tag, uint32_t empty_cases, void* type);

extern std::recursive_mutex g_types_lock;

extern std::map<std::string, Metadata*> g_named_types;

extern std::map<const void*, const char*> g_foreign_names;

extern std::map<const void*, void*> g_array_elements;

extern std::map<const void*, void*> g_dictionary_values;

Metadata* make_metadata(uint64_t kind, const void* descriptor, const std::vector<void*>& arguments, Layout* layout, size_t size, size_t alignment, uint32_t extra_inhabitants);
Metadata* named_type(const std::string& key, uint64_t kind, Layout* layout, size_t size, size_t alignment, uint32_t extra_inhabitants, const std::vector<void*>& arguments = {});
Metadata* pod_type(const std::string& key, size_t size);
std::string pointer_key(const void* pointer);
Metadata* bridge_type(const std::string& key, const std::vector<void*>& arguments = {});
Metadata* word_type();
Metadata* bridge_word_type();
Metadata* native_type();
Metadata* two_word_type(const std::string& key, Metadata* first, Metadata* second);
Metadata* string_type();
Metadata* thick_function_type();
Metadata* optional_of(Metadata* payload);
Metadata* tuple_of(const std::vector<Metadata*>& elements);
Metadata* any_type();
Metadata* protocol_existential(const char* protocol_name);

extern const char* g_current_name;

extern size_t g_current_length;

Metadata* lazy_state_of(Metadata* value);

// Types whose stored layout the app's compiled code depends on; everything else from a framework is one reference.
Metadata* foreign_nominal(const std::string& symbol, const std::vector<void*>& arguments);

constexpr uint32_t kContextKindClass = 16;
constexpr uint32_t kContextKindStruct = 17;

std::string printable(const char* text, size_t length);
extern "C" void* rt_getTypeByMangledNameInContext2(const char* name, size_t length, const void* context, void* const* generics);
extern "C" void* rt_getTypeByMangledNameInContextInMetadataState2(size_t, const char* name, size_t length, const void* context, void* const* generics);

constexpr uint32_t kKindMask = 0x1f;
constexpr uint32_t kContextClass = 16;
constexpr uint32_t kFlagGeneric = 0x80;
constexpr uint32_t kSingletonInitializationMask = 3u << 16;
constexpr uint32_t kSingletonInitialization = 1u << 16;
constexpr size_t kValueTypeHeaderSize = 28;
constexpr size_t kClassHeaderSize = 44;
constexpr uint32_t kClassHasResilientSuperclass = 1u << 29;

struct MetadataResponse {
    void* metadata;
    size_t state;
};

// The metadata form of an Objective-C class: lets it stand as a generic argument, stored as one retained word.
extern "C" void* rt_getObjCClassMetadata(void* cls);
extern "C" void* rt_getObjCClassFromMetadata(Metadata* metadata);
extern "C" MetadataResponse rt_getSingletonMetadata(size_t, const char* descriptor);
extern "C" void rt_initStructMetadata(void** metadata, size_t, size_t field_count, const void* const* field_layouts, uint32_t* field_offsets);

// A generic type's accessor takes its parameters, then one witness table per protocol requirement.
std::vector<void*> key_arguments_for(const char* descriptor, const std::vector<void*>& parameters);
extern "C" void* rt_allocateGenericValueMetadata(const char* descriptor, void* const* arguments, const char* pattern, size_t extra_size);
extern "C" MetadataResponse rt_getGenericMetadata(size_t, void* const* arguments, const char* descriptor);

// Arguments arrive complete, witness tables included, so unlike the mangled form nothing needs looking up.
extern "C" MetadataResponse rt_getOpaqueTypeMetadata(size_t, void* const* arguments, const char* descriptor, size_t ordinal);
extern "C" Metadata* rt_getExistentialTypeMetadata(size_t class_constraint, const void*, size_t protocol_count, const void* const*);
extern "C" Metadata* rt_getFunctionTypeMetadata();
extern "C" Metadata* rt_getMetatypeMetadata(void*);
extern "C" MetadataResponse rt_getTupleTypeMetadata2(size_t, Metadata* first, Metadata* second);
extern "C" MetadataResponse rt_getTupleTypeMetadata3(size_t, Metadata* first, Metadata* second, Metadata* third);
extern "C" MetadataResponse rt_getTupleTypeMetadata(size_t, size_t flags, Metadata* const* elements);

struct TypeLayoutValue {
    size_t size;
    size_t stride;
    uint32_t flags;
    uint32_t extra_inhabitants;
};

extern "C" size_t rt_getTupleTypeLayout2(TypeLayoutValue* out, const TypeLayoutValue* first, const TypeLayoutValue* second);

struct TupleOffsets {
    size_t second;
    size_t third;
};

extern "C" TupleOffsets rt_getTupleTypeLayout3(TypeLayoutValue* out, const TypeLayoutValue* first, const TypeLayoutValue* second, const TypeLayoutValue* third);
extern "C" void rt_getTupleTypeLayout(TypeLayoutValue* out, uint32_t* offsets, size_t, size_t count, const TypeLayoutValue* const* fields);
extern "C" MetadataResponse rt_checkMetadataState(size_t, void* metadata);
extern "C" void rt_initEnumMetadataSingleCase(void** metadata, size_t, const void* payload_layout);
extern "C" void rt_initEnumMetadataSinglePayload(void** metadata, size_t, const void* payload_layout, uint32_t empty_cases);
extern "C" void rt_initEnumMetadataMultiPayload(void** metadata, size_t, size_t payload_count, const void* const* payload_layouts);
extern "C" void* rt_updateClassMetadata2(char* metadata, size_t, size_t field_count, const void* const* field_layouts, size_t* field_offsets);
extern "C" void* rt_allocateGenericClassMetadata(const char* descriptor, void* const* arguments, const char* pattern);

// Completes a generic class: finds its superclass, inherits that class's slots, then lays out its fields.
extern "C" void* rt_initClassMetadata2(char* metadata, size_t flags, size_t field_count, const void* const* field_layouts, size_t* field_offsets);

constexpr size_t kAccessorPoolSize = 256 * 1024;
constexpr size_t kAccessorStubSize = 32;

struct OpaqueRequirements {
    const char* list;
    uint16_t count;
};

// An opaque type descriptor's generic requirements; its underlying type names follow them.
inline OpaqueRequirements opaque_requirements(const char* descriptor) {
    uint32_t flags;
    memcpy(&flags, descriptor, sizeof flags);
    const char* trailing = descriptor + 2 * sizeof(int32_t);
    if (!(flags & kDescriptorGeneric)) return {trailing, 0};
    uint16_t params, count;
    memcpy(&params, trailing, sizeof params);
    memcpy(&count, trailing + 2, sizeof count);
    return {trailing + kGenericHeaderSize + ((params + 3u) & ~3u), count};
}

}  // namespace type_system
