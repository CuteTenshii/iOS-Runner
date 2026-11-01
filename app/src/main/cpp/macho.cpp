#include "macho.h"

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cstdio>
#include <cstring>
#include <map>
#include <string_view>
#include <utility>

#include "engine.h"

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
constexpr uint32_t kLcExportsTrie = 0x33 | kLcReqDyld;
constexpr uint32_t kLcRpath = 0x1c | kLcReqDyld;

constexpr size_t kMachPageSize = 0x4000;
constexpr size_t kGuardSpacing = 0x1000;
constexpr size_t kGuardSlots = 0x4000;

constexpr uint32_t kBuildVersionCommand = 0x32;

struct MachHeader64 {
    uint32_t magic, cputype, cpusubtype, filetype, ncmds, sizeofcmds, flags, reserved;
};
struct LoadCommand {
    uint32_t cmd, cmdsize;
};
struct BuildVersionCommand {
    uint32_t cmd, cmdsize, platform, minos, sdk, ntools;
};
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
struct DylibCommand {
    uint32_t cmd, cmdsize, name_offset, timestamp, current_version, compat_version;
};
struct EntryPointCommand {
    uint32_t cmd, cmdsize;
    uint64_t entryoff, stacksize;
};
struct EncryptionCommand64 {
    uint32_t cmd, cmdsize, cryptoff, cryptsize, cryptid, pad;
};
struct LinkeditDataCommand {
    uint32_t cmd, cmdsize, dataoff, datasize;
};
struct FatHeader {
    uint32_t magic, nfat_arch;
};
struct FatArch {
    uint32_t cputype, cpusubtype, offset, size, align;
};

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

namespace {

struct DyldInfoCommand {
    uint32_t cmd, cmdsize, rebase_off, rebase_size, bind_off, bind_size, weak_bind_off, weak_bind_size, lazy_bind_off, lazy_bind_size, export_off, export_size;
};
struct RpathCommand {
    uint32_t cmd, cmdsize, path_offset;
};

constexpr uint32_t kFileTypeExecute = 2;
constexpr int kOrdinalSelf = 0;
constexpr int kOrdinalMainExecutable = -1;

// Stays mapped until the whole graph is bound, because binding reads the file's load commands.
struct PendingImage {
    MachImage* image = nullptr;
    uint8_t* file = nullptr;
    size_t file_size = 0;
    const uint8_t* slice = nullptr;
    size_t slice_size = 0;
    std::vector<const SegmentCommand64*> segments;
    std::vector<std::string> libraries;
    std::vector<std::string> rpaths;
    const LinkeditDataCommand* chained = nullptr;
    const DyldInfoCommand* dyld_info = nullptr;
    const LinkeditDataCommand* exports = nullptr;
    uint64_t low = 0;
};

struct LoadSession {
    std::string executable_path;
    SymbolResolver resolve;
    std::map<std::string, MachImage*> by_path;
    std::vector<PendingImage> pending;
    std::vector<std::unique_ptr<MachImage>>* images;
    MachImage* main_image = nullptr;
};

uint64_t read_uleb(const uint8_t*& cursor, const uint8_t* end) {
    uint64_t value = 0;
    int shift = 0;
    while (cursor < end) {
        const uint8_t byte = *cursor++;
        if (shift < 64) value |= static_cast<uint64_t>(byte & 0x7f) << shift;
        shift += 7;
        if (!(byte & 0x80)) break;
    }
    return value;
}

int64_t read_sleb(const uint8_t*& cursor, const uint8_t* end) {
    int64_t value = 0;
    int shift = 0;
    uint8_t byte = 0;
    while (cursor < end) {
        byte = *cursor++;
        if (shift < 64) value |= static_cast<int64_t>(byte & 0x7f) << shift;
        shift += 7;
        if (!(byte & 0x80)) break;
    }
    if (shift < 64 && (byte & 0x40)) value |= -(int64_t{1} << shift);
    return value;
}

std::string directory_of(const std::string& path) {
    const size_t slash = path.rfind('/');
    return slash == std::string::npos ? "." : path.substr(0, slash);
}

std::string expand_load_path(const std::string& raw, const std::string& loader_path, const std::string& executable_path) {
    constexpr std::string_view kExecutable = "@executable_path";
    constexpr std::string_view kLoader = "@loader_path";
    if (raw.rfind(kExecutable, 0) == 0) return directory_of(executable_path) + raw.substr(kExecutable.size());
    if (raw.rfind(kLoader, 0) == 0) return directory_of(loader_path) + raw.substr(kLoader.size());
    return raw;
}

// Only libraries shipped inside the bundle load as images; the shims stand in for system ones.
std::string find_bundled_library(const PendingImage& loader, const std::string& name, const std::string& executable_path) {
    const std::string bundle = directory_of(executable_path);
    std::vector<std::string> candidates;
    constexpr std::string_view kRpath = "@rpath/";
    if (name.rfind(kRpath, 0) == 0) {
        const std::string rest = name.substr(kRpath.size());
        for (const auto& rpath : loader.rpaths) candidates.push_back(expand_load_path(rpath, loader.image->path, executable_path) + "/" + rest);
        candidates.push_back(bundle + "/Frameworks/" + rest);
    } else if (name[0] == '@') {
        candidates.push_back(expand_load_path(name, loader.image->path, executable_path));
    }
    for (const auto& candidate : candidates) {
        if (candidate.rfind(bundle + "/", 0) == 0 && access(candidate.c_str(), R_OK) == 0) return candidate;
    }
    return {};
}

bool map_image(LoadSession& session, const std::string& path, MachImage*& mapped, std::string& error) {
    if (const auto found = session.by_path.find(path); found != session.by_path.end()) {
        mapped = found->second;
        return true;
    }
    const int fd = open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        error = "cannot open " + path;
        return false;
    }
    struct stat st{};
    fstat(fd, &st);
    const size_t file_size = static_cast<size_t>(st.st_size);
    auto* file = static_cast<uint8_t*>(mmap(nullptr, file_size, PROT_READ, MAP_PRIVATE, fd, 0));
    close(fd);
    if (file == MAP_FAILED || file_size < sizeof(MachHeader64)) {
        error = "cannot read " + path;
        return false;
    }

