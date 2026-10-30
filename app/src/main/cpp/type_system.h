#pragma once
#include <string_view>

// Runtime type lookup: mangled-name resolution and metadata for standard library and framework types.
void* type_system_symbol(std::string_view name);

// Names the framework type behind runtime metadata, or nullptr for a type defined by the app.
const char* foreign_type_name(const void* metadata);

// Resolves a mangled type name, as swift_getTypeByMangledNameInContext2 does.
void* resolve_mangled_type(const char* name, void* const* generics);
