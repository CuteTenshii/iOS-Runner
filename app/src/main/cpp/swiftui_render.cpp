#include "swiftui_internal.h"

using namespace swiftui;

namespace swiftui {

// Only fields with their own metadata can be views: plain bytes in a modifier or stack have none worth walking.
bool is_view_like(void* type) {
    const uint64_t kind = *static_cast<uint64_t*>(type);
    if (kind == kTupleKind) return true;
    if (foreign_type_name(type) != nullptr) return true;
    return find_view_conformance(type) != nullptr || find_conformance(type, kToolbarContentSymbol) != nullptr || find_conformance(type, kRepresentableSymbol) != nullptr;
}

enum class ActionKind { Closure,
                        Toggle,
                        StepBy,
                        SetValue,
                        DismissSheet,
                        OpenUrl,
                        SetText,
                        PushLink,
                        PopLink };

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

constexpr size_t kStructFieldOffsetVectorOffset = 24;

constexpr size_t kFieldDescriptorHeaderBytes = 16;

constexpr size_t kFieldRecordTypeOffset = 4;

constexpr uint32_t kDescriptorIsGeneric = 0x80;

constexpr const char* kLazyStateSymbol = "_$s7SwiftUI9LazyStateVMn";

constexpr const char* kStateSymbol = "_$s7SwiftUI5StateVMn";

struct StateField {
    char* location;
    size_t index;
    char* storage;
    void* lazy_metadata;
    void* plain_type;
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
        if (symbol == nullptr) continue;
        if (strcmp(symbol, kLazyStateSymbol) == 0) {
            void* metadata = resolve_mangled_type(name, generics);
            found.push_back({view + offsets[i] + lazy_state_location_offset(metadata), i, view + offsets[i], metadata, nullptr});
        } else if (strcmp(symbol, kStateSymbol) == 0) {
            void* type = generic_argument(resolve_mangled_type(name, generics));
            found.push_back({view + offsets[i] + state_location_offset(type), i, view + offsets[i], nullptr, type});
        }
    }
    return found;
}

struct AppearanceAction {
    const void* function;
    void* context;
};

std::vector<AppearanceAction> g_pending_appear;

struct ChangeAction {
    AppearanceAction action;
    void* value;
    void* type;
};

std::vector<ChangeAction> g_pending_changes;

bool values_equal(void* left, void* right, void* type) {
    if (type == string_metadata()) {
        return swift_string_to_utf8(*static_cast<SwiftString*>(left)) == swift_string_to_utf8(*static_cast<SwiftString*>(right));
    }
    return memcmp(left, right, witnesses_of(type)->size) == 0;
}

void* copy_of_value(const void* value, void* type) {
    void* copy = calloc(1, witnesses_of(type)->size + 1);
    witnesses_of(type)->initialize_with_copy(copy, const_cast<void*>(value), type);
    return copy;
}

// `.onChange` compares against the previous render, so the first render never fires it.
void watch_value_change(const std::string& path, char* modifier, void* modifier_type) {
    static std::map<std::string, void*> last_values;
    const auto fields = type_fields(modifier_type);
    void* type = fields[0].type;
    void* current = modifier + fields[0].offset;
    const auto found = last_values.find(path);
    if (found == last_values.end()) {
        last_values[path] = copy_of_value(current, type);
        return;
    }
    if (values_equal(found->second, current, type)) return;
    witnesses_of(type)->destroy(found->second, type);
    free(found->second);
    found->second = copy_of_value(current, type);
    const auto* action = reinterpret_cast<const AppearanceAction*>(modifier + fields[1].offset);
    rt_swift_retain(action->context);
    g_pending_changes.push_back({*action, copy_of_value(current, type), type});
}

void* g_selection_value = nullptr;

void* g_selection_type = nullptr;

std::string text_color_name(uint64_t style) {
    switch ((style & kTextColorMask) >> kTextColorShift) {
        case kColorRed:
            return "red";
        case kColorGreen:
            return "green";
        case kColorAccent:
            return "accent";
        case kColorSecondary:
        case kStyleSecondary:
        case kStyleQuaternary:
            return "secondary";
        default:
            return "";
    }
}

const char* font_name(uint64_t style) {
    switch ((style & kTextFontMask) & 0xffff) {
        case kFontCaption:
            return "caption";
        case kFontCaption2:
            return "caption2";
        case kFontFootnote:
            return "footnote";
        default:
            return "body";
    }
}

