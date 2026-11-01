#include "coredata_shim.h"

#include <dirent.h>
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "engine.h"
#include "foundation_shim.h"
#include "json.h"
#include "key_path.h"
#include "macho.h"
#include "objc_runtime.h"
#include "plist.h"
#include "predicate.h"
#include "stdlib_shim.h"
#include "swift_abi.h"
#include "type_system.h"
#include "uikit_internal.h"

extern "C" void* rt_swift_bridgeObjectRetain(void*);
extern "C" void rt_swift_bridgeObjectRelease(void*);

namespace {
constexpr size_t kRoNameOffset = 24;
constexpr size_t kRoBasePropertiesOffset = 64;

struct Attribute {
    void* value = nullptr;
    bool is_object = true;
};

struct ManagedRecord {
    std::string entity;
    std::map<std::string, Attribute> attributes;
};

struct Store {
    std::unordered_map<void*, ManagedRecord> records;
    std::vector<void*> insertion_order;
    bool has_changes = false;
};

Store& store() {
    static Store instance;
    return instance;
}

const char* class_read_only(const void* cls) {
    uintptr_t data;
    memcpy(&data, static_cast<const char*>(cls) + kClassDataOffset, sizeof data);
    return reinterpret_cast<const char*>(data & kClassDataMask);
}

// "_TtC7Feather8Imported" and "Imported" both name the entity "Imported".
std::string entity_name_of_class(const void* cls) {
    const char* name;
    memcpy(&name, class_read_only(cls) + kRoNameOffset, sizeof name);
    std::string text = name;
    if (text.rfind("_TtC", 0) != 0) return text;
    size_t at = 4;
    std::string last;
    while (at < text.size() && text[at] >= '0' && text[at] <= '9') {
        const size_t length = strtoul(text.c_str() + at, nullptr, 10);
        while (text[at] >= '0' && text[at] <= '9') ++at;
        last = text.substr(at, length);
        at += length;
    }
    return last;
}

// A compiled @NSManaged property's ObjC type encoding: "T@\"NSDate\",&,D,N" for objects, "TB,D,N" for BOOL.
bool property_is_object(const void* cls, const std::string& property) {
    while (cls != nullptr && objc_framework_class_name(cls).empty()) {
        const char* list;
        memcpy(&list, class_read_only(cls) + kRoBasePropertiesOffset, sizeof list);
        if (list != nullptr) {
            uint32_t entry_size, count;
            memcpy(&entry_size, list, sizeof entry_size);
            memcpy(&count, list + 4, sizeof count);
            for (uint32_t i = 0; i < count; ++i) {
                const char* entry = list + 8 + i * entry_size;
                const char* name;
                const char* encoding;
                memcpy(&name, entry, sizeof name);
                memcpy(&encoding, entry + sizeof name, sizeof encoding);
                if (property == name) return encoding[1] == '@';
            }
        }
        memcpy(&cls, static_cast<const char*>(cls) + kClassSuperclassOffset, sizeof cls);
    }
    return true;
}

void* managed_get(void* self, const char* selector) {
    const auto found = store().records.find(self);
    if (found == store().records.end()) return nullptr;
    const auto attribute = found->second.attributes.find(selector);
    return attribute == found->second.attributes.end() ? nullptr : attribute->second.value;
}

void managed_set(void* self, const char* selector, void* value) {
    // "setIconURL:" is the property "iconURL".
    std::string property(selector + 3, strlen(selector) - 4);
    property[0] = static_cast<char>(tolower(property[0]));
    Attribute& attribute = store().records[self].attributes[property];
    attribute.is_object = property_is_object(*static_cast<void**>(self), property);
    if (attribute.is_object) {
        if (value != nullptr) rt_swift_retain(value);
        if (attribute.value != nullptr) rt_swift_release(attribute.value);
    }
    attribute.value = value;
    store().has_changes = true;
}

const void* managed_accessor(const char* selector) {
    const size_t length = strlen(selector);
    if (length > 4 && strncmp(selector, "set", 3) == 0 && selector[length - 1] == ':') return reinterpret_cast<const void*>(managed_set);
    if (strchr(selector, ':') == nullptr) return reinterpret_cast<const void*>(managed_get);
    return nullptr;
}

void* insert_object(void* object, const std::string& entity) {
    ManagedRecord& record = store().records[object];
    record.entity = entity;
    store().insertion_order.push_back(object);
    store().has_changes = true;
    rt_swift_retain(object);
    return object;
}

void* managed_init_with_context(void* self, const char*, void*) { return insert_object(self, entity_name_of_class(*static_cast<void**>(self))); }
void* managed_init_with_entity(void* self, const char*, void*, void*) { return insert_object(self, entity_name_of_class(*static_cast<void**>(self))); }

struct EntityDescription {
    void* isa;
    uint64_t references;
    std::string* name;
};

// `+entity` reaches here through the metaclass chain, so `self` is the class itself.
void* managed_entity(void* cls, const char*) {
    auto* description = static_cast<EntityDescription*>(objc_framework_object("NSEntityDescription"));
    description->name = new std::string(entity_name_of_class(cls));
    return description;
}

std::string nsstring_of_description(void* description) { return *static_cast<EntityDescription*>(description)->name; }

struct SortDescriptor {
    void* isa;
    uint64_t references;
    void* key_path;
    bool ascending;
};

SWIFTCC void* sort_descriptor_init(void* key_path, bool ascending) {
    auto* descriptor = static_cast<SortDescriptor*>(objc_framework_object("NSSortDescriptor"));
    descriptor->key_path = key_path;
    descriptor->ascending = ascending;
    return descriptor;
}

// Apps subclass NSCompoundPredicate with their own instance layout, so what a predicate means is kept beside it.
std::unordered_map<const void*, PredicateRef>& predicate_nodes_by_object() {
    static std::unordered_map<const void*, PredicateRef> nodes;
    return nodes;
}

const PredicateRef& predicate_node(const void* predicate) {
    static const PredicateRef always_true = predicate_constant(true);
    const auto found = predicate_nodes_by_object().find(predicate);
    return found != predicate_nodes_by_object().end() ? found->second : always_true;
}

struct CVarArgExistential {
    uint64_t buffer[3];
    void* type;
    void* table;
};

void* new_predicate(PredicateRef node) {
    void* predicate = objc_framework_object("NSPredicate");
    predicate_nodes_by_object()[predicate] = std::move(node);
    return predicate;
}

PredicateValue predicate_value_from_json(const Json& json) {
    PredicateValue out;
    switch (json.kind) {
        case Json::Kind::Null:
            break;
        case Json::Kind::Bool:
            return PredicateValue::of_bool(json.flag);
        case Json::Kind::Integer:
            return PredicateValue::of_number(static_cast<double>(json.integer));
        case Json::Kind::Number:
        case Json::Kind::Date:
            return PredicateValue::of_number(json.number);
        case Json::Kind::String:
        case Json::Kind::Data:
            return PredicateValue::of_string(json.text);
        case Json::Kind::Array:
            out.kind = PredicateValue::Kind::List;
            for (const Json& item : json.items) out.items.push_back(predicate_value_from_json(item));
            break;
        case Json::Kind::Object:
            out.kind = PredicateValue::Kind::Dictionary;
            for (const auto& [key, item] : json.members) out.entries[key] = predicate_value_from_json(item);
            break;
    }
    return out;
}

// Anything that isn't plain data, such as a managed object, compares by identity.
PredicateValue predicate_value_from_object(void* object) {
    Json json;
    return json_from_object(object, json) ? predicate_value_from_json(json) : PredicateValue::of_object(object);
}

PredicateValue predicate_value_from_argument(const CVarArgExistential& argument) {
    constexpr uint64_t kLargestMetadataKind = 0x7ff;
    uint64_t kind;
    memcpy(&kind, argument.type, sizeof kind);
    if (kind == 0 || kind > kLargestMetadataKind) return predicate_value_from_object(reinterpret_cast<void*>(argument.buffer[0]));
    // A CVarArg existential starts with the same words as an Any.
    Json json;
    if (json_from_any(&argument, json)) return predicate_value_from_json(json);
    engine_log("STOP: an NSPredicate argument of a type this runtime cannot convert");
    abort();
}

SWIFTCC void* predicate_init(SwiftString format, void* arguments, SWIFT_CONTEXT void*) {
    const std::string text = swift_string_to_utf8(format);
    int64_t count;
    memcpy(&count, static_cast<char*>(arguments) + kArrayCountOffset, sizeof count);
    const auto* first = reinterpret_cast<const CVarArgExistential*>(static_cast<char*>(arguments) + kArrayElementsOffset);
    std::vector<PredicateValue> values;
    for (int64_t i = 0; i < count; ++i) values.push_back(predicate_value_from_argument(first[i]));
    std::string error;
    PredicateRef node = predicate_parse(text, values, error);
    if (node == nullptr) {
        engine_log("STOP: unsupported NSPredicate format \"%s\": %s", text.c_str(), error.c_str());
        abort();
    }
    return new_predicate(std::move(node));
}

PredicateValue read_predicate_key(void* object, const std::string& key) {
    const auto record = store().records.find(object);
    if (record != store().records.end()) {
        const auto found = record->second.attributes.find(key);
        if (found == record->second.attributes.end()) return PredicateValue{};
        if (found->second.is_object) return found->second.value != nullptr ? predicate_value_from_object(found->second.value) : PredicateValue{};
        return PredicateValue::of_number(static_cast<double>(reinterpret_cast<int64_t>(found->second.value)));
    }
    if (!objc_responds(object, key.c_str())) return PredicateValue{};
    using Getter = void* (*)(void*, const char*);
    void* result = reinterpret_cast<Getter>(const_cast<void*>(objc_method_for(object, key.c_str())))(object, key.c_str());
    if (property_is_object(*static_cast<void**>(object), key)) return predicate_value_from_object(result);
    return PredicateValue::of_number(static_cast<double>(reinterpret_cast<int64_t>(result)));
}

bool matches(void* object, void* predicate) {
    if (predicate == nullptr) return true;
    // A subclass may answer for itself, and framework predicates reach predicate_evaluate_object the same way.
    constexpr const char* kEvaluate = "evaluateWithObject:";
    using Evaluate = bool (*)(void*, const char*, void*);
    return reinterpret_cast<Evaluate>(const_cast<void*>(objc_method_for(predicate, kEvaluate)))(predicate, kEvaluate, object);
}

std::unordered_map<const void*, void*>& compound_subpredicates() {
    static std::unordered_map<const void*, void*> subpredicates;
    return subpredicates;
}

std::vector<PredicateRef> predicate_nodes(void* array) {
    std::vector<PredicateRef> nodes;
    for (void* item : uikit::object_array_items(array)) nodes.push_back(predicate_node(item));
    return nodes;
}

// NSCompoundPredicateType: not, and, or.
void* predicate_init_compound(void* self, const char*, uint64_t type, void* subpredicates) {
    constexpr uint64_t kNot = 0, kAnd = 1;
    std::vector<PredicateRef> nodes = predicate_nodes(subpredicates);
    if (type == kNot)
        predicate_nodes_by_object()[self] = nodes.empty() ? predicate_constant(false) : predicate_not(nodes[0]);
    else
        predicate_nodes_by_object()[self] = type == kAnd ? predicate_and(std::move(nodes)) : predicate_or(std::move(nodes));
    rt_swift_retain(subpredicates);
    compound_subpredicates()[self] = subpredicates;
    return self;
}

void* predicate_subpredicates(void* self, const char*) {
    const auto found = compound_subpredicates().find(self);
    return found != compound_subpredicates().end() ? found->second : nullptr;
}

void* predicate_and_of(void*, const char*, void* subpredicates) { return new_predicate(predicate_and(predicate_nodes(subpredicates))); }
void* predicate_or_of(void*, const char*, void* subpredicates) { return new_predicate(predicate_or(predicate_nodes(subpredicates))); }
void* predicate_not_of(void*, const char*, void* subpredicate) { return new_predicate(predicate_not(predicate_node(subpredicate))); }
void* predicate_with_value(void*, const char*, bool value) { return new_predicate(predicate_constant(value)); }

bool predicate_evaluate_object(void* self, const char*, void* object) {
    return predicate_evaluate(predicate_node(self), predicate_value_from_object(object), read_predicate_key);
}

// Attribute values compare by what they hold: only strings and dates are ever sorted on.
// The key path's getter returns a bridged value, but its pattern ends in the property's accessor: the sort reads through it.
void sort_key_value(void* object, void* key_path, char (&buffer)[64], void*& type) {
    type = key_path_value_type(key_path);
    memset(buffer, 0, sizeof buffer);
    key_path_get(key_path, &object, buffer, type);
}

int compare_by_key_path(void* left, void* right, void* key_path) {
    char left_value[64], right_value[64];
    void* type;
    sort_key_value(left, key_path, left_value, type);
    sort_key_value(right, key_path, right_value, type);
    TypeField payload;
    const bool left_some = optional_payload(type, left_value, payload) && payload.type != nullptr;
    const bool right_some = optional_payload(type, right_value, payload) && payload.type != nullptr;
    if (!left_some || !right_some) return left_some - right_some;
    const char* name = foreign_type_name(payload.type);
    if (name != nullptr && strcmp(name, "_$s10Foundation4DateVMn") == 0) {
        const double a = *reinterpret_cast<double*>(left_value);
        const double b = *reinterpret_cast<double*>(right_value);
        return (a > b) - (a < b);
    }
    if (payload.type == type_system_symbol("$sSSN")) {
        return swift_string_to_utf8(*reinterpret_cast<SwiftString*>(left_value)).compare(swift_string_to_utf8(*reinterpret_cast<SwiftString*>(right_value)));
    }
    return 0;
}

std::vector<void*> fetch_objects(const std::string& entity, void* predicate, void* sort_descriptors) {
    std::vector<void*> objects;
    for (void* object : store().insertion_order) {
        if (store().records[object].entity == entity && matches(object, predicate)) objects.push_back(object);
    }
    if (sort_descriptors == nullptr) return objects;
    int64_t count;
    memcpy(&count, static_cast<char*>(sort_descriptors) + kArrayCountOffset, sizeof count);
    auto* const* descriptors = reinterpret_cast<SortDescriptor* const*>(static_cast<char*>(sort_descriptors) + kArrayElementsOffset);
    std::stable_sort(objects.begin(), objects.end(), [&](void* left, void* right) {
        for (int64_t i = 0; i < count; ++i) {
            const int order = compare_by_key_path(left, right, descriptors[i]->key_path);
            if (order != 0) return descriptors[i]->ascending ? order < 0 : order > 0;
        }
        return false;
    });
    return objects;
}

std::map<std::string, void*>& entity_classes() {
    static std::map<std::string, void*> classes;
    return classes;
}

struct FetchRequest {
    void* isa;
    uint64_t references;
    std::string* entity;
    void* predicate;
    void* sort_descriptors;
};

void* fetch_request_init(void* self, const char*, void* name) {
    static_cast<FetchRequest*>(self)->entity = new std::string(foundation_object_text(name));
    return self;
}

// `+fetchRequest` reaches here through the metaclass chain, so `self` is the class itself.
void* managed_fetch_request(void* cls, const char*) {
    auto* request = static_cast<FetchRequest*>(objc_framework_object("NSFetchRequest"));
    request->entity = new std::string(entity_name_of_class(cls));
    return request;
}

void fetch_request_set_predicate(void* self, const char*, void* predicate) {
    auto* request = static_cast<FetchRequest*>(self);
    if (predicate != nullptr) rt_swift_retain(predicate);
    if (request->predicate != nullptr) rt_swift_release(request->predicate);
    request->predicate = predicate;
}

// The setter receives an NSArray bridged from a native array, and the sort needs the native array back.
void fetch_request_set_sort(void* self, const char*, void* descriptors) {
    auto* request = static_cast<FetchRequest*>(self);
    void* native = foundation_native_array(descriptors);
    if (native != nullptr) rt_swift_bridgeObjectRetain(native);
    if (request->sort_descriptors != nullptr) rt_swift_bridgeObjectRelease(request->sort_descriptors);
    request->sort_descriptors = native;
}

std::vector<void*> run_request(const FetchRequest* request) { return fetch_objects(*request->entity, request->predicate, request->sort_descriptors); }

SWIFTCC void* context_fetch(FetchRequest* request, void*, void*, SWIFT_CONTEXT void*, SWIFT_ERROR void**) {
    const std::vector<void*> objects = run_request(request);
    char* elements;
    void* storage = make_array_storage(type_system_symbol("$syXlN"), objects.size(), &elements);
    for (size_t i = 0; i < objects.size(); ++i) {
        rt_swift_retain(objects[i]);
        memcpy(elements + i * sizeof(void*), &objects[i], sizeof(void*));
    }
    return storage;
}

SWIFTCC int64_t context_count(FetchRequest* request, void*, void*, SWIFT_CONTEXT void*, SWIFT_ERROR void**) { return static_cast<int64_t>(run_request(request).size()); }

int64_t context_count_objc(void*, const char*, FetchRequest* request, void**) { return static_cast<int64_t>(run_request(request).size()); }

void remove_object(void* object) {
    auto& order = store().insertion_order;
    order.erase(std::remove(order.begin(), order.end(), object), order.end());
    store().records.erase(object);
    store().has_changes = true;
    rt_swift_release(object);
}

void context_delete(void*, const char*, void* object) { remove_object(object); }

void* batch_delete_init(void* self, const char*, void* request) {
    static_cast<FetchRequest*>(self)->entity = static_cast<FetchRequest*>(request)->entity;
    fetch_request_set_predicate(self, nullptr, static_cast<FetchRequest*>(request)->predicate);
    return self;
}

void* context_execute(void*, const char*, FetchRequest* request, void**) {
    for (void* object : run_request(request)) remove_object(object);
    return objc_framework_object("NSBatchDeleteResult");
}

using UniqueTuples = std::vector<std::vector<std::string>>;

const PlistValue* archived_object(const PlistValue& objects, const PlistValue* reference) {
    if (reference == nullptr || reference->kind != PlistValue::Kind::Uid) return reference;
    const auto index = static_cast<size_t>(reference->integer);
    return index < objects.items.size() ? &objects.items[index] : nullptr;
}

// Each entity's uniqueness constraints, from the NSKeyedArchiver plist that a compiled .mom is.
void read_model_constraints(const std::string& path, std::map<std::string, UniqueTuples>& out) {
    PlistValue archive;
    if (!plist_read_file(path, archive)) return;
    const PlistValue* objects = archive.get("$objects");
    if (objects == nullptr) return;
    const auto elements_of = [&](const PlistValue* collection) {
        std::vector<const PlistValue*> elements;
        const PlistValue* list = collection != nullptr ? collection->get("NS.objects") : nullptr;
        if (list == nullptr) return elements;
        for (const PlistValue& item : list->items) elements.push_back(archived_object(*objects, &item));
        return elements;
    };
    for (const PlistValue& object : objects->items) {
        const PlistValue* name = archived_object(*objects, object.get("NSEntityName"));
        const PlistValue* tuples = archived_object(*objects, object.get("NSUniqueTuples"));
        if (name == nullptr || name->kind != PlistValue::Kind::String || tuples == nullptr) continue;
        for (const PlistValue* tuple : elements_of(tuples)) {
            std::vector<std::string> attributes;
            for (const PlistValue* attribute : elements_of(tuple)) {
                if (attribute != nullptr && attribute->kind == PlistValue::Kind::String) attributes.push_back(attribute->text);
            }
            if (!attributes.empty()) out[name->text].push_back(std::move(attributes));
        }
    }
}

const std::map<std::string, UniqueTuples>& model_constraints() {
    static const std::map<std::string, UniqueTuples> constraints = [] {
        std::map<std::string, UniqueTuples> found;
        DIR* bundle = opendir(engine_bundle_dir().c_str());
        if (bundle == nullptr) return found;
        while (const dirent* entry = readdir(bundle)) {
            const std::string name = entry->d_name;
            constexpr std::string_view kExtension = ".momd";
            if (name.size() <= kExtension.size() || name.compare(name.size() - kExtension.size(), kExtension.size(), kExtension) != 0) continue;
            const std::string model = engine_bundle_dir() + "/" + name;
            PlistValue versions;
            if (!plist_read_file(model + "/VersionInfo.plist", versions)) continue;
            read_model_constraints(model + "/" + versions.string_at("NSManagedObjectModel_CurrentVersionName") + ".mom", found);
        }
        closedir(bundle);
        return found;
    }();
    return constraints;
}

// An attribute's identity for a constraint, or false if it's unset, which constraints don't apply to.
bool attribute_identity(const ManagedRecord& record, const std::string& name, std::string& out) {
    const auto found = record.attributes.find(name);
    if (found == record.attributes.end()) return false;
    if (!found->second.is_object) {
        out += std::to_string(reinterpret_cast<int64_t>(found->second.value));
    } else {
        if (found->second.value == nullptr) return false;
        Json json;
        if (!json_from_object(found->second.value, json)) return false;
        out += json_write(json);
    }
    out += '\0';
    return true;
}

// What the model's constraints and the app's merge policies come to: of records with the same identity, the newest stays.
void merge_constraint_duplicates() {
    std::map<void*, void*> replaced;
    for (const auto& [entity, tuples] : model_constraints()) {
        for (const auto& tuple : tuples) {
            std::map<std::string, void*> newest;
            const std::vector<void*> order = store().insertion_order;
            for (void* object : order) {
                const auto record = store().records.find(object);
                if (record == store().records.end() || record->second.entity != entity) continue;
                std::string identity;
                const bool complete = std::all_of(tuple.begin(), tuple.end(), [&](const std::string& name) { return attribute_identity(record->second, name, identity); });
                if (!complete) continue;
                void*& slot = newest[identity];
                if (slot != nullptr) replaced[slot] = object;
                slot = object;
            }
        }
    }
    if (replaced.empty()) return;
    const auto final_object = [&](void* object) {
        for (auto next = replaced.find(object); next != replaced.end(); next = replaced.find(object)) object = next->second;
        return object;
    };
    for (auto& [object, record] : store().records) {
        for (auto& [name, attribute] : record.attributes) {
            if (!attribute.is_object || replaced.count(attribute.value) == 0) continue;
            void* kept = final_object(attribute.value);
            rt_swift_retain(kept);
            rt_swift_release(attribute.value);
            attribute.value = kept;
        }
    }
    engine_log("COREDATA: merged %zu records under the model's uniqueness constraints", replaced.size());
    for (const auto& [object, kept] : replaced) remove_object(object);
}

// Saved as JSON beside the app's other support files; relationships are indexes into the saved list.
std::string store_path() { return engine_bundle_dir() + "/.Library/Application Support/coredata.json"; }

void save_store() {
    merge_constraint_duplicates();
    const auto& order = store().insertion_order;
    std::unordered_map<void*, size_t> indexes;
    for (size_t i = 0; i < order.size(); ++i) indexes[order[i]] = i;
    Json root;
    root.kind = Json::Kind::Array;
    for (void* object : order) {
        const ManagedRecord& record = store().records[object];
        Json entry;
        entry.kind = Json::Kind::Object;
        entry.members.emplace_back("#entity", json_text(record.entity));
        for (const auto& [name, attribute] : record.attributes) {
            if (attribute.value == nullptr && attribute.is_object) continue;
            std::string encoded;
            if (!attribute.is_object)
                encoded = attribute.value != nullptr ? "b1" : "b0";
            else if (const auto related = indexes.find(attribute.value); related != indexes.end())
                encoded = "r" + std::to_string(related->second);
            else
                encoded = foundation_encode_object(attribute.value);
            if (!encoded.empty()) entry.members.emplace_back(name, json_text(encoded));
        }
        root.items.push_back(std::move(entry));
    }
    const std::string path = store_path();
    FILE* file = fopen((path + ".tmp").c_str(), "w");
    if (file == nullptr) {
        engine_log("COREDATA: cannot write %s", path.c_str());
        return;
    }
    const std::string text = json_write(root);
    fwrite(text.data(), 1, text.size(), file);
    fclose(file);
    rename((path + ".tmp").c_str(), path.c_str());
}

void load_store() {
    FILE* file = fopen(store_path().c_str(), "r");
    if (file == nullptr) return;
    std::string text;
    char chunk[4096];
    for (size_t got; (got = fread(chunk, 1, sizeof chunk, file)) > 0;) text.append(chunk, got);
    fclose(file);
    Json root;
    if (!json_parse(text, root)) {
        engine_log("COREDATA: saved store is unreadable, starting empty");
        return;
    }
    using Allocate = void* (*)(void*, void*);
    auto allocate = reinterpret_cast<Allocate>(objc_runtime_symbol("objc_allocWithZone"));
    std::vector<void*> restored;
    for (const Json& entry : root.items) {
        const Json* entity = entry.member("#entity");
        const auto cls = entity == nullptr ? entity_classes().end() : entity_classes().find(entity->text);
        if (cls == entity_classes().end()) {
            restored.push_back(nullptr);
            continue;
        }
        void* object = allocate(cls->second, nullptr);
        ManagedRecord& record = store().records[object];
        record.entity = entity->text;
        store().insertion_order.push_back(object);
        restored.push_back(object);
    }
    for (size_t i = 0; i < root.items.size(); ++i) {
        if (restored[i] == nullptr) continue;
        ManagedRecord& record = store().records[restored[i]];
        for (const auto& [name, value] : root.items[i].members) {
            if (name == "#entity" || value.text.empty()) continue;
            Attribute& attribute = record.attributes[name];
            if (value.text[0] == 'b') {
                attribute.is_object = false;
                attribute.value = value.text == "b1" ? reinterpret_cast<void*>(static_cast<uintptr_t>(1)) : nullptr;
            } else if (value.text[0] == 'r') {
                const size_t index = strtoul(value.text.c_str() + 1, nullptr, 10);
                attribute.value = index < restored.size() && restored[index] != nullptr ? rt_swift_retain(restored[index]) : nullptr;
            } else {
                attribute.value = foundation_decode_object(value.text);
            }
        }
    }
    store().has_changes = false;
    merge_constraint_duplicates();
}

void* view_context() {
    static void* context = objc_framework_object("NSManagedObjectContext");
    return context;
}

void* container_init_with_name(void* self, const char*, void*) { return self; }
void* container_view_context(void*, const char*) { return view_context(); }
void container_load_stores(void*, const char*, void* block) {
    struct Literal {
        void* isa;
        int32_t flags, reserved;
        void (*invoke)(void*, void*, void*);
    };
    load_store();
    void* description = objc_framework_object("NSPersistentStoreDescription");
    static_cast<Literal*>(block)->invoke(block, description, nullptr);
}
void context_ignore_argument(void*, const char*, void*) {}
void context_set_bool(void*, const char*, bool) {}
bool context_has_changes(void*, const char*) { return store().has_changes; }
bool context_save(void*, const char*, void**) {
    save_store();
    store().has_changes = false;
    return true;
}

}  // namespace

