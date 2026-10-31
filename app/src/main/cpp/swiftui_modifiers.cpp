#include "swiftui_internal.h"

namespace swiftui {

// ModifiedContent<Self, M> written inline, for modifiers whose concrete type the app was compiled against.
void write_modified_content(void* out, void* self, void* type, const void* modifier, size_t modifier_size) {
    const auto* witnesses = witnesses_of(type);
    witnesses->initialize_with_copy(out, self, type);
    const size_t offset = (witnesses->size + 7) & ~static_cast<size_t>(7);
    memcpy(static_cast<char*>(out) + offset, modifier, modifier_size);
}

constexpr size_t kPaddingLayoutBytes = 48;

SWIFTCC void view_padding(SWIFT_OUT void* out, uint8_t, uint64_t, bool, void* self_type, void*, SWIFT_CONTEXT void* self) {
    const char modifier[kPaddingLayoutBytes] = {};
    write_modified_content(out, self, self_type, modifier, sizeof modifier);
}

// _EnvironmentKeyWritingModifier: the key path is unused, because the environment isn't modelled.
SWIFTCC void view_environment_word(SWIFT_OUT void* out, uint64_t value, void* self_type, void*, SWIFT_CONTEXT void* self) {
    const uint64_t modifier[] = {0, value};
    write_modified_content(out, self, self_type, modifier, sizeof modifier);
}

SWIFTCC void view_transition(SWIFT_OUT void* out, uint64_t transition, void* self_type, void*, SWIFT_CONTEXT void* self) {
    write_modified_content(out, self, self_type, &transition, sizeof transition);
}

SWIFTCC void view_on_appear(SWIFT_OUT void* out, const void* action, void* context, void* self_type, void*, SWIFT_CONTEXT void* self) {
    const void* const modifier[] = {action, rt_swift_retain(context), nullptr, nullptr};
    write_modified_content(out, self, self_type, modifier, sizeof modifier);
}

SWIFTCC void view_on_disappear(SWIFT_OUT void* out, const void* action, void* context, void* self_type, void*, SWIFT_CONTEXT void* self) {
    const void* const modifier[] = {nullptr, nullptr, action, rt_swift_retain(context)};
    write_modified_content(out, self, self_type, modifier, sizeof modifier);
}

SWIFTCC void view_on_tap(SWIFT_OUT ViewNode** out, int64_t, const void* action, void* context, void* self_type, void*, SWIFT_CONTEXT void* self) {
    auto* node = new_node(NodeKind::Button);
    node->action = action;
    node->action_context = rt_swift_retain(context);
    add_self_child(node, self, self_type);
    *out = node;
}

// The action takes the tap's location, which only arrives in d0/d1 and which an untouched closure ignores.
SWIFTCC void view_on_tap_location(SWIFT_OUT ViewNode** out, int64_t count, uint8_t, const void* action, void* context, void* self_type, void* table, SWIFT_CONTEXT void* self) {
    view_on_tap(out, count, action, context, self_type, table, self);
}

// A row's context menu and swipe actions are reached from a menu next to the row.
SWIFTCC void view_row_menu(SWIFT_OUT ViewNode** out, const void* content, void* context, void* self_type, void* content_type, void*, void*, SWIFT_CONTEXT void* self) {
    auto* node = new_node(NodeKind::RowMenu);
    add_self_child(node, self, self_type);
    evaluate_child(node, content, context, content_type);
    *out = node;
}

SWIFTCC void view_swipe_actions(SWIFT_OUT ViewNode** out, uint8_t, bool, const void* content, void* context, void* self_type, void* content_type, void* a, void* b, SWIFT_CONTEXT void* self) {
    view_row_menu(out, content, context, self_type, content_type, a, b, self);
}

SWIFTCC void view_toolbar_title_menu(SWIFT_OUT ViewNode** out, const void* content, void* context, void* self_type, void*, void*, void*, SWIFT_CONTEXT void* self) {
    auto* node = new_node(NodeKind::Modifier);
    (void)content;
    (void)context;
    add_self_child(node, self, self_type);
    *out = node;
}

SWIFTCC void view_refreshable(SWIFT_OUT ViewNode** out, const void*, void*, void* self_type, void*, SWIFT_CONTEXT void* self) {
    auto* node = new_node(NodeKind::Modifier);
    add_self_child(node, self, self_type);
    *out = node;
}

SWIFTCC void view_on_open_url(SWIFT_OUT ViewNode** out, const void*, void*, void* self_type, void*, SWIFT_CONTEXT void* self) {
    auto* node = new_node(NodeKind::Modifier);
    add_self_child(node, self, self_type);
    *out = node;
}

SWIFTCC void view_tab_item(SWIFT_OUT ViewNode** out, const void* label, void* label_context, void* self_type, void* label_type, void*, void*, SWIFT_CONTEXT void* self) {
    auto* node = new_node(NodeKind::Tab);
    add_self_child(node, self, self_type);
    evaluate_child(node, label, label_context, label_type);
    node->lower = 1;
    *out = node;
}

void make_presented_sheet(ViewNode** out, BindingNode* location, const void* content, void* content_context, void* content_type, void* self, void* self_type) {
    auto* node = new_node(NodeKind::PresentedSheet);
    node->binding = static_cast<BindingNode*>(rt_swift_retain(location));
    node->action = content;
    node->action_context = rt_swift_retain(content_context);
    node->content_type = content_type;
    add_self_child(node, self, self_type);
    *out = node;
}

// Binding<Bool> arrives as {transaction, location, value}; the closure builds the presented view each time.
SWIFTCC void view_sheet_presented(SWIFT_OUT ViewNode** out, uint64_t, BindingNode* location, uint64_t, const void*, void*, const void* content, void* content_context, void* self_type, void* content_type, void*, SWIFT_CONTEXT void* self) {
    make_presented_sheet(out, location, content, content_context, content_type, self, self_type);
}

SWIFTCC void view_navigation_destination(SWIFT_OUT ViewNode** out, uint64_t, BindingNode* location, uint64_t, const void* content, void* content_context, void* self_type, void* content_type, void*, SWIFT_CONTEXT void* self) {
    make_presented_sheet(out, location, content, content_context, content_type, self, self_type);
}

void make_alert(ViewNode** out, std::string title, BindingNode* location, const void* actions, void* actions_context, void* actions_type, void* self_type, void* self) {
    auto* node = new_node(NodeKind::Alert);
    *node->title = std::move(title);
    node->binding = static_cast<BindingNode*>(rt_swift_retain(location));
    add_self_child(node, self, self_type);
    evaluate_child(node, actions, actions_context, actions_type);
    *out = node;
}

SWIFTCC void view_alert_key(SWIFT_OUT ViewNode** out, SwiftString key, uint64_t, LocalizedBox* interpolation, uint64_t, BindingNode* location, uint64_t, const void* actions, void* actions_context, void* self_type, void* actions_type, void*, void*, SWIFT_CONTEXT void* self) {
    make_alert(out, interpolation != nullptr ? localized_text(interpolation) : localization_lookup(swift_string_to_utf8(key)), location, actions, actions_context, actions_type, self_type, self);
}

SWIFTCC void view_alert_string(SWIFT_OUT ViewNode** out, const SwiftString* title, uint64_t, BindingNode* location, uint64_t, const void* actions, void* actions_context, void* self_type, void*, void* actions_type, void*, void*, SWIFT_CONTEXT void* self) {
    make_alert(out, swift_string_to_utf8(*title), location, actions, actions_context, actions_type, self_type, self);
}

// Binding<String> and the optional prompt fill the registers, so Self arrives on the stack.
SWIFTCC void view_searchable(SWIFT_OUT ViewNode** out, uint64_t, BindingNode* location, uint64_t, uint64_t, const void*, uint64_t, uint64_t, uint64_t, uint64_t, void* self_type, void*, SWIFT_CONTEXT void* self) {
    auto* node = new_node(NodeKind::Searchable);
    node->binding = static_cast<BindingNode*>(rt_swift_retain(location));
    add_self_child(node, self, self_type);
    *out = node;
}

// ModifiedContent<Self, _ValueActionModifier<V>>: the watched value, then the action.
SWIFTCC void view_on_change_perform(SWIFT_OUT char* out, void* value, const void* action, void* context, void* self_type, void* value_type, void*, void*, SWIFT_CONTEXT void* self) {
    const auto* content = witnesses_of(self_type);
    content->initialize_with_copy(out, self, self_type);
    char* modifier = out + ((content->size + 7) & ~static_cast<size_t>(7));
    const auto* watched = witnesses_of(value_type);
    watched->initialize_with_copy(modifier, value, value_type);
    const void* const closure[] = {action, rt_swift_retain(context)};
    memcpy(modifier + ((watched->size + 7) & ~static_cast<size_t>(7)), closure, sizeof closure);
}

// A transition only animates a view in and out, which the tree doesn't show.
SWIFTCC uint64_t transition_constant() { return kImmediate; }

SWIFTCC uint64_t transition_with_argument(uint64_t) { return kImmediate; }

// Constructors whose title is a key or string instead of a label view.
std::string key_text(SwiftString key, LocalizedBox* interpolation) {
    return interpolation != nullptr ? localized_text(interpolation) : localization_lookup(swift_string_to_utf8(key));
}

void make_titled_button(ViewNode** out, std::string title, const void* action, void* context) {
    auto* node = new_node(NodeKind::Button);
    *node->title = std::move(title);
    node->action = action;
    node->action_context = rt_swift_retain(context);
    *out = node;
}

SWIFTCC void button_key(SWIFT_OUT ViewNode** out, SwiftString key, uint64_t, LocalizedBox* interpolation, const void* action, void* context) {
    make_titled_button(out, key_text(key, interpolation), action, context);
}

SWIFTCC void button_key_role(SWIFT_OUT ViewNode** out, SwiftString key, uint64_t, LocalizedBox* interpolation, const void*, const void* action, void* context) {
    make_titled_button(out, key_text(key, interpolation), action, context);
}

SWIFTCC void button_string(SWIFT_OUT ViewNode** out, const SwiftString* title, const void* action, void* context, void*, void*) {
    make_titled_button(out, swift_string_to_utf8(*title), action, context);
}

void make_system_label(ViewNode** out, std::string title, SwiftString image) {
    auto* node = new_node(NodeKind::Label);
    *node->title = std::move(title);
    *node->system_image = swift_string_to_utf8(image);
    *out = node;
}

SWIFTCC void label_key_system(SWIFT_OUT ViewNode** out, SwiftString key, uint64_t, LocalizedBox* interpolation, SwiftString image) {
    make_system_label(out, key_text(key, interpolation), image);
}

SWIFTCC void label_string_system(SWIFT_OUT ViewNode** out, const SwiftString* title, SwiftString image, void*, void*) {
    make_system_label(out, swift_string_to_utf8(*title), image);
}

SWIFTCC ViewNode* image_named(SwiftString name, void*) {
    auto* node = new_node(NodeKind::Image);
    *node->title = swift_string_to_utf8(name);
    return node;
}

SWIFTCC void navigation_link_key(SWIFT_OUT ViewNode** out, SwiftString key, uint64_t, LocalizedBox* interpolation, void* destination, void* destination_type, void*) {
    auto* node = new_node(NodeKind::NavigationLink);
    *node->title = key_text(key, interpolation);
    add_value_child(node, destination, destination_type);
    *out = node;
}

SWIFTCC void navigation_link_label(SWIFT_OUT ViewNode** out, void* destination, const void* label, void* label_context, void* label_type, void* destination_type, void*, void*) {
    auto* node = new_node(NodeKind::NavigationLink);
    add_value_child(node, destination, destination_type);
    evaluate_child(node, label, label_context, label_type);
    *out = node;
}

void make_text_field(ViewNode** out, std::string title, BindingNode* location, bool secure) {
    auto* node = new_node(NodeKind::TextField);
    *node->title = std::move(title);
    node->binding = static_cast<BindingNode*>(rt_swift_retain(location));
    node->lower = secure ? 1 : 0;
    *out = node;
}

SWIFTCC void text_field_key(SWIFT_OUT ViewNode** out, SwiftString key, uint64_t, LocalizedBox* interpolation, uint64_t, BindingNode* location) {
    make_text_field(out, key_text(key, interpolation), location, false);
}

SWIFTCC void secure_field_key(SWIFT_OUT ViewNode** out, SwiftString key, uint64_t, LocalizedBox* interpolation, uint64_t, BindingNode* location) {
    make_text_field(out, key_text(key, interpolation), location, true);
}

SWIFTCC void text_field_string(SWIFT_OUT ViewNode** out, const SwiftString* title, uint64_t, BindingNode* location) {
    make_text_field(out, swift_string_to_utf8(*title), location, false);
}

SWIFTCC void for_each_init_identifiable(SWIFT_OUT ViewNode** out, void* data, const void* row, void* row_context, void* data_type, void* id_type, void* row_type) {
    for_each_identifiable(out, data, row, row_context, data_type, id_type, row_type);
}

// Section's forms differ only in which of header, content and footer they take, and how.
ViewNode* new_section() { return new_node(NodeKind::Section); }

SWIFTCC void section_header_view(SWIFT_OUT ViewNode** out, void* header, const void* content, void* context, void* header_type, void* content_type, void*, void*) {
    auto* node = new_section();
    add_value_child(node, header, header_type);
    evaluate_child(node, content, context, content_type);
    *out = node;
}

SWIFTCC void section_content(SWIFT_OUT ViewNode** out, const void* content, void* context, void* content_type, void*) {
    auto* node = new_section();
    evaluate_child(node, content, context, content_type);
    *out = node;
}

SWIFTCC void section_footer_view(SWIFT_OUT ViewNode** out, void* footer, const void* content, void* context, void* content_type, void* footer_type, void*, void*) {
    auto* node = new_section();
    evaluate_child(node, content, context, content_type);
    add_value_child(node, footer, footer_type);
    *out = node;
}

SWIFTCC void section_header_footer(SWIFT_OUT ViewNode** out, void* header, void* footer, const void* content, void* context, void* header_type, void* content_type, void* footer_type, void*) {
    auto* node = new_section();
    add_value_child(node, header, header_type);
    evaluate_child(node, content, context, content_type);
    add_value_child(node, footer, footer_type);
    *out = node;
}

SWIFTCC void section_title_string(SWIFT_OUT ViewNode** out, const SwiftString* title, const void* content, void* context, void* content_type, void*, void*, void*) {
    auto* node = new_section();
    *node->title = swift_string_to_utf8(*title);
    evaluate_child(node, content, context, content_type);
    *out = node;
}

SWIFTCC void section_title_key(SWIFT_OUT ViewNode** out, SwiftString key, uint64_t, LocalizedBox* interpolation, const void* content, void* context, void* content_type, void*) {
    auto* node = new_section();
    *node->title = key_text(key, interpolation);
    evaluate_child(node, content, context, content_type);
    *out = node;
}

// Group is frozen: just its content, in place.
SWIFTCC void group_init(SWIFT_OUT void* out, const void* content, void* context, void*, void*) { rt_call_witness(content, out, context, nullptr, nullptr); }

SWIFTCC void scroll_view_init(SWIFT_OUT ViewNode** out, uint8_t, bool, const void* content, void* context, void* content_type, void*) {
    auto* node = new_node(NodeKind::List);
    evaluate_child(node, content, context, content_type);
    *out = node;
}

// ZStack is frozen: its alignment, then the content in place.
SWIFTCC void z_stack_init(SWIFT_OUT char* out, uint64_t horizontal, uint64_t vertical, const void* content, void* context, void* content_type, void*) {
    memcpy(out, &horizontal, sizeof horizontal);
    memcpy(out + sizeof horizontal, &vertical, sizeof vertical);
    rt_call_witness(content, out + sizeof horizontal + sizeof vertical, context, nullptr, nullptr);
    (void)content_type;
}

SWIFTCC void lazy_stack_init(SWIFT_OUT ViewNode** out, uint64_t, uint64_t, bool, uint8_t, const void* content, void* context, void* content_type, void*) {
    auto* node = new_node(NodeKind::Stack);
    evaluate_child(node, content, context, content_type);
    *out = node;
}

SWIFTCC void lazy_grid_init(SWIFT_OUT ViewNode** out, void*, uint64_t, uint64_t, bool, uint8_t, const void* content, void* context, void* content_type) {
    auto* node = new_node(NodeKind::Stack);
    evaluate_child(node, content, context, content_type);
    *out = node;
}

SWIFTCC void divider_init(SWIFT_OUT ViewNode** out) { *out = new_node(NodeKind::Divider); }

SWIFTCC void* any_view_init(void* value, void* type, void*) {
    auto* node = new_node(NodeKind::Modifier);
    add_self_child(node, value, type);
    witnesses_of(type)->destroy(value, type);
    return node;
}

SWIFTCC void tab_view_selection(SWIFT_OUT ViewNode** out, void*, const void* closure, void* context, void*, void* content_type, void*, void*) {
    auto* node = new_node(NodeKind::TabView);
    evaluate_child(node, closure, context, content_type);
    *out = node;
}

// _ConditionalContent and its Storage share one layout: the larger branch, then the tag.
SWIFTCC void conditional_from_storage(SWIFT_OUT void* out, void* storage, void* true_type, void* false_type) {
    const size_t larger = std::max(witnesses_of(true_type)->size, witnesses_of(false_type)->size);
    memcpy(out, storage, larger + 1);
}

// buildBlock's result is opaque to the app, which only sees one word: a node holding the real content.
SWIFTCC void toolbar_build_block(SWIFT_OUT ViewNode** out, void* value, void* type, void*) {
    auto* node = new_node(NodeKind::Modifier);
    add_self_child(node, value, type);
    *out = node;
}

// A builder result is the value itself, for toolbar content as for views.
SWIFTCC void builder_identity(SWIFT_OUT void* out, void* value, void* type, void*) { witnesses_of(type)->initialize_with_copy(out, value, type); }

// Layout-only descriptions: nothing reads them, but the app passes them around as one word.
SWIFTCC uint64_t immediate_value() { return kImmediate; }

SWIFTCC void with_transaction(SWIFT_OUT void* out, void*, const void* body, void* context) { rt_call_witness(body, out, context, nullptr, nullptr); }

struct FetchRequestBox {
    HeapObject header;
    std::string* entity;
    void* sort_descriptors;
    void* predicate;
};

void destroy_fetch_request(HeapObject* object) {
    auto* box = reinterpret_cast<FetchRequestBox*>(object);
    delete box->entity;
    free(box);
}

const BoxMetadata kFetchRequestMetadata = {destroy_fetch_request, nullptr, 0};

SWIFTCC void fetch_request_init(SWIFT_OUT FetchRequestBox** out, void* entity, void* sort_descriptors, void* predicate, void*, void*) {
    auto* box = static_cast<FetchRequestBox*>(calloc(1, sizeof(FetchRequestBox)));
    box->header.metadata = const_cast<uint64_t*>(&kFetchRequestMetadata.kind);
    new (&box->header.references) std::atomic<uint64_t>(1);
    box->entity = new std::string(coredata_entity_name(entity));
    box->sort_descriptors = sort_descriptors;
    box->predicate = predicate;
    *out = box;
}

void destroy_fetched_results(HeapObject* object) {
    auto* box = reinterpret_cast<FetchedResultsBox*>(object);
    delete box->objects;
    free(box);
}

const BoxMetadata kFetchedResultsMetadata = {destroy_fetched_results, nullptr, 0};

SWIFTCC void fetch_request_wrapped(SWIFT_OUT FetchedResultsBox** out, void*, SWIFT_CONTEXT FetchRequestBox** self) {
    auto* box = static_cast<FetchedResultsBox*>(calloc(1, sizeof(FetchedResultsBox)));
    box->header.metadata = const_cast<uint64_t*>(&kFetchedResultsMetadata.kind);
    new (&box->header.references) std::atomic<uint64_t>(1);
    box->objects = new std::vector<void*>(coredata_fetch(*(*self)->entity, (*self)->predicate, (*self)->sort_descriptors));
    *out = box;
}

const std::vector<void*>& fetched_results_contents(void* value) { return *(*static_cast<FetchedResultsBox**>(value))->objects; }

SWIFTCC void fetched_make_iterator(SWIFT_OUT IndexingIteratorValue* out, void*, void*, SWIFT_CONTEXT FetchedResultsBox** self) { *out = {rt_swift_retain(*self), 0}; }

SWIFTCC void fetched_start_index(SWIFT_OUT int64_t* out, void*, void*, SWIFT_CONTEXT void*) { *out = 0; }

SWIFTCC void fetched_end_index(SWIFT_OUT int64_t* out, void*, void*, SWIFT_CONTEXT FetchedResultsBox** self) { *out = static_cast<int64_t>((*self)->objects->size()); }

SWIFTCC int64_t fetched_count(void*, void*, SWIFT_CONTEXT FetchedResultsBox** self) { return static_cast<int64_t>((*self)->objects->size()); }

SWIFTCC bool fetched_is_empty(void*, void*, SWIFT_CONTEXT FetchedResultsBox** self) { return (*self)->objects->empty(); }

SWIFTCC int64_t fetched_distance(const int64_t* from, const int64_t* to, void*, void*, SWIFT_CONTEXT void*) { return *to - *from; }

SWIFTCC void fetched_index_offset(SWIFT_OUT int64_t* out, const int64_t* index, int64_t distance, void*, void*, SWIFT_CONTEXT void*) { *out = *index + distance; }

SWIFTCC void fetched_form_index(int64_t* index, void*, void*, SWIFT_CONTEXT void*) { ++*index; }

SWIFTCC void fetched_index_after(SWIFT_OUT int64_t* out, const int64_t* index, void*, void*, SWIFT_CONTEXT void*) { *out = *index + 1; }

SWIFTCC void* fetched_copy_to_array(void*, void*, SWIFT_CONTEXT FetchedResultsBox** self) {
    const auto& objects = *(*self)->objects;
    char* elements;
    void* storage = make_array_storage(type_system_symbol("$syXlN"), objects.size(), &elements);
    for (size_t i = 0; i < objects.size(); ++i) {
        void* object = rt_swift_retain(objects[i]);
        memcpy(elements + i * sizeof object, &object, sizeof object);
    }
    return storage;
}

SWIFTCC int64_t fetched_underestimated_count(void*, void*, SWIFT_CONTEXT FetchedResultsBox** self) { return static_cast<int64_t>((*self)->objects->size()); }

void add_swiftui_modifiers_symbols(SymbolTable& table) {
    table.insert({
        {"$s7SwiftUI4ViewPAAE7paddingyQrAA4EdgeO3SetV_12CoreGraphics7CGFloatVSgtF", reinterpret_cast<void*>(view_padding)},
        {"$s7SwiftUI4ViewPAAE4fontyQrAA4FontVSgF", reinterpret_cast<void*>(view_environment_word)},
        {"$s7SwiftUI4ViewPAAE15foregroundColoryQrAA0E0VSgF", reinterpret_cast<void*>(view_environment_word)},
        {"$s7SwiftUI4ViewPAAE10transitionyQrAA13AnyTransitionVF", reinterpret_cast<void*>(view_transition)},
        {"$s7SwiftUI4ViewPAAE8onAppear7performQryycSg_tF", reinterpret_cast<void*>(view_on_appear)},
        {"$s7SwiftUI4ViewPAAE11onDisappear7performQryycSg_tF", reinterpret_cast<void*>(view_on_disappear)},
        {"$s7SwiftUI4ViewPAAE12onTapGesture5count7performQrSi_yyctF", reinterpret_cast<void*>(view_on_tap)},
        {"$s7SwiftUI4ViewPAAE12onTapGesture5count15coordinateSpace7performQrSi_AA010CoordinateI0OySo7CGPointVctF", reinterpret_cast<void*>(view_on_tap_location)},
        {"$s7SwiftUI4ViewPAAE11contextMenu9menuItemsQrqd__yXE_tAaBRd__lF", reinterpret_cast<void*>(view_row_menu)},
        {"$s7SwiftUI4ViewPAAE12swipeActions4edge15allowsFullSwipe7contentQrAA14HorizontalEdgeO_Sbqd__yXEtAaBRd__lF", reinterpret_cast<void*>(view_swipe_actions)},
        {"$s7SwiftUI4ViewPAAE16toolbarTitleMenu7contentQrqd__yXE_tAaBRd__lF", reinterpret_cast<void*>(view_toolbar_title_menu)},
        {"$s7SwiftUI4ViewPAAE11refreshable6actionQryyYaYbc_tF", reinterpret_cast<void*>(view_refreshable)},
        {"$s7SwiftUI4ViewPAAE9onOpenURL7performQry10Foundation0F0Vc_tF", reinterpret_cast<void*>(view_on_open_url)},
        {"$s7SwiftUI4ViewPAAE7tabItemyQrqd__yXEAaBRd__lF", reinterpret_cast<void*>(view_tab_item)},
        {"$s7SwiftUI4ViewPAAE5sheet11isPresented9onDismiss7contentQrAA7BindingVySbG_yycSgqd__yctAaBRd__lF", reinterpret_cast<void*>(view_sheet_presented)},
        {"$s7SwiftUI4ViewPAAE15fullScreenCover11isPresented9onDismiss7contentQrAA7BindingVySbG_yycSgqd__yctAaBRd__lF", reinterpret_cast<void*>(view_sheet_presented)},
        {"$s7SwiftUI4ViewPAAE21navigationDestination11isPresented11destinationQrAA7BindingVySbG_qd__yXEtAaBRd__lF", reinterpret_cast<void*>(view_navigation_destination)},
        {"$s7SwiftUI4ViewPAAE5alert_11isPresented7actionsQrAA18LocalizedStringKeyV_AA7BindingVySbGqd__yXEtAaBRd__lF", reinterpret_cast<void*>(view_alert_key)},
        {"$s7SwiftUI4ViewPAAE5alert_11isPresented7actionsQrqd___AA7BindingVySbGqd_0_yXEtSyRd__AaBRd_0_r0_lF", reinterpret_cast<void*>(view_alert_string)},
        {"$s7SwiftUI13AnyTransitionV5scaleACvgZ", reinterpret_cast<void*>(transition_constant)},
        {"$s7SwiftUI13AnyTransitionV5slideACvgZ", reinterpret_cast<void*>(transition_constant)},
        {"$s7SwiftUI13AnyTransitionV7opacityACvgZ", reinterpret_cast<void*>(transition_constant)},
        {"$s7SwiftUI13AnyTransitionV4move4edgeAcA4EdgeO_tFZ", reinterpret_cast<void*>(transition_with_argument)},
        {"$s7SwiftUI13AnyTransitionV8combined4withA2C_tF", reinterpret_cast<void*>(transition_with_argument)},
        {"$s7SwiftUI6ButtonVA2A4TextVRszrlE_6actionACyAEGAA18LocalizedStringKeyV_yyctcfC", reinterpret_cast<void*>(button_key)},
        {"$s7SwiftUI6ButtonVA2A4TextVRszrlE_4role6actionACyAEGAA18LocalizedStringKeyV_AA0C4RoleVSgyyctcfC", reinterpret_cast<void*>(button_key_role)},
        {"$s7SwiftUI6ButtonVA2A4TextVRszrlE_6actionACyAEGqd___yyctcSyRd__lufC", reinterpret_cast<void*>(button_string)},
        {"$s7SwiftUI5LabelVA2A4TextVRszAA5ImageVRs_rlE_06systemE0ACyAeGGAA18LocalizedStringKeyV_SStcfC", reinterpret_cast<void*>(label_key_system)},
        {"$s7SwiftUI5LabelVA2A4TextVRszAA5ImageVRs_rlE_06systemE0ACyAeGGqd___SStcSyRd__lufC", reinterpret_cast<void*>(label_string_system)},
        {"$s7SwiftUI5ImageV_6bundleACSS_So8NSBundleCSgtcfC", reinterpret_cast<void*>(image_named)},
        {"$s7SwiftUI14NavigationLinkVA2A4TextVRszrlE_11destinationACyAEq_GAA18LocalizedStringKeyV_q_tcfC", reinterpret_cast<void*>(navigation_link_key)},
        {"$s7SwiftUI14NavigationLinkV11destination5labelACyxq_Gq__xyXEtcfC", reinterpret_cast<void*>(navigation_link_label)},
        {"$s7SwiftUI7ForEachVAA7Element_2IDQZRs_AA4ViewR0_s12IdentifiableADRpzrlE_7contentACyxq_q0_Gx_q0_AIctcfC", reinterpret_cast<void*>(for_each_init_identifiable)},
        {"$s7SwiftUI7SectionVA2A4ViewRzAaDR_AA05EmptyD0VRs0_rlE6header7contentACyxq_AFGx_q_yXEtcfC", reinterpret_cast<void*>(section_header_view)},
        {"$s7SwiftUI7SectionVA2A9EmptyViewVRszAA0E0R_AERs0_rlE7contentACyAEq_AEGq_yXE_tcfC", reinterpret_cast<void*>(section_content)},
        {"$s7SwiftUI7SectionVA2A9EmptyViewVRszAA0E0R_AaFR0_rlE6footer7contentACyAEq_q0_Gq0__q_yXEtcfC", reinterpret_cast<void*>(section_footer_view)},
        {"$s7SwiftUI7SectionVA2A4ViewRzAaDR_AaDR0_rlE6header6footer7contentACyxq_q0_Gx_q0_q_yXEtcfC", reinterpret_cast<void*>(section_header_footer)},
        {"$s7SwiftUI7SectionVA2A4TextVRszAA4ViewR_AA05EmptyE0VRs0_rlE_7contentACyAEq_AHGqd___q_yXEtcSyRd__lufC", reinterpret_cast<void*>(section_title_string)},
        {"$s7SwiftUI7SectionVA2A4TextVRszAA4ViewR_AA05EmptyE0VRs0_rlE_7contentACyAEq_AHGAA18LocalizedStringKeyV_q_yXEtcfC", reinterpret_cast<void*>(section_title_key)},
        {"$s7SwiftUI5GroupVA2A4ViewRzlE7contentACyxGxyXE_tcfC", reinterpret_cast<void*>(group_init)},
        {"$s7SwiftUI10ScrollViewV_15showsIndicators7contentACyxGAA4AxisO3SetV_SbxyXEtcfC", reinterpret_cast<void*>(scroll_view_init)},
        {"$s7SwiftUI6ZStackV9alignment7contentACyxGAA9AlignmentV_xyXEtcfC", reinterpret_cast<void*>(z_stack_init)},
        {"$s7SwiftUI7DividerVACycfC", reinterpret_cast<void*>(divider_init)},
        {"$s7SwiftUI7AnyViewVyACxcAA0D0RzlufC", reinterpret_cast<void*>(any_view_init)},
        {"$s7SwiftUI7TabViewV9selection7contentACyxq_GAA7BindingVyxGSg_q_yXEtcfC", reinterpret_cast<void*>(tab_view_selection)},
        {"$s7SwiftUI19_ConditionalContentVA2A4ViewRzAaDR_rlE7storageACyxq_GAC7StorageOyxq__G_tcfC", reinterpret_cast<void*>(conditional_from_storage)},
        {"$s7SwiftUI21ToolbarContentBuilderV10buildBlockyQrxAA0cD0RzlFZ", reinterpret_cast<void*>(toolbar_build_block)},
        {"$s7SwiftUI19TupleToolbarContentVyACyxGxcfC", reinterpret_cast<void*>(builder_identity)},
        {"$s7SwiftUI15withTransactionyxAA0D0V_xyKXEtKlF", reinterpret_cast<void*>(with_transaction)},
        {"$s7SwiftUI12FetchRequestV6entity15sortDescriptors9predicate9animationACyxGSo19NSEntityDescriptionC_SaySo16NSSortDescriptorCGSo11NSPredicateCSgAA9AnimationVSgtcfC", reinterpret_cast<void*>(fetch_request_init)},
        {"$s7SwiftUI12FetchRequestV12wrappedValueAA14FetchedResultsVyxGvg", reinterpret_cast<void*>(fetch_request_wrapped)},
        {"$s7SwiftUI9AnimationV6easeIn8durationACSd_tFZ", reinterpret_cast<void*>(immediate_value)},
        {"$s7SwiftUI9AnimationV9easeInOut8durationACSd_tFZ", reinterpret_cast<void*>(immediate_value)},
        {"$s7SwiftUI9AnimationV13repeatForever12autoreversesACSb_tF", reinterpret_cast<void*>(immediate_value)},
        {"$s7SwiftUI9AnimationV6spring8response15dampingFraction13blendDurationACSd_S2dtFZ", reinterpret_cast<void*>(immediate_value)},
        {"$s7SwiftUI11TransactionV9animationAcA9AnimationVSg_tcfC", reinterpret_cast<void*>(immediate_value)},
        {"$s7SwiftUI8MaterialV9ultraThinACvgZ", reinterpret_cast<void*>(immediate_value)},
        {"$s7SwiftUI15SafeAreaRegionsV3allACvgZ", reinterpret_cast<void*>(immediate_value)},
        {"$s7SwiftUI4ViewPAAE10searchable4text9placement6promptQrAA7BindingVySSG_AA20SearchFieldPlacementVAA4TextVSgtF", reinterpret_cast<void*>(view_searchable)},
        {"$s7SwiftUI10LazyVStackV9alignment7spacing11pinnedViews7contentACyxGAA19HorizontalAlignmentV_12CoreGraphics7CGFloatVSgAA016PinnedScrollableH0VxyXEtcfC", reinterpret_cast<void*>(lazy_stack_init)},
        {"$s7SwiftUI10LazyHStackV9alignment7spacing11pinnedViews7contentACyxGAA17VerticalAlignmentV_12CoreGraphics7CGFloatVSgAA016PinnedScrollableH0VxyXEtcfC", reinterpret_cast<void*>(lazy_stack_init)},
        {"$s7SwiftUI9LazyVGridV7columns9alignment7spacing11pinnedViews7contentACyxGSayAA8GridItemVG_AA19HorizontalAlignmentV12CoreGraphics7CGFloatVSgAA016PinnedScrollableI0VxyXEtcfC", reinterpret_cast<void*>(lazy_grid_init)},
        {"$s7SwiftUI9LazyHGridV4rows9alignment7spacing11pinnedViews7contentACyxGSayAA8GridItemVG_AA17VerticalAlignmentV12CoreGraphics7CGFloatVSgAA016PinnedScrollableI0VxyXEtcfC", reinterpret_cast<void*>(lazy_grid_init)},
        {"$s7SwiftUI4FontV6system4size6weight6designAC12CoreGraphics7CGFloatV_AC6WeightVSgAC6DesignOSgtFZ", reinterpret_cast<void*>(immediate_value)},
        {"$s7SwiftUI5ColorV02uiC0ACSo7UIColorC_tcfC", reinterpret_cast<void*>(immediate_value)},
        {"$s7SwiftUI5ColorVyACSo7UIColorCcfC", reinterpret_cast<void*>(immediate_value)},
        {"$s7SwiftUI5ColorV_3red5green4blue7opacityA2C13RGBColorSpaceO_S4dtcfC", reinterpret_cast<void*>(immediate_value)},
        {"$s7SwiftUI5ColorV7opacityyACSdF", reinterpret_cast<void*>(immediate_value)},
        {"$s7SwiftUI8GradientV6colorsACSayAA5ColorVG_tcfC", reinterpret_cast<void*>(immediate_value)},
        {"$s7SwiftUI9NamespaceV12wrappedValueAC2IDVvg", reinterpret_cast<void*>(immediate_value)},
        {"$s7SwiftUI9TextFieldVA2A0C0VRszrlE_4text16onEditingChanged0F6CommitACyAEGAA18LocalizedStringKeyV_AA7BindingVySSGySbcyyctcfC", reinterpret_cast<void*>(text_field_key)},
        {"$s7SwiftUI9TextFieldVA2A0C0VRszrlE_4text16onEditingChanged0F6CommitACyAEGqd___AA7BindingVySSGySbcyyctcSyRd__lufC", reinterpret_cast<void*>(text_field_string)},
        {"$s7SwiftUI11SecureFieldVA2A4TextVRszrlE_4text8onCommitACyAEGAA18LocalizedStringKeyV_AA7BindingVySSGyyctcfC", reinterpret_cast<void*>(secure_field_key)},
        {"$s7SwiftUI4ViewPAAE8onChange2of7performQrqd___yqd__ctSQRd__lF", reinterpret_cast<void*>(view_on_change_perform)},
    });
}

}  // namespace swiftui
