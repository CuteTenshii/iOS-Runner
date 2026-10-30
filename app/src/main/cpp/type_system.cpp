#include "type_system.h"

#include <pthread.h>
#include <sys/mman.h>

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
#include <vector>

#include "engine.h"
#include "macho.h"

extern "C" void (*storage_destroyer_for(const char* symbol))(void*);

namespace {

constexpr uint64_t kKindStruct = 0x200;
constexpr uint64_t kKindOptional = 0x202;
constexpr uint64_t kKindTuple = 0x301;
constexpr uint64_t kKindFunction = 0x302;
constexpr uint64_t kKindExistential = 0x303;
constexpr uint64_t kKindMetatype = 0x304;

constexpr uint32_t kFlagNonPod = 0x10000;
constexpr uint32_t kFlagNonInline = 0x20000;
constexpr size_t kInlineBufferSize = 24;
constexpr size_t kPointerSize = 8;
constexpr uint32_t kPointerExtraInhabitants = 0x1000;
constexpr uint64_t kBridgePointerMask = 0x0ffffffffffffff8ULL;
constexpr uint64_t kBridgeImmediateBit = 1ULL << 63;
constexpr uint64_t kImmortal = 1ULL << 63;
constexpr uint32_t kDescriptorGeneric = 0x80;
constexpr uint8_t kMarkerAccessFunction = 0xff;
constexpr uint8_t kSymbolicAccessFunction = 0x09;
constexpr uint32_t kDescriptorKindMask = 0x1f;
constexpr uint32_t kDescriptorKindOpaque = 4;
constexpr size_t kGenericHeaderSize = 8;
constexpr size_t kGenericRequirementSize = 12;

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

enum class LayoutKind { Pod, NativeReference, BridgeReference, Aggregate, Optional, OpaqueExistential, StateStorage };

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

MetadataAllocation* allocation_of(const void* metadata) {
    return reinterpret_cast<MetadataAllocation*>(const_cast<char*>(static_cast<const char*>(metadata)) - 2 * sizeof(void*));
}
ValueWitnessTable* witnesses_of(const void* metadata) { return static_cast<void* const*>(metadata)[-1] == nullptr ? nullptr : static_cast<ValueWitnessTable*>(static_cast<void* const*>(metadata)[-1]); }
Layout* layout_of(const void* metadata) { return allocation_of(metadata)->layout; }

struct HeapObject {
    void* metadata;
    std::atomic<uint64_t> references;
};

using Destroyer = void (*)(HeapObject*);

void retain(void* raw) {
    if (raw == nullptr || reinterpret_cast<intptr_t>(raw) < 0) return;
    auto* object = static_cast<HeapObject*>(raw);
    if (!(object->references.load(std::memory_order_relaxed) & kImmortal)) object->references.fetch_add(1, std::memory_order_relaxed);
}

void release(void* raw) {
    if (raw == nullptr || reinterpret_cast<intptr_t>(raw) < 0) return;
    auto* object = static_cast<HeapObject*>(raw);
    if (object->references.load(std::memory_order_relaxed) & kImmortal) return;
    if (object->references.fetch_sub(1, std::memory_order_acq_rel) == 1) {
        reinterpret_cast<Destroyer>(static_cast<void**>(object->metadata)[-2])(object);
    }
}

void* bridge_target(void* raw) {
    const auto bits = reinterpret_cast<uint64_t>(raw);
    return bits & kBridgeImmediateBit ? nullptr : reinterpret_cast<void*>(bits & kBridgePointerMask);
}

void* witness_copy(void* destination, void* source, void* type);
void witness_destroy(void* value, void* type);

void* witness_take(void* destination, void* source, void* type) {
    memcpy(destination, source, witnesses_of(type)->size);
    return destination;
}

void* witness_assign_copy(void* destination, void* source, void* type) {
    if (destination == source) return destination;
    witness_destroy(destination, type);
    return witness_copy(destination, source, type);
}

void* witness_assign_take(void* destination, void* source, void* type) {
    witness_destroy(destination, type);
    return witness_take(destination, source, type);
}

uint8_t state_storage_tag(const void* value, const void* type) {
    return static_cast<const uint8_t*>(value)[witnesses_of(type)->size - 1];
}

void* witness_copy(void* destination, void* source, void* type) {
    const auto* layout = layout_of(type);
    auto* to = static_cast<char*>(destination);
    auto* from = static_cast<char*>(source);
    switch (layout->kind) {
        case LayoutKind::StateStorage:
            memcpy(destination, source, witnesses_of(type)->size);
            if (state_storage_tag(source, type) == layout->value_case) {
                witnesses_of(layout->payload)->initialize_with_copy(destination, source, layout->payload);
            } else {
                retain(*reinterpret_cast<void**>(from + layout->reference_offset));
            }
            break;
        case LayoutKind::Pod:
            memcpy(destination, source, witnesses_of(type)->size);
            break;
        case LayoutKind::NativeReference:
            memcpy(destination, source, kPointerSize);
            retain(*static_cast<void**>(source));
            break;
        case LayoutKind::BridgeReference:
            memcpy(destination, source, kPointerSize);
            retain(bridge_target(*static_cast<void**>(source)));
            break;
        case LayoutKind::Aggregate:
            for (const auto& field : layout->fields) witnesses_of(field.type)->initialize_with_copy(to + field.offset, from + field.offset, field.type);
            break;
        case LayoutKind::Optional: {
            const auto* payload = witnesses_of(layout->payload);
            if (payload->get_enum_tag_single_payload(source, 1, layout->payload) != 0) {
                memcpy(destination, source, witnesses_of(type)->size);
            } else {
                payload->initialize_with_copy(destination, source, layout->payload);
                payload->store_enum_tag_single_payload(destination, 0, 1, layout->payload);
            }
            break;
        }
        case LayoutKind::OpaqueExistential: {
            memcpy(destination, source, witnesses_of(type)->size);
            auto* contained = reinterpret_cast<void**>(from)[3];
            if (contained == nullptr) break;
            const auto* value = witnesses_of(contained);
            if (value->flags & kFlagNonInline) retain(*reinterpret_cast<void**>(from));
            else value->initialize_with_copy(destination, source, contained);
            break;
        }
    }
    return destination;
}

void witness_destroy(void* raw, void* type) {
    const auto* layout = layout_of(type);
    auto* value = static_cast<char*>(raw);
    switch (layout->kind) {
        case LayoutKind::StateStorage:
            if (state_storage_tag(raw, type) == layout->value_case) witnesses_of(layout->payload)->destroy(raw, layout->payload);
            else release(*reinterpret_cast<void**>(value + layout->reference_offset));
            break;
        case LayoutKind::Pod:
            break;
        case LayoutKind::NativeReference:
            release(*static_cast<void**>(raw));
            break;
        case LayoutKind::BridgeReference:
            release(bridge_target(*static_cast<void**>(raw)));
            break;
        case LayoutKind::Aggregate:
            for (const auto& field : layout->fields) witnesses_of(field.type)->destroy(value + field.offset, field.type);
            break;
        case LayoutKind::Optional:
            if (witnesses_of(layout->payload)->get_enum_tag_single_payload(raw, 1, layout->payload) == 0) {
                witnesses_of(layout->payload)->destroy(raw, layout->payload);
            }
            break;
        case LayoutKind::OpaqueExistential: {
            auto* contained = reinterpret_cast<void**>(raw)[3];
            if (contained == nullptr) break;
            const auto* witnesses = witnesses_of(contained);
            if (witnesses->flags & kFlagNonInline) release(*reinterpret_cast<void**>(raw));
            else witnesses->destroy(raw, contained);
            break;
        }
    }
}

void* witness_buffer_copy(void* destination, void* source, void* type) {
    const auto* witnesses = witnesses_of(type);
    if (witnesses->flags & kFlagNonInline) {
        auto* box = *static_cast<void**>(source);
        *static_cast<void**>(destination) = box;
        retain(box);
        return static_cast<char*>(box) + ((sizeof(HeapObject) + (witnesses->flags & 0xff)) & ~static_cast<size_t>(witnesses->flags & 0xff));
    }
    return witness_copy(destination, source, type);
}

uint32_t extra_inhabitant_get(const void* value, void* type) {
    const auto* layout = layout_of(type);
    const auto* bytes = static_cast<const char*>(value);
    switch (layout->kind) {
        case LayoutKind::NativeReference:
        case LayoutKind::BridgeReference: {
            uint64_t bits;
            memcpy(&bits, value, sizeof bits);
            return bits < kPointerExtraInhabitants ? static_cast<uint32_t>(bits) + 1 : 0;
        }
        case LayoutKind::Aggregate:
            for (const auto& field : layout->fields) {
                if (witnesses_of(field.type)->extra_inhabitants != 0) return extra_inhabitant_get(bytes + field.offset, field.type);
            }
            return 0;
        case LayoutKind::Optional: {
            const uint32_t inner = extra_inhabitant_get(value, layout->payload);
            return inner <= 1 ? 0 : inner - 1;
        }
        case LayoutKind::OpaqueExistential: {
            uint64_t bits;
            memcpy(&bits, bytes + 3 * kPointerSize, sizeof bits);
            return bits < kPointerExtraInhabitants ? static_cast<uint32_t>(bits) + 1 : 0;
        }
        case LayoutKind::Pod:
        case LayoutKind::StateStorage:
            return 0;
    }
    return 0;
}

void extra_inhabitant_store(void* value, uint32_t tag, void* type) {
    const auto* layout = layout_of(type);
    auto* bytes = static_cast<char*>(value);
    switch (layout->kind) {
        case LayoutKind::NativeReference:
        case LayoutKind::BridgeReference: {
            const uint64_t bits = tag - 1;
            memcpy(value, &bits, sizeof bits);
            break;
        }
        case LayoutKind::Aggregate:
            for (const auto& field : layout->fields) {
                if (witnesses_of(field.type)->extra_inhabitants != 0) {
                    extra_inhabitant_store(bytes + field.offset, tag, field.type);
                    return;
                }
            }
            break;
        case LayoutKind::Optional:
            extra_inhabitant_store(value, tag + 1, layout->payload);
            break;
        case LayoutKind::OpaqueExistential: {
            const uint64_t bits = tag - 1;
            memcpy(bytes + 3 * kPointerSize, &bits, sizeof bits);
            break;
        }
        case LayoutKind::Pod:
        case LayoutKind::StateStorage:
            break;
    }
}

uint32_t tag_bytes_needed(size_t payload_size, uint32_t empty_cases) {
    if (payload_size >= 4) return 1;
    const uint64_t states = (empty_cases + (1ULL << (payload_size * 8)) - 1) >> (payload_size * 8);
    const uint64_t cases = states + 1;
    return cases <= 0x100 ? 1 : cases <= 0x10000 ? 2 : 4;
}

uint32_t witness_get_tag(const void* value, uint32_t empty_cases, void* type) {
    const auto* witnesses = witnesses_of(type);
    const size_t size = witnesses->size;
    const uint32_t inhabitants = witnesses->extra_inhabitants;
    if (empty_cases > inhabitants) {
        const uint32_t overflow = empty_cases - inhabitants;
        const uint32_t count = tag_bytes_needed(size, overflow);
        uint32_t extra = 0;
        memcpy(&extra, static_cast<const char*>(value) + size, count);
        if (extra != 0) {
            const uint32_t high = size >= 4 ? 0 : (extra - 1) << (size * 8);
            uint32_t low = 0;
            memcpy(&low, value, size < 4 ? size : 4);
            return (high | low) + inhabitants + 1;
        }
    }
    return inhabitants == 0 ? 0 : extra_inhabitant_get(value, type);
}

void witness_store_tag(void* value, uint32_t tag, uint32_t empty_cases, void* type) {
    const auto* witnesses = witnesses_of(type);
    const size_t size = witnesses->size;
    const uint32_t inhabitants = witnesses->extra_inhabitants;
    auto* bytes = static_cast<char*>(value);
    const uint32_t count = empty_cases > inhabitants ? tag_bytes_needed(size, empty_cases - inhabitants) : 0;
    if (tag <= inhabitants) {
        if (count != 0) memset(bytes + size, 0, count);
        if (tag != 0) extra_inhabitant_store(value, tag, type);
        return;
    }
    const uint32_t index = tag - inhabitants - 1;
    uint32_t extra = 1;
    if (size < 4) extra += index >> (size * 8);
    memcpy(bytes + size, &extra, count);
    memset(value, 0, size);
    memcpy(value, &index, size < 4 ? size : 4);
}

std::recursive_mutex g_types_lock;
std::map<std::string, Metadata*> g_named_types;
std::map<const void*, const char*> g_foreign_names;

Metadata* make_metadata(uint64_t kind, const void* descriptor, const std::vector<void*>& arguments, Layout* layout,
                        size_t size, size_t alignment, uint32_t extra_inhabitants) {
    auto* witnesses = new ValueWitnessTable();
    witnesses->initialize_buffer_with_copy = witness_buffer_copy;
    witnesses->destroy = witness_destroy;
    witnesses->initialize_with_copy = witness_copy;
    witnesses->assign_with_copy = witness_assign_copy;
    witnesses->initialize_with_take = witness_take;
    witnesses->assign_with_take = witness_assign_take;
    witnesses->get_enum_tag_single_payload = witness_get_tag;
    witnesses->store_enum_tag_single_payload = witness_store_tag;
    witnesses->size = size;
    witnesses->stride = size == 0 ? 1 : (size + alignment - 1) & ~(alignment - 1);
    witnesses->flags = static_cast<uint32_t>(alignment - 1);
    if (layout->kind != LayoutKind::Pod) witnesses->flags |= kFlagNonPod;
    if (size > kInlineBufferSize || alignment > kPointerSize) witnesses->flags |= kFlagNonInline;
    witnesses->extra_inhabitants = extra_inhabitants;
    // Resilient framework enums are one word here: the case index, tagged so refcounting skips it.
    witnesses->get_enum_tag = +[](const void* value, void*) { return static_cast<uint32_t>(*static_cast<const uint64_t*>(value)); };
    witnesses->destructive_project_enum_data = +[](void*, void*) {};
    witnesses->destructive_inject_enum_tag = +[](void* value, uint32_t tag, void* self) {
        if (layout_of(self)->kind == LayoutKind::StateStorage) {
            static_cast<uint8_t*>(value)[witnesses_of(self)->size - 1] = static_cast<uint8_t>(tag);
        } else {
            *static_cast<uint64_t*>(value) = kBridgeImmediateBit | tag;
        }
    };

    const size_t words = 4 + (arguments.empty() ? 1 : arguments.size());
    auto* block = static_cast<MetadataAllocation*>(calloc(words, sizeof(void*)));
    block->layout = layout;
    block->witnesses = witnesses;
    block->kind = kind;
    block->descriptor = descriptor;
    for (size_t i = 0; i < arguments.size(); ++i) block->arguments[i] = arguments[i];
    return reinterpret_cast<Metadata*>(&block->kind);
}

Metadata* named_type(const std::string& key, uint64_t kind, Layout* layout, size_t size, size_t alignment,
                     uint32_t extra_inhabitants, const std::vector<void*>& arguments = {}) {
    if (const auto found = g_named_types.find(key); found != g_named_types.end()) {
        delete layout;
        return found->second;
    }
    auto* metadata = make_metadata(kind, nullptr, arguments, layout, size, alignment, extra_inhabitants);
    g_named_types[key] = metadata;
    return metadata;
}

Metadata* pod_type(const std::string& key, size_t size) {
    return named_type(key, kKindStruct, new Layout{LayoutKind::Pod, {}, nullptr}, size, size == 0 ? 1 : size, 0);
}

std::string pointer_key(const void* pointer) {
    char text[24];
    snprintf(text, sizeof text, "%p", pointer);
    return text;
}

Metadata* bridge_type(const std::string& key, const std::vector<void*>& arguments = {}) {
    return named_type(key, kKindStruct, new Layout{LayoutKind::BridgeReference, {}, nullptr}, kPointerSize, kPointerSize,
                      kPointerExtraInhabitants, arguments);
}

Metadata* word_type() { return pod_type("Bw", kPointerSize); }
Metadata* bridge_word_type() { return bridge_type("Bb"); }
Metadata* native_type() {
    return named_type("Bo", kKindStruct, new Layout{LayoutKind::NativeReference, {}, nullptr}, kPointerSize, kPointerSize, kPointerExtraInhabitants);
}

Metadata* two_word_type(const std::string& key, Metadata* first, Metadata* second) {
    return named_type(key, kKindStruct, new Layout{LayoutKind::Aggregate, {{first, 0}, {second, kPointerSize}}, nullptr},
                      2 * kPointerSize, kPointerSize, witnesses_of(first)->extra_inhabitants + witnesses_of(second)->extra_inhabitants);
}

Metadata* string_type() { return two_word_type("SS", word_type(), bridge_word_type()); }

Metadata* thick_function_type() {
    return named_type("function", kKindFunction, new Layout{LayoutKind::Aggregate, {{word_type(), 0}, {native_type(), kPointerSize}}, nullptr},
                      2 * kPointerSize, kPointerSize, kPointerExtraInhabitants);
}

Metadata* optional_of(Metadata* payload) {
    const std::string key = "Sq" + pointer_key(payload);
    if (const auto found = g_named_types.find(key); found != g_named_types.end()) return found->second;
    const auto* witnesses = witnesses_of(payload);
    const bool has_spare = witnesses->extra_inhabitants != 0;
    auto* layout = new Layout{LayoutKind::Optional, {}, payload};
    return named_type(key, kKindOptional, layout, witnesses->size + (has_spare ? 0 : 1), (witnesses->flags & 0xff) + 1,
                      has_spare ? witnesses->extra_inhabitants - 1 : 0, {payload});
}

Metadata* tuple_of(const std::vector<Metadata*>& elements) {
    if (elements.empty()) return pod_type("yt", 0);
    std::string key = "tuple";
    for (const auto* element : elements) key += pointer_key(element);
    if (const auto found = g_named_types.find(key); found != g_named_types.end()) return found->second;
    auto* layout = new Layout{LayoutKind::Aggregate, {}, nullptr};
    size_t offset = 0;
    size_t alignment = 1;
    uint32_t inhabitants = 0;
    bool pod = true;
    for (auto* element : elements) {
        const auto* witnesses = witnesses_of(element);
        const size_t mask = witnesses->flags & 0xff;
        offset = (offset + mask) & ~mask;
        layout->fields.push_back({element, offset});
        offset += witnesses->size;
        if (mask + 1 > alignment) alignment = mask + 1;
        if (witnesses->extra_inhabitants > inhabitants) inhabitants = witnesses->extra_inhabitants;
        if (witnesses->flags & kFlagNonPod) pod = false;
    }
    if (pod) layout->kind = LayoutKind::Pod;
    // ABI shape: kind, element count, labels, then a (type, offset) pair for each element.
    std::vector<void*> words = {nullptr};
    for (const auto& field : layout->fields) {
        words.push_back(field.type);
        words.push_back(reinterpret_cast<void*>(field.offset));
    }
    auto* metadata = named_type(key, kKindTuple, layout, offset, alignment, inhabitants, words);
    metadata->descriptor = reinterpret_cast<const void*>(elements.size());
    return metadata;
}

Metadata* any_type() {
    return named_type("yp", kKindExistential, new Layout{LayoutKind::OpaqueExistential, {}, nullptr}, 4 * kPointerSize, kPointerSize, kPointerExtraInhabitants);
}

Metadata* protocol_existential(const char* protocol_name) {
    const std::string name = protocol_name == nullptr ? "" : protocol_name;
    if (name == "_$ss5ErrorMp") return named_type("Error", kKindExistential, new Layout{LayoutKind::NativeReference, {}, nullptr}, kPointerSize, kPointerSize, kPointerExtraInhabitants);
    if (name == "_$sScAMp") return two_word_type("ActorExistential", native_type(), word_type());
    const std::string key = "existential" + name;
    return named_type(key, kKindExistential, new Layout{LayoutKind::OpaqueExistential, {}, nullptr}, 5 * kPointerSize, kPointerSize, kPointerExtraInhabitants);
}

const char* g_current_name = nullptr;
size_t g_current_length = 0;
std::string printable(const char* text, size_t length);

struct FrozenField {
    size_t size;      // 0 means the type comes from `argument`.
    size_t alignment;
    int argument;     // Index of a generic argument, or -1.
    bool reference;   // A retained word, rather than plain bytes.
};

Metadata* metadata_for_field(const FrozenField& field, const std::vector<void*>& arguments) {
    if (field.argument >= 0) return static_cast<Metadata*>(arguments[static_cast<size_t>(field.argument)]);
    if (field.reference) return native_type();
    std::string key = "pod" + std::to_string(field.size) + "/" + std::to_string(field.alignment);
    return named_type(key, kKindStruct, new Layout{LayoutKind::Pod, {}, nullptr}, field.size, field.alignment, 0);
}

Metadata* struct_from_fields(const std::string& key, const std::vector<FrozenField>& fields, const std::vector<void*>& arguments) {
    auto* layout = new Layout{LayoutKind::Aggregate, {}, nullptr};
    size_t offset = 0;
    size_t alignment = 1;
    uint32_t inhabitants = 0;
    bool plain = true;
    for (const auto& field : fields) {
        Metadata* type = metadata_for_field(field, arguments);
        const auto* witnesses = witnesses_of(type);
        const size_t mask = witnesses->flags & 0xff;
        offset = (offset + mask) & ~mask;
        layout->fields.push_back({type, offset});
        offset += witnesses->size;
        if (mask + 1 > alignment) alignment = mask + 1;
        if (witnesses->extra_inhabitants > inhabitants) inhabitants = witnesses->extra_inhabitants;
        if (witnesses->flags & kFlagNonPod) plain = false;
    }
    if (plain) layout->kind = LayoutKind::Pod;
    std::vector<void*> with_offsets = arguments;
    const size_t offset_words = (layout->fields.size() + 1) / 2;
    with_offsets.resize(arguments.size() + offset_words, nullptr);
    auto* metadata = named_type(key, kKindStruct, layout, offset, alignment, inhabitants, with_offsets);
    // The field offset vector follows the generic arguments; the app reads offsets from it directly.
    auto* offsets = reinterpret_cast<uint32_t*>(&metadata->arguments[arguments.size()]);
    for (size_t i = 0; i < layout->fields.size(); ++i) offsets[i] = static_cast<uint32_t>(layout->fields[i].offset);
    return metadata;
}

Metadata* state_storage_of(Metadata* value) {
    const std::string key = "StateStorage" + pointer_key(value);
    const auto* witnesses = witnesses_of(value);
    const size_t payload = witnesses->size < 2 * kPointerSize ? 2 * kPointerSize : witnesses->size;
    const size_t alignment = (witnesses->flags & 0xff) + 1 < kPointerSize ? kPointerSize : (witnesses->flags & 0xff) + 1;
    return named_type(key, kKindStruct, new Layout{LayoutKind::StateStorage, {}, value}, payload + 1, alignment, 0, {value});
}

// Environment<T>.Content: case 0 is a key path, case 1 a resolved value.
Metadata* environment_content_of(Metadata* value) {
    const std::string key = "EnvironmentContent" + pointer_key(value);
    const auto* witnesses = witnesses_of(value);
    const size_t payload = witnesses->size < kPointerSize ? kPointerSize : witnesses->size;
    const size_t alignment = (witnesses->flags & 0xff) + 1 < kPointerSize ? kPointerSize : (witnesses->flags & 0xff) + 1;
    auto* layout = new Layout{LayoutKind::StateStorage, {}, value};
    layout->value_case = 1;
    layout->reference_offset = 0;
    return named_type(key, kKindStruct, layout, payload + 1, alignment, 0, {value});
}

Metadata* lazy_state_of(Metadata* value) {
    const std::string key = "LazyState" + pointer_key(value);
    if (const auto found = g_named_types.find(key); found != g_named_types.end()) return found->second;
    Metadata* storage = state_storage_of(value);
    auto* layout = new Layout{LayoutKind::Aggregate, {}, nullptr};
    const size_t storage_size = witnesses_of(storage)->size;
    const size_t location_offset = (storage_size + kPointerSize - 1) & ~(kPointerSize - 1);
    layout->fields = {{storage, 0}, {native_type(), location_offset}};
    auto* metadata = named_type(key, kKindStruct, layout, location_offset + kPointerSize, (witnesses_of(storage)->flags & 0xff) + 1, 0, {value, nullptr});
    auto* offsets = reinterpret_cast<uint32_t*>(&metadata->arguments[1]);
    offsets[0] = 0;
    offsets[1] = static_cast<uint32_t>(location_offset);
    return metadata;
}

constexpr FrozenField kWord = {8, 8, -1, false};
constexpr FrozenField kReference = {8, 8, -1, true};
constexpr FrozenField kOptionalDouble = {9, 8, -1, false};
constexpr FrozenField kByte = {1, 1, -1, false};
constexpr FrozenField kAlignment = {16, 8, -1, false};
constexpr FrozenField kPadding = {33, 8, -1, false};
constexpr FrozenField kAnchor = {16, 8, -1, false};
constexpr FrozenField kClosure = {16, 8, -1, false};
constexpr FrozenField kFirstArgument = {0, 0, 0, false};
constexpr FrozenField kSecondArgument = {0, 0, 1, false};

// SwiftUI's @frozen types, from its public interface: the app's compiled code writes these fields inline.
const std::unordered_map<std::string_view, std::vector<FrozenField>>& frozen_layouts() {
    static const std::unordered_map<std::string_view, std::vector<FrozenField>> table = {
        {"_$s7SwiftUI15ModifiedContentVMn", {kFirstArgument, kSecondArgument}},
        {"_$s7SwiftUI4TextVMn", {kWord, kWord, kWord, kReference}},
        {"_$s7SwiftUI12_FrameLayoutVMn", {kOptionalDouble, kOptionalDouble, kAlignment}},
        {"_$s7SwiftUI14_PaddingLayoutVMn", {kByte, kPadding}},
        {"_$s7SwiftUI18_AspectRatioLayoutVMn", {kOptionalDouble, kByte}},
        {"_$s7SwiftUI25_AllowsHitTestingModifierVMn", {kByte}},
        {"_$s7SwiftUI15_RotationEffectVMn", {kWord, kAnchor}},
        {"_$s7SwiftUI25_AppearanceActionModifierVMn", {kClosure, kClosure}},
        {"_$s7SwiftUI24_BackgroundStyleModifierVMn", {kFirstArgument, kByte}},
        {"_$s7SwiftUI16_OverlayModifierVMn", {kFirstArgument, kAlignment}},
        {"_$s7SwiftUI11_ClipEffectVMn", {kFirstArgument, {2, 1, -1, false}}},
        {"_$s7SwiftUI21_ContentShapeModifierVMn", {kFirstArgument, kByte}},
        {"_$s7SwiftUI18_AnimationModifierVMn", {kReference, kFirstArgument}},
        {"_$s7SwiftUI30_EnvironmentKeyWritingModifierVMn", {kReference, kFirstArgument}},
        {"_$s7SwiftUI32_EnvironmentKeyTransformModifierVMn", {kReference, kClosure}},
        {"_$s7SwiftUI21_TraitWritingModifierVMn", {kFirstArgument}},
        {"_$s7SwiftUI6HStackVMn", {kWord, kOptionalDouble, kFirstArgument}},
        {"_$s7SwiftUI6VStackVMn", {kWord, kOptionalDouble, kFirstArgument}},
        {"_$s7SwiftUI6ZStackVMn", {kAlignment, kFirstArgument}},
        {"_$s7SwiftUI6SpacerVMn", {kOptionalDouble}},
        {"_$s7SwiftUI6CircleVMn", {}},
        {"_$s7SwiftUI9RectangleVMn", {}},
        {"_$s7SwiftUI9EmptyViewVMn", {}},
        {"_$s7SwiftUI12TupleContentVMn", {kFirstArgument}},
        {"_$s7SwiftUI16_TupleTabContentVMn", {kFirstArgument, kSecondArgument}},
    };
    return table;
}

Metadata* frozen_layout_type(const std::string& key, const std::string& symbol, const std::vector<void*>& arguments) {
    if (symbol == "_$s7SwiftUI9LazyStateVMn" && !arguments.empty()) return lazy_state_of(static_cast<Metadata*>(arguments[0]));
    if (symbol == "_$s7SwiftUI9LazyStateV7StorageOMn" && !arguments.empty()) return state_storage_of(static_cast<Metadata*>(arguments[0]));
    if (symbol == "_$s7SwiftUI11EnvironmentV7ContentOMn" && !arguments.empty()) return environment_content_of(static_cast<Metadata*>(arguments[0]));
    if (symbol == "_$s7SwiftUI11EnvironmentVMn" && !arguments.empty()) {
        Metadata* content = environment_content_of(static_cast<Metadata*>(arguments[0]));
        const auto* witnesses = witnesses_of(content);
        std::vector<void*> with_offset = arguments;
        with_offset.push_back(nullptr);
        auto* metadata = named_type(key, kKindStruct, new Layout{LayoutKind::Aggregate, {{content, 0}}, nullptr}, witnesses->size, (witnesses->flags & 0xff) + 1, 0, with_offset);
        return metadata;
    }
    const auto found = frozen_layouts().find(symbol);
    if (found == frozen_layouts().end()) return nullptr;
    for (const auto& field : found->second) {
        if (field.argument >= 0 && (static_cast<size_t>(field.argument) >= arguments.size() || arguments[static_cast<size_t>(field.argument)] == nullptr)) {
            engine_log("STOP: %s is missing generic argument %d of %zu, in \"%s\"", symbol.c_str(), field.argument, arguments.size(), printable(g_current_name, g_current_length).c_str());
            abort();
        }
    }
    return struct_from_fields(key, found->second, arguments);
}

// Types whose stored layout the app's compiled code depends on; everything else from a framework is one reference.
Metadata* foreign_nominal(const std::string& symbol, const std::vector<void*>& arguments) {
    std::string key = symbol;
    for (const void* argument : arguments) key += pointer_key(argument);
    if (const auto found = g_named_types.find(key); found != g_named_types.end()) return found->second;
    if (storage_destroyer_for(symbol.c_str()) != nullptr) {
        auto* block = static_cast<void**>(calloc(4 + (arguments.empty() ? 1 : arguments.size()), sizeof(void*)));
        block[0] = reinterpret_cast<void*>(storage_destroyer_for(symbol.c_str()));
        block[2] = nullptr;
        auto* metadata = reinterpret_cast<Metadata*>(block + 2);
        for (size_t i = 0; i < arguments.size(); ++i) block[4 + i] = arguments[i];
        g_named_types[key] = metadata;
        g_foreign_names[metadata] = strdup(symbol.c_str());
        return metadata;
    }
    Metadata* metadata = frozen_layout_type(key, symbol, arguments);
    if (metadata == nullptr) {
        metadata = named_type(key, kKindStruct, new Layout{LayoutKind::NativeReference, {}, nullptr}, kPointerSize, kPointerSize, kPointerExtraInhabitants, arguments);
    }
    g_foreign_names[metadata] = strdup(symbol.c_str());
    return metadata;
}

struct NominalReference {
    const void* descriptor = nullptr;
    const char* foreign_symbol = nullptr;
};

struct Node {
    enum class Tag { Type, Nominal, Protocol, ListMarker, FirstElement } tag = Tag::Type;
    Metadata* type = nullptr;
    NominalReference nominal;
};

using MetadataAccessor = void* (*)(uintptr_t, void*, void*, void*);

Metadata* metadata_for_descriptor(const NominalReference& nominal, const std::vector<void*>& arguments) {
    if (nominal.foreign_symbol != nullptr) return foreign_nominal(nominal.foreign_symbol, arguments);
    const auto* descriptor = static_cast<const char*>(nominal.descriptor);
    int32_t offset;
    memcpy(&offset, descriptor + 12, sizeof offset);
    auto accessor = reinterpret_cast<MetadataAccessor>(const_cast<char*>(descriptor + 12 + offset));
    if (arguments.size() <= 3) {
        void* first = arguments.size() > 0 ? arguments[0] : nullptr;
        void* second = arguments.size() > 1 ? arguments[1] : nullptr;
        void* third = arguments.size() > 2 ? arguments[2] : nullptr;
        return static_cast<Metadata*>(accessor(0, first, second, third));
    }
    return static_cast<Metadata*>(accessor(0, const_cast<void**>(arguments.data()), nullptr, nullptr));
}

class MangledName {
public:
    MangledName(const char* text, size_t length, void* const* generics) : cursor_(text), end_(text + length), generics_(generics) {}

