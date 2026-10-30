#pragma once
#include <string_view>

// Returns the Swift and ObjC runtime's implementation of a guest import, or nullptr.
void* swift_runtime_symbol(std::string_view name);

// Finds the implementation a conformance descriptor lists for a resilient protocol requirement.
const void* find_witness(const char* conformance, const void* requirement);

extern "C" void* rt_call_witness(const void* function, void* out, void* self, void* type, void* table);

// Registers the implementation a framework conformance gives for a protocol requirement.
void register_foreign_witness(const char* conformance_symbol, const char* requirement_symbol, const void* implementation);