    PendingImage pending;
    pending.file = file;
    pending.file_size = file_size;
    pending.slice = file;
    pending.slice_size = file_size;
    uint32_t magic;
    memcpy(&magic, file, sizeof magic);
    if (magic == kFatMagicSwapped) {
        const auto* fat = reinterpret_cast<const FatHeader*>(file);
        const auto* arch = reinterpret_cast<const FatArch*>(fat + 1);
        pending.slice = nullptr;
        for (uint32_t i = 0; i < __builtin_bswap32(fat->nfat_arch); ++i) {
            if (__builtin_bswap32(arch[i].cputype) == kCpuArm64 &&
                (__builtin_bswap32(arch[i].cpusubtype) & kCpuSubtypeMask) == 0) {
                pending.slice = file + __builtin_bswap32(arch[i].offset);
                pending.slice_size = __builtin_bswap32(arch[i].size);
            }
        }
        if (pending.slice == nullptr) {
            error = "universal binary has no plain arm64 slice";
            return false;
        }
    }

    const auto* header = reinterpret_cast<const MachHeader64*>(pending.slice);
    if (header->magic != kMagic64 || header->cputype != kCpuArm64) {
        error = "not a 64-bit arm64 Mach-O (32-bit and arm64e apps are unsupported)";
        return false;
    }

    auto owned = std::make_unique<MachImage>();
    MachImage& image = *owned;
    image.path = path;
    const size_t name_start = path.rfind('/');
    image.name = name_start == std::string::npos ? path : path.substr(name_start + 1);
    pending.image = owned.get();

