#pragma once
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

// A value from a binary property list.
struct PlistValue {
    enum class Kind { Null,
                      Boolean,
                      Integer,
                      Real,
                      String,
                      Data,
                      Array,
                      Dictionary };
    Kind kind = Kind::Null;
    bool boolean = false;
    int64_t integer = 0;
    double real = 0;
    std::string text;  // A string's UTF-8, or raw bytes for data.
    std::vector<PlistValue> items;
    std::map<std::string, PlistValue> entries;

    const PlistValue* get(const std::string& key) const;
    std::string string_at(const std::string& key) const;
};

// Reads a binary plist file, returning false if it is missing or malformed.
bool plist_read_file(const std::string& path, PlistValue& out);

// Reads a binary plist held in memory.
bool plist_parse(const std::string& bytes, PlistValue& out);
