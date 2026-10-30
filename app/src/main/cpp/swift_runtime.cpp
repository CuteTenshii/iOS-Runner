#include "swift_runtime.h"

#include <pthread.h>

#include <atomic>
#include <cstdint>
#include <new>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>

#include "engine.h"
#include "macho.h"
#include "swiftui_shim.h"
#include "type_system.h"

namespace {

constexpr uint64_t kImmortal = 1ULL << 63;
// Swift keeps its discriminator bits in the top nibble of bridged references.
constexpr uint64_t kBridgePointerMask = 0x0ffffffffffffff8ULL;
constexpr uint64_t kBridgeImmediateBit = 1ULL << 63;
constexpr size_t kBodyBufferSize = 64 * 1024;
constexpr size_t kForeignWitnessTableWords = 32;

struct HeapObject {
    void* metadata;
    std::atomic<uint64_t> references;
};

using Destroyer = void (*)(HeapObject*);

void* retain_object(void* raw) {
    if (raw == nullptr || reinterpret_cast<intptr_t>(raw) < 0) return raw;
    auto* object = static_cast<HeapObject*>(raw);
    if (!(object->references.load(std::memory_order_relaxed) & kImmortal)) {
        object->references.fetch_add(1, std::memory_order_relaxed);
    }
    return raw;
}

void release_object(void* raw) {
    if (raw == nullptr || reinterpret_cast<intptr_t>(raw) < 0) return;
    auto* object = static_cast<HeapObject*>(raw);
    if (object->references.load(std::memory_order_relaxed) & kImmortal) return;
    if (object->references.fetch_sub(1, std::memory_order_acq_rel) == 1) {
        // Class and box metadata keep their destroyer one word before the value witnesses.
        auto** metadata = static_cast<void**>(object->metadata);
        reinterpret_cast<Destroyer>(metadata[-2])(object);
    }
}

void* bridge_pointer(void* raw) {
    const auto bits = reinterpret_cast<uint64_t>(raw);
    if (bits & kBridgeImmediateBit) return nullptr;
    return reinterpret_cast<void*>(bits & kBridgePointerMask);
}

const void* relative_direct(const void* field) {
    int32_t offset;
    memcpy(&offset, field, sizeof offset);
    return offset == 0 ? nullptr : static_cast<const char*>(field) + offset;
}

// Low bit set means the field points at a GOT slot rather than at the target.
const void* relative_indirectable(const void* field) {
    int32_t offset;
    memcpy(&offset, field, sizeof offset);
    if (offset == 0) return nullptr;
    const char* target = static_cast<const char*>(field) + (offset & ~1);
    if (offset & 1) return *reinterpret_cast<void* const*>(target);
    return target;
}

constexpr uint32_t kConformanceRetroactive = 1u << 6;
constexpr uint32_t kConformanceConditionalMask = 0xffu << 8;
constexpr uint32_t kConformanceConditionalShift = 8;
constexpr uint32_t kConformanceResilient = 1u << 16;
constexpr uint32_t kConformanceGenericTable = 1u << 17;
constexpr size_t kGenericRequirementSize = 12;

struct ResilientWitnesses {
    uint32_t count = 0;
    const char* entries = nullptr;
    const char* after = nullptr;
};

ResilientWitnesses resilient_witnesses(const char* conformance) {
    uint32_t flags;
    memcpy(&flags, conformance + 12, sizeof flags);
    const char* cursor = conformance + 16;
    if (flags & kConformanceRetroactive) cursor += 4;
    cursor += ((flags & kConformanceConditionalMask) >> kConformanceConditionalShift) * kGenericRequirementSize;
    ResilientWitnesses result;
    result.after = cursor;
    if (flags & kConformanceResilient) {
        memcpy(&result.count, cursor, sizeof result.count);
        result.entries = cursor + 4;
        result.after = result.entries + result.count * 8;
    }
    return result;
}

extern "C" {

void* rt_swift_getWitnessTable(const char* conformance, void* type, void* arguments) {
    static std::mutex lock;
    static std::map<std::pair<const void*, void*>, void**> cache;
    const std::lock_guard guard(lock);
    const auto key = std::make_pair(static_cast<const void*>(conformance), type);
    if (const auto found = cache.find(key); found != cache.end()) return found->second;

    if (unresolved_symbol_at(reinterpret_cast<uintptr_t>(conformance)) != nullptr) {
        auto** table = static_cast<void**>(calloc(kForeignWitnessTableWords, sizeof(void*)));
        table[0] = const_cast<char*>(conformance);
        cache[key] = table;
        return table;
    }

    uint32_t flags;
    memcpy(&flags, conformance + 12, sizeof flags);
    const void* pattern = relative_direct(conformance + 8);
    if (!(flags & kConformanceGenericTable)) return const_cast<void*>(pattern);

    const char* generic = resilient_witnesses(conformance).after;
    uint16_t words, private_words;
    memcpy(&words, generic, sizeof words);
    memcpy(&private_words, generic + 2, sizeof private_words);
    auto** allocation = static_cast<void**>(calloc(words + private_words, sizeof(void*)));
    auto** table = allocation + private_words;
    memcpy(table, pattern, words * sizeof(void*));
    table[0] = const_cast<char*>(conformance);
    using Instantiator = void (*)(void**, void*, void*);
    if (const void* instantiator = relative_direct(generic + 4)) {
        reinterpret_cast<Instantiator>(const_cast<void*>(instantiator))(table, type, arguments);
    }
    cache[key] = table;
    return table;
}

void* rt_swift_allocObject(void* metadata, size_t size, size_t alignment_mask) {
    void* memory = nullptr;
    if (posix_memalign(&memory, alignment_mask < 16 ? 16 : alignment_mask + 1, size) != 0) abort();
    auto* object = static_cast<HeapObject*>(memory);
    object->metadata = metadata;
    new (&object->references) std::atomic<uint64_t>(1);
    return object;
}

void* rt_swift_initStackObject(void* metadata, void* raw) {
    auto* object = static_cast<HeapObject*>(raw);
    object->metadata = metadata;
    // Stack objects never reach zero on the normal path; the frame owns their storage.
    new (&object->references) std::atomic<uint64_t>(kImmortal);
    return object;
}

void rt_swift_deallocObject(void* object, size_t, size_t) { free(object); }
void rt_swift_deallocClassInstance(void* object, size_t, size_t) { free(object); }
void* rt_swift_setDeallocating(void* object) { return object; }

void* rt_swift_retain_n(void* raw, uint32_t count) {
    if (raw != nullptr && reinterpret_cast<intptr_t>(raw) >= 0) {
        auto* object = static_cast<HeapObject*>(raw);
        if (!(object->references.load(std::memory_order_relaxed) & kImmortal)) {
            object->references.fetch_add(count, std::memory_order_relaxed);
        }
    }
    return raw;
}

void* rt_swift_bridgeObjectRetain(void* raw) {
    retain_object(bridge_pointer(raw));
    return raw;
}
void rt_swift_bridgeObjectRelease(void* raw) { release_object(bridge_pointer(raw)); }
void* rt_swift_bridgeObjectRetain_n(void* raw, uint32_t count) {
    rt_swift_retain_n(bridge_pointer(raw), count);
    return raw;
}
void rt_swift_bridgeObjectRelease_n(void* raw, uint32_t count) {
    for (uint32_t i = 0; i < count; ++i) release_object(bridge_pointer(raw));
}

bool rt_swift_isUniquelyReferenced_nonNull_native(void* raw) {
    return static_cast<HeapObject*>(raw)->references.load(std::memory_order_acquire) == 1;
}

void rt_once(intptr_t* predicate, void (*function)(void*), void* context) {
    static pthread_mutex_t lock = PTHREAD_RECURSIVE_MUTEX_INITIALIZER_NP;
    pthread_mutex_lock(&lock);
    if (*predicate != -1) {
        function(context);
        __atomic_store_n(predicate, -1, __ATOMIC_RELEASE);
    }
    pthread_mutex_unlock(&lock);
}

void* rt_identity(void* value) { return value; }

void rt_app_main(void* metadata, void** witness_table);
void rt_swiftui_app_main_entry();
void* rt_swift_retain(void* object) { return retain_object(object); }
void rt_swift_release(void* object) { release_object(object); }
}

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

constexpr uint32_t kFlagNonPod = 0x10000;
constexpr uintptr_t kFirstValidPointer = 0x1000;

template <size_t Size>
void* pod_copy(void* destination, void* source, void*) {
    memcpy(destination, source, Size);
    return destination;
}

template <size_t Size>
uint32_t pod_get_tag(const void* value, uint32_t empty_cases, void*) {
    if (empty_cases == 0) return 0;
    uint8_t tag;
    memcpy(&tag, static_cast<const char*>(value) + Size, 1);
    return tag;
}

template <size_t Size>
void pod_store_tag(void* value, uint32_t tag, uint32_t empty_cases, void*) {
    if (empty_cases == 0) return;
    const auto byte = static_cast<uint8_t>(tag);
    memcpy(static_cast<char*>(value) + Size, &byte, 1);
}

template <size_t Size>
constexpr ValueWitnessTable pod_witnesses() {
    return {pod_copy<Size>, +[](void*, void*) {}, pod_copy<Size>, pod_copy<Size>, pod_copy<Size>, pod_copy<Size>,
            pod_get_tag<Size>, pod_store_tag<Size>, Size, Size == 0 ? 1 : Size,
            static_cast<uint32_t>(Size == 0 ? 0 : Size - 1), 0};
}

using RetainFunction = void* (*)(void*);
using ReleaseFunction = void (*)(void*);

template <RetainFunction Retain, ReleaseFunction Release>
constexpr ValueWitnessTable reference_witnesses() {
    return {
        +[](void* destination, void* source, void*) -> void* {
            auto* value = *static_cast<void**>(source);
            *static_cast<void**>(destination) = Retain(value);
            return destination;
        },
        +[](void* value, void*) { Release(*static_cast<void**>(value)); },
        +[](void* destination, void* source, void*) -> void* {
            auto* value = *static_cast<void**>(source);
            *static_cast<void**>(destination) = Retain(value);
            return destination;
        },
        +[](void* destination, void* source, void*) -> void* {
            auto* value = *static_cast<void**>(source);
            auto* previous = *static_cast<void**>(destination);
            *static_cast<void**>(destination) = Retain(value);
            Release(previous);
            return destination;
        },
        pod_copy<8>,
        +[](void* destination, void* source, void*) -> void* {
            auto* previous = *static_cast<void**>(destination);
            *static_cast<void**>(destination) = *static_cast<void**>(source);
            Release(previous);
            return destination;
        },
        // Invalid low pointers are the extra inhabitants: 0 is Optional.none.
        +[](const void* value, uint32_t empty_cases, void*) -> uint32_t {
            if (empty_cases == 0) return 0;
            const auto bits = *static_cast<const uintptr_t*>(value);
            return bits < kFirstValidPointer ? static_cast<uint32_t>(bits) + 1 : 0;
        },
        +[](void* value, uint32_t tag, uint32_t empty_cases, void*) {
            if (empty_cases != 0 && tag != 0) *static_cast<uintptr_t*>(value) = tag - 1;
        },
        8, 8, 7 | kFlagNonPod, static_cast<uint32_t>(kFirstValidPointer)};
}

void* bridge_retain(void* raw) { return rt_swift_bridgeObjectRetain(raw); }
void bridge_release(void* raw) { rt_swift_bridgeObjectRelease(raw); }

const ValueWitnessTable kEmptyWitnesses = pod_witnesses<0>();
const ValueWitnessTable kInt32Witnesses = pod_witnesses<4>();
const ValueWitnessTable kInt64Witnesses = pod_witnesses<8>();
const ValueWitnessTable kNativeObjectWitnesses = reference_witnesses<retain_object, release_object>();
const ValueWitnessTable kBridgeObjectWitnesses = reference_witnesses<bridge_retain, bridge_release>();

constexpr size_t kEmptyCollectionWords = 16;

// Shared empty collections: count 0, immortal, with the all-ones bitmap word Dictionary and Set expect.
uint64_t g_empty_array[kEmptyCollectionWords] = {0, kImmortal};
uint64_t g_empty_dictionary[kEmptyCollectionWords] = {0, kImmortal, 0, 0, 0, 0, 0, 0, ~0ULL};
uint64_t g_empty_set[kEmptyCollectionWords] = {0, kImmortal, 0, 0, 0, 0, 0, ~0ULL};

// Marker addresses: the guest and rt_app_main compare them, and nothing reads through them.
char g_app_init_requirement;
char g_app_body_requirement;

}  // namespace