    uint64_t entry_offset = 0;
    bool has_entry = false;
    const auto* cursor = reinterpret_cast<const uint8_t*>(header + 1);
    for (uint32_t i = 0; i < header->ncmds; ++i) {
        const auto* command = reinterpret_cast<const LoadCommand*>(cursor);
        switch (command->cmd) {
            case kBuildVersionCommand:
                image.minimum_os = reinterpret_cast<const BuildVersionCommand*>(command)->minos;
                break;
            case kLcSegment64:
                pending.segments.push_back(reinterpret_cast<const SegmentCommand64*>(command));
                break;
            case kLcLoadDylib:
            case kLcLoadWeakDylib:
            case kLcReexportDylib:
            case kLcLazyLoadDylib:
            case kLcUpwardDylib: {
                const auto* dylib = reinterpret_cast<const DylibCommand*>(command);
                pending.libraries.emplace_back(reinterpret_cast<const char*>(command) + dylib->name_offset);
                break;
            }
            case kLcRpath:
                pending.rpaths.emplace_back(reinterpret_cast<const char*>(command) + reinterpret_cast<const RpathCommand*>(command)->path_offset);
                break;
            case kLcMain:
                entry_offset = reinterpret_cast<const EntryPointCommand*>(command)->entryoff;
                has_entry = true;
                break;
            case kLcEncryptionInfo64:
                if (reinterpret_cast<const EncryptionCommand64*>(command)->cryptid != 0) {
                    error = image.name + " is FairPlay encrypted (App Store build); it needs a decrypted IPA";
                    return false;
                }
                break;
            case kLcChainedFixups:
                pending.chained = reinterpret_cast<const LinkeditDataCommand*>(command);
                break;
            case kLcExportsTrie:
                pending.exports = reinterpret_cast<const LinkeditDataCommand*>(command);
                break;
            case kLcDyldInfo:
            case kLcDyldInfoOnly:
                pending.dyld_info = reinterpret_cast<const DyldInfoCommand*>(command);
                break;
            default:
                break;
        }
        cursor += command->cmdsize;
    }
    if (header->filetype == kFileTypeExecute && !has_entry) {
        error = "binary has no LC_MAIN entry point";
        return false;
    }
    if (pending.chained == nullptr && pending.dyld_info == nullptr) {
        error = image.name + " has no fixup information";
        return false;
    }

