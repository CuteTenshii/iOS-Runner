#include "type_system.h"

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
#include "swift_runtime.h"
#include "swift_abi.h"

extern "C" void (*storage_destroyer_for(const char* symbol))(void*);

namespace {

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

MetadataAllocation* allocation_of(const void* metadata) {
    return reinterpret_cast<MetadataAllocation*>(const_cast<char*>(static_cast<const char*>(metadata)) - 2 * sizeof(void*));
}
Layout* layout_of(const void* metadata) { return allocation_of(metadata)->layout; }

void retain(void* raw) {
    if (is_not_object(raw)) return;
    auto* object = static_cast<HeapObject*>(raw);
    if (!(object->references.load(std::memory_order_relaxed) & kImmortal)) object->references.fetch_add(1, std::memory_order_relaxed);
}

void release(void* raw) {
    if (is_not_object(raw)) return;
    auto* object = static_cast<HeapObject*>(raw);
    if (object->references.load(std::memory_order_relaxed) & kImmortal) return;
    if (object->references.fetch_sub(1, std::memory_order_acq_rel) == 1) {
        rt_call_destroyer(static_cast<void**>(object->metadata)[-2], object);
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
        case LayoutKind::Conditional: {
            Metadata* active = layout->fields[state_storage_tag(source, type)].type;
            memcpy(destination, source, witnesses_of(type)->size);
            witnesses_of(active)->initialize_with_copy(destination, source, active);
            break;
        }
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
            if (value->flags & kFlagNonInline)
                retain(*reinterpret_cast<void**>(from));
            else
                value->initialize_with_copy(destination, source, contained);
            break;
        }
    }
    return destination;
}

void witness_destroy(void* raw, void* type) {
    const auto* layout = layout_of(type);
    auto* value = static_cast<char*>(raw);
    switch (layout->kind) {
        case LayoutKind::Conditional: {
            Metadata* active = layout->fields[state_storage_tag(raw, type)].type;
            witnesses_of(active)->destroy(raw, active);
            break;
        }
        case LayoutKind::StateStorage:
            if (state_storage_tag(raw, type) == layout->value_case)
                witnesses_of(layout->payload)->destroy(raw, layout->payload);
            else
                release(*reinterpret_cast<void**>(value + layout->reference_offset));
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
            if (witnesses->flags & kFlagNonInline)
                release(*reinterpret_cast<void**>(raw));
            else
                witnesses->destroy(raw, contained);
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

// App-defined types have no layout record here, only their own value witnesses.
std::unordered_set<const void*>& runtime_metadata() {
    static std::unordered_set<const void*> all;
    return all;
}

uint32_t extra_inhabitant_get(const void* value, void* type) {
    if (runtime_metadata().count(type) == 0) {
        const auto* witnesses = witnesses_of(type);
        return witnesses->get_enum_tag_single_payload(value, witnesses->extra_inhabitants, type);
    }
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
        case LayoutKind::Conditional:
            return 0;
    }
    return 0;
}

void extra_inhabitant_store(void* value, uint32_t tag, void* type) {
    if (runtime_metadata().count(type) == 0) {
        const auto* witnesses = witnesses_of(type);
        witnesses->store_enum_tag_single_payload(value, tag, witnesses->extra_inhabitants, type);
        return;
    }
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
        case LayoutKind::Conditional:
            break;
    }
}

