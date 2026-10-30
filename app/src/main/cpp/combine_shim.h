#pragma once
#include <string_view>

// Combine: @Published storage with its publisher, subjects and sinks.
void* combine_symbol(std::string_view name);
