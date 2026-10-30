#include "swiftui_shim.h"

#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <new>
#include <unordered_map>

#include "concurrency_shim.h"
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

// Framework constants are tagged immediates: the high bit makes retain and release ignore them.
constexpr uint64_t kImmediate = 1ULL << 63;
constexpr uint64_t kFontBold = 1ULL << 16;
constexpr uint64_t kFontMonospaced = 1ULL << 17;

enum ConstantId : uint64_t {
    kFontBody = 1, kFontCaption, kFontCaption2, kFontFootnote,
    kColorAccent, kColorRed, kColorGreen, kColorSecondary,
    kStyleSecondary, kStyleQuaternary,
    kAnimationDefault, kButtonRoleDestructive, kToolbarTopBarTrailing,
    kAlignLeading, kAlignTrailing, kAlignCenter, kAlignBottom,
};

template <uint64_t Value>
SWIFTCC uint64_t constant_getter() { return kImmediate | Value; }

struct AlignmentPair {
    uint64_t horizontal;
    uint64_t vertical;
};

template <uint64_t Horizontal, uint64_t Vertical>
SWIFTCC AlignmentPair alignment_getter() { return {kImmediate | Horizontal, kImmediate | Vertical}; }

template <uint8_t Mask>
SWIFTCC uint8_t edge_set_getter() { return Mask; }

struct UnitPointValue {
    double x;
    double y;
};

SWIFTCC UnitPointValue unit_point_center() { return {0.5, 0.5}; }

SWIFTCC uint64_t font_bold(__attribute__((swift_context)) void* font) { return reinterpret_cast<uint64_t>(font) | kFontBold; }
SWIFTCC uint64_t font_monospaced(__attribute__((swift_context)) void* font) { return reinterpret_cast<uint64_t>(font) | kFontMonospaced; }

std::atomic<bool> g_tree_dirty{true};

SWIFTCC void registrar_init(SWIFT_OUT uint64_t* out) { *out = kImmediate; }
SWIFTCC void registrar_access() {}
SWIFTCC void registrar_mark_dirty() { g_tree_dirty.store(true, std::memory_order_relaxed); }

extern "C" void rt_registrar_with_mutation();

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

// withMutation(of:keyPath:_:): the closure is in x2/x3; x8 and the error register x21 pass straight through.
__asm__(
    ".text\n"
    ".globl rt_registrar_with_mutation\n"
    "rt_registrar_with_mutation:\n"
    "  stp x29, x30, [sp, #-32]!\n"
    "  mov x29, sp\n"
    "  str x20, [sp, #16]\n"
    "  mov x20, x3\n"
    "  blr x2\n"
    "  ldr x20, [sp, #16]\n"
    "  ldp x29, x30, [sp], #32\n"
    "  b rt_registrar_mark_dirty_c\n");

extern "C" void rt_registrar_mark_dirty_c() { g_tree_dirty.store(true, std::memory_order_relaxed); }

void swiftui_run_scene(void* scene) {
    auto* box = *static_cast<ViewBox**>(scene);
    void* content = calloc(1, kBodyBufferSize);
    engine_log("SCENE: running WindowGroup content");
    rt_call_witness(box->closure, content, box->closure_context, nullptr, nullptr);
    walk_view(content, box->content_type, box->content_witnesses, 0);
    concurrency_drain();
    engine_log("SCENE: finished walking");
}

