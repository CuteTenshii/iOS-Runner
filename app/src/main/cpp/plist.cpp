#include "plist.h"

#include "base64.h"

#include <cinttypes>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>
#include <iterator>
#include <set>
#include <utility>

namespace {

constexpr size_t kHeaderSize = 8, kTrailerSize = 32;
constexpr int kMaxDepth = 512;
constexpr double kSecondsFrom1970To2001 = 978307200.0;

void append_utf8(std::string& out, uint32_t scalar) {
    if (scalar < 0x80) {
        out += static_cast<char>(scalar);
    } else if (scalar < 0x800) {
        out += static_cast<char>(0xc0 | (scalar >> 6));
        out += static_cast<char>(0x80 | (scalar & 0x3f));
    } else if (scalar < 0x10000) {
        out += static_cast<char>(0xe0 | (scalar >> 12));
        out += static_cast<char>(0x80 | ((scalar >> 6) & 0x3f));
        out += static_cast<char>(0x80 | (scalar & 0x3f));
    } else {
        out += static_cast<char>(0xf0 | (scalar >> 18));
        out += static_cast<char>(0x80 | ((scalar >> 12) & 0x3f));
        out += static_cast<char>(0x80 | ((scalar >> 6) & 0x3f));
        out += static_cast<char>(0x80 | (scalar & 0x3f));
    }
}

struct BinaryReader {
    const std::string& data;
    size_t offset_size = 0, reference_size = 0;
    uint64_t object_count = 0, top_object = 0, table_offset = 0;
    bool failed = false;

    uint64_t big_endian(size_t at, size_t size) {
        if (size > sizeof(uint64_t) || at > data.size() || size > data.size() - at) {
            failed = true;
            return 0;
        }
        uint64_t value = 0;
        for (size_t i = 0; i < size; ++i) value = (value << 8) | static_cast<uint8_t>(data[at + i]);
        return value;
    }

    bool parse_trailer() {
        if (data.size() < kHeaderSize + kTrailerSize || data.compare(0, 6, "bplist") != 0) return false;
        const size_t trailer = data.size() - kTrailerSize;
        offset_size = static_cast<uint8_t>(data[trailer + 6]);
        reference_size = static_cast<uint8_t>(data[trailer + 7]);
        object_count = big_endian(trailer + 8, 8);
        top_object = big_endian(trailer + 16, 8);
        table_offset = big_endian(trailer + 24, 8);
        return offset_size != 0 && reference_size != 0 && top_object < object_count && !failed;
    }

    // A nibble of 0xf means the real count follows as an integer object.
    size_t count_at(size_t start, size_t& body) {
        const uint8_t low = static_cast<uint8_t>(data[start]) & 0x0f;
        body = start + 1;
        if (low != 0x0f) return low;
        if (start + 1 >= data.size()) {
            failed = true;
            return 0;
        }
        const size_t size = size_t{1} << (static_cast<uint8_t>(data[start + 1]) & 0x0f);
        body = start + 2 + size;
        return big_endian(start + 2, size);
    }

    bool fits(size_t body, size_t count, size_t unit) {
        if (body > data.size() || (unit != 0 && count > (data.size() - body) / unit)) failed = true;
        return !failed;
    }

