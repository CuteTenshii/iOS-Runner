#pragma once
#include "macho.h"

// Looks up a guest import (without its leading underscore) in the compatibility runtime.
void* resolve_symbol(const char* name);

// Registers every loaded image with the runtime; the last is the app, whose deployment target is the one reported.
void runtime_attach_images(const std::vector<std::unique_ptr<MachImage>>& images);
