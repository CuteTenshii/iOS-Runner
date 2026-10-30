#pragma once
#include <string_view>

// Runtime type lookup: mangled-name resolution and metadata for standard library and framework types.
void* type_system_symbol(std::string_view name);