    void read(uint64_t index, PlistValue& out, int depth) {
        if (failed || index >= object_count || depth > kMaxDepth) {
            failed = true;
            return;
        }
        const size_t start = big_endian(table_offset + index * offset_size, offset_size);
        if (failed || start >= data.size()) {
            failed = true;
            return;
        }
        const uint8_t marker = static_cast<uint8_t>(data[start]);
        size_t body = 0;
        switch (marker >> 4) {
            case 0x0:
                if (marker == 0x08 || marker == 0x09) {
                    out.kind = PlistValue::Kind::Boolean;
                    out.boolean = marker == 0x09;
                }
                break;
            case 0x1: {
                out.kind = PlistValue::Kind::Integer;
                const size_t size = size_t{1} << (marker & 0x0f);
                // Sixteen-byte integers hold values past Int64 in their low half, the most this type can keep.
                out.integer = static_cast<int64_t>(size > sizeof(uint64_t) ? big_endian(start + 1 + size - 8, 8) : big_endian(start + 1, size));
                break;
            }
            case 0x2: {
                out.kind = PlistValue::Kind::Real;
                const size_t size = size_t{1} << (marker & 0x0f);
                const uint64_t bits = big_endian(start + 1, size);
                if (size == sizeof(float)) {
                    float narrow;
                    const auto small = static_cast<uint32_t>(bits);
                    memcpy(&narrow, &small, sizeof narrow);
                    out.real = narrow;
                } else {
                    memcpy(&out.real, &bits, sizeof out.real);
                }
                break;
            }
            case 0x3: {
                out.kind = PlistValue::Kind::Date;
                const uint64_t bits = big_endian(start + 1, sizeof(double));
                memcpy(&out.real, &bits, sizeof out.real);
                break;
            }
            case 0x4:
            case 0x5: {
                const size_t count = count_at(start, body);
                if (!fits(body, count, 1)) return;
                out.kind = marker >> 4 == 0x4 ? PlistValue::Kind::Data : PlistValue::Kind::String;
                out.text = data.substr(body, count);
                break;
            }
            case 0x6: {
                const size_t count = count_at(start, body);
                if (!fits(body, count, 2)) return;
                out.kind = PlistValue::Kind::String;
                for (size_t i = 0; i < count; ++i) {
                    auto unit = static_cast<uint32_t>(big_endian(body + i * 2, 2));
                    if (unit >= 0xd800 && unit < 0xdc00 && i + 1 < count) {
                        const auto low = static_cast<uint32_t>(big_endian(body + (i + 1) * 2, 2));
                        if (low >= 0xdc00 && low < 0xe000) {
                            unit = 0x10000 + ((unit - 0xd800) << 10) + (low - 0xdc00);
                            ++i;
                        }
                    }
                    append_utf8(out.text, unit);
                }
                break;
            }
            case 0x8:
                out.kind = PlistValue::Kind::Uid;
                out.integer = static_cast<int64_t>(big_endian(start + 1, (marker & 0x0f) + size_t{1}));
                break;
            case 0xa:
            case 0xc: {
                const size_t count = count_at(start, body);
                if (!fits(body, count, reference_size)) return;
                out.kind = PlistValue::Kind::Array;
                out.items.resize(count);
                for (size_t i = 0; i < count && !failed; ++i) read(big_endian(body + i * reference_size, reference_size), out.items[i], depth + 1);
                break;
            }
            case 0xd: {
                const size_t count = count_at(start, body);
                if (!fits(body, count, 2 * reference_size)) return;
                out.kind = PlistValue::Kind::Dictionary;
                for (size_t i = 0; i < count && !failed; ++i) {
                    PlistValue key;
                    read(big_endian(body + i * reference_size, reference_size), key, depth + 1);
                    if (key.kind != PlistValue::Kind::String) {
                        failed = true;
                        return;
                    }
                    read(big_endian(body + (count + i) * reference_size, reference_size), out.entries[key.text], depth + 1);
                }
                break;
            }
            default:
                failed = true;
                break;
        }
    }
};

struct XmlReader {
    const std::string& text;
    size_t at = 0;

    // Skips the prolog, comments and doctype, stopping at the next element tag.
    bool skip_to_tag() {
        for (;;) {
            at = text.find('<', at);
            if (at == std::string::npos) return false;
            const char* close = nullptr;
            if (text.compare(at, 2, "<?") == 0)
                close = "?>";
            else if (text.compare(at, 4, "<!--") == 0)
                close = "-->";
            else if (text.compare(at, 2, "<!") == 0)
                close = ">";
            if (close == nullptr) return true;
            at = text.find(close, at);
            if (at == std::string::npos) return false;
            at += strlen(close);
        }
    }

