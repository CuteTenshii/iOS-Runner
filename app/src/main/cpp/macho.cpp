#include "macho.h"

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cstdio>
#include <cstring>
#include <utility>

namespace {

constexpr uint32_t kMagic64 = 0xfeedfacf;
constexpr uint32_t kFatMagicSwapped = 0xbebafeca;
constexpr uint32_t kCpuArm64 = 0x0100000c;
constexpr uint32_t kCpuSubtypeMask = 0x00ffffff;
constexpr uint32_t kLcReqDyld = 0x80000000;
constexpr uint32_t kLcSegment64 = 0x19;
constexpr uint32_t kLcLoadDylib = 0x0c;
constexpr uint32_t kLcLoadWeakDylib = 0x18 | kLcReqDyld;
constexpr uint32_t kLcReexportDylib = 0x1f | kLcReqDyld;
constexpr uint32_t kLcLazyLoadDylib = 0x20;
constexpr uint32_t kLcUpwardDylib = 0x23 | kLcReqDyld;
constexpr uint32_t kLcEncryptionInfo64 = 0x2c;
constexpr uint32_t kLcMain = 0x28 | kLcReqDyld;
constexpr uint32_t kLcDyldInfo = 0x22;
constexpr uint32_t kLcDyldInfoOnly = 0x22 | kLcReqDyld;
constexpr uint32_t kLcChainedFixups = 0x34 | kLcReqDyld;

constexpr size_t kMachPageSize = 0x4000;
constexpr size_t kGuardSpacing = 0x1000;
constexpr size_t kGuardSlots = 0x4000;

struct MachHeader64 { uint32_t magic, cputype, cpusubtype, filetype, ncmds, sizeofcmds, flags, reserved; };
struct LoadCommand { uint32_t cmd, cmdsize; };
struct SegmentCommand64 {
    uint32_t cmd, cmdsize;
    char segname[16];
    uint64_t vmaddr, vmsize, fileoff, filesize;
    int32_t maxprot, initprot;
    uint32_t nsects, flags;
};
struct Section64 {
    char sectname[16], segname[16];
    uint64_t addr, size;
    uint32_t offset, align, reloff, nreloc, flags, reserved1, reserved2, reserved3;
};
struct DylibCommand { uint32_t cmd, cmdsize, name_offset, timestamp, current_version, compat_version; };
struct EntryPointCommand { uint32_t cmd, cmdsize; uint64_t entryoff, stacksize; };
struct EncryptionCommand64 { uint32_t cmd, cmdsize, cryptoff, cryptsize, cryptid, pad; };
struct LinkeditDataCommand { uint32_t cmd, cmdsize, dataoff, datasize; };
struct FatHeader { uint32_t magic, nfat_arch; };
struct FatArch { uint32_t cputype, cpusubtype, offset, size, align; };

struct ChainedFixupsHeader {
    uint32_t fixups_version, starts_offset, imports_offset, symbols_offset, imports_count, imports_format, symbols_format;
};
struct ChainedStartsInSegment {
    uint32_t size;
    uint16_t page_size, pointer_format;
    uint64_t segment_offset;
    uint32_t max_valid_pointer;
    uint16_t page_count;
    uint16_t page_start[1];
};

constexpr uint16_t kStartNone = 0xffff;
constexpr uint16_t kStartMulti = 0x8000;
constexpr uint16_t kStartLast = 0x8000;
constexpr uint16_t kFormatPtr64 = 2;
constexpr uint16_t kFormatPtr64Offset = 6;

constexpr uint32_t kImportFormatPlain = 1;
constexpr uint32_t kImportFormatAddend = 2;
constexpr uint32_t kImportFormatAddend64 = 3;

uintptr_t g_guard_base = 0;
std::vector<std::string> g_guard_names;
std::vector<std::pair<uintptr_t, uintptr_t>> g_writable_ranges;

uintptr_t guard_address(const std::string& name) {
    if (g_guard_base == 0) {
        void* region = mmap(nullptr, kGuardSpacing * kGuardSlots, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
        if (region == MAP_FAILED) return 0;
        g_guard_base = reinterpret_cast<uintptr_t>(region);
    }
    if (g_guard_names.size() + 2 >= kGuardSlots) return 0;
    g_guard_names.push_back(name);
    // One slot of margin on both sides: Swift reads metadata at negative offsets too.
    return g_guard_base + g_guard_names.size() * kGuardSpacing;
}

struct ResolvedImport {
    uint64_t address;
    uint64_t addend;
};

bool process_chain(uint8_t* location, uint16_t format, uintptr_t preferred_base, uintptr_t base,
                   const std::vector<ResolvedImport>& imports, std::string& error) {
    for (;;) {
        uint64_t raw;
        memcpy(&raw, location, sizeof raw);
        const uint64_t next = (raw >> 51) & 0xfff;
        uint64_t value;
        if (raw >> 63) {
            const uint32_t ordinal = raw & 0xffffff;
            const uint64_t addend = (raw >> 24) & 0xff;
            if (ordinal >= imports.size()) {
                error = "chained fixup references import out of range";
                return false;
            }
            value = imports[ordinal].address == 0 ? 0 : imports[ordinal].address + imports[ordinal].addend + addend;
        } else {
            const uint64_t target = raw & 0xfffffffffULL;
            const uint64_t high8 = (raw >> 36) & 0xff;
            const uint64_t offset = format == kFormatPtr64 ? target - preferred_base : target;
            value = (base + offset) | (high8 << 56);
        }
        memcpy(location, &value, sizeof value);
        if (next == 0) return true;
        location += next * 4;
    }
}

}  // namespace

const MachSection* MachImage::section(const char* segment, const char* name) const {
    for (const auto& candidate : sections) {
        if (candidate.segment == segment && candidate.name == name) return &candidate;
    }
    return nullptr;
}

const char* unresolved_symbol_at(uintptr_t address) {
    if (g_guard_base == 0 || address < g_guard_base || address >= g_guard_base + kGuardSpacing * kGuardSlots) return nullptr;
    const size_t slot = (address - g_guard_base + kGuardSpacing / 2) / kGuardSpacing;
    if (slot == 0 || slot > g_guard_names.size()) return nullptr;
    return g_guard_names[slot - 1].c_str();
}

const void* unresolved_symbol_named(const char* name) {
    for (size_t i = 0; i < g_guard_names.size(); ++i) {
        if (g_guard_names[i] == name) return reinterpret_cast<const void*>(g_guard_base + (i + 1) * kGuardSpacing);
    }
    return nullptr;
}

bool load_macho(const std::string& path, SymbolResolver resolve, MachImage& image, std::string& error) {
    const int fd = open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        error = "cannot open " + path;
        return false;
    }
    struct stat st {};
    fstat(fd, &st);
    const size_t file_size = static_cast<size_t>(st.st_size);
    auto* file = static_cast<uint8_t*>(mmap(nullptr, file_size, PROT_READ, MAP_PRIVATE, fd, 0));
    close(fd);
    if (file == MAP_FAILED || file_size < sizeof(MachHeader64)) {
        error = "cannot read " + path;
        return false;
    }

