#include "swiftui_internal.h"

using namespace swiftui;

static SymbolTable all_swiftui_symbols() {
    SymbolTable all = {
        {"$s7SwiftUI9LazyStateV7StorageO5valueyAEyx_GxcAGmlFWC", const_cast<int32_t*>(&kStorageCaseValueTag)},
        {"$s7SwiftUI10ButtonRoleV6cancelACvgZ", reinterpret_cast<void*>(immediate_indirect)},
        {"$s7SwiftUI14TintShapeStyleVACycfC", reinterpret_cast<void*>(immediate_indirect)},
        {"$s7SwiftUI14PlainListStyleVACycfC", reinterpret_cast<void*>(immediate_indirect)},
        {"$s7SwiftUI16GroupedListStyleVACycfC", reinterpret_cast<void*>(immediate_indirect)},
        {"$s7SwiftUI17InlinePickerStyleVACycfC", reinterpret_cast<void*>(immediate_indirect)},
        {"$s7SwiftUI19TitleOnlyLabelStyleVACycfC", reinterpret_cast<void*>(immediate_indirect)},
        {"$s7SwiftUI20SegmentedPickerStyleVACycfC", reinterpret_cast<void*>(immediate_indirect)},
        {"$s7SwiftUI21BorderlessButtonStyleVACycfC", reinterpret_cast<void*>(immediate_indirect)},
        {"$s7SwiftUI22TitleAndIconLabelStyleVACycfC", reinterpret_cast<void*>(immediate_indirect)},
        {"$s7SwiftUI23LinearProgressViewStyleVACycfC", reinterpret_cast<void*>(immediate_indirect)},
        {"$s7SwiftUI25CircularProgressViewStyleVACycfC", reinterpret_cast<void*>(immediate_indirect)},
        {"$s7SwiftUI10EditButtonVACycfC", reinterpret_cast<void*>(immediate_indirect)},
        {"$s7SwiftUI20SearchFieldPlacementV9automaticACvgZ", reinterpret_cast<void*>(immediate_indirect)},
        {"$s7SwiftUI27ScrollDismissesKeyboardModeV13interactivelyACvgZ", reinterpret_cast<void*>(immediate_indirect)},
        {"$s7SwiftUI27TextInputAutocapitalizationV5neverACvgZ", reinterpret_cast<void*>(immediate_indirect)},
        {"$s7SwiftUI18PresentationDetentV6mediumACvgZ", reinterpret_cast<void*>(immediate_indirect)},
        {"$s7SwiftUI20ToolbarItemPlacementV18cancellationActionACvgZ", reinterpret_cast<void*>(immediate_indirect)},
        {"$s7SwiftUI20ToolbarItemPlacementV18confirmationActionACvgZ", reinterpret_cast<void*>(immediate_indirect)},
        {"$s7SwiftUI20ToolbarItemPlacementV20navigationBarLeadingACvgZ", reinterpret_cast<void*>(immediate_indirect)},
        {"$s7SwiftUI20ToolbarItemPlacementV21navigationBarTrailingACvgZ", reinterpret_cast<void*>(immediate_indirect)},
        {"$s7SwiftUI20ToolbarItemPlacementV9automaticACvgZ", reinterpret_cast<void*>(immediate_indirect)},
        {"$s7SwiftUI20ToolbarItemPlacementV9principalACvgZ", reinterpret_cast<void*>(immediate_indirect)},
        {"$s7SwiftUI20SearchFieldPlacementV30NavigationBarDrawerDisplayModeV6alwaysAEvgZ", reinterpret_cast<void*>(immediate_indirect)},
        {"$s7SwiftUI20SearchFieldPlacementV19navigationBarDrawer11displayModeA2C010Navigationgh7DisplayJ0V_tFZ", reinterpret_cast<void*>(immediate_indirect)},
        {"$s7SwiftUI8GridItemV_7spacing9alignmentA2C4SizeO_12CoreGraphics7CGFloatVSgAA9AlignmentVSgtcfC", reinterpret_cast<void*>(immediate_indirect)},
        {"$s7SwiftUI14LinearGradientV8gradient10startPoint03endG0AcA0D0V_AA04UnitG0VAJtcfC", reinterpret_cast<void*>(immediate_indirect)},
        {"$s7SwiftUI17ContentTransitionV11numericText10countsDownACSb_tFZ", reinterpret_cast<void*>(immediate_indirect)},
        {"$s7SwiftUI17ContentTransitionV7opacityACvgZ", reinterpret_cast<void*>(immediate_indirect)},
        {"$s7SwiftUI18PresentationDetentV6heightyAC12CoreGraphics7CGFloatVFZ", reinterpret_cast<void*>(immediate_indirect)},
        {"$s7SwiftUI25AnimationTimelineScheduleV15minimumInterval6pausedACSdSg_SbtcfC", reinterpret_cast<void*>(immediate_indirect)},
        {"$s7SwiftUI16PageTabViewStyleV16indexDisplayModeA2C05IndexhI0V_tcfC", reinterpret_cast<void*>(immediate_indirect)},
        {"$s7SwiftUI16PageTabViewStyleV16IndexDisplayModeV5neverAEvgZ", reinterpret_cast<void*>(immediate_indirect)},
        {"$s7SwiftUI5ImageV12ResizingModeO7stretchyA2EmFWC", const_cast<int32_t*>(&kImageResizingStretch)},
        {"$s7SwiftUI17NavigationBarItemV16TitleDisplayModeO6inlineyA2EmFWC", const_cast<int32_t*>(&kTitleDisplayInline)},
        {"$s7SwiftUI14ToolbarContentP4body4BodyQzvgTq", &g_toolbar_body_requirement},
        {"$s4Body7SwiftUI14ToolbarContentPTl", &g_toolbar_body_type_requirement},
        {"$s7SwiftUI12ViewModifierP4body7content4BodyQzAA01_cD8_ContentVyxG_tFTq", &g_modifier_body_requirement},
        {"$s4Body7SwiftUI12ViewModifierPTl", &g_modifier_body_type_requirement},
        {"$s7SwiftUI4ViewP4body4BodyQzvgTq", &g_view_body_requirement},
        {"$s4Body7SwiftUI4ViewPTl", &g_view_body_type_requirement},
    };
    add_swiftui_render_symbols(all);
    add_swiftui_views_symbols(all);
    add_swiftui_state_symbols(all);
    add_swiftui_modifiers_symbols(all);
    add_swiftui_layout_symbols(all);
    return all;
}