    bool read_tag(std::string& name, bool& self_closing, bool& closing) {
        if (!skip_to_tag()) return false;
        const size_t end = text.find('>', at);
        if (end == std::string::npos) return false;
        std::string inside = text.substr(at + 1, end - at - 1);
        at = end + 1;
        closing = !inside.empty() && inside[0] == '/';
        if (closing) inside.erase(0, 1);
        self_closing = !inside.empty() && inside.back() == '/';
        if (self_closing) inside.pop_back();
        name = inside.substr(0, inside.find_first_of(" \t\r\n"));
        return true;
    }

    bool read_content(const std::string& name, std::string& out) {
        const std::string close = "</" + name + ">";
        const size_t end = text.find(close, at);
        if (end == std::string::npos) return false;
        out = unescape(text.substr(at, end - at));
        at = end + close.size();
        return true;
    }

    static std::string unescape(const std::string& value) {
        std::string out;
        for (size_t i = 0; i < value.size(); ++i) {
            if (value.compare(i, 9, "<![CDATA[") == 0) {
                const size_t end = value.find("]]>", i + 9);
                if (end == std::string::npos) return out;
                out.append(value, i + 9, end - i - 9);
                i = end + 2;
                continue;
            }
            const size_t semicolon = value[i] == '&' ? value.find(';', i) : std::string::npos;
            if (semicolon == std::string::npos) {
                out += value[i];
                continue;
            }
            const std::string entity = value.substr(i + 1, semicolon - i - 1);
            if (entity == "lt")
                out += '<';
            else if (entity == "gt")
                out += '>';
            else if (entity == "amp")
                out += '&';
            else if (entity == "quot")
                out += '"';
            else if (entity == "apos")
                out += '\'';
            else if (entity.size() > 2 && entity[0] == '#' && entity[1] == 'x')
                append_utf8(out, static_cast<uint32_t>(strtoul(entity.c_str() + 2, nullptr, 16)));
            else if (entity.size() > 1 && entity[0] == '#')
                append_utf8(out, static_cast<uint32_t>(strtoul(entity.c_str() + 1, nullptr, 10)));
            else
                out += value.substr(i, semicolon - i + 1);
            i = semicolon;
        }
        return out;
    }

    bool read_value(const std::string& name, bool self_closing, PlistValue& out, int depth) {
        if (depth > kMaxDepth) return false;
        if (name == "true" || name == "false") {
            out.kind = PlistValue::Kind::Boolean;
            out.boolean = name == "true";
            std::string ignored;
            return self_closing || read_content(name, ignored);
        }
        if (name == "array" || name == "dict") {
            out.kind = name == "array" ? PlistValue::Kind::Array : PlistValue::Kind::Dictionary;
            if (self_closing) return true;
            std::string key;
            bool has_key = false;
            for (;;) {
                std::string tag;
                bool child_self_closing = false, closing = false;
                if (!read_tag(tag, child_self_closing, closing)) return false;
                if (closing) return tag == name && !has_key;
                if (tag == "key") {
                    if (out.kind != PlistValue::Kind::Dictionary || has_key) return false;
                    if (child_self_closing)
                        key.clear();
                    else if (!read_content(tag, key))
                        return false;
                    has_key = true;
                    continue;
                }
                PlistValue child;
                if (!read_value(tag, child_self_closing, child, depth + 1)) return false;
                if (out.kind == PlistValue::Kind::Array) {
                    out.items.push_back(std::move(child));
                } else {
                    if (!has_key) return false;
                    out.entries[key] = std::move(child);
                    has_key = false;
                }
            }
        }
        std::string content;
        if (!self_closing && !read_content(name, content)) return false;
        if (name == "string") {
            out.kind = PlistValue::Kind::String;
            out.text = std::move(content);
        } else if (name == "integer") {
            out.kind = PlistValue::Kind::Integer;
            const size_t digits = content.find_first_not_of(" \t\r\n");
            const bool hex = digits != std::string::npos && content.compare(digits, 2, "0x") == 0;
            out.integer = hex ? static_cast<int64_t>(strtoull(content.c_str(), nullptr, 16)) : strtoll(content.c_str(), nullptr, 10);
        } else if (name == "real") {
            out.kind = PlistValue::Kind::Real;
            out.real = strtod(content.c_str(), nullptr);
        } else if (name == "data") {
            out.kind = PlistValue::Kind::Data;
            out.text = base64_decode(content);
        } else if (name == "date") {
            tm parts{};
            const char* rest = strptime(content.c_str(), "%Y-%m-%dT%H:%M:%S", &parts);
            if (rest == nullptr) return false;
            out.kind = PlistValue::Kind::Date;
            out.real = static_cast<double>(timegm(&parts)) - kSecondsFrom1970To2001;
            if (*rest == '.') out.real += strtod(rest, nullptr);
        } else {
            return false;
        }
        return true;
    }

