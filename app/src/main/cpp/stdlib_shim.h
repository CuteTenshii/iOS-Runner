#pragma once
#include <cstddef>
#include <functional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// Swift standard library entry points: native collection storage, hashing and string operations.
void* stdlib_symbol(std::string_view name);

// Builds a native [String: Bool], in the storage the guest's inlined lookups expect.
void* make_string_bool_dictionary(const std::vector<std::pair<std::string, bool>>& entries);

// Builds a native [String: V], taking ownership of each value's bytes.
// A Set of these object references; `hashable` is the element type's conformance.
void* set_from_objects(const std::vector<void*>& objects, void* type, void* hashable);

void* make_string_dictionary(void* value_type, const std::vector<std::pair<std::string, const void*>>& entries);

// Calls `visit` with each key and a pointer to its value.
void visit_string_dictionary(void* storage, const std::function<void(const std::string&, void*)>& visit);

// Allocates a native array of `count` uninitialized elements; `elements` receives where they go.
void* make_array_storage(void* element_type, size_t count, char** elements);

// An array's element count and first element.
size_t array_storage_contents(void* storage, void* element_type, char** elements);
