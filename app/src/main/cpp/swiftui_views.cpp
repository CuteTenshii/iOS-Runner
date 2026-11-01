#include "swiftui_internal.h"

namespace swiftui {

std::vector<const MachSection*> g_view_conformances;

void* rt_swift_getWitnessTable_for_view(const char* conformance, void* type) {
    return swift_runtime_witness_table(conformance, type, static_cast<void**>(type) + 2);
}

void destroy_box(HeapObject* object) { free(object); }

const BoxMetadata kBoxMetadata = {destroy_box, nullptr, 0};

ViewBox* new_box() {
    auto* box = static_cast<ViewBox*>(calloc(1, sizeof(ViewBox)));
    box->header.metadata = const_cast<uint64_t*>(&kBoxMetadata.kind);
    new (&box->header.references) std::atomic<uint64_t>(1);
    return box;
}

const int32_t kStorageCaseValueTag = 0;

const int32_t kTitleDisplayInline = 2;

char g_view_body_requirement;

char g_view_body_type_requirement;

char g_toolbar_body_requirement;

char g_toolbar_body_type_requirement;

char g_modifier_body_requirement;

char g_modifier_body_type_requirement;

SWIFTCC void window_group_init(SWIFT_OUT void** out, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t,
                               const void* closure, void* context, void* content_type, void** content_witnesses) {
    auto* box = new_box();
    box->closure = closure;
    box->closure_context = context;
    box->content_type = content_type;
    box->content_witnesses = content_witnesses;
    *out = box;
}

// Unlike lazyContent, this closure is non-escaping: its context dies when App.body returns, so it runs now.
SWIFTCC void window_group_content(SWIFT_OUT void** out, const void* closure, void* context, void* content_type, void* content_witnesses) {
    auto* box = new_box();
    box->content_type = content_type;
    box->content_witnesses = static_cast<void**>(content_witnesses);
    const size_t size = witnesses_of(content_type)->size;
    box->closure_context = calloc(1, size == 0 ? 1 : size);
    rt_call_witness(closure, box->closure_context, context, nullptr, nullptr);
    *out = box;
}

SWIFTCC void scene_build_block(SWIFT_OUT void* out, void* value, void* type, void*) {
    witnesses_of(type)->initialize_with_copy(out, value, type);
}

SWIFTCC void registrar_mark_dirty();

void destroy_binding(HeapObject* object) {
    auto* binding = reinterpret_cast<BindingNode*>(object);
    rt_swift_release(binding->source);
    rt_swift_release(binding->key_path);
    rt_swift_release(binding->getter_context);
    rt_swift_release(binding->setter_context);
    free(binding);
}

const BoxMetadata kBindingMetadata = {destroy_binding, nullptr, 0};

BindingNode* new_binding(BindingKind kind) {
    auto* binding = static_cast<BindingNode*>(calloc(1, sizeof(BindingNode)));
    binding->header.metadata = const_cast<uint64_t*>(&kBindingMetadata.kind);
    new (&binding->header.references) std::atomic<uint64_t>(1);
    binding->kind = kind;
    return binding;
}

constexpr size_t kMetadataFirstArgumentWord = 2;

void* generic_argument(void* metadata) { return static_cast<void**>(metadata)[kMetadataFirstArgumentWord]; }

// Framework values can hold plain numbers in the same word as a node, so nodes are identified by registry.
std::unordered_set<const void*>& live_nodes() {
    static std::unordered_set<const void*> nodes;
    return nodes;
}

void destroy_node(HeapObject* object) {
    auto* node = reinterpret_cast<ViewNode*>(object);
    live_nodes().erase(node);
    delete node->title;
    delete node->system_image;
    delete node->detail;
    for (const auto& child : *node->children) {
        witnesses_of(child.type)->destroy(child.value, child.type);
        free(child.value);
    }
    delete node->children;
    rt_swift_release(node->action_context);
    rt_swift_release(node->binding);
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
    node->detail = new std::string();
    node->children = new std::vector<ChildValue>();
    live_nodes().insert(node);
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

// Keeps a copy of `self` as the node's child, for modifiers whose result type is opaque.
void add_self_child(ViewNode* node, void* self, void* type) {
    const size_t size = witnesses_of(type)->size;
    void* value = calloc(1, size == 0 ? 1 : size);
    witnesses_of(type)->initialize_with_copy(value, self, type);
    node->children->push_back({value, type});
}

SWIFTCC void navigation_stack_init(SWIFT_OUT ViewNode** out, const void* closure, void* context, void* root_type, void*) {
    auto* node = new_node(NodeKind::NavigationStack);
    evaluate_child(node, closure, context, root_type);
    *out = node;
}

SWIFTCC void list_init(SWIFT_OUT ViewNode** out, const void* closure, void* context, void* content_type, void*) {
    auto* node = new_node(NodeKind::List);
    evaluate_child(node, closure, context, content_type);
    *out = node;
}

void make_button(ViewNode** out, const void* action, void* action_context, const void* label, void* label_context, void* label_type) {
    auto* node = new_node(NodeKind::Button);
    node->action = action;
    node->action_context = rt_swift_retain(action_context);
    evaluate_child(node, label, label_context, label_type);
    *out = node;
}

SWIFTCC void button_init(SWIFT_OUT ViewNode** out, const void* action, void* action_context, const void* label, void* label_context, void* label_type, void*) {
    make_button(out, action, action_context, label, label_context, label_type);
}

SWIFTCC void button_role_init(SWIFT_OUT ViewNode** out, uint64_t, const void* action, void* action_context, const void* label, void* label_context, void* label_type, void*) {
    make_button(out, action, action_context, label, label_context, label_type);
}

SWIFTCC void label_init(SWIFT_OUT ViewNode** out, const void* title, void* title_context, const void* icon, void* icon_context, void* title_type, void* icon_type, void*, void*) {
    auto* node = new_node(NodeKind::Label);
    evaluate_child(node, title, title_context, title_type);
    evaluate_child(node, icon, icon_context, icon_type);
    *out = node;
}

// Binding<Bool> is concrete, so the compiler passes its three words directly.
SWIFTCC void toggle_label_init(SWIFT_OUT ViewNode** out, uint64_t, BindingNode* location, uint64_t, const void* label, void* label_context, void* label_type, void*) {
    auto* node = new_node(NodeKind::Toggle);
    node->binding = static_cast<BindingNode*>(rt_swift_retain(location));
    evaluate_child(node, label, label_context, label_type);
    *out = node;
}

SWIFTCC void toggle_title_init(SWIFT_OUT ViewNode** out, const SwiftString* title, uint64_t, BindingNode* location, uint64_t, void*, void*) {
    auto* node = new_node(NodeKind::Toggle);
    node->binding = static_cast<BindingNode*>(rt_swift_retain(location));
    *node->title = swift_string_to_utf8(*title);
    *out = node;
}

void add_value_child(ViewNode* node, void* value_pointer, void* type) { add_self_child(node, value_pointer, type); }

SWIFTCC void picker_init(SWIFT_OUT ViewNode** out, const uint64_t* selection, void* label, const void* content, void* content_context, void* label_type, void* selection_type, void* content_type) {
    auto* node = new_node(NodeKind::Picker);
    node->item_type = selection_type;
    node->binding = reinterpret_cast<BindingNode*>(rt_swift_retain(reinterpret_cast<void*>(selection[1])));
    add_value_child(node, label, label_type);
    evaluate_child(node, content, content_context, content_type);
    *out = node;
}

constexpr size_t kArrayCountOffsetBytes = 16;

constexpr size_t kArrayElementsOffsetBytes = 32;

// Evaluates the row closure for each element, which needs Data to be an Array.
void evaluate_rows(ViewNode* node, void* data, void* data_type, const void* row, void* row_context, void* row_type) {
    const char* data_name = foreign_type_name(data_type);
    if (data_name != nullptr && strcmp(data_name, "_$s7SwiftUI14FetchedResultsVMn") == 0) {
        for (void* object : fetched_results_contents(data)) {
            const size_t size = witnesses_of(row_type)->size;
            void* value = calloc(1, size == 0 ? 1 : size);
            rt_call_witness(row, value, row_context, &object, nullptr);
            node->children->push_back({value, row_type});
        }
        return;
    }
    void* element = array_element_type(data_type);
    if (element == nullptr) {
        engine_log("STOP: ForEach over a collection other than Array");
        abort();
    }
    const auto* elements = witnesses_of(element);
    const auto storage = reinterpret_cast<const char*>(*static_cast<uint64_t*>(data) & kBridgePointerMask);
    int64_t count;
    memcpy(&count, storage + kArrayCountOffsetBytes, sizeof count);
    const size_t mask = elements->flags & 0xff;
    const char* first = storage + ((kArrayElementsOffsetBytes + mask) & ~mask);
    for (int64_t i = 0; i < count; ++i) {
        const size_t size = witnesses_of(row_type)->size;
        void* value = calloc(1, size == 0 ? 1 : size);
        rt_call_witness(row, value, row_context, const_cast<char*>(first + static_cast<size_t>(i) * elements->stride), nullptr);
        node->children->push_back({value, row_type});
    }
}

// With a key path, ID follows from the key path and is not passed; here it is.
SWIFTCC void for_each_identifiable(SWIFT_OUT ViewNode** out, void* data, const void* row, void* row_context, void* data_type, void*, void* row_type) {
    auto* node = new_node(NodeKind::ForEach);
    evaluate_rows(node, data, data_type, row, row_context, row_type);
    *out = node;
}

SWIFTCC void for_each_key_path(SWIFT_OUT ViewNode** out, void* data, void*, const void* row, void* row_context, void* data_type, void* row_type) {
    auto* node = new_node(NodeKind::ForEach);
    evaluate_rows(node, data, data_type, row, row_context, row_type);
    *out = node;
}

SWIFTCC void section_create(SWIFT_OUT ViewNode** out, uint64_t, uint64_t, uint64_t, void* content, void* header, void* footer, void* header_type, void* content_type, void* footer_type) {
    auto* node = new_node(NodeKind::Section);
    add_value_child(node, header, header_type);
    add_value_child(node, content, content_type);
    add_value_child(node, footer, footer_type);
    *out = node;
}

SWIFTCC void progress_view_init(SWIFT_OUT ViewNode** out) { *out = new_node(NodeKind::ProgressView); }

SWIFTCC void stepper_init(SWIFT_OUT ViewNode** out, const uint64_t* value, const int64_t* range, const int64_t* step, uint64_t, uint64_t, const void* label, void* label_context, void* label_type) {
    auto* node = new_node(NodeKind::Stepper);
    node->lower = range[0];
    node->upper = range[1];
    node->step = *step;
    node->binding = reinterpret_cast<BindingNode*>(rt_swift_retain(reinterpret_cast<void*>(value[1])));
    evaluate_child(node, label, label_context, label_type);
    *out = node;
}

SWIFTCC ViewNode* image_system_name(SwiftString name) {
    auto* node = new_node(NodeKind::Image);
    *node->system_image = swift_string_to_utf8(name);
    return node;
}

SWIFTCC void image_resource_init(SWIFT_OUT ViewNode** out, SwiftString name, void*) {
    auto* node = new_node(NodeKind::ImageResource);
    *node->title = swift_string_to_utf8(name);
    *out = node;
}

SWIFTCC ViewNode* image_from_resource(ViewNode** resource) {
    auto* node = new_node(NodeKind::Image);
    *node->title = *(*resource)->title;
    return node;
}

// A resizable image is the same picture: layout comes from its modifiers.
SWIFTCC ViewNode* image_resizable(double, double, double, double, const void*, ViewNode* self) { return static_cast<ViewNode*>(rt_swift_retain(self)); }

SWIFTCC void async_image_init(SWIFT_OUT ViewNode** out, const void*, double, void*) { *out = new_node(NodeKind::AsyncImage); }

const int32_t kImageResizingStretch = 1;

void make_disclosure(ViewNode** out, const void* label, void* label_context, void* label_type, const void* content, void* content_context, void* content_type) {
    auto* node = new_node(NodeKind::Disclosure);
    evaluate_child(node, label, label_context, label_type);
    evaluate_child(node, content, content_context, content_type);
    *out = node;
}

SWIFTCC void disclosure_init(SWIFT_OUT ViewNode** out, const void* content, void* content_context, const void* label, void* label_context, void* label_type, void* content_type) {
    make_disclosure(out, label, label_context, label_type, content, content_context, content_type);
}

SWIFTCC void labeled_content_view(SWIFT_OUT ViewNode** out, const SwiftString* title, const void* content, void* content_context, void* content_type) {
    auto* node = new_node(NodeKind::LabeledContent);
    *node->title = swift_string_to_utf8(*title);
    evaluate_child(node, content, content_context, content_type);
    *out = node;
}

SWIFTCC void labeled_content_value(SWIFT_OUT ViewNode** out, LocalizedBox** title, const SwiftString* value) {
    auto* node = new_node(NodeKind::LabeledContent);
    *node->title = localized_text(*title);
    *node->system_image = swift_string_to_utf8(*value);
    *out = node;
}

SWIFTCC void unavailable_init(SWIFT_OUT ViewNode** out, const SwiftString* title, SwiftString system_image, TextValue description) {
    auto* node = new_node(NodeKind::Unavailable);
    *node->title = swift_string_to_utf8(*title);
    *node->system_image = swift_string_to_utf8(system_image);
    if (description.text_object != 0) *node->detail = swift_string_to_utf8({description.text_bits, description.text_object});
    *out = node;
}

SWIFTCC void menu_init(SWIFT_OUT ViewNode** out, const void* content, void* content_context, const void* label, void* label_context, void* label_type, void* content_type) {
    auto* node = new_node(NodeKind::Menu);
    evaluate_child(node, label, label_context, label_type);
    evaluate_child(node, content, content_context, content_type);
    *out = node;
}

SWIFTCC void link_init(SWIFT_OUT ViewNode** out, void** url, const void* label, void* label_context, void* label_type) {
    auto* node = new_node(NodeKind::Link);
    *node->detail = foundation_url_text(*url);
    evaluate_child(node, label, label_context, label_type);
    *out = node;
}

SWIFTCC void toolbar_item_init(SWIFT_OUT ViewNode** out, const void*, const void* content, void* content_context, void* content_type) {
    auto* node = new_node(NodeKind::ToolbarItem);
    evaluate_child(node, content, content_context, content_type);
    *out = node;
}

SWIFTCC void empty_style_init(SWIFT_OUT uint64_t* out) { *out = kImmediate; }

SWIFTCC void* any_shape_style_init(const void*, void*, void*) { return reinterpret_cast<void*>(kImmediate); }

SWIFTCC void with_animation(SWIFT_OUT void* out, void*, const void* body, void* context) { rt_call_witness(body, out, context, nullptr, nullptr); }

// LocalizedStringKey interpolation mirrors LocalizationValue's, boxed the same way.
SWIFTCC void key_interpolation_init(SWIFT_OUT LocalizedBox** out, int64_t, int64_t) { *out = new_localized_box(""); }

SWIFTCC void key_append_literal(SwiftString text, SWIFT_CONTEXT LocalizedBox** self) { *(*self)->key += swift_string_to_utf8(text); }

SWIFTCC void key_append_string(SwiftString text, SWIFT_CONTEXT LocalizedBox** self) {
    *(*self)->key += "%@";
    (*self)->arguments->push_back(swift_string_to_utf8(text));
}

SWIFTCC void key_append_integer(const int64_t* value, SwiftString specifier, void*, void*, SWIFT_CONTEXT LocalizedBox** self) {
    *(*self)->key += swift_string_to_utf8(specifier);
    (*self)->arguments->push_back(std::to_string(*value));
}

SWIFTCC LocalizedKey key_from_interpolation(LocalizedBox** interpolation) { return {{0, 0}, 1, *interpolation}; }

// These two return ModifiedContent<Self, AccessibilityAttachmentModifier>: self, then the modifier's word.
void accessibility_write(void* out, void* self, void* type) {
    const auto* witnesses = witnesses_of(type);
    witnesses->initialize_with_copy(out, self, type);
    const size_t offset = (witnesses->size + 7) & ~static_cast<size_t>(7);
    const uint64_t modifier = kImmediate;
    memcpy(static_cast<char*>(out) + offset, &modifier, sizeof modifier);
}

SWIFTCC void accessibility_label(SWIFT_OUT void* out, TextValue, void* self_type, void*, SWIFT_CONTEXT void* self) { accessibility_write(out, self, self_type); }

SWIFTCC void accessibility_traits(SWIFT_OUT void* out, const void*, void* self_type, void*, SWIFT_CONTEXT void* self) { accessibility_write(out, self, self_type); }

SWIFTCC void accessibility_is_header(SWIFT_OUT uint64_t* out) { *out = kImmediate; }

// `style` modifiers: only the wrapped content matters until rendering honours the style.
SWIFTCC void modifier_generic_style(SWIFT_OUT ViewNode** out, const void*, void* self_type, void*, void*, void*, SWIFT_CONTEXT void* self) {
    auto* node = new_node(NodeKind::Modifier);
    add_self_child(node, self, self_type);
    *out = node;
}

SWIFTCC void navigation_title_key(SWIFT_OUT ViewNode** out, SwiftString key, uint64_t, LocalizedBox* interpolation, void* self_type, void*, SWIFT_CONTEXT void* self) {
    auto* node = new_node(NodeKind::Modifier);
    *node->title = interpolation != nullptr ? localized_text(interpolation) : localization_lookup(swift_string_to_utf8(key));
    add_self_child(node, self, self_type);
    *out = node;
}

SWIFTCC void navigation_title_text(SWIFT_OUT ViewNode** out, TextValue text, void* self_type, void*, SWIFT_CONTEXT void* self) {
    auto* node = new_node(NodeKind::Modifier);
    *node->title = swift_string_to_utf8({text.text_bits, text.text_object});
    add_self_child(node, self, self_type);
    *out = node;
}

SWIFTCC void navigation_title_string(SWIFT_OUT ViewNode** out, const SwiftString* title, void* self_type, void*, void*, void*, SWIFT_CONTEXT void* self) {
    auto* node = new_node(NodeKind::Modifier);
    *node->title = swift_string_to_utf8(*title);
    add_self_child(node, self, self_type);
    *out = node;
}

SWIFTCC void navigation_display_mode(SWIFT_OUT ViewNode** out, const void*, void* self_type, void*, SWIFT_CONTEXT void* self) {
    auto* node = new_node(NodeKind::Modifier);
    add_self_child(node, self, self_type);
    *out = node;
}

SWIFTCC void toolbar_modifier(SWIFT_OUT ViewNode** out, const void* closure, void* context, void* self_type, void* content_type, void*, void*, SWIFT_CONTEXT void* self) {
    auto* node = new_node(NodeKind::Toolbar);
    add_self_child(node, self, self_type);
    evaluate_child(node, closure, context, content_type);
    *out = node;
}

SWIFTCC void on_change_modifier(SWIFT_OUT ViewNode** out, const void*, uint64_t, const void* action, void* action_context, void* self_type, void*, void*, void*, SWIFT_CONTEXT void* self) {
    auto* node = new_node(NodeKind::OnChange);
    node->action = action;
    node->action_context = rt_swift_retain(action_context);
    add_self_child(node, self, self_type);
    *out = node;
}

SWIFTCC void sheet_modifier(SWIFT_OUT ViewNode** out, const uint64_t* item, uint64_t, uint64_t, const void* content, void* content_context, void* self_type, void* item_type, void* content_type, void*, void*, void*, SWIFT_CONTEXT void* self) {
    auto* node = new_node(NodeKind::Sheet);
    node->item_type = item_type;
    node->content_type = content_type;
    node->binding = reinterpret_cast<BindingNode*>(rt_swift_retain(reinterpret_cast<void*>(item[1])));
    node->action = content;
    node->action_context = rt_swift_retain(content_context);
    add_self_child(node, self, self_type);
    *out = node;
}

// Most modifiers only change appearance: they keep `self` as a child, and only differ in where Self is passed.
template <size_t SelfTypeRegister>
SWIFTCC void modifier_passthrough(SWIFT_OUT ViewNode** out, void* x0, void* x1, void* x2, void* x3, void* x4, void* x5, void* x6, void* x7, SWIFT_CONTEXT void* self) {
    void* const registers[] = {x0, x1, x2, x3, x4, x5, x6, x7};
    auto* node = new_node(NodeKind::Modifier);
    add_self_child(node, self, registers[SelfTypeRegister]);
    *out = node;
}

void add_swiftui_views_symbols(SymbolTable& table) {
    table.insert({
        {"$s7SwiftUI11WindowGroupV2id5title11lazyContentACyxGSSSg_AA4TextVSgxyctcfC", reinterpret_cast<void*>(window_group_init)},
        {"$s7SwiftUI11WindowGroupV7contentACyxGxyXE_tcfC", reinterpret_cast<void*>(window_group_content)},
        {"$s7SwiftUI12SceneBuilderV10buildBlockyxxAA0C0RzlFZ", reinterpret_cast<void*>(scene_build_block)},
        {"$s7SwiftUI4ViewPAAE08progressC5StyleyQrqd__AA08ProgresscE0Rd__lF", reinterpret_cast<void*>(modifier_passthrough<1>)},
        {"$s7SwiftUI4ViewPAAE10labelStyleyQrqd__AA05LabelE0Rd__lF", reinterpret_cast<void*>(modifier_passthrough<1>)},
        {"$s7SwiftUI4ViewPAAE03tabC5StyleyQrqd__AA03TabcE0Rd__lF", reinterpret_cast<void*>(modifier_passthrough<1>)},
        {"$s7SwiftUI4ViewPAAE9listStyleyQrqd__AA04ListE0Rd__lF", reinterpret_cast<void*>(modifier_passthrough<1>)},
        {"$s7SwiftUI4ViewPAAE23scrollDismissesKeyboardyQrAA06ScrolleF4ModeVF", reinterpret_cast<void*>(modifier_passthrough<1>)},
        {"$s7SwiftUI4ViewPAAE19presentationDetentsyQrShyAA18PresentationDetentVGF", reinterpret_cast<void*>(modifier_passthrough<1>)},
        {"$s7SwiftUI4ViewPAAE25presentationDragIndicatoryQrAA10VisibilityOF", reinterpret_cast<void*>(modifier_passthrough<1>)},
        {"$s7SwiftUI4ViewPAAE27textInputAutocapitalizationyQrAA04TexteF0VSgF", reinterpret_cast<void*>(modifier_passthrough<1>)},
        {"$s7SwiftUI4ViewPAAE4boldyQrSbF", reinterpret_cast<void*>(modifier_passthrough<1>)},
        {"$s7SwiftUI4ViewPAAE12labelsHiddenQryF", reinterpret_cast<void*>(modifier_passthrough<0>)},
        {"$s7SwiftUI4ViewPAAE12keyboardTypeyQrSo010UIKeyboardE0VF", reinterpret_cast<void*>(modifier_passthrough<1>)},
        {"$s7SwiftUI4ViewPAAE18autocapitalizationyQrSo28UITextAutocapitalizationTypeVF", reinterpret_cast<void*>(modifier_passthrough<1>)},
        {"$s7SwiftUI4ViewPAAE16headerProminenceyQrAA0E0OF", reinterpret_cast<void*>(modifier_passthrough<1>)},
        {"$s7SwiftUI4ViewPAAE10fontWeightyQrAA4FontV0E0VSgF", reinterpret_cast<void*>(modifier_passthrough<2>)},
        {"$s7SwiftUI4ViewPAAE15fullScreenCover4item9onDismiss7contentQrAA7BindingVyqd__SgG_yycSgqd_0_qd__cts12IdentifiableRd__AaBRd_0_r0_lF", reinterpret_cast<void*>(sheet_modifier)},
        {"$s7SwiftUI7ForEachVA2A4ViewR0_rlE_2id7contentACyxq_q0_Gx_s7KeyPathCy7ElementQzq_Gq0_AKctcfC", reinterpret_cast<void*>(for_each_key_path)},
        {"$s7SwiftUI4FormV7contentACyxGxyXE_tcfC", reinterpret_cast<void*>(list_init)},
        {"$s7SwiftUI4ViewPAAE12searchScopes_6scopesQrAA7BindingVyqd__G_qd_0_yXEtSHRd__AaBRd_0_r0_lF", reinterpret_cast<void*>(modifier_passthrough<3>)},
        {"$s7SwiftUI6ToggleVA2A4TextVRszrlE_4isOnACyAEGqd___AA7BindingVySbGtcSyRd__lufC", reinterpret_cast<void*>(toggle_title_init)},
        {"$s7SwiftUI7TabViewV7contentACys5NeverOAA0C14ContentBuilderV0G0VyAF_qd__GGqd__yXE_tcAFRszAKRs_AA0cG0Rd__lufC", reinterpret_cast<void*>(tab_view_init)},
        {"$s7SwiftUI3TabVAAs5NeverORszAA4ViewR_AaFR0_rlE_11systemImage7contentACyAEq_AA07DefaultC5LabelVG10Foundation23LocalizedStringResourceV_SSq_yXEtcAJRs0_rlufC", reinterpret_cast<void*>(tab_init)},
        {"$s7SwiftUI15NavigationStackV4rootACyAA0C4PathVq_Gq_yXE_tcAFRszrlufC", reinterpret_cast<void*>(navigation_stack_init)},
        {"$s7SwiftUI4ListVAAs5NeverORszrlE7contentACyAEq_Gq_yXE_tcfC", reinterpret_cast<void*>(list_init)},
        {"$s7SwiftUI6ButtonV6action5labelACyxGyyc_xyXEtcfC", reinterpret_cast<void*>(button_init)},
        {"$s7SwiftUI6ButtonV4role6action5labelACyxGAA0C4RoleVSg_yycxyXEtcfC", reinterpret_cast<void*>(button_role_init)},
        {"$s7SwiftUI5LabelV5title4iconACyxq_GxyXE_q_yXEtcfC", reinterpret_cast<void*>(label_init)},
        {"$s7SwiftUI6ToggleV4isOn5labelACyxGAA7BindingVySbG_xyXEtcfC", reinterpret_cast<void*>(toggle_label_init)},
        {"$s7SwiftUI6ToggleVA2A4TextVRszrlE_4isOnACyAEGqd___AA7BindingVySbGtcSyRd__lufC", reinterpret_cast<void*>(toggle_title_init)},
        {"$s7SwiftUI4ViewPAAE11buttonStyleyQrqd__AA015PrimitiveButtonE0Rd__lF", reinterpret_cast<void*>(modifier_generic_style)},
        {"$s7SwiftUI4ViewPAAE11pickerStyleyQrqd__AA06PickerE0Rd__lF", reinterpret_cast<void*>(modifier_generic_style)},
        {"$s7SwiftUI4ViewPAAE13textSelectionyQrqd__AA17TextSelectabilityRd__lF", reinterpret_cast<void*>(modifier_generic_style)},
        {"$s7SwiftUI4ViewPAAE15navigationTitleyQrAA18LocalizedStringKeyVF", reinterpret_cast<void*>(navigation_title_key)},
        {"$s7SwiftUI4ViewPAAE15navigationTitleyQrAA4TextVF", reinterpret_cast<void*>(navigation_title_text)},
        {"$s7SwiftUI4ViewPAAE15navigationTitleyQrqd__SyRd__lF", reinterpret_cast<void*>(navigation_title_string)},
        {"$s7SwiftUI4ViewPAAE29navigationBarTitleDisplayModeyQrAA010NavigationE4ItemV0fgH0OF", reinterpret_cast<void*>(navigation_display_mode)},
        {"$s7SwiftUI4ViewPAAE7toolbar7contentQrqd__yXE_tAA14ToolbarContentRd__lF", reinterpret_cast<void*>(toolbar_modifier)},
        {"$s7SwiftUI4ViewPAAE7toolbar7contentQrqd__yXE_tAaBRd__lF", reinterpret_cast<void*>(toolbar_modifier)},
        {"$s7SwiftUI4ViewPAAE8onChange2of7initial_Qrqd___Sbyqd___qd__tctSQRd__lF", reinterpret_cast<void*>(on_change_modifier)},
        {"$s7SwiftUI4ViewPAAE5sheet4item9onDismiss7contentQrAA7BindingVyqd__SgG_yycSgqd_0_qd__cts12IdentifiableRd__AaBRd_0_r0_lF", reinterpret_cast<void*>(sheet_modifier)},
        {"$s7SwiftUI6PickerV9selection5label7contentACyxq_q0_GAA7BindingVyq_G_xq0_yXEtcfC", reinterpret_cast<void*>(picker_init)},
        {"$s7SwiftUI7ForEachVAA7Element_2IDQZRs_s12IdentifiableADRpzrlE6create_7contentACyxq_q0_Gx_q0_AHctFZ", reinterpret_cast<void*>(for_each_identifiable)},
        {"$s7SwiftUI7ForEachVAAE6create_2id7contentACyxq_q0_Gx_s7KeyPathCy7ElementQzq_Gq0_AKctFZ", reinterpret_cast<void*>(for_each_key_path)},
        {"$s7SwiftUI7SectionV6create10isExpanded7content6header6footerACyxq_q0_GAA7BindingVySbGSg_q_xq0_tFZ", reinterpret_cast<void*>(section_create)},
        {"$s7SwiftUI12ProgressViewVA2A05EmptyD0VRs_rlEACyA2EGycAERszrlufC", reinterpret_cast<void*>(progress_view_init)},
        {"$s7SwiftUI7StepperV5value2in4step16onEditingChanged5labelACyxGAA7BindingVyqd__G_SNyqd__G6StrideQyd__ySbcxyXEtcSxRd__lufC", reinterpret_cast<void*>(stepper_init)},
        {"$s7SwiftUI5ImageV10systemNameACSS_tcfC", reinterpret_cast<void*>(image_system_name)},
        {"$s21DeveloperToolsSupport13ImageResourceV4name6bundleACSS_So8NSBundleCtcfC", reinterpret_cast<void*>(image_resource_init)},
        {"$s7SwiftUI5ImageVyAC21DeveloperToolsSupport0C8ResourceVcfC", reinterpret_cast<void*>(image_from_resource)},
        {"$s7SwiftUI5ImageV9resizable9capInsets12resizingModeAcA04EdgeF0V_AC08ResizingH0OtF", reinterpret_cast<void*>(image_resizable)},
        {"$s7SwiftUI10AsyncImageV3url5scale11transaction7contentACyxG10Foundation3URLVSg_12CoreGraphics7CGFloatVAA11TransactionVxAA0cD5PhaseOctcfC", reinterpret_cast<void*>(async_image_init)},
        {"$s7SwiftUI15DisclosureGroupV7content5labelACyxq_Gq_yc_xyXEtcfC", reinterpret_cast<void*>(disclosure_init)},
        {"$s7SwiftUI14LabeledContentVA2A4TextVRszAA4ViewR_rlE_7contentACyAEq_Gqd___q_yXEtcSyRd__lufC", reinterpret_cast<void*>(labeled_content_view)},
        {"$s7SwiftUI14LabeledContentVA2A4TextVRszAERs_rlE_5valueACyA2EG10Foundation23LocalizedStringResourceV_qd__tcSyRd__lufC", reinterpret_cast<void*>(labeled_content_value)},
        {"$s7SwiftUI22ContentUnavailableViewVA2A5LabelVyAA4TextVAA5ImageVGRszAGSgRs_AA05EmptyE0VRs0_rlE_06systemH011descriptionACyAjkMGqd___SSAKtcSyRd__lufC", reinterpret_cast<void*>(unavailable_init)},
        {"$s7SwiftUI4MenuV7content5labelACyxq_Gq_yXE_xyXEtcfC", reinterpret_cast<void*>(menu_init)},
        {"$s7SwiftUI4LinkV11destination5labelACyxG10Foundation3URLV_xyXEtcfC", reinterpret_cast<void*>(link_init)},
        {"$s7SwiftUI11ToolbarItemVAAytRszrlE9placement7contentACyytq_GAA0cD9PlacementV_q_yXEtcfC", reinterpret_cast<void*>(toolbar_item_init)},
        {"$s7SwiftUI15MenuPickerStyleVACycfC", reinterpret_cast<void*>(empty_style_init)},
        {"$s7SwiftUI16PlainButtonStyleVACycfC", reinterpret_cast<void*>(empty_style_init)},
        {"$s7SwiftUI19BorderedButtonStyleVACycfC", reinterpret_cast<void*>(empty_style_init)},
        {"$s7SwiftUI24EnabledTextSelectabilityVACycfC", reinterpret_cast<void*>(empty_style_init)},
        {"$s7SwiftUI13AnyShapeStyleVyACxcAA0dE0RzlufC", reinterpret_cast<void*>(any_shape_style_init)},
        {"$s7SwiftUI13withAnimationyxAA0D0VSg_xyKXEtKlF", reinterpret_cast<void*>(with_animation)},
        {"$s7SwiftUI18LocalizedStringKeyV0D13InterpolationV15literalCapacity18interpolationCountAESi_SitcfC", reinterpret_cast<void*>(key_interpolation_init)},
        {"$s7SwiftUI18LocalizedStringKeyV0D13InterpolationV13appendLiteralyySSF", reinterpret_cast<void*>(key_append_literal)},
        {"$s7SwiftUI18LocalizedStringKeyV0D13InterpolationV06appendF0yySSF", reinterpret_cast<void*>(key_append_string)},
        {"$s7SwiftUI18LocalizedStringKeyV0D13InterpolationV06appendF0_9specifieryx_SStAA18_FormatSpecifiableRzlF", reinterpret_cast<void*>(key_append_integer)},
        {"$s7SwiftUI18LocalizedStringKeyV19stringInterpolationA2C0dG0V_tcfC", reinterpret_cast<void*>(key_from_interpolation)},
        {"$s7SwiftUI4ViewPAAE18accessibilityLabelyAA15ModifiedContentVyxAA31AccessibilityAttachmentModifierVGAA4TextVF", reinterpret_cast<void*>(accessibility_label)},
        {"$s7SwiftUI4ViewPAAE22accessibilityAddTraitsyAA15ModifiedContentVyxAA31AccessibilityAttachmentModifierVGAA0iF0VF", reinterpret_cast<void*>(accessibility_traits)},
        {"$s7SwiftUI19AccessibilityTraitsV8isHeaderACvgZ", reinterpret_cast<void*>(accessibility_is_header)},
    });
}

}  // namespace swiftui
