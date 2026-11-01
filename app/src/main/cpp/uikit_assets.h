#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace uikit {

struct AssetColor {
    double red = 0, green = 0, blue = 0, alpha = 0;
};

// Looks a named color up in the app's compiled asset catalog, returning false if it has none.
bool asset_color(const std::string& name, bool dark, AssetColor& out);

struct AssetImage {
    uint32_t width = 0, height = 0, scale = 1;
    // Premultiplied RGBA, or for a format only the host decodes, its file and the part of it to draw.
    std::vector<uint8_t> rgba;
    std::string encoded;
    uint32_t crop_x = 0, crop_y = 0;
    // Gray and alpha only, which is what a template image compiles to.
    bool alpha_only = false;
};

bool asset_image_exists(const std::string& name);
// Decodes the best version of a named image: the wanted appearance first, then the highest scale.
bool asset_image(const std::string& name, bool dark, AssetImage& out);

}  // namespace uikit
