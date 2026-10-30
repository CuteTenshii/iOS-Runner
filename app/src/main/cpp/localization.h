#pragma once
#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

// A localizable string: its lookup key plus the values to substitute into the format.
struct LocalizedBox {
    void* metadata;
    std::atomic<uint64_t> references;
    std::string* key;
    std::vector<std::string>* arguments;
};

LocalizedBox* new_localized_box(const std::string& key);
std::string localized_text(const LocalizedBox* box);

// Reads the bundle's Localizable.strings for the given language, falling back to the app's own text.
void localization_set_language(const std::string& language);
std::string localization_lookup(const std::string& key);
std::string localization_format(const std::string& format, const std::vector<std::string>& arguments);