using Nodes = std::vector<Json>;

using Properties = std::vector<std::pair<std::string, Json>>;

Json json_number(int64_t number) {
    Json value;
    value.kind = Json::Kind::Integer;
    value.integer = number;
    return value;
}

Json json_flag(bool flag) {
    Json value;
    value.kind = Json::Kind::Bool;
    value.flag = flag;
    return value;
}

Nodes render(void* value, void* type, const std::string& path, int depth);

void append_nodes(Nodes& to, Nodes from) { to.insert(to.end(), std::make_move_iterator(from.begin()), std::make_move_iterator(from.end())); }

Nodes render_children(ViewNode* node, const std::string& path, int depth) {
    Nodes out;
    size_t index = 0;
    for (const auto& child : *node->children) append_nodes(out, render(child.value, child.type, path + "/" + std::to_string(index++), depth + 1));
    return out;
}

Json tree_node(const std::string& kind, Properties properties = {}, Nodes children = {}) {
    Json out;
    out.kind = Json::Kind::Object;
    out.members.emplace_back("k", json_text(kind));
    for (auto& property : properties) out.members.push_back(std::move(property));
    if (!children.empty()) {
        Json list;
        list.kind = Json::Kind::Array;
        list.items = std::move(children);
        out.members.emplace_back("c", std::move(list));
    }
    return out;
}

Properties text_properties(const std::string& title, const std::string& icon) {
    Properties out;
    if (!title.empty()) out.emplace_back("t", json_text(title));
    if (!icon.empty()) out.emplace_back("i", json_text(icon));
    return out;
}

Json plain_text(const std::string& text, const char* font = "body") { return tree_node("text", {{"t", json_text(text)}, {"font", json_text(font)}}); }

std::set<std::string>& pushed_links() {
    static std::set<std::string> links;
    return links;
}

// A pushed destination shows as a sheet over its stack, with dismissal as the back gesture.
Nodes render_navigation_link(ViewNode* node, const std::string& path, int depth) {
    Nodes label = node->children->size() > 1 ? render((*node->children)[1].value, (*node->children)[1].type, path + "/l", depth + 1) : Nodes();
    if (!node->title->empty()) label = {plain_text(*node->title)};
    const int push = add_action({ActionKind::PushLink, nullptr, nullptr, nullptr, nullptr, nullptr, 0, path});
    Nodes out = {tree_node("button", {{"a", json_number(push)}}, std::move(label))};
    if (pushed_links().count(path) != 0) {
        const int pop = add_action({ActionKind::PopLink, nullptr, nullptr, nullptr, nullptr, nullptr, 0, path});
        const auto& destination = (*node->children)[0];
        out.push_back(tree_node("sheet", {{"d", json_number(pop)}}, render(destination.value, destination.type, path + "/dest", depth + 1)));
    }
    return out;
}

// `.tabItem` names its tab with a Label or Text, which is all the tab bar shows of it.
void find_tab_label(const Json& node, std::string& title, std::string& icon) {
    if (const Json* text = node.member("t"); text != nullptr && title.empty()) title = text->text;
    if (const Json* image = node.member("i"); image != nullptr && icon.empty()) icon = image->text;
    if (const Json* children = node.member("c")) {
        for (const Json& child : children->items) find_tab_label(child, title, icon);
    }
}

Nodes render_tab_item(ViewNode* node, const std::string& path, int depth) {
    Nodes content = render((*node->children)[0].value, (*node->children)[0].type, path + "/0", depth + 1);
    const Nodes label = render((*node->children)[1].value, (*node->children)[1].type, path + "/1", depth + 1);
    std::string title;
    std::string icon;
    for (const Json& part : label) find_tab_label(part, title, icon);
    return {tree_node("tab", text_properties(title, icon), std::move(content))};
}

