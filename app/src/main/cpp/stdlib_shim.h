#pragma once
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// Swift standard library entry points: native collection storage, hashing and string operations.
void* stdlib_symbol(std::string_view name);

// Builds a native [String: Bool], in the storage the guest's inlined lookups expect.
void* make_string_bool_dictionary(const std::vector<std::pair<std::string, bool>>& entries);
