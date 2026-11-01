#include "json.h"

#include "base64.h"

#include <cstdio>
#include <cstdlib>

namespace {

constexpr int kMaximumDepth = 256;

struct Parser {
    const std::string& source;
    size_t cursor = 0;

    void skip_space() {
        while (cursor < source.size() && (source[cursor] == ' ' || source[cursor] == '\n' || source[cursor] == '\t' || source[cursor] == '\r')) ++cursor;
    }

    bool literal(const char* word) {
        const std::string text(word);
        if (source.compare(cursor, text.size(), text) != 0) return false;
        cursor += text.size();
        return true;
    }

    static void append_utf8(std::string& out, uint32_t code) {
        if (code < 0x80) {
            out += static_cast<char>(code);
        } else if (code < 0x800) {
            out += static_cast<char>(0xC0 | (code >> 6));
            out += static_cast<char>(0x80 | (code & 0x3F));
        } else if (code < 0x10000) {
            out += static_cast<char>(0xE0 | (code >> 12));
            out += static_cast<char>(0x80 | ((code >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (code & 0x3F));
        } else {
            out += static_cast<char>(0xF0 | (code >> 18));
            out += static_cast<char>(0x80 | ((code >> 12) & 0x3F));
            out += static_cast<char>(0x80 | ((code >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (code & 0x3F));
        }
    }

    bool hex4(uint32_t& out) {
        if (cursor + 4 > source.size()) return false;
        out = 0;
        for (int i = 0; i < 4; ++i) {
            const char c = source[cursor++];
            out <<= 4;
            if (c >= '0' && c <= '9')
                out |= static_cast<uint32_t>(c - '0');
            else if (c >= 'a' && c <= 'f')
                out |= static_cast<uint32_t>(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F')
                out |= static_cast<uint32_t>(c - 'A' + 10);
            else
                return false;
        }
        return true;
    }

    bool string(std::string& out) {
        if (cursor >= source.size() || source[cursor] != '"') return false;
        ++cursor;
        while (cursor < source.size()) {
            const char c = source[cursor++];
            if (c == '"') return true;
            if (c != '\\') {
                out += c;
                continue;
            }
            if (cursor >= source.size()) return false;
            const char escape = source[cursor++];
            switch (escape) {
                case '"':
                    out += '"';
                    break;
                case '\\':
                    out += '\\';
                    break;
                case '/':
                    out += '/';
                    break;
                case 'b':
                    out += '\b';
                    break;
                case 'f':
                    out += '\f';
                    break;
                case 'n':
                    out += '\n';
                    break;
                case 'r':
                    out += '\r';
                    break;
                case 't':
                    out += '\t';
                    break;
                case 'u': {
                    uint32_t code;
                    if (!hex4(code)) return false;
                    if (code >= 0xD800 && code < 0xDC00 && source.compare(cursor, 2, "\\u") == 0) {
                        cursor += 2;
                        uint32_t low;
                        if (!hex4(low) || low < 0xDC00 || low > 0xDFFF) return false;
                        code = 0x10000 + ((code - 0xD800) << 10) + (low - 0xDC00);
                    }
                    append_utf8(out, code);
                    break;
                }
                default:
                    return false;
            }
        }
        return false;
    }

    bool number(Json& out) {
        const size_t start = cursor;
        bool integral = true;
        if (cursor < source.size() && source[cursor] == '-') ++cursor;
        while (cursor < source.size()) {
            const char c = source[cursor];
            if (c == '.' || c == 'e' || c == 'E' || c == '+' || (c == '-' && cursor != start))
                integral = false;
            else if (c < '0' || c > '9')
                break;
            ++cursor;
        }
        if (cursor == start) return false;
        const std::string text = source.substr(start, cursor - start);
        char* end = nullptr;
        if (integral) {
            out.kind = Json::Kind::Integer;
            out.integer = strtoll(text.c_str(), &end, 10);
            out.number = static_cast<double>(out.integer);
        } else {
            out.kind = Json::Kind::Number;
            out.number = strtod(text.c_str(), &end);
            out.integer = static_cast<int64_t>(out.number);
        }
        return end != nullptr && *end == '\0';
    }

    bool value(Json& out, int depth) {
        if (depth > kMaximumDepth) return false;
        skip_space();
        if (cursor >= source.size()) return false;
        const char c = source[cursor];
        if (c == '{') {
            ++cursor;
            out.kind = Json::Kind::Object;
            skip_space();
            if (cursor < source.size() && source[cursor] == '}') {
                ++cursor;
                return true;
            }
            for (;;) {
                skip_space();
                std::string key;
                if (!string(key)) return false;
                skip_space();
                if (cursor >= source.size() || source[cursor++] != ':') return false;
                Json member;
                if (!value(member, depth + 1)) return false;
                out.members.emplace_back(std::move(key), std::move(member));
                skip_space();
                if (cursor >= source.size()) return false;
                const char next = source[cursor++];
                if (next == '}') return true;
                if (next != ',') return false;
            }
        }
        if (c == '[') {
            ++cursor;
            out.kind = Json::Kind::Array;
            skip_space();
            if (cursor < source.size() && source[cursor] == ']') {
                ++cursor;
                return true;
            }
            for (;;) {
                Json item;
                if (!value(item, depth + 1)) return false;
                out.items.push_back(std::move(item));
                skip_space();
                if (cursor >= source.size()) return false;
                const char next = source[cursor++];
                if (next == ']') return true;
                if (next != ',') return false;
            }
        }
        if (c == '"') {
            out.kind = Json::Kind::String;
            return string(out.text);
        }
        if (literal("true")) {
            out.kind = Json::Kind::Bool;
            out.flag = true;
            return true;
        }
        if (literal("false")) {
            out.kind = Json::Kind::Bool;
            return true;
        }
        if (literal("null")) return true;
        return number(out);
    }
};

void write_string(const std::string& text, std::string& out) {
    out += '"';
    for (const char c : text) {
        switch (c) {
            case '"':
                out += "\\\"";
                break;
            case '\\':
                out += "\\\\";
                break;
            case '\n':
                out += "\\n";
                break;
            case '\r':
                out += "\\r";
                break;
            case '\t':
                out += "\\t";
                break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    char escaped[8];
                    snprintf(escaped, sizeof escaped, "\\u%04x", static_cast<unsigned>(c));
                    out += escaped;
                } else {
                    out += c;
                }
        }
    }
    out += '"';
}

void write_value(const Json& value, std::string& out) {
    switch (value.kind) {
        case Json::Kind::Null:
            out += "null";
            break;
        case Json::Kind::Bool:
            out += value.flag ? "true" : "false";
            break;
        case Json::Kind::Integer:
            out += std::to_string(value.integer);
            break;
        case Json::Kind::Number: {
            char text[40];
            snprintf(text, sizeof text, "%.17g", value.number);
            out += text;
            break;
        }
        case Json::Kind::String:
            write_string(value.text, out);
            break;
        // These are JSONEncoder's default strategies.
        case Json::Kind::Date: {
            char text[40];
            snprintf(text, sizeof text, "%.17g", value.number);
            out += text;
            break;
        }
        case Json::Kind::Data:
            write_string(base64_encode(value.text), out);
            break;
        case Json::Kind::Array: {
            out += '[';
            for (size_t i = 0; i < value.items.size(); ++i) {
                if (i != 0) out += ',';
                write_value(value.items[i], out);
            }
            out += ']';
            break;
        }
        case Json::Kind::Object: {
            out += '{';
            for (size_t i = 0; i < value.members.size(); ++i) {
                if (i != 0) out += ',';
                write_string(value.members[i].first, out);
                out += ':';
                write_value(value.members[i].second, out);
            }
            out += '}';
            break;
        }
    }
}

}  // namespace

const Json* Json::member(const std::string& name) const {
    for (const auto& [key, value] : members) {
        if (key == name) return &value;
    }
    return nullptr;
}

bool json_parse(const std::string& source, Json& out) {
    Parser parser{source};
    if (!parser.value(out, 0)) return false;
    parser.skip_space();
    return parser.cursor == source.size();
}

std::string json_write(const Json& value) {
    std::string out;
    write_value(value, out);
    return out;
}