Nodes render_node(ViewNode* node, const std::string& path, int depth) {
    switch (node->kind) {
        case NodeKind::Task:
            if (started_tasks().insert(path).second) concurrency_start(node->action, node->action_context);
            return {};
        case NodeKind::Button: {
            const int action = add_action({ActionKind::Closure, node->action, node->action_context, nullptr, nullptr, nullptr, 0, {}});
            Nodes children = node->title->empty() ? render_children(node, path, depth) : Nodes{plain_text(*node->title)};
            return {tree_node("button", {{"a", json_number(action)}}, std::move(children))};
        }
        case NodeKind::NavigationLink:
            return render_navigation_link(node, path, depth);
        case NodeKind::TextField: {
            SwiftString text{};
            binding_read(node->binding, &text, string_metadata());
            const int action = add_action({ActionKind::SetText, nullptr, nullptr, node->binding, nullptr, nullptr, 0, {}});
            Properties properties = {{"a", json_number(action)}, {"t", json_text(*node->title)}, {"v", json_text(swift_string_to_utf8(text))}};
            if (node->lower != 0) properties.emplace_back("secure", json_flag(true));
            return {tree_node("field", std::move(properties))};
        }
        case NodeKind::Searchable: {
            SwiftString text{};
            binding_read(node->binding, &text, string_metadata());
            const int action = add_action({ActionKind::SetText, nullptr, nullptr, node->binding, nullptr, nullptr, 0, {}});
            Nodes children = {tree_node("field", {{"a", json_number(action)}, {"t", json_text("Search")}, {"v", json_text(swift_string_to_utf8(text))}})};
            append_nodes(children, render_children(node, path, depth));
            return {tree_node("v", {}, std::move(children))};
        }
        case NodeKind::Stack:
            return {tree_node("v", {}, render_children(node, path, depth))};
        case NodeKind::Divider:
            return {};
        case NodeKind::Link: {
            const int action = add_action({ActionKind::OpenUrl, nullptr, nullptr, nullptr, nullptr, nullptr, 0, *node->detail});
            return {tree_node("button", {{"a", json_number(action)}}, render_children(node, path, depth))};
        }
        case NodeKind::Toggle: {
            bool current = false;
            binding_read(node->binding, &current, bool_metadata());
            const int action = add_action({ActionKind::Toggle, nullptr, nullptr, node->binding, nullptr, nullptr, 0, {}});
            Properties properties = {{"a", json_number(action)}, {"v", json_flag(current)}};
            if (!node->title->empty()) properties.emplace_back("t", json_text(*node->title));
            return {tree_node("toggle", std::move(properties), render_children(node, path, depth))};
        }
        case NodeKind::Stepper: {
            int64_t current = 0;
            binding_read(node->binding, &current, int_metadata());
            Properties properties = {{"v", json_number(current)}};
            if (current - node->step >= node->lower) {
                properties.emplace_back("d", json_number(add_action({ActionKind::StepBy, nullptr, nullptr, node->binding, nullptr, nullptr, -node->step, {}})));
            }
            if (current + node->step <= node->upper) {
                properties.emplace_back("a", json_number(add_action({ActionKind::StepBy, nullptr, nullptr, node->binding, nullptr, nullptr, node->step, {}})));
            }
            return {tree_node("stepper", std::move(properties), render_children(node, path, depth))};
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
            Nodes children = render_children(node, path, depth);
            g_selection_value = outer_value;
            g_selection_type = outer_type;
            g_picker_binding = outer_binding;
            return {tree_node("picker", text_properties(*node->title, {}), std::move(children))};
        }
        case NodeKind::Sheet: {
            Nodes children = render_children(node, path, depth);
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
                children.push_back(tree_node("sheet", {{"d", json_number(action)}}, render(content, node->content_type, path + "/sheet", depth + 1)));
            }
            return children;
        }
        case NodeKind::RowMenu: {
            Nodes row = render((*node->children)[0].value, (*node->children)[0].type, path + "/0", depth + 1);
            Nodes menu = render((*node->children)[1].value, (*node->children)[1].type, path + "/1", depth + 1);
            if (!menu.empty()) {
                Nodes items = {tree_node("image", {{"i", json_text("ellipsis")}})};
                append_nodes(items, std::move(menu));
                row.push_back(tree_node("menu", {}, std::move(items)));
            }
            return {tree_node("h", {}, std::move(row))};
        }
        case NodeKind::PresentedSheet: {
            Nodes children = render_children(node, path, depth);
            bool presented = false;
            binding_read(node->binding, &presented, bool_metadata());
            if (!presented) return children;
            const size_t size = witnesses_of(node->content_type)->size;
            void* content = calloc(1, size == 0 ? 1 : size);
            rt_call_witness(node->action, content, node->action_context, nullptr, nullptr);
            const int action = add_action({ActionKind::SetValue, nullptr, nullptr, node->binding, new bool(false), bool_metadata(), 0, {}});
            children.push_back(tree_node("sheet", {{"d", json_number(action)}}, render(content, node->content_type, path + "/sheet", depth + 1)));
            return children;
        }
        case NodeKind::Alert: {
            Nodes children = render((*node->children)[0].value, (*node->children)[0].type, path + "/0", depth + 1);
            bool presented = false;
            binding_read(node->binding, &presented, bool_metadata());
            if (!presented) return children;
            Nodes body;
            for (size_t i = node->children->size(); i-- > 1;) {
                append_nodes(body, render((*node->children)[i].value, (*node->children)[i].type, path + "/alert" + std::to_string(i), depth + 1));
            }
            const int action = add_action({ActionKind::SetValue, nullptr, nullptr, node->binding, new bool(false), bool_metadata(), 0, {}});
            children.push_back(tree_node("alert", {{"d", json_number(action)}, {"t", json_text(*node->title)}}, std::move(body)));
            return children;
        }
        case NodeKind::TabView:
            return {tree_node("tabs", {}, render_children(node, path, depth))};
        case NodeKind::Tab: {
            if (node->lower != 0) return render_tab_item(node, path, depth);
            return {tree_node("tab", text_properties(*node->title, *node->system_image), render_children(node, path, depth))};
        }
        case NodeKind::NavigationStack:
            return {tree_node("nav", {}, render_children(node, path, depth))};
        case NodeKind::List:
            return {tree_node("list", {}, render_children(node, path, depth))};
        case NodeKind::Section: {
            Nodes children;
            if (!node->title->empty()) children.push_back(tree_node("text", {{"t", json_text(*node->title)}, {"font", json_text("caption")}, {"color", json_text("secondary")}}));
            append_nodes(children, render_children(node, path, depth));
            return {tree_node("section", {}, std::move(children))};
        }
        case NodeKind::Label: {
            if (node->children->empty()) return {tree_node("h", {}, {tree_node("image", text_properties({}, *node->system_image)), plain_text(*node->title)})};
            return {tree_node("h", {}, render_children(node, path, depth))};
        }
        case NodeKind::Menu:
            return {tree_node("menu", {}, render_children(node, path, depth))};
        case NodeKind::Disclosure: {
            Nodes children;
            if (!node->title->empty()) children.push_back(plain_text(*node->title));
            append_nodes(children, render_children(node, path, depth));
            return {tree_node("disclosure", {}, std::move(children))};
        }
        case NodeKind::Toolbar:
            return {tree_node("toolbar", {}, render_children(node, path, depth))};
        case NodeKind::ToolbarItem:
            return {tree_node("toolbaritem", {}, render_children(node, path, depth))};
        case NodeKind::ProgressView:
            return {tree_node("progress")};
        case NodeKind::AsyncImage:
            return {tree_node("image")};
        case NodeKind::Image:
            return {tree_node("image", text_properties(*node->title, *node->system_image))};
        case NodeKind::LabeledContent:
            return {tree_node("labeled", text_properties(*node->title, *node->system_image), render_children(node, path, depth))};
        case NodeKind::Unavailable: {
            Properties properties = text_properties(*node->title, *node->system_image);
            if (!node->detail->empty()) properties.emplace_back("s", json_text(*node->detail));
            return {tree_node("unavailable", std::move(properties))};
        }
        case NodeKind::Modifier: {
            Nodes children = render_children(node, path, depth);
            if (node->title->empty()) return children;
            return {tree_node("titled", {{"t", json_text(*node->title)}}, std::move(children))};
        }
        case NodeKind::ForEach:
        case NodeKind::OnChange:
        case NodeKind::ImageResource:
            return render_children(node, path, depth);
    }
    return {};
}

