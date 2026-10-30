#pragma once
#include <string_view>

void* swiftui_symbol(std::string_view name);

// Evaluates the scene `App.body` wrote to `scene`, walking its view tree.
void swiftui_run_scene(void* scene);
