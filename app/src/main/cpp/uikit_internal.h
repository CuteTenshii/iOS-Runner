#pragma once
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "objc_runtime.h"

// Private to the uikit shim sources.
namespace uikit {

using SymbolMap = std::unordered_map<std::string_view, void*>;

void add_app_symbols(SymbolMap& table);
void add_constant_symbols(SymbolMap& table);
void register_app_classes();
void register_property_fallback();

void* application_object();

struct Rect4 {
    double x, y, width, height;
};

// A nib's objects, built and connected; returns its top-level objects other than proxies.
// `externals` resolves the proxies a storyboard's nib uses for objects in other nibs.
std::vector<void*> nib_instantiate(const std::string& path, void* owner, void* storyboard, void* externals = nullptr);
const void* nib_init_with_coder();
void register_nib_classes();
void register_view_classes();
void register_collection_classes();
void add_collection_symbols(SymbolMap& table);
// A UINib's contents, from the file it names or the archive a storyboard embedded in it.
std::vector<void*> nib_instantiate_object(void* nib, void* owner);

// Implementations shared by every UIKit class, found by selector.
const void* uikit_method_for_selector(const std::string& selector);

const void* uikit_layout_method_for_selector(const std::string& selector);
const void* uikit_collection_method_for_selector(const std::string& selector);
const void* uikit_color_method_for_selector(const std::string& selector);
// Sets the frame of every view under `root` from its constraints.
void uikit_solve_layout(void* root, double width, double height, double top_inset);

// The object references in an NSArray, or in a Swift array bridged to one.
std::vector<void*> object_array_items(void* array);

void finish_decoded_object(void* object, const std::string& class_name);
void object_set_value(void* object, const std::string& key, void* value);
void control_add_target(void* control, void* target, const std::string& action, uint64_t events);
bool object_is_kind(void* object, const char* framework_class);

std::string storyboard_directory(void* storyboard);
void* storyboard_create_initial(const std::string& name);
bool window_is_visible();
void* key_window();
Rect4 window_size_rect();
void uikit_render_if_changed();
void uikit_run_action(int action);
void uikit_dismiss_presented(void* controller);
void uikit_run_alert_action(void* action);
void uikit_autoresize_children(void* view, double delta_width, double delta_height);

void property_set_rect(void* object, const std::string& key, const double* values);
void property_get_rect(void* object, const std::string& key, double* out);
int64_t property_get_word(void* object, const std::string& key);

// Unknown UIKit accessors share this per-object store, so what a setter was given its getter returns.
void property_set_object(void* object, const std::string& key, void* value);
void* property_get_object(void* object, const std::string& key);
void property_forget(void* object);
// Takes a strong reference, for anything held in one of this layer's own lists.
void* object_retain(void* object);
std::string color_hex_text(void* color);
void show_contained_controller(void* controller);
void resolve_tab_placeholders(void* controller);

struct ListLayoutCell {
    void* view;
    std::function<void()> select;
};
struct ListLayout {
    std::vector<ListLayoutCell> cells;
    double content_height = 0;
};
void* index_path_at(int64_t section, int64_t item);
int64_t index_path_section(void* path);
int64_t index_path_item(void* path);
bool list_is_view(void* view);
void run_primary_action(void* action);
// Asks a collection or table view's data source for its cells and places them.
ListLayout list_layout(void* view, double width);

void property_set_word(void* object, const std::string& key, int64_t value);
void property_set_floats(void* object, const std::string& key, double first, double second);
double property_get_float(void* object, const std::string& key);
void property_get_floats(void* object, const std::string& key, double* out);

}  // namespace uikit