    bool read_document(PlistValue& out) {
        std::string tag;
        bool self_closing = false, closing = false;
        if (!read_tag(tag, self_closing, closing) || closing) return false;
        if (tag == "plist") {
            if (!read_tag(tag, self_closing, closing) || closing) return false;
        }
        return read_value(tag, self_closing, out, 0);
    }
};

// Flattens the value into the object table, sharing equal strings the way Apple's writer does.
struct BinaryWriter {
    std::vector<std::string> objects;
    std::map<std::string, size_t> strings;
    size_t reference_size = 1;

    static void put_big_endian(std::string& out, uint64_t value, size_t size) {
        for (size_t i = size; i > 0; --i) out += static_cast<char>((value >> ((i - 1) * 8)) & 0xff);
    }

    static std::string integer_object(int64_t value) {
        std::string out;
        // Negative values take the full eight bytes, as they would read back as large unsigned ones otherwise.
        if (value >= 0 && value <= 0xff) {
            out += '\x10';
            put_big_endian(out, static_cast<uint64_t>(value), 1);
        } else if (value >= 0 && value <= 0xffff) {
            out += '\x11';
            put_big_endian(out, static_cast<uint64_t>(value), 2);
        } else if (value >= 0 && value <= 0xffffffffLL) {
            out += '\x12';
            put_big_endian(out, static_cast<uint64_t>(value), 4);
        } else {
            out += '\x13';
            put_big_endian(out, static_cast<uint64_t>(value), 8);
        }
        return out;
    }

    static std::string header(uint8_t kind, size_t count) {
        std::string out;
        if (count < 15) {
            out += static_cast<char>((kind << 4) | count);
        } else {
            out += static_cast<char>((kind << 4) | 0x0f);
            out += integer_object(static_cast<int64_t>(count));
        }
        return out;
    }

    static bool is_ascii(const std::string& text) {
        for (const char c : text) {
            if (static_cast<uint8_t>(c) >= 0x80) return false;
        }
        return true;
    }

    static std::string string_object(const std::string& text) {
        if (is_ascii(text)) return header(0x5, text.size()) + text;
        std::string units;
        size_t count = 0;
        for (size_t i = 0; i < text.size();) {
            const auto first = static_cast<uint8_t>(text[i]);
            size_t length = first < 0x80 ? 1 : first < 0xe0 ? 2
                                           : first < 0xf0   ? 3
                                                            : 4;
            if (i + length > text.size()) length = text.size() - i;
            uint32_t scalar = length == 1 ? first : first & (0xff >> (length + 1));
            for (size_t j = 1; j < length; ++j) scalar = (scalar << 6) | (static_cast<uint8_t>(text[i + j]) & 0x3f);
            i += length;
            if (scalar >= 0x10000) {
                scalar -= 0x10000;
                put_big_endian(units, 0xd800 + (scalar >> 10), 2);
                put_big_endian(units, 0xdc00 + (scalar & 0x3ff), 2);
                count += 2;
            } else {
                put_big_endian(units, scalar, 2);
                ++count;
            }
        }
        return header(0x6, count) + units;
    }

