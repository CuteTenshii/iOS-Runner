#include <cmath>
#include <cstdint>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

#include "engine.h"
#include "foundation_internal.h"
#include "json.h"
#include "swift_abi.h"
#include "uikit_assets.h"
#include "uikit_internal.h"

using foundation::NSArrayObject;
using foundation::nsstring_text;

namespace uikit {

namespace {

constexpr double kFallbackWidth = 390, kFallbackHeight = 844;

Json number(double value) {
    Json out;
    out.kind = Json::Kind::Number;
    out.number = value;
    return out;
}

Json object_of(std::vector<std::pair<std::string, Json>> members) {
    Json out;
    out.kind = Json::Kind::Object;
    out.members = std::move(members);
    return out;
}

Json array_of(std::vector<Json> items) {
    Json out;
    out.kind = Json::Kind::Array;
    out.items = std::move(items);
    return out;
}

// Colors from nibs carry their resolved components, under the keys of their archived form.
std::string color_text(void* color) {
    if (color == nullptr) return {};
    const double alpha = property_get_float(color, "alpha");
    double red = property_get_float(color, "red"), green = property_get_float(color, "green"), blue = property_get_float(color, "blue");
    if (property_get_word(color, "colorComponentCount") == 2) red = green = blue = property_get_float(color, "white");
    const auto channel = [](double value) { return static_cast<int>(std::lround((value < 0 ? 0 : value > 1 ? 1
                                                                                                           : value) *
                                                                                255)); };
    char text[16];
    snprintf(text, sizeof text, "#%02x%02x%02x%02x", channel(alpha), channel(red), channel(green), channel(blue));
    return text;
}

std::string text_of(void* view) {
    void* text = property_get_object(view, "text");
    if (text != nullptr && objc_framework_class_name(*static_cast<void**>(text)) == "NSString") return nsstring_text(text);
    void* attributed = property_get_object(view, "attributedText");
    void* inner = attributed != nullptr ? property_get_object(attributed, "string") : nullptr;
    return inner != nullptr ? nsstring_text(inner) : std::string();
}

int add_action(std::function<void()> action);

Json describe_view(void* view, double parent_x, double parent_y) {
    double bounds[4], center[4];
    property_get_rect(view, "bounds", bounds);
    property_get_rect(view, "center", center);
    const double width = bounds[2], height = bounds[3];
    const double x = center[0] - width / 2, y = center[1] - height / 2;
    std::vector<std::pair<std::string, Json>> members = {
        {"k", json_text("uiview")},
        {"x", number(x)},
        {"y", number(y)},
        {"w", number(width)},
        {"h", number(height)},
        {"class", json_text(objc_class_name(*static_cast<void**>(view)))},
    };
    if (const std::string background = color_text(property_get_object(view, "backgroundColor")); !background.empty()) members.emplace_back("bg", json_text(background));
    if (const std::string text = text_of(view); !text.empty()) {
        members.emplace_back("text", json_text(text));
        if (const std::string color = color_text(property_get_object(view, "textColor")); !color.empty()) members.emplace_back("color", json_text(color));
        void* font = property_get_object(view, "font");
        if (font != nullptr) members.emplace_back("size", number(property_get_float(font, "fontPointSize")));
        members.emplace_back("align", number(static_cast<double>(property_get_word(view, "textAlignment"))));
        members.emplace_back("lines", number(static_cast<double>(property_get_word(view, "numberOfLines"))));
    }
    if (property_get_word(view, "hidden") != 0) members.emplace_back("hidden", number(1));
    if (void* target = property_get_object(view, "actionTarget")) {
        void* selector = property_get_object(view, "actionSelector");
        const int id = add_action([target, selector, view] { objc_send(target, nsstring_text(selector).c_str(), view); });
        members.emplace_back("a", number(id));
    }
    std::vector<Json> children;
    auto* subviews = static_cast<NSArrayObject*>(property_get_object(view, "subviews"));
    if (subviews != nullptr) {
        for (void* child : *subviews->items) children.push_back(describe_view(child, parent_x + x, parent_y + y));
    }
    members.emplace_back("c", array_of(std::move(children)));
    return object_of(std::move(members));
}

void* visible_controller(void* controller) {
    if (controller == nullptr) return nullptr;
    if (void* presented = property_get_object(controller, "presentedViewController")) return visible_controller(presented);
    return controller;
}

std::string g_last_tree;
std::string g_last_status_bar;
std::vector<std::function<void()>> g_actions;

int add_action(std::function<void()> action) {
    g_actions.push_back(std::move(action));
    return static_cast<int>(g_actions.size()) - 1;
}

Json tree_leaf(const char* kind, std::vector<std::pair<std::string, Json>> members) {
    members.insert(members.begin(), {"k", json_text(kind)});
    return object_of(std::move(members));
}

std::string text_property(void* object, const char* key) {
    void* value = property_get_object(object, key);
    return value != nullptr ? nsstring_text(value) : std::string();
}

// The screen an action or redraw concerns, down through containers and presented controllers.
void* top_controller(void* controller) {
    if (controller == nullptr) return nullptr;
    if (void* presented = property_get_object(controller, "presentedViewController"); presented != nullptr && !object_is_kind(presented, "UIAlertController")) return top_controller(presented);
    if (object_is_kind(controller, "UINavigationController")) {
        const auto items = object_array_items(property_get_object(controller, "viewControllers"));
        if (!items.empty()) return top_controller(items.back());
    }
    if (object_is_kind(controller, "UITabBarController")) {
        const auto items = object_array_items(property_get_object(controller, "viewControllers"));
        if (!items.empty()) {
            const auto index = static_cast<size_t>(property_get_word(controller, "selectedIndex"));
            return top_controller(items[index < items.size() ? index : 0]);
        }
    }
    return controller;
}

void* presented_alert(void* controller) {
    for (; controller != nullptr; controller = property_get_object(controller, "presentedViewController")) {
        if (object_is_kind(controller, "UIAlertController")) return controller;
    }
    return nullptr;
}

Json describe_alert(void* alert, void* window_root) {
    std::vector<Json> children;
    if (const std::string message = text_property(alert, "message"); !message.empty()) children.push_back(tree_leaf("text", {{"t", json_text(message)}}));
    for (void* action : object_array_items(property_get_object(alert, "actions"))) {
        const int id = add_action([action, alert, window_root] {
            uikit_dismiss_presented(alert);
            uikit_run_alert_action(action);
        });
        children.push_back(tree_leaf("button", {{"a", number(id)}, {"c", array_of({tree_leaf("text", {{"t", json_text(text_property(action, "title"))}})})}}));
    }
    // Every press also sends this one, which has nothing left to do by then.
    const int dismissed = add_action([] {});
    return tree_leaf("alert", {{"d", number(dismissed)}, {"t", json_text(text_property(alert, "title"))}, {"c", array_of(std::move(children))}});
}

constexpr int64_t kStatusBarLightContent = 1, kStatusBarDarkContent = 3;

int64_t status_bar_style(void* controller) {
    if (controller != nullptr && objc_responds(controller, "childViewControllerForStatusBarStyle")) {
        using Child = void* (*)(void*, const char*);
        void* child = reinterpret_cast<Child>(const_cast<void*>(objc_method_for(controller, "childViewControllerForStatusBarStyle")))(controller, "childViewControllerForStatusBarStyle");
        if (child != nullptr) return status_bar_style(child);
    }
    if (controller == nullptr || !objc_responds(controller, "preferredStatusBarStyle")) return 0;
    using Style = int64_t (*)(void*, const char*);
    return reinterpret_cast<Style>(const_cast<void*>(objc_method_for(controller, "preferredStatusBarStyle")))(controller, "preferredStatusBarStyle");
}

bool status_bar_hidden(void* controller) {
    if (controller == nullptr || !objc_responds(controller, "prefersStatusBarHidden")) return false;
    using Hidden = bool (*)(void*, const char*);
    return reinterpret_cast<Hidden>(const_cast<void*>(objc_method_for(controller, "prefersStatusBarHidden")))(controller, "prefersStatusBarHidden");
}

void update_status_bar(void* controller, void* view) {
    const int64_t style = status_bar_style(controller);
    Json state = tree_leaf("statusbar", {{"style", json_text(style == kStatusBarLightContent ? "light" : style == kStatusBarDarkContent ? "dark"
                                                                                                                                        : "default")},
                                         {"hidden", number(status_bar_hidden(controller) ? 1 : 0)},
                                         {"bg", json_text(color_text(property_get_object(view, "backgroundColor")))}});
    const std::string text = json_write(state);
    if (text == g_last_status_bar) return;
    g_last_status_bar = text;
    engine_emit("statusbar", text);
}

constexpr int64_t kFlexibleLeft = 1, kFlexibleWidth = 2, kFlexibleRight = 4, kFlexibleTop = 8, kFlexibleHeight = 16, kFlexibleBottom = 32;

// Shares a size change among whichever of the three parts along an axis are flexible, as UIKit does.
void autoresize_axis(double& origin, double& size, double delta, bool flex_before, bool flex_size, bool flex_after) {
    const int parts = (flex_before ? 1 : 0) + (flex_size ? 1 : 0) + (flex_after ? 1 : 0);
    if (parts == 0) return;
    const double share = delta / parts;
    if (flex_before) origin += share;
    if (flex_size) size += share;
}

void autoresize_children(void* view, double delta_width, double delta_height) {
    auto* subviews = static_cast<NSArrayObject*>(property_get_object(view, "subviews"));
    if (subviews == nullptr) return;
    for (void* child : *subviews->items) {
        const int64_t mask = property_get_word(child, "autoresizingMask");
        double bounds[4], center[4];
        property_get_rect(child, "bounds", bounds);
        property_get_rect(child, "center", center);
        double x = center[0] - bounds[2] / 2, y = center[1] - bounds[3] / 2;
        const double old_width = bounds[2], old_height = bounds[3];
        autoresize_axis(x, bounds[2], delta_width, mask & kFlexibleLeft, mask & kFlexibleWidth, mask & kFlexibleRight);
        autoresize_axis(y, bounds[3], delta_height, mask & kFlexibleTop, mask & kFlexibleHeight, mask & kFlexibleBottom);
        const double moved[4] = {x + bounds[2] / 2, y + bounds[3] / 2, 0, 0};
        property_set_rect(child, "bounds", bounds);
        property_set_rect(child, "center", moved);
        autoresize_children(child, bounds[2] - old_width, bounds[3] - old_height);
    }
}

void* make_color(double red, double green, double blue, double alpha) {
    void* color = objc_framework_object("UIColor");
    property_set_word(color, "colorComponentCount", 4);
    property_set_floats(color, "red", red, 0);
    property_set_floats(color, "green", green, 0);
    property_set_floats(color, "blue", blue, 0);
    property_set_floats(color, "alpha", alpha, 0);
    return color;
}

void* color_with_rgba(void*, const char*, double red, double green, double blue, double alpha) { return make_color(red, green, blue, alpha); }
void* color_with_white(void*, const char*, double white, double alpha) { return make_color(white, white, white, alpha); }
void* color_init_rgba(void* self, const char*, double red, double green, double blue, double alpha) {
    property_set_word(self, "colorComponentCount", 4);
    property_set_floats(self, "red", red, 0);
    property_set_floats(self, "green", green, 0);
    property_set_floats(self, "blue", blue, 0);
    property_set_floats(self, "alpha", alpha, 0);
    return self;
}

void* color_with_alpha(void* self, const char*, double alpha) {
    return make_color(property_get_float(self, "red"), property_get_float(self, "green"), property_get_float(self, "blue"), alpha);
}

struct NamedColor {
    double red, green, blue, alpha;
};

// iOS's light-appearance values for the system colors.
template <int red, int green, int blue, int alpha = 255>
void* system_color(void*, const char*) { return make_color(red / 255.0, green / 255.0, blue / 255.0, alpha / 255.0); }

}  // namespace

void uikit_autoresize_children(void* view, double delta_width, double delta_height) {
    if (delta_width != 0 || delta_height != 0) autoresize_children(view, delta_width, delta_height);
}

const void* uikit_color_method_for_selector(const std::string& selector) {
    static const std::unordered_map<std::string_view, const void*> table = {
        {"colorWithRed:green:blue:alpha:", reinterpret_cast<const void*>(color_with_rgba)},
        {"colorWithWhite:alpha:", reinterpret_cast<const void*>(color_with_white)},
        {"initWithRed:green:blue:alpha:", reinterpret_cast<const void*>(color_init_rgba)},
        {"colorWithAlphaComponent:", reinterpret_cast<const void*>(color_with_alpha)},
        {"whiteColor", reinterpret_cast<const void*>(system_color<255, 255, 255>)},
        {"blackColor", reinterpret_cast<const void*>(system_color<0, 0, 0>)},
        {"clearColor", reinterpret_cast<const void*>(system_color<0, 0, 0, 0>)},
        {"systemBackgroundColor", reinterpret_cast<const void*>(system_color<255, 255, 255>)},
        {"secondarySystemBackgroundColor", reinterpret_cast<const void*>(system_color<242, 242, 247>)},
        {"tertiarySystemBackgroundColor", reinterpret_cast<const void*>(system_color<255, 255, 255>)},
        {"systemGroupedBackgroundColor", reinterpret_cast<const void*>(system_color<242, 242, 247>)},
        {"secondarySystemGroupedBackgroundColor", reinterpret_cast<const void*>(system_color<255, 255, 255>)},
        {"labelColor", reinterpret_cast<const void*>(system_color<0, 0, 0>)},
        {"secondaryLabelColor", reinterpret_cast<const void*>(system_color<60, 60, 67, 153>)},
        {"tertiaryLabelColor", reinterpret_cast<const void*>(system_color<60, 60, 67, 76>)},
        {"quaternaryLabelColor", reinterpret_cast<const void*>(system_color<60, 60, 67, 46>)},
        {"separatorColor", reinterpret_cast<const void*>(system_color<60, 60, 67, 73>)},
        {"systemFillColor", reinterpret_cast<const void*>(system_color<120, 120, 128, 51>)},
        {"secondarySystemFillColor", reinterpret_cast<const void*>(system_color<120, 120, 128, 40>)},
        {"tertiarySystemFillColor", reinterpret_cast<const void*>(system_color<118, 118, 128, 31>)},
        {"quaternarySystemFillColor", reinterpret_cast<const void*>(system_color<116, 116, 128, 20>)},
        {"systemGrayColor", reinterpret_cast<const void*>(system_color<142, 142, 147>)},
        {"systemRedColor", reinterpret_cast<const void*>(system_color<255, 59, 48>)},
        {"systemGreenColor", reinterpret_cast<const void*>(system_color<52, 199, 89>)},
        {"systemBlueColor", reinterpret_cast<const void*>(system_color<0, 122, 255>)},
        {"systemOrangeColor", reinterpret_cast<const void*>(system_color<255, 149, 0>)},
        {"systemYellowColor", reinterpret_cast<const void*>(system_color<255, 204, 0>)},
        {"systemPurpleColor", reinterpret_cast<const void*>(system_color<175, 82, 222>)},
    };
    const auto found = table.find(selector);
    return found == table.end() ? nullptr : found->second;
}

Rect4 window_size_rect() {
    Json size;
    double width = kFallbackWidth, height = kFallbackHeight;
    if (json_parse(engine_host_query("screen-size", ""), size)) {
        const Json* w = size.member("w");
        const Json* h = size.member("h");
        if (w != nullptr && h != nullptr) {
            width = w->kind == Json::Kind::Integer ? static_cast<double>(w->integer) : w->number;
            height = h->kind == Json::Kind::Integer ? static_cast<double>(h->integer) : h->number;
        }
void* color_named(void*, const char*, void* name) {
    AssetColor color;
    if (!asset_color(nsstring_text(name), engine_host_query("dark-mode", "") == "true", color)) return nullptr;
    return make_color(color.red, color.green, color.blue, color.alpha);
}

void* color_named_in_bundle(void* self, const char* selector, void* name, void*, void*) { return color_named(self, selector, name); }

    }
    return {0, 0, width, height};
}

void uikit_render_if_changed() {
    void* window = key_window();
    if (window == nullptr) return;
    void* root = property_get_object(window, "rootViewController");
    void* controller = top_controller(root);
    if (controller == nullptr) return;
    void* view = property_get_object(controller, "view");
    if (view == nullptr) return;
    const Rect4 screen = window_size_rect();
    double previous[4];
    property_get_rect(view, "bounds", previous);
    if (previous[2] != screen.width || previous[3] != screen.height) autoresize_children(view, screen.width - previous[2], screen.height - previous[3]);
    const double bounds[4] = {0, 0, screen.width, screen.height};
        {"colorNamed:", reinterpret_cast<const void*>(color_named)},
        {"colorNamed:inBundle:compatibleWithTraitCollection:", reinterpret_cast<const void*>(color_named_in_bundle)},
    const double center[4] = {screen.width / 2, screen.height / 2, 0, 0};
    property_set_rect(view, "bounds", bounds);
    property_set_rect(view, "center", center);
    uikit_solve_layout(view, screen.width, screen.height, 0);
    update_status_bar(controller, view);
    g_actions.clear();
    std::vector<Json> nodes = {describe_view(view, 0, 0)};
    if (void* alert = presented_alert(root)) nodes.push_back(describe_alert(alert, root));
    const std::string tree = json_write(array_of(std::move(nodes)));
    if (tree == g_last_tree) return;
    g_last_tree = tree;
    engine_emit("tree", tree);
}

void uikit_run_action(int action) {
    if (action < 0 || static_cast<size_t>(action) >= g_actions.size()) return;
    // Copied, as an action may redraw and replace the list it lives in.
    const std::function<void()> run = g_actions[static_cast<size_t>(action)];
    run();
}

}  // namespace uikit

std::string uikit::color_hex_text(void* color) { return uikit::color_text(color); }
