#include "plist.h"

#include <cstring>
#include <fstream>
#include <iterator>
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
