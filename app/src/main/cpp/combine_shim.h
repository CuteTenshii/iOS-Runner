#pragma once
#include <string_view>

// Combine: @Published storage, subjects, sinks and the operators, as a chain of type-erased publisher nodes.
void* combine_symbol(std::string_view name);
