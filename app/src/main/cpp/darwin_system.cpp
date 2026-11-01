#include <dlfcn.h>
#include <errno.h>
#include <pwd.h>
#include <semaphore.h>
#include <signal.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include <atomic>
#include <cstdint>
#include <cstring>
#include <string>
#include <thread>

#include "darwin_shim.h"
#include "engine.h"
#include "macho.h"
#include "symbols.h"

namespace {

// bionic's errno values differ from Darwin's above ERANGE, and every wrapped call leaves bionic's value behind.
int errno_to_darwin(int value) {
    switch (value) {
        case EAGAIN:
            return 35;
        case EDEADLK:
            return 11;
        case ENAMETOOLONG:
            return 63;
        case ENOLCK:
            return 77;
        case ENOSYS:
            return 78;
        case ENOTEMPTY:
            return 66;
        case ELOOP:
            return 62;
        case EOVERFLOW:
            return 84;
        case ENOTSOCK:
            return 38;
        case EDESTADDRREQ:
            return 39;
        case EMSGSIZE:
            return 40;
        case EPROTOTYPE:
            return 41;
        case ENOPROTOOPT:
            return 42;
        case EPROTONOSUPPORT:
            return 43;
        case EOPNOTSUPP:
            return 102;
        case EAFNOSUPPORT:
            return 47;
        case EADDRINUSE:
            return 48;
        case EADDRNOTAVAIL:
            return 49;
        case ENETDOWN:
            return 50;
        case ENETUNREACH:
            return 51;
        case ECONNABORTED:
            return 53;
        case ECONNRESET:
            return 54;
        case ENOBUFS:
            return 55;
        case EISCONN:
            return 56;
        case ENOTCONN:
            return 57;
        case ETIMEDOUT:
            return 60;
        case ECONNREFUSED:
            return 61;
        case EHOSTUNREACH:
            return 65;
        case EALREADY:
            return 37;
        case EINPROGRESS:
            return 36;
        case ECANCELED:
            return 89;
        default:
            return value;
    }
}

// Translates in place once: a second read must not map an already translated value again.
int* darwin_error() {
    static thread_local int translated = 0;
    if (errno != translated) {
        translated = errno_to_darwin(errno);
        errno = translated;
    }
    return &errno;
}

long darwin_sysconf(int name) {
    constexpr int kArgMax = 1, kChildMax = 2, kClockTick = 3, kOpenMax = 5, kPageSize = 29, kCpusConfigured = 57, kCpusOnline = 58, kPhysicalPages = 200;
    switch (name) {
        case kArgMax:
            return sysconf(_SC_ARG_MAX);
        case kChildMax:
            return sysconf(_SC_CHILD_MAX);
        case kClockTick:
            return sysconf(_SC_CLK_TCK);
        case kOpenMax:
            return sysconf(_SC_OPEN_MAX);
        case kPageSize:
            return 16384;
        case kCpusConfigured:
            return sysconf(_SC_NPROCESSORS_CONF);
        case kCpusOnline:
            return sysconf(_SC_NPROCESSORS_ONLN);
        case kPhysicalPages:
            return sysconf(_SC_PHYS_PAGES);
        default:
            errno = EINVAL;
            return -1;
    }
}

int copy_sysctl_value(const void* value, size_t size, void* out, size_t* out_size) {
    if (out == nullptr) {
        *out_size = size;
        return 0;
    }
    const size_t copied = *out_size < size ? *out_size : size;
    memcpy(out, value, copied);
    *out_size = copied;
    if (copied < size) {
        errno = ENOMEM;
        return -1;
    }
    return 0;
}

int sysctl_int(int64_t value, void* out, size_t* out_size) {
    if (out != nullptr && *out_size == sizeof(int32_t)) {
        const auto narrow = static_cast<int32_t>(value);
        return copy_sysctl_value(&narrow, sizeof narrow, out, out_size);
    }
    return copy_sysctl_value(&value, sizeof value, out, out_size);
}

int sysctl_text(const char* value, void* out, size_t* out_size) { return copy_sysctl_value(value, strlen(value) + 1, out, out_size); }

int64_t physical_memory() { return static_cast<int64_t>(sysconf(_SC_PHYS_PAGES)) * sysconf(_SC_PAGESIZE); }

constexpr const char* kReportedMachine = "iPhone14,2";

int darwin_sysctlbyname(const char* name, void* out, size_t* out_size, const void*, size_t) {
    const std::string key = name;
    const long cpus = sysconf(_SC_NPROCESSORS_ONLN);
    if (key == "hw.machine" || key == "hw.model") return sysctl_text(kReportedMachine, out, out_size);
    if (key == "hw.ncpu" || key == "hw.activecpu" || key == "hw.logicalcpu" || key == "hw.physicalcpu" || key == "hw.logicalcpu_max" || key == "hw.physicalcpu_max") return sysctl_int(cpus, out, out_size);
    if (key == "hw.memsize" || key == "hw.physmem") return sysctl_int(physical_memory(), out, out_size);
    if (key == "hw.pagesize") return sysctl_int(16384, out, out_size);
    if (key == "kern.osproductversion") {
        // The deployment target, like every other version check in the engine.
        const uint32_t version = engine_images().back()->minimum_os;
        const std::string text = std::to_string(version >> 16) + "." + std::to_string((version >> 8) & 0xff) + "." + std::to_string(version & 0xff);
        return sysctl_text(text.c_str(), out, out_size);
    }
    if (key == "kern.ostype") return sysctl_text("Darwin", out, out_size);
    if (key == "kern.osrelease") return sysctl_text("23.0.0", out, out_size);
    if (key == "kern.osversion") return sysctl_text("21A329", out, out_size);
    if (key == "kern.hostname") return sysctl_text("localhost", out, out_size);
    if (key == "kern.boottime") {
        timespec now{}, up{};
        clock_gettime(CLOCK_REALTIME, &now);
        clock_gettime(CLOCK_BOOTTIME, &up);
        const timeval boot{now.tv_sec - up.tv_sec, 0};
        return copy_sysctl_value(&boot, sizeof boot, out, out_size);
    }
    errno = ENOENT;
    return -1;
}

int darwin_sysctl(const int* names, unsigned count, void* out, size_t* out_size, const void*, size_t) {
    constexpr int kCtlKern = 1, kCtlHw = 6, kKernBootTime = 21, kKernProc = 14, kHwMachine = 1, kHwModel = 2, kHwNcpu = 3, kHwPageSize = 7, kHwMemSize = 24;
    if (count >= 2 && names[0] == kCtlHw) {
        switch (names[1]) {
            case kHwMachine:
            case kHwModel:
                return sysctl_text(kReportedMachine, out, out_size);
            case kHwNcpu:
                return sysctl_int(sysconf(_SC_NPROCESSORS_ONLN), out, out_size);
            case kHwPageSize:
                return sysctl_int(16384, out, out_size);
            case kHwMemSize:
                return sysctl_int(physical_memory(), out, out_size);
            default:
                break;
        }
    }
    if (count >= 2 && names[0] == kCtlKern && names[1] == kKernBootTime) return darwin_sysctlbyname("kern.boottime", out, out_size, nullptr, 0);
    // A zeroed kinfo_proc says "not being debugged", the one thing apps ask this for.
    if (count >= 2 && names[0] == kCtlKern && names[1] == kKernProc && out != nullptr) {
        memset(out, 0, *out_size);
        return 0;
    }
    errno = ENOENT;
    return -1;
}

std::string executable_path() {
    const auto& images = engine_images();
    return images.empty() ? std::string() : images.back()->path;
}

// dyld lists the executable first, while the engine keeps initialization order with it last.
const MachImage* image_at(uint32_t index) {
    const auto& images = engine_images();
    if (index >= images.size()) return nullptr;
    return index == 0 ? images.back().get() : images[index - 1].get();
}

const MachImage* image_with_header(const void* header) {
    for (const auto& image : engine_images()) {
        if (reinterpret_cast<const void*>(image->base) == header) return image.get();
    }
    return nullptr;
}

const MachImage* image_containing(const void* address) {
    const auto value = reinterpret_cast<uintptr_t>(address);
    for (const auto& image : engine_images()) {
        if (value >= image->base && value < image->base + image->size) return image.get();
    }
    return nullptr;
}

uint8_t* darwin_getsectiondata(const void* header, const char* segment, const char* section, unsigned long* size) {
    const MachImage* image = image_with_header(header);
    const MachSection* found = image != nullptr ? image->section(segment, section) : nullptr;
    if (found == nullptr) {
        if (size != nullptr) *size = 0;
        return nullptr;
    }
    if (size != nullptr) *size = found->size;
    return reinterpret_cast<uint8_t*>(found->address);
}

// This older call answers with the unslid address, which callers then slide themselves.
char* darwin_getsectdatafromheader(const void* header, const char* segment, const char* section, uint64_t* size) {
    const MachImage* image = image_with_header(header);
    const MachSection* found = image != nullptr ? image->section(segment, section) : nullptr;
    if (found == nullptr) {
        if (size != nullptr) *size = 0;
        return nullptr;
    }
    if (size != nullptr) *size = found->size;
    return reinterpret_cast<char*>(found->address - static_cast<uintptr_t>(image->slide));
}

void darwin_register_add_image(void (*callback)(const void*, intptr_t)) {
    for (uint32_t i = 0; const MachImage* image = image_at(i); ++i) callback(reinterpret_cast<const void*>(image->base), image->slide);
}

int darwin_dladdr(const void* address, Dl_info* info) {
    const MachImage* image = image_containing(address);
    if (image != nullptr) {
        info->dli_fname = image->path.c_str();
        info->dli_fbase = reinterpret_cast<void*>(image->base);
        info->dli_sname = nullptr;
        info->dli_saddr = nullptr;
        return 1;
    }
    return dladdr(address, info);
}

int darwin_NSGetExecutablePath(char* out, uint32_t* size) {
    const std::string path = executable_path();
    if (path.size() + 1 > *size) {
        *size = static_cast<uint32_t>(path.size() + 1);
        return -1;
    }
    memcpy(out, path.c_str(), path.size() + 1);
    return 0;
}

const char* darwin_getprogname() {
    static const std::string name = [] {
        const std::string path = executable_path();
        return path.substr(path.rfind('/') + 1);
    }();
    return name.c_str();
}

void* darwin_dlopen(const char* path, int) {
    // System libraries answer through the shims, so any open succeeds with a handle dlsym ignores.
    static char handle;
    (void)path;
    return &handle;
}

constexpr int kKernSuccess = 0, kKernNotSupported = 46, kKernInvalidArgument = 4;
constexpr size_t kMachPageBytes = 16384;
size_t g_vm_page_size = kMachPageBytes;
size_t g_vm_page_mask = kMachPageBytes - 1;
uint32_t g_mach_task_self = 1;
uint64_t g_ndr_record = 0;
void* g_concrete_global_block[32];
void* g_os_log_default[4];

uint64_t monotonic_nanoseconds() {
    timespec now{};
    clock_gettime(CLOCK_MONOTONIC, &now);
    return static_cast<uint64_t>(now.tv_sec) * 1000000000ULL + static_cast<uint64_t>(now.tv_nsec);
}

int darwin_timebase(uint32_t* info) {
    info[0] = 1;
    info[1] = 1;
    return kKernSuccess;
}

int darwin_clock_get_time(uint32_t, int32_t* out) {
    timespec now{};
    clock_gettime(CLOCK_REALTIME, &now);
    out[0] = static_cast<int32_t>(now.tv_sec);
    out[1] = static_cast<int32_t>(now.tv_nsec);
    return kKernSuccess;
}

uint64_t darwin_clock_gettime_nsec_np(int) { return monotonic_nanoseconds(); }

int darwin_vm_allocate(uint32_t, uintptr_t* address, size_t size, int) {
    void* mapped = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (mapped == MAP_FAILED) return kKernInvalidArgument;
    *address = reinterpret_cast<uintptr_t>(mapped);
    return kKernSuccess;
}

int darwin_vm_deallocate(uint32_t, uintptr_t address, size_t size) { return munmap(reinterpret_cast<void*>(address), size) == 0 ? kKernSuccess : kKernInvalidArgument; }

int darwin_semaphore_create(uint32_t, sem_t** out, int, int value) {
    *out = new sem_t;
    sem_init(*out, 0, static_cast<unsigned>(value));
    return kKernSuccess;
}

int darwin_semaphore_destroy(uint32_t, sem_t* semaphore) {
    sem_destroy(semaphore);
    delete semaphore;
    return kKernSuccess;
}

int darwin_host_page_size(uint32_t, size_t* out) {
    *out = kMachPageBytes;
    return kKernSuccess;
}

// Crash reporters and exception handlers are the callers: "unsupported" is an answer they all handle.
int mach_not_supported() { return kKernNotSupported; }
int mach_success() { return kKernSuccess; }
uint32_t mach_port_self() { return static_cast<uint32_t>(gettid()); }

void* null_result() { return nullptr; }
int zero_result() { return 0; }
void no_operation() {}

int darwin_error_result() {
    errno = ENOSYS;
    return -1;
}

void* darwin_os_log_create(const char*, const char*) { return g_os_log_default; }

// Blocks capture objects with these helpers, per the Clang block ABI.
constexpr int kFieldObject = 3, kFieldBlock = 7, kFieldByref = 8, kFieldWeak = 16, kFieldCaller = 128;
constexpr int kBlockFieldMask = kFieldObject | kFieldBlock | kFieldByref;
constexpr int32_t kByrefNeedsFree = 1 << 24;
constexpr int32_t kByrefHasHelpers = 1 << 25;

struct Byref {
    void* isa;
    Byref* forwarding;
    int32_t flags;
    int32_t size;
    void (*keep)(void*, void*);
    void (*destroy)(void*);
};

using ObjectFunction = void* (*)(void*);

void* retain_object(void* object) {
    static const auto retain = reinterpret_cast<ObjectFunction>(resolve_symbol("swift_retain"));
    return retain(object);
}

void release_object(void* object) {
    static const auto release = reinterpret_cast<ObjectFunction>(resolve_symbol("swift_release"));
    release(object);
}

extern "C" void* darwin_block_copy(const void*);
extern "C" void darwin_block_release(const void*);

Byref* copy_byref(Byref* source) {
    Byref* current = source->forwarding;
    if (current->flags & kByrefNeedsFree) {
        ++current->flags;
        return current;
    }
    auto* copy = static_cast<Byref*>(malloc(static_cast<size_t>(source->size)));
    memcpy(copy, source, static_cast<size_t>(source->size));
    copy->forwarding = copy;
    copy->flags = (source->flags & ~0xffff) | kByrefNeedsFree | 2;
    source->forwarding = copy;
    if (source->flags & kByrefHasHelpers) source->keep(copy, source);
    return copy;
}

void darwin_block_object_assign(void** destination, void* object, int flags) {
    if (flags & (kFieldCaller | kFieldWeak)) {
        *destination = object;
        return;
    }
    switch (flags & kBlockFieldMask) {
        case kFieldObject:
            if (object != nullptr) retain_object(object);
            *destination = object;
            break;
        case kFieldBlock:
            *destination = darwin_block_copy(object);
            break;
        case kFieldByref:
            *destination = copy_byref(static_cast<Byref*>(object));
            break;
        default:
            *destination = object;
            break;
    }
}

void darwin_block_object_dispose(void* object, int flags) {
    if (flags & (kFieldCaller | kFieldWeak)) return;
    switch (flags & kBlockFieldMask) {
        case kFieldObject:
            if (object != nullptr) release_object(object);
            break;
        case kFieldBlock:
            darwin_block_release(object);
            break;
        case kFieldByref: {
            Byref* current = static_cast<Byref*>(object)->forwarding;
            if (!(current->flags & kByrefNeedsFree)) break;
            current->flags -= 2;
            if ((current->flags & 0xffff) != 0) break;
            if (current->flags & kByrefHasHelpers) current->destroy(current);
            free(current);
            break;
        }
        default:
            break;
    }
}

int32_t atomic_add32(int32_t amount, std::atomic<int32_t>* value) { return value->fetch_add(amount) + amount; }

bool atomic_compare_pointer(void* expected, void* replacement, std::atomic<void*>* value) { return value->compare_exchange_strong(expected, replacement); }

void spin_lock(std::atomic<int32_t>* lock) {
    int32_t expected = 0;
    while (!lock->compare_exchange_weak(expected, 1, std::memory_order_acquire)) {
        expected = 0;
        std::this_thread::yield();
    }
}

void spin_unlock(std::atomic<int32_t>* lock) { lock->store(0, std::memory_order_release); }

}  // namespace

