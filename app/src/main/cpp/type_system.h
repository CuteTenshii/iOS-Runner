#pragma once
#include <cstddef>
#include <string_view>

// Runtime type lookup: mangled-name resolution and metadata for standard library and framework types.
void* type_system_symbol(std::string_view name);

// Names the framework type behind runtime metadata, or nullptr for a type defined by the app.
const char* foreign_type_name(const void* metadata);

// Resolves a mangled type name, as swift_getTypeByMangledNameInContext2 does.
void* resolve_mangled_type(const char* name, void* const* generics);

// Metadata of a generic standard library storage class, whose prefix holds its destroyer.
void* storage_class_metadata(const char* symbol, void* const* arguments, size_t count);

// Generates executable code that puts `data` in x16 and jumps to `entry`, preserving all argument registers.
void* make_code_stub(void* entry, const void* data);

// Metadata of LazyState<T>, which the app stores inline: a Storage<T> followed by a location word.
void* lazy_state_metadata(void* value_type);
