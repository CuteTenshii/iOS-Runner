#pragma once
#include <string_view>

// UIKit for apps that start through UIApplicationMain. SwiftUI apps never enter it, and keep nil answers from UI classes.
void* uikit_symbol(std::string_view name);
// Whether a framework class is, or descends from, another; only classes this layer draws have known parents.
bool uikit_class_inherits(const std::string& framework_class, const std::string& ancestor);
void uikit_register_classes();
