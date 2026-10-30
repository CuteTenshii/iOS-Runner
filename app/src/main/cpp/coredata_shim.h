#pragma once
#include <string>
#include <string_view>
#include <vector>

// Core Data: an object store behind NSPersistentContainer, with NSManagedObject's dynamic accessors.
void coredata_register_classes();
void* coredata_symbol(std::string_view name);

// The context the app's @FetchRequests and @Environment(\\.managedObjectContext) read from.
void* coredata_view_context();

// Objects of an entity, in insertion order, borrowed from the store.
std::vector<void*> coredata_objects_of(const std::string& entity);

// The entity an NSEntityDescription, or a managed object subclass's +entity, names.
std::string coredata_entity_name(void* description);
