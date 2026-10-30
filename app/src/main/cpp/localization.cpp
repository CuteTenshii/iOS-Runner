#include "localization.h"

#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <new>
#include <unordered_map>

#include "engine.h"

namespace {

constexpr size_t kTrailerSize = 32;
constexpr size_t kHeaderSize = 8;
constexpr uint8_t kMarkerAscii = 0x5;
constexpr uint8_t kMarkerUtf16 = 0x6;
constexpr uint8_t kMarkerDictionary = 0xd;
constexpr uint8_t kCountInline = 0x0f;

struct BinaryPlist {
    std::string data;
    size_t offset_size = 0;
    size_t reference_size = 0;
    uint64_t object_count = 0;
    uint64_t top_object = 0;
    uint64_t table_offset = 0;

    uint64_t read_big_endian(size_t position, size_t size) const {
        uint64_t value = 0;
        for (size_t i = 0; i < size && position + i < data.size(); ++i) value = (value << 8) | static_cast<uint8_t>(data[position + i]);
        return value;
    }

    size_t object_offset(uint64_t index) const { return read_big_endian(table_offset + index * offset_size, offset_size); }

    bool parse_trailer() {
        if (data.size() < kHeaderSize + kTrailerSize || data.compare(0, 6, "bplist") != 0) return false;
        const size_t trailer = data.size() - kTrailerSize;
        offset_size = static_cast<uint8_t>(data[trailer + 6]);
        reference_size = static_cast<uint8_t>(data[trailer + 7]);
        object_count = read_big_endian(trailer + 8, 8);
        top_object = read_big_endian(trailer + 16, 8);
        table_offset = read_big_endian(trailer + 24, 8);
        return true;
    }

    size_t read_count(size_t start, size_t& body) const {
        const uint8_t marker = static_cast<uint8_t>(data[start]);
        size_t count = marker & kCountInline;
        body = start + 1;
        if (count == kCountInline) {
            const size_t width = 1ULL << (static_cast<uint8_t>(data[body]) & kCountInline);
            count = read_big_endian(body + 1, width);
            body += 1 + width;
        }
        return count;
    }