    Metadata* parse() {
        while (cursor_ < end_ && failure_.empty()) step();
        if (failure_.empty() && (stack_.size() != 1 || stack_.back().tag == Node::Tag::ListMarker || stack_.back().tag == Node::Tag::FirstElement)) fail("unbalanced type stack");
        if (!failure_.empty()) return nullptr;
        return as_type(stack_.back());
    }
    const std::string& failure() const { return failure_; }

private:
    void fail(const std::string& reason) {
        if (failure_.empty()) failure_ = reason;
    }

    Metadata* as_type(const Node& node) {
        if (node.tag == Node::Tag::Type) return node.type;
        if (node.tag == Node::Tag::Nominal) return metadata_for_descriptor(node.nominal, {});
        if (node.tag == Node::Tag::Protocol) return protocol_existential(node.nominal.foreign_symbol);
        fail("list marker used as a type");
        return nullptr;
    }

    Node pop() {
        if (stack_.empty()) {
            fail("type stack underflow");
            return {};
        }
        Node node = stack_.back();
        stack_.pop_back();
        return node;
    }

    void push_type(Metadata* type) {
        Node node;
        node.type = type;
        stack_.push_back(node);
    }

    void substitute(const Node& node) { substitutions_.push_back(node); }

    uint64_t read_natural() {
        uint64_t value = 0;
        while (cursor_ < end_ && *cursor_ >= '0' && *cursor_ <= '9') value = value * 10 + static_cast<uint64_t>(*cursor_++ - '0');
        return value;
    }

