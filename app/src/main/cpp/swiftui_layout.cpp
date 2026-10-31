#include "swiftui_internal.h"

namespace swiftui {

struct OptionalInt {
    int64_t value;
    bool is_none;
};

SWIFTCC OptionalInt environment_no_limit(SWIFT_CONTEXT void*) { return {0, true}; }

SWIFTCC uint8_t environment_leading_alignment(SWIFT_CONTEXT void*) { return 0; }

SWIFTCC void dismiss_action_call(SWIFT_CONTEXT void*) { dismiss_presented_sheet(); }

SWIFTCC void open_url_action_call(void** url, SWIFT_CONTEXT void*) { engine_emit("open", foundation_url_text(*url)); }

// A tab's identified view is the tab itself, kept as one reference to its node.
SWIFTCC void tab_identified_view(SWIFT_OUT void** out, void*, void*, SWIFT_CONTEXT void** self) { *out = rt_swift_retain(*self); }

// `.task`: the action's async entry point and context; a view tree starts each task once.
SWIFTCC void task_modifier_init(SWIFT_OUT ViewNode** out, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, const void* action, void* context) {
    auto* node = new_node(NodeKind::Task);
    node->action = action;
    node->action_context = rt_swift_retain(context);
    *out = node;
}

std::set<std::string>& started_tasks() {
    static std::set<std::string> started;
    return started;
}

// SwiftUI's runtime warnings go nowhere: the log object only needs to be something it can pass along.
SWIFTCC void* runtime_issues_log() { return reinterpret_cast<void*>(kImmediate); }

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

TextValue make_text(const std::string& text) {
    const SwiftString value = swift_string_from_utf8(text);
    return {value.count_and_flags, value.object, 0, kImmediate};
}

SWIFTCC LocalizedKey localized_key_literal(SwiftString literal) { return {literal, 0, nullptr}; }

SWIFTCC TextValue text_from_key(SwiftString key, uint64_t, LocalizedBox* interpolation) {
    if (interpolation != nullptr) return make_text(localized_text(interpolation));
    return make_text(localization_lookup(swift_string_to_utf8(key)));
}

SWIFTCC TextValue text_from_string(const SwiftString* value, void*, void*) { return make_text(swift_string_to_utf8(*value)); }

SWIFTCC TextValue text_from_resource(LocalizedBox** resource) { return make_text(localized_text(*resource)); }

// The compiled code owns and releases the string inline, so a modified copy needs its own reference.
void retain_text(const TextValue& text) { rt_swift_bridgeObjectRetain(reinterpret_cast<void*>(text.text_object)); }

SWIFTCC TextValue text_font(uint64_t font, TextValue text) {
    retain_text(text);
    text.style = (text.style & ~kTextFontMask) | (font & kTextFontMask);
    return text;
}

SWIFTCC TextValue text_bold(TextValue text) {
    retain_text(text);
    text.style |= kFontBold;
    return text;
}

SWIFTCC TextValue text_foreground_color(uint64_t, TextValue text) {
    retain_text(text);
    return text;
}

SWIFTCC TextValue text_font_weight(uint64_t, bool, TextValue text) {
    retain_text(text);
    text.style |= kFontBold;
    return text;
}

SWIFTCC TextValue text_foreground_style(const uint32_t* style, TextValue text, void*, void*) {
    retain_text(text);
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

constexpr const char* kViewProtocolSymbol = "_$s7SwiftUI4ViewMp";

constexpr size_t kConformanceFlagsOffset = 12;

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

// Finds an app type's conformance to a framework protocol in the app's own conformance list.
const char* find_conformance(const void* type, const char* protocol_symbol) {
    const void* descriptor = static_cast<void* const*>(type)[kMetadataDescriptorWord];
    const MachSection* section = g_view_conformances;
    if (section == nullptr) return nullptr;
    for (size_t i = 0; i + sizeof(int32_t) <= section->size; i += sizeof(int32_t)) {
        const auto* conformance = static_cast<const char*>(relative_pointer(reinterpret_cast<const char*>(section->address + i)));
        const char* protocol = unresolved_symbol_at(reinterpret_cast<uintptr_t>(indirectable_pointer(conformance)));
        if (protocol == nullptr || strcmp(protocol, protocol_symbol) != 0) continue;
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

const char* find_view_conformance(const void* type) { return find_conformance(type, kViewProtocolSymbol); }

BindingNode* g_picker_binding = nullptr;

bool is_runtime_node_type(void* type) {
    const auto* witnesses = witnesses_of(type);
    return foreign_type_name(type) != nullptr && witnesses->size == sizeof(void*) && (witnesses->flags & kFlagNonPod) != 0;
}

void add_swiftui_layout_symbols(SymbolTable& table) {
    table.insert({
        {"$s7SwiftUI17EnvironmentValuesV9lineLimitSiSgvg", reinterpret_cast<void*>(environment_no_limit)},
        {"$s7SwiftUI17EnvironmentValuesV22multilineTextAlignmentAA0fG0Ovg", reinterpret_cast<void*>(environment_leading_alignment)},
        {"$s7SwiftUI4TextV4boldACyF", reinterpret_cast<void*>(text_bold)},
        {"$s7SwiftUI4TextV15foregroundColoryAcA0E0VSgF", reinterpret_cast<void*>(text_foreground_color)},
        {"$s7SwiftUI4TextV10fontWeightyAcA4FontV0E0VSgF", reinterpret_cast<void*>(text_font_weight)},
        {"$s7SwiftUI18LocalizedStringKeyVyACSScfC", reinterpret_cast<void*>(localized_key_literal)},
        {"$s7SwiftUI12_FrameLayoutV5width6height9alignmentAC12CoreGraphics7CGFloatVSg_AjA9AlignmentVtcfC", reinterpret_cast<void*>(frame_layout_init)},
        {"$s7SwiftUI10EdgeInsetsV4_allAC12CoreGraphics7CGFloatV_tcfC", reinterpret_cast<void*>(edge_insets_all)},
        {"$s7SwiftUI18LocalizedStringKeyV13stringLiteralACSS_tcfC", reinterpret_cast<void*>(localized_key_literal)},
        {"$s7SwiftUI4TextV_9tableName6bundle7commentAcA18LocalizedStringKeyV_SSSgSo8NSBundleCSgs06StaticI0VSgtcfC", reinterpret_cast<void*>(text_from_key)},
        {"$s7SwiftUI4TextVyACxcSyRzlufC", reinterpret_cast<void*>(text_from_string)},
        {"$s7SwiftUI4TextVyAC10Foundation23LocalizedStringResourceVcfC", reinterpret_cast<void*>(text_from_resource)},
        {"$s7SwiftUI4TextV4fontyAcA4FontVSgF", reinterpret_cast<void*>(text_font)},
        {"$s7SwiftUI4TextV15foregroundStyleyACxAA05ShapeE0RzlF", reinterpret_cast<void*>(text_foreground_style)},
        {"$s7SwiftUI13DismissActionV14callAsFunctionyyF", reinterpret_cast<void*>(dismiss_action_call)},
        {"$s7SwiftUI13OpenURLActionV14callAsFunctionyy10Foundation3URLVF", reinterpret_cast<void*>(open_url_action_call)},
        {"$s7SwiftUI14_TaskModifier2V4name18executorPreference8priority6actionACSS_Sch_pSgScPyyYaYAcntcfC", reinterpret_cast<void*>(task_modifier_init)},
        {"$s7SwiftUI3LogO013runtimeIssuesC0So9OS_os_logCvgZ", reinterpret_cast<void*>(runtime_issues_log)},
    });
}

}  // namespace swiftui
