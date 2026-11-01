#include "uikit_assets.h"

#include <zlib.h>

#include <algorithm>
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
#include "third_party/astcenc/astcenc.h"
#include "third_party/lzfse/lzfse.h"

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

constexpr uint32_t kAttributeScale = 12;
constexpr uint32_t kCompressionGzip = 2, kCompressionAstc = 7, kCompressionHeif = 9;
constexpr uint16_t kLayoutInternalLink = 1003;
constexpr uint32_t kLinkTag = 1010;
constexpr size_t kPixelFormatOffset = 24, kWidthOffset = 12, kHeightOffset = 16, kScaleOffset = 20, kLayoutOffset = 36, kTlvLengthOffset = 168;
constexpr uint32_t kPercentPerScale = 100;
constexpr std::string_view kStoredGrayAlpha = " 8AG";

struct Bitmap {
    uint32_t width = 0, height = 0;
    std::vector<uint8_t> rgba;
    // A format the host's decoders read, used when the catalog holds no pixels this engine can decode.
    std::string encoded;
    bool alpha_only = false;
};

bool inflate_gzip(std::string_view input, std::string& out, size_t expected) {
    constexpr int kGzipWindowBits = 15 + 16;
    z_stream stream{};
    if (inflateInit2(&stream, kGzipWindowBits) != Z_OK) return false;
    const size_t start = out.size();
    out.resize(start + expected);
    stream.next_in = reinterpret_cast<Bytef*>(const_cast<char*>(input.data()));
    stream.avail_in = static_cast<uInt>(input.size());
    stream.next_out = reinterpret_cast<Bytef*>(out.data() + start);
    stream.avail_out = static_cast<uInt>(expected);
    const int result = inflate(&stream, Z_FINISH);
    inflateEnd(&stream);
    out.resize(start + stream.total_out);
    return result == Z_STREAM_END;
}

// Each tile is 'KCBC', two zero words, its row count, its byte count, then a gzip stream.
bool read_gzip_tiles(std::string_view body, uint32_t tile_count, uint32_t height, size_t bytes_per_pixel, uint32_t width, std::string& out) {
    constexpr size_t kTileHeaderBytes = 20, kTileRowsOffset = 12, kTileLengthOffset = 16;
    size_t at = 0;
    for (uint32_t i = 0; i < tile_count; ++i) {
        if (at + kTileHeaderBytes > body.size() || body.compare(at, 4, "KCBC") != 0) return false;
        const auto rows = little<uint32_t>(body, at + kTileRowsOffset);
        const auto length = little<uint32_t>(body, at + kTileLengthOffset);
        if (at + kTileHeaderBytes + length > body.size()) return false;
        if (!inflate_gzip(body.substr(at + kTileHeaderBytes, length), out, size_t{rows} * width * bytes_per_pixel)) return false;
        at += kTileHeaderBytes + length;
    }
    return out.size() == size_t{height} * width * bytes_per_pixel;
}

void to_rgba(const std::string& pixels, std::string_view format, Bitmap& bitmap) {
    const size_t count = size_t{bitmap.width} * bitmap.height;
    bitmap.rgba.resize(count * 4);
    const auto* from = reinterpret_cast<const uint8_t*>(pixels.data());
    uint8_t* to = bitmap.rgba.data();
    for (size_t i = 0; i < count; ++i, to += 4) {
        if (format == "ARGB") {
            to[0] = from[i * 4 + 2];
            to[1] = from[i * 4 + 1];
            to[2] = from[i * 4];
            to[3] = from[i * 4 + 3];
        } else if (format == "GA8 ") {
            to[0] = to[1] = to[2] = from[i * 2];
            to[3] = from[i * 2 + 1];
        } else {
            uint16_t pixel;
            memcpy(&pixel, from + i * 2, sizeof pixel);
            constexpr uint32_t kFiveBitMax = 31, kByteMax = 255;
            to[0] = static_cast<uint8_t>(((pixel >> 10) & kFiveBitMax) * kByteMax / kFiveBitMax);
            to[1] = static_cast<uint8_t>(((pixel >> 5) & kFiveBitMax) * kByteMax / kFiveBitMax);
            to[2] = static_cast<uint8_t>((pixel & kFiveBitMax) * kByteMax / kFiveBitMax);
            to[3] = static_cast<uint8_t>(kByteMax);
        }
    }
}