// What a custom modifier's `content` placeholder stands for while its body renders.
Nodes g_modifier_content;

Nodes render_custom_modifier(const char* conformance, void* modifier, void* type, Nodes content, const std::string& path, int depth) {
    const void* body = find_witness(conformance, &g_modifier_body_requirement);
    const void* body_type_name = find_witness(conformance, &g_modifier_body_type_requirement);
    if (body == nullptr || body_type_name == nullptr) return content;
    const auto* body_name = reinterpret_cast<const char*>(reinterpret_cast<uintptr_t>(body_type_name) & ~static_cast<uintptr_t>(1));
    void* body_metadata = resolve_mangled_type(body_name, static_cast<void* const*>(type) + kMetadataArgumentsWord);
    const size_t size = witnesses_of(body_metadata)->size;
    void* output = calloc(1, size == 0 ? 1 : size);
    auto** table = static_cast<void**>(rt_swift_getWitnessTable_for_view(conformance, type));
    char placeholder = 0;
    rt_call_modifier_body(body, output, &placeholder, modifier, type, table);
    Nodes outer = std::move(g_modifier_content);
    g_modifier_content = std::move(content);
    Nodes rendered = render(output, body_metadata, path + "/mod", depth + 1);
    g_modifier_content = std::move(outer);
    return rendered;
}

