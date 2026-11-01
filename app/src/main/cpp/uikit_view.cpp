#include <algorithm>
#include <cstdint>
#include <cstring>
#include <map>
#include <string>
#include <unordered_map>
#include <vector>

#include "concurrency_shim.h"
#include "engine.h"
#include "foundation_internal.h"
#include "json.h"
#include "plist.h"
#include "swift_abi.h"
#include "uikit_internal.h"

using foundation::new_nsarray;
using foundation::new_nsstring;
using foundation::NSArrayObject;
using foundation::nsstring_text;

namespace uikit {

namespace {

constexpr size_t kBlockInvokeWord = 2;

struct Rect {
    double x, y, width, height;
};
struct Point {
    double x, y;
};
struct Size {
    double width, height;
};

using Message0 = void* (*)(void*, const char*);
using Message1 = void* (*)(void*, const char*, void*);

void* send0(void* receiver, const char* selector) { return reinterpret_cast<Message0>(const_cast<void*>(objc_method_for(receiver, selector)))(receiver, selector); }
void* send1(void* receiver, const char* selector, void* argument) { return reinterpret_cast<Message1>(const_cast<void*>(objc_method_for(receiver, selector)))(receiver, selector, argument); }

void send_bool(void* receiver, const char* selector, bool value) {
    if (!objc_responds(receiver, selector)) return;
    using Message = void (*)(void*, const char*, bool);
    reinterpret_cast<Message>(const_cast<void*>(objc_method_for(receiver, selector)))(receiver, selector, value);
}

void send_if_responds(void* receiver, const char* selector) {
    if (receiver != nullptr && objc_responds(receiver, selector)) send0(receiver, selector);
}

NSArrayObject* array_property(void* object, const char* key) {
    auto* array = static_cast<NSArrayObject*>(property_get_object(object, key));
    if (array == nullptr) {
        array = static_cast<NSArrayObject*>(new_nsarray());
        property_set_object(object, key, array);
    }
    return array;
}

Rect frame_of(void* view) {
    double bounds[4], center[4];
    property_get_rect(view, "bounds", bounds);
    property_get_rect(view, "center", center);
    return {center[0] - bounds[2] / 2, center[1] - bounds[3] / 2, bounds[2], bounds[3]};
}

void set_frame(void* view, const Rect& frame) {
    double bounds[4];
    property_get_rect(view, "bounds", bounds);
    bounds[2] = frame.width;
    bounds[3] = frame.height;
    property_set_rect(view, "bounds", bounds);
    const double center[4] = {frame.x + frame.width / 2, frame.y + frame.height / 2, 0, 0};
    property_set_rect(view, "center", center);
}

Rect view_frame(void* self, const char*) { return frame_of(self); }

Rect view_bounds(void* self, const char*) {
    double bounds[4];
    property_get_rect(self, "bounds", bounds);
    return {bounds[0], bounds[1], bounds[2], bounds[3]};
}

void view_set_frame(void* self, const char*, Rect frame) { set_frame(self, frame); }

void view_set_bounds(void* self, const char*, Rect rect) {
    const double bounds[4] = {rect.x, rect.y, rect.width, rect.height};
    property_set_rect(self, "bounds", bounds);
}

void* view_init_frame(void* self, const char*, Rect frame) {
    set_frame(self, frame);
    return self;
}

void view_remove_from_superview(void* self, const char*) {
    void* parent = property_get_object(self, "superview");
    if (parent == nullptr) return;
    auto* items = array_property(parent, "subviews")->items;
    items->erase(std::remove(items->begin(), items->end(), self), items->end());
    property_set_object(self, "superview", nullptr);
}

void attach_subview(void* parent, void* child, size_t position) {
    view_remove_from_superview(child, nullptr);
    auto* items = array_property(parent, "subviews")->items;
    items->insert(items->begin() + static_cast<long>(position < items->size() ? position : items->size()), object_retain(child));
    property_set_object(child, "superview", parent);
}

void view_add_subview(void* self, const char*, void* child) {
    if (child != nullptr) attach_subview(self, child, SIZE_MAX);
}

void view_insert_at(void* self, const char*, void* child, int64_t index) { attach_subview(self, child, static_cast<size_t>(index)); }

void view_insert_relative(void* self, void* child, void* other, size_t shift) {
    auto* items = array_property(self, "subviews")->items;
    const auto found = std::find(items->begin(), items->end(), other);
    attach_subview(self, child, static_cast<size_t>(found - items->begin()) + shift);
}

void view_insert_below(void* self, const char*, void* child, void* other) { view_insert_relative(self, child, other, 0); }
void view_insert_above(void* self, const char*, void* child, void* other) { view_insert_relative(self, child, other, 1); }

void* view_subviews(void* self, const char*) { return array_property(self, "subviews"); }

void* view_window(void* self, const char*) {
    void* view = self;
    while (view != nullptr) {
        if (object_is_kind(view, "UIWindow")) return view;
        view = property_get_object(view, "superview");
    }
    return nullptr;
}

void* view_constraints(void* self, const char*) { return array_property(self, "viewAutolayoutConstraints"); }

void view_add_constraint(void* self, const char*, void* constraint) { array_property(self, "viewAutolayoutConstraints")->items->push_back(object_retain(constraint)); }

void view_add_constraints(void* self, const char*, void* constraints) {
    auto* target = array_property(self, "viewAutolayoutConstraints")->items;
    for (void* constraint : object_array_items(constraints)) target->push_back(object_retain(constraint));
}

void* run_block_now(void* block) {
    if (block == nullptr) return nullptr;
    using Invoke = void (*)(void*);
    reinterpret_cast<Invoke>(static_cast<void**>(block)[kBlockInvokeWord])(block);
    return nullptr;
}

void finish_block(void* block) {
    if (block == nullptr) return;
    using Invoke = void (*)(void*, bool);
    reinterpret_cast<Invoke>(static_cast<void**>(block)[kBlockInvokeWord])(block, true);
}

// Animations jump straight to their end state.
void animate_simple(void*, const char*, void* animations, void* completion, double) {
    run_block_now(animations);
    finish_block(completion);
}

void animate_only(void*, const char*, void* animations, double) { run_block_now(animations); }

void transition_with_view(void*, const char*, void*, uint64_t, void* animations, void* completion, double) {
    run_block_now(animations);
    finish_block(completion);
}

void animate_options(void*, const char*, uint64_t, void* animations, void* completion, double, double) {
    run_block_now(animations);
    finish_block(completion);
}

void animate_spring(void*, const char*, uint64_t, void* animations, void* completion, double, double, double, double) {
    run_block_now(animations);
    finish_block(completion);
}

void perform_without_animation(void*, const char*, void* actions) { run_block_now(actions); }

void* new_named(const char* class_name) {
    void* object = objc_framework_object(class_name);
    return object;
}

void* get_or_create(void* self, const char* key, const char* class_name) {
    void* value = property_get_object(self, key);
    if (value == nullptr) {
        value = new_named(class_name);
        property_set_object(self, key, value);
    }
    return value;
}

void* view_layer(void* self, const char*) { return get_or_create(self, "layer", "CALayer"); }
void* trait_collection(void* self, const char*) { return get_or_create(self, "traitCollection", "UITraitCollection"); }
void* view_safe_area_guide(void* self, const char*) { return get_or_create(self, "safeAreaLayoutGuide", "UILayoutGuide"); }
void* view_margins_guide(void* self, const char*) { return get_or_create(self, "layoutMarginsGuide", "UILayoutGuide"); }
void* button_with_type(void*, const char*, int64_t) { return new_named("UIButton"); }
void* button_title_label(void* self, const char*) { return get_or_create(self, "titleLabel", "UILabel"); }

// Buttons here have one look, so the states they're given all set it.
void button_set_title(void* self, const char*, void* title, uint64_t) { property_set_object(self, "buttonTitle", title); }
void* button_title(void* self, const char*, uint64_t) { return property_get_object(self, "buttonTitle"); }
void* button_current_title(void* self, const char*) { return property_get_object(self, "buttonTitle"); }
void button_set_title_color(void* self, const char*, void* color, uint64_t) { property_set_object(self, "textColor", color); }
void button_set_image(void* self, const char*, void* image, uint64_t) { property_set_object(self, "buttonImage", image); }

void* tab_bar_item(void* self, const char*) { return get_or_create(self, "tabBarItem", "UITabBarItem"); }
void* navigation_item(void* self, const char*) { return get_or_create(self, "navigationItem", "UINavigationItem"); }

// Controllers here always fill the screen, and setup code commonly copies this size in viewDidLoad.
void size_to_screen(void* view) {
    const Rect4 screen = window_size_rect();
    double previous[4];
    property_get_rect(view, "bounds", previous);
    uikit_autoresize_children(view, screen.width - previous[2], screen.height - previous[3]);
    const double bounds[4] = {0, 0, screen.width, screen.height};
    const double center[4] = {screen.width / 2, screen.height / 2, 0, 0};
    property_set_rect(view, "bounds", bounds);
    property_set_rect(view, "center", center);
}

void load_view_of(void* controller) {
    if (property_get_object(controller, "view") != nullptr) return;
    void* name = property_get_object(controller, "nibName");
    void* storyboard = property_get_object(controller, "storyboard");
    if (name != nullptr && storyboard != nullptr) {
        const std::string path = storyboard_directory(storyboard) + "/" + nsstring_text(name) + ".nib";
        nib_instantiate(path, controller, storyboard, property_get_object(controller, "externalObjectsTableForViewLoading"));
    }
    if (property_get_object(controller, "view") == nullptr && objc_responds(controller, "loadView")) send0(controller, "loadView");
    if (property_get_object(controller, "view") == nullptr) {
        void* view = new_named("UIView");
        property_set_object(controller, "view", view);
    }
    void* view = property_get_object(controller, "view");
    property_set_object(view, "viewController", controller);
    size_to_screen(view);
    send_if_responds(controller, "viewDidLoad");
}

void stack_add_arranged(void* self, const char*, void* child) {
    array_property(self, "stackViewArrangedSubviews")->items->push_back(object_retain(child));
    attach_subview(self, child, SIZE_MAX);
}

void* stack_arranged(void* self, const char*) { return array_property(self, "stackViewArrangedSubviews"); }

void tab_set_controllers(void* self, const char*, void* controllers, bool) {
    auto* items = array_property(self, "viewControllers")->items;
    items->clear();
    for (void* controller : object_array_items(controllers)) {
        items->push_back(object_retain(controller));
        property_set_object(controller, "parentViewController", self);
    }
}

void* make_font(double size) {
    void* font = objc_framework_object("UIFont");
    property_set_floats(font, "fontPointSize", size, 0);
    return font;
}

constexpr double kBodyFontSize = 17;

void* font_of_size(void*, const char*, double size) { return make_font(size); }
void* font_of_size_weight(void*, const char*, double size, double) { return make_font(size); }
void* font_named(void*, const char*, void*, double size) { return make_font(size); }

// The sizes iOS uses at its default Dynamic Type setting.
void* font_for_style(void*, const char*, void* style) {
    static const std::unordered_map<std::string, double> kSizes = {
        {"UIFontTextStyleLargeTitle", 34},
        {"UIFontTextStyleTitle1", 28},
        {"UIFontTextStyleTitle2", 22},
        {"UIFontTextStyleTitle3", 20},
        {"UIFontTextStyleHeadline", 17},
        {"UIFontTextStyleSubheadline", 15},
        {"UIFontTextStyleBody", 17},
        {"UIFontTextStyleCallout", 16},
        {"UIFontTextStyleFootnote", 13},
    };
    const auto found = kSizes.find(nsstring_text(style));
    return make_font(found != kSizes.end() ? found->second : kBodyFontSize);
}

// Labels and buttons have a font from the start, which apps read to derive others.
void* view_font(void* self, const char*) {
    constexpr double kDefaultFontSize = 17;
    void* font = property_get_object(self, "font");
    if (font == nullptr) {
        font = new_named("UIFont");
        property_set_floats(font, "fontPointSize", kDefaultFontSize, 0);
        property_set_object(self, "font", font);
    }
    return font;
}

double font_point_size(void* self, const char*) { return property_get_float(self, "fontPointSize"); }

void* controller_view_or_nil(void* self) { return property_get_object(self, "view"); }

void* controller_view(void* self, const char*) {
    if (object_is_kind(self, "UIViewController")) load_view_of(self);
    return controller_view_or_nil(self);
}

void controller_load_if_needed(void* self, const char*) { load_view_of(self); }
bool controller_is_loaded(void* self, const char*) { return controller_view_or_nil(self) != nullptr; }

void* controller_children(void* self, const char*) { return array_property(self, "childViewControllers"); }

void controller_add_child(void* self, const char*, void* child) {
    array_property(self, "childViewControllers")->items->push_back(object_retain(child));
    property_set_object(child, "parentViewController", self);
}

void controller_remove_from_parent(void* self, const char*) {
    void* parent = property_get_object(self, "parentViewController");
    if (parent == nullptr) return;
    auto* items = array_property(parent, "childViewControllers")->items;
    items->erase(std::remove(items->begin(), items->end(), self), items->end());
    property_set_object(self, "parentViewController", nullptr);
}

void* ancestor_of_kind(void* self, const char* kind) {
    for (void* parent = property_get_object(self, "parentViewController"); parent != nullptr; parent = property_get_object(parent, "parentViewController")) {
        if (object_is_kind(parent, kind)) return parent;
    }
    return nullptr;
}

void* controller_navigation(void* self, const char*) { return ancestor_of_kind(self, "UINavigationController"); }
void* controller_tab_bar(void* self, const char*) { return ancestor_of_kind(self, "UITabBarController"); }

void appear(void* controller) {
    if (controller == nullptr) return;
    property_set_word(controller, "isOnScreen", 1);
    load_view_of(controller);
    send_bool(controller, "viewWillAppear:", false);
    send_if_responds(controller, "viewWillLayoutSubviews");
    send_if_responds(controller, "viewDidLayoutSubviews");
    send_bool(controller, "viewDidAppear:", false);
}

void disappear(void* controller) {
    if (controller == nullptr) return;
    property_set_word(controller, "isOnScreen", 0);
    send_bool(controller, "viewWillDisappear:", false);
    send_bool(controller, "viewDidDisappear:", false);
}

void* top_of_stack(void* navigation) {
    auto* items = array_property(navigation, "viewControllers")->items;
    return items->empty() ? nullptr : items->back();
}

void* nav_top(void* self, const char*) { return top_of_stack(self); }

void nav_set_controllers(void* self, const char*, void* controllers, bool) {
    auto* items = array_property(self, "viewControllers")->items;
    void* previous = top_of_stack(self);
    items->clear();
    for (void* controller : object_array_items(controllers)) {
        items->push_back(object_retain(controller));
        property_set_object(controller, "parentViewController", self);
    }
    if (top_of_stack(self) != previous && window_is_visible()) {
        disappear(previous);
        appear(top_of_stack(self));
    }
}

void nav_push(void* self, const char*, void* controller, bool) {
    void* previous = top_of_stack(self);
    array_property(self, "viewControllers")->items->push_back(object_retain(controller));
    property_set_object(controller, "parentViewController", self);
    if (window_is_visible()) {
        disappear(previous);
        appear(controller);
    }
}

void* nav_pop(void* self, const char*, bool) {
    auto* items = array_property(self, "viewControllers")->items;
    if (items->size() < 2) return nullptr;
    void* popped = items->back();
    items->pop_back();
    if (window_is_visible()) {
        disappear(popped);
        appear(items->back());
    }
    return popped;
}

void* nav_init_root(void* self, const char*, void* root) {
    array_property(self, "viewControllers")->items->push_back(object_retain(root));
    property_set_object(root, "parentViewController", self);
    return self;
}

void* tab_selected_controller(void* self) {
    auto* items = array_property(self, "viewControllers")->items;
    if (items->empty()) return nullptr;
    const auto index = static_cast<size_t>(property_get_word(self, "selectedIndex"));
    return (*items)[index < items->size() ? index : 0];
}

void* tab_selected(void* self, const char*) { return tab_selected_controller(self); }

void tab_set_selected_index(void* self, const char*, int64_t index) {
    void* previous = tab_selected_controller(self);
    property_set_word(self, "selectedIndex", index);
    if (window_is_visible() && previous != tab_selected_controller(self)) {
        disappear(previous);
        appear(tab_selected_controller(self));
    }
}

void* alert_make(void*, const char*, void* title, void* message, int64_t style) {
    void* alert = objc_framework_object("UIAlertController");
    property_set_object(alert, "title", title);
    property_set_object(alert, "message", message);
    property_set_word(alert, "preferredStyle", style);
    return alert;
}

void* alert_action_make(void*, const char*, void* title, int64_t style, void* handler) {
    void* action = objc_framework_object("UIAlertAction");
    property_set_object(action, "title", title);
    property_set_word(action, "style", style);
    if (handler != nullptr) property_set_object(action, "handler", foundation::darwin_block_copy(handler));
    return action;
}

void alert_add_action(void* self, const char*, void* action) { array_property(self, "actions")->items->push_back(object_retain(action)); }

void send_to_delegate(void* delegate, const char* selector, void* controller, void* argument) {
    if (delegate == nullptr || !objc_responds(delegate, selector)) return;
    using Send = void (*)(void*, const char*, void*, void*);
    reinterpret_cast<Send>(const_cast<void*>(objc_method_for(delegate, selector)))(delegate, selector, controller, argument);
}

// A document picker is Android's file picker. Apps expect a path they can read, so chosen files are copied in first.
void present_document_picker(void* presenter, void* controller) {
    object_retain(controller);
    const int id = engine_host_register(
        [presenter, controller](const std::string& payload) {
            property_set_object(presenter, "presentedViewController", nullptr);
            void* delegate = property_get_object(controller, "delegate");
            Json reply;
            std::vector<void*> urls;
            if (json_parse(payload, reply)) {
                if (const Json* paths = reply.member("paths")) {
                    for (const Json& path : paths->items) urls.push_back(foundation_file_url_object(path.text));
                }
            }
            if (urls.empty()) {
                send_to_delegate(delegate, "documentPickerWasCancelled:", controller, nullptr);
                return;
            }
            send_to_delegate(delegate, "documentPicker:didPickDocumentsAtURLs:", controller, foundation_new_array(urls));
        },
        false);
    Json identifier;
    identifier.kind = Json::Kind::Integer;
    identifier.integer = id;
    Json multiple;
    multiple.kind = Json::Kind::Bool;
    multiple.flag = property_get_word(controller, "allowsMultipleSelection") != 0;
    Json spec;
    spec.kind = Json::Kind::Object;
    spec.members = {{"id", identifier},
                    {"types", json_text(foundation::nsstring_text(property_get_object(controller, "pickerMimeTypes")))},
                    {"multiple", multiple},
                    {"directory", json_text(engine_bundle_dir() + "/.tmp/Inbox")}};
    engine_emit("pick-files", json_write(spec));
}

// SFSafariViewController is a Chrome Custom Tab: the host shows it, and says when the user has come back.
void present_custom_tab(void* presenter, void* controller) {
    const int id = engine_host_register(
        [presenter, controller](const std::string&) {
            property_set_object(presenter, "presentedViewController", nullptr);
            void* delegate = property_get_object(controller, "delegate");
            constexpr const char* kDidFinish = "safariViewControllerDidFinish:";
            if (delegate != nullptr && objc_responds(delegate, kDidFinish)) {
                using Finish = void (*)(void*, const char*, void*);
                reinterpret_cast<Finish>(const_cast<void*>(objc_method_for(delegate, kDidFinish)))(delegate, kDidFinish, controller);
            }
        },
        false);
    Json identifier;
    identifier.kind = Json::Kind::Integer;
    identifier.integer = id;
    Json spec;
    spec.kind = Json::Kind::Object;
    spec.members = {{"id", identifier},
                    {"url", json_text(foundation::nsstring_text(property_get_object(controller, "safariURL")))},
                    {"bar", json_text(color_hex_text(property_get_object(controller, "preferredBarTintColor")))}};
    engine_emit("custom-tab", json_write(spec));
}

void present_controller(void* self, const char*, void* controller, bool, void* completion) {
    property_set_object(self, "presentedViewController", controller);
    property_set_object(controller, "presentingViewController", self);
    if (object_is_kind(controller, "UIDocumentPickerViewController")) {
        present_document_picker(self, controller);
        run_block_now(completion);
        return;
    }
    if (object_is_kind(controller, "SFSafariViewController")) {
        present_custom_tab(self, controller);
        run_block_now(completion);
        return;
    }
    // An alert is drawn by the host as a dialog, and has no view of its own to load.
    if (!object_is_kind(controller, "UIAlertController")) appear(controller);
    run_block_now(completion);
}

void dismiss_controller(void* self, const char*, bool, void* completion) {
    void* presented = property_get_object(self, "presentedViewController");
    void* presenter = presented != nullptr ? self : property_get_object(self, "presentingViewController");
    if (presenter != nullptr) {
        disappear(property_get_object(presenter, "presentedViewController"));
        property_set_object(presenter, "presentedViewController", nullptr);
    }
    run_block_now(completion);
}

void* g_key_window = nullptr;

void window_make_visible(void* self, const char*) {
    bool first = g_key_window == nullptr;
    g_key_window = self;
    if (!first) return;
    void* root = property_get_object(self, "rootViewController");
    appear(root);
}

void window_set_root(void* self, const char*, void* controller) {
    void* previous = property_get_object(self, "rootViewController");
    property_set_object(self, "rootViewController", controller);
    if (g_key_window == self && previous != controller) {
        disappear(previous);
        appear(controller);
    }
}

// Storyboards: a directory of nibs, and a plist naming the controllers in it.
struct Storyboard {
    std::string directory;
    PlistValue info;
};

std::unordered_map<void*, Storyboard>& storyboards() {
    static std::unordered_map<void*, Storyboard> table;
    return table;
}

void* storyboard_named(const std::string& name) {
    const std::string directory = engine_bundle_dir() + "/" + name + ".storyboardc";
    PlistValue info;
    if (!plist_read_file(directory + "/Info.plist", info)) {
        engine_log("UIKIT: no storyboard %s", name.c_str());
        return nullptr;
    }
    void* object = objc_framework_object("UIStoryboard");
    storyboards()[object] = {directory, info};
    return object;
}

void* controller_from(void* storyboard, const std::string& identifier) {
    const auto found = storyboards().find(storyboard);
    if (found == storyboards().end()) return nullptr;
    const PlistValue* nibs = found->second.info.get("UIViewControllerIdentifiersToNibNames");
    if (nibs != nullptr && nibs->get(identifier) != nullptr) {
        const std::vector<void*> top = nib_instantiate(found->second.directory + "/" + nibs->string_at(identifier) + ".nib", nullptr, storyboard);
        for (void* object : top) {
            if (object_is_kind(object, "UIViewController")) {
                property_set_object(object, "storyboard", storyboard);
                return object;
            }
        }
        engine_log("UIKIT: no view controller in the nib for %s", identifier.c_str());
        return nullptr;
    }
    const PlistValue* references = found->second.info.get("UIViewControllerIdentifiersToExternalStoryboardReferences");
    const PlistValue* reference = references != nullptr ? references->get(identifier) : nullptr;
    if (reference != nullptr) {
        void* other = storyboard_named(reference->string_at("UIReferencedStoryboardName"));
        const std::string referenced = reference->string_at("UIReferencedControllerIdentifier");
        if (other == nullptr) return nullptr;
        if (!referenced.empty()) return controller_from(other, referenced);
        const std::string entry = storyboards()[other].info.string_at("UIStoryboardDesignatedEntryPointIdentifier");
        return controller_from(other, entry);
    }
    engine_log("UIKIT: no controller %s in storyboard", identifier.c_str());
    return nullptr;
}

void* storyboard_with_name(void*, const char*, void* name, void*) { return storyboard_named(nsstring_text(name)); }

void* storyboard_initial(void* self, const char*) {
    const auto found = storyboards().find(self);
    return found == storyboards().end() ? nullptr : controller_from(self, found->second.info.string_at("UIStoryboardDesignatedEntryPointIdentifier"));
}

void* storyboard_with_identifier(void* self, const char*, void* identifier) { return controller_from(self, nsstring_text(identifier)); }

constexpr double kScreenScale = 3;

Rect screen_bounds(void*, const char*) {
    const Rect4 size = window_size_rect();
    return {size.x, size.y, size.width, size.height};
}
double screen_scale(void*, const char*) { return kScreenScale; }

void* main_screen(void*, const char*) {
    static void* screen = objc_framework_object("UIScreen");
    return screen;
}

void set_autolayout_flag(void* self, const char*, bool value) { property_set_word(self, "translatesAutoresizingMaskIntoConstraints", value); }

}  // namespace

void uikit_dismiss_presented(void* controller) {
    void* presenter = property_get_object(controller, "presentingViewController");
    if (presenter != nullptr) property_set_object(presenter, "presentedViewController", nullptr);
}

void uikit_run_alert_action(void* action) {
    void* handler = property_get_object(action, "handler");
    if (handler == nullptr) return;
    using Invoke = void (*)(void*, void*);
    reinterpret_cast<Invoke>(static_cast<void**>(handler)[kBlockInvokeWord])(handler, action);
}

std::string storyboard_directory(void* storyboard) {
    const auto found = storyboards().find(storyboard);
    return found == storyboards().end() ? std::string() : found->second.directory;
}

void* storyboard_create_initial(const std::string& name) {
    void* storyboard = storyboard_named(name);
    return storyboard != nullptr ? storyboard_initial(storyboard, nullptr) : nullptr;
}

bool window_is_visible() { return g_key_window != nullptr; }
void* key_window() { return g_key_window; }

const void* uikit_method_for_selector(const std::string& selector) {
    static const std::unordered_map<std::string_view, const void*> table = {
        {"initWithCoder:", nib_init_with_coder()},
        {"initWithFrame:", reinterpret_cast<const void*>(view_init_frame)},
        {"frame", reinterpret_cast<const void*>(view_frame)},
        {"bounds", reinterpret_cast<const void*>(view_bounds)},
        {"setFrame:", reinterpret_cast<const void*>(view_set_frame)},
        {"setBounds:", reinterpret_cast<const void*>(view_set_bounds)},
        {"addSubview:", reinterpret_cast<const void*>(view_add_subview)},
        {"insertSubview:atIndex:", reinterpret_cast<const void*>(view_insert_at)},
        {"insertSubview:belowSubview:", reinterpret_cast<const void*>(view_insert_below)},
        {"insertSubview:aboveSubview:", reinterpret_cast<const void*>(view_insert_above)},
        {"removeFromSuperview", reinterpret_cast<const void*>(view_remove_from_superview)},
        {"subviews", reinterpret_cast<const void*>(view_subviews)},
        {"window", reinterpret_cast<const void*>(view_window)},
        {"constraints", reinterpret_cast<const void*>(view_constraints)},
        {"addConstraint:", reinterpret_cast<const void*>(view_add_constraint)},
        {"addConstraints:", reinterpret_cast<const void*>(view_add_constraints)},
        {"addArrangedSubview:", reinterpret_cast<const void*>(stack_add_arranged)},
        {"arrangedSubviews", reinterpret_cast<const void*>(stack_arranged)},
        {"font", reinterpret_cast<const void*>(view_font)},
        {"systemFontOfSize:", reinterpret_cast<const void*>(font_of_size)},
        {"boldSystemFontOfSize:", reinterpret_cast<const void*>(font_of_size)},
        {"italicSystemFontOfSize:", reinterpret_cast<const void*>(font_of_size)},
        {"systemFontOfSize:weight:", reinterpret_cast<const void*>(font_of_size_weight)},
        {"monospacedSystemFontOfSize:weight:", reinterpret_cast<const void*>(font_of_size_weight)},
        {"fontWithName:size:", reinterpret_cast<const void*>(font_named)},
        {"fontWithSize:", reinterpret_cast<const void*>(+[](void*, const char*, double size) -> void* { return make_font(size); })},
        {"preferredFontForTextStyle:", reinterpret_cast<const void*>(font_for_style)},
        {"pointSize", reinterpret_cast<const void*>(font_point_size)},
        {"layer", reinterpret_cast<const void*>(view_layer)},
        {"traitCollection", reinterpret_cast<const void*>(trait_collection)},
        {"safeAreaLayoutGuide", reinterpret_cast<const void*>(view_safe_area_guide)},
        {"layoutMarginsGuide", reinterpret_cast<const void*>(view_margins_guide)},
        {"setTranslatesAutoresizingMaskIntoConstraints:", reinterpret_cast<const void*>(set_autolayout_flag)},
        {"animateWithDuration:animations:completion:", reinterpret_cast<const void*>(animate_simple)},
        {"animateWithDuration:animations:", reinterpret_cast<const void*>(animate_only)},
        {"transitionWithView:duration:options:animations:completion:", reinterpret_cast<const void*>(transition_with_view)},
        {"animateWithDuration:delay:options:animations:completion:", reinterpret_cast<const void*>(animate_options)},
        {"animateWithDuration:delay:usingSpringWithDamping:initialSpringVelocity:options:animations:completion:", reinterpret_cast<const void*>(animate_spring)},
        {"performWithoutAnimation:", reinterpret_cast<const void*>(perform_without_animation)},
        {"view", reinterpret_cast<const void*>(controller_view)},
        {"loadViewIfNeeded", reinterpret_cast<const void*>(controller_load_if_needed)},
        {"isViewLoaded", reinterpret_cast<const void*>(controller_is_loaded)},
        {"childViewControllers", reinterpret_cast<const void*>(controller_children)},
        {"addChildViewController:", reinterpret_cast<const void*>(controller_add_child)},
        {"removeFromParentViewController", reinterpret_cast<const void*>(controller_remove_from_parent)},
        {"navigationController", reinterpret_cast<const void*>(controller_navigation)},
        {"tabBarController", reinterpret_cast<const void*>(controller_tab_bar)},
        {"tabBarItem", reinterpret_cast<const void*>(tab_bar_item)},
        {"buttonWithType:", reinterpret_cast<const void*>(button_with_type)},
        {"titleLabel", reinterpret_cast<const void*>(button_title_label)},
        {"setTitle:forState:", reinterpret_cast<const void*>(button_set_title)},
        {"titleForState:", reinterpret_cast<const void*>(button_title)},
        {"currentTitle", reinterpret_cast<const void*>(button_current_title)},
        {"setTitleColor:forState:", reinterpret_cast<const void*>(button_set_title_color)},
        {"setImage:forState:", reinterpret_cast<const void*>(button_set_image)},
        {"collectionView", reinterpret_cast<const void*>(controller_view)},
        {"tableView", reinterpret_cast<const void*>(controller_view)},
        {"navigationItem", reinterpret_cast<const void*>(navigation_item)},
        {"topViewController", reinterpret_cast<const void*>(nav_top)},
        {"visibleViewController", reinterpret_cast<const void*>(nav_top)},
        {"setViewControllers:animated:", reinterpret_cast<const void*>(nav_set_controllers)},
        {"pushViewController:animated:", reinterpret_cast<const void*>(nav_push)},
        {"popViewControllerAnimated:", reinterpret_cast<const void*>(nav_pop)},
        {"initWithRootViewController:", reinterpret_cast<const void*>(nav_init_root)},
        {"selectedViewController", reinterpret_cast<const void*>(tab_selected)},
        {"setSelectedIndex:", reinterpret_cast<const void*>(tab_set_selected_index)},
        {"alertControllerWithTitle:message:preferredStyle:", reinterpret_cast<const void*>(alert_make)},
        {"actionWithTitle:style:handler:", reinterpret_cast<const void*>(alert_action_make)},
        {"addAction:", reinterpret_cast<const void*>(alert_add_action)},
        {"presentViewController:animated:completion:", reinterpret_cast<const void*>(present_controller)},
        {"dismissViewControllerAnimated:completion:", reinterpret_cast<const void*>(dismiss_controller)},
        {"makeKeyAndVisible", reinterpret_cast<const void*>(window_make_visible)},
        {"setRootViewController:", reinterpret_cast<const void*>(window_set_root)},
        {"storyboardWithName:bundle:", reinterpret_cast<const void*>(storyboard_with_name)},
        {"instantiateInitialViewController", reinterpret_cast<const void*>(storyboard_initial)},
        {"instantiateViewControllerWithIdentifier:", reinterpret_cast<const void*>(storyboard_with_identifier)},
        {"mainScreen", reinterpret_cast<const void*>(main_screen)},
        {"scale", reinterpret_cast<const void*>(screen_scale)},
        {"nativeScale", reinterpret_cast<const void*>(screen_scale)},
    };
    const auto found = table.find(selector);
    if (found != table.end()) return found->second;
    if (const void* layout = uikit_layout_method_for_selector(selector)) return layout;
    if (const void* collection = uikit_collection_method_for_selector(selector)) return collection;
    return uikit_color_method_for_selector(selector);
}

void register_view_classes() {
    objc_register_method("-[UIScreen bounds]", reinterpret_cast<const void*>(screen_bounds));
    objc_register_method("-[UIScreen nativeBounds]", reinterpret_cast<const void*>(screen_bounds));
}

}  // namespace uikit