    static size_t count_objects(const PlistValue& value) {
        size_t count = 1;
        for (const auto& item : value.items) count += count_objects(item);
        for (const auto& [key, item] : value.entries) count += 1 + count_objects(item);
        return count;
    }

    size_t add_string(const std::string& text) {
        const auto found = strings.find(text);
        if (found != strings.end()) return found->second;
        objects.push_back(string_object(text));
        strings[text] = objects.size() - 1;
        return objects.size() - 1;
    }

    size_t add(const PlistValue& value) {
        switch (value.kind) {
            case PlistValue::Kind::String:
                return add_string(value.text);
            case PlistValue::Kind::Array:
            case PlistValue::Kind::Dictionary: {
                // Containers take their slot first, so the top object is always number zero.
                const size_t slot = objects.size();
                objects.emplace_back();
                std::vector<size_t> references;
                if (value.kind == PlistValue::Kind::Array) {
                    for (const auto& item : value.items) references.push_back(add(item));
                } else {
                    for (const auto& [key, item] : value.entries) references.push_back(add_string(key));
                    for (const auto& [key, item] : value.entries) references.push_back(add(item));
                }
                const size_t count = value.kind == PlistValue::Kind::Array ? value.items.size() : value.entries.size();
                std::string object = header(value.kind == PlistValue::Kind::Array ? 0xa : 0xd, count);
                for (const size_t reference : references) put_big_endian(object, reference, reference_size);
                objects[slot] = std::move(object);
                return slot;
            }
            default:
                break;
        }
        std::string object;
        switch (value.kind) {
            case PlistValue::Kind::Null:
                object += '\0';
                break;
            case PlistValue::Kind::Boolean:
                object += value.boolean ? '\x09' : '\x08';
                break;
            case PlistValue::Kind::Integer:
                object = integer_object(value.integer);
                break;
            case PlistValue::Kind::Real:
            case PlistValue::Kind::Date: {
                object += value.kind == PlistValue::Kind::Real ? '\x23' : '\x33';
                uint64_t bits;
                memcpy(&bits, &value.real, sizeof bits);
                put_big_endian(object, bits, sizeof bits);
                break;
            }
            case PlistValue::Kind::Data:
                object = header(0x4, value.text.size()) + value.text;
                break;
            case PlistValue::Kind::Uid: {
                size_t size = 1;
                while (size < sizeof(uint64_t) && (static_cast<uint64_t>(value.integer) >> (size * 8)) != 0) ++size;
                object += static_cast<char>(0x80 | (size - 1));
                put_big_endian(object, static_cast<uint64_t>(value.integer), size);
                break;
            }
            default:
                break;
        }
        objects.push_back(std::move(object));
        return objects.size() - 1;
    }

    static size_t bytes_for(uint64_t value) {
        size_t size = 1;
        while (size < sizeof(uint64_t) && (value >> (size * 8)) != 0) ++size;
        return size;
    }