Nodes render(void* value, void* type, const std::string& path, int depth) {
    if (depth > kMaximumDepth) {
        engine_log("STOP: view tree deeper than %d levels", kMaximumDepth);
        abort();
    }
    TypeField branch;
    if (conditional_active(type, value, branch) || optional_payload(type, value, branch)) {
        return branch.type != nullptr ? render(value, branch.type, path, depth) : Nodes();
    }
    const uint64_t kind_word = *static_cast<uint64_t*>(type);
    const char* name = foreign_type_name(type);
    if (name != nullptr || kind_word == kTupleKind) {
        if (name != nullptr && strcmp(name, kTextSymbol) == 0) {
            const auto* text = static_cast<const TextValue*>(value);
            Properties properties = {{"t", json_text(swift_string_to_utf8({text->text_bits, text->text_object}))}};
            const std::string color = text_color_name(text->style);
            if (!color.empty()) properties.emplace_back("color", json_text(color));
            properties.emplace_back("font", json_text(font_name(text->style)));
            if (text->style & kFontBold) properties.emplace_back("bold", json_flag(true));
            return {tree_node("text", std::move(properties))};
        }
        const auto* witnesses = witnesses_of(type);
        if (witnesses->size == sizeof(void*) && (witnesses->flags & kFlagNonPod) != 0) {
            auto* node = *static_cast<ViewNode**>(value);
            if (live_nodes().count(node) != 0) return render_node(node, path, depth);
        }
        const auto fields = type_fields(type);
        if (fields.empty()) {
            if (name != nullptr && strcmp(name, "_$s7SwiftUI21_ViewModifier_ContentVMn") == 0) return g_modifier_content;
            if (name != nullptr && strcmp(name, "_$s7SwiftUI6SpacerVMn") == 0) return {tree_node("spacer")};
            return {};
        }
        // Only `.onReceive`'s content is a view; its publisher reports through the app's own observers.
        if (name != nullptr && strcmp(name, "_$s7SwiftUI16SubscriptionViewVMn") == 0) return render(static_cast<char*>(value) + fields[0].offset, fields[0].type, path, depth);
        if (name != nullptr && strcmp(name, kModifiedContentSymbol) == 0) {
            Nodes content = render(static_cast<char*>(value) + fields[0].offset, fields[0].type, path, depth);
            const char* modifier = foreign_type_name(fields[1].type);
            auto* modifier_value = static_cast<char*>(value) + fields[1].offset;
            if (modifier != nullptr && strcmp(modifier, kTagModifierSymbol) == 0 && g_selection_value != nullptr) {
                void* tag_type = type_fields(fields[1].type)[0].type;
                void* tag = calloc(1, witnesses_of(tag_type)->size + 1);
                witnesses_of(tag_type)->initialize_with_copy(tag, modifier_value, tag_type);
                const bool selected = tag_type == g_selection_type && values_equal(tag, g_selection_value, tag_type);
                const int action = add_action({ActionKind::SetValue, nullptr, nullptr, g_picker_binding, tag, tag_type, 0, {}});
                return {tree_node("option", {{"a", json_number(action)}, {"v", json_flag(selected)}}, std::move(content))};
            }
            if (modifier != nullptr && strcmp(modifier, "_$s7SwiftUI13_TaskModifierVMn") == 0) {
                const auto* task = reinterpret_cast<const AppearanceAction*>(modifier_value);
                if (started_tasks().insert(path + "#task").second) concurrency_start(task->function, rt_swift_retain(task->context));
            }
            if (modifier != nullptr && strcmp(modifier, kAppearanceSymbol) == 0) {
                const auto* action = reinterpret_cast<const AppearanceAction*>(modifier_value);
                if (action->function != nullptr && started_tasks().insert(path + "#appear").second) g_pending_appear.push_back(*action);
            }
            if (modifier != nullptr && strcmp(modifier, "_$s7SwiftUI20_ValueActionModifierVMn") == 0) watch_value_change(path, modifier_value, fields[1].type);
            if (const char* custom = find_conformance(fields[1].type, kViewModifierSymbol)) {
                return render_custom_modifier(custom, modifier_value, fields[1].type, std::move(content), path, depth);
            }
            if (is_runtime_node_type(fields[1].type)) {
                append_nodes(content, render(modifier_value, fields[1].type, path + "/m", depth));
            }
            return content;
        }
        Nodes children;
        size_t index = 0;
        for (const auto& field : fields) {
            const size_t current = index++;
            // A view builder's tuple holds only views, including Optional ones from `if let`.
            if (kind_word != kTupleKind && !is_view_like(field.type)) continue;
            append_nodes(children, render(static_cast<char*>(value) + field.offset, field.type, path + "." + std::to_string(current), depth + 1));
        }
        if (name == nullptr) return children;
        if (strcmp(name, "_$s7SwiftUI21_ViewModifier_ContentVMn") == 0) return g_modifier_content;
        if (strcmp(name, "_$s7SwiftUI6HStackVMn") == 0) return {tree_node("h", {}, std::move(children))};
        if (strcmp(name, "_$s7SwiftUI6VStackVMn") == 0) {
            // A stack around one view adds nothing, and would stop a tab bar or list from filling the screen.
            if (children.size() == 1) return children;
            return {tree_node("v", {}, std::move(children))};
        }
        return children;
    }
    if (const char* representable = find_conformance(type, kRepresentableSymbol)) {
        const void* make = swift_runtime_find_witness_named(representable, kMakeUIViewRequirement);
        if (make == nullptr) return {tree_node("native")};
        // The context is opaque to the view; this app's implementation only returns a stored view.
        void* context = calloc(1, kRepresentableContextBytes);
        auto** table = static_cast<void**>(swift_runtime_witness_table(representable, type, static_cast<void**>(type) + kMetadataArgumentsWord));
        // UIView is a class, so the view comes back directly, owned by the caller.
        void* view = rt_call_make_ui_view(make, context, value, type, table);
        free(context);
        const int web_view = foundation_web_view_id(view);
        rt_swift_release(view);
        return {tree_node("native", {{"w", json_number(web_view)}})};
    }
    const char* conformance = find_view_conformance(type);
    const bool is_toolbar_content = conformance == nullptr;
    if (is_toolbar_content) conformance = find_conformance(type, kToolbarContentSymbol);
    if (conformance == nullptr) return {};
    const void* body = find_witness(conformance, is_toolbar_content ? &g_toolbar_body_requirement : &g_view_body_requirement);
    const void* body_type_name = find_witness(conformance, is_toolbar_content ? &g_toolbar_body_type_requirement : &g_view_body_type_requirement);
    if (body == nullptr || body_type_name == nullptr) {
        engine_log("STOP: a view conformance has no body witness");
        abort();
    }

    // Structural identity: the same position in the tree keeps the same @State across re-evaluation.
    auto fields = state_fields(static_cast<char*>(value), type);
    for (const auto& field : fields) {
        const auto saved = preserved_states().find(path + "#" + std::to_string(field.index));
        if (saved != preserved_states().end()) {
            *reinterpret_cast<StateBox**>(field.location) = static_cast<StateBox*>(rt_swift_retain(saved->second));
        } else if (field.lazy_metadata != nullptr) {
            // Created now, so every copy of the view the body's closures capture shares this box.
            state_box_for(field.storage, field.lazy_metadata);
        } else {
            plain_state_box(field.storage, field.plain_type);
        }
    }
    // The low bit of an associated type witness marks it as a mangled name rather than an accessor.
    const auto* body_name = reinterpret_cast<const char*>(reinterpret_cast<uintptr_t>(body_type_name) & ~static_cast<uintptr_t>(1));
    void* const* generics = static_cast<void* const*>(type) + kMetadataArgumentsWord;
    void* body_metadata = resolve_mangled_type(body_name, generics);
    const size_t size = witnesses_of(body_metadata)->size;
    void* output = calloc(1, size == 0 ? 1 : size);
    auto** table = static_cast<void**>(rt_swift_getWitnessTable_for_view(conformance, type));
    g_body_path = path;
    rt_call_witness(body, output, value, type, table);
    g_body_path.clear();
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
        case ActionKind::SetText:
            break;
        case ActionKind::PushLink:
            pushed_links().insert(action.text);
            break;
        case ActionKind::PopLink: {
            pushed_links().erase(action.text);
            const std::string destination = action.text + "/dest";
            for (auto it = preserved_states().begin(); it != preserved_states().end();) {
                it = it->first.find(destination) != std::string::npos ? preserved_states().erase(it) : std::next(it);
            }
            for (auto it = started_tasks().begin(); it != started_tasks().end();) {
                it = it->find(destination) != std::string::npos ? started_tasks().erase(it) : std::next(it);
            }
            break;
        }
    }
}