    const uint8_t* slice = file;
    size_t slice_size = file_size;
    uint32_t magic;
    memcpy(&magic, file, sizeof magic);
    if (magic == kFatMagicSwapped) {
        const auto* fat = reinterpret_cast<const FatHeader*>(file);
        const auto* arch = reinterpret_cast<const FatArch*>(fat + 1);
        slice = nullptr;
        for (uint32_t i = 0; i < __builtin_bswap32(fat->nfat_arch); ++i) {
            if (__builtin_bswap32(arch[i].cputype) == kCpuArm64 &&
                (__builtin_bswap32(arch[i].cpusubtype) & kCpuSubtypeMask) == 0) {
                slice = file + __builtin_bswap32(arch[i].offset);
                slice_size = __builtin_bswap32(arch[i].size);
            }
        }
        if (slice == nullptr) {
            error = "universal binary has no plain arm64 slice";
            return false;
        }
    }

    const auto* header = reinterpret_cast<const MachHeader64*>(slice);
    if (header->magic != kMagic64 || header->cputype != kCpuArm64) {
        error = "not a 64-bit arm64 Mach-O (32-bit and arm64e apps are unsupported)";
        return false;
    }

    std::vector<const SegmentCommand64*> segments;
    std::vector<std::string> libraries;
    const LinkeditDataCommand* chained = nullptr;
    uint64_t entry_offset = 0;
    bool has_entry = false;
    bool has_classic_fixups = false;

