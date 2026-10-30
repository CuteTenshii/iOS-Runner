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

enum class LayoutKind { Pod, NativeReference, BridgeReference, Aggregate, Optional, OpaqueExistential };

struct Metadata;

struct Field {
    Metadata* type;
    size_t offset;
};

struct Layout {
    LayoutKind kind = LayoutKind::Pod;
    std::vector<Field> fields;
    Metadata* payload = nullptr;
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

void* witness_copy(void* destination, void* source, void* type) {
    const auto* layout = layout_of(type);
    auto* to = static_cast<char*>(destination);
    auto* from = static_cast<char*>(source);
    switch (layout->kind) {
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
    return named_type(key, kKindTuple, layout, offset, alignment, inhabitants);
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

// Types whose stored layout the app's compiled code depends on; everything else from a framework is one reference.
Metadata* foreign_nominal(const std::string& symbol, const std::vector<void*>& arguments) {
    std::string key = symbol;
    for (const void* argument : arguments) key += pointer_key(argument);
    if (const auto found = g_named_types.find(key); found != g_named_types.end()) return found->second;
    return named_type(key, kKindStruct, new Layout{LayoutKind::NativeReference, {}, nullptr}, kPointerSize, kPointerSize,
                      kPointerExtraInhabitants, arguments);
}

struct NominalReference {
    const void* descriptor = nullptr;
    const char* foreign_symbol = nullptr;
};

struct Node {
    enum class Tag { Type, Nominal, Protocol, ListMarker } tag = Tag::Type;
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
    MangledName(const char* text, size_t length, void* const* generics) : cursor_(text), end_(text + length), start_(text), generics_(generics) {}

    Metadata* parse() {
        while (cursor_ < end_ && failure_.empty()) step();
        if (failure_.empty() && (stack_.size() != 1 || stack_.back().tag == Node::Tag::ListMarker)) fail("unbalanced type stack");
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
        substitute(node);
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
                if (concurrency == 'M') { push_type(two_word_type("ScM", native_type(), word_type())); return; }
                if (concurrency == 'P') { push_type(pod_type("ScP", 1)); return; }
                fail(std::string("unknown concurrency type Sc") + concurrency);
                return;
            }
            default:
                break;
        }
        fail(std::string("unknown standard type S") + code);
    }

    void bound_generic() {
        std::vector<void*> arguments;
        while (!stack_.empty() && stack_.back().tag != Node::Tag::ListMarker) {
            if (stack_.size() >= 2 && stack_[stack_.size() - 2].tag == Node::Tag::ListMarker) {
                arguments.insert(arguments.begin(), as_type(pop()));
                break;
            }
            arguments.insert(arguments.begin(), as_type(pop()));
        }
        pop();
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
        while (!stack_.empty() && stack_.back().tag != Node::Tag::ListMarker) {
            elements.insert(elements.begin(), as_type(pop()));
        }
        pop();
        push_type(tuple_of(elements));
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
        if (byte >= 1 && byte <= 0x17) {
            if (byte != 1 && byte != 2) {
                fail("unsupported symbolic reference kind");
                return;
            }
            symbolic(byte);
            return;
        }
        if (byte == 0xff) {
            // Absolute pointer to a metadata-access helper, aligned in the name; not used for type values here.
            fail("unsupported metadata-access-function reference");
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
                } else if (stack_.size() == 1 || stack_[stack_.size() - 2].tag != Node::Tag::ListMarker) {
                    // Marks the first tuple element: tuples carry no opening `y`, so insert it under the element.
                    if (stack_.empty()) { fail("list separator on empty stack"); return; }
                    Node element = pop();
                    Node marker;
                    marker.tag = Node::Tag::ListMarker;
                    stack_.push_back(marker);
                    stack_.push_back(element);
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
                if (cursor_ < end_ && *cursor_ == 'l') { ++cursor_; push_type(native_type()); return; }
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
    const char* start_;
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
            out += "<ref>";
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
    MangledName parser(name, length, generics);
    Metadata* result = parser.parse();
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
    return nullptr;
}


constexpr size_t kAccessorPoolSize = 256 * 1024;
constexpr size_t kAccessorStubSize = 32;

// Accessors take generic arguments in registers, so each needs its arity; anything unlisted is non-generic.
size_t accessor_generic_arity(std::string_view symbol) {
    if (symbol == "$s7SwiftUI15ModifiedContentVMa") return 2;
    if (symbol == "$s7SwiftUI8BindableVMa" || symbol == "$s7SwiftUI9LazyStateVMa" || symbol == "$sSqMa" ||
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
    if (name == "$s7SwiftUI15ModifiedContentVMa") {
        std::string key(symbol);
        for (const void* argument : arguments) key += pointer_key(argument);
        if (const auto found = g_named_types.find(key); found != g_named_types.end()) return {found->second, 0};
        auto* tuple = tuple_of({static_cast<Metadata*>(first), static_cast<Metadata*>(second)});
        const auto* witnesses = witnesses_of(tuple);
        return {named_type(key, kKindStruct, new Layout(*layout_of(tuple)), witnesses->size, (witnesses->flags & 0xff) + 1, witnesses->extra_inhabitants, arguments), 0};
    }
    return {foreign_nominal(symbol, arguments), 0};
}

extern "C" void rt_foreign_access_entry();
__asm__(
    ".text\n"
    ".globl rt_foreign_access_entry\n"
    "rt_foreign_access_entry:\n"
    "  mov x4, x16\n"
    "  b rt_foreign_access\n");

void* make_accessor_stub(const std::string& symbol) {
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
    const char* name = strdup(symbol.c_str());
    void* entry = reinterpret_cast<void*>(rt_foreign_access_entry);
    memcpy(stub + 4, &name, sizeof name);
    memcpy(stub + 6, &entry, sizeof entry);
    __builtin___clear_cache(reinterpret_cast<char*>(stub), reinterpret_cast<char*>(stub + 8));
    return stub;
}

}  // namespace

void* type_system_symbol(std::string_view name) {
    if (name == "swift_getTypeByMangledNameInContext2") return reinterpret_cast<void*>(rt_getTypeByMangledNameInContext2);
    if (name == "swift_getTypeByMangledNameInContextInMetadataState2") return reinterpret_cast<void*>(rt_getTypeByMangledNameInContextInMetadataState2);
    if (name == "swift_getSingletonMetadata") return reinterpret_cast<void*>(rt_getSingletonMetadata);
    if (name == "swift_initStructMetadata") return reinterpret_cast<void*>(rt_initStructMetadata);
    if (name == "swift_updateClassMetadata2") return reinterpret_cast<void*>(rt_updateClassMetadata2);
    if (name.size() > 4 && name.substr(0, 2) == "$s" && name.substr(name.size() - 2) == "Ma") return make_accessor_stub(std::string(name));
    return nullptr;
}
