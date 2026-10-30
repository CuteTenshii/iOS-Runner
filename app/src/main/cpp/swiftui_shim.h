#pragma once
#include <string_view>

#include "macho.h"

void* swiftui_symbol(std::string_view name);

// Evaluates the scene `App.body` wrote to `scene`, walking its view tree.
void swiftui_run_scene(void* scene);

// Gives the walker the app's conformance list, which it uses to find View implementations.
void swiftui_attach_image(const MachImage& image);
