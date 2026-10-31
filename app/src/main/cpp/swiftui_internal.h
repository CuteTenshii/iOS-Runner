#pragma once
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iterator>
#include <map>
#include <new>
#include <set>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include "concurrency_shim.h"
#include "coredata_shim.h"
#include "engine.h"
#include "foundation_shim.h"
#include "json.h"
#include "key_path.h"
#include "localization.h"
#include "macho.h"
#include "objc_runtime.h"
#include "stdlib_shim.h"
#include "swift_abi.h"
#include "swift_runtime.h"
#include "swiftui_shim.h"
#include "symbols.h"
#include "type_system.h"

// Private to the swiftui shim sources.
namespace swiftui {

extern const MachSection* g_view_conformances;

void* rt_swift_getWitnessTable_for_view(const char* conformance, void* type);

constexpr int kMaximumDepth = 64;

// Runtime-owned object behind a framework value; the guest only sees it as one retained reference.
struct ViewBox {
    HeapObject header;
    const void* closure;
    void* closure_context;
    void* content_type;
    void** content_witnesses;
};

void destroy_box(HeapObject* object);

struct BoxMetadata {
    void (*destroyer)(HeapObject*);
    const void* witnesses;
    uint64_t kind;
};

extern const int32_t kStorageCaseValueTag;

extern const int32_t kTitleDisplayInline;

extern char g_view_body_requirement;

extern char g_view_body_type_requirement;

extern char g_toolbar_body_requirement;

extern char g_toolbar_body_type_requirement;

extern char g_modifier_body_requirement;

extern char g_modifier_body_type_requirement;

enum class BindingKind : uint32_t { State,
                                    Closures,
                                    ObjectKeyPath,
                                    BindingKeyPath,
                                    AppStorage };

// A two-way reference to a value, in whichever of SwiftUI's forms the app created it.
struct BindingNode {
    HeapObject header;
    BindingKind kind;
    void* source;  // The state box, parent binding or observed object, retained.
    void* key_path;
    void* parent_type;
    const void* getter;
    void* getter_context;
    const void* setter;
    void* setter_context;
};

extern "C" void* rt_swift_bridgeObjectRetain(void*);
BindingNode* new_binding(BindingKind kind);
void* generic_argument(void* metadata);

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
    TabView,
    Tab,
    NavigationStack,
    List,
    Button,
    Label,
    Toggle,
    Modifier,
    Sheet,
    Toolbar,
    OnChange,
    Task,
    Picker,
    ForEach,
    Section,
    ProgressView,
    Stepper,
    Image,
    AsyncImage,
    Disclosure,
    LabeledContent,
    Unavailable,
    Menu,
    Link,
    ToolbarItem,
    ImageResource,
    RowMenu,
    PresentedSheet,
    Alert,
    NavigationLink,
    TextField,
    Stack,
    Divider,
    Searchable,
};

