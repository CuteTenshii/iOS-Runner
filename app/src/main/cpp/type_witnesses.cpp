#include "type_system_internal.h"

namespace type_system {

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

}  // namespace type_system
