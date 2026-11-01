#include "plist.h"

#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>
#include <iterator>
#include <utility>
#include <vector>

namespace {

constexpr size_t kHeaderSize = 8, kTrailerSize = 32;
constexpr int kMaxDepth = 64;

struct Reader {
    std::string data;
    size_t offset_size = 0, reference_size = 0;
    uint64_t object_count = 0, top_object = 0, table_offset = 0;

    uint64_t big_endian(size_t at, size_t size) const {
        uint64_t value = 0;
        for (size_t i = 0; i < size && at + i < data.size(); ++i) value = (value << 8) | static_cast<uint8_t>(data[at + i]);
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
        return offset_size != 0 && reference_size != 0;
    }

    // A nibble of 0xf means the real count follows as an integer object.
    size_t count_at(size_t start, size_t& body) const {
        const uint8_t low = static_cast<uint8_t>(data[start]) & 0x0f;
        body = start + 1;
        if (low != 0x0f) return low;
        const size_t size = size_t{1} << (static_cast<uint8_t>(data[start + 1]) & 0x0f);
        body = start + 2 + size;
        return big_endian(start + 2, size);
    }

    void read(uint64_t index, PlistValue& out, int depth) const {
        if (index >= object_count || depth > kMaxDepth) return;
        const size_t start = big_endian(table_offset + index * offset_size, offset_size);
        if (start >= data.size()) return;
        const uint8_t marker = static_cast<uint8_t>(data[start]);
        size_t body = 0;
        switch (marker >> 4) {
            case 0x0:
                if (marker == 0x08 || marker == 0x09) {
                    out.kind = PlistValue::Kind::Boolean;
                    out.boolean = marker == 0x09;
                }
                break;
            case 0x1:
                out.kind = PlistValue::Kind::Integer;
                out.integer = static_cast<int64_t>(big_endian(start + 1, size_t{1} << (marker & 0x0f)));
                break;
            case 0x2: {
                out.kind = PlistValue::Kind::Real;
                const uint64_t bits = big_endian(start + 1, size_t{1} << (marker & 0x0f));
                if ((marker & 0x0f) == 2) {
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
            case 0x4: {
                const size_t count = count_at(start, body);
                out.kind = PlistValue::Kind::Data;
                out.text = data.substr(body, count);
                break;
            }
            case 0x5: {
                const size_t count = count_at(start, body);
                out.kind = PlistValue::Kind::String;
                out.text = data.substr(body, count);
                break;
            }
            case 0x6: {
                const size_t count = count_at(start, body);
                out.kind = PlistValue::Kind::String;
                for (size_t i = 0; i < count; ++i) {
                    const auto unit = static_cast<uint32_t>(big_endian(body + i * 2, 2));
                    if (unit < 0x80) {
                        out.text += static_cast<char>(unit);
                    } else if (unit < 0x800) {
                        out.text += static_cast<char>(0xc0 | (unit >> 6));
                        out.text += static_cast<char>(0x80 | (unit & 0x3f));
                    } else {
                        out.text += static_cast<char>(0xe0 | (unit >> 12));
                        out.text += static_cast<char>(0x80 | ((unit >> 6) & 0x3f));
                        out.text += static_cast<char>(0x80 | (unit & 0x3f));
                    }
                }
                break;
            }
            case 0xa: {
                const size_t count = count_at(start, body);
                out.kind = PlistValue::Kind::Array;
                out.items.resize(count);
                for (size_t i = 0; i < count; ++i) read(big_endian(body + i * reference_size, reference_size), out.items[i], depth + 1);
                break;
            }
            case 0xd: {
                const size_t count = count_at(start, body);
                out.kind = PlistValue::Kind::Dictionary;
                for (size_t i = 0; i < count; ++i) {
                    PlistValue key;
                    read(big_endian(body + i * reference_size, reference_size), key, depth + 1);
                    read(big_endian(body + (count + i) * reference_size, reference_size), out.entries[key.text], depth + 1);
                }
                break;
            }
            default:
                break;
        }
    }
};

// XML plists: a small subset of XML, since the format has no attributes that matter and no mixed content.
struct XmlReader {
    const std::string& text;
    size_t at = 0;

    void skip_to_tag() {
        for (;;) {
            at = text.find('<', at);
            if (at == std::string::npos) return;
            if (text.compare(at, 2, "<?") == 0)
                at = text.find("?>", at);
            else if (text.compare(at, 4, "<!--") == 0)
                at = text.find("-->", at);
            else if (text.compare(at, 2, "<!") == 0)
                at = text.find('>', at);
            else
                return;
            if (at == std::string::npos) return;
            ++at;
        }
    }

    bool read_tag(std::string& name, bool& self_closing, bool& closing) {
        skip_to_tag();
        if (at == std::string::npos) return false;
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

    std::string read_content(const std::string& name) {
        const std::string close = "</" + name + ">";
        const size_t end = text.find(close, at);
        if (end == std::string::npos) return {};
        std::string content = text.substr(at, end - at);
        at = end + close.size();
        return content;
    }

    static std::string unescape(std::string value) {
        static const std::pair<const char*, const char*> kEntities[] = {{"&lt;", "<"}, {"&gt;", ">"}, {"&quot;", "\""}, {"&apos;", "'"}, {"&amp;", "&"}};
        for (const auto& [entity, plain] : kEntities) {
            size_t position = 0;
            while ((position = value.find(entity, position)) != std::string::npos) {
                value.replace(position, strlen(entity), plain);
                position += strlen(plain);
            }
        }
        return value;
    }

    static std::string decode_base64(const std::string& value) {
        std::string out;
        uint32_t bits = 0;
        int count = 0;
        for (const char c : value) {
            int digit = -1;
            if (c >= 'A' && c <= 'Z')
                digit = c - 'A';
            else if (c >= 'a' && c <= 'z')
                digit = c - 'a' + 26;
            else if (c >= '0' && c <= '9')
                digit = c - '0' + 52;
            else if (c == '+')
                digit = 62;
            else if (c == '/')
                digit = 63;
            if (digit < 0) continue;
            bits = (bits << 6) | static_cast<uint32_t>(digit);
            count += 6;
            if (count >= 8) {
                count -= 8;
                out += static_cast<char>((bits >> count) & 0xff);
            }
        }
        return out;
    }

    bool read_value(const std::string& name, bool self_closing, PlistValue& out, int depth) {
        if (depth > kMaxDepth) return false;
        if (name == "true" || name == "false") {
            out.kind = PlistValue::Kind::Boolean;
            out.boolean = name == "true";
            if (!self_closing) read_content(name);
            return true;
        }
        if (name == "array" || name == "dict") {
            out.kind = name == "array" ? PlistValue::Kind::Array : PlistValue::Kind::Dictionary;
            if (self_closing) return true;
            std::string key;
            for (;;) {
                std::string tag;
                bool child_self_closing = false, closing = false;
                if (!read_tag(tag, child_self_closing, closing)) return false;
                if (closing) return tag == name;
                if (tag == "key") {
                    key = unescape(read_content(tag));
                    continue;
                }
                PlistValue child;
                if (!read_value(tag, child_self_closing, child, depth + 1)) return false;
                if (out.kind == PlistValue::Kind::Array)
                    out.items.push_back(std::move(child));
                else
                    out.entries[key] = std::move(child);
            }
        }
        const std::string content = self_closing ? std::string() : read_content(name);
        if (name == "string") {
            out.kind = PlistValue::Kind::String;
            out.text = unescape(content);
        } else if (name == "integer") {
            out.kind = PlistValue::Kind::Integer;
            out.integer = strtoll(content.c_str(), nullptr, 10);
        } else if (name == "real") {
            out.kind = PlistValue::Kind::Real;
            out.real = strtod(content.c_str(), nullptr);
        } else if (name == "data") {
            out.kind = PlistValue::Kind::Data;
            out.text = decode_base64(content);
        } else if (name == "date") {
            constexpr double kSecondsFrom1970To2001 = 978307200.0;
            tm parts{};
            if (strptime(content.c_str(), "%Y-%m-%dT%H:%M:%SZ", &parts) == nullptr) return false;
            out.kind = PlistValue::Kind::Date;
            out.real = static_cast<double>(timegm(&parts)) - kSecondsFrom1970To2001;
        } else {
            return false;
        }
        return true;
    }

    bool read_document(PlistValue& out) {
        std::string tag;
        bool self_closing = false, closing = false;
        if (!read_tag(tag, self_closing, closing) || tag != "plist") return false;
        if (!read_tag(tag, self_closing, closing) || closing) return false;
        return read_value(tag, self_closing, out, 0);
    }
};

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
    Reader reader;
    reader.data = bytes;
    if (!reader.parse_trailer()) return false;
    reader.read(reader.top_object, out, 0);
    return true;
}

bool plist_read_file(const std::string& path, PlistValue& out) {
    std::ifstream file(path, std::ios::binary);
    if (!file) return false;
    return plist_parse(std::string(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()), out);
}
