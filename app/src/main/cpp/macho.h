#pragma once
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

struct MachSection {
    std::string segment;
    std::string name;
    uintptr_t address;
    size_t size;
    uint32_t flags;
};

struct MachImport {
    std::string name;
    std::string library;
    bool weak;
    bool resolved;
};

struct MachImage {
    std::string path;
    std::string name;
    uintptr_t base = 0;
    size_t size = 0;
    intptr_t slide = 0;
    uintptr_t entry = 0;
    // The deployment target, encoded as Darwin versions are: major << 16 | minor << 8 | patch.
    uint32_t minimum_os = 0;
    std::vector<MachSection> sections;
    std::vector<MachImport> imports;
    // Parallel to the load commands' libraries: null for those the shims stand in for.
    std::vector<MachImage*> dependencies;
    const uint8_t* exports = nullptr;
    size_t exports_size = 0;

    const MachSection* section(const char* segment, const char* name) const;
};

using SymbolResolver = void* (*)(const char* name);

// Maps the executable at `path` and the libraries it bundles, and applies their fixups.
// `images` ends up in initialization order, dependencies first, so the executable is last.
bool load_macho_graph(const std::string& path, SymbolResolver resolve, std::vector<std::unique_ptr<MachImage>>& images, std::string& error);

// Finds an exported symbol, named without its leading underscore, in any of the images.
void* macho_find_export(const std::vector<std::unique_ptr<MachImage>>& images, const char* name);

// Runs the image's C++ static initializers and other load-time functions.
void run_image_initializers(const MachImage& image, int argc, const char** argv, const char** envp, const char** apple);

// Names the unresolved import owning `address`, or nullptr if it is outside the guard region.
const char* unresolved_symbol_at(uintptr_t address);

// The guard address standing in for the unresolved import `name`, or nullptr if it was not imported.
const void* unresolved_symbol_named(const char* name);

// True if `address` lies in a writable segment of the loaded image.
bool image_address_is_writable(uintptr_t address);