    void symbolic(uint8_t code) {
        int32_t offset;
        memcpy(&offset, cursor_, sizeof offset);
        const void* target = cursor_ + offset;
        cursor_ += 4;
        if (code == 2) target = *static_cast<void* const*>(target);
        Node node;
        node.tag = Node::Tag::Nominal;
        if (const char* foreign = unresolved_symbol_at(reinterpret_cast<uintptr_t>(target))) node.nominal.foreign_symbol = foreign;
        else node.nominal.descriptor = target;
        if (node.nominal.foreign_symbol != nullptr && strstr(node.nominal.foreign_symbol, "Mp") != nullptr &&
            strcmp(node.nominal.foreign_symbol + strlen(node.nominal.foreign_symbol) - 2, "Mp") == 0) {
            node.tag = Node::Tag::Protocol;
        } else if (cursor_ < end_ && *cursor_ == '_' && cursor_ + 1 < end_ && cursor_[1] == 'p') {
            node.tag = Node::Tag::Protocol;
        }
        stack_.push_back(node);
        if (!names_opaque_declaration(node.nominal)) substitute(node);
    }

    // An opaque type descriptor names a declaration rather than a type, so it takes no substitution slot.
    static bool names_opaque_declaration(const NominalReference& nominal) {
        if (nominal.foreign_symbol != nullptr) {
            const std::string_view symbol(nominal.foreign_symbol);
            return symbol.size() >= 4 && symbol.substr(symbol.size() - 4) == "QOMQ";
        }
        if (nominal.descriptor == nullptr) return false;
        uint32_t flags;
        memcpy(&flags, nominal.descriptor, sizeof flags);
        return (flags & kDescriptorKindMask) == kDescriptorKindOpaque;
    }

