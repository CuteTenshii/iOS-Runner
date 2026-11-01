#include "uikit_assets.h"

#include <cstdint>
#include <cstring>
#include <fstream>
#include <iterator>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "engine.h"

namespace uikit {

namespace {

constexpr char kStoreMagic[] = "BOMStore";
constexpr size_t kStoreMagicLength = 8;
constexpr size_t kRenditionHeaderBytes = 184;
constexpr uint32_t kAttributeAppearance = 7;
constexpr uint32_t kAttributeNameIdentifier = 17;
constexpr uint32_t kAppearanceDark = 1;
constexpr char kColorTag[] = "RLOC";
constexpr size_t kColorTagLength = 4;

// A BOM store's own structure is big-endian, and the catalog data inside it little-endian.
class Store {
public:
    explicit Store(std::string bytes) : bytes_(std::move(bytes)) {}

    bool open() {
        if (bytes_.compare(0, kStoreMagicLength, kStoreMagic) != 0) return false;
        index_ = big32(16);
        const uint32_t variables = big32(24);
        const uint32_t count = big32(variables);
        size_t at = variables + 4;
        for (uint32_t i = 0; i < count && at + 5 <= bytes_.size(); ++i) {
            const uint32_t block = big32(at);
            const auto length = static_cast<uint8_t>(bytes_[at + 4]);
            variables_[bytes_.substr(at + 5, length)] = block;
            at += 5 + length;
        }
        return true;
    }

    std::string_view variable(const std::string& name) const {
        const auto found = variables_.find(name);
        return found == variables_.end() ? std::string_view() : block(found->second);
    }

    std::string_view block(uint32_t index) const {
        const size_t entry = index_ + 4 + size_t{index} * 8;
        const uint32_t offset = big32(entry);
        const uint32_t length = big32(entry + 4);
        if (offset > bytes_.size() || length > bytes_.size() - offset) return {};
        return std::string_view(bytes_).substr(offset, length);
    }

    // Each (key, value) in a tree's leaves, which are chained in order.
    std::vector<std::pair<std::string_view, std::string_view>> tree(const std::string& name) const {
        std::vector<std::pair<std::string_view, std::string_view>> entries;
        const auto found = variables_.find(name);
        if (found == variables_.end()) return entries;
        const std::string_view header = block(found->second);
        constexpr size_t kRootOffset = 8, kNodeHeaderBytes = 12;
        if (header.size() < kRootOffset + 4) return entries;
        uint32_t node = read_big32(header, kRootOffset);
        for (std::string_view page = block(node); page.size() >= kNodeHeaderBytes && !is_leaf(page); page = block(node)) node = read_big32(page, kNodeHeaderBytes);
        while (node != 0) {
            const std::string_view page = block(node);
            if (page.size() < kNodeHeaderBytes) break;
            const auto count = static_cast<uint16_t>((static_cast<uint8_t>(page[2]) << 8) | static_cast<uint8_t>(page[3]));
            for (size_t i = 0; i < count && kNodeHeaderBytes + (i + 1) * 8 <= page.size(); ++i) {
                const uint32_t value = read_big32(page, kNodeHeaderBytes + i * 8);
                const uint32_t key = read_big32(page, kNodeHeaderBytes + i * 8 + 4);
                entries.emplace_back(block(key), block(value));
            }
            node = read_big32(page, 4);
        }
        return entries;
    }

private:
    std::string bytes_;
    uint32_t index_ = 0;
    std::map<std::string, uint32_t> variables_;

    static bool is_leaf(std::string_view page) { return page[0] != 0 || page[1] != 0; }

    static uint32_t read_big32(std::string_view data, size_t at) {
        if (at + 4 > data.size()) return 0;
        const auto byte = [&](size_t i) { return static_cast<uint32_t>(static_cast<uint8_t>(data[at + i])); };
        return (byte(0) << 24) | (byte(1) << 16) | (byte(2) << 8) | byte(3);
    }

