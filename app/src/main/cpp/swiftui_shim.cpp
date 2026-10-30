#include "swiftui_shim.h"

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iterator>
#include <string>
#include <vector>
#include <map>
#include <new>
#include <set>
#include <unordered_map>
#include <unordered_set>

#include "concurrency_shim.h"
#include "engine.h"
#include "foundation_shim.h"
#include "key_path.h"
#include "localization.h"
#include "macho.h"
#include "swift_runtime.h"
#include "type_system.h"

#define SWIFTCC __attribute__((swiftcall))
#define SWIFT_OUT __attribute__((swift_indirect_result))
#define SWIFT_CONTEXT __attribute__((swift_context))

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

constexpr uint64_t kImmediate = 1ULL << 63;
const int32_t kStorageCaseValueTag = 0;
const int32_t kTitleDisplayInline = 2;
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

SWIFTCC void registrar_mark_dirty();

enum class BindingKind : uint32_t { State, Closures, ObjectKeyPath, BindingKeyPath };

// A two-way reference to a value, in whichever of SwiftUI's forms the app created it.
struct BindingNode {
    HeapHeader header;
    BindingKind kind;
    void* source;  // The state box, parent binding or observed object, retained.
    void* key_path;
    void* parent_type;
    const void* getter;
    void* getter_context;
    const void* setter;
    void* setter_context;
};

extern "C" void* rt_swift_retain(void*);
extern "C" void rt_swift_release(void*);
extern "C" void* rt_swift_bridgeObjectRetain(void*);

