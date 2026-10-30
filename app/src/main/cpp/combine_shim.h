#pragma once
#include <string_view>

// Combine: @Published storage with its publisher, subjects and sinks. Chains forward values to sinks directly.
void* combine_symbol(std::string_view name);
