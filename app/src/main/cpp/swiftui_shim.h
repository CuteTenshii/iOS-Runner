#pragma once
#include <string>
#include <string_view>

#include "macho.h"

void* swiftui_symbol(std::string_view name);

// Evaluates the scene `App.body` wrote to `scene`, walking its view tree.
void swiftui_run_scene(void* scene);

// Gives the walker the app's conformance list, which it uses to find View implementations.
void swiftui_attach_image(const MachImage& image);

// Writes the text a field finished editing to its binding. Runs on the guest thread.
void swiftui_set_text(int action, const std::string& text);