void coredata_attach_image(const MachImage& image) {
    const MachSection* list = image.section("__DATA_CONST", "__objc_classlist");
    if (list == nullptr) list = image.section("__DATA", "__objc_classlist");
    if (list == nullptr) return;
    for (size_t offset = 0; offset + sizeof(void*) <= list->size; offset += sizeof(void*)) {
        void* cls;
        memcpy(&cls, reinterpret_cast<const char*>(list->address + offset), sizeof cls);
        entity_classes()[entity_name_of_class(cls)] = cls;
    }
}

void* coredata_view_context() { return view_context(); }

std::vector<void*> coredata_fetch(const std::string& entity, void* predicate, void* sort_descriptors) {
    return fetch_objects(entity, predicate, sort_descriptors);
}

namespace {

constexpr size_t kBlockInvokeWord = 2;

void invoke_plain_block(void* block) {
    using Invoke = void (*)(void*);
    reinterpret_cast<Invoke>(static_cast<void**>(block)[kBlockInvokeWord])(block);
}

void* return_self_argument(void* self, const char*, void*) { return self; }
void* return_self_two_arguments(void* self, const char*, void*, void*) { return self; }
void* return_self_word(void* self, const char*, uint64_t) { return self; }
void ignore_word(void*, const char*, uint64_t) {}
void ignore_argument(void*, const char*, void*) {}
void ignore_nothing(void*, const char*) {}

// Every context shares the one store, and all the guest's work is on one thread, so blocks just run.
void context_perform(void*, const char*, void* block) { invoke_plain_block(block); }
// The "new" prefix means the caller owns the result, even though every context is the same one here.
void* container_background_context(void*, const char*) { return rt_swift_retain(view_context()); }

void container_background_task(void*, const char*, void* block) {
    using Invoke = void (*)(void*, void*);
    reinterpret_cast<Invoke>(static_cast<void**>(block)[kBlockInvokeWord])(block, view_context());
}

void* context_object_with_id(void*, const char*, void* identifier) { return identifier; }
void* managed_object_id(void* self, const char*) { return self; }
void* managed_context(void*, const char*) { return view_context(); }
bool managed_is_deleted(void* self, const char*) { return store().records.count(self) == 0; }
bool managed_false(void*, const char*) { return false; }

using PerformBody = SWIFTCC void (*)(SWIFT_OUT void*, SWIFT_CONTEXT void*, SWIFT_ERROR void**);
using ContextBody = SWIFTCC void (*)(SWIFT_OUT void*, void*, SWIFT_CONTEXT void*, SWIFT_ERROR void**);

// performAndWait: the body runs right here, and whatever it throws passes through.
SWIFTCC void context_perform_and_wait(SWIFT_OUT void* out, PerformBody body, void* body_context, void*, SWIFT_CONTEXT void*, SWIFT_ERROR void** error) { body(out, body_context, error); }

extern "C" void* rt_coredata_perform_body(void* out, PerformBody body, void* body_context) {
    void* error = nullptr;
    body(out, body_context, &error);
    return error;
}

extern "C" void* rt_coredata_background_body(void* out, ContextBody body, void* body_context) {
    void* error = nullptr;
    body(out, view_context(), body_context, &error);
    return error;
}

extern "C" void rt_coredata_perform();
extern "C" void rt_coredata_background();

struct AsyncRecord {
    int32_t function;
    uint32_t context_size;
};

AsyncRecord g_perform_record;
AsyncRecord g_background_record;

// Nothing tracks unsaved changes per context, so these report none and readers fall back to fetching.
void* context_empty_set(void*, const char*) { return foundation_new_array({}); }

// A fetched results controller: one section holding the request's results, refetched on demand.
constexpr size_t kControllerRequestWord = 2, kControllerDelegateWord = 3;

std::unordered_map<void*, std::vector<void*>>& controller_results() {
    static std::unordered_map<void*, std::vector<void*>> results;
    return results;
}

void* results_controller_init(void* self, const char*, void* request, void*, void*, void*) {
    static_cast<void**>(self)[kControllerRequestWord] = rt_swift_retain(request);
    return self;
}

bool results_controller_fetch(void* self, const char*, void**) {
    controller_results()[self] = run_request(static_cast<FetchRequest*>(static_cast<void**>(self)[kControllerRequestWord]));
    return true;
}

void* results_controller_objects(void* self, const char*) { return foundation_new_array(controller_results()[self]); }
void* results_controller_request(void* self, const char*) { return static_cast<void**>(self)[kControllerRequestWord]; }
void results_controller_set_delegate(void* self, const char*, void* delegate) { static_cast<void**>(self)[kControllerDelegateWord] = delegate; }
void* results_controller_delegate(void* self, const char*) { return static_cast<void**>(self)[kControllerDelegateWord]; }

void* results_controller_sections(void* self, const char*) {
    static_cast<void>(self);
    auto** section = static_cast<void**>(objc_framework_object("_NSDefaultSectionInfo"));
    section[kControllerRequestWord] = self;
    return foundation_new_array({section});
}

int64_t section_count(void* self, const char*) { return static_cast<int64_t>(controller_results()[static_cast<void**>(self)[kControllerRequestWord]].size()); }
void* section_objects(void* self, const char*) { return foundation_new_array(controller_results()[static_cast<void**>(self)[kControllerRequestWord]]); }
void* section_name(void*, const char*) { return nullptr; }

void* merged_model(void*, const char*, void*) { return objc_framework_object("NSManagedObjectModel"); }

void* managed_value_for_key(void* self, const char*, void* key) { return managed_get(self, foundation_object_text(key).c_str()); }

void managed_set_value(void* self, const char*, void* value, void* key) {
    std::string property = foundation_object_text(key);
    std::string selector = "set" + property + ":";
    selector[3] = static_cast<char>(toupper(static_cast<unsigned char>(selector[3])));
    managed_set(self, selector.c_str(), value);
}

}  // namespace