    std::string write(const PlistValue& value) {
        reference_size = bytes_for(count_objects(value));
        const size_t top = add(value);
        std::string out = "bplist00";
        std::vector<uint64_t> offsets;
        for (const auto& object : objects) {
            offsets.push_back(out.size());
            out += object;
        }
        const uint64_t table_offset = out.size();
        const size_t offset_size = bytes_for(table_offset);
        for (const uint64_t offset : offsets) put_big_endian(out, offset, offset_size);
        out.append(6, '\0');
        out += static_cast<char>(offset_size);
        out += static_cast<char>(reference_size);
        put_big_endian(out, objects.size(), 8);
        put_big_endian(out, top, 8);
        put_big_endian(out, table_offset, 8);
        return out;
    }
};

std::string xml_escaped(const std::string& text) {
    std::string out;
    for (const char c : text) {
        if (c == '<')
            out += "&lt;";
        else if (c == '>')
            out += "&gt;";
        else if (c == '&')
            out += "&amp;";
        else
            out += c;
    }
    return out;
}

void write_xml_value(const PlistValue& value, std::string& out, int depth) {
    const std::string indent(static_cast<size_t>(depth), '\t');
    char buffer[64];
    switch (value.kind) {
        case PlistValue::Kind::Null:
            break;
        case PlistValue::Kind::Boolean:
            out += indent + (value.boolean ? "<true/>\n" : "<false/>\n");
            break;
        case PlistValue::Kind::Integer:
            snprintf(buffer, sizeof buffer, "%" PRId64, value.integer);
            out += indent + "<integer>" + buffer + "</integer>\n";
            break;
        case PlistValue::Kind::Real:
            snprintf(buffer, sizeof buffer, "%.17g", value.real);
            out += indent + "<real>" + buffer + "</real>\n";
            break;
        case PlistValue::Kind::String:
            out += indent + "<string>" + xml_escaped(value.text) + "</string>\n";
            break;
        case PlistValue::Kind::Data:
            out += indent + "<data>" + base64_encode(value.text) + "</data>\n";
            break;
        case PlistValue::Kind::Date: {
            const auto seconds = static_cast<time_t>(floor(value.real + kSecondsFrom1970To2001));
            tm parts{};
            gmtime_r(&seconds, &parts);
            strftime(buffer, sizeof buffer, "%Y-%m-%dT%H:%M:%SZ", &parts);
            out += indent + "<date>" + buffer + "</date>\n";
            break;
        }
        case PlistValue::Kind::Uid:
            // XML has no UID type: this is the dictionary form NSKeyedArchiver's XML output uses.
            snprintf(buffer, sizeof buffer, "%" PRId64, value.integer);
            out += indent + "<dict>\n" + indent + "\t<key>CF$UID</key>\n" + indent + "\t<integer>" + buffer + "</integer>\n" + indent + "</dict>\n";
            break;
        case PlistValue::Kind::Array:
            if (value.items.empty()) {
                out += indent + "<array/>\n";
                break;
            }
            out += indent + "<array>\n";
            for (const auto& item : value.items) write_xml_value(item, out, depth + 1);
            out += indent + "</array>\n";
            break;
        case PlistValue::Kind::Dictionary:
            if (value.entries.empty()) {
                out += indent + "<dict/>\n";
                break;
            }
            out += indent + "<dict>\n";
            for (const auto& [key, item] : value.entries) {
                out += indent + "\t<key>" + xml_escaped(key) + "</key>\n";
                write_xml_value(item, out, depth + 1);
            }
            out += indent + "</dict>\n";
            break;
    }
}

}  // namespace

const PlistValue* PlistValue::get(const std::string& key) const {
    const auto found = entries.find(key);
    return found == entries.end() ? nullptr : &found->second;
}

std::string PlistValue::string_at(const std::string& key) const {
    const PlistValue* value = get(key);
    return value != nullptr && value->kind == Kind::String ? value->text : std::string();
}

bool plist_parse(const std::string& bytes, PlistValue& out) {
    if (bytes.compare(0, 6, "bplist") != 0) {
        XmlReader xml{bytes};
        return xml.read_document(out);
    }
    BinaryReader reader{bytes};
    if (!reader.parse_trailer()) return false;
    reader.read(reader.top_object, out, 0);
    return !reader.failed;
}

bool plist_read_file(const std::string& path, PlistValue& out) {
    std::ifstream file(path, std::ios::binary);
    if (!file) return false;
    return plist_parse(std::string(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()), out);
}

std::string plist_write_binary(const PlistValue& value) {
    BinaryWriter writer;
    return writer.write(value);
}

std::string plist_write_xml(const PlistValue& value) {
    std::string out = "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" \"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\n<plist version=\"1.0\">\n";
    write_xml_value(value, out, 0);
    out += "</plist>\n";
    return out;
}