void* swiftui_symbol(std::string_view name) {
    static const bool registered = [] {
        register_foreign_witness("_$s7SwiftUI3TabVyxq_q0_GAA0C7ContentAASHRzAA4ViewR_AaFR0_rlMc",
                                 "_$s7SwiftUI10TabContentP15_identifiedView011_IdentifiedF0QzvgTq", reinterpret_cast<const void*>(tab_identified_view));
        constexpr const char* kFetchedSequence = "_$s7SwiftUI14FetchedResultsVyxGSTAAMc";
        constexpr const char* kFetchedCollection = "_$s7SwiftUI14FetchedResultsVyxGSlAAMc";
        register_foreign_witness(kFetchedSequence, "_$sST12makeIterator0B0QzyFTq", reinterpret_cast<const void*>(fetched_make_iterator));
        register_foreign_witness(kFetchedSequence, "_$sST22_copyToContiguousArrays0cD0Vy7ElementQzGyFTq", reinterpret_cast<const void*>(fetched_copy_to_array));
        register_foreign_witness(kFetchedSequence, "_$sST19underestimatedCountSivgTq", reinterpret_cast<const void*>(fetched_underestimated_count));
        register_foreign_witness(kFetchedCollection, "_$sSl8distance4from2toSi5IndexQz_AEtFTq", reinterpret_cast<const void*>(fetched_distance));
        register_foreign_witness(kFetchedCollection, "_$sSl5index_8offsetBy5IndexQzAD_SitFTq", reinterpret_cast<const void*>(fetched_index_offset));
        register_foreign_witness(kFetchedCollection, "_$sSl10startIndex0B0QzvgTq", reinterpret_cast<const void*>(fetched_start_index));
        register_foreign_witness(kFetchedCollection, "_$sSl8endIndex0B0QzvgTq", reinterpret_cast<const void*>(fetched_end_index));
        register_foreign_witness(kFetchedCollection, "_$sSl5countSivgTq", reinterpret_cast<const void*>(fetched_count));
        register_foreign_witness(kFetchedCollection, "_$sSl7isEmptySbvgTq", reinterpret_cast<const void*>(fetched_is_empty));
        register_foreign_witness(kFetchedCollection, "_$sSl9formIndex5aftery0B0Qzz_tFTq", reinterpret_cast<const void*>(fetched_form_index));
        register_foreign_witness(kFetchedCollection, "_$sSl5index5after5IndexQzAD_tFTq", reinterpret_cast<const void*>(fetched_index_after));
        register_foreign_witness(kFetchedCollection, "_$sSly7ElementQz5IndexQzcirTq", reinterpret_cast<const void*>(fetched_read));
        return true;
    }();
    (void)registered;
    static const SymbolTable table = all_swiftui_symbols();
    return lookup_symbol(table, name);
}