// Runtime object behind a resilient view: its properties plus already-evaluated child values.
struct ViewNode {
    HeapObject header;
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
std::unordered_set<const void*>& live_nodes();
ViewNode* new_node(NodeKind kind);

// Runs a @ViewBuilder closure and keeps its result as a child of `node`.
void evaluate_child(ViewNode* node, const void* closure, void* context, void* type);

// Keeps a copy of `self` as the node's child, for modifiers whose result type is opaque.
void add_self_child(ViewNode* node, void* self, void* type);
void add_value_child(ViewNode* node, void* value_pointer, void* type);

// With a key path, ID follows from the key path and is not passed; here it is.
SWIFTCC void for_each_identifiable(SWIFT_OUT ViewNode** out, void* data, const void* row, void* row_context, void* data_type, void*, void* row_type);

extern const int32_t kImageResizingStretch;

// Constructors whose title is a key or string instead of a label view.
std::string key_text(SwiftString key, LocalizedBox* interpolation);

struct FetchedResultsBox {
    HeapObject header;
    std::vector<void*>* objects;
};

const std::vector<void*>& fetched_results_contents(void* value);

// FetchedResults' Collection witnesses: its Index is Int and Iterator is IndexingIterator, as in the framework.
struct IndexingIteratorValue {
    void* elements;
    int64_t position;
};

SWIFTCC void fetched_make_iterator(SWIFT_OUT IndexingIteratorValue* out, void*, void*, SWIFT_CONTEXT FetchedResultsBox** self);
SWIFTCC void fetched_start_index(SWIFT_OUT int64_t* out, void*, void*, SWIFT_CONTEXT void*);
SWIFTCC void fetched_end_index(SWIFT_OUT int64_t* out, void*, void*, SWIFT_CONTEXT FetchedResultsBox** self);
SWIFTCC int64_t fetched_count(void*, void*, SWIFT_CONTEXT FetchedResultsBox** self);
SWIFTCC bool fetched_is_empty(void*, void*, SWIFT_CONTEXT FetchedResultsBox** self);
SWIFTCC int64_t fetched_distance(const int64_t* from, const int64_t* to, void*, void*, SWIFT_CONTEXT void*);
SWIFTCC void fetched_index_offset(SWIFT_OUT int64_t* out, const int64_t* index, int64_t distance, void*, void*, SWIFT_CONTEXT void*);
SWIFTCC void fetched_form_index(int64_t* index, void*, void*, SWIFT_CONTEXT void*);
SWIFTCC void fetched_index_after(SWIFT_OUT int64_t* out, const int64_t* index, void*, void*, SWIFT_CONTEXT void*);
SWIFTCC void* fetched_copy_to_array(void*, void*, SWIFT_CONTEXT FetchedResultsBox** self);
SWIFTCC int64_t fetched_underestimated_count(void*, void*, SWIFT_CONTEXT FetchedResultsBox** self);

struct ReadYield {
    void (*resume)(void*, bool) SWIFTCC;
    void* value;
};

// A read accessor is a yield-once coroutine: the element is borrowed from the caller's frame buffer.
SWIFTCC ReadYield fetched_read(void** frame, const int64_t* index, void*, void*, SWIFT_CONTEXT FetchedResultsBox** self);

// Backing store of @State, attached to the inline LazyState through its location word.
struct StateBox {
    HeapObject header;
    void* type;
    void* value;
};

size_t lazy_state_location_offset(void* metadata);

// The first read fixes the state: a thunk runs once, and later copies of the view share the box.
StateBox* state_box_for(char* self, void* metadata);

// Framework constants are tagged immediates: the high bit makes retain and release ignore them.
constexpr uint64_t kFontBold = 1ULL << 16;

enum ConstantId : uint64_t {
    kFontBody = 1,
    kFontCaption,
    kFontCaption2,
    kFontFootnote,
    kColorAccent,
    kColorRed,
    kColorGreen,
    kColorSecondary,
    kStyleSecondary,
    kStyleQuaternary,
    kAnimationDefault,
    kButtonRoleDestructive,
    kToolbarTopBarTrailing,
    kAlignLeading,
    kAlignTrailing,
    kAlignCenter,
    kAlignBottom,
    kAlignTop,
    kFontLargeTitle,
    kFontTitle,
    kFontTitle2,
    kFontHeadline,
    kFontSubheadline,
    kColorPrimary,
    kColorGray,
    kColorPink,
    kColorBlack,
    kColorClear,
    kColorWhite,
    kColorOrange,
    kColorYellow,
    kStylePrimary,
    kWeightBold,
    kWeightSemibold,
    kStyleConstantOther,
};

extern std::atomic<bool> g_tree_dirty;

void binding_read(BindingNode* binding, void* out, void* type);
void binding_write(BindingNode* binding, void* value, void* type);

// State<T> is frozen: the value inline, then the location word this box hangs from.
size_t state_location_offset(void* type);
StateBox* plain_state_box(char* self, void* type);

extern std::string g_body_path;

// A tab's identified view is the tab itself, kept as one reference to its node.
SWIFTCC void tab_identified_view(SWIFT_OUT void** out, void*, void*, SWIFT_CONTEXT void** self);
std::set<std::string>& started_tasks();

// Text: words 0-1 hold the resolved string, and the last word packs font and colour as an immediate.
constexpr uint64_t kTextFontMask = 0x3ffff;
constexpr unsigned kTextColorShift = 24;
constexpr uint64_t kTextColorMask = 0xffULL << kTextColorShift;
constexpr const char* kToolbarContentSymbol = "_$s7SwiftUI14ToolbarContentMp";
constexpr const char* kViewModifierSymbol = "_$s7SwiftUI12ViewModifierMp";
constexpr const char* kRepresentableSymbol = "_$s7SwiftUI19UIViewRepresentableMp";

const void* relative_pointer(const char* field);

// Finds an app type's conformance to a framework protocol in the app's own conformance list.
const char* find_conformance(const void* type, const char* protocol_symbol);
extern "C" void* rt_call_make_ui_view(const void* function, void* context, void* self, void* type, void** table);
extern "C" void rt_call_modifier_body(const void* function, void* out, void* content, void* self, void* type, void** table);
const char* find_view_conformance(const void* type);

constexpr uint64_t kTupleKind = 0x301;
constexpr const char* kTextSymbol = "_$s7SwiftUI4TextVMn";
constexpr const char* kModifiedContentSymbol = "_$s7SwiftUI15ModifiedContentVMn";
constexpr const char* kTagModifierSymbol = "_$s7SwiftUI24_TagTraitWritingModifierVMn";
constexpr const char* kAppearanceSymbol = "_$s7SwiftUI25_AppearanceActionModifierVMn";
constexpr const char* kMakeUIViewRequirement = "_$s7SwiftUI19UIViewRepresentableP04makeC07context0C4TypeQzAA0cD7ContextVyxG_tFTq";
constexpr size_t kRepresentableContextBytes = 256;

extern BindingNode* g_picker_binding;

bool is_runtime_node_type(void* type);
void* bool_metadata();
void* int_metadata();
void dismiss_presented_sheet();

using SymbolTable = std::unordered_map<std::string_view, void*>;
void add_swiftui_render_symbols(SymbolTable& table);
void add_swiftui_views_symbols(SymbolTable& table);
void add_swiftui_state_symbols(SymbolTable& table);
void add_swiftui_modifiers_symbols(SymbolTable& table);
void add_swiftui_layout_symbols(SymbolTable& table);

}  // namespace swiftui