    uint64_t low = UINT64_MAX;
    uint64_t high = 0;
    for (const auto* segment : pending.segments) {
        if (segment->initprot == 0 && segment->filesize == 0) continue;
        if (segment->vmaddr < low) low = segment->vmaddr;
        if (segment->vmaddr + segment->vmsize > high) high = segment->vmaddr + segment->vmsize;
    }
    if (low >= high) {
        error = image.name + " has no loadable segments";
        return false;
    }
    pending.low = low;
    const size_t total = ((high - low) + kMachPageSize - 1) & ~(kMachPageSize - 1);
    // Mach-O segments are 16 KiB aligned, which mmap does not guarantee on 4 KiB-page devices.
    void* reservation = mmap(nullptr, total + kMachPageSize, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (reservation == MAP_FAILED) {
        error = "cannot reserve memory for " + image.name;
        return false;
    }
    auto* base = reinterpret_cast<uint8_t*>((reinterpret_cast<uintptr_t>(reservation) + kMachPageSize - 1) & ~(kMachPageSize - 1));

    image.base = reinterpret_cast<uintptr_t>(base);
    image.size = total;
    image.slide = static_cast<intptr_t>(image.base - low);
    image.entry = has_entry ? image.base + entry_offset : 0;

    for (const auto* segment : pending.segments) {
        if (segment->initprot == 0 && segment->filesize == 0) continue;
        if (segment->fileoff + segment->filesize > pending.slice_size) {
            error = image.name + ": segment extends past the end of the file";
            return false;
        }
        memcpy(base + (segment->vmaddr - low), pending.slice + segment->fileoff, segment->filesize);
        const auto* sections = reinterpret_cast<const Section64*>(segment + 1);
        for (uint32_t i = 0; i < segment->nsects; ++i) {
            image.sections.push_back({std::string(sections[i].segname, strnlen(sections[i].segname, 16)),
                                      std::string(sections[i].sectname, strnlen(sections[i].sectname, 16)),
                                      image.base + (sections[i].addr - low), static_cast<size_t>(sections[i].size), sections[i].flags});
        }
        if (strncmp(segment->segname, "__LINKEDIT", 16) == 0) {
            const uint32_t trie_offset = pending.exports != nullptr ? pending.exports->dataoff : pending.dyld_info != nullptr ? pending.dyld_info->export_off
                                                                                                                              : 0;
            const uint32_t trie_size = pending.exports != nullptr ? pending.exports->datasize : pending.dyld_info != nullptr ? pending.dyld_info->export_size
                                                                                                                             : 0;
            if (trie_size != 0 && trie_offset >= segment->fileoff) {
                image.exports = base + (segment->vmaddr - low) + (trie_offset - segment->fileoff);
                image.exports_size = trie_size;
            }
        }
    }

    if (header->filetype == kFileTypeExecute) session.main_image = owned.get();
    session.by_path[path] = owned.get();

    // Dependencies come first, so the image order is also the order initializers must run in.
    image.dependencies.resize(pending.libraries.size(), nullptr);
    for (size_t i = 0; i < pending.libraries.size(); ++i) {
        const std::string found = find_bundled_library(pending, pending.libraries[i], session.executable_path);
        if (found.empty()) continue;
        std::string dependency_error;
        MachImage* dependency = nullptr;
        if (map_image(session, found, dependency, dependency_error)) {
            image.dependencies[i] = dependency;
        } else {
            // Its imports stay guard addresses, which name the problem only if they are actually used.
            engine_log("DYLIB: skipping %s: %s", found.c_str(), dependency_error.c_str());
        }
    }
    session.pending.push_back(pending);
    session.images->push_back(std::move(owned));
    mapped = pending.image;
    return true;
}

uintptr_t lookup_export(const MachImage& image, const char* name, int depth = 0) {
    constexpr uint64_t kKindMask = 0x03;
    constexpr uint64_t kKindAbsolute = 0x02;
    constexpr uint64_t kFlagReexport = 0x08;
    constexpr uint64_t kFlagStubAndResolver = 0x10;
    constexpr int kMaxReexportDepth = 16;
    if (image.exports == nullptr || depth > kMaxReexportDepth) return 0;
    const uint8_t* const end = image.exports + image.exports_size;
    const uint8_t* node = image.exports;
    const char* rest = name;
    for (;;) {
        const uint8_t* cursor = node;
        const uint64_t terminal_size = read_uleb(cursor, end);
        if (*rest == '\0') {
            if (terminal_size == 0) return 0;
            const uint64_t flags = read_uleb(cursor, end);
            if (flags & kFlagReexport) {
                const uint64_t ordinal = read_uleb(cursor, end);
                const char* imported = reinterpret_cast<const char*>(cursor);
                if (ordinal == 0 || ordinal > image.dependencies.size() || image.dependencies[ordinal - 1] == nullptr) return 0;
                return lookup_export(*image.dependencies[ordinal - 1], *imported != '\0' ? imported : name, depth + 1);
            }
            // The stub binds lazily through its own pointer, which this loader has already filled in.
            if (flags & kFlagStubAndResolver) return image.base + read_uleb(cursor, end);
            const uint64_t value = read_uleb(cursor, end);
            return (flags & kKindMask) == kKindAbsolute ? value : image.base + value;
        }
        cursor += terminal_size;
        if (cursor >= end) return 0;
        const uint8_t children = *cursor++;
        const uint8_t* next = nullptr;
        for (uint8_t child = 0; child < children && next == nullptr; ++child) {
            const char* edge = reinterpret_cast<const char*>(cursor);
            const size_t edge_length = strlen(edge);
            cursor += edge_length + 1;
            const uint64_t offset = read_uleb(cursor, end);
            if (strncmp(rest, edge, edge_length) == 0) {
                rest += edge_length;
                next = image.exports + offset;
            }
        }
        if (next == nullptr || next >= end) return 0;
        node = next;
    }
}

// `name` keeps its leading underscore, as the trie stores it. Returns false only if no guard address is left.
bool resolve_import(LoadSession& session, const PendingImage& pending, const char* name, int ordinal, bool weak, uint64_t addend, ResolvedImport& out, std::string& error) {
    MachImage& image = *pending.image;
    const char* lookup = name[0] == '_' ? name + 1 : name;
    std::string library = ordinal > 0 && static_cast<size_t>(ordinal) <= pending.libraries.size() ? pending.libraries[ordinal - 1] : "";
    const size_t slash = library.rfind('/');
    if (slash != std::string::npos) library = library.substr(slash + 1);

    uintptr_t address = 0;
    if (ordinal > 0 && static_cast<size_t>(ordinal) <= image.dependencies.size() && image.dependencies[ordinal - 1] != nullptr) {
        address = lookup_export(*image.dependencies[ordinal - 1], name);
    } else if (ordinal == kOrdinalSelf) {
        address = lookup_export(image, name);
    } else if (ordinal == kOrdinalMainExecutable && session.main_image != nullptr) {
        address = lookup_export(*session.main_image, name);
    }
    if (address == 0) address = reinterpret_cast<uintptr_t>(session.resolve(lookup));
    // Flat and coalesced lookups may be satisfied by any image, starting with this one.
    if (address == 0 && ordinal < kOrdinalMainExecutable) {
        address = lookup_export(image, name);
        for (size_t i = 0; address == 0 && i < session.images->size(); ++i) address = lookup_export(*(*session.images)[i], name);
    }

    const bool found = address != 0;
    uint64_t target = address;
    // A weak function stays null for its availability check, but a weak type or conformance can still be named.
    const size_t name_length = strlen(name);
    const bool weak_type = name_length > 2 && (strcmp(name + name_length - 2, "Mn") == 0 || strcmp(name + name_length - 2, "Mc") == 0);
    if (!found && (!weak || weak_type)) {
        target = guard_address(name);
        if (target == 0) {
            error = "too many unresolved imports";
            return false;
        }
    }
    out = {target, addend};
    image.imports.push_back({name, library, weak, found});
    return true;
}

bool apply_chained_fixups(LoadSession& session, const PendingImage& pending, std::string& error) {
    MachImage& image = *pending.image;
    auto* base = reinterpret_cast<uint8_t*>(image.base);
    const uint8_t* fixups = pending.slice + pending.chained->dataoff;
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
        ResolvedImport entry{};
        if (!resolve_import(session, pending, symbols + name_offset, ordinal, weak, addend, entry, error)) return false;
        resolved.push_back(entry);
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
                    if (!process_chain(page_base + (overflow & ~kStartLast), starts->pointer_format, pending.low, image.base, resolved, error)) return false;
                    if (overflow & kStartLast) break;
                }
            } else if (!process_chain(page_base + start, starts->pointer_format, pending.low, image.base, resolved, error)) {
                return false;
            }
        }
    }
    return true;
}

