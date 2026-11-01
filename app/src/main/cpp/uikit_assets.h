#pragma once
#include <string>

namespace uikit {

struct AssetColor {
    double red = 0, green = 0, blue = 0, alpha = 0;
};

// Looks a named color up in the app's compiled asset catalog, returning false if it has none.
bool asset_color(const std::string& name, bool dark, AssetColor& out);

}  // namespace uikit