void* g_root_content = nullptr;

void* g_root_type = nullptr;

void render_tree() {
    g_actions.clear();
    g_presented_sheet = nullptr;
    g_tree_dirty.store(false, std::memory_order_relaxed);
    Json tree;
    tree.kind = Json::Kind::Array;
    tree.items = render(g_root_content, g_root_type, "r", 0);
    engine_emit("tree", json_write(tree));
    std::vector<AppearanceAction> appeared;
    appeared.swap(g_pending_appear);
    for (const auto& action : appeared) {
        rt_call_witness(action.function, nullptr, action.context, nullptr, nullptr);
        g_tree_dirty.store(true, std::memory_order_relaxed);
    }
    std::vector<ChangeAction> changes;
    changes.swap(g_pending_changes);
    for (const auto& change : changes) {
        rt_call_witness(change.action.function, nullptr, change.action.context, change.value, nullptr);
        rt_swift_release(change.action.context);
        witnesses_of(change.type)->destroy(change.value, change.type);
        free(change.value);
        g_tree_dirty.store(true, std::memory_order_relaxed);
    }
}

}  // namespace swiftui

// makeUIView(context:): context in x0, self in x20, then Self and its witness table.
__asm__(
    ".text\n"
    ".globl rt_call_make_ui_view\n"
    "rt_call_make_ui_view:\n" SWIFT_CALL_PROLOGUE
    "  mov x0, x1\n"
    "  mov x20, x2\n"
    "  mov x1, x3\n"
    "  mov x2, x4\n" SWIFT_CALL_EPILOGUE);

