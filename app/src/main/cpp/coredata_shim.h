#pragma once
#include <string>
#include <string_view>
#include <vector>

// Core Data: an object store behind NSPersistentContainer, with NSManagedObject's dynamic accessors.
void coredata_register_classes();
void* coredata_symbol(std::string_view name);

struct MachImage;

// Finds the app's managed object classes, which restoring saved objects needs.
void coredata_attach_image(const MachImage& image);

// The context the app's @Environment(\\.managedObjectContext) reads.
void* coredata_view_context();

// The objects an @FetchRequest shows: `sort_descriptors` is a native [NSSortDescriptor], `predicate` may be null.
std::vector<void*> coredata_fetch(const std::string& entity, void* predicate, void* sort_descriptors);

// The entity an NSEntityDescription names.
std::string coredata_entity_name(void* description);
