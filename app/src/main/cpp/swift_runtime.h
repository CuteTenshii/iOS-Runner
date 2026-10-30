#pragma once
#include <string_view>

// Returns the Swift and ObjC runtime's implementation of a guest import, or nullptr.
void* swift_runtime_symbol(std::string_view name);

// Finds the implementation a conformance descriptor lists for a resilient protocol requirement.
const void* find_witness(const char* conformance, const void* requirement);

extern "C" void* rt_call_witness(const void* function, void* out, void* self, void* type, void* table);

// Registers the implementation a framework conformance gives for a protocol requirement.
void register_foreign_witness(const char* conformance_symbol, const char* requirement_symbol, const void* implementation);

// Instantiates a witness table, as swift_getWitnessTable does.
void* swift_runtime_witness_table(const char* conformance, void* type, void* arguments);

// Finds the witness table of `type` for a protocol: from the app's conformance list, else a framework table.
void* swift_runtime_find_conformance(void* type, const void* protocol);

// Gives the runtime the app's conformance list.
void swift_runtime_attach_conformances(const void* section, size_t size);

// A distinct witness table standing in for a conformance whose type is not known.
void* swift_runtime_placeholder_conformance(const void* protocol);
