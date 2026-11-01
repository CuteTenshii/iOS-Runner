#include "type_system_internal.h"

namespace type_system {

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
                     uint32_t extra_inhabitants, const std::vector<void*>& arguments) {
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

Metadata* bridge_type(const std::string& key, const std::vector<void*>& arguments) {
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
        {"_$s10Foundation11JSONEncoderC16OutputFormattingVMn", {kWord}},
        {"_$sSo9NSRunLoopC10FoundationE17SchedulerTimeTypeV6StrideVMn", {kWord}},
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
    if (symbol == "_$s8Dispatch0A12TimeIntervalOMn") {
        // Generic code stores the count, then injects the case into the word after it.
        Metadata* interval = named_type(symbol, kKindStruct, new Layout{LayoutKind::Pod, {}, nullptr}, 2 * kPointerSize, kPointerSize, 0);
        auto* witnesses = witnesses_of(interval);
        witnesses->get_enum_tag = +[](const void* value, void*) { return static_cast<const uint32_t*>(value)[2]; };
        witnesses->destructive_project_enum_data = +[](void*, void*) {};
        witnesses->destructive_inject_enum_tag = +[](void* value, uint32_t tag, void*) { static_cast<uint64_t*>(value)[1] = tag; };
        return interval;
    }
    if (symbol == "_$s10Foundation8CalendarV9ComponentOMn" || symbol == "_$s7Network6NWPathV6StatusOMn" || symbol == "_$s7Network11NWInterfaceV13InterfaceTypeOMn" || symbol == "_$s10Foundation20ByteCountFormatStyleV0E0OMn") {
        // A plain tag word that generic code fills through the enum witnesses.
        Metadata* component = pod_type(symbol, kPointerSize);
        auto* witnesses = witnesses_of(component);
        witnesses->get_enum_tag = +[](const void* value, void*) { return *static_cast<const uint32_t*>(value); };
        witnesses->destructive_inject_enum_tag = +[](void* value, uint32_t tag, void*) { *static_cast<uint64_t*>(value) = tag; };
        return component;
    }
    if (symbol == "_$s10Foundation4DataVMn") return two_word_type("Data", word_type(), bridge_word_type());
    if (symbol == "_$s8Dispatch0A4TimeVMn") return pod_type("DispatchTime", 8);
    if (symbol == "_$sSS10FoundationE8EncodingVMn") return pod_type("String.Encoding", 8);
    // Holds a continuation, which isn't reference counted; the arguments stay for resume to find its value type.
    // A task group is a raw pointer to storage in its creator's frame.
    if (symbol == "_$sScGMn" || symbol == "_$sScgMn") return named_type(key, kKindStruct, new Layout{LayoutKind::Pod, {}, nullptr}, kPointerSize, kPointerSize, 0, arguments);
    if (symbol == "_$sScCMn") return named_type(key, kKindStruct, new Layout{LayoutKind::Pod, {}, nullptr}, kPointerSize, kPointerSize, 0, arguments);
    // Payload-free framework enums are plain tag words; a reference's layout would read small tags as Optional.none.
    if (symbol == "_$s7SwiftUI11ColorSchemeOMn" || symbol == "_$s7SwiftUI8EditModeOMn" || symbol == "_$s7SwiftUI22UserInterfaceSizeClassOMn" || symbol == "_$s7SwiftUI10ProminenceOMn") return pod_type(symbol, 8);
    // Frozen as finished or failure(F), which is the same shape as an Optional of F.
    if (symbol == "_$s7Combine11SubscribersO10CompletionOMn" && !arguments.empty()) return optional_of(static_cast<Metadata*>(arguments[0]));
    if (symbol == "_$ss15ContinuousClockVMn") return pod_type("ContinuousClock", 0);
    if (symbol == "_$ss15ContinuousClockV7InstantVMn") return named_type("ContinuousClock.Instant", kKindStruct, new Layout{LayoutKind::Pod, {}, nullptr}, 16, 8, 0);
    if (symbol == "_$sScPMn") return pod_type("TaskPriority", 1);
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

}  // namespace type_system