void destroy_binding(HeapHeader* object) {
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

struct TextValue {
    uint64_t text_bits;
    uint64_t text_object;
    uint64_t kind;
    uint64_t style;
};

struct LocalizedKey {
    SwiftString key;
    uint64_t has_formatting;
    LocalizedBox* interpolation;
};

struct ChildValue {
    void* value;
    void* type;
};

enum class NodeKind : uint32_t {
    TabView, Tab, NavigationStack, List, Button, Label, Toggle, Modifier,
    Sheet, Toolbar, OnChange, Task, Picker, ForEach, Section, ProgressView, Stepper, Image, AsyncImage,
    Disclosure, LabeledContent, Unavailable, Menu, Link, ToolbarItem, ImageResource,
};

// Runtime object behind a resilient view: its properties plus already-evaluated child values.
struct ViewNode {
    HeapHeader header;
    NodeKind kind;
    std::string* title;
    std::string* system_image;
    std::vector<ChildValue>* children;
    const void* action;
    void* action_context;
    BindingNode* binding;
    std::string* detail;
    void* item_type;
    void* content_type;
    int64_t lower;
    int64_t upper;
    int64_t step;
};

// Framework values can hold plain numbers in the same word as a node, so nodes are identified by registry.
std::unordered_set<const void*>& live_nodes() {
    static std::unordered_set<const void*> nodes;
    return nodes;
}

void destroy_node(HeapHeader* object) {
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

SWIFTCC void button_init(SWIFT_OUT ViewNode** out, const void* action, void* action_context, const void* label, void* label_context, void* label_type, void*) {
    auto* node = new_node(NodeKind::Button);
    node->action = action;
    node->action_context = rt_swift_retain(action_context);
    evaluate_child(node, label, label_context, label_type);
    *out = node;
}

SWIFTCC void button_role_init(SWIFT_OUT ViewNode** out, uint64_t, const void* action, void* action_context, const void* label, void* label_context, void* label_type, void*) {
    auto* node = new_node(NodeKind::Button);
    node->action = action;
    node->action_context = rt_swift_retain(action_context);
    evaluate_child(node, label, label_context, label_type);
    *out = node;
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
constexpr uint64_t kCollectionPointerMask = 0x00fffffffffffff8ULL;

// Evaluates the row closure for each element, which needs Data to be an Array.
void evaluate_rows(ViewNode* node, void* data, void* data_type, const void* row, void* row_context, void* row_type) {
    void* element = array_element_type(data_type);
    if (element == nullptr) {
        engine_log("STOP: ForEach over a collection other than Array");
        abort();
    }
    const auto* elements = witnesses_of(element);
    const auto storage = reinterpret_cast<const char*>(*static_cast<uint64_t*>(data) & kCollectionPointerMask);
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

SWIFTCC void disclosure_init(SWIFT_OUT ViewNode** out, const void* content, void* content_context, const void* label, void* label_context, void* label_type, void* content_type) {
    auto* node = new_node(NodeKind::Disclosure);
    evaluate_child(node, label, label_context, label_type);
    evaluate_child(node, content, content_context, content_type);
    *out = node;
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

// Backing store of @State, attached to the inline LazyState through its location word.
struct StateBox {
    HeapHeader header;
    void* type;
    void* value;
};

const BoxMetadata kStateMetadata = {destroy_box, nullptr, 0};

constexpr uint8_t kStorageCaseThunk = 1;
constexpr uint32_t kLazyStateLocationField = 1;
constexpr size_t kLazyStateOffsetsWord = 3;

size_t lazy_state_location_offset(void* metadata) {
    const auto* offsets = reinterpret_cast<const uint32_t*>(static_cast<void**>(metadata) + kLazyStateOffsetsWord);
    return offsets[kLazyStateLocationField];
}

void* state_value(StateBox* state) { return state->value; }

// The first read fixes the state: a thunk runs once, and later copies of the view share the box.
StateBox* state_box_for(char* self, void* metadata) {
    void* type = generic_argument(metadata);
    auto** location = reinterpret_cast<StateBox**>(self + lazy_state_location_offset(metadata));
    if (*location != nullptr) return *location;
    const auto* witnesses = witnesses_of(type);
    auto* state = static_cast<StateBox*>(calloc(1, sizeof(StateBox)));
    state->header.metadata = const_cast<uint64_t*>(&kStateMetadata.kind);
    new (&state->header.references) std::atomic<uint64_t>(1);
    state->type = type;
    state->value = calloc(1, witnesses->size == 0 ? 1 : witnesses->size);
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

// Framework constants are tagged immediates: the high bit makes retain and release ignore them.
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

SWIFTCC uint64_t font_bold(__attribute__((swift_context)) void* font) { return reinterpret_cast<uint64_t>(font) | kFontBold; }
SWIFTCC uint64_t font_monospaced(__attribute__((swift_context)) void* font) { return reinterpret_cast<uint64_t>(font) | kFontMonospaced; }

std::atomic<bool> g_tree_dirty{true};

SWIFTCC void registrar_init(SWIFT_OUT uint64_t* out) { *out = kImmediate; }
SWIFTCC void registrar_access() {}
SWIFTCC void registrar_mark_dirty() { g_tree_dirty.store(true, std::memory_order_relaxed); }

extern "C" void rt_registrar_with_mutation();

void binding_read(BindingNode* binding, void* out, void* type);
void binding_write(BindingNode* binding, void* value, void* type);

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

constexpr uint64_t kEnvironmentImmediate = kImmediate;

SWIFTCC void environment_values_init(SWIFT_OUT uint64_t* out) { *out = kEnvironmentImmediate; }
SWIFTCC bool environment_is_enabled(SWIFT_CONTEXT void*) { return true; }
SWIFTCC void environment_set_ignored() {}

// Optional<AnyShapeStyle> is one reference word here; null means no tint.
SWIFTCC void* environment_tint(SWIFT_CONTEXT void*) { return nullptr; }

SWIFTCC void environment_action_get(SWIFT_OUT uint64_t* out, SWIFT_CONTEXT void*) { *out = kImmediate; }

void dismiss_presented_sheet();

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

// Text: words 0-1 hold the resolved string, and the last word packs font and colour as an immediate.
constexpr uint64_t kTextFontMask = 0x3ffff;
constexpr unsigned kTextColorShift = 24;
constexpr uint64_t kTextColorMask = 0xffULL << kTextColorShift;

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

const char* node_kind_name(NodeKind kind) {
    switch (kind) {
        case NodeKind::TabView: return "TabView";
        case NodeKind::Tab: return "Tab";
        case NodeKind::NavigationStack: return "NavigationStack";
        case NodeKind::List: return "List";
        case NodeKind::Button: return "Button";
        case NodeKind::Label: return "Label";
        case NodeKind::Toggle: return "Toggle";
        case NodeKind::Modifier: return "Modifier";
        case NodeKind::Sheet: return "Sheet";
        case NodeKind::Toolbar: return "Toolbar";
        case NodeKind::OnChange: return "OnChange";
        case NodeKind::Task: return "Task";
        case NodeKind::Picker: return "Picker";
        case NodeKind::ForEach: return "ForEach";
        case NodeKind::Section: return "Section";
        case NodeKind::ProgressView: return "ProgressView";
        case NodeKind::Stepper: return "Stepper";
        case NodeKind::Image: return "Image";
        case NodeKind::AsyncImage: return "AsyncImage";
        case NodeKind::Disclosure: return "Disclosure";
        case NodeKind::LabeledContent: return "LabeledContent";
        case NodeKind::Unavailable: return "Unavailable";
        case NodeKind::Menu: return "Menu";
        case NodeKind::Link: return "Link";
        case NodeKind::ToolbarItem: return "ToolbarItem";
        case NodeKind::ImageResource: return "ImageResource";
    }
    return "?";
}

constexpr const char* kViewProtocolSymbol = "_$s7SwiftUI4ViewMp";
constexpr const char* kRepresentableSymbol = "_$s7SwiftUI19UIViewRepresentableMp";
constexpr size_t kConformanceTypeOffset = 4;
constexpr size_t kConformanceFlagsOffset = 12;
constexpr uint32_t kTypeReferenceKindMask = 0x7u << 3;
constexpr uint32_t kTypeReferenceIndirect = 0x1u << 3;
constexpr size_t kMetadataDescriptorWord = 1;
constexpr size_t kMetadataArgumentsWord = 2;
constexpr size_t kDescriptorNameOffset = 8;

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

extern "C" void* rt_call_make_ui_view(const void* function, void* context, void* self, void* type, void** table);

const char* find_view_conformance(const void* type) { return find_conformance(type, kViewProtocolSymbol); }

constexpr uint64_t kTupleKind = 0x301;
constexpr const char* kTextSymbol = "_$s7SwiftUI4TextVMn";
constexpr const char* kModifiedContentSymbol = "_$s7SwiftUI15ModifiedContentVMn";
constexpr const char* kTagModifierSymbol = "_$s7SwiftUI24_TagTraitWritingModifierVMn";
constexpr const char* kAppearanceSymbol = "_$s7SwiftUI25_AppearanceActionModifierVMn";
constexpr const char* kMakeUIViewRequirement = "_$s7SwiftUI19UIViewRepresentableP04makeC07context0C4TypeQzAA0cD7ContextVyxG_tFTq";
constexpr size_t kRepresentableContextBytes = 256;
BindingNode* g_picker_binding = nullptr;

bool is_runtime_node_type(void* type) {
    const auto* witnesses = witnesses_of(type);
    return foreign_type_name(type) != nullptr && witnesses->size == sizeof(void*) && (witnesses->flags & kFlagNonPod) != 0;
}

// Only fields with their own metadata can be views: plain bytes in a modifier or stack have none worth walking.
bool is_view_like(void* type) {
    const uint64_t kind = *static_cast<uint64_t*>(type);
    if (kind == kTupleKind) return true;
    if (foreign_type_name(type) != nullptr) return true;
    return find_view_conformance(type) != nullptr || find_conformance(type, kRepresentableSymbol) != nullptr;
}

std::string json_string(const std::string& text) {
    std::string out = "\"";
    for (const char c : text) {
        const auto byte = static_cast<unsigned char>(c);
        if (c == '"' || c == '\\') {
            out += '\\';
            out += c;
        } else if (c == '\n') {
            out += "\\n";
        } else if (byte < 0x20) {
            char escaped[8];
            snprintf(escaped, sizeof escaped, "\\u%04x", byte);
            out += escaped;
        } else {
            out += c;
        }
    }
    return out + "\"";
}

enum class ActionKind { Closure, Toggle, StepBy, SetValue, DismissSheet, OpenUrl };

struct Action {
    ActionKind kind;
    const void* function;
    void* context;
    BindingNode* binding;
    void* value;
    void* type;
    int64_t amount;
    std::string text;
};

std::vector<Action> g_actions;
BindingNode* g_presented_sheet = nullptr;
void* g_presented_sheet_type = nullptr;

int add_action(Action action) {
    g_actions.push_back(std::move(action));
    return static_cast<int>(g_actions.size()) - 1;
}

void* bool_metadata() {
    static void* metadata = type_system_symbol("$sSbN");
    return metadata;
}

void* int_metadata() {
    static void* metadata = type_system_symbol("$sSiN");
    return metadata;
}

void* string_metadata() {
    static void* metadata = type_system_symbol("$sSSN");
    return metadata;
}

std::map<std::string, StateBox*>& preserved_states() {
    static std::map<std::string, StateBox*> states;
    return states;
}

constexpr size_t kDescriptorFieldsOffset = 16;
constexpr size_t kStructFieldOffsetVectorOffset = 24;
constexpr size_t kFieldDescriptorHeaderBytes = 16;
constexpr size_t kFieldRecordBytes = 12;
constexpr size_t kFieldRecordTypeOffset = 4;
constexpr uint32_t kDescriptorIsGeneric = 0x80;
constexpr const char* kLazyStateSymbol = "_$s7SwiftUI9LazyStateVMn";

struct StateField {
    char* location;
    size_t index;
};

// Finds a view's @State storage from its reflection metadata, so state can outlive each re-evaluation of the view.
std::vector<StateField> state_fields(char* view, void* type) {
    std::vector<StateField> found;
    const auto* descriptor = static_cast<const char*>(static_cast<void**>(type)[kMetadataDescriptorWord]);
    if (descriptor == nullptr) return found;
    uint32_t flags;
    memcpy(&flags, descriptor, sizeof flags);
    constexpr uint32_t kKindMask = 0x1f;
    constexpr uint32_t kKindStruct = 17;
    if ((flags & kKindMask) != kKindStruct) return found;
    const auto* fields = static_cast<const char*>(relative_pointer(descriptor + kDescriptorFieldsOffset));
    if (fields == nullptr) return found;
    uint32_t count;
    memcpy(&count, fields + 12, sizeof count);
    uint32_t vector_word;
    memcpy(&vector_word, descriptor + kStructFieldOffsetVectorOffset, sizeof vector_word);
    const auto* offsets = reinterpret_cast<const uint32_t*>(static_cast<void**>(type) + vector_word);
    void* const* generics = (flags & kDescriptorIsGeneric) ? static_cast<void* const*>(type) + kMetadataArgumentsWord : nullptr;
    for (uint32_t i = 0; i < count; ++i) {
        const char* record = fields + kFieldDescriptorHeaderBytes + i * kFieldRecordBytes;
        int32_t name_offset;
        memcpy(&name_offset, record + kFieldRecordTypeOffset, sizeof name_offset);
        if (name_offset == 0) continue;
        const char* name = record + kFieldRecordTypeOffset + name_offset;
        // Only a symbolic reference to LazyState can name one; plain field types need no metadata lookup.
        if (static_cast<uint8_t>(name[0]) != 1 && static_cast<uint8_t>(name[0]) != 2) continue;
        int32_t target_offset;
        memcpy(&target_offset, name + 1, sizeof target_offset);
        const void* target = name + 1 + target_offset;
        if (static_cast<uint8_t>(name[0]) == 2) memcpy(&target, target, sizeof target);
        const char* symbol = unresolved_symbol_at(reinterpret_cast<uintptr_t>(target));
        if (symbol == nullptr || strcmp(symbol, kLazyStateSymbol) != 0) continue;
        void* metadata = resolve_mangled_type(name, generics);
        found.push_back({view + offsets[i] + lazy_state_location_offset(metadata), i});
    }
    return found;
}

struct AppearanceAction {
    const void* function;
    void* context;
};

std::vector<AppearanceAction> g_pending_appear;

void* g_selection_value = nullptr;
void* g_selection_type = nullptr;

bool values_equal(void* left, void* right, void* type) {
    if (type == string_metadata()) {
        return swift_string_to_utf8(*static_cast<SwiftString*>(left)) == swift_string_to_utf8(*static_cast<SwiftString*>(right));
    }
    return memcmp(left, right, witnesses_of(type)->size) == 0;
}

std::string text_color_name(uint64_t style) {
    switch ((style & kTextColorMask) >> kTextColorShift) {
        case kColorRed: return "red";
        case kColorGreen: return "green";
        case kColorAccent: return "accent";
        case kColorSecondary:
        case kStyleSecondary:
        case kStyleQuaternary: return "secondary";
        default: return "";
    }
}

const char* font_name(uint64_t style) {
    switch ((style & kTextFontMask) & 0xffff) {
        case kFontCaption: return "caption";
        case kFontCaption2: return "caption2";
        case kFontFootnote: return "footnote";
        default: return "body";
    }
}

std::string render(void* value, void* type, const std::string& path, int depth);

std::string render_children(ViewNode* node, const std::string& path, int depth) {
    std::string out;
    size_t index = 0;
    for (const auto& child : *node->children) {
        const std::string rendered = render(child.value, child.type, path + "/" + std::to_string(index++), depth + 1);
        if (rendered.empty()) continue;
        if (!out.empty()) out += ',';
        out += rendered;
    }
    return out;
}

std::string node_json(const std::string& kind, const std::string& properties, const std::string& children) {
    std::string out = "{\"k\":\"" + kind + "\"";
    if (!properties.empty()) out += "," + properties;
    if (!children.empty()) out += ",\"c\":[" + children + "]";
    return out + "}";
}

std::string text_properties(const std::string& title, const std::string& icon) {
    std::string out;
    if (!title.empty()) out += "\"t\":" + json_string(title);
    if (!icon.empty()) {
        if (!out.empty()) out += ',';
        out += "\"i\":" + json_string(icon);
    }
    return out;
}

std::string render_node(ViewNode* node, const std::string& path, int depth) {
    switch (node->kind) {
        case NodeKind::Task:
            if (started_tasks().insert(path).second) concurrency_start(node->action, node->action_context);
            return {};
        case NodeKind::Button: {
            const int action = add_action({ActionKind::Closure, node->action, node->action_context, nullptr, nullptr, nullptr, 0, {}});
            return node_json("button", "\"a\":" + std::to_string(action), render_children(node, path, depth));
        }
        case NodeKind::Link: {
            const int action = add_action({ActionKind::OpenUrl, nullptr, nullptr, nullptr, nullptr, nullptr, 0, *node->detail});
            return node_json("button", "\"a\":" + std::to_string(action), render_children(node, path, depth));
        }
        case NodeKind::Toggle: {
            bool current = false;
            binding_read(node->binding, &current, bool_metadata());
            const int action = add_action({ActionKind::Toggle, nullptr, nullptr, node->binding, nullptr, nullptr, 0, {}});
            std::string properties = "\"a\":" + std::to_string(action) + ",\"v\":" + (current ? "true" : "false");
            if (!node->title->empty()) properties += ",\"t\":" + json_string(*node->title);
            return node_json("toggle", properties, render_children(node, path, depth));
        }
        case NodeKind::Stepper: {
            int64_t current = 0;
            binding_read(node->binding, &current, int_metadata());
            std::string properties = "\"v\":" + std::to_string(current);
            if (current - node->step >= node->lower) {
                properties += ",\"d\":" + std::to_string(add_action({ActionKind::StepBy, nullptr, nullptr, node->binding, nullptr, nullptr, -node->step, {}}));
            }
            if (current + node->step <= node->upper) {
                properties += ",\"a\":" + std::to_string(add_action({ActionKind::StepBy, nullptr, nullptr, node->binding, nullptr, nullptr, node->step, {}}));
            }
            return node_json("stepper", properties, render_children(node, path, depth));
        }
        case NodeKind::Picker: {
            void* outer_value = g_selection_value;
            void* outer_type = g_selection_type;
            void* selection_type = node->item_type;
            void* current = calloc(1, witnesses_of(selection_type)->size + 1);
            binding_read(node->binding, current, selection_type);
            BindingNode* outer_binding = g_picker_binding;
            g_selection_value = current;
            g_selection_type = selection_type;
            g_picker_binding = node->binding;
            const std::string children = render_children(node, path, depth);
            g_selection_value = outer_value;
            g_selection_type = outer_type;
            g_picker_binding = outer_binding;
            return node_json("picker", {}, children);
        }
        case NodeKind::Sheet: {
            std::string children = render_children(node, path, depth);
            void* optional_type = optional_metadata(node->item_type);
            void* item = calloc(1, witnesses_of(optional_type)->size + 1);
            binding_read(node->binding, item, optional_type);
            TypeField payload;
            if (optional_payload(optional_type, item, payload) && payload.type != nullptr) {
                const size_t size = witnesses_of(node->content_type)->size;
                void* content = calloc(1, size == 0 ? 1 : size);
                rt_call_witness(node->action, content, node->action_context, item, nullptr);
                g_presented_sheet = node->binding;
                g_presented_sheet_type = optional_type;
                const int action = add_action({ActionKind::DismissSheet, nullptr, nullptr, nullptr, nullptr, nullptr, 0, {}});
                const std::string sheet = render(content, node->content_type, path + "/sheet", depth + 1);
                const std::string json = node_json("sheet", "\"d\":" + std::to_string(action), sheet);
                children = children.empty() ? json : children + "," + json;
            }
            return children;
        }
        case NodeKind::TabView: return node_json("tabs", {}, render_children(node, path, depth));
        case NodeKind::Tab: return node_json("tab", text_properties(*node->title, *node->system_image), render_children(node, path, depth));
        case NodeKind::NavigationStack: return node_json("nav", {}, render_children(node, path, depth));
        case NodeKind::List: return node_json("list", {}, render_children(node, path, depth));
        case NodeKind::Section: return node_json("section", {}, render_children(node, path, depth));
        case NodeKind::Label: return node_json("h", {}, render_children(node, path, depth));
        case NodeKind::Menu: return node_json("menu", {}, render_children(node, path, depth));
        case NodeKind::Disclosure: return node_json("disclosure", {}, render_children(node, path, depth));
        case NodeKind::Toolbar: return node_json("toolbar", {}, render_children(node, path, depth));
        case NodeKind::ToolbarItem: return node_json("toolbaritem", {}, render_children(node, path, depth));
        case NodeKind::ProgressView: return node_json("progress", {}, {});
        case NodeKind::AsyncImage: return node_json("image", {}, {});
        case NodeKind::Image: return node_json("image", text_properties(*node->title, *node->system_image), {});
        case NodeKind::LabeledContent: return node_json("labeled", text_properties(*node->title, *node->system_image), render_children(node, path, depth));
        case NodeKind::Unavailable: {
            std::string properties = text_properties(*node->title, *node->system_image);
            if (!node->detail->empty()) properties += ",\"s\":" + json_string(*node->detail);
            return node_json("unavailable", properties, {});
        }
        case NodeKind::Modifier: {
            const std::string children = render_children(node, path, depth);
            if (node->title->empty()) return children;
            return node_json("titled", "\"t\":" + json_string(*node->title), children);
        }
        case NodeKind::ForEach:
        case NodeKind::OnChange:
        case NodeKind::ImageResource:
            return render_children(node, path, depth);
    }
    return {};
}

std::string render(void* value, void* type, const std::string& path, int depth) {
    if (depth > kMaximumDepth) {
        engine_log("STOP: view tree deeper than %d levels", kMaximumDepth);
        abort();
    }
    TypeField branch;
    if (conditional_active(type, value, branch) || optional_payload(type, value, branch)) {
        return branch.type != nullptr ? render(value, branch.type, path, depth) : std::string();
    }
    const uint64_t kind_word = *static_cast<uint64_t*>(type);
    const char* name = foreign_type_name(type);
    if (name != nullptr || kind_word == kTupleKind) {
        if (name != nullptr && strcmp(name, kTextSymbol) == 0) {
            const auto* text = static_cast<const TextValue*>(value);
            std::string properties = "\"t\":" + json_string(swift_string_to_utf8({text->text_bits, text->text_object}));
            const std::string color = text_color_name(text->style);
            if (!color.empty()) properties += ",\"color\":\"" + color + "\"";
            properties += ",\"font\":\"" + std::string(font_name(text->style)) + "\"";
            if (text->style & kFontBold) properties += ",\"bold\":true";
            return node_json("text", properties, {});
        }
        const auto* witnesses = witnesses_of(type);
        if (witnesses->size == sizeof(void*) && (witnesses->flags & kFlagNonPod) != 0) {
            auto* node = *static_cast<ViewNode**>(value);
            if (live_nodes().count(node) != 0) return render_node(node, path, depth);
        }
        const auto fields = type_fields(type);
        if (fields.empty()) {
            if (name != nullptr && strcmp(name, "_$s7SwiftUI6SpacerVMn") == 0) return node_json("spacer", {}, {});
            return {};
        }
        if (name != nullptr && strcmp(name, kModifiedContentSymbol) == 0) {
            std::string content = render(static_cast<char*>(value) + fields[0].offset, fields[0].type, path, depth);
            const char* modifier = foreign_type_name(fields[1].type);
            auto* modifier_value = static_cast<char*>(value) + fields[1].offset;
            if (modifier != nullptr && strcmp(modifier, kTagModifierSymbol) == 0 && g_selection_value != nullptr) {
                void* tag_type = type_fields(fields[1].type)[0].type;
                void* tag = calloc(1, witnesses_of(tag_type)->size + 1);
                witnesses_of(tag_type)->initialize_with_copy(tag, modifier_value, tag_type);
                const bool selected = tag_type == g_selection_type && values_equal(tag, g_selection_value, tag_type);
                const int action = add_action({ActionKind::SetValue, nullptr, nullptr, g_picker_binding, tag, tag_type, 0, {}});
                return node_json("option", "\"a\":" + std::to_string(action) + ",\"v\":" + (selected ? "true" : "false"), content);
            }
            if (modifier != nullptr && strcmp(modifier, kAppearanceSymbol) == 0) {
                const auto* action = reinterpret_cast<const AppearanceAction*>(modifier_value);
                if (action->function != nullptr && started_tasks().insert(path + "#appear").second) g_pending_appear.push_back(*action);
            }
            if (is_runtime_node_type(fields[1].type)) {
                const std::string extra = render(modifier_value, fields[1].type, path + "/m", depth);
                if (!extra.empty()) content = content.empty() ? extra : content + "," + extra;
            }
            return content;
        }
        std::string children;
        size_t index = 0;
        for (const auto& field : fields) {
            const size_t current = index++;
            if (!is_view_like(field.type)) continue;
            const std::string rendered = render(static_cast<char*>(value) + field.offset, field.type, path + "." + std::to_string(current), depth + 1);
            if (rendered.empty()) continue;
            if (!children.empty()) children += ',';
            children += rendered;
        }
        if (name == nullptr) return children;
        if (strcmp(name, "_$s7SwiftUI6HStackVMn") == 0) return node_json("h", {}, children);
        if (strcmp(name, "_$s7SwiftUI6VStackVMn") == 0) return node_json("v", {}, children);
        return children;
    }
    if (const char* representable = find_conformance(type, kRepresentableSymbol)) {
        const void* make = swift_runtime_find_witness_named(representable, kMakeUIViewRequirement);
        if (make == nullptr) return node_json("native", {}, {});
        // The context is opaque to the view; this app's implementation only returns a stored view.
        void* context = calloc(1, kRepresentableContextBytes);
        auto** table = static_cast<void**>(swift_runtime_witness_table(representable, type, static_cast<void**>(type) + kMetadataArgumentsWord));
        // UIView is a class, so the view comes back directly, owned by the caller.
        void* view = rt_call_make_ui_view(make, context, value, type, table);
        free(context);
        const int web_view = foundation_web_view_id(view);
        rt_swift_release(view);
        return node_json("native", "\"w\":" + std::to_string(web_view), {});
    }
    const char* conformance = find_view_conformance(type);
    if (conformance == nullptr) return {};
    const void* body = find_witness(conformance, &g_view_body_requirement);
    const void* body_type_name = find_witness(conformance, &g_view_body_type_requirement);
    if (body == nullptr || body_type_name == nullptr) {
        engine_log("STOP: a view conformance has no body witness");
        abort();
    }

    // Structural identity: the same position in the tree keeps the same @State across re-evaluation.
    auto fields = state_fields(static_cast<char*>(value), type);
    for (const auto& field : fields) {
        const auto saved = preserved_states().find(path + "#" + std::to_string(field.index));
        if (saved != preserved_states().end()) *reinterpret_cast<StateBox**>(field.location) = static_cast<StateBox*>(rt_swift_retain(saved->second));
    }
    // The low bit of an associated type witness marks it as a mangled name rather than an accessor.
    const auto* body_name = reinterpret_cast<const char*>(reinterpret_cast<uintptr_t>(body_type_name) & ~static_cast<uintptr_t>(1));
    void* const* generics = static_cast<void* const*>(type) + kMetadataArgumentsWord;
    void* body_metadata = resolve_mangled_type(body_name, generics);
    const size_t size = witnesses_of(body_metadata)->size;
    void* output = calloc(1, size == 0 ? 1 : size);
    auto** table = static_cast<void**>(rt_swift_getWitnessTable_for_view(conformance, type));
    rt_call_witness(body, output, value, type, table);
    for (const auto& field : fields) {
        auto* state = *reinterpret_cast<StateBox**>(field.location);
        if (state != nullptr) preserved_states()[path + "#" + std::to_string(field.index)] = static_cast<StateBox*>(rt_swift_retain(state));
    }
    return render(output, body_metadata, path + "/b", depth + 1);
}

void dismiss_presented_sheet() {
    if (g_presented_sheet == nullptr) return;
    const auto* witnesses = witnesses_of(g_presented_sheet_type);
    void* nothing = calloc(1, witnesses->size + 1);
    witnesses->store_enum_tag_single_payload(nothing, 1, 1, g_presented_sheet_type);
    binding_write(g_presented_sheet, nothing, g_presented_sheet_type);
    g_presented_sheet = nullptr;
    // A sheet's next presentation starts over, with fresh @State and onAppear.
    constexpr const char* kSheetPath = "/sheet";
    for (auto it = preserved_states().begin(); it != preserved_states().end();) {
        it = it->first.find(kSheetPath) != std::string::npos ? preserved_states().erase(it) : std::next(it);
    }
    for (auto it = started_tasks().begin(); it != started_tasks().end();) {
        it = it->find(kSheetPath) != std::string::npos ? started_tasks().erase(it) : std::next(it);
    }
}

void perform_action(const Action& action) {
    switch (action.kind) {
        case ActionKind::Closure:
            rt_call_witness(action.function, nullptr, action.context, nullptr, nullptr);
            break;
        case ActionKind::Toggle: {
            bool current = false;
            binding_read(action.binding, &current, bool_metadata());
            current = !current;
            binding_write(action.binding, &current, bool_metadata());
            break;
        }
        case ActionKind::StepBy: {
            int64_t current = 0;
            binding_read(action.binding, &current, int_metadata());
            current += action.amount;
            binding_write(action.binding, &current, int_metadata());
            break;
        }
        case ActionKind::SetValue:
            binding_write(action.binding, action.value, action.type);
            break;
        case ActionKind::DismissSheet:
            dismiss_presented_sheet();
            break;
        case ActionKind::OpenUrl:
            engine_emit("open", action.text);
            break;
    }
}

void* g_root_content = nullptr;
void* g_root_type = nullptr;

void render_tree() {
    g_actions.clear();
    g_presented_sheet = nullptr;
    g_tree_dirty.store(false, std::memory_order_relaxed);
    const std::string tree = render(g_root_content, g_root_type, "r", 0);
    engine_emit("tree", "[" + tree + "]");
    std::vector<AppearanceAction> appeared;
    appeared.swap(g_pending_appear);
    for (const auto& action : appeared) {
        rt_call_witness(action.function, nullptr, action.context, nullptr, nullptr);
        g_tree_dirty.store(true, std::memory_order_relaxed);
    }
}

}  // namespace

// makeUIView(context:): context in x0, self in x20, then Self and its witness table.
__asm__(
    ".text\n"
    ".globl rt_call_make_ui_view\n"
    "rt_call_make_ui_view:\n"
    "  stp x29, x30, [sp, #-32]!\n"
    "  mov x29, sp\n"
    "  str x20, [sp, #16]\n"
    "  mov x16, x0\n"
    "  mov x0, x1\n"
    "  mov x20, x2\n"
    "  mov x1, x3\n"
    "  mov x2, x4\n"
    "  blr x16\n"
    "  ldr x20, [sp, #16]\n"
    "  ldp x29, x30, [sp], #32\n"
    "  ret\n");

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

void swiftui_attach_image(const MachImage& image) {
    g_view_conformances = image.section("__TEXT", "__swift5_proto");
    if (g_view_conformances != nullptr) swift_runtime_attach_conformances(reinterpret_cast<const void*>(g_view_conformances->address), g_view_conformances->size);
}

void swiftui_run_scene(void* scene) {
    auto* box = *static_cast<ViewBox**>(scene);
    const size_t size = witnesses_of(box->content_type)->size;
    g_root_content = calloc(1, size == 0 ? 1 : size);
    g_root_type = box->content_type;
    engine_log("SCENE: running WindowGroup content");
    rt_call_witness(box->closure, g_root_content, box->closure_context, nullptr, nullptr);

    // The guest thread lives here: each action runs the app's own code, then the tree is re-evaluated if needed.
    g_tree_dirty.store(true, std::memory_order_relaxed);
    for (;;) {
        while (g_tree_dirty.load(std::memory_order_relaxed)) {
            render_tree();
            concurrency_drain();
        }
        const int action = engine_wait_for_action(concurrency_next_deadline());
        if (action == kEngineEventRan) {
            // Host events and timers only redraw if they changed something the app observes.
            concurrency_fire_timers();
            concurrency_drain();
            continue;
        }
        if (action < 0) return;
        if (static_cast<size_t>(action) < g_actions.size()) {
            const Action current = g_actions[static_cast<size_t>(action)];
            perform_action(current);
            concurrency_drain();
        }
        g_tree_dirty.store(true, std::memory_order_relaxed);
    }
}

void* swiftui_symbol(std::string_view name) {
    static const bool registered = [] {
        register_foreign_witness("_$s7SwiftUI3TabVyxq_q0_GAA0C7ContentAASHRzAA4ViewR_AaFR0_rlMc",
                                 "_$s7SwiftUI10TabContentP15_identifiedView011_IdentifiedF0QzvgTq", reinterpret_cast<const void*>(tab_identified_view));
        return true;
    }();
    (void)registered;
    static const std::unordered_map<std::string_view, void*> table = {
        {"$s7SwiftUI11WindowGroupV2id5title11lazyContentACyxGSSSg_AA4TextVSgxyctcfC", reinterpret_cast<void*>(window_group_init)},
        {"$s7SwiftUI12SceneBuilderV10buildBlockyxxAA0C0RzlFZ", reinterpret_cast<void*>(scene_build_block)},
        {"$s7SwiftUI9LazyStateV12wrappedValueACyxGxyXA_tcfC", reinterpret_cast<void*>(lazy_state_init)},
        {"$s7SwiftUI9LazyStateV7StorageO5valueyAEyx_GxcAGmlFWC", const_cast<int32_t*>(&kStorageCaseValueTag)},
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
        {"$s7SwiftUI7BindingV12wrappedValuexvg", reinterpret_cast<void*>(binding_wrapped_get)},
        {"$s7SwiftUI7BindingV12wrappedValuexvs", reinterpret_cast<void*>(binding_wrapped_set)},
        {"$s7SwiftUI7BindingV13dynamicMemberACyqd__Gs15WritableKeyPathCyxqd__G_tcluig", reinterpret_cast<void*>(binding_member)},
        {"$s7SwiftUI7BindingV3get3setACyxGxyc_yxctcfC", reinterpret_cast<void*>(binding_get_set)},
        {"$s7SwiftUI9LazyStateV14projectedValueAA7BindingVyxGvg", reinterpret_cast<void*>(lazy_state_projected)},
        {"$s7SwiftUI8BindableVAARlzC11Observation10ObservableRzlE12wrappedValueACyxGx_tcfC", reinterpret_cast<void*>(bindable_init)},
        {"$s7SwiftUI8BindableV12wrappedValuexvg", reinterpret_cast<void*>(bindable_wrapped_get)},
        {"$s7SwiftUI8BindableV14projectedValueACyxGvg", reinterpret_cast<void*>(bindable_projected)},
        {"$s7SwiftUI8BindableVAARlzClE13dynamicMemberAA7BindingVyqd__Gs24ReferenceWritableKeyPathCyxqd__G_tcluig", reinterpret_cast<void*>(bindable_member)},
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
        {"$s7SwiftUI17EnvironmentValuesVACycfC", reinterpret_cast<void*>(environment_values_init)},
        {"$s7SwiftUI17EnvironmentValuesV9isEnabledSbvg", reinterpret_cast<void*>(environment_is_enabled)},
        {"$s7SwiftUI17EnvironmentValuesV9isEnabledSbvs", reinterpret_cast<void*>(environment_set_ignored)},
        {"$s7SwiftUI17EnvironmentValuesV4tintAA13AnyShapeStyleVSgvg", reinterpret_cast<void*>(environment_tint)},
        {"$s7SwiftUI17EnvironmentValuesV4tintAA13AnyShapeStyleVSgvs", reinterpret_cast<void*>(environment_set_ignored)},
        {"$s7SwiftUI17EnvironmentValuesV7dismissAA13DismissActionVvg", reinterpret_cast<void*>(environment_action_get)},
        {"$s7SwiftUI17EnvironmentValuesV7openURLAA13OpenURLActionVvg", reinterpret_cast<void*>(environment_action_get)},
        {"$s7SwiftUI17EnvironmentValuesV7openURLAA13OpenURLActionVvs", reinterpret_cast<void*>(environment_set_ignored)},
        {"$s7SwiftUI13DismissActionV14callAsFunctionyyF", reinterpret_cast<void*>(dismiss_action_call)},
        {"$s7SwiftUI13OpenURLActionV14callAsFunctionyy10Foundation3URLVF", reinterpret_cast<void*>(open_url_action_call)},
        {"$s7SwiftUI14_TaskModifier2V4name18executorPreference8priority6actionACSS_Sch_pSgScPyyYaYAcntcfC", reinterpret_cast<void*>(task_modifier_init)},
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
        {"$s7SwiftUI5ImageV12ResizingModeO7stretchyA2EmFWC", const_cast<int32_t*>(&kImageResizingStretch)},
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
        {"$s7SwiftUI17NavigationBarItemV16TitleDisplayModeO6inlineyA2EmFWC", const_cast<int32_t*>(&kTitleDisplayInline)},
        {"$s7SwiftUI3LogO013runtimeIssuesC0So9OS_os_logCvgZ", reinterpret_cast<void*>(runtime_issues_log)},
        {"$s7SwiftUI4ViewP4body4BodyQzvgTq", &g_view_body_requirement},
        {"$s4Body7SwiftUI4ViewPTl", &g_view_body_type_requirement},
    };
    const auto found = table.find(name);
    return found == table.end() ? nullptr : found->second;
}