    const auto* cursor = reinterpret_cast<const uint8_t*>(header + 1);
    for (uint32_t i = 0; i < header->ncmds; ++i) {
        const auto* command = reinterpret_cast<const LoadCommand*>(cursor);
        switch (command->cmd) {
            case kLcSegment64:
                segments.push_back(reinterpret_cast<const SegmentCommand64*>(command));
                break;
            case kLcLoadDylib:
            case kLcLoadWeakDylib:
            case kLcReexportDylib:
            case kLcLazyLoadDylib:
            case kLcUpwardDylib: {
                const auto* dylib = reinterpret_cast<const DylibCommand*>(command);
                libraries.emplace_back(reinterpret_cast<const char*>(command) + dylib->name_offset);
                break;
            }
            case kLcMain:
                entry_offset = reinterpret_cast<const EntryPointCommand*>(command)->entryoff;
                has_entry = true;
                break;
            case kLcEncryptionInfo64:
                if (reinterpret_cast<const EncryptionCommand64*>(command)->cryptid != 0) {
                    error = "binary is FairPlay encrypted (App Store build); it needs a decrypted IPA";
                    return false;
                }
                break;
            case kLcChainedFixups:
                chained = reinterpret_cast<const LinkeditDataCommand*>(command);
                break;
            case kLcDyldInfo:
            case kLcDyldInfoOnly:
                has_classic_fixups = true;
                break;
            default:
                break;
        }
        cursor += command->cmdsize;
    }
    if (!has_entry) {
        error = "binary has no LC_MAIN entry point";
        return false;
    }
    if (chained == nullptr) {
        error = has_classic_fixups ? "binary uses pre-iOS 15 dyld opcodes, which the loader does not read yet"
                                   : "binary has no fixup information";
        return false;
    }