    void standard(char code) {
        switch (code) {
            case 'S': push_type(string_type()); return;
            case 'i': push_type(pod_type("Si", 8)); return;
            case 'u': push_type(pod_type("Su", 8)); return;
            case 'b': push_type(named_type("Sb", kKindStruct, new Layout{LayoutKind::Pod, {}, nullptr}, 1, 1, 254)); return;
            case 'd': push_type(pod_type("Sd", 8)); return;
            case 'f': push_type(pod_type("Sf", 4)); return;
            case 'J': push_type(two_word_type("SJ", word_type(), bridge_word_type())); return;
            case 's': push_type(named_type("Ss", kKindStruct, new Layout{LayoutKind::Aggregate, {{word_type(), 0}, {word_type(), 8}, {word_type(), 16}, {bridge_word_type(), 24}}, nullptr}, 32, 8, kPointerExtraInhabitants)); return;
            case 'g': {
                Metadata* payload = as_type(pop());
                if (payload == nullptr) return;
                Node node;
                node.type = optional_of(payload);
                stack_.push_back(node);
                substitute(node);
                return;
            }
            case 'a': case 'D': case 'h': {
                Node node;
                node.tag = Node::Tag::Nominal;
                node.nominal.foreign_symbol = code == 'a' ? "$Array" : code == 'D' ? "$Dictionary" : "$Set";
                stack_.push_back(node);
                return;
            }
            case 'c': {
                if (cursor_ >= end_) break;
                const char concurrency = *cursor_++;
                if (concurrency == 'A') {
                    Node node;
                    node.tag = Node::Tag::Protocol;
                    node.nominal.foreign_symbol = "_$sScAMp";
                    stack_.push_back(node);
                    return;
                }
                if (concurrency == 'M') { push_type(native_type()); return; }
                if (concurrency == 'P') { push_type(pod_type("ScP", 1)); return; }
                fail(std::string("unknown concurrency type Sc") + concurrency);
                return;
            }
            default:
                break;
        }
        fail(std::string("unknown standard type S") + code);
    }

