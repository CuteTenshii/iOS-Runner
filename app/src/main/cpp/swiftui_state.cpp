#include "swiftui_internal.h"

namespace swiftui {

SWIFTCC void read_finished(void*, bool) {}

// A read accessor is a yield-once coroutine: the element is borrowed from the caller's frame buffer.
SWIFTCC ReadYield fetched_read(void** frame, const int64_t* index, void*, void*, SWIFT_CONTEXT FetchedResultsBox** self) {
    frame[0] = (*(*self)->objects)[static_cast<size_t>(*index)];
    return {read_finished, frame};
}

SWIFTCC void* fetched_results_subscript(int64_t index, void*, SWIFT_CONTEXT FetchedResultsBox** self) { return rt_swift_retain((*(*self)->objects)[static_cast<size_t>(index)]); }

SWIFTCC TextValue text_bold(TextValue text);

SWIFTCC TextValue text_foreground_color(uint64_t, TextValue text);

SWIFTCC TextValue text_font_weight(uint64_t, bool, TextValue text);

SWIFTCC ViewNode* image_from_ui_image(void*) { return new_node(NodeKind::Image); }

constexpr size_t kStrokeStyleBytes = 40;

constexpr size_t kFlexFrameBytes = 112;

SWIFTCC void stroke_style_init(SWIFT_OUT char* out) { memset(out, 0, kStrokeStyleBytes); }

SWIFTCC void flex_frame_init(SWIFT_OUT char* out) { memset(out, 0, kFlexFrameBytes); }

SWIFTCC void progress_value_init(SWIFT_OUT ViewNode** out) { *out = new_node(NodeKind::ProgressView); }

SWIFTCC void empty_node_init(SWIFT_OUT ViewNode** out) { *out = new_node(NodeKind::Modifier); }

// _ConditionalContent as written by the toolbar builder: the larger branch's bytes, then the tag.
void write_conditional(char* out, void* value, void* type, void* true_type, void* false_type, uint8_t tag) {
    const size_t larger = std::max(witnesses_of(true_type)->size, witnesses_of(false_type)->size);
    witnesses_of(type)->initialize_with_copy(out, value, type);
    out[larger] = static_cast<char>(tag);
}

SWIFTCC void toolbar_build_first(SWIFT_OUT char* out, void* value, void* true_type, void* false_type) { write_conditional(out, value, true_type, true_type, false_type, 0); }

SWIFTCC void toolbar_build_second(SWIFT_OUT char* out, void* value, void* true_type, void* false_type) { write_conditional(out, value, false_type, true_type, false_type, 1); }

SWIFTCC void toolbar_build_if(SWIFT_OUT void* out, void* value, void* type, void*) {
    void* optional = optional_metadata(type);
    witnesses_of(optional)->initialize_with_copy(out, value, optional);
}

SWIFTCC void picker_key(SWIFT_OUT ViewNode** out, SwiftString key, uint64_t, LocalizedBox* interpolation, const uint64_t* selection, const void* content, void* context, void* selection_type, void* content_type) {
    auto* node = new_node(NodeKind::Picker);
    *node->title = key_text(key, interpolation);
    node->item_type = selection_type;
    node->binding = reinterpret_cast<BindingNode*>(rt_swift_retain(reinterpret_cast<void*>(selection[1])));
    evaluate_child(node, content, context, content_type);
    *out = node;
}

SWIFTCC void toggle_key(SWIFT_OUT ViewNode** out, SwiftString key, uint64_t, LocalizedBox* interpolation, uint64_t, BindingNode* location, uint64_t) {
    auto* node = new_node(NodeKind::Toggle);
    node->binding = static_cast<BindingNode*>(rt_swift_retain(location));
    *node->title = key_text(key, interpolation);
    *out = node;
}

void make_titled_disclosure(ViewNode** out, std::string title, const void* content, void* context, void* content_type) {
    auto* node = new_node(NodeKind::Disclosure);
    *node->title = std::move(title);
    evaluate_child(node, content, context, content_type);
    *out = node;
}

SWIFTCC void disclosure_key(SWIFT_OUT ViewNode** out, SwiftString key, uint64_t, LocalizedBox* interpolation, const void* content, void* context, void* content_type) {
    make_titled_disclosure(out, key_text(key, interpolation), content, context, content_type);
}

SWIFTCC void disclosure_string(SWIFT_OUT ViewNode** out, const SwiftString* title, const void* content, void* context, void* content_type) {
    make_titled_disclosure(out, swift_string_to_utf8(*title), content, context, content_type);
}

SWIFTCC void disclosure_expanded(SWIFT_OUT ViewNode** out, uint64_t, BindingNode*, uint64_t, const void* content, void* content_context, const void* label, void* label_context, void* label_type, void* content_type) {
    make_disclosure(out, label, label_context, label_type, content, content_context, content_type);
}

SWIFTCC void labeled_strings(SWIFT_OUT ViewNode** out, const SwiftString* title, const SwiftString* value) {
    auto* node = new_node(NodeKind::LabeledContent);
    *node->title = swift_string_to_utf8(*title);
    *node->system_image = swift_string_to_utf8(*value);
    *out = node;
}

struct SizeValue {
    double width;
    double height;
};

SWIFTCC SizeValue geometry_size(SWIFT_CONTEXT void*) { return {390, 844}; }

const BoxMetadata kStateMetadata = {destroy_box, nullptr, 0};

constexpr uint8_t kStorageCaseThunk = 1;

constexpr uint32_t kLazyStateLocationField = 1;

constexpr size_t kLazyStateOffsetsWord = 3;

size_t lazy_state_location_offset(void* metadata) {
    const auto* offsets = reinterpret_cast<const uint32_t*>(static_cast<void**>(metadata) + kLazyStateOffsetsWord);
    return offsets[kLazyStateLocationField];
}

void* state_value(StateBox* state) { return state->value; }

StateBox* new_state_box(void* type) {
    const size_t size = witnesses_of(type)->size;
    auto* state = static_cast<StateBox*>(calloc(1, sizeof(StateBox)));
    state->header.metadata = const_cast<uint64_t*>(&kStateMetadata.kind);
    new (&state->header.references) std::atomic<uint64_t>(1);
    state->type = type;
    state->value = calloc(1, size == 0 ? 1 : size);
    return state;
}

// The first read fixes the state: a thunk runs once, and later copies of the view share the box.
StateBox* state_box_for(char* self, void* metadata) {
    void* type = generic_argument(metadata);
    auto** location = reinterpret_cast<StateBox**>(self + lazy_state_location_offset(metadata));
    if (*location != nullptr) return *location;
    const auto* witnesses = witnesses_of(type);
    StateBox* state = new_state_box(type);
    const size_t payload = witnesses->size < 2 * sizeof(void*) ? 2 * sizeof(void*) : witnesses->size;
    if (static_cast<uint8_t>(self[payload]) == kStorageCaseThunk) {
        const void* thunk;
        void* context;
        memcpy(&thunk, self, sizeof thunk);
        memcpy(&context, self + sizeof thunk, sizeof context);
        rt_call_witness(thunk, state->value, context, nullptr, nullptr);
    } else {
        witnesses->initialize_with_copy(state->value, self, type);
    }
    *location = state;
    return state;
}

SWIFTCC void lazy_state_init(SWIFT_OUT char* out, const void* initializer, void* context, void* type) {
    void* metadata = lazy_state_metadata(type);
    const auto* witnesses = witnesses_of(type);
    const size_t payload = witnesses->size < 2 * sizeof(void*) ? 2 * sizeof(void*) : witnesses->size;
    memset(out, 0, witnesses_of(metadata)->size);
    memcpy(out, &initializer, sizeof initializer);
    memcpy(out + sizeof initializer, &context, sizeof context);
    out[payload] = static_cast<char>(kStorageCaseThunk);
}

SWIFTCC void lazy_state_wrapped_value_get(SWIFT_OUT void* out, void* metadata, SWIFT_CONTEXT char* self) {
    auto* state = state_box_for(self, metadata);
    witnesses_of(state->type)->initialize_with_copy(out, state->value, state->type);
}

constexpr uint64_t kFontMonospaced = 1ULL << 17;

template <uint64_t Value>
SWIFTCC uint64_t constant_getter() { return kImmediate | Value; }

// Types reached through a metadata accessor are returned indirectly.
template <uint64_t Value>
SWIFTCC void indirect_constant_getter(SWIFT_OUT uint64_t* out) { *out = kImmediate | Value; }

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

SWIFTCC UnitPointValue unit_point_top() { return {0.5, 0.0}; }

SWIFTCC UnitPointValue unit_point_bottom() { return {0.5, 1.0}; }

SWIFTCC uint64_t font_bold(uint64_t font) { return font | kFontBold; }

SWIFTCC uint64_t font_monospaced(uint64_t font) { return font | kFontMonospaced; }

std::atomic<bool> g_tree_dirty{true};

SWIFTCC void registrar_init(SWIFT_OUT uint64_t* out) { *out = kImmediate; }

SWIFTCC void registrar_access() {}

SWIFTCC void registrar_mark_dirty() { g_tree_dirty.store(true, std::memory_order_relaxed); }

extern "C" void rt_registrar_with_mutation();

// AppStorage is one reference to this box: the key, and the default until something is stored.
struct AppStorageBox {
    HeapObject header;
    std::string* key;
    std::string* fallback_text;
    int64_t fallback_number;
};

void destroy_app_storage(HeapObject* object) {
    auto* box = reinterpret_cast<AppStorageBox*>(object);
    delete box->key;
    delete box->fallback_text;
    free(box);
}

const BoxMetadata kAppHeapMetadata = {destroy_app_storage, nullptr, 0};

AppStorageBox* new_app_storage(SwiftString key) {
    auto* box = static_cast<AppStorageBox*>(calloc(1, sizeof(AppStorageBox)));
    box->header.metadata = const_cast<uint64_t*>(&kAppHeapMetadata.kind);
    new (&box->header.references) std::atomic<uint64_t>(1);
    box->key = new std::string(swift_string_to_utf8(key));
    box->fallback_text = new std::string();
    return box;
}

SWIFTCC AppStorageBox* app_storage_bool(bool value, SwiftString key, void*) {
    AppStorageBox* box = new_app_storage(key);
    box->fallback_number = value ? 1 : 0;
    return box;
}

SWIFTCC AppStorageBox* app_storage_integer(int64_t value, SwiftString key, void*) {
    AppStorageBox* box = new_app_storage(key);
    box->fallback_number = value;
    return box;
}

SWIFTCC AppStorageBox* app_storage_string(SwiftString value, SwiftString key, void*) {
    AppStorageBox* box = new_app_storage(key);
    *box->fallback_text = swift_string_to_utf8(value);
    return box;
}

void* string_metadata();

void app_storage_read(void* raw, void* out, void* type) {
    auto* box = static_cast<AppStorageBox*>(raw);
    if (type == bool_metadata()) {
        *static_cast<bool*>(out) = foundation_defaults_lookup_bool(*box->key, box->fallback_number != 0);
    } else if (type == int_metadata()) {
        *static_cast<int64_t*>(out) = foundation_defaults_lookup_integer(*box->key, box->fallback_number);
    } else {
        *static_cast<SwiftString*>(out) = swift_string_from_utf8(foundation_defaults_lookup_string(*box->key, *box->fallback_text));
    }
}

void app_storage_write(void* raw, void* value, void* type) {
    auto* box = static_cast<AppStorageBox*>(raw);
    if (type == bool_metadata()) {
        foundation_defaults_store_bool(*box->key, *static_cast<bool*>(value));
    } else if (type == int_metadata()) {
        foundation_defaults_store_integer(*box->key, *static_cast<int64_t*>(value));
    } else {
        foundation_defaults_store_string(*box->key, swift_string_to_utf8(*static_cast<SwiftString*>(value)));
    }
}

void binding_read(BindingNode* binding, void* out, void* type) {
    switch (binding->kind) {
        case BindingKind::State: {
            auto* state = static_cast<StateBox*>(binding->source);
            witnesses_of(type)->initialize_with_copy(out, state_value(state), type);
            return;
        }
        case BindingKind::Closures:
            rt_call_witness(binding->getter, out, binding->getter_context, nullptr, nullptr);
            return;
        case BindingKind::ObjectKeyPath:
            key_path_get(binding->key_path, &binding->source, out, type);
            return;
        case BindingKind::AppStorage:
            app_storage_read(binding->source, out, type);
            return;
        case BindingKind::BindingKeyPath: {
            const size_t size = witnesses_of(binding->parent_type)->size;
            void* parent = calloc(1, size == 0 ? 1 : size);
            binding_read(static_cast<BindingNode*>(binding->source), parent, binding->parent_type);
            key_path_get(binding->key_path, parent, out, type);
            witnesses_of(binding->parent_type)->destroy(parent, binding->parent_type);
            free(parent);
            return;
        }
    }
}

void binding_write(BindingNode* binding, void* value, void* type) {
    switch (binding->kind) {
        case BindingKind::State:
            witnesses_of(type)->assign_with_copy(state_value(static_cast<StateBox*>(binding->source)), value, type);
            break;
        case BindingKind::Closures:
            rt_call_witness(binding->setter, nullptr, binding->setter_context, value, nullptr);
            break;
        case BindingKind::ObjectKeyPath:
            key_path_set(binding->key_path, &binding->source, value, type);
            break;
        case BindingKind::AppStorage:
            app_storage_write(binding->source, value, type);
            break;
        case BindingKind::BindingKeyPath: {
            const size_t size = witnesses_of(binding->parent_type)->size;
            void* parent = calloc(1, size == 0 ? 1 : size);
            binding_read(static_cast<BindingNode*>(binding->source), parent, binding->parent_type);
            key_path_set(binding->key_path, parent, value, type);
            binding_write(static_cast<BindingNode*>(binding->source), parent, binding->parent_type);
            witnesses_of(binding->parent_type)->destroy(parent, binding->parent_type);
            free(parent);
            break;
        }
    }
    registrar_mark_dirty();
}

// Binding is frozen: transaction, location, then an inline copy of the value. The node is the location.
struct BindingHeader {
    uint64_t transaction;
    BindingNode* location;
};

constexpr size_t kBindingValueOffset = sizeof(BindingHeader);

void write_binding(void* out, BindingNode* node, void* type) {
    auto* header = static_cast<BindingHeader*>(out);
    header->transaction = 0;
    header->location = node;
    const auto* witnesses = witnesses_of(type);
    const size_t mask = witnesses->flags & 0xff;
    const size_t offset = (kBindingValueOffset + mask) & ~mask;
    binding_read(node, static_cast<char*>(out) + offset, type);
}

SWIFTCC void binding_wrapped_get(SWIFT_OUT void* out, void* metadata, SWIFT_CONTEXT BindingHeader* self) { binding_read(self->location, out, generic_argument(metadata)); }

SWIFTCC void binding_wrapped_set(void* value, void* metadata, SWIFT_CONTEXT BindingHeader* self) {
    void* type = generic_argument(metadata);
    binding_write(self->location, value, type);
    witnesses_of(type)->destroy(value, type);
}

SWIFTCC void binding_get_set(SWIFT_OUT void* out, const void* getter, void* getter_context, const void* setter, void* setter_context, void* type) {
    auto* binding = new_binding(BindingKind::Closures);
    binding->getter = getter;
    binding->getter_context = getter_context;
    binding->setter = setter;
    binding->setter_context = setter_context;
    write_binding(out, binding, type);
}

SWIFTCC void lazy_state_projected(SWIFT_OUT void* out, void* metadata, SWIFT_CONTEXT char* self) {
    auto* binding = new_binding(BindingKind::State);
    binding->source = rt_swift_retain(state_box_for(self, metadata));
    write_binding(out, binding, generic_argument(metadata));
}

SWIFTCC void binding_member(SWIFT_OUT void* out, void* key_path, void* metadata, SWIFT_CONTEXT BindingHeader* self) {
    auto* binding = new_binding(BindingKind::BindingKeyPath);
    binding->source = rt_swift_retain(self->location);
    binding->key_path = rt_swift_retain(key_path);
    binding->parent_type = generic_argument(metadata);
    write_binding(out, binding, key_path_value_type(key_path));
}

// The guest passes the value's type itself, because AppStorage is loadable as its one reference.
SWIFTCC void app_storage_wrapped_get(SWIFT_OUT void* out, void* box, void* type) { app_storage_read(box, out, type); }

SWIFTCC void app_storage_wrapped_set(void* value, void* box, void* type) {
    app_storage_write(box, value, type);
    witnesses_of(type)->destroy(value, type);
    registrar_mark_dirty();
}

SWIFTCC void app_storage_projected(SWIFT_OUT void* out, void* box, void* type) {
    auto* binding = new_binding(BindingKind::AppStorage);
    binding->source = rt_swift_retain(box);
    write_binding(out, binding, type);
}

// State<T> is frozen: the value inline, then the location word this box hangs from.
size_t state_location_offset(void* type) { return (witnesses_of(type)->size + 7) & ~static_cast<size_t>(7); }

StateBox* plain_state_box(char* self, void* type) {
    auto** location = reinterpret_cast<StateBox**>(self + state_location_offset(type));
    if (*location != nullptr) return *location;
    const auto* witnesses = witnesses_of(type);
    StateBox* state = new_state_box(type);
    witnesses->initialize_with_copy(state->value, self, type);
    *location = state;
    return state;
}

SWIFTCC void state_init(SWIFT_OUT char* out, void* value, void* type) {
    witnesses_of(type)->initialize_with_take(out, value, type);
    memset(out + state_location_offset(type), 0, sizeof(void*));
}

SWIFTCC void state_wrapped_get(SWIFT_OUT void* out, void* metadata, SWIFT_CONTEXT char* self) {
    void* type = generic_argument(metadata);
    witnesses_of(type)->initialize_with_copy(out, plain_state_box(self, type)->value, type);
}

SWIFTCC void state_wrapped_set(void* value, void* metadata, SWIFT_CONTEXT char* self) {
    void* type = generic_argument(metadata);
    witnesses_of(type)->assign_with_take(plain_state_box(self, type)->value, value, type);
    registrar_mark_dirty();
}

SWIFTCC void state_projected(SWIFT_OUT void* out, void* metadata, SWIFT_CONTEXT char* self) {
    void* type = generic_argument(metadata);
    auto* binding = new_binding(BindingKind::State);
    binding->source = rt_swift_retain(plain_state_box(self, type));
    write_binding(out, binding, type);
}

SWIFTCC void binding_constant(SWIFT_OUT void* out, void* value, void* type) {
    const auto* witnesses = witnesses_of(type);
    StateBox* state = new_state_box(type);
    witnesses->initialize_with_take(state->value, value, type);
    auto* binding = new_binding(BindingKind::State);
    binding->source = state;
    write_binding(out, binding, type);
}

SWIFTCC void binding_projected(SWIFT_OUT void* out, void* metadata, SWIFT_CONTEXT void* self) { witnesses_of(metadata)->initialize_with_copy(out, self, metadata); }

// Bindable keeps the observed object itself in its single word.
SWIFTCC void bindable_init(SWIFT_OUT void** out, void* object) { *out = object; }

SWIFTCC void bindable_wrapped_get(SWIFT_OUT void** out, void*, SWIFT_CONTEXT void** self) { *out = rt_swift_retain(*self); }

SWIFTCC void bindable_projected(SWIFT_OUT void** out, void*, SWIFT_CONTEXT void** self) { *out = rt_swift_retain(*self); }

SWIFTCC void bindable_member(SWIFT_OUT void* out, void* key_path, void*, SWIFT_CONTEXT void** self) {
    auto* binding = new_binding(BindingKind::ObjectKeyPath);
    binding->source = rt_swift_retain(*self);
    binding->key_path = rt_swift_retain(key_path);
    write_binding(out, binding, key_path_value_type(key_path));
}

// The view whose body is being evaluated, which is what gives a @StateObject its identity.
std::string g_body_path;

struct StateObjectKey {
    const void* thunk;
    std::string path;
    bool operator<(const StateObjectKey& other) const { return thunk != other.thunk ? thunk < other.thunk : path < other.path; }
};

// StateObject is {thunk, context, is_object}. An object made once must outlive every copy of the view holding the thunk.
void* state_object_resolve(const void* thunk, void* context, uint8_t is_object) {
    if (is_object != 0) return const_cast<void*>(thunk);
    static std::map<StateObjectKey, void*> by_position;
    static std::map<const void*, void*> by_context;
    static std::map<const void*, void*> latest;
    if (context != nullptr) {
        const auto seen = by_context.find(context);
        if (seen != by_context.end()) return seen->second;
    }
    if (g_body_path.empty()) {
        const auto last = latest.find(thunk);
        if (last != latest.end()) return last->second;
    }
    void*& object = by_position[{thunk, g_body_path}];
    if (object == nullptr) {
        // The autoclosure returns the class instance directly, in x0, so it needs no result buffer.
        using Thunk = SWIFTCC void* (*)(SWIFT_CONTEXT void*);
        object = reinterpret_cast<Thunk>(const_cast<void*>(thunk))(context);
    }
    latest[thunk] = object;
    if (context != nullptr) by_context[rt_swift_retain(context)] = object;
    return object;
}

SWIFTCC void* state_object_wrapped(const void* thunk, void* context, uint8_t is_object) { return rt_swift_retain(state_object_resolve(thunk, context, is_object)); }

// ObservedObject is {seed, object}, and the guest reads the object straight out of the second word.
struct ObservedObjectValue {
    uint64_t seed;
    void* object;
};

SWIFTCC ObservedObjectValue observed_object_init(void* object) { return {0, object}; }

SWIFTCC void* observed_object_projected(uint64_t, void* object) { return rt_swift_retain(object); }

constexpr uint64_t kEnvironmentImmediate = kImmediate;

SWIFTCC void environment_values_init(SWIFT_OUT uint64_t* out) { *out = kEnvironmentImmediate; }

SWIFTCC bool environment_is_enabled(SWIFT_CONTEXT void*) { return true; }

SWIFTCC void environment_set_ignored() {}

// Optional<AnyShapeStyle> is one reference word here; null means no tint.
SWIFTCC void* environment_tint(SWIFT_CONTEXT void*) { return nullptr; }

SWIFTCC void environment_action_get(SWIFT_OUT uint64_t* out, SWIFT_CONTEXT void*) { *out = kImmediate; }

// Phones are compact; the size class is an Optional<tag word> with its presence byte after it.
SWIFTCC void environment_size_class(SWIFT_OUT char* out, SWIFT_CONTEXT void*) {
    const int64_t compact = *static_cast<int32_t*>(resolve_symbol("$s7SwiftUI22UserInterfaceSizeClassO7compactyA2CmFWC"));
    memcpy(out, &compact, sizeof compact);
    out[sizeof compact] = 0;
}

constexpr int64_t kColorSchemeLight = -1;

SWIFTCC void environment_color_scheme(SWIFT_OUT int64_t* out, SWIFT_CONTEXT void*) { *out = kColorSchemeLight; }

SWIFTCC bool edit_mode_is_editing(SWIFT_CONTEXT const int64_t* self) { return *self == *static_cast<int32_t*>(resolve_symbol("$s7SwiftUI8EditModeO6activeyA2CmFWC")); }

SWIFTCC bool word_tags_equal(const int64_t* left, const int64_t* right) { return *left == *right; }

SWIFTCC void* environment_no_value(SWIFT_CONTEXT void*) { return nullptr; }

SWIFTCC void environment_no_edit_mode(SWIFT_OUT uint64_t* out, SWIFT_CONTEXT void*) { out[0] = out[1] = out[2] = 0; }

SWIFTCC void* environment_context(SWIFT_CONTEXT void*) { return rt_swift_retain(coredata_view_context()); }

void add_swiftui_state_symbols(SymbolTable& table) {
    table.insert({
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
        {"$s7SwiftUI10ButtonRoleV11destructiveACvgZ", reinterpret_cast<void*>(indirect_constant_getter<kButtonRoleDestructive>)},
        {"$s7SwiftUI20ToolbarItemPlacementV14topBarTrailingACvgZ", reinterpret_cast<void*>(indirect_constant_getter<kToolbarTopBarTrailing>)},
        {"$s7SwiftUI19HorizontalAlignmentV7leadingACvgZ", reinterpret_cast<void*>(constant_getter<kAlignLeading>)},
        {"$s7SwiftUI17VerticalAlignmentV6centerACvgZ", reinterpret_cast<void*>(constant_getter<kAlignCenter>)},
        {"$s7SwiftUI9AlignmentV6centerACvgZ", reinterpret_cast<void*>(alignment_getter<kAlignCenter, kAlignCenter>)},
        {"$s7SwiftUI9AlignmentV14bottomTrailingACvgZ", reinterpret_cast<void*>(alignment_getter<kAlignTrailing, kAlignBottom>)},
        {"$s7SwiftUI4EdgeO3SetV3allAEvgZ", reinterpret_cast<void*>(edge_set_getter<0x0f>)},
        {"$s7SwiftUI4EdgeO3SetV6bottomAEvgZ", reinterpret_cast<void*>(edge_set_getter<0x04>)},
        {"$s7SwiftUI4EdgeO3SetV8trailingAEvgZ", reinterpret_cast<void*>(edge_set_getter<0x08>)},
        {"$s7SwiftUI9UnitPointV6centerACvgZ", reinterpret_cast<void*>(unit_point_center)},
        {"$s7SwiftUI4FontV10largeTitleACvgZ", reinterpret_cast<void*>(constant_getter<kFontLargeTitle>)},
        {"$s7SwiftUI4FontV5titleACvgZ", reinterpret_cast<void*>(constant_getter<kFontTitle>)},
        {"$s7SwiftUI4FontV6title2ACvgZ", reinterpret_cast<void*>(constant_getter<kFontTitle2>)},
        {"$s7SwiftUI4FontV8headlineACvgZ", reinterpret_cast<void*>(constant_getter<kFontHeadline>)},
        {"$s7SwiftUI4FontV11subheadlineACvgZ", reinterpret_cast<void*>(constant_getter<kFontSubheadline>)},
        {"$s7SwiftUI5ColorV7primaryACvgZ", reinterpret_cast<void*>(constant_getter<kColorPrimary>)},
        {"$s7SwiftUI5ColorV4grayACvgZ", reinterpret_cast<void*>(constant_getter<kColorGray>)},
        {"$s7SwiftUI5ColorV4pinkACvgZ", reinterpret_cast<void*>(constant_getter<kColorPink>)},
        {"$s7SwiftUI5ColorV5blackACvgZ", reinterpret_cast<void*>(constant_getter<kColorBlack>)},
        {"$s7SwiftUI5ColorV5clearACvgZ", reinterpret_cast<void*>(constant_getter<kColorClear>)},
        {"$s7SwiftUI5ColorV5whiteACvgZ", reinterpret_cast<void*>(constant_getter<kColorWhite>)},
        {"$s7SwiftUI5ColorV6orangeACvgZ", reinterpret_cast<void*>(constant_getter<kColorOrange>)},
        {"$s7SwiftUI5ColorV6yellowACvgZ", reinterpret_cast<void*>(constant_getter<kColorYellow>)},
        {"$s7SwiftUI19HorizontalAlignmentV6centerACvgZ", reinterpret_cast<void*>(constant_getter<kAlignCenter>)},
        {"$s7SwiftUI22HierarchicalShapeStyleV7primaryACvgZ", reinterpret_cast<void*>(constant_getter<kStylePrimary>)},
        {"$s7SwiftUI4FontV6WeightV4boldAEvgZ", reinterpret_cast<void*>(constant_getter<kWeightBold>)},
        {"$s7SwiftUI4FontV6WeightV8semiboldAEvgZ", reinterpret_cast<void*>(constant_getter<kWeightSemibold>)},
        {"$s7SwiftUI9AlignmentV10topLeadingACvgZ", reinterpret_cast<void*>(alignment_getter<kAlignLeading, kAlignTop>)},
        {"$s7SwiftUI9AlignmentV13bottomLeadingACvgZ", reinterpret_cast<void*>(alignment_getter<kAlignLeading, kAlignBottom>)},
        {"$s7SwiftUI9AlignmentV6bottomACvgZ", reinterpret_cast<void*>(alignment_getter<kAlignCenter, kAlignBottom>)},
        {"$s7SwiftUI9AlignmentV7leadingACvgZ", reinterpret_cast<void*>(alignment_getter<kAlignLeading, kAlignCenter>)},
        {"$s7SwiftUI4EdgeO3SetV10horizontalAEvgZ", reinterpret_cast<void*>(edge_set_getter<0x0a>)},
        {"$s7SwiftUI4EdgeO3SetV3topAEvgZ", reinterpret_cast<void*>(edge_set_getter<0x01>)},
        {"$s7SwiftUI4EdgeO3SetV8verticalAEvgZ", reinterpret_cast<void*>(edge_set_getter<0x05>)},
        {"$s7SwiftUI4AxisO3SetV10horizontalAEvgZ", reinterpret_cast<void*>(edge_set_getter<0x01>)},
        {"$s7SwiftUI4AxisO3SetV8verticalAEvgZ", reinterpret_cast<void*>(edge_set_getter<0x02>)},
        {"$s7SwiftUI9UnitPointV3topACvgZ", reinterpret_cast<void*>(unit_point_top)},
        {"$s7SwiftUI9UnitPointV6bottomACvgZ", reinterpret_cast<void*>(unit_point_bottom)},
        {"$s7SwiftUI14ObservedObjectV12wrappedValueACyxGx_tcfC", reinterpret_cast<void*>(observed_object_init)},
        {"$s7SwiftUI14ObservedObjectV14projectedValueAC7WrapperVyx_Gvg", reinterpret_cast<void*>(observed_object_projected)},
        {"$s7SwiftUI11StateObjectV12wrappedValuexvg", reinterpret_cast<void*>(state_object_wrapped)},
        {"$s7SwiftUI11StateObjectV14projectedValueAA08ObservedD0V7WrapperVyx_Gvg", reinterpret_cast<void*>(state_object_wrapped)},
        {"$s7SwiftUI14ObservedObjectV7WrapperV13dynamicMemberAA7BindingVyqd__Gs24ReferenceWritableKeyPathCyxqd__G_tcluig", reinterpret_cast<void*>(bindable_member)},
        {"$s7SwiftUI5StateV12wrappedValueACyxGx_tcfC", reinterpret_cast<void*>(state_init)},
        {"$s7SwiftUI5StateV12wrappedValuexvg", reinterpret_cast<void*>(state_wrapped_get)},
        {"$s7SwiftUI5StateV12wrappedValuexvs", reinterpret_cast<void*>(state_wrapped_set)},
        {"$s7SwiftUI5StateV14projectedValueAA7BindingVyxGvg", reinterpret_cast<void*>(state_projected)},
        {"$s7SwiftUI7BindingV8constantyACyxGxFZ", reinterpret_cast<void*>(binding_constant)},
        {"$s7SwiftUI7BindingV14projectedValueACyxGvg", reinterpret_cast<void*>(binding_projected)},
        {"$s7SwiftUI10AppStorageV12wrappedValue_5storeACySbGSb_SSSo14NSUserDefaultsCSgtcSbRszlufC", reinterpret_cast<void*>(app_storage_bool)},
        {"$s7SwiftUI10AppStorageV12wrappedValue_5storeACySiGSi_SSSo14NSUserDefaultsCSgtcSiRszlufC", reinterpret_cast<void*>(app_storage_integer)},
        {"$s7SwiftUI10AppStorageV12wrappedValue_5storeACySSGSS_SSSo14NSUserDefaultsCSgtcSSRszlufC", reinterpret_cast<void*>(app_storage_string)},
        {"$s7SwiftUI10AppStorageV12wrappedValuexvg", reinterpret_cast<void*>(app_storage_wrapped_get)},
        {"$s7SwiftUI10AppStorageV12wrappedValuexvs", reinterpret_cast<void*>(app_storage_wrapped_set)},
        {"$s7SwiftUI10AppStorageV14projectedValueAA7BindingVyxGvg", reinterpret_cast<void*>(app_storage_projected)},
        {"$s7SwiftUI14FetchedResultsVyxSicig", reinterpret_cast<void*>(fetched_results_subscript)},
        {"$s7SwiftUI11TransactionV9animationAA9AnimationVSgvs", reinterpret_cast<void*>(environment_set_ignored)},
        {"$s7SwiftUI17EnvironmentValuesV19horizontalSizeClassAA013UserInterfacefG0OSgvg", reinterpret_cast<void*>(environment_size_class)},
        {"$s7SwiftUI17EnvironmentValuesV19horizontalSizeClassAA013UserInterfacefG0OSgvs", reinterpret_cast<void*>(environment_set_ignored)},
        {"$s7SwiftUI17EnvironmentValuesV11colorSchemeAA05ColorF0Ovg", reinterpret_cast<void*>(environment_color_scheme)},
        {"$s7SwiftUI17EnvironmentValuesV11colorSchemeAA05ColorF0Ovs", reinterpret_cast<void*>(environment_set_ignored)},
        {"$s7SwiftUI17EnvironmentValuesV8editModeAA7BindingVyAA04EditF0OGSgvg", reinterpret_cast<void*>(environment_no_edit_mode)},
        {"$s7SwiftUI17EnvironmentValuesV8editModeAA7BindingVyAA04EditF0OGSgvs", reinterpret_cast<void*>(environment_set_ignored)},
        {"$s7SwiftUI17EnvironmentValuesV15foregroundColorAA0F0VSgvg", reinterpret_cast<void*>(environment_no_value)},
        {"$s7SwiftUI17EnvironmentValuesV15foregroundColorAA0F0VSgvs", reinterpret_cast<void*>(environment_set_ignored)},
        {"$s7SwiftUI17EnvironmentValuesV4fontAA4FontVSgvg", reinterpret_cast<void*>(environment_no_value)},
        {"$s7SwiftUI17EnvironmentValuesV4fontAA4FontVSgvs", reinterpret_cast<void*>(environment_set_ignored)},
        {"$s7SwiftUI17EnvironmentValuesV17contentTransitionAA07ContentF0Vvg", reinterpret_cast<void*>(environment_action_get)},
        {"$s7SwiftUI17EnvironmentValuesV17contentTransitionAA07ContentF0Vvs", reinterpret_cast<void*>(environment_set_ignored)},
        {"$s7SwiftUI17EnvironmentValuesV20managedObjectContextSo09NSManagedfG0Cvg", reinterpret_cast<void*>(environment_context)},
        {"$s7SwiftUI17EnvironmentValuesV20managedObjectContextSo09NSManagedfG0Cvs", reinterpret_cast<void*>(environment_set_ignored)},
        {"$s7SwiftUI17EnvironmentValuesV9lineLimitSiSgvs", reinterpret_cast<void*>(environment_set_ignored)},
        {"$s7SwiftUI17EnvironmentValuesV22multilineTextAlignmentAA0fG0Ovs", reinterpret_cast<void*>(environment_set_ignored)},
        {"$s7SwiftUI11ColorSchemeO2eeoiySbAC_ACtFZ", reinterpret_cast<void*>(word_tags_equal)},
        {"$s7SwiftUI8EditModeO2eeoiySbAC_ACtFZ", reinterpret_cast<void*>(word_tags_equal)},
        {"$s7SwiftUI5ImageV02uiC0ACSo7UIImageC_tcfC", reinterpret_cast<void*>(image_from_ui_image)},
        {"$s7SwiftUI11StrokeStyleV9lineWidth0E3Cap0E4Join10miterLimit4dash0K5PhaseAC12CoreGraphics7CGFloatV_So06CGLineG0VSo0pH0VALSayALGALtcfC", reinterpret_cast<void*>(stroke_style_init)},
        {"$s7SwiftUI16_FlexFrameLayoutV8minWidth05idealG003maxG00F6Height0hJ00iJ09alignmentAC12CoreGraphics7CGFloatVSg_A5nA9AlignmentVtcfC", reinterpret_cast<void*>(flex_frame_init)},
        {"$s7SwiftUI12ProgressViewV5value5totalACyAA05EmptyD0VAGGqd__Sg_qd__tcAGRszAGRs_SBRd__lufC", reinterpret_cast<void*>(progress_value_init)},
        {"$s7SwiftUI21ToolbarContentBuilderV11buildEither5firstAA012_ConditionalD0Vyxq_Gx_tAA0cD0RzAaIR_r0_lFZ", reinterpret_cast<void*>(toolbar_build_first)},
        {"$s7SwiftUI21ToolbarContentBuilderV11buildEither6secondAA012_ConditionalD0Vyxq_Gq__tAA0cD0RzAaIR_r0_lFZ", reinterpret_cast<void*>(toolbar_build_second)},
        {"$s7SwiftUI21ToolbarContentBuilderV7buildIfyxSgAeA0cD0RzlFZ", reinterpret_cast<void*>(toolbar_build_if)},
        {"$s7SwiftUI13GeometryProxyV4sizeSo6CGSizeVvg", reinterpret_cast<void*>(geometry_size)},
        {"$s7SwiftUI12TimelineViewVA2A0D0R_rlE_7contentACyxq_Gx_q_AC7ContextVyxq__GctcfC", reinterpret_cast<void*>(empty_node_init)},
        {"$s7SwiftUI6PickerVA2A4TextVRszrlE_9selection7contentACyAEq_q0_GAA18LocalizedStringKeyV_AA7BindingVyq_Gq0_yXEtcfC", reinterpret_cast<void*>(picker_key)},
        {"$s7SwiftUI6ToggleVA2A4TextVRszrlE_4isOnACyAEGAA18LocalizedStringKeyV_AA7BindingVySbGtcfC", reinterpret_cast<void*>(toggle_key)},
        {"$s7SwiftUI15DisclosureGroupV10isExpanded7content5labelACyxq_GAA7BindingVySbG_q_ycxyXEtcfC", reinterpret_cast<void*>(disclosure_expanded)},
        {"$s7SwiftUI15DisclosureGroupVA2A4TextVRszrlE_7contentACyAEq_GAA18LocalizedStringKeyV_q_yctcfC", reinterpret_cast<void*>(disclosure_key)},
        {"$s7SwiftUI15DisclosureGroupVA2A4TextVRszrlE_7contentACyAEq_Gqd___q_yctcSyRd__lufC", reinterpret_cast<void*>(disclosure_string)},
        {"$s7SwiftUI14LabeledContentVA2A4TextVRszAERs_rlE_5valueACyA2EGqd___qd_0_tcSyRd__SyRd_0_r0_lufC", reinterpret_cast<void*>(labeled_strings)},
        {"$s7SwiftUI11ColorPickerVA2A4TextVRszrlE_9selection15supportsOpacityACyAEGAA18LocalizedStringKeyV_AA7BindingVyAA0C0VGSbtcfC", reinterpret_cast<void*>(empty_node_init)},
        {"$s7SwiftUI22UIHostingConfigurationVA2A9EmptyViewVRs_rlE7contentACyxAEGxyXE_tcfC", reinterpret_cast<void*>(empty_node_init)},
        {"$s7SwiftUI8EditModeO9isEditingSbvg", reinterpret_cast<void*>(edit_mode_is_editing)},
        {"$s11Observation0A9RegistrarVACycfC", reinterpret_cast<void*>(registrar_init)},
        {"$s11Observation0A9RegistrarV6access_7keyPathyx_s03KeyE0Cyxq_GtAA10ObservableRzr0_lF", reinterpret_cast<void*>(registrar_access)},
        {"$s11Observation0A9RegistrarV7willSet_7keyPathyx_s03KeyF0Cyxq_GtAA10ObservableRzr0_lF", reinterpret_cast<void*>(registrar_access)},
        {"$s11Observation0A9RegistrarV6didSet_7keyPathyx_s03KeyF0Cyxq_GtAA10ObservableRzr0_lF", reinterpret_cast<void*>(registrar_mark_dirty)},
        {"$s11Observation0A9RegistrarV12withMutation2of7keyPath_q0_x_s03KeyG0Cyxq_Gq0_yKXEtKAA10ObservableRzr1_lF", reinterpret_cast<void*>(rt_registrar_with_mutation)},
        {"$s7SwiftUI7BindingV12wrappedValuexvg", reinterpret_cast<void*>(binding_wrapped_get)},
        {"$s7SwiftUI7BindingV12wrappedValuexvs", reinterpret_cast<void*>(binding_wrapped_set)},
        {"$s7SwiftUI7BindingV13dynamicMemberACyqd__Gs15WritableKeyPathCyxqd__G_tcluig", reinterpret_cast<void*>(binding_member)},
        {"$s7SwiftUI7BindingV3get3setACyxGxyc_yxctcfC", reinterpret_cast<void*>(binding_get_set)},
        {"$s7SwiftUI9LazyStateV14projectedValueAA7BindingVyxGvg", reinterpret_cast<void*>(lazy_state_projected)},
        {"$s7SwiftUI8BindableVAARlzC11Observation10ObservableRzlE12wrappedValueACyxGx_tcfC", reinterpret_cast<void*>(bindable_init)},
        {"$s7SwiftUI8BindableV12wrappedValuexvg", reinterpret_cast<void*>(bindable_wrapped_get)},
        {"$s7SwiftUI8BindableV14projectedValueACyxGvg", reinterpret_cast<void*>(bindable_projected)},
        {"$s7SwiftUI8BindableVAARlzClE13dynamicMemberAA7BindingVyqd__Gs24ReferenceWritableKeyPathCyxqd__G_tcluig", reinterpret_cast<void*>(bindable_member)},
        {"$s7SwiftUI17EnvironmentValuesVACycfC", reinterpret_cast<void*>(environment_values_init)},
        {"$s7SwiftUI17EnvironmentValuesV9isEnabledSbvg", reinterpret_cast<void*>(environment_is_enabled)},
        {"$s7SwiftUI17EnvironmentValuesV9isEnabledSbvs", reinterpret_cast<void*>(environment_set_ignored)},
        {"$s7SwiftUI17EnvironmentValuesV4tintAA13AnyShapeStyleVSgvg", reinterpret_cast<void*>(environment_tint)},
        {"$s7SwiftUI17EnvironmentValuesV4tintAA13AnyShapeStyleVSgvs", reinterpret_cast<void*>(environment_set_ignored)},
        {"$s7SwiftUI17EnvironmentValuesV7dismissAA13DismissActionVvg", reinterpret_cast<void*>(environment_action_get)},
        {"$s7SwiftUI17EnvironmentValuesV7openURLAA13OpenURLActionVvg", reinterpret_cast<void*>(environment_action_get)},
        {"$s7SwiftUI17EnvironmentValuesV7openURLAA13OpenURLActionVvs", reinterpret_cast<void*>(environment_set_ignored)},
    });
}

}  // namespace swiftui
