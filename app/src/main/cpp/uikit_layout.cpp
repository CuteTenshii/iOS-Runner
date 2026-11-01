#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <functional>
#include <map>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "engine.h"
#include "foundation_internal.h"
#include "uikit_internal.h"

using foundation::new_nsarray;
using foundation::NSArrayObject;
using foundation::nsstring_text;

namespace uikit {

namespace {

enum Attribute : int64_t {
    kNone = 0,
    kLeft = 1,
    kRight = 2,
    kTop = 3,
    kBottom = 4,
    kLeading = 5,
    kTrailing = 6,
    kWidth = 7,
    kHeight = 8,
    kCenterX = 9,
    kCenterY = 10,
    kLastBaseline = 11,
    kFirstBaseline = 12,
    kLeftMargin = 13,
    kRightMargin = 14,
    kTopMargin = 15,
    kBottomMargin = 16,
    kLeadingMargin = 17,
    kTrailingMargin = 18,
    kCenterXMargin = 19,
    kCenterYMargin = 20
};

constexpr double kDefaultMargin = 8;
constexpr double kRequiredPriority = 1000;
constexpr int kSolverPasses = 24;
// Slightly over a typical average, since clipping a label looks worse than a little slack.
constexpr double kAverageGlyphWidth = 0.62;
constexpr double kLineHeight = 1.25;
constexpr double kDefaultFontSize = 17;
constexpr double kDefaultControlHeight = 34;
constexpr double kNoIntrinsicSize = -1;

struct Edges {
    double left = 0, right = 0, top = 0, bottom = 0;
    bool pin_left = false, pin_right = false, pin_top = false, pin_bottom = false;
    bool pin_width = false, pin_height = false;
};

std::vector<void*>& active_constraints() {
    static std::vector<void*> constraints;
    return constraints;
}

void activate(void* constraint) {
    if (constraint == nullptr) return;
    auto& all = active_constraints();
    if (std::find(all.begin(), all.end(), constraint) == all.end()) all.push_back(object_retain(constraint));
    property_set_word(constraint, "active", 1);
}

void deactivate(void* constraint) {
    auto& all = active_constraints();
    all.erase(std::remove(all.begin(), all.end(), constraint), all.end());
    property_set_word(constraint, "active", 0);
}

void activate_all(void*, const char*, void* constraints) {
    for (void* constraint : object_array_items(constraints)) activate(constraint);
}

void deactivate_all(void*, const char*, void* constraints) {
    for (void* constraint : object_array_items(constraints)) deactivate(constraint);
}

void set_active(void* self, const char*, bool value) {
    if (value)
        activate(self);
    else
        deactivate(self);
}

void* make_anchor(void* item, Attribute attribute) {
    void* anchor = objc_framework_object("NSLayoutAnchor");
    property_set_object(anchor, "item", item);
    property_set_word(anchor, "attribute", attribute);
    return anchor;
}

template <Attribute attribute>
void* anchor_getter(void* self, const char*) { return make_anchor(self, attribute); }

constexpr int64_t kRelationLess = -1, kRelationEqual = 0, kRelationGreater = 1;

void* make_constraint(void* first, int64_t first_attribute, int64_t relation, void* second, int64_t second_attribute, double multiplier, double constant) {
    void* constraint = objc_framework_object("NSLayoutConstraint");
    property_set_object(constraint, "firstItem", first);
    property_set_word(constraint, "firstAttribute", first_attribute);
    property_set_word(constraint, "relation", relation);
    property_set_object(constraint, "secondItem", second);
    property_set_word(constraint, "secondAttribute", second_attribute);
    property_set_floats(constraint, "multiplier", multiplier, 0);
    property_set_floats(constraint, "constant", constant, 0);
    property_set_floats(constraint, "priority", kRequiredPriority, 0);
    return constraint;
}

void* anchor_constraint(void* self, void* other, int64_t relation, double constant) {
    return make_constraint(property_get_object(self, "item"), property_get_word(self, "attribute"), relation, other != nullptr ? property_get_object(other, "item") : nullptr, other != nullptr ? property_get_word(other, "attribute") : kNone, 1, constant);
}

void* equal_anchor(void* self, const char*, void* other) { return anchor_constraint(self, other, kRelationEqual, 0); }
void* equal_anchor_constant(void* self, const char*, void* other, double constant) { return anchor_constraint(self, other, kRelationEqual, constant); }
void* greater_anchor(void* self, const char*, void* other) { return anchor_constraint(self, other, kRelationGreater, 0); }
void* greater_anchor_constant(void* self, const char*, void* other, double constant) { return anchor_constraint(self, other, kRelationGreater, constant); }
void* less_anchor(void* self, const char*, void* other) { return anchor_constraint(self, other, kRelationLess, 0); }
void* less_anchor_constant(void* self, const char*, void* other, double constant) { return anchor_constraint(self, other, kRelationLess, constant); }
void* equal_constant(void* self, const char*, double constant) { return anchor_constraint(self, nullptr, kRelationEqual, constant); }
void* greater_constant(void* self, const char*, double constant) { return anchor_constraint(self, nullptr, kRelationGreater, constant); }
void* less_constant(void* self, const char*, double constant) { return anchor_constraint(self, nullptr, kRelationLess, constant); }

void* equal_anchor_multiplier(void* self, const char*, void* other, double multiplier) {
    void* constraint = anchor_constraint(self, other, kRelationEqual, 0);
    property_set_floats(constraint, "multiplier", multiplier, 0);
    return constraint;
}

void* equal_anchor_multiplier_constant(void* self, const char*, void* other, double multiplier, double constant) {
    void* constraint = anchor_constraint(self, other, kRelationEqual, constant);
    property_set_floats(constraint, "multiplier", multiplier, 0);
    return constraint;
}

void* constraint_with_items(void*, const char*, void* first, int64_t first_attribute, int64_t relation, void* second, int64_t second_attribute, double multiplier, double constant) {
    return make_constraint(first, first_attribute, relation, second, second_attribute, multiplier, constant);
}

// Layout guides resolve to the view that owns them, inset by the system bars.
void* guide_owner(void* item) {
    if (item == nullptr || objc_framework_root_of(item) != "UILayoutGuide") return item;
    return property_get_object(item, "owningView");
}

void collect_views(void* view, std::vector<void*>& out) {
    out.push_back(view);
    auto* subviews = static_cast<NSArrayObject*>(property_get_object(view, "subviews"));
    if (subviews == nullptr) return;
    for (void* child : *subviews->items) collect_views(child, out);
}

std::string view_text(void* view) {
    void* text = property_get_object(view, "text");
    if (text != nullptr) return nsstring_text(text);
    void* attributed = property_get_object(view, "attributedText");
    void* inner = attributed != nullptr ? property_get_object(attributed, "string") : nullptr;
    return inner != nullptr ? nsstring_text(inner) : std::string();
}

double font_size_of(void* view) {
    void* font = property_get_object(view, "font");
    const double size = font != nullptr ? property_get_float(font, "fontPointSize") : 0;
    return size > 0 ? size : kDefaultFontSize;
}

// Estimated from glyph counts: asking the host per label would stall every layout pass.
void intrinsic_size(void* view, double available_width, double& width, double& height) {
    width = height = kNoIntrinsicSize;
    const std::string root = objc_framework_root_of(view);
    const std::string text = view_text(view);
    if (root == "UILabel" || root == "UIButton" || root == "UITextField") {
        const double size = font_size_of(view);
        const double natural = static_cast<double>(text.size()) * size * kAverageGlyphWidth;
        const int64_t lines = property_get_word(view, "numberOfLines");
        width = natural;
        double line_count = 1;
        if (lines != 1 && available_width > 0 && natural > available_width) {
            width = available_width;
            line_count = ceil(natural / available_width);
            if (lines > 0 && line_count > static_cast<double>(lines)) line_count = static_cast<double>(lines);
        }
        height = ceil(line_count * size * kLineHeight);
        if (root == "UIButton" && height < kDefaultControlHeight) height = kDefaultControlHeight;
    } else if (root == "UISwitch") {
        width = 51;
        height = 31;
    } else if (root == "UIActivityIndicatorView") {
        width = height = 20;
    }
}

double read_attribute(const Edges& edges, int64_t attribute, double margin) {
    switch (attribute) {
        case kLeft:
        case kLeading:
            return edges.left;
        case kRight:
        case kTrailing:
            return edges.right;
        case kTop:
        case kFirstBaseline:
            return edges.top;
        case kBottom:
        case kLastBaseline:
            return edges.bottom;
        case kWidth:
            return edges.right - edges.left;
        case kHeight:
            return edges.bottom - edges.top;
        case kCenterX:
        case kCenterXMargin:
            return (edges.left + edges.right) / 2;
        case kCenterY:
        case kCenterYMargin:
            return (edges.top + edges.bottom) / 2;
        case kLeftMargin:
        case kLeadingMargin:
            return edges.left + margin;
        case kRightMargin:
        case kTrailingMargin:
            return edges.right - margin;
        case kTopMargin:
            return edges.top + margin;
        case kBottomMargin:
            return edges.bottom - margin;
        default:
            return 0;
    }
}

void write_attribute(Edges& edges, int64_t attribute, double value) {
    switch (attribute) {
        case kLeft:
        case kLeading:
        case kLeftMargin:
        case kLeadingMargin:
            if (edges.pin_width && !edges.pin_right) edges.right = value + (edges.right - edges.left);
            edges.left = value;
            edges.pin_left = true;
            break;
        case kRight:
        case kTrailing:
        case kRightMargin:
        case kTrailingMargin:
            if (edges.pin_width && !edges.pin_left) edges.left = value - (edges.right - edges.left);
            edges.right = value;
            edges.pin_right = true;
            break;
        case kTop:
        case kFirstBaseline:
        case kTopMargin:
            if (edges.pin_height && !edges.pin_bottom) edges.bottom = value + (edges.bottom - edges.top);
            edges.top = value;
            edges.pin_top = true;
            break;
        case kBottom:
        case kLastBaseline:
        case kBottomMargin:
            if (edges.pin_height && !edges.pin_top) edges.top = value - (edges.bottom - edges.top);
            edges.bottom = value;
            edges.pin_bottom = true;
            break;
        case kWidth:
            if (edges.pin_right && !edges.pin_left)
                edges.left = edges.right - value;
            else
                edges.right = edges.left + value;
            edges.pin_width = true;
            break;
        case kHeight:
            if (edges.pin_bottom && !edges.pin_top)
                edges.top = edges.bottom - value;
            else
                edges.bottom = edges.top + value;
            edges.pin_height = true;
            break;
        case kCenterX:
        case kCenterXMargin: {
            const double half = (edges.right - edges.left) / 2;
            edges.left = value - half;
            edges.right = value + half;
            break;
        }
        case kCenterY:
        case kCenterYMargin: {
            const double half = (edges.bottom - edges.top) / 2;
            edges.top = value - half;
            edges.bottom = value + half;
            break;
        }
        default:
            break;
    }
}

double safe_top_inset() { return property_get_float(key_window(), "safeAreaTop"); }

void lay_out_stack(void* stack, std::unordered_map<void*, Edges>& frames) {
    auto* arranged = static_cast<NSArrayObject*>(property_get_object(stack, "stackViewArrangedSubviews"));
    if (arranged == nullptr) arranged = static_cast<NSArrayObject*>(property_get_object(stack, "arrangedSubviews"));
    if (arranged == nullptr) return;
    const auto found = frames.find(stack);
    if (found == frames.end()) return;
    const Edges& box = found->second;
    const bool vertical = (property_get_word(stack, "stackViewAxis") | property_get_word(stack, "axis")) == 1;
    const double spacing = property_get_float(stack, "stackViewSpacing") + property_get_float(stack, "spacing");
    double cursor = vertical ? box.top : box.left;
    const double cross = vertical ? box.right - box.left : box.bottom - box.top;
    for (void* child : *arranged->items) {
        const auto entry = frames.find(child);
        if (entry == frames.end() || property_get_word(child, "hidden") != 0) continue;
        Edges& edges = entry->second;
        double width, height;
        intrinsic_size(child, vertical ? cross : 0, width, height);
        if (vertical) {
            const double length = edges.pin_height || height < 0 ? edges.bottom - edges.top : height;
            edges.left = box.left;
            edges.right = box.right;
            edges.top = cursor;
            edges.bottom = cursor + length;
            cursor += length + spacing;
        } else {
            const double length = edges.pin_width || width < 0 ? edges.right - edges.left : width;
            edges.top = box.top;
            edges.bottom = box.bottom;
            edges.left = cursor;
            edges.right = cursor + length;
            cursor += length + spacing;
        }
    }
}

}  // namespace

void uikit_solve_layout(void* root, double width, double height, double top_inset) {
    property_set_floats(key_window(), "safeAreaTop", top_inset, 0);
    std::vector<void*> views;
    collect_views(root, views);
    std::unordered_map<void*, Edges> frames;
    std::unordered_set<void*> in_tree(views.begin(), views.end());

    // Start from where the nibs placed everything, in root coordinates.
    frames[root] = {0, width, 0, height, true, true, true, true, true, true};
    const std::function<void(void*, double, double)> place = [&](void* view, double origin_x, double origin_y) {
        auto* subviews = static_cast<NSArrayObject*>(property_get_object(view, "subviews"));
        if (subviews == nullptr) return;
        for (void* child : *subviews->items) {
            double bounds[4], center[4];
            property_get_rect(child, "bounds", bounds);
            property_get_rect(child, "center", center);
            Edges edges;
            edges.left = origin_x + center[0] - bounds[2] / 2;
            edges.right = edges.left + bounds[2];
            edges.top = origin_y + center[1] - bounds[3] / 2;
            edges.bottom = edges.top + bounds[3];
            frames[child] = edges;
            place(child, edges.left, edges.top);
        }
    };
    place(root, 0, 0);

    std::vector<void*> constraints = active_constraints();
    for (void* view : views) {
        auto* owned = static_cast<NSArrayObject*>(property_get_object(view, "viewAutolayoutConstraints"));
        if (owned == nullptr) continue;
        for (void* constraint : *owned->items) {
            if (std::find(constraints.begin(), constraints.end(), constraint) == constraints.end()) constraints.push_back(constraint);
        }
    }
    const auto priority_of = [](void* constraint) {
        const double priority = property_get_float(constraint, "priority");
        return priority > 0 ? priority : kRequiredPriority;
    };
    std::stable_sort(constraints.begin(), constraints.end(), [&](void* a, void* b) { return priority_of(a) < priority_of(b); });

    for (int pass = 0; pass < kSolverPasses; ++pass) {
        for (auto& [view, edges] : frames) {
            if (view == root) continue;
            edges.pin_left = edges.pin_right = edges.pin_top = edges.pin_bottom = edges.pin_width = edges.pin_height = false;
        }
        // Lowest priority first, so that the required constraints have the last word.
        for (void* constraint : constraints) {
            void* first = guide_owner(property_get_object(constraint, "firstItem"));
            void* second = guide_owner(property_get_object(constraint, "secondItem"));
            if (first == nullptr || first == root || in_tree.count(first) == 0) continue;
            if (second != nullptr && in_tree.count(second) == 0) continue;
            const int64_t first_attribute = property_get_word(constraint, "firstAttribute");
            const int64_t second_attribute = property_get_word(constraint, "secondAttribute");
            const double stored_multiplier = property_get_float(constraint, "multiplier");
            const double multiplier = stored_multiplier != 0 ? stored_multiplier : 1;
            double value = property_get_float(constraint, "constant");
            if (second != nullptr) {
                double attribute_value = read_attribute(frames[second], second_attribute, kDefaultMargin);
                void* raw_second = property_get_object(constraint, "secondItem");
                if (raw_second != second && (second_attribute == kTop || second_attribute == kTopMargin)) attribute_value += top_inset;
                value += multiplier * attribute_value;
            }
            Edges& edges = frames[first];
            const double current = read_attribute(edges, first_attribute, kDefaultMargin);
            const int64_t relation = property_get_word(constraint, "relation");
            if ((relation == kRelationGreater && current >= value) || (relation == kRelationLess && current <= value)) continue;
            write_attribute(edges, first_attribute, value);
        }
        for (void* view : views) {
            if (view == root) continue;
            Edges& edges = frames[view];
            double natural_width, natural_height;
            const bool edges_fix_width = edges.pin_left && edges.pin_right;
            intrinsic_size(view, edges_fix_width ? edges.right - edges.left : 0, natural_width, natural_height);
            if (natural_width >= 0 && !edges.pin_width && !edges_fix_width) {
                if (edges.pin_right && !edges.pin_left)
                    edges.left = edges.right - natural_width;
                else
                    edges.right = edges.left + natural_width;
            }
            if (natural_height >= 0 && !edges.pin_height && !(edges.pin_top && edges.pin_bottom)) {
                if (edges.pin_bottom && !edges.pin_top)
                    edges.top = edges.bottom - natural_height;
                else
                    edges.bottom = edges.top + natural_height;
            }
            if (objc_framework_root_of(view) == "UIStackView") lay_out_stack(view, frames);
        }
    }

    // Back to each view's own parent.
    const std::function<void(void*)> commit = [&](void* view) {
        auto* subviews = static_cast<NSArrayObject*>(property_get_object(view, "subviews"));
        if (subviews == nullptr) return;
        const Edges& parent = frames[view];
        for (void* child : *subviews->items) {
            const Edges& edges = frames[child];
            double bounds[4];
            property_get_rect(child, "bounds", bounds);
            bounds[2] = edges.right - edges.left;
            bounds[3] = edges.bottom - edges.top;
            property_set_rect(child, "bounds", bounds);
            const double center[4] = {(edges.left + edges.right) / 2 - parent.left, (edges.top + edges.bottom) / 2 - parent.top, 0, 0};
            property_set_rect(child, "center", center);
            commit(child);
        }
    };
    commit(root);
}

const void* uikit_layout_method_for_selector(const std::string& selector) {
    static const std::unordered_map<std::string_view, const void*> table = {
        {"activateConstraints:", reinterpret_cast<const void*>(activate_all)},
        {"deactivateConstraints:", reinterpret_cast<const void*>(deactivate_all)},
        {"setActive:", reinterpret_cast<const void*>(set_active)},
        {"leftAnchor", reinterpret_cast<const void*>(anchor_getter<kLeft>)},
        {"rightAnchor", reinterpret_cast<const void*>(anchor_getter<kRight>)},
        {"topAnchor", reinterpret_cast<const void*>(anchor_getter<kTop>)},
        {"bottomAnchor", reinterpret_cast<const void*>(anchor_getter<kBottom>)},
        {"leadingAnchor", reinterpret_cast<const void*>(anchor_getter<kLeading>)},
        {"trailingAnchor", reinterpret_cast<const void*>(anchor_getter<kTrailing>)},
        {"widthAnchor", reinterpret_cast<const void*>(anchor_getter<kWidth>)},
        {"heightAnchor", reinterpret_cast<const void*>(anchor_getter<kHeight>)},
        {"centerXAnchor", reinterpret_cast<const void*>(anchor_getter<kCenterX>)},
        {"centerYAnchor", reinterpret_cast<const void*>(anchor_getter<kCenterY>)},
        {"firstBaselineAnchor", reinterpret_cast<const void*>(anchor_getter<kFirstBaseline>)},
        {"lastBaselineAnchor", reinterpret_cast<const void*>(anchor_getter<kLastBaseline>)},
        {"constraintEqualToAnchor:", reinterpret_cast<const void*>(equal_anchor)},
        {"constraintEqualToAnchor:constant:", reinterpret_cast<const void*>(equal_anchor_constant)},
        {"constraintGreaterThanOrEqualToAnchor:", reinterpret_cast<const void*>(greater_anchor)},
        {"constraintGreaterThanOrEqualToAnchor:constant:", reinterpret_cast<const void*>(greater_anchor_constant)},
        {"constraintLessThanOrEqualToAnchor:", reinterpret_cast<const void*>(less_anchor)},
        {"constraintLessThanOrEqualToAnchor:constant:", reinterpret_cast<const void*>(less_anchor_constant)},
        {"constraintEqualToConstant:", reinterpret_cast<const void*>(equal_constant)},
        {"constraintGreaterThanOrEqualToConstant:", reinterpret_cast<const void*>(greater_constant)},
        {"constraintLessThanOrEqualToConstant:", reinterpret_cast<const void*>(less_constant)},
        {"constraintEqualToAnchor:multiplier:", reinterpret_cast<const void*>(equal_anchor_multiplier)},
        {"constraintEqualToAnchor:multiplier:constant:", reinterpret_cast<const void*>(equal_anchor_multiplier_constant)},
        {"constraintWithItem:attribute:relatedBy:toItem:attribute:multiplier:constant:", reinterpret_cast<const void*>(constraint_with_items)},
    };
    const auto found = table.find(selector);
    return found == table.end() ? nullptr : found->second;
}

}  // namespace uikit