    bool read_string(uint64_t index, std::string& out) const {
        const size_t start = object_offset(index);
        const uint8_t kind = static_cast<uint8_t>(data[start]) >> 4;
        size_t body;
        const size_t count = read_count(start, body);
        if (kind == kMarkerAscii) {
            out = data.substr(body, count);
            return true;
        }
        if (kind != kMarkerUtf16) return false;
        out.clear();
        for (size_t i = 0; i < count; ++i) {
            uint32_t unit = static_cast<uint32_t>(read_big_endian(body + i * 2, 2));
            if (unit >= 0xd800 && unit < 0xdc00 && i + 1 < count) {
                const uint32_t low = static_cast<uint32_t>(read_big_endian(body + (++i) * 2, 2));
                unit = 0x10000 + ((unit - 0xd800) << 10) + (low - 0xdc00);
            }
            if (unit < 0x80) {
                out += static_cast<char>(unit);
            } else if (unit < 0x800) {
                out += static_cast<char>(0xc0 | (unit >> 6));
                out += static_cast<char>(0x80 | (unit & 0x3f));
            } else if (unit < 0x10000) {
                out += static_cast<char>(0xe0 | (unit >> 12));
                out += static_cast<char>(0x80 | ((unit >> 6) & 0x3f));
                out += static_cast<char>(0x80 | (unit & 0x3f));
            } else {
                out += static_cast<char>(0xf0 | (unit >> 18));
                out += static_cast<char>(0x80 | ((unit >> 12) & 0x3f));
                out += static_cast<char>(0x80 | ((unit >> 6) & 0x3f));
                out += static_cast<char>(0x80 | (unit & 0x3f));
            }
        }
        return true;
    }
};

std::string g_language = "en";

const std::unordered_map<std::string, std::string>& strings() {
    static const auto* table = [] {
        auto* loaded = new std::unordered_map<std::string, std::string>();
        std::ifstream file(engine_bundle_dir() + "/" + g_language + ".lproj/Localizable.strings", std::ios::binary);
        BinaryPlist plist;
        plist.data.assign(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
        if (!plist.parse_trailer()) return loaded;
        const size_t start = plist.object_offset(plist.top_object);
        if ((static_cast<uint8_t>(plist.data[start]) >> 4) != kMarkerDictionary) return loaded;
        size_t body;
        const size_t count = plist.read_count(start, body);
        for (size_t i = 0; i < count; ++i) {
            std::string key, value;
            const uint64_t key_reference = plist.read_big_endian(body + i * plist.reference_size, plist.reference_size);
            const uint64_t value_reference = plist.read_big_endian(body + (count + i) * plist.reference_size, plist.reference_size);
            if (plist.read_string(key_reference, key) && plist.read_string(value_reference, value)) (*loaded)[key] = value;
        }
        engine_log("LOCALIZATION: %zu strings for %s", loaded->size(), g_language.c_str());
        return loaded;
    }();
    return *table;
}

void destroy_box(void* raw) {
    auto* box = static_cast<LocalizedBox*>(raw);
    delete box->key;
    delete box->arguments;
    free(box);
}

struct BoxMetadata {
    void (*destroyer)(void*);
    const void* witnesses;
    uint64_t kind;
};

const BoxMetadata kLocalizedMetadata = {destroy_box, nullptr, 0};

}  // namespace

LocalizedBox* new_localized_box(const std::string& key) {
    auto* box = static_cast<LocalizedBox*>(calloc(1, sizeof(LocalizedBox)));
    box->metadata = const_cast<uint64_t*>(&kLocalizedMetadata.kind);
    new (&box->references) std::atomic<uint64_t>(1);
    box->key = new std::string(key);
    box->arguments = new std::vector<std::string>();
    return box;
}

void localization_set_language(const std::string& language) { g_language = language; }

std::string localization_lookup(const std::string& key) {
    const auto found = strings().find(key);
    return found == strings().end() ? key : found->second;
}

std::string localization_format(const std::string& format, const std::vector<std::string>& arguments) {
    std::string out;
    size_t next = 0;
    for (size_t i = 0; i < format.size(); ++i) {
        if (format[i] != '%' || i + 1 >= format.size()) {
            out += format[i];
            continue;
        }
        if (format[i + 1] == '%') {
            out += '%';
            ++i;
            continue;
        }
        size_t cursor = i + 1;
        size_t position = 0;
        while (cursor < format.size() && format[cursor] >= '0' && format[cursor] <= '9') position = position * 10 + static_cast<size_t>(format[cursor++] - '0');
        size_t index = next;
        if (cursor < format.size() && format[cursor] == '$' && position > 0) {
            index = position - 1;
            ++cursor;
        } else {
            ++next;
        }
        while (cursor < format.size() && strchr("lhzqtj", format[cursor]) != nullptr) ++cursor;
        if (cursor < format.size()) ++cursor;
        if (index < arguments.size()) out += arguments[index];
        i = cursor - 1;
    }
    return out;
}

std::string localized_text(const LocalizedBox* box) {
    return localization_format(localization_lookup(*box->key), *box->arguments);
}

std::string bundle_info_string(const std::string& key) {
    std::ifstream file(engine_bundle_dir() + "/Info.plist", std::ios::binary);
    BinaryPlist plist;
    plist.data.assign(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
    if (!plist.parse_trailer()) return {};
    const size_t start = plist.object_offset(plist.top_object);
    if ((static_cast<uint8_t>(plist.data[start]) >> 4) != kMarkerDictionary) return {};
    size_t body;
    const size_t count = plist.read_count(start, body);
    for (size_t i = 0; i < count; ++i) {
        std::string name, value;
        if (plist.read_string(plist.read_big_endian(body + i * plist.reference_size, plist.reference_size), name) && name == key &&
            plist.read_string(plist.read_big_endian(body + (count + i) * plist.reference_size, plist.reference_size), value)) {
            return value;
        }
    }
    return {};
}