bool apply_rebases(const PendingImage& pending, std::string& error) {
    constexpr uint8_t kDone = 0x00, kSetType = 0x10, kSetSegment = 0x20, kAddAddress = 0x30, kAddScaled = 0x40, kDoImmediate = 0x50, kDoUleb = 0x60, kDoAddAddress = 0x70, kDoSkipping = 0x80;
    const uint8_t* cursor = pending.slice + pending.dyld_info->rebase_off;
    const uint8_t* const end = cursor + pending.dyld_info->rebase_size;
    uint8_t* location = nullptr;
    const auto rebase = [&](uint64_t count, uint64_t skip) {
        for (uint64_t i = 0; i < count; ++i) {
            uint64_t value;
            memcpy(&value, location, sizeof value);
            value += static_cast<uint64_t>(pending.image->slide);
            memcpy(location, &value, sizeof value);
            location += sizeof value + skip;
        }
    };
    while (cursor < end) {
        const uint8_t byte = *cursor++;
        const uint8_t immediate = byte & 0x0f;
        switch (byte & 0xf0) {
            case kDone:
                return true;
            case kSetType:
                break;
            case kSetSegment: {
                if (immediate >= pending.segments.size()) {
                    error = "rebase names a segment out of range";
                    return false;
                }
                const uint64_t offset = read_uleb(cursor, end);
                location = reinterpret_cast<uint8_t*>(pending.image->base + (pending.segments[immediate]->vmaddr - pending.low) + offset);
                break;
            }
            case kAddAddress:
                location += read_uleb(cursor, end);
                break;
            case kAddScaled:
                location += immediate * sizeof(uint64_t);
                break;
            case kDoImmediate:
                rebase(immediate, 0);
                break;
            case kDoUleb:
                rebase(read_uleb(cursor, end), 0);
                break;
            case kDoAddAddress:
                rebase(1, read_uleb(cursor, end));
                break;
            case kDoSkipping: {
                const uint64_t count = read_uleb(cursor, end);
                rebase(count, read_uleb(cursor, end));
                break;
            }
            default:
                error = "unknown rebase opcode";
                return false;
        }
    }
    return true;
}