uint32_t tag_bytes_needed(size_t payload_size, uint32_t empty_cases) {
    if (payload_size >= 4) return 1;
    const uint64_t states = (empty_cases + (1ULL << (payload_size * 8)) - 1) >> (payload_size * 8);
    const uint64_t cases = states + 1;
    return cases <= 0x100 ? 1 : cases <= 0x10000 ? 2
                                                 : 4;
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
std::map<const void*, void*> g_array_elements;
std::map<const void*, void*> g_dictionary_values;

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
        if (layout_of(self)->kind == LayoutKind::StateStorage || layout_of(self)->kind == LayoutKind::Conditional) {
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
    auto* metadata = reinterpret_cast<Metadata*>(&block->kind);
    runtime_metadata().insert(metadata);
    return metadata;
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
    size_t size;  // 0 means the type comes from `argument`.
    size_t alignment;
    int argument;    // Index of a generic argument, or -1.
    bool reference;  // A retained word, rather than plain bytes.
};

Metadata* metadata_for_field(const FrozenField& field, const std::vector<void*>& arguments) {
    if (field.argument >= 0) return static_cast<Metadata*>(arguments[static_cast<size_t>(field.argument)]);
    if (field.reference) return native_type();
    std::string key = "pod" + std::to_string(field.size) + "/" + std::to_string(field.alignment);
    return named_type(key, kKindStruct, new Layout{LayoutKind::Pod, {}, nullptr}, field.size, field.alignment, 0);
}

Metadata* struct_from_fields(const std::string& key, const std::vector<FrozenField>& fields, const std::vector<void*>& arguments, size_t witness_words) {
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
    with_offsets.resize(arguments.size() + witness_words + offset_words, nullptr);
    auto* metadata = named_type(key, kKindStruct, layout, offset, alignment, inhabitants, with_offsets);
    // Metadata holds generic arguments, then their witness tables, then field offsets, which the app reads directly.
    auto* offsets = reinterpret_cast<uint32_t*>(&metadata->arguments[arguments.size() + witness_words]);
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

// _ConditionalContent: either view inline, then a case byte.
Metadata* conditional_of(const std::string& key, Metadata* first, Metadata* second, const std::vector<void*>& arguments) {
    const auto* a = witnesses_of(first);
    const auto* b = witnesses_of(second);
    const size_t payload = a->size > b->size ? a->size : b->size;
    const size_t mask = (a->flags & 0xff) > (b->flags & 0xff) ? (a->flags & 0xff) : (b->flags & 0xff);
    auto* layout = new Layout{LayoutKind::Conditional, {{first, 0}, {second, 0}}, nullptr};
    return named_type(key, kKindStruct, layout, payload + 1, mask + 1, 0, arguments);
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
        {"_$s7SwiftUI7BindingVMn", {kWord, kReference, kFirstArgument}},
        {"_$s7SwiftUI4TextVMn", {kWord, kReference, kWord, kReference}},
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
        // The value, then a byte marking it as tagged.
        {"_$s7SwiftUI24_TagTraitWritingModifierVMn", {kFirstArgument, {1, 1, -1, false}}},
        {"_$s7SwiftUI6HStackVMn", {kWord, kOptionalDouble, kFirstArgument}},
        {"_$s7SwiftUI6VStackVMn", {kWord, kOptionalDouble, kFirstArgument}},
        {"_$s7SwiftUI6ZStackVMn", {kAlignment, kFirstArgument}},
        {"_$s7SwiftUI13_VariadicViewO4TreeVMn", {kFirstArgument, kSecondArgument}},
        {"_$s7SwiftUI13_HStackLayoutVMn", {kWord, kOptionalDouble}},
        {"_$s7SwiftUI13_VStackLayoutVMn", {kWord, kOptionalDouble}},
        {"_$s7SwiftUI13_ZStackLayoutVMn", {kAlignment}},
        {"_$s7SwiftUI6SpacerVMn", {kOptionalDouble}},
        {"_$s7SwiftUI5StateVMn", {kFirstArgument, kReference}},
        {"_$ss16IndexingIteratorVMn", {kFirstArgument, kWord}},
        {"_$s7SwiftUI16SubscriptionViewVMn", {kSecondArgument, kFirstArgument, kClosure}},
        {"_$s7SwiftUI14ObservedObjectVMn", {kWord, kReference}},
        {"_$s7SwiftUI11StateObjectVMn", {kWord, kReference, kByte}},
        {"_$s7SwiftUI19_BackgroundModifierVMn", {kFirstArgument, kAlignment}},
        {"_$s7SwiftUI10_ShapeViewVMn", {kFirstArgument, kSecondArgument, {2, 1, -1, false}}},
        {"_$s7SwiftUI13_StrokedShapeVMn", {kFirstArgument, kWord, kWord, kWord, kReference, kWord}},
        {"_$s7SwiftUI13_TrimmedShapeVMn", {kFirstArgument, kWord, kWord}},
        {"_$s7SwiftUI16RoundedRectangleVMn", {kAnchor, kByte}},
        {"_$s7SwiftUI16RoundedRectangleV6_InsetVMn", {kAnchor, kByte, kWord}},
        {"_$s7SwiftUI22UnevenRoundedRectangleVMn", {kAnchor, kAnchor, kByte}},
        {"_$s7SwiftUI7CapsuleVMn", {kByte}},
        {"_$s7SwiftUI6CircleV6_InsetVMn", {kWord}},
        {"_$s7SwiftUI11_MaskEffectVMn", {kFirstArgument}},
        {"_$s7SwiftUI13_OffsetEffectVMn", {kAnchor}},
        {"_$s7SwiftUI14_OpacityEffectVMn", {kWord}},
        {"_$s7SwiftUI12_ScaleEffectVMn", {kAnchor, kAnchor}},
        {"_$s7SwiftUI15_HiddenModifierVMn", {}},
        {"_$s7SwiftUI16_FixedSizeLayoutVMn", {{2, 1, -1, false}}},
        {"_$s7SwiftUI16_FlexFrameLayoutVMn", {kOptionalDouble, kOptionalDouble, kOptionalDouble, kOptionalDouble, kOptionalDouble, kOptionalDouble, kAlignment}},
        {"_$s7SwiftUI21_OverlayStyleModifierVMn", {kFirstArgument, kAlignment}},
        {"_$s7SwiftUI24_ForegroundStyleModifierVMn", {kFirstArgument}},
        {"_$s7SwiftUI20_TransactionModifierVMn", {kClosure}},
        {"_$s7SwiftUI20_ValueActionModifierVMn", {kFirstArgument, kClosure}},
        {"_$s7SwiftUI18_TaskValueModifierVMn", {kFirstArgument, kClosure, kWord}},
        {"_$s7SwiftUI23_GeometryActionModifierVMn", {kFirstArgument, kClosure, kClosure}},
        {"_$s7SwiftUI30_SafeAreaRegionsIgnoringLayoutVMn", {kWord, kByte}},
        {"_$s7SwiftUI14GeometryReaderVMn", {kClosure}},
        {"_$s7SwiftUI15ForegroundStyleVMn", {}},
        {"_$s7SwiftUI21_ViewModifier_ContentVMn", {}},
        {"_$s7SwiftUI6CircleVMn", {}},
        {"_$s7SwiftUI9RectangleVMn", {}},
        {"_$s7SwiftUI9EmptyViewVMn", {}},
        {"_$s7SwiftUI12TupleContentVMn", {kFirstArgument}},
        {"_$s7SwiftUI9TupleViewVMn", {kFirstArgument}},
        {"_$s7SwiftUI5GroupVMn", {kFirstArgument}},
        {"_$s7SwiftUI13_TaskModifierVMn", {kWord, kReference, kByte}},
        {"_$s7SwiftUI22HierarchicalShapeStyleVMn", {{4, 4, -1, false}}},
        {"_$s7SwiftUI19TupleToolbarContentVMn", {kFirstArgument}},
        {"_$s7SwiftUI16_TupleTabContentVMn", {kFirstArgument, kSecondArgument}},
    };
    return table;
}

// How many witness tables a type's constraints add to its metadata.
size_t witness_word_count(const std::string& symbol) {
    static const std::unordered_map<std::string_view, size_t> counts = {
        {"_$s7SwiftUI6HStackVMn", 1},
        {"_$s7SwiftUI6VStackVMn", 1},
        {"_$s7SwiftUI6ZStackVMn", 1},
        {"_$s7SwiftUI13_VariadicViewO4TreeVMn", 1},
        {"_$s7SwiftUI24_BackgroundStyleModifierVMn", 1},
        {"_$s7SwiftUI16_OverlayModifierVMn", 1},
        {"_$s7SwiftUI11_ClipEffectVMn", 1},
        {"_$s7SwiftUI21_ContentShapeModifierVMn", 1},
        {"_$s7SwiftUI18_AnimationModifierVMn", 1},
        {"_$s7SwiftUI21_TraitWritingModifierVMn", 1},
        {"_$s7SwiftUI24_TagTraitWritingModifierVMn", 1},
        {"_$s7SwiftUI14ObservedObjectVMn", 1},
        {"_$s7SwiftUI11StateObjectVMn", 1},
        {"_$s7SwiftUI16SubscriptionViewVMn", 2},
        {"_$ss16IndexingIteratorVMn", 1},
        {"_$s7SwiftUI5GroupVMn", 1},
        {"_$s7SwiftUI19_BackgroundModifierVMn", 1},
        {"_$s7SwiftUI10_ShapeViewVMn", 2},
        {"_$s7SwiftUI13_StrokedShapeVMn", 1},
        {"_$s7SwiftUI13_TrimmedShapeVMn", 1},
        {"_$s7SwiftUI11_MaskEffectVMn", 1},
        {"_$s7SwiftUI21_OverlayStyleModifierVMn", 1},
        {"_$s7SwiftUI24_ForegroundStyleModifierVMn", 1},
        {"_$s7SwiftUI20_ValueActionModifierVMn", 1},
        {"_$s7SwiftUI18_TaskValueModifierVMn", 1},
        {"_$s7SwiftUI23_GeometryActionModifierVMn", 1},
        {"_$s7SwiftUI14GeometryReaderVMn", 1},
    };
    const auto found = counts.find(symbol);
    return found == counts.end() ? 0 : found->second;
}

Metadata* frozen_layout_type(const std::string& key, const std::string& symbol, const std::vector<void*>& arguments) {
    if (symbol == "_$ss11AnyHashableVMn") return named_type("AnyHashable", kKindStruct, new Layout{LayoutKind::OpaqueExistential, {}, nullptr}, 5 * kPointerSize, kPointerSize, kPointerExtraInhabitants);
    // Result has _ConditionalContent's shape: the larger of two payloads, then which one it holds.
    if ((symbol == "_$s7SwiftUI19_ConditionalContentVMn" || symbol == "_$s7SwiftUI19_ConditionalContentV7StorageOMn" || symbol == "_$ss6ResultOMn") && arguments.size() >= 2) {
        return conditional_of(key, static_cast<Metadata*>(arguments[0]), static_cast<Metadata*>(arguments[1]), arguments);
    }
    if (symbol == "_$s10Foundation4DateVMn") return pod_type("Date", 8);
    if (symbol == "_$s8Dispatch0A4TimeVMn") return pod_type("DispatchTime", 8);
    if (symbol == "_$sSS10FoundationE8EncodingVMn") return pod_type("String.Encoding", 8);
    // Holds a continuation, which isn't reference counted; the arguments stay for resume to find its value type.
    // A task group is a raw pointer to storage in its creator's frame.
    if (symbol == "_$sScGMn" || symbol == "_$sScgMn") return named_type(key, kKindStruct, new Layout{LayoutKind::Pod, {}, nullptr}, kPointerSize, kPointerSize, 0, arguments);
    if (symbol == "_$sScCMn") return named_type(key, kKindStruct, new Layout{LayoutKind::Pod, {}, nullptr}, kPointerSize, kPointerSize, 0, arguments);
    // Payload-free framework enums are plain tag words; a reference's layout would read small tags as Optional.none.
    if (symbol == "_$s7SwiftUI11ColorSchemeOMn" || symbol == "_$s7SwiftUI8EditModeOMn" || symbol == "_$s7SwiftUI22UserInterfaceSizeClassOMn" || symbol == "_$s7SwiftUI10ProminenceOMn") return pod_type(symbol, 8);
    if (symbol == "_$ss15ContinuousClockVMn") return pod_type("ContinuousClock", 0);
    if (symbol == "_$ss15ContinuousClockV7InstantVMn") return named_type("ContinuousClock.Instant", kKindStruct, new Layout{LayoutKind::Pod, {}, nullptr}, 16, 8, 0);
    if (symbol == "_$ss5Int32VMn") return pod_type("Int32", 4);
    if (symbol == "_$ss5UInt8VMn") return pod_type("UInt8", 1);
    if (symbol == "_$ss5NeverOMn") return pod_type("Never", 0);
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
    if (symbol == "_$s7SwiftUI21_TraitWritingModifierVMn") {
        // The modifier stores the trait's value rather than its key; ZIndex is this app's only such trait.
        return struct_from_fields(key, {{sizeof(double), sizeof(double), -1, false}}, arguments, witness_word_count(symbol));
    }
    return struct_from_fields(key, found->second, arguments, witness_word_count(symbol));
}

// Generic code copies class values through these. A class's prefix holds a destroyer where value types keep a
// layout, so they forward as a plain reference rather than reading the layout of the type they're given.
ValueWitnessTable* class_reference_witnesses() {
    static ValueWitnessTable* witnesses = [] {
        auto* table = new ValueWitnessTable(*witnesses_of(native_type()));
        table->initialize_buffer_with_copy = +[](void* destination, void* source, void*) { return witness_buffer_copy(destination, source, native_type()); };
        table->destroy = +[](void* value, void*) { witness_destroy(value, native_type()); };
        table->initialize_with_copy = +[](void* destination, void* source, void*) { return witness_copy(destination, source, native_type()); };
        table->assign_with_copy = +[](void* destination, void* source, void*) { return witness_assign_copy(destination, source, native_type()); };
        table->assign_with_take = +[](void* destination, void* source, void*) { return witness_assign_take(destination, source, native_type()); };
        table->get_enum_tag_single_payload = +[](const void* value, uint32_t count, void*) { return witness_get_tag(value, count, native_type()); };
        table->store_enum_tag_single_payload = +[](void* value, uint32_t tag, uint32_t count, void*) { witness_store_tag(value, tag, count, native_type()); };
        return table;
    }();
    return witnesses;
}

// Types whose stored layout the app's compiled code depends on; everything else from a framework is one reference.
Metadata* foreign_nominal(const std::string& symbol, const std::vector<void*>& arguments) {
    std::string key = symbol;
    for (const void* argument : arguments) key += pointer_key(argument);
    if (const auto found = g_named_types.find(key); found != g_named_types.end()) return found->second;
    // Framework classes are heap objects, so their metadata needs a destroyer where value types keep a layout.
    // CheckedContinuation is the one struct whose mangled name happens to end like a class.
    const bool is_class = symbol != "_$sScCMn" && symbol.size() > 3 && symbol.compare(symbol.size() - 3, 3, "CMn") == 0;
    if (storage_destroyer_for(symbol.c_str()) != nullptr || is_class) {
        // The guest reads a class's instance size at +0x30 before calling swift_allocObject on it.
        constexpr size_t kClassSizeWord = 8;
        auto* block = static_cast<void**>(calloc(std::max<size_t>(kClassSizeWord + 1, 4 + arguments.size() + 1), sizeof(void*)));
        if (is_class && arguments.size() <= kClassSizeWord - 4) {
            // The guest sizes array allocations from this: an array's header also holds its count and capacity.
            const uint32_t instance_size = symbol == "_$ss23_ContiguousArrayStorageCMn" ? 4 * sizeof(void*) : 2 * sizeof(void*);
            const uint16_t alignment_mask = sizeof(void*) - 1;
            memcpy(block + kClassSizeWord, &instance_size, sizeof instance_size);
            memcpy(reinterpret_cast<char*>(block + kClassSizeWord) + sizeof instance_size, &alignment_mask, sizeof alignment_mask);
        }
        auto destroyer = storage_destroyer_for(symbol.c_str());
        block[0] = reinterpret_cast<void*>(destroyer != nullptr ? destroyer : free);
        block[1] = class_reference_witnesses();
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

extern "C" void* rt_getTypeByMangledNameInContext2(const char* name, size_t length, const void* context, void* const* generics);

struct Node {
    enum class Tag { Type,
                     Nominal,
                     Protocol,
                     ListMarker,
                     FirstElement } tag = Tag::Type;
    Metadata* type = nullptr;
    NominalReference nominal;
};

using MetadataAccessor = void* (*)(uintptr_t, void*, void*, void*);

std::vector<void*> key_arguments_for(const char* descriptor, const std::vector<void*>& parameters);

Metadata* metadata_for_descriptor(const NominalReference& nominal, const std::vector<void*>& parameters) {
    if (nominal.foreign_symbol != nullptr) return foreign_nominal(nominal.foreign_symbol, parameters);
    const auto* descriptor = static_cast<const char*>(nominal.descriptor);
    if (descriptor == nullptr) {
        engine_log("STOP: a mangled name refers to a type with no descriptor, in \"%s\"", printable(g_current_name, g_current_length).c_str());
        abort();
    }
    const std::vector<void*> arguments = key_arguments_for(descriptor, parameters);
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

constexpr uint32_t kContextKindExtension = 1;
constexpr uint32_t kContextKindAnonymous = 2;
constexpr uint32_t kContextKindOpaque = 4;
constexpr uint32_t kContextKindClass = 16;
constexpr uint32_t kContextKindStruct = 17;
constexpr uint32_t kContextKindEnum = 18;
constexpr size_t kExtensionGenericOffset = 12;
constexpr size_t kAnonymousGenericOffset = 8;
constexpr size_t kOpaqueGenericOffset = 8;
constexpr size_t kClassGenericOffset = 44;
constexpr size_t kValueTypeGenericOffset = 28;
constexpr size_t kContextParentOffset = 4;

// From the outermost generic context in, the running parameter count at each generic depth.
std::vector<uint64_t> generic_depth_bases(const void* descriptor) {
    std::vector<uint64_t> bases;
    const auto* context = static_cast<const char*>(descriptor);
    while (context != nullptr && unresolved_symbol_at(reinterpret_cast<uintptr_t>(context)) == nullptr) {
        uint32_t flags;
        memcpy(&flags, context, sizeof flags);
        size_t header = 0;
        switch (flags & kDescriptorKindMask) {
            case kContextKindExtension:
                header = kExtensionGenericOffset;
                break;
            case kContextKindAnonymous:
                header = kAnonymousGenericOffset;
                break;
            case kContextKindOpaque:
                header = kOpaqueGenericOffset;
                break;
            case kContextKindClass:
                header = kClassGenericOffset;
                break;
            case kContextKindStruct:
            case kContextKindEnum:
                header = kValueTypeGenericOffset;
                break;
            default:
                break;
        }
        if (header != 0 && (flags & kDescriptorGeneric)) {
            uint16_t params;
            memcpy(&params, context + header, sizeof params);
            bases.insert(bases.begin(), params);
        }
        int32_t offset;
        memcpy(&offset, context + kContextParentOffset, sizeof offset);
        if (offset == 0) break;
        const char* field = context + kContextParentOffset + (offset & ~1);
        const char* parent = field;
        if (offset & 1) memcpy(&parent, field, sizeof parent);
        context = parent;
    }
    return bases;
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

    // Parameters are stored flat: these are the cumulative counts of each enclosing generic context.
    void use_context(const void* descriptor) { depth_bases_ = generic_depth_bases(descriptor); }
    void limit_generics(size_t count) { generic_limit_ = count; }

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
        if (const char* foreign = unresolved_symbol_at(reinterpret_cast<uintptr_t>(target)))
            node.nominal.foreign_symbol = foreign;
        else
            node.nominal.descriptor = target;
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
            case 'S':
                push_type(string_type());
                return;
            case 'i':
                push_type(pod_type("Si", 8));
                return;
            case 'u':
                push_type(pod_type("Su", 8));
                return;
            case 'b':
                push_type(named_type("Sb", kKindStruct, new Layout{LayoutKind::Pod, {}, nullptr}, 1, 1, 254));
                return;
            case 'd':
                push_type(pod_type("Sd", 8));
                return;
            case 'f':
                push_type(pod_type("Sf", 4));
                return;
            case 'J':
                push_type(two_word_type("SJ", word_type(), bridge_word_type()));
                return;
            case 's':
                push_type(named_type("Ss", kKindStruct, new Layout{LayoutKind::Aggregate, {{word_type(), 0}, {word_type(), 8}, {word_type(), 16}, {bridge_word_type(), 24}}, nullptr}, 32, 8, kPointerExtraInhabitants));
                return;
            case 'g': {
                Metadata* payload = as_type(pop());
                if (payload == nullptr) return;
                Node node;
                node.type = optional_of(payload);
                stack_.push_back(node);
                substitute(node);
                return;
            }
            case 'o': {
                // An imported Objective-C class named inline: only its reference layout matters here.
                const uint64_t length = read_natural();
                if (static_cast<uint64_t>(end_ - cursor_) < length + 1 || cursor_[length] != 'C') {
                    fail("unsupported imported type");
                    return;
                }
                cursor_ += length + 1;
                Node node;
                node.type = native_type();
                stack_.push_back(node);
                substitute(node);
                return;
            }
            case 'p':
            case 'P':
            case 'r':
            case 'R': {
                // Unsafe pointers: one plain word, whatever they point at.
                Node node;
                node.tag = Node::Tag::Nominal;
                node.nominal.foreign_symbol = "$Pointer";
                stack_.push_back(node);
                return;
            }
            case 'a':
            case 'D':
            case 'h': {
                Node node;
                node.tag = Node::Tag::Nominal;
                node.nominal.foreign_symbol = code == 'a' ? "$Array" : code == 'D' ? "$Dictionary"
                                                                                   : "$Set";
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
                if (concurrency == 'c') {
                    Node node;
                    node.tag = Node::Tag::Nominal;
                    node.nominal.foreign_symbol = "$CheckedContinuation";
                    stack_.push_back(node);
                    return;
                }
                // Task, TaskGroup, ThrowingTaskGroup, AsyncStream, AsyncThrowingStream and CancellationError.
                if (strchr("TGgSsE", concurrency) != nullptr) {
                    static const std::map<char, const char*> kSymbols = {{'T', "_$sScTMn"}, {'G', "_$sScGMn"}, {'g', "_$sScgMn"}, {'S', "_$sScSMn"}, {'s', "_$sScsMn"}, {'E', "_$sScEMn"}};
                    Node node;
                    node.tag = Node::Tag::Nominal;
                    node.nominal.foreign_symbol = kSymbols.at(concurrency);
                    stack_.push_back(node);
                    return;
                }
                if (concurrency == 'C') {
                    Node node;
                    node.tag = Node::Tag::Nominal;
                    node.nominal.foreign_symbol = "_$sScCMn";
                    stack_.push_back(node);
                    return;
                }
                if (concurrency == 'M') {
                    push_type(native_type());
                    return;
                }
                if (concurrency == 'P') {
                    push_type(pod_type("ScP", 1));
                    return;
                }
                fail(std::string("unknown concurrency type Sc") + concurrency);
                return;
            }
            case 'O':
                push_type(pod_type("ObjectIdentifier", kPointerSize));
                return;
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
        if (stack_.empty())
            fail("generic arguments without a list start");
        else
            pop();
        return arguments;
    }

    void bound_generic() {
        std::vector<void*> arguments = pop_generic_arguments();
        if (!failure_.empty()) return;
        for (const void* argument : arguments) {
            if (argument == nullptr) {
                fail("generic argument did not resolve");
                return;
            }
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
            node.type = strcmp(name, "$Pointer") == 0 ? pod_type("Bp", kPointerSize) : bridge_type(key, arguments);
            if (strcmp(name, "$Array") == 0 && !arguments.empty()) g_array_elements[node.type] = arguments[0];
            if (strcmp(name, "$Dictionary") == 0 && arguments.size() == 2) g_dictionary_values[node.type] = arguments[1];
        } else {
            node.type = metadata_for_descriptor(base.nominal, arguments);
        }
        stack_.push_back(node);
        substitute(node);
    }

    void tuple() {
        std::vector<Metadata*> elements;
        // A one-element pack has no marker, and must stop at the enclosing generic list.
        while (!stack_.empty() && stack_.back().tag != Node::Tag::ListMarker) {
            Node top = pop();
            if (top.tag == Node::Tag::FirstElement) {
                elements.insert(elements.begin(), as_type(pop()));
                break;
            }
            elements.insert(elements.begin(), as_type(top));
        }
        if (!failure_.empty()) return;
        for (const auto* element : elements) {
            if (element == nullptr) {
                fail("tuple element did not resolve");
                return;
            }
        }
        push_type(tuple_of(elements));
    }

    // A generic argument buffer also holds the witness tables that the requirements make key arguments.
    void append_key_witnesses(const char* requirements, uint16_t count, std::vector<void*>& arguments) {
        const std::vector<void*> types = arguments;
        for (uint16_t i = 0; i < count; ++i) {
            const char* requirement = requirements + i * kGenericRequirementSize;
            uint32_t flags;
            memcpy(&flags, requirement, sizeof flags);
            if ((flags & kRequirementKindMask) != kRequirementKindProtocol || !(flags & kRequirementHasKeyArgument)) continue;
            int32_t param_offset, protocol_offset;
            memcpy(&param_offset, requirement + 4, sizeof param_offset);
            memcpy(&protocol_offset, requirement + 8, sizeof protocol_offset);
            // The low bit of the offset marks a mangled name.
            const char* param_name = requirement + 4 + (param_offset & ~1);
            const size_t length = mangled_name_length(param_name);
            MangledName parameter(param_name, length, types.data());
            parameter.use_context(context_for_requirements_);
            parameter.limit_generics(types.size());
            // A subject fixed by a same-type constraint is not passed, and its witness is only forwarded.
            Metadata* type = parameter.parse();
            const char* protocol_field = requirement + 8 + (protocol_offset & ~1);
            const void* protocol = protocol_field;
            if (protocol_offset & 1) memcpy(&protocol, protocol_field, sizeof protocol);
            arguments.push_back(type != nullptr ? swift_runtime_find_conformance(type, protocol) : swift_runtime_placeholder_conformance(protocol));
        }
    }

    void opaque_type() {
        if (cursor_ >= end_) {
            fail("truncated Q operator");
            return;
        }
        const char variety = *cursor_++;
        // A pack gathers its elements from the stack like a tuple, and is carried as one.
        if (variety == 'P') {
            tuple();
            return;
        }
        if (variety != 'o') {
            fail(std::string("unsupported Q operator ") + variety);
            return;
        }
        const uint64_t ordinal = cursor_ < end_ && *cursor_ == '_' ? 0 : read_natural() + 1;
        if (cursor_ >= end_ || *cursor_++ != '_') {
            fail("bad opaque type ordinal");
            return;
        }
        std::vector<void*> arguments = pop_generic_arguments();
        Node base = pop();
        if (base.tag != Node::Tag::Nominal) {
            fail("opaque type without a descriptor");
            return;
        }

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
                const char* requirement_list = trailing + kGenericHeaderSize + ((params + 3u) & ~3u);
                context_for_requirements_ = descriptor;
                append_key_witnesses(requirement_list, requirements, arguments);
                trailing = requirement_list + requirements * kGenericRequirementSize;
            }
            int32_t offset;
            memcpy(&offset, trailing + ordinal * sizeof(int32_t), sizeof offset);
            const char* underlying = trailing + ordinal * sizeof(int32_t) + offset;
            const size_t length = mangled_name_length(underlying);
            MangledName nested(underlying, length, arguments.data());
            nested.use_context(descriptor);
            node.type = nested.parse();
            if (node.type == nullptr) {
                fail("opaque underlying type: " + nested.failure());
                return;
            }
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
        // A tuple element's label: no effect on layout, but as an identifier it takes a substitution slot.
        if (code >= '1' && code <= '9') {
            --cursor_;
            cursor_ += read_natural();
            substitute(Node{});
            return;
        }
        switch (code) {
            case 'S':
                if (cursor_ >= end_) {
                    fail("truncated standard type");
                    return;
                }
                // `S2S` repeats the standard type that follows the count.
                if (*cursor_ >= '2' && *cursor_ <= '9') {
                    const uint64_t count = read_natural();
                    if (cursor_ >= end_) {
                        fail("truncated standard type");
                        return;
                    }
                    const char repeated = *cursor_++;
                    for (uint64_t i = 0; i < count; ++i) {
                        standard(repeated);
                        if (!failure_.empty()) return;
                    }
                    return;
                }
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
            // inout: a parameter convention, and function types here are only ever laid out as closures.
            case 'z':
                return;
            default:
                fail(std::string("unsupported operator '") + code + "'");
                return;
        }
    }

    void builtin() {
        if (cursor_ >= end_) {
            fail("truncated builtin");
            return;
        }
        const char code = *cursor_++;
        switch (code) {
            case 'D':
                push_type(pod_type("BD", 96));
                return;
            case 'o':
            case 'O':
                push_type(native_type());
                return;
            case 'b':
                push_type(bridge_word_type());
                return;
            case 'w':
                push_type(word_type());
                return;
            case 'p':
                push_type(word_type());
                return;
            default:
                fail(std::string("unknown builtin B") + code);
        }
    }

    void generic(uint64_t depth, uint64_t index) {
        if (generics_ == nullptr) {
            fail("generic parameter without generic arguments");
            return;
        }
        uint64_t base = 0;
        if (depth != 0) {
            if (depth > depth_bases_.size()) {
                fail("generic parameter deeper than its context");
                return;
            }
            base = depth_bases_[depth - 1];
        }
        if (generic_limit_ != kNoGenericLimit && base + index >= generic_limit_) {
            fail("generic parameter is not passed");
            return;
        }
        push_type(static_cast<Metadata*>(generics_[base + index]));
    }

    void generic_parameter() {
        if (cursor_ < end_ && *cursor_ == 'd') {
            ++cursor_;
            const uint64_t depth = read_natural() + 1;
            if (cursor_ >= end_ || *cursor_++ != '_') {
                fail("bad generic depth");
                return;
            }
            const uint64_t index = cursor_ < end_ && *cursor_ == '_' ? 0 : read_natural() + 1;
            if (cursor_ >= end_ || *cursor_++ != '_') {
                fail("bad generic index");
                return;
            }
            generic(depth, index);
            return;
        }
        const uint64_t index = cursor_ < end_ && *cursor_ == '_' ? 0 : read_natural() + 1;
        if (cursor_ >= end_ || *cursor_++ != '_') {
            fail("bad generic parameter");
            return;
        }
        generic(0, index + 1);
    }

    void substitution() {
        for (;;) {
            bool has_count = false;
            uint64_t count = 0;
            if (cursor_ < end_ && *cursor_ >= '0' && *cursor_ <= '9') {
                count = read_natural();
                has_count = true;
            }
            if (cursor_ >= end_) {
                fail("truncated substitution");
                return;
            }
            const char letter = *cursor_++;
            if (letter == '_') {
                // Indexes past the alphabet: `A_` is 26, `A<n>_` is n + 27.
                const uint64_t index = has_count ? count + 27 : 26;
                if (index >= substitutions_.size()) {
                    fail("substitution " + std::to_string(index) + " out of range, have " + std::to_string(substitutions_.size()));
                    return;
                }
                stack_.push_back(substitutions_[index]);
                return;
            }
            const bool last = letter >= 'A' && letter <= 'Z';
            if (!last && !(letter >= 'a' && letter <= 'z')) {
                fail("malformed substitution");
                return;
            }
            const uint64_t index = static_cast<uint64_t>(last ? letter - 'A' : letter - 'a');
            if (index >= substitutions_.size()) {
                fail("substitution " + std::to_string(index) + " out of range, have " + std::to_string(substitutions_.size()));
                return;
            }
            for (uint64_t i = 0; i < (has_count ? count : 1); ++i) stack_.push_back(substitutions_[index]);
            if (last) return;
        }
    }

    const char* cursor_;
    const char* end_;
    void* const* generics_;
    std::vector<Node> stack_;
    std::vector<Node> substitutions_;
    std::string failure_;
    std::vector<uint64_t> depth_bases_;
    const void* context_for_requirements_ = nullptr;
    static constexpr uint64_t kNoGenericLimit = UINT64_MAX;
    uint64_t generic_limit_ = kNoGenericLimit;
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

extern "C" void* rt_getTypeByMangledNameInContext2(const char* name, size_t length, const void* context, void* const* generics) {
    const std::lock_guard guard(g_types_lock);
    const char* outer_name = g_current_name;
    const size_t outer_length = g_current_length;
    g_current_name = name;
    g_current_length = length;
    MangledName parser(name, length, generics);
    if (context != nullptr) parser.use_context(context);
    Metadata* result = parser.parse();
    g_current_name = outer_name;
    g_current_length = outer_length;
    if (result == nullptr) {
        engine_log("STOP: cannot resolve type: %s, in \"%s\"", parser.failure().c_str(), printable(name, length).c_str());
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

// Generic metadata instantiation for the app's own generic structs and enums, per the Swift ABI.
constexpr size_t kStructGenericHeaderOffset = 28;
constexpr size_t kEnumGenericHeaderOffset = 28;
constexpr size_t kClassGenericHeaderOffset = 44;
constexpr size_t kPatternOffsetInHeader = 4;
constexpr size_t kNumKeyArgumentsOffset = 12;  // After the two relative pointers and NumParams, NumRequirements.
constexpr uint32_t kPatternHasExtraData = 1;
constexpr unsigned kPatternValueKindShift = 21;
constexpr uint32_t kPatternValueKindMask = 0x7ff;
constexpr size_t kPatternFlagsOffset = 8;
constexpr size_t kValuePatternWitnessesOffset = 12;
constexpr size_t kValuePatternExtraOffset = 16;
constexpr size_t kValueMetadataHeaderBytes = 3 * kPointerSize;

const char* relative_address(const char* field) {
    int32_t offset;
    memcpy(&offset, field, sizeof offset);
    return offset == 0 ? nullptr : field + offset;
}

size_t generic_header_offset(const char* descriptor) {
    uint32_t flags;
    memcpy(&flags, descriptor, sizeof flags);
    switch (flags & kDescriptorKindMask) {
        case kContextKindClass:
            return kClassGenericHeaderOffset;
        case kContextKindStruct:
            return kStructGenericHeaderOffset;
        default:
            return kEnumGenericHeaderOffset;
    }
}

uint16_t key_argument_count(const char* descriptor) {
    uint16_t count;
    memcpy(&count, descriptor + generic_header_offset(descriptor) + kNumKeyArgumentsOffset, sizeof count);
    return count;
}

constexpr size_t kGenericHeaderBytes = 16;
constexpr size_t kNumParamsOffset = 8;
constexpr size_t kNumRequirementsOffset = 10;
constexpr size_t kRequirementBytes = 12;

// A generic type's accessor takes its parameters, then one witness table per protocol requirement.
std::vector<void*> key_arguments_for(const char* descriptor, const std::vector<void*>& parameters) {
    std::vector<void*> arguments = parameters;
    uint32_t flags;
    memcpy(&flags, descriptor, sizeof flags);
    if (!(flags & kDescriptorGeneric)) return arguments;
    const char* header = descriptor + generic_header_offset(descriptor);
    uint16_t param_count, requirement_count;
    memcpy(&param_count, header + kNumParamsOffset, sizeof param_count);
    memcpy(&requirement_count, header + kNumRequirementsOffset, sizeof requirement_count);
    const char* requirements = header + kGenericHeaderBytes + ((param_count + 3u) & ~3u);
    for (uint16_t i = 0; i < requirement_count; ++i) {
        const char* requirement = requirements + i * kRequirementBytes;
        uint32_t requirement_flags;
        memcpy(&requirement_flags, requirement, sizeof requirement_flags);
        if (!(requirement_flags & kRequirementHasKeyArgument) || (requirement_flags & kRequirementKindMask) != kRequirementKindProtocol) continue;
        const char* subject_name = relative_address(requirement + sizeof(int32_t));
        void* subject = rt_getTypeByMangledNameInContext2(subject_name, strlen(subject_name), descriptor, parameters.data());
        int32_t offset;
        memcpy(&offset, requirement + 2 * sizeof(int32_t), sizeof offset);
        // The low bit marks an indirect reference; a set second bit would mean an ObjC protocol, which has no table.
        const char* protocol_field = requirement + 2 * sizeof(int32_t) + (offset & ~3);
        const void* protocol = offset & 1 ? *reinterpret_cast<const void* const*>(protocol_field) : protocol_field;
        arguments.push_back(swift_runtime_find_conformance(subject, protocol));
    }
    return arguments;
}

extern "C" void* rt_allocateGenericValueMetadata(const char* descriptor, void* const* arguments, const char* pattern, size_t extra_size) {
    auto* block = static_cast<void**>(calloc(1, kValueMetadataHeaderBytes + extra_size));
    void** metadata = block + 1;
    uint32_t flags;
    memcpy(&flags, pattern + kPatternFlagsOffset, sizeof flags);
    metadata[-1] = const_cast<char*>(relative_address(pattern + kValuePatternWitnessesOffset));
    metadata[0] = reinterpret_cast<void*>(static_cast<uintptr_t>((flags >> kPatternValueKindShift) & kPatternValueKindMask));
    metadata[1] = const_cast<char*>(descriptor);
    memcpy(metadata + 2, arguments, key_argument_count(descriptor) * sizeof(void*));
    if (flags & kPatternHasExtraData) {
        const char* partial = pattern + kValuePatternExtraOffset;
        uint16_t offset_in_words, size_in_words;
        memcpy(&offset_in_words, partial + 4, sizeof offset_in_words);
        memcpy(&size_in_words, partial + 6, sizeof size_in_words);
        // The pattern's offset counts from after the kind and descriptor words.
        memcpy(metadata + 2 + offset_in_words, relative_address(partial), size_in_words * sizeof(void*));
    }
    return metadata;
}

struct CachedGeneric {
    const char* descriptor;
    std::vector<void*> arguments;
    bool operator<(const CachedGeneric& other) const { return descriptor != other.descriptor ? descriptor < other.descriptor : arguments < other.arguments; }
};

extern "C" MetadataResponse rt_getGenericMetadata(size_t, void* const* arguments, const char* descriptor) {
    static std::map<CachedGeneric, void*> cache;
    const uint16_t count = key_argument_count(descriptor);
    CachedGeneric key{descriptor, std::vector<void*>(arguments, arguments + count)};
    {
        const std::lock_guard guard(g_types_lock);
        if (const auto found = cache.find(key); found != cache.end()) return {static_cast<Metadata*>(found->second), 0};
    }
    const char* pattern = relative_address(descriptor + generic_header_offset(descriptor) + kPatternOffsetInHeader);
    using Instantiate = void* (*)(const void*, void* const*, const void*);
    using Complete = MetadataResponse (*)(void*, void*, const void*);
    auto instantiate = reinterpret_cast<Instantiate>(const_cast<char*>(relative_address(pattern)));
    void* metadata = instantiate(descriptor, arguments, pattern);
    {
        // Recursive types look themselves up while completing.
        const std::lock_guard guard(g_types_lock);
        cache[key] = metadata;
    }
    if (const char* completion = relative_address(pattern + sizeof(int32_t))) {
        void* context[4] = {};
        reinterpret_cast<Complete>(const_cast<char*>(completion))(metadata, context, pattern);
    }
    return {static_cast<Metadata*>(metadata), 0};
}

// Arguments arrive complete, witness tables included, so unlike the mangled form nothing needs looking up.
extern "C" MetadataResponse rt_getOpaqueTypeMetadata(size_t, void* const* arguments, const char* descriptor, size_t ordinal) {
    if (const char* symbol = unresolved_symbol_at(reinterpret_cast<uintptr_t>(descriptor))) {
        const std::lock_guard guard(g_types_lock);
        return {foreign_nominal(symbol, {}), 0};
    }
    uint32_t flags;
    memcpy(&flags, descriptor, sizeof flags);
    const char* trailing = descriptor + 2 * sizeof(int32_t);
    if (flags & kDescriptorGeneric) {
        uint16_t params, requirements;
        memcpy(&params, trailing, sizeof params);
        memcpy(&requirements, trailing + 2, sizeof requirements);
        trailing += kGenericHeaderSize + ((params + 3u) & ~3u) + requirements * kGenericRequirementSize;
    }
    const char* underlying = relative_address(trailing + ordinal * sizeof(int32_t));
    const size_t length = mangled_name_length(underlying);
    return {static_cast<Metadata*>(rt_getTypeByMangledNameInContext2(underlying, length, descriptor, arguments)), 0};
}

// Types the app builds at runtime instead of naming them.
constexpr size_t kOpaqueExistentialWords = 4;
constexpr size_t kClassConstraintClass = 0;

extern "C" Metadata* rt_getExistentialTypeMetadata(size_t class_constraint, const void*, size_t protocol_count, const void* const*) {
    const std::lock_guard guard(g_types_lock);
    if (class_constraint == kClassConstraintClass) {
        if (protocol_count == 0) return native_type();
        const std::string key = "ClassExistential" + std::to_string(protocol_count);
        auto* layout = new Layout{LayoutKind::Aggregate, {{native_type(), 0}}, nullptr};
        return named_type(key, kKindStruct, layout, (1 + protocol_count) * kPointerSize, kPointerSize, kPointerExtraInhabitants);
    }
    if (protocol_count <= 1) return any_type();
    const std::string key = "OpaqueExistential" + std::to_string(protocol_count);
    return named_type(key, kKindStruct, new Layout{LayoutKind::OpaqueExistential, {}, nullptr}, (kOpaqueExistentialWords + protocol_count) * kPointerSize, kPointerSize, kPointerExtraInhabitants);
}

extern "C" Metadata* rt_getFunctionTypeMetadata() {
    const std::lock_guard guard(g_types_lock);
    return thick_function_type();
}

extern "C" Metadata* rt_getMetatypeMetadata(void*) {
    const std::lock_guard guard(g_types_lock);
    return pod_type("metatype", kPointerSize);
}

extern "C" MetadataResponse rt_getTupleTypeMetadata2(size_t, Metadata* first, Metadata* second) {
    const std::lock_guard guard(g_types_lock);
    return {tuple_of({first, second}), 0};
}

extern "C" MetadataResponse rt_getTupleTypeMetadata3(size_t, Metadata* first, Metadata* second, Metadata* third) {
    const std::lock_guard guard(g_types_lock);
    return {tuple_of({first, second, third}), 0};
}

constexpr size_t kTupleCountMask = 0xffff;

extern "C" MetadataResponse rt_getTupleTypeMetadata(size_t, size_t flags, Metadata* const* elements) {
    const std::lock_guard guard(g_types_lock);
    return {tuple_of(std::vector<Metadata*>(elements, elements + (flags & kTupleCountMask))), 0};
}

struct TypeLayoutValue {
    size_t size;
    size_t stride;
    uint32_t flags;
    uint32_t extra_inhabitants;
};

// Lays out tuple fields for code that has only the fields' layouts, and returns their offsets.
void tuple_layout(TypeLayoutValue* out, size_t* offsets, const TypeLayoutValue* const* fields, size_t count) {
    size_t offset = 0;
    size_t mask = 0;
    uint32_t flags = 0;
    for (size_t i = 0; i < count; ++i) {
        const size_t field_mask = fields[i]->flags & 0xff;
        offset = (offset + field_mask) & ~field_mask;
        if (offsets != nullptr) offsets[i] = offset;
        offset += fields[i]->size;
        if (field_mask > mask) mask = field_mask;
        flags |= fields[i]->flags & kFlagNonPod;
    }
    *out = {offset, offset == 0 ? 1 : (offset + mask) & ~mask, static_cast<uint32_t>(mask) | flags, 0};
}

extern "C" size_t rt_getTupleTypeLayout2(TypeLayoutValue* out, const TypeLayoutValue* first, const TypeLayoutValue* second) {
    const TypeLayoutValue* fields[] = {first, second};
    size_t offsets[2];
    tuple_layout(out, offsets, fields, 2);
    return offsets[1];
}

struct TupleOffsets {
    size_t second;
    size_t third;
};

extern "C" TupleOffsets rt_getTupleTypeLayout3(TypeLayoutValue* out, const TypeLayoutValue* first, const TypeLayoutValue* second, const TypeLayoutValue* third) {
    const TypeLayoutValue* fields[] = {first, second, third};
    size_t offsets[3];
    tuple_layout(out, offsets, fields, 3);
    return {offsets[1], offsets[2]};
}

extern "C" void rt_getTupleTypeLayout(TypeLayoutValue* out, uint32_t* offsets, size_t, size_t count, const TypeLayoutValue* const* fields) {
    std::vector<size_t> computed(count);
    tuple_layout(out, computed.data(), fields, count);
    if (offsets != nullptr) {
        for (size_t i = 0; i < count; ++i) offsets[i] = static_cast<uint32_t>(computed[i]);
    }
}

extern "C" MetadataResponse rt_checkMetadataState(size_t, void* metadata) { return {static_cast<Metadata*>(metadata), 0}; }

// How many bytes after the payload an enum needs to tell `tags` cases apart.
size_t enum_tag_bytes(size_t payload_size, uint64_t empty_cases, uint64_t payload_cases) {
    uint64_t tags = payload_cases;
    if (empty_cases > 0) {
        if (payload_size >= 4) {
            tags += 1;
        } else {
            const uint64_t per_tag = 1ULL << (payload_size * 8);
            tags += (empty_cases + per_tag - 1) / per_tag;
        }
    }
    if (tags <= 1) return 0;
    if (tags <= 0x100) return 1;
    if (tags <= 0x10000) return 2;
    return 4;
}

void install_enum_witnesses(void** metadata, const ValueWitnessTable* base, size_t size, size_t alignment_mask, uint32_t pod_flags, uint32_t inhabitants) {
    auto* updated = new ValueWitnessTable(*base);
    updated->size = size;
    updated->stride = size == 0 ? 1 : (size + alignment_mask) & ~alignment_mask;
    updated->flags = static_cast<uint32_t>(alignment_mask) | (pod_flags & kFlagNonPod);
    if (size > kInlineBufferSize || alignment_mask >= kPointerSize) updated->flags |= kFlagNonInline;
    updated->extra_inhabitants = inhabitants;
    metadata[-1] = updated;
}

extern "C" void rt_initEnumMetadataSingleCase(void** metadata, size_t, const void* payload_layout) {
    const auto* payload = witnesses_from_type_layout(payload_layout);
    install_enum_witnesses(metadata, static_cast<ValueWitnessTable*>(metadata[-1]), payload->size, payload->flags & 0xff, payload->flags, payload->extra_inhabitants);
}

extern "C" void rt_initEnumMetadataSinglePayload(void** metadata, size_t, const void* payload_layout, uint32_t empty_cases) {
    const auto* payload = witnesses_from_type_layout(payload_layout);
    const uint32_t available = payload->extra_inhabitants;
    if (empty_cases <= available) {
        install_enum_witnesses(metadata, static_cast<ValueWitnessTable*>(metadata[-1]), payload->size, payload->flags & 0xff, payload->flags, available - empty_cases);
        return;
    }
    const size_t tag = enum_tag_bytes(payload->size, empty_cases - available, 1);
    install_enum_witnesses(metadata, static_cast<ValueWitnessTable*>(metadata[-1]), payload->size + tag, payload->flags & 0xff, payload->flags, 0);
}

constexpr size_t kEnumPayloadCasesOffset = 20;
constexpr size_t kEnumEmptyCasesOffset = 24;
constexpr unsigned kEnumPayloadSizeOffsetShift = 24;
constexpr uint32_t kEnumPayloadCasesMask = 0xffffff;

extern "C" void rt_initEnumMetadataMultiPayload(void** metadata, size_t, size_t payload_count, const void* const* payload_layouts) {
    size_t largest = 0;
    size_t alignment_mask = 0;
    uint32_t flags = 0;
    for (size_t i = 0; i < payload_count; ++i) {
        const auto* payload = witnesses_from_type_layout(payload_layouts[i]);
        if (payload->size > largest) largest = payload->size;
        if ((payload->flags & 0xff) > alignment_mask) alignment_mask = payload->flags & 0xff;
        flags |= payload->flags;
    }
    const auto* descriptor = static_cast<const char*>(metadata[1]);
    uint32_t cases_and_offset, empty_cases;
    memcpy(&cases_and_offset, descriptor + kEnumPayloadCasesOffset, sizeof cases_and_offset);
    memcpy(&empty_cases, descriptor + kEnumEmptyCasesOffset, sizeof empty_cases);
    const size_t tag = enum_tag_bytes(largest, empty_cases, cases_and_offset & kEnumPayloadCasesMask);
    if (const uint32_t offset = cases_and_offset >> kEnumPayloadSizeOffsetShift) metadata[offset] = reinterpret_cast<void*>(largest);
    install_enum_witnesses(metadata, static_cast<ValueWitnessTable*>(metadata[-1]), largest + tag, alignment_mask, flags, 0);
}
constexpr size_t kClassInstanceSizeOffset = 48;
constexpr size_t kClassInstanceAlignOffset = 52;
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

// Generic class instantiation, per the Swift ABI: fixed metadata words, then immediate members, then extra data.
constexpr size_t kClassNegativeSizeOffset = 24;
constexpr size_t kClassPositiveSizeOffset = 28;
constexpr size_t kClassImmediateMembersOffset = 32;
constexpr size_t kClassSuperclassTypeOffset = 20;
constexpr size_t kPatternDestroyOffset = 12;
constexpr size_t kPatternIvarDestroyerOffset = 16;
constexpr size_t kPatternClassFlagsOffset = 20;
constexpr size_t kPatternRoOffset = 24;
constexpr size_t kPatternMetaclassOffset = 26;
constexpr size_t kPatternMetaclassRoOffset = 28;
constexpr size_t kClassPatternTrailerOffset = 32;
constexpr uint32_t kPatternHasImmediateMembers = 1u << 31;
constexpr size_t kPartialPatternBytes = 8;
constexpr size_t kClassFixedWords = 10;
constexpr size_t kClassFlagsWord = 5;
constexpr size_t kClassDataWord = 4;
constexpr size_t kClassDescriptionWord = 8;
constexpr size_t kClassIvarDestroyerWord = 9;
constexpr size_t kSwiftClassDataBits = 3;

void copy_partial_pattern(void** target, const char* partial) {
    uint16_t offset, size;
    memcpy(&offset, partial + 4, sizeof offset);
    memcpy(&size, partial + 6, sizeof size);
    memcpy(target + offset, relative_address(partial), size * sizeof(void*));
}

uint32_t read_u32_at(const char* at) {
    uint32_t value;
    memcpy(&value, at, sizeof value);
    return value;
}

extern "C" void* rt_allocateGenericClassMetadata(const char* descriptor, void* const* arguments, const char* pattern) {
    const uint32_t negative = read_u32_at(descriptor + kClassNegativeSizeOffset);
    const uint32_t positive = read_u32_at(descriptor + kClassPositiveSizeOffset);
    const uint32_t immediate = read_u32_at(descriptor + kClassImmediateMembersOffset);
    const uint32_t flags = read_u32_at(pattern + kPatternFlagsOffset);
    const char* trailer = pattern + kClassPatternTrailerOffset;
    const char* immediate_pattern = nullptr;
    if (flags & kPatternHasImmediateMembers) {
        immediate_pattern = trailer;
        trailer += kPartialPatternBytes;
    }
    const char* extra_pattern = flags & kPatternHasExtraData ? trailer : nullptr;
    size_t extra_words = 0;
    if (extra_pattern != nullptr) {
        uint16_t offset, size;
        memcpy(&offset, extra_pattern + 4, sizeof offset);
        memcpy(&size, extra_pattern + 6, sizeof size);
        extra_words = offset + size;
    }
    auto* block = static_cast<void**>(calloc(negative + positive + extra_words, sizeof(void*)));
    void** metadata = block + negative;
    void** extra = metadata + positive;
    metadata[-2] = const_cast<char*>(relative_address(pattern + kPatternDestroyOffset));
    metadata[-1] = const_cast<void*>(static_cast<const void*>(witnesses_of(native_type())));
    metadata[kClassDescriptionWord] = const_cast<char*>(descriptor);
    metadata[kClassIvarDestroyerWord] = const_cast<char*>(relative_address(pattern + kPatternIvarDestroyerOffset));
    memcpy(&metadata[kClassFlagsWord], pattern + kPatternClassFlagsOffset, sizeof(uint32_t));
    const uint32_t class_size = (negative + positive) * sizeof(void*);
    const uint32_t address_point = negative * sizeof(void*);
    memcpy(static_cast<void*>(metadata + 7), &class_size, sizeof class_size);
    memcpy(reinterpret_cast<char*>(metadata + 7) + sizeof class_size, &address_point, sizeof address_point);
    void** members = metadata + positive - immediate;
    memcpy(members, arguments, key_argument_count(descriptor) * sizeof(void*));
    if (immediate_pattern != nullptr) copy_partial_pattern(members, immediate_pattern);
    if (extra_pattern != nullptr) {
        copy_partial_pattern(extra, extra_pattern);
        uint16_t ro, meta, meta_ro;
        memcpy(&ro, pattern + kPatternRoOffset, sizeof ro);
        memcpy(&meta, pattern + kPatternMetaclassOffset, sizeof meta);
        memcpy(&meta_ro, pattern + kPatternMetaclassRoOffset, sizeof meta_ro);
        // These three offsets are in words from the start of the extra data.
        metadata[kClassDataWord] = reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(extra + ro) | kSwiftClassDataBits);
        auto** metaclass = reinterpret_cast<void**>(extra + meta);
        metaclass[kClassDataWord] = reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(extra + meta_ro) | kSwiftClassDataBits);
        metadata[0] = metaclass;
    }
    return metadata;
}

constexpr uint32_t kClassHasVTable = 0x80000000;
constexpr uint32_t kClassHasOverrides = 0x40000000;
constexpr size_t kVTableHeaderBytes = 8;
constexpr size_t kMethodDescriptorBytes = 8;
constexpr size_t kMethodImplementationOffset = 4;
constexpr size_t kOverrideEntryBytes = 12;
constexpr size_t kResilientSuperclassBytes = 4;
constexpr uint32_t kMetadataInitializationShift = 16;
constexpr uint32_t kMetadataInitializationMask = 3;
constexpr uint32_t kSingletonMetadataInitialization = 1;
constexpr size_t kSingletonInitializationBytes = 12;

const char* indirect_relative(const char* field) {
    int32_t offset;
    memcpy(&offset, field, sizeof offset);
    if (offset == 0) return nullptr;
    const char* target = field + (offset & ~1);
    return offset & 1 ? *reinterpret_cast<const char* const*>(target) : target;
}

// Where a class descriptor's trailing vtable starts: after its generic requirements.
const char* class_vtable_header(const char* descriptor) {
    const char* cursor = descriptor + kClassGenericHeaderOffset;
    if (read_u32_at(descriptor) & kDescriptorGeneric) {
        uint16_t params, requirements;
        memcpy(&params, cursor + kNumParamsOffset, sizeof params);
        memcpy(&requirements, cursor + kNumRequirementsOffset, sizeof requirements);
        cursor += kGenericHeaderBytes + ((params + 3u) & ~3u) + requirements * kRequirementBytes;
    }
    const uint32_t flags = read_u32_at(descriptor);
    if (flags & kClassHasResilientSuperclass) cursor += kResilientSuperclassBytes;
    if (((flags >> kMetadataInitializationShift) & kMetadataInitializationMask) == kSingletonMetadataInitialization) cursor += kSingletonInitializationBytes;
    return cursor;
}

// Fills the class's own slots from its descriptor, then applies the overrides of inherited ones.
void install_class_vtable(void** metadata, const char* descriptor) {
    const uint32_t flags = read_u32_at(descriptor);
    if (!(flags & (kClassHasVTable | kClassHasOverrides))) return;
    const char* cursor = class_vtable_header(descriptor);
    if (flags & kClassHasVTable) {
        const uint32_t offset = read_u32_at(cursor);
        const uint32_t size = read_u32_at(cursor + 4);
        cursor += kVTableHeaderBytes;
        for (uint32_t i = 0; i < size; ++i) {
            const char* implementation = relative_address(cursor + kMethodImplementationOffset);
            metadata[offset + i] = const_cast<char*>(implementation);
            cursor += kMethodDescriptorBytes;
        }
    }
    if (!(flags & kClassHasOverrides)) return;
    const uint32_t count = read_u32_at(cursor);
    cursor += sizeof(uint32_t);
    for (uint32_t i = 0; i < count; ++i, cursor += kOverrideEntryBytes) {
        const char* base = indirect_relative(cursor);
        const char* method = indirect_relative(cursor + 4);
        if (base == nullptr || method == nullptr || unresolved_symbol_at(reinterpret_cast<uintptr_t>(base)) != nullptr) continue;
        const char* header = class_vtable_header(base);
        const uint32_t base_offset = read_u32_at(header);
        const size_t index = static_cast<size_t>(method - (header + kVTableHeaderBytes)) / kMethodDescriptorBytes;
        metadata[base_offset + index] = const_cast<char*>(relative_address(cursor + 8));
    }
}

extern "C" void* rt_updateClassMetadata2(char* metadata, size_t, size_t field_count, const void* const* field_layouts, size_t* field_offsets);

// Completes a generic class: finds its superclass, inherits that class's slots, then lays out its fields.
extern "C" void* rt_initClassMetadata2(char* metadata, size_t flags, size_t field_count, const void* const* field_layouts, size_t* field_offsets) {
    auto** words = reinterpret_cast<void**>(metadata);
    const auto* descriptor = static_cast<const char*>(words[kClassDescriptionWord]);
    const uint32_t positive = read_u32_at(descriptor + kClassPositiveSizeOffset);
    const uint32_t immediate = read_u32_at(descriptor + kClassImmediateMembersOffset);
    void* const* arguments = words + positive - immediate;
    if (const char* name = relative_address(descriptor + kClassSuperclassTypeOffset)) {
        const size_t length = mangled_name_length(name);
        auto** super = static_cast<void**>(rt_getTypeByMangledNameInContext2(name, length, descriptor, arguments));
        words[1] = super;
        if (super != nullptr && unresolved_symbol_at(reinterpret_cast<uintptr_t>(super)) == nullptr && super[kClassDescriptionWord] != nullptr) {
            const uint32_t super_positive = read_u32_at(static_cast<const char*>(super[kClassDescriptionWord]) + kClassPositiveSizeOffset);
            memcpy(words + kClassFixedWords, super + kClassFixedWords, (super_positive - kClassFixedWords) * sizeof(void*));
        }
    }
    install_class_vtable(words, descriptor);
    return rt_updateClassMetadata2(metadata, flags, field_count, field_layouts, field_offsets);
}

constexpr size_t kAccessorPoolSize = 256 * 1024;
constexpr size_t kAccessorStubSize = 32;

// Accessors take generic arguments in registers, so each needs its arity; anything unlisted is non-generic.
size_t accessor_generic_arity(std::string_view symbol) {
    static const std::unordered_map<std::string_view, size_t> arities = {
        {"$s7SwiftUI5StateVMa", 1},
        {"$s7SwiftUI7BindingVMa", 1},
        {"$s7SwiftUI12FetchRequestVMa", 1},
        {"$s7SwiftUI5GroupVMa", 1},
        {"$s7SwiftUI4FormVMa", 1},
        {"$s7SwiftUI10ScrollViewVMa", 1},
        {"$s7SwiftUI6ZStackVMa", 1},
        {"$s7SwiftUI9LazyVGridVMa", 1},
        {"$s7SwiftUI28UIApplicationDelegateAdaptorVMa", 1},
        {"$s7SwiftUI4ListVMa", 2},
        {"$s7SwiftUI4MenuVMa", 2},
        {"$s7SwiftUI15NavigationStackVMa", 2},
        {"$s7SwiftUI11ToolbarItemVMa", 2},
        {"$s7SwiftUI19_ConditionalContentVMa", 2},
        {"$s7SwiftUI19_ConditionalContentV7StorageOMa", 2},
        {"$s7SwiftUI7SectionVMa", 3},
        {"$sScCMa", 2},
        {"$s7SwiftUI20_ValueActionModifierVMa", 1},
        {"$s7Combine9PublishedVMa", 1},
        {"$s7Combine8DeferredVMa", 1},
        {"$s7Combine11SubscribersO10CompletionOMa", 1},
        {"$s7Combine12AnyPublisherVMa", 2},
        {"$s7Combine18PassthroughSubjectCMa", 2},
        {"$s7Combine6FutureCMa", 2},
        {"$sSaMa", 1},
        {"$sShMa", 1},
        {"$sSDMa", 2},
        {"$ss6ResultOMa", 2},
    };
    if (const auto found = arities.find(symbol); found != arities.end()) return found->second;
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
    if (name == "$s10Foundation4DateVMa") return {pod_type("Date", 8), 0};
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
    if (name == "swift_getTypeByMangledNameInContext") return reinterpret_cast<void*>(rt_getTypeByMangledNameInContext2);
    if (name == "swift_getTypeByMangledNameInContextInMetadataState") return reinterpret_cast<void*>(rt_getTypeByMangledNameInContextInMetadataState2);
    if (name == "swift_getTypeByMangledNameInContextInMetadataState2") return reinterpret_cast<void*>(rt_getTypeByMangledNameInContextInMetadataState2);
    if (name == "swift_getGenericMetadata") return reinterpret_cast<void*>(rt_getGenericMetadata);
    if (name == "swift_allocateGenericValueMetadata") return reinterpret_cast<void*>(rt_allocateGenericValueMetadata);
    if (name == "swift_getOpaqueTypeMetadata") return reinterpret_cast<void*>(rt_getOpaqueTypeMetadata);
    if (name == "swift_allocateGenericClassMetadata") return reinterpret_cast<void*>(rt_allocateGenericClassMetadata);
    if (name == "swift_initClassMetadata2") return reinterpret_cast<void*>(rt_initClassMetadata2);
    if (name == "swift_getExistentialTypeMetadata") return reinterpret_cast<void*>(rt_getExistentialTypeMetadata);
    if (name == "swift_getFunctionTypeMetadata") return reinterpret_cast<void*>(rt_getFunctionTypeMetadata);
    if (name == "swift_getFunctionTypeMetadata0") return reinterpret_cast<void*>(rt_getFunctionTypeMetadata);
    if (name == "swift_getFunctionTypeMetadata1") return reinterpret_cast<void*>(rt_getFunctionTypeMetadata);
    if (name == "swift_getFunctionTypeMetadata2") return reinterpret_cast<void*>(rt_getFunctionTypeMetadata);
    if (name == "swift_getMetatypeMetadata") return reinterpret_cast<void*>(rt_getMetatypeMetadata);
    if (name == "swift_getTupleTypeMetadata") return reinterpret_cast<void*>(rt_getTupleTypeMetadata);
    if (name == "swift_getTupleTypeMetadata2") return reinterpret_cast<void*>(rt_getTupleTypeMetadata2);
    if (name == "swift_getTupleTypeMetadata3") return reinterpret_cast<void*>(rt_getTupleTypeMetadata3);
    if (name == "swift_getTupleTypeLayout") return reinterpret_cast<void*>(rt_getTupleTypeLayout);
    if (name == "swift_getTupleTypeLayout2") return reinterpret_cast<void*>(rt_getTupleTypeLayout2);
    if (name == "swift_getTupleTypeLayout3") return reinterpret_cast<void*>(rt_getTupleTypeLayout3);
    if (name == "swift_checkMetadataState") return reinterpret_cast<void*>(rt_checkMetadataState);
    if (name == "swift_initEnumMetadataSingleCase") return reinterpret_cast<void*>(rt_initEnumMetadataSingleCase);
    if (name == "swift_initEnumMetadataSinglePayload") return reinterpret_cast<void*>(rt_initEnumMetadataSinglePayload);
    if (name == "swift_initEnumMetadataMultiPayload") return reinterpret_cast<void*>(rt_initEnumMetadataMultiPayload);
    if (name == "swift_getObjCClassMetadata") return reinterpret_cast<void*>(rt_getObjCClassMetadata);
    if (name == "swift_getObjCClassFromMetadata") return reinterpret_cast<void*>(rt_getObjCClassFromMetadata);
    if (name == "swift_getSingletonMetadata") return reinterpret_cast<void*>(rt_getSingletonMetadata);
    if (name == "swift_initStructMetadata") return reinterpret_cast<void*>(rt_initStructMetadata);
    if (name == "swift_updateClassMetadata2") return reinterpret_cast<void*>(rt_updateClassMetadata2);
    if (name.size() > 3 && name.substr(0, 2) == "$s" && name.back() == 'N') {
        const std::lock_guard guard(g_types_lock);
        // Unlike nominal types, these two symbols name the full metadata, one word before its address point.
        if (name == "$sytN") return reinterpret_cast<char*>(pod_type("yt", 0)) - sizeof(void*);
        if (name == "$sypN") return reinterpret_cast<char*>(any_type()) - sizeof(void*);
        if (name == "$sSSN" || name == "$sSS8UTF8ViewVN" || name == "$ss11_StringGutsVN" || name == "$ss26DefaultStringInterpolationVN") return string_type();
        if (name == "$sSiN") return pod_type("Si", 8);
        if (name == "$sSbN") return named_type("Sb", kKindStruct, new Layout{LayoutKind::Pod, {}, nullptr}, 1, 1, 254);
        if (name == "$sSJN") return two_word_type("SJ", word_type(), bridge_word_type());
        if (name == "$sSsN" || name == "$sSs8UTF8ViewVN") return named_type("Ss", kKindStruct, new Layout{LayoutKind::Aggregate, {{word_type(), 0}, {word_type(), 8}, {word_type(), 16}, {bridge_word_type(), 24}}, nullptr}, 32, 8, kPointerExtraInhabitants);
        if (name == "$ss5NeverON") return pod_type("Never", 0);
        if (name == "$s7SwiftUI22HierarchicalShapeStyleVN") return pod_type(std::string(name), 4);
        if (name == "$s10Foundation4DateVN") return pod_type("Date", 8);
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
    const size_t length = mangled_name_length(name);
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

std::vector<TypeField> type_fields(const void* metadata) {
    std::vector<TypeField> result;
    const std::lock_guard guard(g_types_lock);
    if (g_foreign_names.find(metadata) == g_foreign_names.end() && static_cast<const Metadata*>(metadata)->kind != kKindTuple) return result;
    const auto* layout = layout_of(metadata);
    if (layout == nullptr) return result;
    for (const auto& field : layout->fields) result.push_back({field.type, field.offset});
    return result;
}

bool conditional_active(const void* metadata, const void* value, TypeField& out) {
    const auto* layout = layout_of(metadata);
    if (g_foreign_names.find(metadata) == g_foreign_names.end() || layout->kind != LayoutKind::Conditional) return false;
    out = {layout->fields[state_storage_tag(value, metadata)].type, 0};
    return true;
}

bool optional_payload(const void* metadata, const void* value, TypeField& out) {
    if (static_cast<const Metadata*>(metadata)->kind != kKindOptional) return false;
    auto* payload = layout_of(metadata)->payload;
    if (witnesses_of(payload)->get_enum_tag_single_payload(value, 1, payload) != 0)
        out = {nullptr, 0};
    else
        out = {payload, 0};
    return true;
}

void* array_element_type(const void* metadata) {
    const std::lock_guard guard(g_types_lock);
    const auto found = g_array_elements.find(metadata);
    return found == g_array_elements.end() ? nullptr : found->second;
}

void* optional_metadata(void* payload) {
    const std::lock_guard guard(g_types_lock);
    return optional_of(static_cast<Metadata*>(payload));
}

void* any_object_metadata() {
    const std::lock_guard guard(g_types_lock);
    return native_type();
}

namespace {

constexpr size_t kCastUnconditional = 1;
constexpr size_t kCastTakeOnSuccess = 2;
constexpr size_t kCastDestroyOnFailure = 4;
constexpr size_t kAnyTypeWord = 3;

}  // namespace

// `as? [T]` and `as? [String: T]` convert element by element, failing as a whole if one element does not fit.
bool cast_collection(void* destination, void* value, void* contained, void* target_type) {
    uint64_t word;
    void* target_element = array_element_type(target_type);
    void* source_element = array_element_type(contained);
    if (target_element != nullptr && source_element != nullptr) {
        memcpy(&word, value, sizeof word);
        char* from;
        const size_t count = array_storage_contents(reinterpret_cast<void*>(word & kBridgePointerMask), source_element, &from);
        char* to;
        void* storage = make_array_storage(target_element, count, &to);
        const size_t from_stride = witnesses_of(source_element)->stride;
        const size_t to_stride = witnesses_of(target_element)->stride;
        for (size_t i = 0; i < count; ++i) {
            if (!swift_cast(to + i * to_stride, from + i * from_stride, source_element, target_element, 0)) {
                for (size_t done = 0; done < i; ++done) witnesses_of(target_element)->destroy(to + done * to_stride, target_element);
                free(storage);
                return false;
            }
        }
        memcpy(destination, &storage, sizeof storage);
        return true;
    }
    void* target_value = dictionary_value_type(target_type);
    void* source_value = dictionary_value_type(contained);
    if (target_value == nullptr || source_value == nullptr) return false;
    memcpy(&word, value, sizeof word);
    const size_t stride = witnesses_of(target_value)->stride;
    std::vector<std::pair<std::string, size_t>> converted;
    std::vector<char> bytes;
    bool failed = false;
    visit_string_dictionary(reinterpret_cast<void*>(word & kBridgePointerMask), [&](const std::string& key, void* member) {
        if (failed) return;
        const size_t at = bytes.size();
        bytes.resize(at + stride);
        std::vector<char> slot(stride + 8);
        if (!swift_cast(slot.data(), member, source_value, target_value, 0)) {
            failed = true;
            return;
        }
        memcpy(bytes.data() + at, slot.data(), stride);
        converted.emplace_back(key, at);
    });
    if (failed) {
        for (const auto& entry : converted) witnesses_of(target_value)->destroy(bytes.data() + entry.second, target_value);
        return false;
    }
    std::vector<std::pair<std::string, const void*>> entries;
    for (const auto& [key, at] : converted) entries.emplace_back(key, bytes.data() + at);
    void* storage = make_string_dictionary(target_value, entries);
    memcpy(destination, &storage, sizeof storage);
    return true;
}

bool swift_cast(void* destination, void* source, void* source_type, void* target_type, size_t flags) {
    const auto* source_metadata = static_cast<Metadata*>(source_type);
    void* value = source;
    void* contained = source_type;
    // Open an Any: its value sits inline, or in a box for large types.
    if (source_metadata->kind == kKindExistential && layout_of(source_type)->kind == LayoutKind::OpaqueExistential) {
        auto** words = static_cast<void**>(source);
        contained = words[kAnyTypeWord];
        if (contained == nullptr) goto fail;
        if (witnesses_of(contained)->flags & kFlagNonInline) {
            const size_t mask = witnesses_of(contained)->flags & 0xff;
            value = static_cast<char*>(words[0]) + ((sizeof(HeapObject) + mask) & ~mask);
        }
    }
    // An Optional target holds the value with no case to store, when it uses spare bits.
    if (static_cast<Metadata*>(target_type)->kind == kKindOptional) {
        Metadata* payload = layout_of(target_type)->payload;
        if (swift_cast(destination, value, contained, payload, 0)) {
            witnesses_of(payload)->store_enum_tag_single_payload(destination, 0, 1, payload);
            if (flags & kCastTakeOnSuccess) witnesses_of(source_type)->destroy(source, source_type);
            return true;
        }
        goto fail;
    }
    if (contained == target_type) {
        witnesses_of(target_type)->initialize_with_copy(destination, value, target_type);
        if (flags & kCastTakeOnSuccess) witnesses_of(source_type)->destroy(source, source_type);
        return true;
    }
    if (static_cast<Metadata*>(target_type)->kind == kKindExistential && layout_of(target_type)->kind == LayoutKind::OpaqueExistential && witnesses_of(target_type)->size == 4 * kPointerSize) {
        auto** words = static_cast<void**>(destination);
        memset(words, 0, 4 * kPointerSize);
        if (witnesses_of(contained)->flags & kFlagNonInline) {
            engine_log("STOP: cast of a large value into Any");
            abort();
        }
        witnesses_of(contained)->initialize_with_copy(destination, value, contained);
        words[kAnyTypeWord] = contained;
        if (flags & kCastTakeOnSuccess) witnesses_of(source_type)->destroy(source, source_type);
        return true;
    }
    if (cast_collection(destination, value, contained, target_type)) {
        if (flags & kCastTakeOnSuccess) witnesses_of(source_type)->destroy(source, source_type);
        return true;
    }
fail:
    if (flags & kCastUnconditional) {
        engine_log("STOP: an unconditional cast failed");
        abort();
    }
    if (flags & kCastDestroyOnFailure) witnesses_of(source_type)->destroy(source, source_type);
    return false;
}

void* date_metadata() {
    const std::lock_guard guard(g_types_lock);
    return pod_type("Date", 8);
}

void* dictionary_value_type(const void* metadata) {
    const std::lock_guard guard(g_types_lock);
    const auto found = g_dictionary_values.find(metadata);
    return found == g_dictionary_values.end() ? nullptr : found->second;
}