    std::vector<void*> pop_generic_arguments() {
        std::vector<void*> arguments;
        while (!stack_.empty() && stack_.back().tag != Node::Tag::ListMarker) {
            Node top = pop();
            if (top.tag == Node::Tag::FirstElement) continue;
            arguments.insert(arguments.begin(), as_type(top));
        }
        if (stack_.empty()) fail("generic arguments without a list start");
        else pop();
        return arguments;
    }

    void bound_generic() {
        std::vector<void*> arguments = pop_generic_arguments();
        if (!failure_.empty()) return;
        for (const void* argument : arguments) {
            if (argument == nullptr) { fail("generic argument did not resolve"); return; }
        }
        Node base = pop();
        if (base.tag != Node::Tag::Nominal) {
            fail("bound generic base is not a nominal type");
            return;
        }
        Node node;
        const char* name = base.nominal.foreign_symbol;
        if (name != nullptr && name[0] == '$') {
            std::string key = name;
            for (const void* argument : arguments) key += pointer_key(argument);
            node.type = bridge_type(key, arguments);
        } else {
            node.type = metadata_for_descriptor(base.nominal, arguments);
        }
        stack_.push_back(node);
        substitute(node);
    }

    void tuple() {
        std::vector<Metadata*> elements;
        while (!stack_.empty()) {
            Node top = pop();
            if (top.tag == Node::Tag::FirstElement) {
                elements.insert(elements.begin(), as_type(pop()));
                break;
            }
            elements.insert(elements.begin(), as_type(top));
        }
        if (!failure_.empty()) return;
        for (const auto* element : elements) {
            if (element == nullptr) { fail("tuple element did not resolve"); return; }
        }
        push_type(tuple_of(elements));
    }