// The async forms run their bodies at once, since all contexts share this thread, then return as async callees do.
__asm__(
    ".text\n"
    ".globl rt_coredata_perform\n"
    "rt_coredata_perform:\n"
    "  stp x29, x30, [sp, #-16]!\n"
    "  mov x29, sp\n"
    "  mov x1, x2\n"
    "  mov x2, x3\n"
    "  bl rt_coredata_perform_body\n"
    "  mov x20, x0\n"
    "  ldp x29, x30, [sp], #16\n"
    "  ldr x16, [x22, #8]\n"
    "  br x16\n"
    ".globl rt_coredata_background\n"
    "rt_coredata_background:\n"
    "  stp x29, x30, [sp, #-16]!\n"
    "  mov x29, sp\n"
    "  bl rt_coredata_background_body\n"
    "  mov x20, x0\n"
    "  ldp x29, x30, [sp], #16\n"
    "  ldr x16, [x22, #8]\n"
    "  br x16\n");

std::string coredata_entity_name(void* description) { return nsstring_of_description(description); }

void* coredata_symbol(std::string_view name) {
    constexpr std::string_view kPerform = "$sSo22NSManagedObjectContextC8CoreDataE7perform8schedule_xAbCE17ScheduledTaskTypeO_xyKctYaKlF";
    constexpr std::string_view kBackground = "$sSo21NSPersistentContainerC8CoreDataE21performBackgroundTaskyxxSo22NSManagedObjectContextCKcYaKlF";
    if (name == kPerform) return reinterpret_cast<void*>(rt_coredata_perform);
    if (name == kBackground) return reinterpret_cast<void*>(rt_coredata_background);
    if (name.size() > 2 && name.substr(name.size() - 2) == "Tu" && (name.substr(0, name.size() - 2) == kPerform || name.substr(0, name.size() - 2) == kBackground)) {
        const bool perform = name.substr(0, name.size() - 2) == kPerform;
        AsyncRecord& record = perform ? g_perform_record : g_background_record;
        const auto entry = reinterpret_cast<const char*>(perform ? rt_coredata_perform : rt_coredata_background);
        record.function = static_cast<int32_t>(entry - reinterpret_cast<const char*>(&record.function));
        record.context_size = sizeof(void*) * 2;
        return &record;
    }
    if (name == "$sSo22NSManagedObjectContextC8CoreDataE14performAndWaityxxyKXEKlF") return reinterpret_cast<void*>(context_perform_and_wait);
    static void* merge_policy = objc_framework_object("NSMergePolicy");
    if (name == "NSMergeByPropertyObjectTrumpMergePolicy" || name == "NSOverwriteMergePolicy") return &merge_policy;
    if (name == "$sSo16NSSortDescriptorC10FoundationE7keyPath9ascendingABs03KeyE0Cyxq_G_Sbtcr0_lufC") return reinterpret_cast<void*>(sort_descriptor_init);
    if (name == "$sSo11NSPredicateC10FoundationE6format_ABSSh_s7CVarArg_pdtcfC") return reinterpret_cast<void*>(predicate_init);
    if (name == "$sSo22NSManagedObjectContextC8CoreDataE5fetchySayxGSo14NSFetchRequestCyxGKSo0gH6ResultRzlF") return reinterpret_cast<void*>(context_fetch);
    if (name == "$sSo22NSManagedObjectContextC8CoreDataE5count3forSiSo14NSFetchRequestCyxG_tKSo0hI6ResultRzlF") return reinterpret_cast<void*>(context_count);
    return nullptr;
}

