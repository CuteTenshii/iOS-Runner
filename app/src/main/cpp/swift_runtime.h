#pragma once
#include <string_view>

// Returns the Swift and ObjC runtime's implementation of a guest import, or nullptr.
void* swift_runtime_symbol(std::string_view name);
