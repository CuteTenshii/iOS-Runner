#include "swiftui_shim.h"

#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <new>
#include <unordered_map>

#include "concurrency_shim.h"
#include "engine.h"
#include "foundation_shim.h"
#include "localization.h"
#include "macho.h"
#include "swift_runtime.h"
#include "type_system.h"

#define SWIFTCC __attribute__((swiftcall))
#define SWIFT_OUT __attribute__((swift_indirect_result))

namespace {

constexpr size_t kBodyBufferSize = 64 * 1024;
constexpr uint32_t kFlagNonPod = 0x10000;
const MachSection* g_view_conformances = nullptr;

void* rt_swift_getWitnessTable_for_view(const char* conformance, void* type) {
    return swift_runtime_witness_table(conformance, type, static_cast<void**>(type) + 2);
}
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

struct ChildValue {
    void* value;
    void* type;
};

enum class NodeKind : uint32_t { TabView, Tab };

// Runtime object behind a resilient view: its properties plus already-evaluated child values.
struct ViewNode {
    HeapHeader header;
    NodeKind kind;
    std::string* title;
    std::string* system_image;
    std::vector<ChildValue>* children;
};

void destroy_node(HeapHeader* object) {
    auto* node = reinterpret_cast<ViewNode*>(object);
    delete node->title;
    delete node->system_image;
    for (const auto& child : *node->children) {
        witnesses_of(child.type)->destroy(child.value, child.type);
        free(child.value);
    }
    delete node->children;
    free(node);
}

const BoxMetadata kNodeMetadata = {destroy_node, nullptr, 0};

ViewNode* new_node(NodeKind kind) {
    auto* node = static_cast<ViewNode*>(calloc(1, sizeof(ViewNode)));
    node->header.metadata = const_cast<uint64_t*>(&kNodeMetadata.kind);
    new (&node->header.references) std::atomic<uint64_t>(1);
    node->kind = kind;
    node->title = new std::string();
    node->system_image = new std::string();
    node->children = new std::vector<ChildValue>();
    return node;
}

// Runs a @ViewBuilder closure and keeps its result as a child of `node`.
void evaluate_child(ViewNode* node, const void* closure, void* context, void* type) {
    const size_t size = witnesses_of(type)->size;
    void* value = calloc(1, size == 0 ? 1 : size);
    rt_call_witness(closure, value, context, nullptr, nullptr);
    node->children->push_back({value, type});
}

SWIFTCC void tab_view_init(SWIFT_OUT ViewNode** out, const void* closure, void* context, void* content_type, void*) {
    auto* node = new_node(NodeKind::TabView);
    evaluate_child(node, closure, context, content_type);
    *out = node;
}

SWIFTCC void tab_init(SWIFT_OUT ViewNode** out, LocalizedBox** title, SwiftString system_image, const void* closure, void* context, void* content_type, void*) {
    auto* node = new_node(NodeKind::Tab);
    *node->title = localized_text(*title);
    *node->system_image = swift_string_to_utf8(system_image);
    evaluate_child(node, closure, context, content_type);
    *out = node;
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

// Optional doubles travel in integer registers, so these words hold raw bit patterns.
struct FrameLayout {
    uint64_t width;
    uint64_t width_is_nil;
    uint64_t height;
    uint64_t height_is_nil;
    uint64_t horizontal;
    uint64_t vertical;
};

SWIFTCC void frame_layout_init(SWIFT_OUT FrameLayout* out, uint64_t width, uint64_t width_nil, uint64_t height, uint64_t height_nil, uint64_t horizontal, uint64_t vertical) {
    *out = {width, width_nil, height, height_nil, horizontal, vertical};
}

// Text: words 0-1 hold the resolved string, and the last word packs font and colour as an immediate.
struct TextValue {
    uint64_t text_bits;
    uint64_t text_object;
    uint64_t kind;
    uint64_t style;
};

constexpr uint64_t kTextFontMask = 0x3ffff;
constexpr unsigned kTextColorShift = 24;
constexpr uint64_t kTextColorMask = 0xffULL << kTextColorShift;

TextValue make_text(const std::string& text) {
    const SwiftString value = swift_string_from_utf8(text);
    return {value.count_and_flags, value.object, 0, kImmediate};
}

struct LocalizedKey {
    SwiftString key;
    uint64_t has_formatting;
    LocalizedBox* interpolation;
};

SWIFTCC LocalizedKey localized_key_literal(SwiftString literal) { return {literal, 0, nullptr}; }

SWIFTCC TextValue text_from_key(SwiftString key, uint64_t, LocalizedBox* interpolation) {
    if (interpolation != nullptr) return make_text(localized_text(interpolation));
    return make_text(localization_lookup(swift_string_to_utf8(key)));
}

SWIFTCC TextValue text_from_string(const SwiftString* value, void*, void*) { return make_text(swift_string_to_utf8(*value)); }
SWIFTCC TextValue text_from_resource(LocalizedBox** resource) { return make_text(localized_text(*resource)); }

SWIFTCC TextValue text_font(uint64_t font, TextValue text) {
    text.style = (text.style & ~kTextFontMask) | (font & kTextFontMask);
    return text;
}

SWIFTCC TextValue text_foreground_style(const uint32_t* style, TextValue text, void*, void*) {
    text.style = (text.style & ~kTextColorMask) | (static_cast<uint64_t>(*style & 0xff) << kTextColorShift);
    return text;
}

struct EdgeInsetsValue {
    double top;
    double leading;
    double bottom;
    double trailing;
};

SWIFTCC EdgeInsetsValue edge_insets_all(double value) { return {value, value, value, value}; }

const char* node_kind_name(NodeKind kind) {
    switch (kind) {
        case NodeKind::TabView: return "TabView";
        case NodeKind::Tab: return "Tab";
    }
    return "?";
}

constexpr const char* kViewProtocolSymbol = "_$s7SwiftUI4ViewMp";
constexpr size_t kConformanceTypeOffset = 4;
constexpr size_t kConformanceFlagsOffset = 12;
constexpr uint32_t kTypeReferenceKindMask = 0x7u << 3;
constexpr uint32_t kTypeReferenceIndirect = 0x1u << 3;
constexpr size_t kMetadataDescriptorWord = 1;
constexpr size_t kMetadataArgumentsWord = 2;

const void* relative_pointer(const char* field) {
    int32_t offset;
    memcpy(&offset, field, sizeof offset);
    return offset == 0 ? nullptr : field + offset;
}

const void* indirectable_pointer(const char* field) {
    int32_t offset;
    memcpy(&offset, field, sizeof offset);
    if (offset == 0) return nullptr;
    const char* target = field + (offset & ~1);
    return offset & 1 ? *reinterpret_cast<const void* const*>(target) : target;
}

// Finds an app type's View conformance in the app's own conformance list.
const char* find_view_conformance(const void* type) {
    const void* descriptor = static_cast<void* const*>(type)[kMetadataDescriptorWord];
    const MachSection* section = g_view_conformances;
    if (section == nullptr) return nullptr;
    for (size_t i = 0; i + sizeof(int32_t) <= section->size; i += sizeof(int32_t)) {
        const auto* conformance = static_cast<const char*>(relative_pointer(reinterpret_cast<const char*>(section->address + i)));
        const char* protocol = unresolved_symbol_at(reinterpret_cast<uintptr_t>(indirectable_pointer(conformance)));
        if (protocol == nullptr || strcmp(protocol, kViewProtocolSymbol) != 0) continue;
        uint32_t flags;
        memcpy(&flags, conformance + kConformanceFlagsOffset, sizeof flags);
        const void* conforming;
        if ((flags & kTypeReferenceKindMask) == kTypeReferenceIndirect) {
            conforming = indirectable_pointer(conformance + kConformanceTypeOffset);
            const char* raw = conformance + kConformanceTypeOffset;
            int32_t offset;
            memcpy(&offset, raw, sizeof offset);
            conforming = *reinterpret_cast<const void* const*>(raw + offset);
        } else {
            conforming = relative_pointer(conformance + kConformanceTypeOffset);
        }
        if (conforming == descriptor) return conformance;
    }
    return nullptr;
}

void walk_view(void* value, void* type, int depth) {
    if (depth > kMaximumDepth) {
        engine_log("STOP: view tree deeper than %d levels", kMaximumDepth);
        abort();
    }
    if (const char* name = foreign_type_name(type)) {
        const auto* witnesses = witnesses_of(type);
        const bool reference_node = witnesses->size == sizeof(void*) && (witnesses->flags & kFlagNonPod) != 0;
        if (reference_node) {
            auto* node = *static_cast<ViewNode**>(value);
            if (node != nullptr && node->header.metadata == &kNodeMetadata.kind) {
                engine_log("VIEW: %*s%s \"%s\" %s", depth * 2, "", node_kind_name(node->kind), node->title->c_str(), node->system_image->c_str());
                for (const auto& child : *node->children) walk_view(child.value, child.type, depth + 1);
                return;
            }
        }
        engine_log("VIEW: %*s%s (no renderer yet)", depth * 2, "", name);
        return;
    }
    const char* conformance = find_view_conformance(type);
    if (conformance == nullptr) {
        engine_log("VIEW: %*s(type %p has no View conformance, kind %llx)", depth * 2, "", type, static_cast<unsigned long long>(*static_cast<uint64_t*>(type)));
        return;
    }
    const void* body = find_witness(conformance, &g_view_body_requirement);
    const void* body_type_name = find_witness(conformance, &g_view_body_type_requirement);
    if (body == nullptr || body_type_name == nullptr) {
        engine_log("STOP: a view conformance has no body witness");
        abort();
    }
    engine_log("VIEW: %*sapp view", depth * 2, "");
    void* const* generics = static_cast<void* const*>(type) + kMetadataArgumentsWord;
    // The low bit of an associated type witness marks it as a mangled name rather than an accessor.
    const auto* body_name = reinterpret_cast<const char*>(reinterpret_cast<uintptr_t>(body_type_name) & ~static_cast<uintptr_t>(1));
    void* body_metadata = resolve_mangled_type(body_name, generics);
    const size_t size = witnesses_of(body_metadata)->size;
    void* output = calloc(1, size == 0 ? 1 : size);
    auto** table = static_cast<void**>(rt_swift_getWitnessTable_for_view(conformance, type));
    rt_call_witness(body, output, value, type, table);
    walk_view(output, body_metadata, depth + 1);
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

void swiftui_attach_image(const MachImage& image) { g_view_conformances = image.section("__TEXT", "__swift5_proto"); }

void swiftui_run_scene(void* scene) {
    auto* box = *static_cast<ViewBox**>(scene);
    void* content = calloc(1, kBodyBufferSize);
    engine_log("SCENE: running WindowGroup content");
    rt_call_witness(box->closure, content, box->closure_context, nullptr, nullptr);
    walk_view(content, box->content_type, 0);
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
        {"$s7SwiftUI12_FrameLayoutV5width6height9alignmentAC12CoreGraphics7CGFloatVSg_AjA9AlignmentVtcfC", reinterpret_cast<void*>(frame_layout_init)},
        {"$s7SwiftUI10EdgeInsetsV4_allAC12CoreGraphics7CGFloatV_tcfC", reinterpret_cast<void*>(edge_insets_all)},
        {"$s7SwiftUI18LocalizedStringKeyV13stringLiteralACSS_tcfC", reinterpret_cast<void*>(localized_key_literal)},
        {"$s7SwiftUI4TextV_9tableName6bundle7commentAcA18LocalizedStringKeyV_SSSgSo8NSBundleCSgs06StaticI0VSgtcfC", reinterpret_cast<void*>(text_from_key)},
        {"$s7SwiftUI4TextVyACxcSyRzlufC", reinterpret_cast<void*>(text_from_string)},
        {"$s7SwiftUI4TextVyAC10Foundation23LocalizedStringResourceVcfC", reinterpret_cast<void*>(text_from_resource)},
        {"$s7SwiftUI4TextV4fontyAcA4FontVSgF", reinterpret_cast<void*>(text_font)},
        {"$s7SwiftUI4TextV15foregroundStyleyACxAA05ShapeE0RzlF", reinterpret_cast<void*>(text_foreground_style)},
        {"$s7SwiftUI7TabViewV7contentACys5NeverOAA0C14ContentBuilderV0G0VyAF_qd__GGqd__yXE_tcAFRszAKRs_AA0cG0Rd__lufC", reinterpret_cast<void*>(tab_view_init)},
        {"$s7SwiftUI3TabVAAs5NeverORszAA4ViewR_AaFR0_rlE_11systemImage7contentACyAEq_AA07DefaultC5LabelVG10Foundation23LocalizedStringResourceV_SSq_yXEtcAJRs0_rlufC", reinterpret_cast<void*>(tab_init)},
        {"$s7SwiftUI4ViewP4body4BodyQzvgTq", &g_view_body_requirement},
        {"$s4Body7SwiftUI4ViewPTl", &g_view_body_type_requirement},
    };
    const auto found = table.find(name);
    return found == table.end() ? nullptr : found->second;
}