// ViewModifier.body(content:): the placeholder content in x0, self in x20, then Self and its witness table.
__asm__(
    ".text\n"
    ".globl rt_call_modifier_body\n"
    "rt_call_modifier_body:\n" SWIFT_CALL_PROLOGUE
    "  mov x8, x1\n"
    "  mov x0, x2\n"
    "  mov x20, x3\n"
    "  mov x1, x4\n"
    "  mov x2, x5\n" SWIFT_CALL_EPILOGUE);

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

// The tree the action belongs to may have been replaced since; a stale id must not touch another binding.
void swiftui_set_text(int action, const std::string& text) {
    if (action < 0 || static_cast<size_t>(action) >= g_actions.size() || g_actions[static_cast<size_t>(action)].kind != ActionKind::SetText) return;
    SwiftString value = swift_string_from_utf8(text);
    binding_write(g_actions[static_cast<size_t>(action)].binding, &value, string_metadata());
}

void swiftui_attach_image(const MachImage& image) {
    g_view_conformances = image.section("__TEXT", "__swift5_proto");
    if (g_view_conformances != nullptr) swift_runtime_attach_conformances(reinterpret_cast<const void*>(g_view_conformances->address), g_view_conformances->size);
}

void swiftui_run_scene(void* scene) {
    auto* box = *static_cast<ViewBox**>(scene);
    const size_t size = witnesses_of(box->content_type)->size;
    g_root_content = box->closure == nullptr ? box->closure_context : calloc(1, size == 0 ? 1 : size);
    g_root_type = box->content_type;
    engine_log("SCENE: running WindowGroup content");
    if (box->closure != nullptr) rt_call_witness(box->closure, g_root_content, box->closure_context, nullptr, nullptr);

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

// UIApplicationDelegateAdaptor holds the delegate object, created as UIKit would before the first scene.
SWIFTCC void delegate_adaptor_init(SWIFT_OUT void** out, void* delegate_type) {
    engine_log("GUEST: creating the application delegate");
    *out = objc_alloc_init(delegate_type);
}

namespace swiftui {

void add_swiftui_render_symbols(SymbolTable& table) {
    table.insert({
        {"$s7SwiftUI28UIApplicationDelegateAdaptorVyACyxGxmcfC", reinterpret_cast<void*>(delegate_adaptor_init)},
    });
}

}  // namespace swiftui
