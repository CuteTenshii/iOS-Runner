#include "key_path.h"

#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <new>

#include "engine.h"
#include "swift_abi.h"
#include "type_system.h"

namespace {

// Field layout of KeyPathPattern and its components, from the Swift ABI.
constexpr size_t kPatternValueTypeOffset = 12;
constexpr size_t kPatternComponentsOffset = 24;
constexpr uint32_t kBufferSizeMask = 0x00ffffff;
constexpr uint32_t kComponentKindMask = 0x7f000000;
constexpr unsigned kComponentKindShift = 24;
constexpr uint32_t kPayloadMask = 0x00ffffff;
constexpr uint32_t kKindExternal = 0;
constexpr uint32_t kKindStruct = 1;
constexpr uint32_t kKindComputed = 2;
constexpr uint32_t kKindClass = 3;
constexpr uint32_t kKindOptional = 4;
constexpr uint32_t kOffsetOutOfLine = 0x00ffffff;
constexpr uint32_t kOffsetUnresolvedDirect = 0x00fffffe;
constexpr uint32_t kOffsetUnresolvedIndirect = 0x00fffffd;
constexpr uint32_t kComputedSettable = 0x00400000;
constexpr uint32_t kComputedHasArguments = 0x00080000;
constexpr size_t kTemporaryBufferSize = 512;

struct KeyPathObject {
    void* metadata;
    std::atomic<uint64_t> references;
    const char* pattern;
    const void* arguments;
};

const HeapMetadata kKeyPathMetadata = {free, nullptr, 0};

uint32_t read_u32(const char* at) {
    uint32_t value;
    memcpy(&value, at, sizeof value);
    return value;
}

const void* relative_target(const char* field) {
    int32_t offset;
    memcpy(&offset, field, sizeof offset);
    return offset == 0 ? nullptr : field + offset;
}

// Follows the offset word of an unresolved stored component to the value it names.
size_t stored_offset(uint32_t payload, const char*& cursor) {
    if (payload == kOffsetOutOfLine) {
        const uint32_t value = read_u32(cursor);
        cursor += sizeof(uint32_t);
        return value;
    }
    if (payload == kOffsetUnresolvedDirect) {
        const auto* global = static_cast<const uint32_t*>(relative_target(cursor));
        cursor += sizeof(uint32_t);
        return *global;
    }
    if (payload == kOffsetUnresolvedIndirect) {
        // Either the offset variable itself or a GOT-like pointer to it: an offset is never a valid address.
        const auto* slot = static_cast<const uint64_t*>(relative_target(cursor));
        cursor += sizeof(uint32_t);
        constexpr uint64_t kLargestFieldOffset = 1ULL << 20;
        return *slot < kLargestFieldOffset ? *slot : *reinterpret_cast<const uint32_t*>(*slot);
    }
    return payload;
}

extern "C" void rt_key_path_getter(const void* function, void* out, void* base, const void* arguments, size_t size);
extern "C" void rt_key_path_setter(const void* function, void* value, void* base, const void* arguments, size_t size);

struct Step {
    uint32_t kind;
    size_t offset;
    const void* getter;
    const void* setter;
    const void* arguments;
    size_t argument_size;
};

// Decodes one component and advances past it; `arguments` is advanced past any captured arguments.
bool decode_step(const char*& cursor, Step& step, const char* end) {
    if (cursor >= end) return false;
    const uint32_t header = read_u32(cursor);
    cursor += sizeof header;
    step = {};
    step.kind = (header & kComponentKindMask) >> kComponentKindShift;
    uint32_t payload = header & kPayloadMask;
    if (step.kind == kKindExternal) {
        // A property in another module: its descriptor and arguments, then a local component that does the work.
        cursor += sizeof(uint32_t) * (1 + payload);
        const uint32_t local = read_u32(cursor);
        cursor += sizeof local;
        step.kind = (local & kComponentKindMask) >> kComponentKindShift;
        payload = local & kPayloadMask;
    }
    switch (step.kind) {
        case kKindStruct:
        case kKindClass:
            step.offset = stored_offset(payload, cursor);
            return true;
        case kKindComputed:
            cursor += sizeof(uint32_t);  // identity, only used for key path equality
            step.getter = relative_target(cursor);
            cursor += sizeof(uint32_t);
            if (payload & kComputedSettable) {
                step.setter = relative_target(cursor);
                cursor += sizeof(uint32_t);
            }
            if (payload & kComputedHasArguments) {
                engine_log("STOP: key paths with captured arguments are not supported yet");
                abort();
            }
            return true;
        case kKindOptional:
            return true;
        default:
            engine_log("STOP: unsupported key path component kind %u", step.kind);
            abort();
    }
}

}  // namespace

// Swift's thin key path accessors: value pointers in order, then arguments, result through x8.
__asm__(
    ".text\n"
    ".globl rt_key_path_getter\n"
    "rt_key_path_getter:\n"
    "  stp x29, x30, [sp, #-16]!\n"
    "  mov x29, sp\n"
    "  mov x16, x0\n"
    "  mov x8, x1\n"
    "  mov x0, x2\n"
    "  mov x1, x3\n"
    "  mov x2, x4\n"
    "  blr x16\n"
    "  ldp x29, x30, [sp], #16\n"
    "  ret\n"
    ".globl rt_key_path_setter\n"
    "rt_key_path_setter:\n"
    "  stp x29, x30, [sp, #-16]!\n"
    "  mov x29, sp\n"
    "  mov x16, x0\n"
    "  mov x0, x1\n"
    "  mov x1, x2\n"
    "  mov x2, x3\n"
    "  mov x3, x4\n"
    "  blr x16\n"
    "  ldp x29, x30, [sp], #16\n"
    "  ret\n");

