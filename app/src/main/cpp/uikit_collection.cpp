#include <algorithm>
#include <cstdint>
#include <cstring>
#include <map>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "engine.h"
#include "foundation_internal.h"
#include "swift_abi.h"
#include "uikit_internal.h"

using foundation::new_nsarray;
using foundation::new_nsstring;
using foundation::NSArrayObject;
using foundation::nsstring_text;

namespace uikit {

namespace {

// The tree holds every cell because the host doesn't report scrolling; this bounds its size.
constexpr size_t kMaxCells = 200;
constexpr double kDefaultRowHeight = 44;
constexpr size_t kBlockInvokeWord = 2;

struct CGRect {
    double x, y, width, height;
};
struct CGSize {
    double width, height;
};

void* index_path(int64_t section, int64_t item) {
    static std::map<std::pair<int64_t, int64_t>, void*> interned;
    void*& path = interned[{section, item}];
    if (path == nullptr) {
        path = objc_framework_object("NSIndexPath");
        property_set_word(path, "section", section);
        property_set_word(path, "item", item);
    }
    return path;
}

int64_t path_section(void* path) { return property_get_word(path, "section"); }
int64_t path_item(void* path) { return property_get_word(path, "item"); }

void* index_path_for(void*, const char*, int64_t item, int64_t section) { return index_path(section, item); }
int64_t message_path_section(void* self, const char*) { return path_section(self); }
int64_t message_path_item(void* self, const char*) { return path_item(self); }
uint64_t index_path_length(void*, const char*) { return 2; }
int64_t message_path_at(void* self, const char*, uint64_t position) { return position == 0 ? path_section(self) : path_item(self); }
int64_t index_path_compare(void* self, const char*, void* other) {
    const std::pair mine{path_section(self), path_item(self)};
    const std::pair theirs{path_section(other), path_item(other)};
    if (mine == theirs) return 0;
    return mine < theirs ? -1 : 1;
}

SWIFTCC void swift_index_path_from_object(SWIFT_OUT void** out, void* path) { *out = object_retain(path); }
SWIFTCC void swift_index_path_make(SWIFT_OUT void** out, int64_t item, int64_t section) { *out = object_retain(index_path(section, item)); }
SWIFTCC void* swift_index_path_to_object(SWIFT_CONTEXT void** self) { return object_retain(*self); }
SWIFTCC int64_t swift_index_path_item(SWIFT_CONTEXT void** self) { return path_item(*self); }
SWIFTCC int64_t swift_index_path_section(SWIFT_CONTEXT void** self) { return path_section(*self); }
// Paths are interned, so one position is always one object.
SWIFTCC bool swift_index_path_equal(void** first, void** second) { return *first == *second; }

struct Cell {
    void* view;
    std::string reuse_identifier;
    int64_t section, item;
};

struct ListState {
    std::map<std::string, void*> cell_nibs;
    std::map<std::string, void*> cell_classes;
    std::map<std::string, std::vector<void*>> pool;
    std::vector<Cell> cells;
    std::vector<int64_t> counts;
    std::string dequeued_identifier;
    void* source = nullptr;
    bool loaded_prototypes = false;
    bool stale = true;
};

std::unordered_map<void*, ListState>& lists() {
    static std::unordered_map<void*, ListState> table;
    return table;
}

bool is_table(void* view) { return object_is_kind(view, "UITableView"); }

void load_prototypes(void* view, ListState& state) {
    if (state.loaded_prototypes) return;
    state.loaded_prototypes = true;
    const bool table = is_table(view);
    void* dictionary = property_get_object(view, table ? "tableViewCellPrototypeNibs" : "collectionViewCellNibDict");
    if (dictionary == nullptr) return;
    void* externals = property_get_object(view, table ? "tableViewCellPrototypeNibExternalObjects" : "collectionViewCellPrototypeNibExternalObjects");
    const auto no_externals = std::vector<void*>{};
    const auto& external_pairs = externals != nullptr ? *static_cast<NSArrayObject*>(externals)->items : no_externals;
    // A nib stores a dictionary as its keys and values in turn.
    const auto& pairs = *static_cast<NSArrayObject*>(dictionary)->items;
    for (size_t i = 0; i + 1 < pairs.size(); i += 2) {
        const std::string identifier = nsstring_text(pairs[i]);
        state.cell_nibs[identifier] = pairs[i + 1];
        for (size_t j = 0; j + 1 < external_pairs.size(); j += 2) {
            if (nsstring_text(external_pairs[j]) == identifier) property_set_object(pairs[i + 1], "externals", external_pairs[j + 1]);
        }
    }
}

void* make_from_class(void* cls, bool table, const std::string& identifier) {
    if (!objc_framework_class_name(cls).empty()) return objc_framework_object(objc_framework_class_name(cls).c_str());
    void* instance = objc_allocate(cls);
    if (table) {
        using Init = void* (*)(void*, const char*, int64_t, void*);
        return reinterpret_cast<Init>(const_cast<void*>(objc_method_for(instance, "initWithStyle:reuseIdentifier:")))(instance, "initWithStyle:reuseIdentifier:", 0, new_nsstring(identifier));
    }
    using Init = void* (*)(void*, const char*, CGRect);
    return reinterpret_cast<Init>(const_cast<void*>(objc_method_for(instance, "initWithFrame:")))(instance, "initWithFrame:", CGRect{0, 0, 0, 0});
}

void* dequeue(void* view, const std::string& identifier) {
    ListState& state = lists()[view];
    load_prototypes(view, state);
    state.dequeued_identifier = identifier;
    auto& pool = state.pool[identifier];
    if (!pool.empty()) {
        void* cell = pool.back();
        pool.pop_back();
        if (objc_responds(cell, "prepareForReuse")) objc_send(cell, "prepareForReuse", nullptr);
        return cell;
    }
    void* cell = nullptr;
    if (const auto nib = state.cell_nibs.find(identifier); nib != state.cell_nibs.end()) {
        for (void* top : nib_instantiate_object(nib->second, nullptr)) {
            if (object_is_kind(top, "UIView")) {
                cell = top;
                break;
            }
        }
    } else if (const auto cls = state.cell_classes.find(identifier); cls != state.cell_classes.end()) {
        cell = make_from_class(cls->second, is_table(view), identifier);
    }
    if (cell != nullptr) {
        object_retain(cell);
        property_set_object(cell, "reuseIdentifier", new_nsstring(identifier));
    } else {
        engine_log("UIKIT: nothing registered for cell \"%s\"", identifier.c_str());
    }
    return cell;
}

void register_cell_nib(void* self, const char*, void* nib, void* identifier) { lists()[self].cell_nibs[nsstring_text(identifier)] = object_retain(nib); }
void register_cell_class(void* self, const char*, void* cls, void* identifier) { lists()[self].cell_classes[nsstring_text(identifier)] = cls; }
void* dequeue_cell_for_path(void* self, const char*, void* identifier, void*) { return dequeue(self, nsstring_text(identifier)); }
void* dequeue_cell(void* self, const char*, void* identifier) { return dequeue(self, nsstring_text(identifier)); }

void invalidate(void* self) { lists()[self].stale = true; }
void invalidate_message(void* self, const char*) { invalidate(self); }
void perform_batch(void* self, const char*, void* updates, void* completion) {
    using Updates = void (*)(void*);
    using Completion = void (*)(void*, bool);
    if (updates != nullptr) reinterpret_cast<Updates>(static_cast<void**>(updates)[kBlockInvokeWord])(updates);
    invalidate(self);
    if (completion != nullptr) reinterpret_cast<Completion>(static_cast<void**>(completion)[kBlockInvokeWord])(completion, true);
}

void* cell_at(void* self, const char*, void* path) {
    for (const Cell& cell : lists()[self].cells) {
        if (cell.section == path_section(path) && cell.item == path_item(path)) return cell.view;
    }
    return nullptr;
}

void* path_of_cell(void* self, const char*, void* view) {
    for (const Cell& cell : lists()[self].cells) {
        if (cell.view == view) return index_path(cell.section, cell.item);
    }
    return nullptr;
}

void* visible_cells(void* self, const char*) {
    auto* array = static_cast<NSArrayObject*>(new_nsarray());
    for (const Cell& cell : lists()[self].cells) array->items->push_back(cell.view);
    return array;
}

void* visible_paths(void* self, const char*) {
    auto* array = static_cast<NSArrayObject*>(new_nsarray());
    for (const Cell& cell : lists()[self].cells) array->items->push_back(index_path(cell.section, cell.item));
    return array;
}

int64_t ask_section_count(void* view, void* source) {
    const char* selector = is_table(view) ? "numberOfSectionsInTableView:" : "numberOfSectionsInCollectionView:";
    if (!objc_responds(source, selector)) return 1;
    using Message = int64_t (*)(void*, const char*, void*);
    return reinterpret_cast<Message>(const_cast<void*>(objc_method_for(source, selector)))(source, selector, view);
}

int64_t ask_item_count(void* view, void* source, int64_t section) {
    const char* selector = is_table(view) ? "tableView:numberOfRowsInSection:" : "collectionView:numberOfItemsInSection:";
    if (!objc_responds(source, selector)) return 0;
    using Message = int64_t (*)(void*, const char*, void*, int64_t);
    return reinterpret_cast<Message>(const_cast<void*>(objc_method_for(source, selector)))(source, selector, view, section);
}

void* ask_cell(void* view, void* source, void* path) {
    const char* selector = is_table(view) ? "tableView:cellForRowAtIndexPath:" : "collectionView:cellForItemAtIndexPath:";
    if (!objc_responds(source, selector)) return nullptr;
    using Message = void* (*)(void*, const char*, void*, void*);
    return reinterpret_cast<Message>(const_cast<void*>(objc_method_for(source, selector)))(source, selector, view, path);
}

std::vector<int64_t> ask_counts(void* view, void* source) {
    std::vector<int64_t> counts;
    if (source == nullptr) return counts;
    const int64_t sections = ask_section_count(view, source);
    for (int64_t section = 0; section < sections; ++section) counts.push_back(ask_item_count(view, source, section));
    return counts;
}

void rebuild_cells(void* view, ListState& state, void* source, std::vector<int64_t> counts) {
    // Cells from a static table have no identifier and belong to their row for good.
    for (const Cell& cell : state.cells) {
        if (!cell.reuse_identifier.empty()) state.pool[cell.reuse_identifier].push_back(cell.view);
    }
    state.cells.clear();
    state.stale = false;
    state.source = source;
    state.counts = std::move(counts);
    if (source == nullptr) return;
    size_t total = 0;
    for (size_t section = 0; section < state.counts.size(); ++section) {
        for (int64_t item = 0; item < state.counts[section]; ++item) {
            if (total++ == kMaxCells) {
                engine_log("UIKIT: list %p drew only its first %zu cells", view, kMaxCells);
                return;
            }
            const auto section_index = static_cast<int64_t>(section);
            state.dequeued_identifier.clear();
            void* cell = ask_cell(view, source, index_path(section_index, item));
            if (cell != nullptr) state.cells.push_back({cell, state.dequeued_identifier, section_index, item});
        }
    }
}

// Cells size themselves in iOS; the size from their nib is the closest thing without measuring text.
double cell_design_height(void* cell) {
    double bounds[4];
    property_get_rect(cell, "bounds", bounds);
    return bounds[3] > 0 ? bounds[3] : kDefaultRowHeight;
}

double table_row_height(void* view, void* delegate, const Cell& cell) {
    constexpr const char* selector = "tableView:heightForRowAtIndexPath:";
    double height = 0;
    if (delegate != nullptr && objc_responds(delegate, selector)) {
        using Message = double (*)(void*, const char*, void*, void*);
        height = reinterpret_cast<Message>(const_cast<void*>(objc_method_for(delegate, selector)))(delegate, selector, view, index_path(cell.section, cell.item));
    }
    if (height <= 0) height = property_get_float(view, "rowHeight");
    return height > 0 ? height : cell_design_height(cell.view);
}

CGSize collection_item_size(void* view, void* layout, void* delegate, const Cell& cell, double width) {
    constexpr const char* selector = "collectionView:layout:sizeForItemAtIndexPath:";
    if (delegate != nullptr && objc_responds(delegate, selector)) {
        using Message = CGSize (*)(void*, const char*, void*, void*, void*);
        const CGSize size = reinterpret_cast<Message>(const_cast<void*>(objc_method_for(delegate, selector)))(delegate, selector, view, layout, index_path(cell.section, cell.item));
        if (size.width > 0 && size.height > 0) return {std::min(size.width, width), size.height};
    }
    double item[4] = {0, 0, 0, 0};
    if (layout != nullptr) property_get_rect(layout, "itemSize", item);
    // Only a flow layout has an item size; every other layout here is a list.
    if (item[0] > 0 && item[1] > 0) return {std::min(item[0], width), item[1]};
    return {width, cell_design_height(cell.view)};
}

std::function<void()> select_action(void* view, void* delegate, const Cell& cell) {
    const char* selector = is_table(view) ? "tableView:didSelectRowAtIndexPath:" : "collectionView:didSelectItemAtIndexPath:";
    if (delegate == nullptr || !objc_responds(delegate, selector)) return nullptr;
    void* path = index_path(cell.section, cell.item);
    return [view, delegate, selector, path] {
        using Message = void (*)(void*, const char*, void*, void*);
        reinterpret_cast<Message>(const_cast<void*>(objc_method_for(delegate, selector)))(delegate, selector, view, path);
    };
}

void* layout_of(void* view) {
    void* layout = property_get_object(view, "collectionViewLayout");
    return layout != nullptr ? layout : property_get_object(view, "collectionLayout");
}
void* collection_view_layout(void* self, const char*) { return layout_of(self); }

void* init_collection_view(void* self, const char*, CGRect frame, void* layout) {
    const double bounds[4] = {0, 0, frame.width, frame.height};
    const double center[4] = {frame.x + frame.width / 2, frame.y + frame.height / 2, 0, 0};
    property_set_rect(self, "bounds", bounds);
    property_set_rect(self, "center", center);
    property_set_object(self, "collectionViewLayout", layout);
    return self;
}

void* init_table_cell(void* self, const char*, int64_t, void* identifier) {
    if (identifier != nullptr) property_set_object(self, "reuseIdentifier", identifier);
    return self;
}

void* cell_content_view(void* self, const char*) {
    if (void* existing = property_get_object(self, "contentView")) return existing;
    void* content = objc_framework_object("UIView");
    property_set_object(self, "contentView", content);
    objc_send(self, "addSubview:", content);
    return content;
}

void add_target(void* self, const char*, void* target, const char* action, uint64_t events) { control_add_target(self, target, action, events); }

void* nib_named(void*, const char*, void* name, void*) {
    void* nib = objc_framework_object("UINib");
    property_set_object(nib, "nibPath", new_nsstring(engine_bundle_dir() + "/" + nsstring_text(name) + ".nib"));
    return nib;
}

void* nib_instantiate_message(void* self, const char*, void* owner, void*) {
    auto* array = static_cast<NSArrayObject*>(new_nsarray());
    for (void* object : nib_instantiate_object(self, owner)) array->items->push_back(object);
    return array;
}

using ActionHandler = SWIFTCC void (*)(void* action, SWIFT_CONTEXT void* context);

SWIFTCC void* action_init(SwiftString title, void* image, void*, uint64_t, uint64_t, uint64_t, uint64_t, ActionHandler handler, void* context) {
    void* action = object_retain(objc_framework_object("UIAction"));
    property_set_object(action, "title", new_nsstring(swift_string_to_utf8(title)));
    property_set_object(action, "image", image);
    property_set_word(action, "handlerFunction", reinterpret_cast<int64_t>(handler));
    property_set_word(action, "handlerContext", reinterpret_cast<int64_t>(context));
    return action;
}

SWIFTCC void* menu_init(SwiftString title, void* image, void*, uint64_t, void* children) {
    void* menu = object_retain(objc_framework_object("UIMenu"));
    property_set_object(menu, "title", new_nsstring(swift_string_to_utf8(title)));
    property_set_object(menu, "image", image);
    property_set_object(menu, "children", children);
    return menu;
}

SWIFTCC void* bar_item_system_init(int64_t, void* action, void* menu) {
    void* item = object_retain(objc_framework_object("UIBarButtonItem"));
    property_set_object(item, "primaryAction", action);
    property_set_object(item, "menu", menu);
    return item;
}

SWIFTCC void* bar_item_title_init(SwiftString title, void* image, void* action, void* menu) {
    void* item = bar_item_system_init(0, action, menu);
    if (title.object != 0) property_set_object(item, "title", new_nsstring(swift_string_to_utf8(title)));
    property_set_object(item, "image", image);
    return item;
}

// UITableViewController's own answers, which come from a static table's sections in its nib.
std::vector<void*> static_sections(void* controller) {
    void* source = property_get_object(controller, "staticDataSource");
    return source != nullptr ? object_array_items(property_get_object(source, "tableSections")) : std::vector<void*>{};
}

std::vector<void*> static_rows(void* controller, int64_t section) {
    const std::vector<void*> sections = static_sections(controller);
    if (section < 0 || static_cast<size_t>(section) >= sections.size()) return {};
    return object_array_items(property_get_object(sections[static_cast<size_t>(section)], "tableSectionRows"));
}

void* static_row(void* controller, void* path) {
    const std::vector<void*> rows = static_rows(controller, path_section(path));
    const auto item = static_cast<size_t>(path_item(path));
    return item < rows.size() ? rows[item] : nullptr;
}

int64_t controller_section_count(void* self, const char*, void*) {
    if (property_get_object(self, "staticDataSource") == nullptr) return 1;
    return static_cast<int64_t>(static_sections(self).size());
}
int64_t controller_row_count(void* self, const char*, void*, int64_t section) { return static_cast<int64_t>(static_rows(self, section).size()); }
void* controller_cell(void* self, const char*, void*, void* path) {
    void* row = static_row(self, path);
    return row != nullptr ? property_get_object(row, "tableRowCell") : nullptr;
}
double controller_row_height(void* self, const char*, void*, void* path) {
    void* row = static_row(self, path);
    return row != nullptr ? property_get_float(row, "tableRowHeight") : 0;
}
void* controller_header_title(void* self, const char*, void*, int64_t section) {
    const std::vector<void*> sections = static_sections(self);
    if (section < 0 || static_cast<size_t>(section) >= sections.size()) return nullptr;
    return property_get_object(sections[static_cast<size_t>(section)], "tableSectionHeaderTitle");
}

SWIFTCC void* button_init(int64_t, void* action) {
    void* button = object_retain(objc_framework_object("UIButton"));
    property_set_object(button, "primaryAction", action);
    return button;
}

void* list_configuration_box() { return objc_framework_object("UICollectionLayoutListConfiguration"); }
SWIFTCC void list_configuration_init(SWIFT_OUT void** out, uint64_t) { *out = list_configuration_box(); }
// The host draws its own lists, so a list's looks are accepted and not kept.
SWIFTCC void list_configuration_ignore(uint64_t, uint64_t, SWIFT_CONTEXT void**) {}
SWIFTCC void* compositional_list_layout(void*) { return object_retain(objc_framework_object("UICollectionViewCompositionalLayout")); }

}  // namespace

void* index_path_at(int64_t section, int64_t item) { return index_path(section, item); }
int64_t index_path_section(void* path) { return path_section(path); }
int64_t index_path_item(void* path) { return path_item(path); }

bool list_is_view(void* view) { return object_is_kind(view, "UICollectionView") || object_is_kind(view, "UITableView"); }

ListLayout list_layout(void* view, double width) {
    ListState& state = lists()[view];
    void* source = property_get_object(view, "dataSource");
    std::vector<int64_t> counts = ask_counts(view, source);
    if (state.stale || source != state.source || counts != state.counts) rebuild_cells(view, state, source, std::move(counts));

    void* delegate = property_get_object(view, "delegate");
    void* layout = layout_of(view);
    const bool table = is_table(view);
    const double line_spacing = layout != nullptr ? property_get_float(layout, "lineSpacing") : 0;
    const double item_spacing = layout != nullptr ? property_get_float(layout, "interitemSpacing") : 0;

    ListLayout result;
    double x = 0, y = 0, row_height = 0;
    for (const Cell& cell : state.cells) {
        CGSize size = table ? CGSize{width, table_row_height(view, delegate, cell)} : collection_item_size(view, layout, delegate, cell, width);
        if (x > 0 && x + size.width > width) {
            y += row_height + line_spacing;
            x = 0;
            row_height = 0;
        }
        const double bounds[4] = {0, 0, size.width, size.height};
        const double center[4] = {x + size.width / 2, y + size.height / 2, 0, 0};
        property_set_rect(cell.view, "bounds", bounds);
        property_set_rect(cell.view, "center", center);
        uikit_solve_layout(cell.view, size.width, size.height, 0);
        result.cells.push_back({cell.view, select_action(view, delegate, cell)});
        x += size.width + item_spacing;
        row_height = std::max(row_height, size.height);
    }
    result.content_height = y + row_height;
    return result;
}

const void* uikit_collection_method_for_selector(const std::string& selector) {
    static const std::unordered_map<std::string, const void*> table = {
        {"registerNib:forCellWithReuseIdentifier:", reinterpret_cast<const void*>(register_cell_nib)},
        {"registerNib:forCellReuseIdentifier:", reinterpret_cast<const void*>(register_cell_nib)},
        {"registerClass:forCellWithReuseIdentifier:", reinterpret_cast<const void*>(register_cell_class)},
        {"registerClass:forCellReuseIdentifier:", reinterpret_cast<const void*>(register_cell_class)},
        {"dequeueReusableCellWithReuseIdentifier:forIndexPath:", reinterpret_cast<const void*>(dequeue_cell_for_path)},
        {"dequeueReusableCellWithIdentifier:forIndexPath:", reinterpret_cast<const void*>(dequeue_cell_for_path)},
        {"dequeueReusableCellWithIdentifier:", reinterpret_cast<const void*>(dequeue_cell)},
        {"reloadData", reinterpret_cast<const void*>(invalidate_message)},
        {"insertItemsAtIndexPaths:", reinterpret_cast<const void*>(invalidate_message)},
        {"deleteItemsAtIndexPaths:", reinterpret_cast<const void*>(invalidate_message)},
        {"reloadItemsAtIndexPaths:", reinterpret_cast<const void*>(invalidate_message)},
        {"moveItemAtIndexPath:toIndexPath:", reinterpret_cast<const void*>(invalidate_message)},
        {"insertSections:", reinterpret_cast<const void*>(invalidate_message)},
        {"deleteSections:", reinterpret_cast<const void*>(invalidate_message)},
        {"reloadSections:", reinterpret_cast<const void*>(invalidate_message)},
        {"insertRowsAtIndexPaths:withRowAnimation:", reinterpret_cast<const void*>(invalidate_message)},
        {"deleteRowsAtIndexPaths:withRowAnimation:", reinterpret_cast<const void*>(invalidate_message)},
        {"reloadRowsAtIndexPaths:withRowAnimation:", reinterpret_cast<const void*>(invalidate_message)},
        {"insertSections:withRowAnimation:", reinterpret_cast<const void*>(invalidate_message)},
        {"deleteSections:withRowAnimation:", reinterpret_cast<const void*>(invalidate_message)},
        {"reloadSections:withRowAnimation:", reinterpret_cast<const void*>(invalidate_message)},
        {"endUpdates", reinterpret_cast<const void*>(invalidate_message)},
        {"performBatchUpdates:completion:", reinterpret_cast<const void*>(perform_batch)},
        {"cellForItemAtIndexPath:", reinterpret_cast<const void*>(cell_at)},
        {"cellForRowAtIndexPath:", reinterpret_cast<const void*>(cell_at)},
        {"indexPathForCell:", reinterpret_cast<const void*>(path_of_cell)},
        {"visibleCells", reinterpret_cast<const void*>(visible_cells)},
        {"indexPathsForVisibleItems", reinterpret_cast<const void*>(visible_paths)},
        {"indexPathsForVisibleRows", reinterpret_cast<const void*>(visible_paths)},
        {"collectionViewLayout", reinterpret_cast<const void*>(collection_view_layout)},
        {"initWithFrame:collectionViewLayout:", reinterpret_cast<const void*>(init_collection_view)},
        {"initWithStyle:reuseIdentifier:", reinterpret_cast<const void*>(init_table_cell)},
        {"contentView", reinterpret_cast<const void*>(cell_content_view)},
        {"addTarget:action:forControlEvents:", reinterpret_cast<const void*>(add_target)},
        {"nibWithNibName:bundle:", reinterpret_cast<const void*>(nib_named)},
        {"instantiateWithOwner:options:", reinterpret_cast<const void*>(nib_instantiate_message)},
    };
    const auto found = table.find(selector);
    return found == table.end() ? nullptr : found->second;
}

void run_primary_action(void* action) {
    const auto handler = reinterpret_cast<ActionHandler>(property_get_word(action, "handlerFunction"));
    if (handler != nullptr) handler(action, reinterpret_cast<void*>(property_get_word(action, "handlerContext")));
}

void register_collection_classes() {
    objc_register_method("-[UITableViewController numberOfSectionsInTableView:]", reinterpret_cast<const void*>(controller_section_count));
    objc_register_method("-[UITableViewController tableView:numberOfRowsInSection:]", reinterpret_cast<const void*>(controller_row_count));
    objc_register_method("-[UITableViewController tableView:cellForRowAtIndexPath:]", reinterpret_cast<const void*>(controller_cell));
    objc_register_method("-[UITableViewController tableView:heightForRowAtIndexPath:]", reinterpret_cast<const void*>(controller_row_height));
    objc_register_method("-[UITableViewController tableView:titleForHeaderInSection:]", reinterpret_cast<const void*>(controller_header_title));
    objc_register_method("+[NSIndexPath indexPathForItem:inSection:]", reinterpret_cast<const void*>(index_path_for));
    objc_register_method("+[NSIndexPath indexPathForRow:inSection:]", reinterpret_cast<const void*>(index_path_for));
    objc_register_method("-[NSIndexPath section]", reinterpret_cast<const void*>(message_path_section));
    objc_register_method("-[NSIndexPath item]", reinterpret_cast<const void*>(message_path_item));
    objc_register_method("-[NSIndexPath row]", reinterpret_cast<const void*>(message_path_item));
    objc_register_method("-[NSIndexPath length]", reinterpret_cast<const void*>(index_path_length));
    objc_register_method("-[NSIndexPath indexAtPosition:]", reinterpret_cast<const void*>(message_path_at));
    objc_register_method("-[NSIndexPath compare:]", reinterpret_cast<const void*>(index_path_compare));
}

void add_collection_symbols(SymbolMap& table) {
    table.insert({
        {"$s10Foundation9IndexPathV36_unconditionallyBridgeFromObjectiveCyACSo07NSIndexC0CSgFZ", reinterpret_cast<void*>(swift_index_path_from_object)},
        {"$s10Foundation9IndexPathV19_bridgeToObjectiveCSo07NSIndexC0CyF", reinterpret_cast<void*>(swift_index_path_to_object)},
        {"$s10Foundation9IndexPathV5UIKitE4item7sectionACSi_SitcfC", reinterpret_cast<void*>(swift_index_path_make)},
        {"$s10Foundation9IndexPathV5UIKitE3row7sectionACSi_SitcfC", reinterpret_cast<void*>(swift_index_path_make)},
        {"$s10Foundation9IndexPathV5UIKitE4itemSivg", reinterpret_cast<void*>(swift_index_path_item)},
        {"$s10Foundation9IndexPathV5UIKitE3rowSivg", reinterpret_cast<void*>(swift_index_path_item)},
        {"$s10Foundation9IndexPathV5UIKitE7sectionSivg", reinterpret_cast<void*>(swift_index_path_section)},
        {"$s10Foundation9IndexPathV2eeoiySbAC_ACtFZ", reinterpret_cast<void*>(swift_index_path_equal)},
        {"$sSo8UIActionC5UIKitE5title5image10identifier20discoverabilityTitle10attributes5state7handlerABSS_So7UIImageCSgSo0A10IdentifieraSgSSSgSo23UIMenuElementAttributesVSo0mN5StateVyABctcfC", reinterpret_cast<void*>(action_init)},
        {"$sSo6UIMenuC5UIKitE5title5image10identifier7options8childrenABSS_So7UIImageCSgSo0A10IdentifieraSgSo0A7OptionsVSaySo0A7ElementCGtcfC", reinterpret_cast<void*>(menu_init)},
        {"$sSo15UIBarButtonItemC5UIKitE06systemC013primaryAction4menuABSo0ab6SystemC0V_So8UIActionCSgSo6UIMenuCSgtcfC", reinterpret_cast<void*>(bar_item_system_init)},
        {"$sSo15UIBarButtonItemC5UIKitE5title5image13primaryAction4menuABSSSg_So7UIImageCSgSo8UIActionCSgSo6UIMenuCSgtcfC", reinterpret_cast<void*>(bar_item_title_init)},
        {"$sSo8UIButtonC5UIKitE4type13primaryActionABSo0A4TypeV_So8UIActionCSgtcfC", reinterpret_cast<void*>(button_init)},
        {"$s5UIKit35UICollectionLayoutListConfigurationV10appearanceA2C10AppearanceO_tcfC", reinterpret_cast<void*>(list_configuration_init)},
        {"$s5UIKit35UICollectionLayoutListConfigurationV15showsSeparatorsSbvs", reinterpret_cast<void*>(list_configuration_ignore)},
        {"$s5UIKit35UICollectionLayoutListConfigurationV15backgroundColorSo7UIColorCSgvs", reinterpret_cast<void*>(list_configuration_ignore)},
        {"$s5UIKit35UICollectionLayoutListConfigurationV10headerModeAC06HeaderG0Ovs", reinterpret_cast<void*>(list_configuration_ignore)},
        {"$s5UIKit35UICollectionLayoutListConfigurationV10footerModeAC06FooterG0Ovs", reinterpret_cast<void*>(list_configuration_ignore)},
        {"$s5UIKit35UICollectionLayoutListConfigurationV020trailingSwipeActionsE8ProviderSo07UISwipehE0CSg10Foundation9IndexPathVcSgvs", reinterpret_cast<void*>(list_configuration_ignore)},
        {"$sSo35UICollectionViewCompositionalLayoutC5UIKitE4list5usingAbC0aD17ListConfigurationV_tFZ", reinterpret_cast<void*>(compositional_list_layout)},
    });
}

}  // namespace uikit