// Register-preserving retain and release variants: the argument arrives in the named register,
// callers rely on every other caller-saved register surviving, and retains return the object in x0.
#define PRESERVING_THUNK(name, target, reg, result)        \
    ".globl " #name "\n"                           \
    #name ":\n"                                    \
    "  stp x29, x30, [sp, #-176]!\n"               \
    "  stp x0, x1, [sp, #16]\n"                    \
    "  stp x2, x3, [sp, #32]\n"                    \
    "  stp x4, x5, [sp, #48]\n"                    \
    "  stp x6, x7, [sp, #64]\n"                    \
    "  stp x8, x9, [sp, #80]\n"                    \
    "  stp x10, x11, [sp, #96]\n"                  \
    "  stp x12, x13, [sp, #112]\n"                 \
    "  stp x14, x15, [sp, #128]\n"                 \
    "  stp x16, x17, [sp, #144]\n"                 \
    "  mov x0, " #reg "\n"                         \
    "  bl " #target "\n"                           \
    "  ldp x0, x1, [sp, #16]\n"                    \
    "  ldp x2, x3, [sp, #32]\n"                    \
    "  ldp x4, x5, [sp, #48]\n"                    \
    "  ldp x6, x7, [sp, #64]\n"                    \
    "  ldp x8, x9, [sp, #80]\n"                    \
    "  ldp x10, x11, [sp, #96]\n"                  \
    "  ldp x12, x13, [sp, #112]\n"                 \
    "  ldp x14, x15, [sp, #128]\n"                 \
    "  ldp x16, x17, [sp, #144]\n"                 \
    "  ldp x29, x30, [sp], #176\n"                 \
    result                                         \
    "  ret\n"