void* swiftui_symbol(std::string_view name) {
    static const std::unordered_map<std::string_view, void*> table = {
        {"$s7SwiftUI11WindowGroupV2id5title11lazyContentACyxGSSSg_AA4TextVSgxyctcfC", reinterpret_cast<void*>(window_group_init)},
        {"$s7SwiftUI12SceneBuilderV10buildBlockyxxAA0C0RzlFZ", reinterpret_cast<void*>(scene_build_block)},
        {"$s7SwiftUI9LazyStateV12wrappedValueACyxGxyXA_tcfC", reinterpret_cast<void*>(lazy_state_init)},
        {"$s7SwiftUI9LazyStateV12wrappedValuexvg", reinterpret_cast<void*>(lazy_state_wrapped_value_get)},
        {"$s7SwiftUI4FontV4bodyACvgZ", reinterpret_cast<void*>(constant_getter<kFontBody>)},
        {"$s7SwiftUI4FontV7captionACvgZ", reinterpret_cast<void*>(constant_getter<kFontCaption>)},
        {"$s7SwiftUI4FontV8caption2ACvgZ", reinterpret_cast<void*>(constant_getter<kFontCaption2>)},
        {"$s7SwiftUI4FontV8footnoteACvgZ", reinterpret_cast<void*>(constant_getter<kFontFootnote>)},
        {"$s7SwiftUI4FontV4boldACyF", reinterpret_cast<void*>(font_bold)},
        {"$s7SwiftUI4FontV10monospacedACyF", reinterpret_cast<void*>(font_monospaced)},
        {"$s7SwiftUI5ColorV06accentC0ACvgZ", reinterpret_cast<void*>(constant_getter<kColorAccent>)},
        {"$s7SwiftUI5ColorV3redACvgZ", reinterpret_cast<void*>(constant_getter<kColorRed>)},
        {"$s7SwiftUI5ColorV5greenACvgZ", reinterpret_cast<void*>(constant_getter<kColorGreen>)},
        {"$s7SwiftUI5ColorV9secondaryACvgZ", reinterpret_cast<void*>(constant_getter<kColorSecondary>)},
        {"$s7SwiftUI22HierarchicalShapeStyleV9secondaryACvgZ", reinterpret_cast<void*>(constant_getter<kStyleSecondary>)},
        {"$s7SwiftUI22HierarchicalShapeStyleV10quaternaryACvgZ", reinterpret_cast<void*>(constant_getter<kStyleQuaternary>)},
        {"$s7SwiftUI9AnimationV7defaultACvgZ", reinterpret_cast<void*>(constant_getter<kAnimationDefault>)},
        {"$s7SwiftUI10ButtonRoleV11destructiveACvgZ", reinterpret_cast<void*>(constant_getter<kButtonRoleDestructive>)},
        {"$s7SwiftUI20ToolbarItemPlacementV14topBarTrailingACvgZ", reinterpret_cast<void*>(constant_getter<kToolbarTopBarTrailing>)},
        {"$s7SwiftUI19HorizontalAlignmentV7leadingACvgZ", reinterpret_cast<void*>(constant_getter<kAlignLeading>)},
        {"$s7SwiftUI17VerticalAlignmentV6centerACvgZ", reinterpret_cast<void*>(constant_getter<kAlignCenter>)},
        {"$s7SwiftUI9AlignmentV6centerACvgZ", reinterpret_cast<void*>(alignment_getter<kAlignCenter, kAlignCenter>)},
        {"$s7SwiftUI9AlignmentV14bottomTrailingACvgZ", reinterpret_cast<void*>(alignment_getter<kAlignTrailing, kAlignBottom>)},
        {"$s7SwiftUI4EdgeO3SetV3allAEvgZ", reinterpret_cast<void*>(edge_set_getter<0x0f>)},
        {"$s7SwiftUI4EdgeO3SetV6bottomAEvgZ", reinterpret_cast<void*>(edge_set_getter<0x04>)},
        {"$s7SwiftUI4EdgeO3SetV8trailingAEvgZ", reinterpret_cast<void*>(edge_set_getter<0x08>)},
        {"$s7SwiftUI9UnitPointV6centerACvgZ", reinterpret_cast<void*>(unit_point_center)},
        {"$s11Observation0A9RegistrarVACycfC", reinterpret_cast<void*>(registrar_init)},
        {"$s11Observation0A9RegistrarV6access_7keyPathyx_s03KeyE0Cyxq_GtAA10ObservableRzr0_lF", reinterpret_cast<void*>(registrar_access)},
        {"$s11Observation0A9RegistrarV7willSet_7keyPathyx_s03KeyF0Cyxq_GtAA10ObservableRzr0_lF", reinterpret_cast<void*>(registrar_access)},
        {"$s11Observation0A9RegistrarV6didSet_7keyPathyx_s03KeyF0Cyxq_GtAA10ObservableRzr0_lF", reinterpret_cast<void*>(registrar_mark_dirty)},
        {"$s11Observation0A9RegistrarV12withMutation2of7keyPath_q0_x_s03KeyG0Cyxq_Gq0_yKXEtKAA10ObservableRzr1_lF", reinterpret_cast<void*>(rt_registrar_with_mutation)},
        {"$s7SwiftUI4ViewP4body4BodyQzvgTq", &g_view_body_requirement},
        {"$s4Body7SwiftUI4ViewPTl", &g_view_body_type_requirement},
    };
    const auto found = table.find(name);
    return found == table.end() ? nullptr : found->second;
}
