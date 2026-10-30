#include "swiftui_shim.h"

#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <new>
#include <unordered_map>

#include "engine.h"
#include "swift_runtime.h"
#include "type_system.h"

#define SWIFTCC __attribute__((swiftcall))
#define SWIFT_OUT __attribute__((swift_indirect_result))

namespace {

constexpr size_t kBodyBufferSize = 64 * 1024;
constexpr int kMaximumDepth = 64;

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

const ValueWitnessTable* witnesses_of(const void* metadata) {
    return static_cast<const ValueWitnessTable*>(static_cast<void* const*>(metadata)[-1]);
}

struct HeapHeader {
    void* metadata;
    std::atomic<uint64_t> references;
};

// Runtime-owned object behind a framework value; the guest only sees it as one retained reference.
struct ViewBox {
    HeapHeader header;
    const void* closure;
    void* closure_context;
    void* content_type;
    void** content_witnesses;
};

void destroy_box(HeapHeader* object) { free(object); }

struct BoxMetadata {
    void (*destroyer)(HeapHeader*);
    const void* witnesses;
    uint64_t kind;
};

const BoxMetadata kBoxMetadata = {destroy_box, nullptr, 0};

ViewBox* new_box() {
    auto* box = static_cast<ViewBox*>(calloc(1, sizeof(ViewBox)));
    box->header.metadata = const_cast<uint64_t*>(&kBoxMetadata.kind);
    new (&box->header.references) std::atomic<uint64_t>(1);
    return box;
}

char g_view_body_requirement;
char g_view_body_type_requirement;

SWIFTCC void window_group_init(SWIFT_OUT void** out, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t,
                               const void* closure, void* context, void* content_type, void** content_witnesses) {
    auto* box = new_box();
    box->closure = closure;
    box->closure_context = context;
    box->content_type = content_type;
    box->content_witnesses = content_witnesses;
    *out = box;
}

SWIFTCC void scene_build_block(SWIFT_OUT void* out, void* value, void* type, void*) {
    witnesses_of(type)->initialize_with_copy(out, value, type);
}

// Backing store of @State: the initial value is computed on first read, as LazyState promises.
struct StateBox {
    HeapHeader header;
    void* type;
    const void* initializer;
    void* initializer_context;
    void* value;
};

const BoxMetadata kStateMetadata = {destroy_box, nullptr, 0};

void* state_value(StateBox* state) {
    if (state->value == nullptr) {
        const auto* witnesses = witnesses_of(state->type);
        state->value = calloc(1, witnesses->size == 0 ? 1 : witnesses->size);
        rt_call_witness(state->initializer, state->value, state->initializer_context, nullptr, nullptr);
    }
    return state->value;
}

SWIFTCC void lazy_state_init(SWIFT_OUT StateBox** out, const void* initializer, void* context, void* type) {
    auto* state = static_cast<StateBox*>(calloc(1, sizeof(StateBox)));
    state->header.metadata = const_cast<uint64_t*>(&kStateMetadata.kind);
    new (&state->header.references) std::atomic<uint64_t>(1);
    state->type = type;
    state->initializer = initializer;
    state->initializer_context = context;
    *out = state;
}

SWIFTCC void lazy_state_wrapped_value_get(SWIFT_OUT void* out, __attribute__((swift_context)) StateBox** self) {
    auto* state = *self;
    witnesses_of(state->type)->initialize_with_copy(out, state_value(state), state->type);
}

void walk_view(void* value, void* type, void** witnesses, int depth) {
    if (depth > kMaximumDepth) {
        engine_log("STOP: view tree deeper than %d levels", kMaximumDepth);
        abort();
    }
    if (const char* name = foreign_type_name(type)) {
        engine_log("VIEW: %*s%s", depth * 2, "", name);
        return;
    }
    const auto* conformance = static_cast<const char*>(witnesses[0]);
    const void* body = find_witness(conformance, &g_view_body_requirement);
    const void* body_type_name = find_witness(conformance, &g_view_body_type_requirement);
    if (body == nullptr || body_type_name == nullptr) {
        engine_log("STOP: a view conformance has no body witness");
        abort();
    }
    engine_log("VIEW: %*sapp view %p, calling body", depth * 2, "", type);
    void* output = calloc(1, kBodyBufferSize);
    rt_call_witness(body, output, value, type, witnesses);
    void* body_metadata = resolve_mangled_type(static_cast<const char*>(body_type_name), nullptr);
    engine_log("VIEW: %*sbody returned type %s", depth * 2, "", foreign_type_name(body_metadata) ? foreign_type_name(body_metadata) : "(app)");
}

}  // namespace

void swiftui_run_scene(void* scene) {
    auto* box = *static_cast<ViewBox**>(scene);
    void* content = calloc(1, kBodyBufferSize);
    engine_log("SCENE: running WindowGroup content");
    rt_call_witness(box->closure, content, box->closure_context, nullptr, nullptr);
    walk_view(content, box->content_type, box->content_witnesses, 0);
    engine_log("SCENE: finished walking");
}

void* swiftui_symbol(std::string_view name) {
    static const std::unordered_map<std::string_view, void*> table = {
        {"$s7SwiftUI11WindowGroupV2id5title11lazyContentACyxGSSSg_AA4TextVSgxyctcfC", reinterpret_cast<void*>(window_group_init)},
        {"$s7SwiftUI12SceneBuilderV10buildBlockyxxAA0C0RzlFZ", reinterpret_cast<void*>(scene_build_block)},
        {"$s7SwiftUI9LazyStateV12wrappedValueACyxGxyXA_tcfC", reinterpret_cast<void*>(lazy_state_init)},
        {"$s7SwiftUI9LazyStateV12wrappedValuexvg", reinterpret_cast<void*>(lazy_state_wrapped_value_get)},
        {"$s7SwiftUI4ViewP4body4BodyQzvgTq", &g_view_body_requirement},
        {"$s4Body7SwiftUI4ViewPTl", &g_view_body_type_requirement},
    };
    const auto found = table.find(name);
    return found == table.end() ? nullptr : found->second;
}
