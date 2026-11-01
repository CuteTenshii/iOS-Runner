#pragma once
#include <string_view>

// UIKit for apps that start through UIApplicationMain. SwiftUI apps never enter it, and keep nil answers from UI classes.
void* uikit_symbol(std::string_view name);
void uikit_register_classes();