void coredata_register_classes() {
    for (const char* selector : {"-[NSManagedObjectContext registeredObjects]", "-[NSManagedObjectContext insertedObjects]", "-[NSManagedObjectContext updatedObjects]", "-[NSManagedObjectContext deletedObjects]"}) objc_register_method(selector, reinterpret_cast<const void*>(context_empty_set));
    objc_register_method("-[NSFetchedResultsController initWithFetchRequest:managedObjectContext:sectionNameKeyPath:cacheName:]", reinterpret_cast<const void*>(results_controller_init));
    objc_register_method("-[NSFetchedResultsController performFetch:]", reinterpret_cast<const void*>(results_controller_fetch));
    objc_register_method("-[NSFetchedResultsController fetchedObjects]", reinterpret_cast<const void*>(results_controller_objects));
    objc_register_method("-[NSFetchedResultsController fetchRequest]", reinterpret_cast<const void*>(results_controller_request));
    objc_register_method("-[NSFetchedResultsController sections]", reinterpret_cast<const void*>(results_controller_sections));
    objc_register_method("-[NSFetchedResultsController setDelegate:]", reinterpret_cast<const void*>(results_controller_set_delegate));
    objc_register_method("-[NSFetchedResultsController delegate]", reinterpret_cast<const void*>(results_controller_delegate));
    objc_register_method("-[_NSDefaultSectionInfo numberOfObjects]", reinterpret_cast<const void*>(section_count));
    objc_register_method("-[_NSDefaultSectionInfo objects]", reinterpret_cast<const void*>(section_objects));
    objc_register_method("-[_NSDefaultSectionInfo name]", reinterpret_cast<const void*>(section_name));
    objc_register_method("-[_NSDefaultSectionInfo indexTitle]", reinterpret_cast<const void*>(section_name));
    objc_register_method("-[NSMergePolicy initWithMergeType:]", reinterpret_cast<const void*>(return_self_word));
    objc_register_method("-[NSManagedObjectContext initWithConcurrencyType:]", reinterpret_cast<const void*>(return_self_word));
    objc_register_method("-[NSManagedObjectContext setParentContext:]", reinterpret_cast<const void*>(ignore_argument));
    objc_register_method("-[NSManagedObjectContext setPersistentStoreCoordinator:]", reinterpret_cast<const void*>(ignore_argument));
    objc_register_method("-[NSManagedObjectContext performBlock:]", reinterpret_cast<const void*>(context_perform));
    objc_register_method("-[NSManagedObjectContext performBlockAndWait:]", reinterpret_cast<const void*>(context_perform));
    objc_register_method("-[NSManagedObjectContext objectWithID:]", reinterpret_cast<const void*>(context_object_with_id));
    objc_register_method("-[NSManagedObjectContext refreshAllObjects]", reinterpret_cast<const void*>(ignore_nothing));
    objc_register_method("-[NSManagedObjectContext reset]", reinterpret_cast<const void*>(ignore_nothing));
    objc_register_method("-[NSManagedObjectContext processPendingChanges]", reinterpret_cast<const void*>(ignore_nothing));
    objc_register_method("-[NSPersistentContainer initWithName:managedObjectModel:]", reinterpret_cast<const void*>(return_self_two_arguments));
    objc_register_method("-[NSPersistentContainer newBackgroundContext]", reinterpret_cast<const void*>(container_background_context));
    objc_register_method("-[NSPersistentContainer performBackgroundTask:]", reinterpret_cast<const void*>(container_background_task));
    objc_register_method("+[NSManagedObjectModel mergedModelFromBundles:]", reinterpret_cast<const void*>(merged_model));
    objc_register_method("-[NSManagedObjectModel initWithContentsOfURL:]", reinterpret_cast<const void*>(return_self_argument));
    objc_register_method("-[NSManagedObject objectID]", reinterpret_cast<const void*>(managed_object_id));
    objc_register_method("-[NSManagedObject managedObjectContext]", reinterpret_cast<const void*>(managed_context));
    objc_register_method("-[NSManagedObject isDeleted]", reinterpret_cast<const void*>(managed_is_deleted));
    objc_register_method("-[NSManagedObject isInserted]", reinterpret_cast<const void*>(managed_false));
    objc_register_method("-[NSManagedObject willAccessValueForKey:]", reinterpret_cast<const void*>(ignore_argument));
    objc_register_method("-[NSManagedObject didAccessValueForKey:]", reinterpret_cast<const void*>(ignore_argument));
    objc_register_method("-[NSManagedObject valueForKey:]", reinterpret_cast<const void*>(managed_value_for_key));
    objc_register_method("-[NSManagedObject setValue:forKey:]", reinterpret_cast<const void*>(managed_set_value));
    objc_register_method("-[NSFetchRequest setResultType:]", reinterpret_cast<const void*>(ignore_word));
    objc_register_method("-[NSFetchRequest setReturnsObjectsAsFaults:]", reinterpret_cast<const void*>(ignore_word));
    objc_register_method("-[NSFetchRequest setIncludesPendingChanges:]", reinterpret_cast<const void*>(ignore_word));
    objc_register_method("-[NSFetchRequest setPropertiesToFetch:]", reinterpret_cast<const void*>(ignore_argument));
    objc_register_method("-[NSFetchRequest setRelationshipKeyPathsForPrefetching:]", reinterpret_cast<const void*>(ignore_argument));
    objc_register_fallback("NSManagedObject", managed_accessor);
    objc_register_method("-[NSManagedObject fetchRequest]", reinterpret_cast<const void*>(managed_fetch_request));
    objc_register_method("-[NSFetchRequest initWithEntityName:]", reinterpret_cast<const void*>(fetch_request_init));
    objc_register_method("-[NSCompoundPredicate initWithType:subpredicates:]", reinterpret_cast<const void*>(predicate_init_compound));
    objc_register_method("-[NSCompoundPredicate subpredicates]", reinterpret_cast<const void*>(predicate_subpredicates));
    objc_register_method("-[NSCompoundPredicate evaluateWithObject:]", reinterpret_cast<const void*>(predicate_evaluate_object));
    objc_register_method("+[NSCompoundPredicate andPredicateWithSubpredicates:]", reinterpret_cast<const void*>(predicate_and_of));
    objc_register_method("+[NSCompoundPredicate orPredicateWithSubpredicates:]", reinterpret_cast<const void*>(predicate_or_of));
    objc_register_method("+[NSCompoundPredicate notPredicateWithSubpredicate:]", reinterpret_cast<const void*>(predicate_not_of));
    objc_register_method("+[NSPredicate predicateWithValue:]", reinterpret_cast<const void*>(predicate_with_value));
    objc_register_method("-[NSPredicate evaluateWithObject:]", reinterpret_cast<const void*>(predicate_evaluate_object));
    objc_register_method("-[NSFetchRequest setPredicate:]", reinterpret_cast<const void*>(fetch_request_set_predicate));
    objc_register_method("-[NSFetchRequest predicate]", reinterpret_cast<const void*>(+[](void* self, const char*) { return static_cast<FetchRequest*>(self)->predicate; }));
    objc_register_method("-[NSFetchRequest setSortDescriptors:]", reinterpret_cast<const void*>(fetch_request_set_sort));
    objc_register_method("-[NSBatchDeleteRequest initWithFetchRequest:]", reinterpret_cast<const void*>(batch_delete_init));
    objc_register_method("-[NSManagedObjectContext deleteObject:]", reinterpret_cast<const void*>(context_delete));
    objc_register_method("-[NSManagedObjectContext executeRequest:error:]", reinterpret_cast<const void*>(context_execute));
    objc_register_method("-[NSManagedObjectContext countForFetchRequest:error:]", reinterpret_cast<const void*>(context_count_objc));
    objc_register_method("-[NSManagedObject entity]", reinterpret_cast<const void*>(managed_entity));
    objc_register_method("-[NSManagedObject initWithContext:]", reinterpret_cast<const void*>(managed_init_with_context));
    objc_register_method("-[NSManagedObject initWithEntity:insertIntoManagedObjectContext:]", reinterpret_cast<const void*>(managed_init_with_entity));
    objc_register_method("-[NSPersistentContainer initWithName:]", reinterpret_cast<const void*>(container_init_with_name));
    objc_register_method("-[NSPersistentContainer viewContext]", reinterpret_cast<const void*>(container_view_context));
    objc_register_method("-[NSPersistentContainer loadPersistentStoresWithCompletionHandler:]", reinterpret_cast<const void*>(container_load_stores));
    objc_register_method("-[NSManagedObjectContext setAutomaticallyMergesChangesFromParent:]", reinterpret_cast<const void*>(context_set_bool));
    objc_register_method("-[NSManagedObjectContext setMergePolicy:]", reinterpret_cast<const void*>(context_ignore_argument));
    objc_register_method("-[NSManagedObjectContext hasChanges]", reinterpret_cast<const void*>(context_has_changes));
    objc_register_method("-[NSManagedObjectContext save:]", reinterpret_cast<const void*>(context_save));
}
