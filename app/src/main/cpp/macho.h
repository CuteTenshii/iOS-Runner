#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

struct MachSection {
    std::string segment;
    std::string name;
    uintptr_t address;
    size_t size;
};

struct MachImport {
    std::string name;
    std::string library;
    bool weak;
    bool resolved;
};

struct MachImage {
    uintptr_t base = 0;
    size_t size = 0;
    intptr_t slide = 0;
    uintptr_t entry = 0;
    // The deployment target, encoded as Darwin versions are: major << 16 | minor << 8 | patch.
    uint32_t minimum_os = 0;
    std::vector<MachSection> sections;
    std::vector<MachImport> imports;

    const MachSection* section(const char* segment, const char* name) const;
};

using SymbolResolver = void* (*)(const char* name);

// Maps the arm64 Mach-O at `path` and applies its fixups. Returns false and fills `error` on failure.
bool load_macho(const std::string& path, SymbolResolver resolve, MachImage& image, std::string& error);

// Names the unresolved import owning `address`, or nullptr if it is outside the guard region.
const char* unresolved_symbol_at(uintptr_t address);

// The guard address standing in for the unresolved import `name`, or nullptr if it was not imported.
const void* unresolved_symbol_named(const char* name);

// True if `address` lies in a writable segment of the loaded image.
bool image_address_is_writable(uintptr_t address);