    void opaque_type() {
        if (cursor_ >= end_) { fail("truncated Q operator"); return; }
        const char variety = *cursor_++;
        // A pack is carried as its tuple, whose layout is all the runtime needs from it.
        if (variety == 'P') return;
        if (variety != 'o') {
            fail(std::string("unsupported Q operator ") + variety);
            return;
        }
        const uint64_t ordinal = cursor_ < end_ && *cursor_ == '_' ? 0 : read_natural() + 1;
        if (cursor_ >= end_ || *cursor_++ != '_') { fail("bad opaque type ordinal"); return; }
        std::vector<void*> arguments = pop_generic_arguments();
        Node base = pop();
        if (base.tag != Node::Tag::Nominal) { fail("opaque type without a descriptor"); return; }

        Node node;
        if (base.nominal.foreign_symbol != nullptr) {
            node.type = foreign_nominal(base.nominal.foreign_symbol, arguments);
        } else {
            const auto* descriptor = static_cast<const char*>(base.nominal.descriptor);
            uint32_t flags;
            memcpy(&flags, descriptor, sizeof flags);
            const char* trailing = descriptor + 2 * sizeof(int32_t);
            if (flags & kDescriptorGeneric) {
                uint16_t params, requirements;
                memcpy(&params, trailing, sizeof params);
                memcpy(&requirements, trailing + 2, sizeof requirements);
                trailing += kGenericHeaderSize + ((params + 3u) & ~3u) + requirements * kGenericRequirementSize;
            }
            int32_t offset;
            memcpy(&offset, trailing + ordinal * sizeof(int32_t), sizeof offset);
            const char* underlying = trailing + ordinal * sizeof(int32_t) + offset;
            size_t length = 0;
            while (underlying[length] != 0) {
                const auto byte = static_cast<uint8_t>(underlying[length]);
                length += byte >= 1 && byte <= 0x17 ? 5 : 1;
            }
            MangledName nested(underlying, length, arguments.data());
            node.type = nested.parse();
            if (node.type == nullptr) { fail("opaque underlying type: " + nested.failure()); return; }
        }
        stack_.push_back(node);
        substitute(node);
    }

    void implementation_function() {
        size_t consumed = 0;
        while (cursor_ < end_ && *cursor_ != '_') {
            if (strchr("inxygelmrodukaR", *cursor_) != nullptr) ++consumed;
            ++cursor_;
        }
        if (cursor_ >= end_) {
            fail("unterminated function type");
            return;
        }
        ++cursor_;
        // The first convention letter describes the callee, which has no type on the stack.
        for (size_t i = 1; i < consumed && !stack_.empty(); ++i) pop();
        push_type(thick_function_type());
    }

    void step() {
        const char code = *cursor_++;
        const auto byte = static_cast<uint8_t>(code);
        // 0xFF only announces that a metadata access function follows.
        if (byte == kMarkerAccessFunction) return;
        if (byte == kSymbolicAccessFunction) {
            int32_t offset;
            memcpy(&offset, cursor_, sizeof offset);
            auto function = reinterpret_cast<void* (*)(void* const*)>(const_cast<char*>(cursor_ + offset));
            cursor_ += sizeof offset;
            push_type(static_cast<Metadata*>(function(generics_)));
            return;
        }
        if (byte >= 1 && byte <= 0x17) {
            if (byte != 1 && byte != 2) {
                fail("unsupported symbolic reference kind");
                return;
            }
            symbolic(byte);
            return;
        }
        switch (code) {
            case 'S':
                if (cursor_ >= end_) { fail("truncated standard type"); return; }
                standard(*cursor_++);
                return;
            case 'y':
                if (cursor_ < end_ && *cursor_ == 't') {
                    ++cursor_;
                    push_type(pod_type("yt", 0));
                } else if (cursor_ < end_ && *cursor_ == 'p') {
                    ++cursor_;
                    push_type(any_type());
                } else {
                    Node marker;
                    marker.tag = Node::Tag::ListMarker;
                    stack_.push_back(marker);
                }
                return;
            case 'G':
                bound_generic();
                return;
            case 'c':
                pop();
                pop();
                push_type(thick_function_type());
                return;
            case 't':
                tuple();
                return;
            case '_':
                if (cursor_ < end_ && *cursor_ == 'p') {
                    ++cursor_;
                    Node top = pop();
                    if (top.tag == Node::Tag::Nominal) top.tag = Node::Tag::Protocol;
                    push_type(as_type(top));
                } else {
                    Node marker;
                    marker.tag = Node::Tag::FirstElement;
                    stack_.push_back(marker);
                }
                return;
            case 'x':
                generic(0, 0);
                return;
            case 'q':
                generic_parameter();
                return;
            case 'A':
                substitution();
                return;
            case 'X':
                if (cursor_ < end_ && *cursor_ == 'l') {
                    // `yXl` is AnyObject: a composition of no protocols, whose empty list was just pushed.
                    ++cursor_;
                    if (!stack_.empty() && stack_.back().tag == Node::Tag::ListMarker) pop();
                    push_type(native_type());
                    return;
                }
                fail("unsupported X type");
                return;
            case 'm':
                pop();
                push_type(named_type("metatype", kKindMetatype, new Layout{LayoutKind::Pod, {}, nullptr}, kPointerSize, kPointerSize, 0));
                return;
            case 'B':
                builtin();
                return;
            case 'I':
                implementation_function();
                return;
            case 'Q':
                opaque_type();
                return;
            default:
                fail(std::string("unsupported operator '") + code + "'");
                return;
        }
    }

    void builtin() {
        if (cursor_ >= end_) { fail("truncated builtin"); return; }
        const char code = *cursor_++;
        switch (code) {
            case 'D': push_type(pod_type("BD", 96)); return;
            case 'o': case 'O': push_type(native_type()); return;
            case 'b': push_type(bridge_word_type()); return;
            case 'w': push_type(word_type()); return;
            case 'p': push_type(word_type()); return;
            default: fail(std::string("unknown builtin B") + code);
        }
    }

    void generic(uint64_t depth, uint64_t index) {
        if (depth != 0) {
            fail("nested generic parameters are not supported yet");
            return;
        }
        if (generics_ == nullptr) {
            fail("generic parameter without generic arguments");
            return;
        }
        push_type(static_cast<Metadata*>(generics_[index]));
    }

    void generic_parameter() {
        if (cursor_ < end_ && *cursor_ == 'd') {
            ++cursor_;
            const uint64_t depth = read_natural() + 1;
            if (cursor_ >= end_ || *cursor_++ != '_') { fail("bad generic depth"); return; }
            const uint64_t index = cursor_ < end_ && *cursor_ == '_' ? 0 : read_natural() + 1;
            if (cursor_ >= end_ || *cursor_++ != '_') { fail("bad generic index"); return; }
            generic(depth, index);
            return;
        }
        const uint64_t index = cursor_ < end_ && *cursor_ == '_' ? 0 : read_natural() + 1;
        if (cursor_ >= end_ || *cursor_++ != '_') { fail("bad generic parameter"); return; }
        generic(0, index + 1);
    }

    void substitution() {
        for (;;) {
            uint64_t repeat = 1;
            if (cursor_ < end_ && *cursor_ >= '0' && *cursor_ <= '9') repeat = read_natural();
            if (cursor_ >= end_) { fail("truncated substitution"); return; }
            const char letter = *cursor_++;
            const bool last = letter >= 'A' && letter <= 'Z';
            const uint64_t index = static_cast<uint64_t>(last ? letter - 'A' : letter - 'a');
            if (index >= substitutions_.size()) { fail("substitution out of range"); return; }
            for (uint64_t i = 0; i < repeat; ++i) stack_.push_back(substitutions_[index]);
            if (last) return;
        }
    }