// A container's child first appears when the container draws it.
void uikit::show_contained_controller(void* controller) {
    if (uikit::property_get_word(controller, "isOnScreen") == 0) uikit::appear(controller);
}

// A tab in another storyboard is a placeholder until it's needed; only the tab bar knows its item if the target has none.
void uikit::resolve_tab_placeholders(void* controller) {
    constexpr char kPlaceholderClass[] = "UIStoryboardViewControllerPlaceholder";
    auto* tabs = uikit::array_property(controller, "viewControllers")->items;
    void* bar = uikit::property_get_object(controller, "tabBar");
    const std::vector<void*> bar_items = bar != nullptr ? object_array_items(uikit::property_get_object(bar, "items")) : std::vector<void*>{};
    for (size_t i = 0; i < tabs->size(); ++i) {
        void* tab = (*tabs)[i];
        if (objc_framework_root_of(tab) != kPlaceholderClass) continue;
        void* identifier = uikit::property_get_object(tab, "storyboardIdentifier");
        void* storyboard = uikit::property_get_object(controller, "storyboard");
        void* resolved = identifier != nullptr && storyboard != nullptr ? uikit::controller_from(storyboard, foundation::nsstring_text(identifier)) : nullptr;
        if (resolved == nullptr) continue;
        (*tabs)[i] = uikit::object_retain(resolved);
        uikit::property_set_object(resolved, "parentViewController", controller);
        void* own_item = uikit::property_get_object(resolved, "tabBarItem");
        const bool has_title = own_item != nullptr && uikit::property_get_object(own_item, "title") != nullptr;
        if (!has_title && i < bar_items.size()) uikit::property_set_object(resolved, "tabBarItem", bar_items[i]);
    }
}
