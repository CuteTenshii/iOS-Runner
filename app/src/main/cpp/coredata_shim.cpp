#include "coredata_shim.h"

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <unordered_map>
#include <vector>

#include "engine.h"
#include "foundation_shim.h"
#include "objc_runtime.h"

extern "C" void* rt_swift_retain(void*);
extern "C" void rt_swift_release(void*);

namespace {

constexpr uintptr_t kClassDataMask = ~static_cast<uintptr_t>(7);
constexpr size_t kClassSuperclassOffset = 8;
constexpr size_t kClassDataOffset = 32;
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
    void* description = objc_framework_object("NSPersistentStoreDescription");
    static_cast<Literal*>(block)->invoke(block, description, nullptr);
}
void context_ignore_argument(void*, const char*, void*) {}
void context_set_bool(void*, const char*, bool) {}
bool context_has_changes(void*, const char*) { return store().has_changes; }
bool context_save(void*, const char*, void**) {
    store().has_changes = false;
    return true;
}

}  // namespace

void* coredata_view_context() { return view_context(); }

std::vector<void*> coredata_objects_of(const std::string& entity) {
    std::vector<void*> objects;
    for (void* object : store().insertion_order) {
        if (store().records[object].entity == entity) objects.push_back(object);
    }
    return objects;
}

std::string coredata_entity_name(void* description) { return nsstring_of_description(description); }

void* coredata_symbol(std::string_view name) {
    static void* merge_policy = objc_framework_object("NSMergePolicy");
    if (name == "NSMergeByPropertyObjectTrumpMergePolicy") return &merge_policy;
    return nullptr;
}

void coredata_register_classes() {
    objc_register_fallback("NSManagedObject", managed_accessor);
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
