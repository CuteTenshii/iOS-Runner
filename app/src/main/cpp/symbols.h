#pragma once
#include "macho.h"

// Looks up a guest import (without its leading underscore) in the compatibility runtime.
void* resolve_symbol(const char* name);

void runtime_attach_image(const MachImage& image);