    uint64_t low = UINT64_MAX;
    uint64_t high = 0;
    for (const auto* segment : segments) {
        if (segment->initprot == 0 && segment->filesize == 0) continue;
        if (segment->vmaddr < low) low = segment->vmaddr;
        if (segment->vmaddr + segment->vmsize > high) high = segment->vmaddr + segment->vmsize;
    }
    if (low >= high) {
        error = "binary has no loadable segments";
        return false;
    }
    const size_t total = ((high - low) + kMachPageSize - 1) & ~(kMachPageSize - 1);
    // Mach-O segments are 16 KiB aligned, which mmap does not guarantee on 4 KiB-page devices.
    void* reservation = mmap(nullptr, total + kMachPageSize, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (reservation == MAP_FAILED) {
        error = "cannot reserve memory for the image";
        return false;
    }
    auto* base = reinterpret_cast<uint8_t*>((reinterpret_cast<uintptr_t>(reservation) + kMachPageSize - 1) & ~(kMachPageSize - 1));

    image.base = reinterpret_cast<uintptr_t>(base);
    image.size = total;
    image.slide = static_cast<intptr_t>(image.base - low);
    image.entry = image.base + entry_offset;

    for (const auto* segment : segments) {
        if (segment->initprot == 0 && segment->filesize == 0) continue;
        if (segment->fileoff + segment->filesize > slice_size) {
            error = "segment extends past the end of the file";
            return false;
        }
        memcpy(base + (segment->vmaddr - low), slice + segment->fileoff, segment->filesize);
        const auto* sections = reinterpret_cast<const Section64*>(segment + 1);
        for (uint32_t i = 0; i < segment->nsects; ++i) {
            image.sections.push_back({std::string(sections[i].segname, strnlen(sections[i].segname, 16)),
                                      std::string(sections[i].sectname, strnlen(sections[i].sectname, 16)),
                                      image.base + (sections[i].addr - low), static_cast<size_t>(sections[i].size)});
        }
    }

    const uint8_t* fixups = slice + chained->dataoff;
    const auto* fixup_header = reinterpret_cast<const ChainedFixupsHeader*>(fixups);
    if (fixup_header->symbols_format != 0) {
        error = "compressed chained fixup symbols are unsupported";
        return false;
    }
    const char* symbols = reinterpret_cast<const char*>(fixups + fixup_header->symbols_offset);
    const uint8_t* import_table = fixups + fixup_header->imports_offset;

    std::vector<ResolvedImport> resolved;
    resolved.reserve(fixup_header->imports_count);
    for (uint32_t i = 0; i < fixup_header->imports_count; ++i) {
        int32_t ordinal;
        bool weak;
        uint32_t name_offset;
        uint64_t addend = 0;
        if (fixup_header->imports_format == kImportFormatPlain) {
            uint32_t raw;
            memcpy(&raw, import_table + i * 4, 4);
            ordinal = static_cast<int8_t>(raw & 0xff);
            weak = (raw >> 8) & 1;
            name_offset = raw >> 9;
        } else if (fixup_header->imports_format == kImportFormatAddend) {
            uint32_t raw;
            int32_t small_addend;
            memcpy(&raw, import_table + i * 8, 4);
            memcpy(&small_addend, import_table + i * 8 + 4, 4);
            ordinal = static_cast<int8_t>(raw & 0xff);
            weak = (raw >> 8) & 1;
            name_offset = raw >> 9;
            addend = static_cast<uint64_t>(static_cast<int64_t>(small_addend));
        } else if (fixup_header->imports_format == kImportFormatAddend64) {
            uint64_t raw;
            memcpy(&raw, import_table + i * 16, 8);
            memcpy(&addend, import_table + i * 16 + 8, 8);
            ordinal = static_cast<int16_t>(raw & 0xffff);
            weak = (raw >> 16) & 1;
            name_offset = static_cast<uint32_t>(raw >> 32);
        } else {
            error = "unknown chained import format";
            return false;
        }

        const char* name = symbols + name_offset;
        const char* lookup = name[0] == '_' ? name + 1 : name;
        std::string library = ordinal > 0 && static_cast<size_t>(ordinal) <= libraries.size() ? libraries[ordinal - 1] : "";
        const size_t slash = library.rfind('/');
        if (slash != std::string::npos) library = library.substr(slash + 1);

        void* address = resolve(lookup);
        const bool found = address != nullptr;
        uint64_t target = reinterpret_cast<uint64_t>(address);
        if (!found && !weak) {
            target = guard_address(name);
            if (target == 0) {
                error = "too many unresolved imports";
                return false;
            }
        }
        resolved.push_back({target, addend});
        image.imports.push_back({name, library, weak, found});
    }

    const uint8_t* starts_in_image = fixups + fixup_header->starts_offset;
    uint32_t seg_count;
    memcpy(&seg_count, starts_in_image, 4);
    for (uint32_t i = 0; i < seg_count; ++i) {
        uint32_t seg_info_offset;
        memcpy(&seg_info_offset, starts_in_image + 4 + i * 4, 4);
        if (seg_info_offset == 0) continue;
        const auto* starts = reinterpret_cast<const ChainedStartsInSegment*>(starts_in_image + seg_info_offset);
        if (starts->pointer_format != kFormatPtr64 && starts->pointer_format != kFormatPtr64Offset) {
            char message[80];
            snprintf(message, sizeof message, "unsupported chained pointer format %u", starts->pointer_format);
            error = message;
            return false;
        }
        for (uint16_t page = 0; page < starts->page_count; ++page) {
            const uint16_t start = starts->page_start[page];
            if (start == kStartNone) continue;
            uint8_t* page_base = base + starts->segment_offset + page * static_cast<size_t>(starts->page_size);
            if (start & kStartMulti) {
                uint16_t index = start & ~kStartMulti;
                for (;;) {
                    const uint16_t overflow = starts->page_start[index++];
                    if (!process_chain(page_base + (overflow & ~kStartLast), starts->pointer_format, low, image.base, resolved, error)) return false;
                    if (overflow & kStartLast) break;
                }
            } else if (!process_chain(page_base + start, starts->pointer_format, low, image.base, resolved, error)) {
                return false;
            }
        }
    }

    for (const auto* segment : segments) {
        if (segment->initprot == 0 && segment->filesize == 0) continue;
        int protection = 0;
        if (segment->initprot & 1) protection |= PROT_READ;
        if (segment->initprot & 2) protection |= PROT_WRITE;
        if (segment->initprot & 4) protection |= PROT_EXEC;
        // __DATA_CONST stays writable: the Swift and ObjC shims patch metadata caches in place.
        if (strncmp(segment->segname, "__DATA_CONST", 16) == 0) protection |= PROT_WRITE;
        const uintptr_t start = (image.base + (segment->vmaddr - low)) & ~(kMachPageSize - 1);
        if (protection & PROT_WRITE) g_writable_ranges.emplace_back(start, start + segment->vmsize);
        const size_t length = (segment->vmsize + kMachPageSize - 1) & ~(kMachPageSize - 1);
        if (mprotect(reinterpret_cast<void*>(start), length, protection) != 0) {
            error = "mprotect failed for segment " + std::string(segment->segname, strnlen(segment->segname, 16));
            return false;
        }
    }
    __builtin___clear_cache(reinterpret_cast<char*>(base), reinterpret_cast<char*>(base + total));

    munmap(file, file_size);
    return true;
}

bool image_address_is_writable(uintptr_t address) {
    for (const auto& [start, end] : g_writable_ranges) {
        if (address >= start && address < end) return true;
    }
    return false;
}