#define THUNK_PAIR(reg) \
    PRESERVING_THUNK(rt_retain_##reg, rt_swift_retain, reg, "  mov x0, " #reg "\n") PRESERVING_THUNK(rt_release_##reg, rt_swift_release, reg, "")

__asm__(
    ".text\n"
    THUNK_PAIR(x1) THUNK_PAIR(x2) THUNK_PAIR(x8) THUNK_PAIR(x9) THUNK_PAIR(x10)
    THUNK_PAIR(x19) THUNK_PAIR(x20) THUNK_PAIR(x21) THUNK_PAIR(x22) THUNK_PAIR(x23)
    THUNK_PAIR(x24) THUNK_PAIR(x25) THUNK_PAIR(x26) THUNK_PAIR(x27) THUNK_PAIR(x28)
    // App.main() arrives with Self in x0, its witness table in x1 and the metatype in x20.
    ".globl rt_swiftui_app_main_entry\n"
    "rt_swiftui_app_main_entry:\n"
    "  b rt_app_main\n"
    // Swift's self and indirect-result registers cannot be set from C++.
    ".globl rt_call_witness\n"
    "rt_call_witness:\n"
    "  stp x29, x30, [sp, #-32]!\n"
    "  mov x29, sp\n"
    "  stp x19, x20, [sp, #16]\n"
    "  mov x16, x0\n"
    "  mov x8, x1\n"
    "  mov x20, x2\n"
    "  mov x0, x3\n"
    "  mov x1, x4\n"
    "  blr x16\n"
    "  ldp x19, x20, [sp, #16]\n"
    "  ldp x29, x30, [sp], #32\n"
    "  ret\n");

#define DECLARE_THUNK_PAIR(reg) extern "C" void rt_retain_##reg(); extern "C" void rt_release_##reg();
DECLARE_THUNK_PAIR(x1) DECLARE_THUNK_PAIR(x2) DECLARE_THUNK_PAIR(x8) DECLARE_THUNK_PAIR(x9) DECLARE_THUNK_PAIR(x10)
DECLARE_THUNK_PAIR(x19) DECLARE_THUNK_PAIR(x20) DECLARE_THUNK_PAIR(x21) DECLARE_THUNK_PAIR(x22) DECLARE_THUNK_PAIR(x23)
DECLARE_THUNK_PAIR(x24) DECLARE_THUNK_PAIR(x25) DECLARE_THUNK_PAIR(x26) DECLARE_THUNK_PAIR(x27) DECLARE_THUNK_PAIR(x28)

const void* find_witness(const char* conformance, const void* requirement) {
    const auto witnesses = resilient_witnesses(conformance);
    for (uint32_t i = 0; i < witnesses.count; ++i) {
        const char* entry = witnesses.entries + i * 8;
        if (relative_indirectable(entry) == requirement) return relative_direct(entry + 4);
    }
    return nullptr;
}

extern "C" void rt_app_main(void* metadata, void** witness_table) {
    engine_log("REACHED: SwiftUI App.main()");
    const auto* conformance = static_cast<const char*>(witness_table[0]);
    const void* init = find_witness(conformance, &g_app_init_requirement);
    const void* body = find_witness(conformance, &g_app_body_requirement);
    if (init == nullptr || body == nullptr) {
        engine_log("STOP: App conformance has no init or body witness");
        abort();
    }
    auto** full_metadata = static_cast<void**>(metadata);
    const auto* value_witnesses = static_cast<const char*>(full_metadata[-1]);
    size_t app_size;
    memcpy(&app_size, value_witnesses + 0x40, sizeof app_size);

    void* app = calloc(1, app_size < 16 ? 16 : app_size);
    engine_log("GUEST: calling App.init(), value size %zu", app_size);
    rt_call_witness(init, app, metadata, metadata, witness_table);
    engine_log("GUEST: App.init() returned");

    void* scene = calloc(1, kBodyBufferSize);
    engine_log("GUEST: calling App.body");
    rt_call_witness(body, scene, app, metadata, witness_table);
    engine_log("GUEST: App.body returned");
    swiftui_run_scene(scene);
}

uint64_t* empty_set_singleton() { return g_empty_set; }

void* swift_runtime_witness_table(const char* conformance, void* type, void* arguments) { return rt_swift_getWitnessTable(conformance, type, arguments); }

namespace {

struct DispatchThunk {
    const char* requirement;
    size_t table_register;
};

std::map<std::string, const void*>& foreign_witnesses() {
    static std::map<std::string, const void*> witnesses;
    return witnesses;
}

std::map<const void*, const char*>& static_witness_tables() {
    static std::map<const void*, const char*> tables;
    return tables;
}

const void* find_witness_by_name(const char* conformance, const char* requirement) {
    const auto witnesses = resilient_witnesses(conformance);
    for (uint32_t i = 0; i < witnesses.count; ++i) {
        const char* entry = witnesses.entries + i * 8;
        const char* name = unresolved_symbol_at(reinterpret_cast<uintptr_t>(relative_indirectable(entry)));
        if (name != nullptr && strcmp(name, requirement) == 0) return relative_direct(entry + 4);
    }
    return nullptr;
}

constexpr size_t kForeignWitnessTableBytes = kForeignWitnessTableWords * sizeof(void*);

extern "C" void rt_protocol_dispatch();

void* make_static_witness_table(const char* symbol) {
    auto* table = calloc(1, kForeignWitnessTableBytes);
    static_witness_tables()[table] = strdup(symbol);
    return table;
}

}  // namespace

extern "C" const void* rt_dispatch_lookup(const DispatchThunk* thunk, void** table) {
    const char* conformance_name = nullptr;
    if (const auto found = static_witness_tables().find(table); found != static_witness_tables().end()) conformance_name = found->second;
    else conformance_name = unresolved_symbol_at(reinterpret_cast<uintptr_t>(table[0]));
    if (conformance_name != nullptr) {
        const std::string key = std::string(conformance_name) + "|" + thunk->requirement;
        const auto found = foreign_witnesses().find(key);
        if (found == foreign_witnesses().end()) {
            engine_log("STOP: no framework witness for %s", key.c_str());
            abort();
        }
        return found->second;
    }
    const void* witness = find_witness_by_name(static_cast<const char*>(table[0]), thunk->requirement);
    if (witness == nullptr) {
        engine_log("STOP: app conformance has no witness for %s", thunk->requirement);
        abort();
    }
    return witness;
}

void register_foreign_witness(const char* conformance_symbol, const char* requirement_symbol, const void* implementation) {
    foreign_witnesses()[std::string(conformance_symbol) + "|" + requirement_symbol] = implementation;
}

// x16 holds the thunk; the witness table is in whichever argument register its signature uses.
__asm__(
    ".text\n"
    ".globl rt_protocol_dispatch\n"
    "rt_protocol_dispatch:\n"
    "  stp x29, x30, [sp, #-208]!\n"
    "  mov x29, sp\n"
    "  stp x0, x1, [sp, #16]\n"
    "  stp x2, x3, [sp, #32]\n"
    "  stp x4, x5, [sp, #48]\n"
    "  stp x6, x7, [sp, #64]\n"
    "  str x8, [sp, #80]\n"
    "  stp d0, d1, [sp, #96]\n"
    "  stp d2, d3, [sp, #112]\n"
    "  stp d4, d5, [sp, #128]\n"
    "  stp d6, d7, [sp, #144]\n"
    "  mov x0, x16\n"
    "  ldr x17, [x16, #8]\n"
    "  add x17, sp, x17, lsl #3\n"
    "  ldr x1, [x17, #16]\n"
    "  bl rt_dispatch_lookup\n"
    "  mov x16, x0\n"
    "  ldp x0, x1, [sp, #16]\n"
    "  ldp x2, x3, [sp, #32]\n"
    "  ldp x4, x5, [sp, #48]\n"
    "  ldp x6, x7, [sp, #64]\n"
    "  ldr x8, [sp, #80]\n"
    "  ldp d0, d1, [sp, #96]\n"
    "  ldp d2, d3, [sp, #112]\n"
    "  ldp d4, d5, [sp, #128]\n"
    "  ldp d6, d7, [sp, #144]\n"
    "  ldp x29, x30, [sp], #208\n"
    "  br x16\n");

void* swift_runtime_symbol(std::string_view name) {
#define SYM(symbol, target) {symbol, reinterpret_cast<void*>(target)}
#define THUNK_SYMS(reg)                                  \
    SYM("swift_retain_" #reg, rt_retain_##reg),          \
    SYM("swift_release_" #reg, rt_release_##reg),        \
    SYM("objc_retain_" #reg, rt_retain_##reg),           \
    SYM("objc_release_" #reg, rt_release_##reg)
    static const std::unordered_map<std::string_view, void*> table = {
        THUNK_SYMS(x1), THUNK_SYMS(x2), THUNK_SYMS(x8), THUNK_SYMS(x9), THUNK_SYMS(x10),
        THUNK_SYMS(x19), THUNK_SYMS(x20), THUNK_SYMS(x21), THUNK_SYMS(x22), THUNK_SYMS(x23),
        THUNK_SYMS(x24), THUNK_SYMS(x25), THUNK_SYMS(x26), THUNK_SYMS(x27), THUNK_SYMS(x28),
        SYM("swift_retain", retain_object),
        SYM("swift_release", release_object),
        SYM("objc_retain", retain_object),
        SYM("objc_release", release_object),
        SYM("objc_retainAutoreleasedReturnValue", retain_object),
        SYM("swift_unknownObjectRetain", retain_object),
        SYM("swift_unknownObjectRelease", release_object),
        SYM("swift_errorRelease", release_object),
        SYM("swift_retain_n", rt_swift_retain_n),
        SYM("swift_bridgeObjectRetain", rt_swift_bridgeObjectRetain),
        SYM("swift_bridgeObjectRelease", rt_swift_bridgeObjectRelease),
        SYM("swift_bridgeObjectRetain_n", rt_swift_bridgeObjectRetain_n),
        SYM("swift_bridgeObjectRelease_n", rt_swift_bridgeObjectRelease_n),
        SYM("swift_isUniquelyReferenced_nonNull_native", rt_swift_isUniquelyReferenced_nonNull_native),
        SYM("swift_allocObject", rt_swift_allocObject),
        SYM("swift_initStackObject", rt_swift_initStackObject),
        SYM("swift_deallocObject", rt_swift_deallocObject),
        SYM("swift_deallocClassInstance", rt_swift_deallocClassInstance),
        SYM("swift_setDeallocating", rt_swift_setDeallocating),
        SYM("swift_slowAlloc", +[](size_t size, size_t) { return malloc(size); }),
        SYM("swift_slowDealloc", +[](void* pointer, size_t, size_t) { free(pointer); }),
        SYM("swift_getWitnessTable", rt_swift_getWitnessTable),
        SYM("swift_once", rt_once),
        SYM("dispatch_once_f", rt_once),
        SYM("objc_opt_self", rt_identity),
        SYM("swift_getObjCClassMetadata", rt_identity),
        SYM("swift_getObjCClassFromMetadata", rt_identity),
        SYM("swift_beginAccess", +[](void*, void*, uintptr_t, void*) {}),
        SYM("swift_endAccess", +[](void*) {}),
        SYM("_swiftImmortalRefCount", kImmortal),
        SYM("_swiftEmptyArrayStorage", g_empty_array),
        SYM("_swiftEmptyDictionarySingleton", g_empty_dictionary),
        SYM("_swiftEmptySetSingleton", g_empty_set),
        {"$sytWV", const_cast<ValueWitnessTable*>(&kEmptyWitnesses)},
        {"$sBi32_WV", const_cast<ValueWitnessTable*>(&kInt32Witnesses)},
        {"$sBi64_WV", const_cast<ValueWitnessTable*>(&kInt64Witnesses)},
        {"$sBoWV", const_cast<ValueWitnessTable*>(&kNativeObjectWitnesses)},
        {"$sBOWV", const_cast<ValueWitnessTable*>(&kNativeObjectWitnesses)},
        {"$sBbWV", const_cast<ValueWitnessTable*>(&kBridgeObjectWitnesses)},
        SYM("$s7SwiftUI3AppPAAE4mainyyFZ", rt_swiftui_app_main_entry),
        SYM("$s7SwiftUI3AppPxycfCTq", &g_app_init_requirement),
        SYM("$s7SwiftUI3AppP4body4BodyQzvgTq", &g_app_body_requirement),
    };
#undef THUNK_SYMS
#undef SYM
    if (const auto found = table.find(name); found != table.end()) return found->second;
    // Static witness tables of framework conformances: the guest passes them around, only thunks use them.
    if (name.size() > 2 && name.substr(name.size() - 2) == "WP") {
        return make_static_witness_table(("_" + std::string(name)).c_str());
    }
    // Dispatch thunks: the witness table register follows from each requirement's signature.
    static const std::unordered_map<std::string_view, size_t> thunks = {
        {"$ss23CustomStringConvertibleP11descriptionSSvgTj", 1},
        {"$sScA15unownedExecutorScevgTj", 1},
    };
    if (const auto thunk = thunks.find(name); thunk != thunks.end()) {
        std::string requirement = "_" + std::string(name);
        requirement.back() = 'q';
        auto* record = new DispatchThunk{strdup(requirement.c_str()), thunk->second};
        return make_code_stub(reinterpret_cast<void*>(rt_protocol_dispatch), record);
    }
    return nullptr;
}