void* key_path_create(const void* pattern, const void* arguments) {
    auto* object = static_cast<KeyPathObject*>(calloc(1, sizeof(KeyPathObject)));
    object->metadata = const_cast<uint64_t*>(&kKeyPathMetadata.kind);
    new (&object->references) std::atomic<uint64_t>(1);
    object->pattern = static_cast<const char*>(pattern);
    object->arguments = arguments;
    return object;
}

namespace {

// Walks to the last component, leaving `base` pointing at the value that component is applied to.
template <typename Finish>
void walk_key_path(const void* key_path, void* root, Finish finish) {
    const auto* path = static_cast<const KeyPathObject*>(key_path);
    const char* buffer = path->pattern + kPatternComponentsOffset - sizeof(uint32_t);
    const char* cursor = buffer + sizeof(uint32_t);
    const char* end = cursor + (read_u32(buffer) & kBufferSizeMask);

    void* base = root;
    char temporary[kTemporaryBufferSize];
    for (;;) {
        Step step;
        if (!decode_step(cursor, step, end)) {
            engine_log("STOP: empty key path");
            abort();
        }
        // Components after the first are preceded by a metadata reference for the intermediate type.
        const bool last = cursor + sizeof(uint32_t) >= end;
        if (last) {
            finish(step, base);
            return;
        }
        cursor += sizeof(uint32_t);
        switch (step.kind) {
            case kKindStruct:
                base = static_cast<char*>(base) + step.offset;
                break;
            case kKindClass:
                base = *static_cast<char**>(base) + step.offset;
                break;
            case kKindComputed:
                rt_key_path_getter(step.getter, temporary, base, step.arguments, step.argument_size);
                base = temporary;
                break;
            default:
                break;
        }
    }
}

[[noreturn]] void stop_unsupported_component(uint32_t kind) {
    engine_log("STOP: unsupported final key path component %u", kind);
    abort();
}

}  // namespace

void key_path_get(const void* key_path, void* root, void* out, void* value_type) {
    walk_key_path(key_path, root, [&](const Step& step, void* base) {
        switch (step.kind) {
            case kKindStruct:
                witnesses_of(value_type)->initialize_with_copy(out, static_cast<char*>(base) + step.offset, value_type);
                break;
            case kKindClass:
                witnesses_of(value_type)->initialize_with_copy(out, *static_cast<char**>(base) + step.offset, value_type);
                break;
            case kKindComputed:
                rt_key_path_getter(step.getter, out, base, step.arguments, step.argument_size);
                break;
            default:
                stop_unsupported_component(step.kind);
        }
    });
}

void key_path_set(const void* key_path, void* root, void* new_value, void* value_type) {
    walk_key_path(key_path, root, [&](const Step& step, void* base) {
        switch (step.kind) {
            case kKindStruct:
                witnesses_of(value_type)->assign_with_copy(static_cast<char*>(base) + step.offset, new_value, value_type);
                break;
            case kKindClass:
                witnesses_of(value_type)->assign_with_copy(*static_cast<char**>(base) + step.offset, new_value, value_type);
                break;
            case kKindComputed:
                if (step.setter == nullptr) {
                    engine_log("STOP: wrote through a read-only key path");
                    abort();
                }
                rt_key_path_setter(step.setter, new_value, base, step.arguments, step.argument_size);
                break;
            default:
                stop_unsupported_component(step.kind);
        }
    });
}

void* key_path_value_type(const void* key_path) {
    const auto* path = static_cast<const KeyPathObject*>(key_path);
    // The low bit of the offset marks a mangled name rather than an accessor.
    const auto target = reinterpret_cast<uintptr_t>(relative_target(path->pattern + kPatternValueTypeOffset));
    return resolve_mangled_type(reinterpret_cast<const char*>(target & ~static_cast<uintptr_t>(1)), static_cast<void* const*>(path->arguments));
}

namespace {

// What swift_modifyAtReferenceWritableKeyPath keeps between yielding the value and being resumed.
struct ModifyState {
    const void* key_path;
    void* root;
    void* value;
    void* type;
};

SWIFTCC void finish_modify(void** buffer, bool) {
    auto* state = static_cast<ModifyState*>(*buffer);
    key_path_set(state->key_path, state->root, state->value, state->type);
    witnesses_of(state->type)->destroy(state->value, state->type);
    free(state->value);
    free(state);
}

}  // namespace

SWIFTCC KeyPathModification key_path_begin_modify(void** buffer, void* root, const void* key_path) {
    void* type = key_path_value_type(key_path);
    size_t size;
    memcpy(&size, static_cast<const char*>(static_cast<void**>(type)[-1]) + kValueWitnessSizeOffset, sizeof size);
    auto* state = static_cast<ModifyState*>(calloc(1, sizeof(ModifyState)));
    *state = {key_path, root, calloc(1, size == 0 ? 1 : size), type};
    key_path_get(key_path, root, state->value, type);
    *buffer = state;
    return {reinterpret_cast<const void*>(finish_modify), state->value};
}
