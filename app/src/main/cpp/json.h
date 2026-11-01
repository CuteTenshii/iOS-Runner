#pragma once
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

// A parsed JSON value. Object members keep their source order.
struct Json {
    enum class Kind { Null,
                      Bool,
                      Integer,
                      Number,
                      String,
                      Array,
                      Object,
                      Date,   // `number` holds seconds since 2001; plists have these and JSON doesn't.
                      Data }; // `text` holds the bytes.
    Kind kind = Kind::Null;
    bool flag = false;
    int64_t integer = 0;
    double number = 0;
    std::string text;
    std::vector<Json> items;
    std::vector<std::pair<std::string, Json>> members;

    const Json* member(const std::string& name) const;
};

inline Json json_text(const std::string& text) {
    Json value;
    value.kind = Json::Kind::String;
    value.text = text;
    return value;
}

// Returns false on malformed input, which includes anything after the first value.
bool json_parse(const std::string& source, Json& out);
std::string json_write(const Json& value);
