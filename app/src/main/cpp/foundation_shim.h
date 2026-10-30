#pragma once
#include <cstdint>
#include <string>
#include <string_view>

void* foundation_symbol(std::string_view name);
void foundation_register_classes();

// A Swift String as its two ABI words.
struct SwiftString {
    uint64_t count_and_flags;
    uint64_t object;
};

std::string swift_string_to_utf8(SwiftString value);
SwiftString swift_string_from_utf8(const std::string& text);
