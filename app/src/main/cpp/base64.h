#pragma once
#include <string>

std::string base64_encode(const std::string& bytes);

// Skips anything outside the alphabet, such as the line breaks in a plist's <data>.
std::string base64_decode(const std::string& text);

// Stops at padding and returns false on any other character outside the alphabet.
bool base64_decode_strict(const std::string& text, std::string& out);