#define DARWIN_FN(name) {#name, reinterpret_cast<void*>(name)}
#define DARWIN_AS(name, target) {#name, reinterpret_cast<void*>(target)}

void add_darwin_system_symbols(DarwinSymbolTable& table) {
    table.insert({
        DARWIN_AS(__error, darwin_error),
        // Guest frames carry compact unwind info, which no unwinder here reads, so an exception can't be caught yet.
        DARWIN_AS(_Unwind_RaiseException, +[](void*) -> int {
            engine_log("STOP: guest exception or Rust panic: unwinding isn't implemented");
            abort(); }),
        {"$s6Darwin5errnos5Int32Vvg", reinterpret_cast<void*>(+[]() -> int { return *darwin_error(); })},
        {"$s6Darwin7SIG_DFLys5Int32VXCSgvg", reinterpret_cast<void*>(null_result)},
        // The enum's cases are errno 1 to 106 in order, so its tag is one less and the next tag means nil.
        {"$s6Darwin14POSIXErrorCodeO8rawValueACSgs5Int32V_tcfC", reinterpret_cast<void*>(+[](int32_t raw) -> uint32_t {
             constexpr int32_t kLastPosixError = 106;
             return raw >= 1 && raw <= kLastPosixError ? static_cast<uint32_t>(raw - 1) : static_cast<uint32_t>(kLastPosixError); })},
        DARWIN_AS(sysconf, darwin_sysconf),
        DARWIN_AS(sysctl, darwin_sysctl),
        DARWIN_AS(sysctlbyname, darwin_sysctlbyname),
        DARWIN_AS(getpagesize, +[]() -> int { return kMachPageBytes; }),
        DARWIN_AS(_dyld_image_count, +[]() -> uint32_t { return static_cast<uint32_t>(engine_images().size()); }),
        DARWIN_AS(_dyld_get_image_header, +[](uint32_t index) -> const void* {
            const MachImage* image = image_at(index);
            return image != nullptr ? reinterpret_cast<const void*>(image->base) : nullptr; }),
        DARWIN_AS(_dyld_get_image_name, +[](uint32_t index) -> const char* {
            const MachImage* image = image_at(index);
            return image != nullptr ? image->path.c_str() : nullptr; }),
        DARWIN_AS(_dyld_get_image_vmaddr_slide, +[](uint32_t index) -> intptr_t {
            const MachImage* image = image_at(index);
            return image != nullptr ? image->slide : 0; }),
        DARWIN_AS(_dyld_register_func_for_add_image, darwin_register_add_image),
        DARWIN_AS(_dyld_register_func_for_remove_image, no_operation),
        DARWIN_AS(getsectiondata, darwin_getsectiondata),
        DARWIN_AS(getsectdatafromheader, darwin_getsectdatafromheader),
        DARWIN_AS(getsectdatafromheader_64, darwin_getsectdatafromheader),
        DARWIN_AS(dladdr, darwin_dladdr),
        DARWIN_AS(dlopen, darwin_dlopen),
        DARWIN_AS(dlclose, zero_result),
        DARWIN_AS(dlerror, null_result),
        DARWIN_AS(_NSGetExecutablePath, darwin_NSGetExecutablePath),
        DARWIN_AS(getprogname, darwin_getprogname),
        DARWIN_AS(issetugid, zero_result),
        DARWIN_AS(sigaction, zero_result),
        DARWIN_AS(signal, null_result),
        DARWIN_AS(sigaltstack, zero_result),
        DARWIN_AS(backtrace, zero_result),
        DARWIN_AS(backtrace_symbols, null_result),
        DARWIN_AS(backtrace_symbols_fd, no_operation),
        DARWIN_AS(tcgetattr, darwin_error_result),
        DARWIN_AS(tcsetattr, darwin_error_result),
        DARWIN_AS(getmntinfo, zero_result),
        DARWIN_AS(glob, +[]() -> int { constexpr int kGlobNoMatch = -3; return kGlobNoMatch; }),
        DARWIN_AS(globfree, no_operation),
        DARWIN_AS(getpwuid_r, +[](uid_t, void*, char*, size_t, void** result) -> int {
            *result = nullptr;
            return 0; }),
        DARWIN_AS(mach_absolute_time, monotonic_nanoseconds),
        DARWIN_AS(mach_timebase_info, darwin_timebase),
        DARWIN_AS(clock_gettime_nsec_np, darwin_clock_gettime_nsec_np),
        DARWIN_AS(clock_get_time, darwin_clock_get_time),
        DARWIN_AS(mach_host_self, mach_port_self),
        DARWIN_AS(mach_thread_self, mach_port_self),
        DARWIN_AS(host_get_clock_service, mach_success),
        DARWIN_AS(host_page_size, darwin_host_page_size),
        DARWIN_AS(host_statistics, mach_not_supported),
        DARWIN_AS(vm_allocate, darwin_vm_allocate),
        DARWIN_AS(vm_deallocate, darwin_vm_deallocate),
        DARWIN_AS(vm_map, mach_not_supported),
        DARWIN_AS(vm_protect, mach_not_supported),
        DARWIN_AS(vm_read_overwrite, mach_not_supported),
        DARWIN_AS(vm_region_64, mach_not_supported),
        DARWIN_AS(mach_make_memory_entry_64, mach_not_supported),
        DARWIN_AS(mach_msg, mach_not_supported),
        DARWIN_AS(mach_port_allocate, mach_not_supported),
        DARWIN_AS(mach_port_deallocate, mach_success),
        DARWIN_AS(mach_port_insert_right, mach_not_supported),
        DARWIN_AS(mach_port_mod_refs, mach_success),
        DARWIN_AS(mach_port_move_member, mach_not_supported),
        DARWIN_AS(mach_port_request_notification, mach_not_supported),
        DARWIN_AS(exception_raise, mach_not_supported),
        DARWIN_AS(exception_raise_state, mach_not_supported),
        DARWIN_AS(exception_raise_state_identity, mach_not_supported),
        DARWIN_AS(task_get_exception_ports, mach_not_supported),
        DARWIN_AS(task_swap_exception_ports, mach_not_supported),
        DARWIN_AS(task_threads, mach_not_supported),
        DARWIN_AS(thread_get_exception_ports, mach_not_supported),
        DARWIN_AS(thread_get_state, mach_not_supported),
        DARWIN_AS(thread_resume, mach_not_supported),
        DARWIN_AS(thread_suspend, mach_not_supported),
        DARWIN_AS(thread_swap_exception_ports, mach_not_supported),
        DARWIN_AS(semaphore_create, darwin_semaphore_create),
        DARWIN_AS(semaphore_destroy, darwin_semaphore_destroy),
        DARWIN_AS(semaphore_signal, +[](sem_t* semaphore) -> int { return sem_post(semaphore) == 0 ? kKernSuccess : kKernInvalidArgument; }),
        DARWIN_AS(semaphore_wait, +[](sem_t* semaphore) -> int { return sem_wait(semaphore) == 0 ? kKernSuccess : kKernInvalidArgument; }),
        DARWIN_AS(malloc_get_all_zones, mach_not_supported),
        DARWIN_AS(malloc_zone_from_ptr, null_result),
        DARWIN_AS(asl_new, null_result),
        DARWIN_AS(asl_open, null_result),
        DARWIN_AS(asl_search, null_result),
        DARWIN_AS(asl_next, null_result),
        DARWIN_AS(asl_get, null_result),
        DARWIN_AS(asl_set, zero_result),
        DARWIN_AS(asl_set_query, zero_result),
        DARWIN_AS(asl_send, zero_result),
        DARWIN_AS(asl_free, no_operation),
        DARWIN_AS(asl_release, no_operation),
        DARWIN_AS(notify_register_dispatch, zero_result),
        DARWIN_AS(notify_cancel, zero_result),
        DARWIN_AS(DNSServiceNATPortMappingCreate, +[]() -> int { constexpr int kDnsServiceUnsupported = -65544; return kDnsServiceUnsupported; }),
        DARWIN_AS(DNSServiceProcessResult, +[]() -> int { constexpr int kDnsServiceUnsupported = -65544; return kDnsServiceUnsupported; }),
        DARWIN_AS(DNSServiceRefDeallocate, no_operation),
        DARWIN_AS(DNSServiceRefSockFD, +[]() -> int { return -1; }),
        DARWIN_AS(_Block_object_assign, darwin_block_object_assign),
        DARWIN_AS(_Block_object_dispose, darwin_block_object_dispose),
        DARWIN_AS(OSAtomicAdd32Barrier, atomic_add32),
        DARWIN_AS(OSAtomicCompareAndSwapPtrBarrier, atomic_compare_pointer),
        DARWIN_AS(OSMemoryBarrier, +[]() { std::atomic_thread_fence(std::memory_order_seq_cst); }),
        DARWIN_AS(OSSpinLockLock, spin_lock),
        DARWIN_AS(OSSpinLockUnlock, spin_unlock),
        DARWIN_AS(os_log_create, darwin_os_log_create),
        DARWIN_AS(_os_log_error_impl, no_operation),
        DARWIN_AS(os_release, no_operation),
        DARWIN_AS(voucher_adopt, null_result),
        DARWIN_AS(qos_class_self, +[]() -> uint32_t { constexpr uint32_t kQosUserInteractive = 0x21; return kQosUserInteractive; }),
        {"vm_page_size", &g_vm_page_size},
        {"vm_page_mask", &g_vm_page_mask},
        {"mach_task_self_", &g_mach_task_self},
        {"NDR_record", &g_ndr_record},
        {"_NSConcreteGlobalBlock", g_concrete_global_block},
        {"_os_log_default", g_os_log_default},
        DARWIN_FN(getpid),
        DARWIN_FN(getppid),
        DARWIN_FN(getuid),
        DARWIN_FN(geteuid),
        DARWIN_FN(getgid),
        DARWIN_FN(getegid),
        DARWIN_FN(getrusage),
        DARWIN_FN(setrlimit),
        DARWIN_FN(kill),
        DARWIN_FN(raise),
        DARWIN_FN(nice),
        DARWIN_FN(sleep),
        DARWIN_FN(usleep),
        DARWIN_FN(nanosleep),
        DARWIN_FN(time),
        DARWIN_FN(mktime),
        DARWIN_FN(timegm),
        DARWIN_FN(gmtime),
        DARWIN_FN(gmtime_r),
        DARWIN_FN(localtime),
        DARWIN_FN(localtime_r),
        DARWIN_FN(strftime),
        DARWIN_FN(waitpid),
    });
}