// An LZFSE stream holding an ASTC file: its 16-byte header gives the block size, and blocks follow.
bool read_astc(std::string_view body, Bitmap& bitmap) {
    constexpr size_t kPrefixBytes = 12, kExpandedSizeOffset = 8, kAstcHeaderBytes = 16;
    if (body.size() < kPrefixBytes) return false;
    const auto expanded_size = little<uint32_t>(body, kExpandedSizeOffset);
    std::vector<uint8_t> expanded(expanded_size);
    const size_t written = lzfse_decode_buffer(expanded.data(), expanded.size(), reinterpret_cast<const uint8_t*>(body.data()) + kPrefixBytes, body.size() - kPrefixBytes, nullptr);
    if (written < kAstcHeaderBytes) return false;
    const uint32_t block_x = expanded[4], block_y = expanded[5];
    astcenc_config config{};
    if (astcenc_config_init(ASTCENC_PRF_LDR, block_x, block_y, 1, ASTCENC_PRE_FASTEST, ASTCENC_FLG_DECOMPRESS_ONLY, &config) != ASTCENC_SUCCESS) return false;
    astcenc_context* context = nullptr;
    if (astcenc_context_alloc(&config, 1, &context) != ASTCENC_SUCCESS) return false;
    bitmap.rgba.resize(size_t{bitmap.width} * bitmap.height * 4);
    void* slices[] = {bitmap.rgba.data()};
    astcenc_image image{bitmap.width, bitmap.height, 1, ASTCENC_TYPE_U8, slices};
    const astcenc_swizzle swizzle{ASTCENC_SWZ_B, ASTCENC_SWZ_G, ASTCENC_SWZ_R, ASTCENC_SWZ_A};
    const astcenc_error result = astcenc_decompress_image(context, expanded.data() + kAstcHeaderBytes, written - kAstcHeaderBytes, &image, &swizzle, 0);
    astcenc_context_free(context);
    return result == ASTCENC_SUCCESS;
}

bool decode_pixels(std::string_view rendition, Bitmap& bitmap) {
    constexpr size_t kBitmapHeaderBytes = 16, kCompressionOffset = 8, kCountOffset = 12;
    const auto tlv_length = little<uint32_t>(rendition, kTlvLengthOffset);
    const size_t body_start = kRenditionHeaderBytes + tlv_length;
    if (body_start + kBitmapHeaderBytes > rendition.size() || rendition.compare(body_start, 4, "MLEC") != 0) return false;
    const auto compression = little<uint32_t>(rendition, body_start + kCompressionOffset);
    const auto count = little<uint32_t>(rendition, body_start + kCountOffset);
    const std::string_view body = rendition.substr(body_start + kBitmapHeaderBytes);
    // The pixel format is a four-character code stored backwards.
    const std::string_view stored_format = rendition.substr(kPixelFormatOffset, 4);
    const std::string format(stored_format.rbegin(), stored_format.rend());
    bitmap.width = little<uint32_t>(rendition, kWidthOffset);
    bitmap.height = little<uint32_t>(rendition, kHeightOffset);
    bitmap.alpha_only = format == "GA8 ";
    if (compression == kCompressionGzip) {
        const size_t bytes_per_pixel = format == "ARGB" ? 4 : 2;
        std::string pixels;
        if (!read_gzip_tiles(body, count, bitmap.height, bytes_per_pixel, bitmap.width, pixels)) return false;
        to_rgba(pixels, format, bitmap);
        return true;
    }
    if (compression == kCompressionAstc) return read_astc(body, bitmap);
    if (compression == kCompressionHeif) {
        constexpr size_t kHeifPrefixBytes = 8;
        if (body.size() < kHeifPrefixBytes) return false;
        bitmap.encoded = std::string(body.substr(kHeifPrefixBytes));
        return true;
    }
    return false;
}

struct AssetLink {
    uint32_t x, y, width, height;
    std::vector<std::pair<uint16_t, uint16_t>> key;
};

std::optional<AssetLink> link_of(std::string_view rendition) {
    constexpr size_t kMagicAndZeroBytes = 8, kRectBytes = 16, kLayoutAndLengthBytes = 4, kReservedBytes = 2;
    const auto tlv_length = little<uint32_t>(rendition, kTlvLengthOffset);
    size_t at = kRenditionHeaderBytes;
    const size_t end = std::min(kRenditionHeaderBytes + tlv_length, rendition.size());
    while (at + 8 <= end) {
        const auto tag = little<uint32_t>(rendition, at);
        const auto length = little<uint32_t>(rendition, at + 4);
        if (tag == kLinkTag) {
            const size_t value = at + 8;
            AssetLink link{little<uint32_t>(rendition, value + kMagicAndZeroBytes), little<uint32_t>(rendition, value + kMagicAndZeroBytes + 4), little<uint32_t>(rendition, value + kMagicAndZeroBytes + 8), little<uint32_t>(rendition, value + kMagicAndZeroBytes + 12), {}};
            const size_t key_header = value + kMagicAndZeroBytes + kRectBytes;
            const auto key_bytes = little<uint16_t>(rendition, key_header + 2);
            for (size_t pair = key_header + kLayoutAndLengthBytes + kReservedBytes; pair + 4 <= key_header + kLayoutAndLengthBytes + key_bytes; pair += 4) {
                link.key.emplace_back(little<uint16_t>(rendition, pair), little<uint16_t>(rendition, pair + 2));
            }
            return link;
        }
        at += 8 + length;
    }
    return std::nullopt;
}