    uint32_t big32(size_t at) const { return read_big32(bytes_, at); }
};

template <typename T>
T little(std::string_view data, size_t at) {
    T value{};
    if (at + sizeof value <= data.size()) memcpy(&value, data.data() + at, sizeof value);
    return value;
}

// A rendition's key holds one 16-bit value per attribute, in the order KEYFORMAT lists.
std::vector<uint32_t> key_attributes(const Store& store) {
    constexpr size_t kCountOffset = 8, kAttributesOffset = 12;
    const std::string_view format = store.variable("KEYFORMAT");
    const auto count = little<uint32_t>(format, kCountOffset);
    std::vector<uint32_t> attributes;
    for (uint32_t i = 0; i < count; ++i) attributes.push_back(little<uint32_t>(format, kAttributesOffset + size_t{i} * 4));
    return attributes;
}

std::optional<uint16_t> name_identifier(const Store& store, const std::string& name) {
    constexpr size_t kPairCountOffset = 4, kPairsOffset = 6;
    for (const auto& [key, value] : store.tree("FACETKEYS")) {
        if (key != name) continue;
        const auto count = little<uint16_t>(value, kPairCountOffset);
        for (uint16_t i = 0; i < count; ++i) {
            const size_t pair = kPairsOffset + size_t{i} * 4;
            if (little<uint16_t>(value, pair) == kAttributeNameIdentifier) return little<uint16_t>(value, pair + 2);
        }
    }
    return std::nullopt;
}

const Store* main_catalog() {
    static const Store* const catalog = []() -> const Store* {
        std::ifstream file(engine_bundle_dir() + "/Assets.car", std::ios::binary);
        if (!file) return nullptr;
        auto* store = new Store(std::string(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()));
        if (store->open()) return store;
        engine_log("UIKIT: Assets.car is not a catalog this reader understands");
        delete store;
        return nullptr;
    }();
    return catalog;
}

bool parse_color(std::string_view rendition, AssetColor& out) {
    if (rendition.size() < kRenditionHeaderBytes) return false;
    const size_t tag = rendition.find(std::string_view(kColorTag, kColorTagLength), kRenditionHeaderBytes);
    if (tag == std::string_view::npos) return false;
    constexpr size_t kCountOffset = 12, kComponentsOffset = 16;
    const auto count = little<uint32_t>(rendition, tag + kCountOffset);
    if (count != 4 && count != 2) return false;
    const size_t components = tag + kComponentsOffset;
    if (components + count * sizeof(double) > rendition.size()) return false;
    if (count == 4) {
        out = {little<double>(rendition, components), little<double>(rendition, components + 8), little<double>(rendition, components + 16), little<double>(rendition, components + 24)};
    } else {
        const auto white = little<double>(rendition, components);
        out = {white, white, white, little<double>(rendition, components + 8)};
    }
    return true;
}

}  // namespace

bool asset_color(const std::string& name, bool dark, AssetColor& out) {
    const Store* store = main_catalog();
    if (store == nullptr) return false;
    const std::optional<uint16_t> identifier = name_identifier(*store, name);
    if (!identifier) return false;
    const std::vector<uint32_t> attributes = key_attributes(*store);
    bool found = false;
    for (const auto& [key, rendition] : store->tree("RENDITIONS")) {
        uint16_t appearance = 0;
        bool matches = false;
        for (size_t i = 0; i < attributes.size(); ++i) {
            const auto value = little<uint16_t>(key, i * 2);
            if (attributes[i] == kAttributeNameIdentifier) matches = value == *identifier;
            if (attributes[i] == kAttributeAppearance) appearance = value;
        }
        if (!matches) continue;
        // The default appearance stands in for one the catalog doesn't have.
        const bool wanted = appearance == (dark ? kAppearanceDark : 0);
        AssetColor color;
        if ((wanted || !found) && parse_color(rendition, color)) {
            out = color;
            found = true;
            if (wanted) return true;
        }
    }
    return found;
}

}  // namespace uikit