    const char* cursor_;
    const char* end_;
    void* const* generics_;
    std::vector<Node> stack_;
    std::vector<Node> substitutions_;
    std::string failure_;
};

std::string printable(const char* text, size_t length) {
    std::string out;
    for (size_t i = 0; i < length; ++i) {
        const auto byte = static_cast<uint8_t>(text[i]);
        if (byte >= 1 && byte <= 0x17) {
            int32_t offset;
            memcpy(&offset, text + i + 1, sizeof offset);
            const void* target = text + i + 1 + offset;
            if (byte == 2) target = *static_cast<void* const*>(target);
            const char* symbol = unresolved_symbol_at(reinterpret_cast<uintptr_t>(target));
            out += symbol != nullptr ? std::string("<") + symbol + ">" : std::string("<app>");
            i += 4;
        } else if (byte >= 0x20 && byte < 0x7f) {
            out += static_cast<char>(byte);
        } else {
            out += '?';
        }
    }
    return out;
}

extern "C" void* rt_getTypeByMangledNameInContext2(const char* name, size_t length, const void*, void* const* generics) {
    const std::lock_guard guard(g_types_lock);
    const char* outer_name = g_current_name;
    const size_t outer_length = g_current_length;
    g_current_name = name;
    g_current_length = length;
    MangledName parser(name, length, generics);
    Metadata* result = parser.parse();
    g_current_name = outer_name;
    g_current_length = outer_length;
    if (result == nullptr) {
        engine_log("STOP: cannot resolve type \"%s\": %s", printable(name, length).c_str(), parser.failure().c_str());
        abort();
    }
    return result;
}

extern "C" void* rt_getTypeByMangledNameInContextInMetadataState2(size_t, const char* name, size_t length, const void* context, void* const* generics) {
    return rt_getTypeByMangledNameInContext2(name, length, context, generics);
}


constexpr uint32_t kKindMask = 0x1f;
constexpr uint32_t kContextClass = 16;
constexpr uint32_t kFlagGeneric = 0x80;
constexpr uint32_t kSingletonInitializationMask = 3u << 16;
constexpr uint32_t kSingletonInitialization = 1u << 16;
constexpr size_t kValueTypeHeaderSize = 28;
constexpr size_t kClassHeaderSize = 44;
constexpr uint32_t kClassHasResilientSuperclass = 1u << 29;
constexpr size_t kValueWitnessSizeOffset = offsetof(ValueWitnessTable, size);

struct MetadataResponse {
    void* metadata;
    size_t state;
};

struct SingletonInitialization {
    int32_t cache;
    int32_t incomplete;
    int32_t completion;
};

using CompletionFunction = void* (*)(void*, void*, void*);

const void* relative_target(const int32_t* field) { return reinterpret_cast<const char*>(field) + *field; }

constexpr uint64_t kKindObjCClassWrapper = 0x305;

// The metadata form of an Objective-C class: lets it stand as a generic argument, stored as one retained word.
extern "C" void* rt_getObjCClassMetadata(void* cls) {
    const std::lock_guard guard(g_types_lock);
    if (unresolved_symbol_at(reinterpret_cast<uintptr_t>(cls)) == nullptr) return cls;
    const std::string key = "objc" + pointer_key(cls);
    if (const auto found = g_named_types.find(key); found != g_named_types.end()) return found->second;
    auto* metadata = make_metadata(kKindObjCClassWrapper, cls, {}, new Layout{LayoutKind::NativeReference, {}, nullptr}, kPointerSize, kPointerSize, kPointerExtraInhabitants);
    g_named_types[key] = metadata;
    return metadata;
}

extern "C" void* rt_getObjCClassFromMetadata(Metadata* metadata) {
    return metadata->kind == kKindObjCClassWrapper ? const_cast<void*>(metadata->descriptor) : metadata;
}

extern "C" MetadataResponse rt_getSingletonMetadata(size_t, const char* descriptor) {
    static pthread_mutex_t lock = PTHREAD_RECURSIVE_MUTEX_INITIALIZER_NP;
    uint32_t flags;
    memcpy(&flags, descriptor, sizeof flags);
    size_t header = (flags & kKindMask) == kContextClass ? kClassHeaderSize : kValueTypeHeaderSize;
    if (flags & kFlagGeneric) {
        engine_log("STOP: swift_getSingletonMetadata on a generic descriptor");
        abort();
    }
    if ((flags & kKindMask) == kContextClass && (flags & kClassHasResilientSuperclass)) header += sizeof(int32_t);
    if ((flags & kSingletonInitializationMask) != kSingletonInitialization) {
        engine_log("STOP: swift_getSingletonMetadata on a descriptor without singleton initialization");
        abort();
    }
    const auto* initialization = reinterpret_cast<const SingletonInitialization*>(descriptor + header);
    auto** cache = const_cast<void**>(static_cast<void* const*>(relative_target(&initialization->cache)));

    pthread_mutex_lock(&lock);
    if (cache[0] == nullptr) {
        void* metadata = const_cast<void*>(relative_target(&initialization->incomplete));
        auto completion = reinterpret_cast<CompletionFunction>(const_cast<void*>(relative_target(&initialization->completion)));
        cache[0] = metadata;
        void* dependency = completion(metadata, nullptr, nullptr);
        if (dependency != nullptr) {
            engine_log("STOP: metadata completion reported an unresolved dependency");
            abort();
        }
    }
    void* result = cache[0];
    pthread_mutex_unlock(&lock);
    return {result, 0};
}

const ValueWitnessTable* witnesses_from_type_layout(const void* layout) {
    return reinterpret_cast<const ValueWitnessTable*>(static_cast<const char*>(layout) - kValueWitnessSizeOffset);
}

extern "C" void rt_initStructMetadata(void** metadata, size_t, size_t field_count, const void* const* field_layouts, uint32_t* field_offsets) {
    size_t offset = 0;
    size_t alignment_mask = 0;
    uint32_t flags = 0;
    uint32_t inhabitants = 0;
    for (size_t i = 0; i < field_count; ++i) {
        const auto* field = witnesses_from_type_layout(field_layouts[i]);
        const size_t mask = field->flags & 0xff;
        offset = (offset + mask) & ~mask;
        field_offsets[i] = static_cast<uint32_t>(offset);
        offset += field->size;
        if (mask > alignment_mask) alignment_mask = mask;
        flags |= field->flags & (kFlagNonPod | kFlagNonInline);
        if (field->extra_inhabitants > inhabitants) inhabitants = field->extra_inhabitants;
    }
    auto* updated = new ValueWitnessTable(*static_cast<ValueWitnessTable*>(metadata[-1]));
    updated->size = offset;
    updated->stride = offset == 0 ? 1 : (offset + alignment_mask) & ~alignment_mask;
    updated->flags = static_cast<uint32_t>(alignment_mask) | (flags & kFlagNonPod);
    if (offset > kInlineBufferSize || alignment_mask >= kPointerSize) updated->flags |= kFlagNonInline;
    updated->extra_inhabitants = inhabitants;
    metadata[-1] = updated;
}

constexpr size_t kClassSuperclassOffset = 8;
constexpr size_t kClassInstanceSizeOffset = 48;
constexpr size_t kClassInstanceAlignOffset = 52;
constexpr size_t kClassDataOffset = 32;
constexpr size_t kRoInstanceSizeOffset = 8;
constexpr size_t kRoIvarsOffset = 48;
constexpr uint32_t kRootInstanceSize = sizeof(HeapObject);

extern "C" void* rt_updateClassMetadata2(char* metadata, size_t, size_t field_count, const void* const* field_layouts, size_t* field_offsets) {
    uint32_t size = kRootInstanceSize;
    uint16_t alignment_mask = 15;
    void* super;
    memcpy(&super, metadata + kClassSuperclassOffset, sizeof super);
    if (super != nullptr && unresolved_symbol_at(reinterpret_cast<uintptr_t>(super)) == nullptr) {
        memcpy(&size, static_cast<char*>(super) + kClassInstanceSizeOffset, sizeof size);
        uint16_t inherited;
        memcpy(&inherited, static_cast<char*>(super) + kClassInstanceAlignOffset, sizeof inherited);
        if (inherited > alignment_mask) alignment_mask = inherited;
    }
    for (size_t i = 0; i < field_count; ++i) {
        const auto* field = witnesses_from_type_layout(field_layouts[i]);
        const size_t mask = field->flags & 0xff;
        size = static_cast<uint32_t>((size + mask) & ~mask);
        field_offsets[i] = size;
        size += static_cast<uint32_t>(field->size);
    }
    memcpy(metadata + kClassInstanceSizeOffset, &size, sizeof size);
    memcpy(metadata + kClassInstanceAlignOffset, &alignment_mask, sizeof alignment_mask);

    // The guest reads field offsets from globals that the ObjC runtime fills in through the ivar list.
    uintptr_t data;
    memcpy(&data, metadata + kClassDataOffset, sizeof data);
    auto* read_only = reinterpret_cast<char*>(data & ~static_cast<uintptr_t>(7));
    memcpy(read_only + kRoInstanceSizeOffset, &size, sizeof size);
    const char* ivars;
    memcpy(&ivars, read_only + kRoIvarsOffset, sizeof ivars);
    if (ivars != nullptr) {
        uint32_t entry_size, count;
        memcpy(&entry_size, ivars, sizeof entry_size);
        memcpy(&count, ivars + sizeof entry_size, sizeof count);
        const char* entries = ivars + 2 * sizeof(uint32_t);
        for (uint32_t i = 0; i < count && i < field_count; ++i) {
            uint64_t* offset_global;
            memcpy(&offset_global, entries + i * entry_size, sizeof offset_global);
            if (offset_global == nullptr) continue;
            // Offsets the compiler could prove are read-only constants, and must already agree.
            if (image_address_is_writable(reinterpret_cast<uintptr_t>(offset_global))) {
                *offset_global = field_offsets[i];
            } else if (*offset_global != field_offsets[i]) {
                engine_log("STOP: field %u is compiled at offset %llu but laid out at %zu", i, static_cast<unsigned long long>(*offset_global), field_offsets[i]);
                abort();
            }
        }
    }
    return nullptr;
}


constexpr size_t kAccessorPoolSize = 256 * 1024;
constexpr size_t kAccessorStubSize = 32;

// Accessors take generic arguments in registers, so each needs its arity; anything unlisted is non-generic.
size_t accessor_generic_arity(std::string_view symbol) {
    if (symbol == "$s7SwiftUI15ModifiedContentVMa") return 2;
    if (symbol == "$s7SwiftUI8BindableVMa" || symbol == "$s7SwiftUI9LazyStateVMa" || symbol == "$sSqMa" ||
        symbol == "$s7SwiftUI11EnvironmentV7ContentOMa" ||
        symbol == "$s7SwiftUI16TagValueTraitKeyVMa" || symbol == "$s7SwiftUI21_TraitWritingModifierVMa" ||
        symbol == "$s7SwiftUI24_TagTraitWritingModifierVMa") {
        return 1;
    }
    return 0;
}

extern "C" MetadataResponse rt_foreign_access(size_t, void* first, void* second, void* third, const char* symbol) {
    const std::lock_guard guard(g_types_lock);
    const std::string_view name(symbol);
    if (name == "$sSqMa") return {optional_of(static_cast<Metadata*>(first)), 0};
    const size_t arity = accessor_generic_arity(name);
    std::vector<void*> arguments;
    if (arity > 0) arguments.push_back(first);
    if (arity > 1) arguments.push_back(second);
    if (arity > 2) arguments.push_back(third);
    if (name == "$s7SwiftUI15ModifiedContentVMa") return {foreign_nominal("_$s7SwiftUI15ModifiedContentVMn", arguments), 0};
    std::string descriptor = std::string("_") + symbol;
    descriptor.replace(descriptor.size() - 2, 2, "Mn");
    return {foreign_nominal(descriptor, arguments), 0};
}

extern "C" void rt_foreign_access_entry();
__asm__(
    ".text\n"
    ".globl rt_foreign_access_entry\n"
    "rt_foreign_access_entry:\n"
    "  mov x4, x16\n"
    "  b rt_foreign_access\n");

void* make_accessor_stub(const std::string& symbol) {
    return make_code_stub(reinterpret_cast<void*>(rt_foreign_access_entry), strdup(symbol.c_str()));
}

}  // namespace

