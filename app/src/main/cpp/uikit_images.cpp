#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <map>
#include <string>
#include <unordered_map>
#include <vector>

#include "engine.h"
#include "foundation_internal.h"
#include "uikit_assets.h"
#include "uikit_internal.h"

using foundation::NSArrayObject;
using foundation::nsstring_text;

namespace uikit {

namespace {

constexpr const char* kSourceKeys[] = {"assetName", "resourceName", "systemName", "systemSymbolResourceName", "filePath", "imageData"};
constexpr int64_t kRenderingOriginal = 1, kRenderingTemplate = 2;

struct CGSize {
    double width, height;
};

// What a catalog image became on disk, so that drawing it again doesn't decode it again.
struct StoredImage {
    bool loaded = false;
    std::string file;
    uint32_t width = 0, height = 0, scale = 1;
    uint32_t crop_x = 0, crop_y = 0, crop_width = 0, crop_height = 0;
    bool alpha_only = false;
};

std::string image_directory() {
    const std::string directory = engine_bundle_dir() + "/.tmp/images";
    std::error_code ignored;
    std::filesystem::create_directories(directory, ignored);
    return directory;
}

std::string safe_file_name(const std::string& name) {
    std::string safe;
    for (const char c : name) safe += isalnum(static_cast<unsigned char>(c)) != 0 ? c : '_';
    return safe + "-" + std::to_string(std::hash<std::string>{}(name));
}

const StoredImage& stored_asset(const std::string& name, bool dark) {
    static std::map<std::string, StoredImage> stored;
    const std::string key = name + (dark ? "|dark" : "|light");
    StoredImage& image = stored[key];
    if (image.loaded) return image;
    image.loaded = true;
    AssetImage asset;
    if (!asset_image(name, dark, asset)) return image;
    image.width = asset.width;
    image.height = asset.height;
    image.scale = asset.scale > 0 ? asset.scale : 1;
    image.alpha_only = asset.alpha_only;
    const std::string base = image_directory() + "/" + safe_file_name(key);
    if (!asset.encoded.empty()) {
        image.file = base + ".encoded";
        image.crop_x = asset.crop_x;
        image.crop_y = asset.crop_y;
        image.crop_width = asset.width;
        image.crop_height = asset.height;
        std::ofstream(image.file, std::ios::binary) << asset.encoded;
        return image;
    }
    image.file = base + ".rgba";
    std::ofstream file(image.file, std::ios::binary);
    file.write(reinterpret_cast<const char*>(&asset.width), sizeof asset.width);
    file.write(reinterpret_cast<const char*>(&asset.height), sizeof asset.height);
    file.write(reinterpret_cast<const char*>(asset.rgba.data()), static_cast<std::streamsize>(asset.rgba.size()));
    return image;
}

// The file imageNamed: finds for a loose image in the bundle, highest scale first, with its scale.
std::string bundle_image_path(const std::string& name, uint32_t& scale) {
    static const std::vector<std::pair<std::string, uint32_t>> kSuffixes = {{"@3x", 3}, {"@2x", 2}, {"", 1}};
    const std::filesystem::path relative(name);
    const std::string stem = relative.has_extension() ? relative.stem().string() : name;
    const std::string extension = relative.has_extension() ? relative.extension().string() : ".png";
    for (const auto& [suffix, suffix_scale] : kSuffixes) {
        const std::string path = engine_bundle_dir() + "/" + stem + suffix + extension;
        if (std::filesystem::is_regular_file(path)) {
            scale = suffix_scale;
            return path;
        }
    }
    return {};
}

void* image_with(const char* key, void* value) {
    void* image = objc_framework_object("UIImage");
    property_set_object(image, key, value);
    return image;
}

void* image_named(void*, const char*, void* name) {
    if (name == nullptr) return nullptr;
    const std::string text = nsstring_text(name);
    uint32_t scale = 1;
    if (asset_image_exists(text) || !bundle_image_path(text, scale).empty()) return image_with("assetName", name);
    return nullptr;
}
void* image_named_in_bundle(void* self, const char* selector, void* name, void*, void*) { return image_named(self, selector, name); }
void* system_image_named(void*, const char*, void* name) { return name != nullptr ? image_with("systemName", name) : nullptr; }
void* system_image_with_configuration(void* self, const char* selector, void* name, void*) { return system_image_named(self, selector, name); }

void* image_from_data(void*, const char*, void* data) { return data != nullptr ? image_with("imageData", data) : nullptr; }
void* image_from_data_scale(void* self, const char* selector, void* data, double) { return image_from_data(self, selector, data); }
void* image_from_file(void*, const char*, void* path) {
    if (path == nullptr || !std::filesystem::is_regular_file(nsstring_text(path))) return nullptr;
    return image_with("filePath", path);
}
void* init_with_data(void* self, const char*, void* data) {
    if (data == nullptr) return nullptr;
    property_set_object(self, "imageData", data);
    return self;
}
void* init_with_data_scale(void* self, const char* selector, void* data, double) { return init_with_data(self, selector, data); }
void* init_with_file(void* self, const char*, void* path) {
    if (path == nullptr || !std::filesystem::is_regular_file(nsstring_text(path))) return nullptr;
    property_set_object(self, "filePath", path);
    return self;
}

void* copy_image(void* source) {
    void* copy = objc_framework_object("UIImage");
    for (const char* key : kSourceKeys) {
        if (void* value = property_get_object(source, key)) property_set_object(copy, key, value);
    }
    property_set_word(copy, "renderingMode", property_get_word(source, "renderingMode"));
    if (void* tint = property_get_object(source, "tintColor")) property_set_object(copy, "tintColor", tint);
    return copy;
}

void* image_with_rendering_mode(void* self, const char*, int64_t mode) {
    void* copy = copy_image(self);
    property_set_word(copy, "renderingMode", mode);
    return copy;
}
void* image_with_tint(void* self, const char*, void* color) {
    void* copy = copy_image(self);
    property_set_object(copy, "tintColor", color);
    property_set_word(copy, "renderingMode", kRenderingTemplate);
    return copy;
}
void* image_with_tint_mode(void* self, const char* selector, void* color, int64_t) { return image_with_tint(self, selector, color); }
void* image_unchanged(void* self, const char*) { return self; }
void* image_unchanged_with(void* self, const char*, void*) { return self; }
int64_t image_rendering_mode(void* self, const char*) { return property_get_word(self, "renderingMode"); }

std::string first_text(void* image, const char* first, const char* second) {
    void* value = property_get_object(image, first);
    if (value == nullptr) value = property_get_object(image, second);
    return value != nullptr ? nsstring_text(value) : std::string();
}

uint32_t big_endian32(const unsigned char* bytes) { return (uint32_t{bytes[0]} << 24) | (uint32_t{bytes[1]} << 16) | (uint32_t{bytes[2]} << 8) | bytes[3]; }

// A PNG's or JPEG's own header, as UIImage knows its size before anything draws it.
bool encoded_size(const std::string& bytes, double& width, double& height) {
    const auto* data = reinterpret_cast<const unsigned char*>(bytes.data());
    constexpr size_t kPngWidthOffset = 16, kPngHeaderBytes = 24;
    constexpr unsigned char kPngFirstByte = 0x89, kJpegMarker = 0xFF, kJpegStart = 0xD8;
    if (bytes.size() >= kPngHeaderBytes && data[0] == kPngFirstByte) {
        width = big_endian32(data + kPngWidthOffset);
        height = big_endian32(data + kPngWidthOffset + 4);
        return true;
    }
    if (bytes.size() < 4 || data[0] != kJpegMarker || data[1] != kJpegStart) return false;
    constexpr unsigned char kFirstFrameMarker = 0xC0, kLastFrameMarker = 0xCF, kHuffmanMarker = 0xC4, kArithmeticMarker = 0xCC, kLosslessMarker = 0xC8;
    for (size_t at = 2; at + 9 < bytes.size();) {
        if (data[at] != kJpegMarker) return false;
        const unsigned char marker = data[at + 1];
        const size_t length = (size_t{data[at + 2]} << 8) | data[at + 3];
        const bool frame = marker >= kFirstFrameMarker && marker <= kLastFrameMarker && marker != kHuffmanMarker && marker != kLosslessMarker && marker != kArithmeticMarker;
        if (frame) {
            height = (uint32_t{data[at + 5]} << 8) | data[at + 6];
            width = (uint32_t{data[at + 7]} << 8) | data[at + 8];
            return true;
        }
        at += 2 + length;
    }
    return false;
}

std::string read_file(const std::string& path) {
    std::ifstream file(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
}

std::string image_bytes(void* image) {
    if (void* data = property_get_object(image, "imageData")) return *static_cast<foundation::NSDataObject*>(data)->bytes;
    if (void* path = property_get_object(image, "filePath")) return read_file(nsstring_text(path));
    return {};
}

CGSize image_size(void* self, const char*) {
    const std::string asset = first_text(self, "assetName", "resourceName");
    double width = 0, height = 0;
    if (!asset.empty()) {
        uint32_t scale = 1;
        const std::string path = asset_image_exists(asset) ? std::string() : bundle_image_path(asset, scale);
        if (!path.empty()) {
            encoded_size(read_file(path), width, height);
            return {width / scale, height / scale};
        }
        const StoredImage& stored = stored_asset(asset, false);
        return {static_cast<double>(stored.width) / stored.scale, static_cast<double>(stored.height) / stored.scale};
    }
    encoded_size(image_bytes(self), width, height);
    return {width, height};
}

double image_scale(void* self, const char*) {
    const std::string asset = first_text(self, "assetName", "resourceName");
    return asset.empty() || !asset_image_exists(asset) ? 1 : stored_asset(asset, false).scale;
}

void* button_content(void* view) {
    void* stateful = property_get_object(view, "buttonStatefulContent");
    if (stateful == nullptr) return nullptr;
    // A nib stores a dictionary as its keys and values in turn, and the normal state comes first.
    const auto& pairs = *static_cast<NSArrayObject*>(stateful)->items;
    return pairs.size() >= 2 ? pairs[1] : nullptr;
}

}  // namespace

void* image_of_view(void* view) {
    if (object_is_kind(view, "UIImageView")) return property_get_object(view, "image");
    if (!object_is_kind(view, "UIButton")) return nullptr;
    if (void* image = property_get_object(view, "buttonImage")) return image;
    if (void* configuration = property_get_object(view, "swiftConfiguration")) {
        if (void* image = property_get_object(configuration, "image")) return image;
    }
    if (void* configuration = property_get_object(view, "buttonConfiguration")) {
        if (void* image = property_get_object(configuration, "buttonConfigurationImage")) return image;
    }
    void* content = button_content(view);
    return content != nullptr ? property_get_object(content, "image") : nullptr;
}

std::string button_nib_title(void* view) {
    if (void* configuration = property_get_object(view, "buttonConfiguration")) {
        if (void* title = property_get_object(configuration, "buttonConfigurationTitle")) return nsstring_text(title);
    }
    void* content = button_content(view);
    void* title = content != nullptr ? property_get_object(content, "title") : nullptr;
    return title != nullptr ? nsstring_text(title) : std::string();
}

bool describe_image(void* image, bool dark, ImageDescription& out) {
    if (image == nullptr) return false;
    const int64_t mode = property_get_word(image, "renderingMode");
    out.template_image = mode == kRenderingTemplate;
    out.symbol = first_text(image, "systemName", "systemSymbolResourceName");
    if (!out.symbol.empty()) return true;

    const std::string asset = first_text(image, "assetName", "resourceName");
    if (!asset.empty() && asset_image_exists(asset)) {
        const StoredImage& stored = stored_asset(asset, dark);
        if (stored.file.empty()) return false;
        out.file = stored.file;
        out.width = static_cast<double>(stored.width) / stored.scale;
        out.height = static_cast<double>(stored.height) / stored.scale;
        out.crop_x = stored.crop_x;
        out.crop_y = stored.crop_y;
        out.crop_width = stored.crop_width;
        out.crop_height = stored.crop_height;
        if (mode != kRenderingOriginal && stored.alpha_only) out.template_image = true;
        return true;
    }
    if (!asset.empty()) {
        uint32_t scale = 1;
        out.file = bundle_image_path(asset, scale);
        if (out.file.empty()) return false;
        encoded_size(read_file(out.file), out.width, out.height);
        out.width /= scale;
        out.height /= scale;
        return true;
    }
    if (void* path = property_get_object(image, "filePath")) {
        out.file = nsstring_text(path);
    } else if (void* data = property_get_object(image, "imageData")) {
        const std::string& bytes = *static_cast<foundation::NSDataObject*>(data)->bytes;
        out.file = image_directory() + "/data-" + std::to_string(std::hash<std::string>{}(bytes)) + ".encoded";
        if (!std::filesystem::exists(out.file)) std::ofstream(out.file, std::ios::binary) << bytes;
    } else {
        return false;
    }
    encoded_size(image_bytes(image), out.width, out.height);
    return true;
}

void register_image_classes() {
    objc_register_method("+[UIImage imageNamed:]", reinterpret_cast<const void*>(image_named));
    objc_register_method("+[UIImage imageNamed:inBundle:compatibleWithTraitCollection:]", reinterpret_cast<const void*>(image_named_in_bundle));
    objc_register_method("+[UIImage imageNamed:inBundle:withConfiguration:]", reinterpret_cast<const void*>(image_named_in_bundle));
    objc_register_method("+[UIImage systemImageNamed:]", reinterpret_cast<const void*>(system_image_named));
    objc_register_method("+[UIImage systemImageNamed:withConfiguration:]", reinterpret_cast<const void*>(system_image_with_configuration));
    objc_register_method("+[UIImage imageWithData:]", reinterpret_cast<const void*>(image_from_data));
    objc_register_method("+[UIImage imageWithData:scale:]", reinterpret_cast<const void*>(image_from_data_scale));
    objc_register_method("+[UIImage imageWithContentsOfFile:]", reinterpret_cast<const void*>(image_from_file));
    objc_register_method("-[UIImage initWithData:]", reinterpret_cast<const void*>(init_with_data));
    objc_register_method("-[UIImage initWithData:scale:]", reinterpret_cast<const void*>(init_with_data_scale));
    objc_register_method("-[UIImage initWithContentsOfFile:]", reinterpret_cast<const void*>(init_with_file));
    for (const std::string cls : {"UIImage", "UIImageNibPlaceholder"}) {
        const auto add = [&cls](const char* selector, const void* implementation) { objc_register_method(("-[" + cls + " " + selector + "]").c_str(), implementation); };
        add("imageWithRenderingMode:", reinterpret_cast<const void*>(image_with_rendering_mode));
        add("imageWithTintColor:", reinterpret_cast<const void*>(image_with_tint));
        add("imageWithTintColor:renderingMode:", reinterpret_cast<const void*>(image_with_tint_mode));
        add("imageFlippedForRightToLeftLayoutDirection", reinterpret_cast<const void*>(image_unchanged));
        add("imageWithoutBaseline", reinterpret_cast<const void*>(image_unchanged));
        add("imageWithConfiguration:", reinterpret_cast<const void*>(image_unchanged_with));
        add("imageByApplyingSymbolConfiguration:", reinterpret_cast<const void*>(image_unchanged_with));
        add("renderingMode", reinterpret_cast<const void*>(image_rendering_mode));
        add("size", reinterpret_cast<const void*>(image_size));
        add("scale", reinterpret_cast<const void*>(image_scale));
    }
}

}  // namespace uikit