// Lazy bindings run here too: nothing later would fill them, as there is no dyld_stub_binder.
bool apply_binds(LoadSession& session, const PendingImage& pending, uint32_t offset, uint32_t size, std::string& error) {
    constexpr uint8_t kDone = 0x00, kOrdinalImmediate = 0x10, kOrdinalUleb = 0x20, kOrdinalSpecial = 0x30, kSymbol = 0x40, kType = 0x50, kAddend = 0x60, kSegment = 0x70, kAddAddress = 0x80, kBind = 0x90, kBindAddAddress = 0xa0, kBindScaled = 0xb0, kBindSkipping = 0xc0;
    constexpr uint8_t kSymbolWeakImport = 0x01;
    const uint8_t* cursor = pending.slice + offset;
    const uint8_t* const end = cursor + size;
    uint8_t* location = nullptr;
    int ordinal = 0;
    const char* symbol = nullptr;
    bool weak = false;
    int64_t addend = 0;
    std::map<std::pair<const char*, int>, uint64_t> bound;
    const auto bind = [&]() {
        if (symbol == nullptr || location == nullptr) {
            error = "bind opcode without a symbol or address";
            return false;
        }
        const auto key = std::make_pair(symbol, ordinal);
        auto found = bound.find(key);
        if (found == bound.end()) {
            ResolvedImport entry{};
            if (!resolve_import(session, pending, symbol, ordinal, weak, 0, entry, error)) return false;
            found = bound.emplace(key, entry.address).first;
        }
        const uint64_t value = found->second == 0 ? 0 : found->second + static_cast<uint64_t>(addend);
        memcpy(location, &value, sizeof value);
        location += sizeof value;
        return true;
    };
    while (cursor < end) {
        const uint8_t byte = *cursor++;
        const uint8_t immediate = byte & 0x0f;
        switch (byte & 0xf0) {
            case kDone:
                break;
            case kOrdinalImmediate:
                ordinal = immediate;
                break;
            case kOrdinalUleb:
                ordinal = static_cast<int>(read_uleb(cursor, end));
                break;
            case kOrdinalSpecial:
                ordinal = immediate == 0 ? 0 : static_cast<int8_t>(0xf0 | immediate);
                break;
            case kSymbol:
                symbol = reinterpret_cast<const char*>(cursor);
                weak = (immediate & kSymbolWeakImport) != 0;
                cursor += strlen(symbol) + 1;
                break;
            case kType:
                break;
            case kAddend:
                addend = read_sleb(cursor, end);
                break;
            case kSegment: {
                if (immediate >= pending.segments.size()) {
                    error = "bind names a segment out of range";
                    return false;
                }
                const uint64_t segment_offset = read_uleb(cursor, end);
                location = reinterpret_cast<uint8_t*>(pending.image->base + (pending.segments[immediate]->vmaddr - pending.low) + segment_offset);
                break;
            }
            case kAddAddress:
                location += read_uleb(cursor, end);
                break;
            case kBind:
                if (!bind()) return false;
                break;
            case kBindAddAddress:
                if (!bind()) return false;
                location += read_uleb(cursor, end);
                break;
            case kBindScaled:
                if (!bind()) return false;
                location += immediate * sizeof(uint64_t);
                break;
            case kBindSkipping: {
                const uint64_t count = read_uleb(cursor, end);
                const uint64_t skip = read_uleb(cursor, end);
                for (uint64_t i = 0; i < count; ++i) {
                    if (!bind()) return false;
                    location += skip;
                }
                break;
            }
            default:
                error = "unknown bind opcode";
                return false;
        }
    }
    return true;
}