void* type_system_symbol(std::string_view name) {
    if (name == "swift_getTypeByMangledNameInContext2") return reinterpret_cast<void*>(rt_getTypeByMangledNameInContext2);
    if (name == "swift_getTypeByMangledNameInContextInMetadataState2") return reinterpret_cast<void*>(rt_getTypeByMangledNameInContextInMetadataState2);
    if (name == "swift_getObjCClassMetadata") return reinterpret_cast<void*>(rt_getObjCClassMetadata);
    if (name == "swift_getObjCClassFromMetadata") return reinterpret_cast<void*>(rt_getObjCClassFromMetadata);
    if (name == "swift_getSingletonMetadata") return reinterpret_cast<void*>(rt_getSingletonMetadata);
    if (name == "swift_initStructMetadata") return reinterpret_cast<void*>(rt_initStructMetadata);
    if (name == "swift_updateClassMetadata2") return reinterpret_cast<void*>(rt_updateClassMetadata2);
    if (name.size() > 3 && name.substr(0, 2) == "$s" && name.back() == 'N') {
        const std::lock_guard guard(g_types_lock);
        // Unlike nominal types, this symbol names the full metadata, one word before its address point.
        if (name == "$sytN") return reinterpret_cast<char*>(pod_type("yt", 0)) - sizeof(void*);
        if (name == "$sypN") return any_type();
        if (name == "$sSSN" || name == "$sSS8UTF8ViewVN" || name == "$ss11_StringGutsVN" || name == "$ss26DefaultStringInterpolationVN") return string_type();
        if (name == "$sSiN") return pod_type("Si", 8);
        if (name == "$sSbN") return named_type("Sb", kKindStruct, new Layout{LayoutKind::Pod, {}, nullptr}, 1, 1, 254);
        if (name == "$sSJN") return two_word_type("SJ", word_type(), bridge_word_type());
        if (name == "$sSsN" || name == "$sSs8UTF8ViewVN") return named_type("Ss", kKindStruct, new Layout{LayoutKind::Aggregate, {{word_type(), 0}, {word_type(), 8}, {word_type(), 16}, {bridge_word_type(), 24}}, nullptr}, 32, 8, kPointerExtraInhabitants);
        if (name == "$ss5NeverON") return pod_type(std::string(name), 0);
        if (name == "$s7SwiftUI22HierarchicalShapeStyleVN") return pod_type(std::string(name), 4);
        if (name == "$s10Foundation4DataVN" || name == "$s10Foundation4DataV15_RepresentationON") return two_word_type("Data", word_type(), bridge_word_type());
        std::string descriptor = "_" + std::string(name);
        descriptor.back() = 'M';
        descriptor += 'n';
        return foreign_nominal(descriptor, {});
    }
    if (name.size() > 4 && name.substr(0, 2) == "$s" && name.substr(name.size() - 2) == "Ma") return make_accessor_stub(std::string(name));
    return nullptr;
}

const char* foreign_type_name(const void* metadata) {
    const std::lock_guard guard(g_types_lock);
    const auto found = g_foreign_names.find(metadata);
    return found == g_foreign_names.end() ? nullptr : found->second;
}

void* resolve_mangled_type(const char* name, void* const* generics) {
    size_t length = 0;
    while (name[length] != 0) {
        const auto byte = static_cast<uint8_t>(name[length]);
        length += byte >= 1 && byte <= 0x17 ? 5 : 1;
    }
    return rt_getTypeByMangledNameInContext2(name, length, nullptr, generics);
}

void* storage_class_metadata(const char* symbol, void* const* arguments, size_t count) {
    const std::lock_guard guard(g_types_lock);
    return foreign_nominal(symbol, std::vector<void*>(arguments, arguments + count));
}

void* make_code_stub(void* entry, const void* data) {
    static char* pool = nullptr;
    static size_t used = 0;
    if (pool == nullptr) {
        pool = static_cast<char*>(mmap(nullptr, kAccessorPoolSize, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
        if (pool == MAP_FAILED) {
            pool = nullptr;
            return nullptr;
        }
    }
    if (used + kAccessorStubSize > kAccessorPoolSize) return nullptr;
    auto* stub = reinterpret_cast<uint32_t*>(pool + used);
    used += kAccessorStubSize;
    stub[0] = 0x58000090;  // ldr x16, #16
    stub[1] = 0x580000b1;  // ldr x17, #20
    stub[2] = 0xd61f0220;  // br x17
    stub[3] = 0xd503201f;  // nop, keeps the literals 8-byte aligned
    memcpy(stub + 4, &data, sizeof data);
    memcpy(stub + 6, &entry, sizeof entry);
    __builtin___clear_cache(reinterpret_cast<char*>(stub), reinterpret_cast<char*>(stub + 8));
    return stub;
}

void* lazy_state_metadata(void* value_type) {
    const std::lock_guard guard(g_types_lock);
    return lazy_state_of(static_cast<Metadata*>(value_type));
}