// The atlas a link points into: every attribute the link doesn't list is zero, including the name.
std::string_view find_atlas(const Store& store, const std::vector<uint32_t>& attributes, const AssetLink& link) {
    for (const auto& [key, rendition] : store.tree("RENDITIONS")) {
        bool matches = true;
        for (size_t i = 0; i < attributes.size() && matches; ++i) {
            uint16_t wanted = 0;
            for (const auto& [attribute, value] : link.key) {
                if (attribute == attributes[i]) wanted = value;
            }
            matches = little<uint16_t>(key, i * 2) == wanted;
        }
        if (matches) return rendition;
    }
    return {};
}

// A link's rectangle has its origin at the atlas's bottom left, as Core Graphics does.
uint32_t top_of(const AssetLink& link, uint32_t atlas_height) { return atlas_height - link.y - link.height; }

bool crop(const Bitmap& atlas, const AssetLink& link, Bitmap& out) {
    if (link.x + link.width > atlas.width || link.y + link.height > atlas.height) return false;
    const uint32_t top = top_of(link, atlas.height);
    out.width = link.width;
    out.height = link.height;
    out.alpha_only = atlas.alpha_only;
    out.rgba.resize(size_t{link.width} * link.height * 4);
    for (uint32_t row = 0; row < link.height; ++row) {
        memcpy(out.rgba.data() + size_t{row} * link.width * 4, atlas.rgba.data() + (size_t{top + row} * atlas.width + link.x) * 4, size_t{link.width} * 4);
    }
    return true;
}

}  // namespace

bool asset_image_exists(const std::string& name) {
    const Store* store = main_catalog();
    return store != nullptr && name_identifier(*store, name).has_value();
}

bool asset_image(const std::string& name, bool dark, AssetImage& out) {
    const Store* store = main_catalog();
    if (store == nullptr) return false;
    const std::optional<uint16_t> identifier = name_identifier(*store, name);
    if (!identifier) return false;
    const std::vector<uint32_t> attributes = key_attributes(*store);

    struct Candidate {
        int score;
        std::string_view rendition;
    };
    std::vector<Candidate> candidates;
    for (const auto& [key, rendition] : store->tree("RENDITIONS")) {
        bool matches = false;
        uint16_t appearance = 0, scale = 0;
        for (size_t i = 0; i < attributes.size(); ++i) {
            const auto value = little<uint16_t>(key, i * 2);
            if (attributes[i] == kAttributeNameIdentifier) matches = value == *identifier;
            if (attributes[i] == kAttributeAppearance) appearance = value;
            if (attributes[i] == kAttributeScale) scale = value;
        }
        if (!matches || rendition.size() < kRenditionHeaderBytes) continue;
        constexpr int kAppearanceWeight = 100;
        const bool wanted_appearance = appearance == (dark ? kAppearanceDark : 0);
        if (!wanted_appearance && appearance != 0) continue;
        candidates.push_back({(wanted_appearance ? kAppearanceWeight : 0) + scale, rendition});
    }
    std::stable_sort(candidates.begin(), candidates.end(), [](const Candidate& a, const Candidate& b) { return a.score > b.score; });

    // Atlases serve many images, and one image may ask for several versions of itself below.
    static std::map<const char*, Bitmap> atlases;
    for (const Candidate& candidate : candidates) {
        const std::string_view rendition = candidate.rendition;
        Bitmap bitmap;
        if (little<uint16_t>(rendition, kLayoutOffset) == kLayoutInternalLink) {
            const std::optional<AssetLink> link = link_of(rendition);
            if (!link) continue;
            const std::string_view atlas_rendition = find_atlas(*store, attributes, *link);
            if (atlas_rendition.empty()) continue;
            auto atlas = atlases.find(atlas_rendition.data());
            if (atlas == atlases.end()) {
                Bitmap decoded;
                if (!decode_pixels(atlas_rendition, decoded)) continue;
                atlas = atlases.emplace(atlas_rendition.data(), std::move(decoded)).first;
            }
            if (!atlas->second.encoded.empty()) {
                out.encoded = atlas->second.encoded;
                out.crop_x = link->x;
                out.crop_y = top_of(*link, atlas->second.height);
                out.width = link->width;
                out.height = link->height;
                out.scale = little<uint32_t>(rendition, kScaleOffset) / kPercentPerScale;
                out.alpha_only = rendition.substr(kPixelFormatOffset, 4) == kStoredGrayAlpha;
                return true;
            }
            if (!crop(atlas->second, *link, bitmap)) continue;
        } else if (!decode_pixels(rendition, bitmap)) {
            continue;
        }
        out.width = bitmap.width;
        out.height = bitmap.height;
        out.scale = little<uint32_t>(rendition, kScaleOffset) / kPercentPerScale;
        out.alpha_only = rendition.substr(kPixelFormatOffset, 4) == kStoredGrayAlpha;
        out.rgba = std::move(bitmap.rgba);
        out.encoded = std::move(bitmap.encoded);
        return true;
    }
    engine_log("UIKIT: no rendition of image \"%s\" could be decoded", name.c_str());
    return false;
}

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