bool protect_image(const PendingImage& pending, std::string& error) {
    const MachImage& image = *pending.image;
    for (const auto* segment : pending.segments) {
        if (segment->initprot == 0 && segment->filesize == 0) continue;
        int protection = 0;
        if (segment->initprot & 1) protection |= PROT_READ;
        if (segment->initprot & 2) protection |= PROT_WRITE;
        if (segment->initprot & 4) protection |= PROT_EXEC;
        // __DATA_CONST stays writable: the Swift and ObjC shims patch metadata caches in place.
        if (strncmp(segment->segname, "__DATA_CONST", 16) == 0) protection |= PROT_WRITE;
        const uintptr_t start = (image.base + (segment->vmaddr - pending.low)) & ~(kMachPageSize - 1);
        if (protection & PROT_WRITE) g_writable_ranges.emplace_back(start, start + segment->vmsize);
        const size_t length = (segment->vmsize + kMachPageSize - 1) & ~(kMachPageSize - 1);
        if (mprotect(reinterpret_cast<void*>(start), length, protection) != 0) {
            error = "mprotect failed for segment " + std::string(segment->segname, strnlen(segment->segname, 16));
            return false;
        }
    }
    __builtin___clear_cache(reinterpret_cast<char*>(image.base), reinterpret_cast<char*>(image.base + image.size));
    return true;
}

}  // namespace

bool load_macho_graph(const std::string& path, SymbolResolver resolve, std::vector<std::unique_ptr<MachImage>>& images, std::string& error) {
    LoadSession session;
    session.executable_path = path;
    session.resolve = resolve;
    session.images = &images;
    MachImage* main_image = nullptr;
    if (!map_image(session, path, main_image, error)) return false;

    // Everything is mapped before anything binds, so cycles between frameworks can still see each other's exports.
    for (const auto& pending : session.pending) {
        if (pending.chained != nullptr) {
            if (!apply_chained_fixups(session, pending, error)) return false;
            continue;
        }
        const auto* info = pending.dyld_info;
        if (!apply_rebases(pending, error) || !apply_binds(session, pending, info->bind_off, info->bind_size, error) ||
            !apply_binds(session, pending, info->lazy_bind_off, info->lazy_bind_size, error)) {
            error = pending.image->name + ": " + error;
            return false;
        }
    }
    for (const auto& pending : session.pending) {
        if (!protect_image(pending, error)) return false;
        munmap(pending.file, pending.file_size);
    }
    return true;
}

void* macho_find_export(const std::vector<std::unique_ptr<MachImage>>& images, const char* name) {
    const std::string symbol = std::string("_") + name;
    for (const auto& image : images) {
        if (const uintptr_t address = lookup_export(*image, symbol.c_str())) return reinterpret_cast<void*>(address);
    }
    return nullptr;
}

void run_image_initializers(const MachImage& image, int argc, const char** argv, const char** envp, const char** apple) {
    constexpr uint32_t kSectionTypeMask = 0xff;
    constexpr uint32_t kModInitFunctionPointers = 0x09;
    constexpr uint32_t kInitFunctionOffsets = 0x16;
    using Initializer = void (*)(int, const char**, const char**, const char**);
    for (const auto& section : image.sections) {
        const uint32_t type = section.flags & kSectionTypeMask;
        if (type == kModInitFunctionPointers) {
            for (size_t offset = 0; offset + sizeof(uintptr_t) <= section.size; offset += sizeof(uintptr_t)) {
                uintptr_t function;
                memcpy(&function, reinterpret_cast<const void*>(section.address + offset), sizeof function);
                reinterpret_cast<Initializer>(function)(argc, argv, envp, apple);
            }
        } else if (type == kInitFunctionOffsets) {
            for (size_t offset = 0; offset + sizeof(uint32_t) <= section.size; offset += sizeof(uint32_t)) {
                uint32_t function;
                memcpy(&function, reinterpret_cast<const void*>(section.address + offset), sizeof function);
                reinterpret_cast<Initializer>(image.base + function)(argc, argv, envp, apple);
            }
        }
    }
}

bool image_address_is_writable(uintptr_t address) {
    for (const auto& [start, end] : g_writable_ranges) {
        if (address >= start && address < end) return true;
    }
    return false;
}
