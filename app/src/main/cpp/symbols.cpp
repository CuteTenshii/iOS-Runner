#include "symbols.h"

#include <malloc.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <string_view>
#include <unordered_map>

#include "codable_shim.h"
#include "combine_shim.h"
#include "concurrency_shim.h"
#include "coredata_shim.h"
#include "dispatch_shim.h"
#include "engine.h"
#include "foundation_shim.h"
#include "objc_runtime.h"
#include "regex_shim.h"
#include "stdlib_shim.h"
#include "swift_runtime.h"
#include "swiftui_shim.h"
#include "type_system.h"
#include "swift_abi.h"

extern "C" {
void darwin_chkstk();
uintptr_t darwin_stack_guard = 0x5bd1e9955bd1e995ULL;

[[noreturn]] void darwin_stack_chk_fail() {
    engine_log("STOP: guest stack protector failure");
    abort();
}
// Blocks, per the Clang block ABI.
struct BlockDescriptor {
    uintptr_t reserved;
    uintptr_t size;
    void (*copy)(void*, const void*);
    void (*dispose)(const void*);
};

struct BlockLayout {
    void* isa;
    int32_t flags;
    int32_t references;
    void* invoke;
    BlockDescriptor* descriptor;
};

constexpr int32_t kBlockHasCopyDispose = 1 << 25;
constexpr int32_t kBlockIsGlobal = 1 << 28;
constexpr int32_t kBlockIsHeap = 1 << 24;
void* darwin_stack_block_class[32];
void* darwin_malloc_block_class[32];

void* darwin_block_copy(const void* raw) {
    if (raw == nullptr) return nullptr;
    auto* block = static_cast<BlockLayout*>(const_cast<void*>(raw));
    if (block->flags & kBlockIsGlobal) return block;
    if (block->flags & kBlockIsHeap) {
        ++block->references;
        return block;
    }
    auto* copy = static_cast<BlockLayout*>(malloc(block->descriptor->size));
    memcpy(copy, block, block->descriptor->size);
    copy->isa = darwin_malloc_block_class;
    copy->flags |= kBlockIsHeap;
    copy->references = 1;
    if (block->flags & kBlockHasCopyDispose) block->descriptor->copy(copy, block);
    return copy;
}

void darwin_block_release(const void* raw) {
    if (raw == nullptr) return;
    auto* block = static_cast<BlockLayout*>(const_cast<void*>(raw));
    if (!(block->flags & kBlockIsHeap) || --block->references > 0) return;
    if (block->flags & kBlockHasCopyDispose) block->descriptor->dispose(block);
    free(block);
}

uint8_t darwin_log_type_fault() { return 0x11; }
bool darwin_log_type_enabled(void*, uint8_t) { return false; }
void darwin_log_impl(...) {}

// The app is built for iOS 27 and this runtime answers as iOS 27, so every availability check passes.
bool darwin_availability_version_check(uint32_t, const void*) { return true; }

// The guest runs on one thread, so a lock is never contended and a trylock always succeeds.
void darwin_lock_noop(void*) {}
bool darwin_trylock_succeeds(void*) { return true; }
int darwin_mutex_ok(void*, ...) { return 0; }

double darwin_absolute_time() {
    timespec now{};
    clock_gettime(CLOCK_REALTIME, &now);
    return static_cast<double>(now.tv_sec) + static_cast<double>(now.tv_nsec) / 1e9 - kSecondsFrom1970To2001;
}

void* darwin_dlsym(void*, const char* name) { return resolve_symbol(name); }

// The runner presents itself as the app's deployment target: every app must work there, without weak imports.
uint32_t g_reported_os = 0;

int32_t platform_version_at_least(uint32_t, uint32_t major, uint32_t minor, uint32_t patch) {
    return ((major << 16) | (minor << 8) | patch) <= g_reported_os ? 1 : 0;
}

size_t darwin_malloc_size(void* pointer) { return malloc_usable_size(pointer); }
void darwin_bzero(void* pointer, size_t size) { memset(pointer, 0, size); }
}

__asm__(R"(
.text
.globl darwin_chkstk
darwin_chkstk:
    ret
)");

namespace {

const std::unordered_map<std::string_view, void*>& symbol_table() {
    static const std::unordered_map<std::string_view, void*> table = {
        {"__chkstk_darwin", reinterpret_cast<void*>(darwin_chkstk)},
        {"__stack_chk_guard", &darwin_stack_guard},
        {"__stack_chk_fail", reinterpret_cast<void*>(darwin_stack_chk_fail)},
        {"_Block_copy", reinterpret_cast<void*>(darwin_block_copy)},
        {"_Block_release", reinterpret_cast<void*>(darwin_block_release)},
        {"_NSConcreteStackBlock", darwin_stack_block_class},
        {"$sSo13os_log_type_ta0A0E5faultABvgZ", reinterpret_cast<void*>(darwin_log_type_fault)},
        {"os_log_type_enabled", reinterpret_cast<void*>(darwin_log_type_enabled)},
        {"_os_log_impl", reinterpret_cast<void*>(darwin_log_impl)},
        {"_availability_version_check", reinterpret_cast<void*>(darwin_availability_version_check)},
        {"acos", reinterpret_cast<void*>(static_cast<double (*)(double)>(::acos))},
        {"asin", reinterpret_cast<void*>(static_cast<double (*)(double)>(::asin))},
        {"atan", reinterpret_cast<void*>(static_cast<double (*)(double)>(::atan))},
        {"acosh", reinterpret_cast<void*>(static_cast<double (*)(double)>(::acosh))},
        {"asinh", reinterpret_cast<void*>(static_cast<double (*)(double)>(::asinh))},
        {"atanh", reinterpret_cast<void*>(static_cast<double (*)(double)>(::atanh))},
        {"cos", reinterpret_cast<void*>(static_cast<double (*)(double)>(::cos))},
        {"cosh", reinterpret_cast<void*>(static_cast<double (*)(double)>(::cosh))},
        {"sin", reinterpret_cast<void*>(static_cast<double (*)(double)>(::sin))},
        {"sinh", reinterpret_cast<void*>(static_cast<double (*)(double)>(::sinh))},
        {"tan", reinterpret_cast<void*>(static_cast<double (*)(double)>(::tan))},
        {"tanh", reinterpret_cast<void*>(static_cast<double (*)(double)>(::tanh))},
        {"exp", reinterpret_cast<void*>(static_cast<double (*)(double)>(::exp))},
        {"exp2", reinterpret_cast<void*>(static_cast<double (*)(double)>(::exp2))},
        {"expm1", reinterpret_cast<void*>(static_cast<double (*)(double)>(::expm1))},
        {"log", reinterpret_cast<void*>(static_cast<double (*)(double)>(::log))},
        {"log10", reinterpret_cast<void*>(static_cast<double (*)(double)>(::log10))},
        {"log1p", reinterpret_cast<void*>(static_cast<double (*)(double)>(::log1p))},
        {"log2", reinterpret_cast<void*>(static_cast<double (*)(double)>(::log2))},
        {"cbrt", reinterpret_cast<void*>(static_cast<double (*)(double)>(::cbrt))},
        {"erf", reinterpret_cast<void*>(static_cast<double (*)(double)>(::erf))},
        {"erfc", reinterpret_cast<void*>(static_cast<double (*)(double)>(::erfc))},
        {"sqrt", reinterpret_cast<void*>(static_cast<double (*)(double)>(::sqrt))},
        {"ceil", reinterpret_cast<void*>(static_cast<double (*)(double)>(::ceil))},
        {"floor", reinterpret_cast<void*>(static_cast<double (*)(double)>(::floor))},
        {"round", reinterpret_cast<void*>(static_cast<double (*)(double)>(::round))},
        {"trunc", reinterpret_cast<void*>(static_cast<double (*)(double)>(::trunc))},
        {"fabs", reinterpret_cast<void*>(static_cast<double (*)(double)>(::fabs))},
        {"rint", reinterpret_cast<void*>(static_cast<double (*)(double)>(::rint))},
        {"nearbyint", reinterpret_cast<void*>(static_cast<double (*)(double)>(::nearbyint))},
        {"acosf", reinterpret_cast<void*>(static_cast<float (*)(float)>(::acosf))},
        {"asinf", reinterpret_cast<void*>(static_cast<float (*)(float)>(::asinf))},
        {"atanf", reinterpret_cast<void*>(static_cast<float (*)(float)>(::atanf))},
        {"acoshf", reinterpret_cast<void*>(static_cast<float (*)(float)>(::acoshf))},
        {"asinhf", reinterpret_cast<void*>(static_cast<float (*)(float)>(::asinhf))},
        {"atanhf", reinterpret_cast<void*>(static_cast<float (*)(float)>(::atanhf))},
        {"cosf", reinterpret_cast<void*>(static_cast<float (*)(float)>(::cosf))},
        {"coshf", reinterpret_cast<void*>(static_cast<float (*)(float)>(::coshf))},
        {"sinf", reinterpret_cast<void*>(static_cast<float (*)(float)>(::sinf))},
        {"sinhf", reinterpret_cast<void*>(static_cast<float (*)(float)>(::sinhf))},
        {"tanf", reinterpret_cast<void*>(static_cast<float (*)(float)>(::tanf))},
        {"tanhf", reinterpret_cast<void*>(static_cast<float (*)(float)>(::tanhf))},
        {"expf", reinterpret_cast<void*>(static_cast<float (*)(float)>(::expf))},
        {"exp2f", reinterpret_cast<void*>(static_cast<float (*)(float)>(::exp2f))},
        {"expm1f", reinterpret_cast<void*>(static_cast<float (*)(float)>(::expm1f))},
        {"logf", reinterpret_cast<void*>(static_cast<float (*)(float)>(::logf))},
        {"log10f", reinterpret_cast<void*>(static_cast<float (*)(float)>(::log10f))},
        {"log1pf", reinterpret_cast<void*>(static_cast<float (*)(float)>(::log1pf))},
        {"log2f", reinterpret_cast<void*>(static_cast<float (*)(float)>(::log2f))},
        {"cbrtf", reinterpret_cast<void*>(static_cast<float (*)(float)>(::cbrtf))},
        {"erff", reinterpret_cast<void*>(static_cast<float (*)(float)>(::erff))},
        {"erfcf", reinterpret_cast<void*>(static_cast<float (*)(float)>(::erfcf))},
        {"sqrtf", reinterpret_cast<void*>(static_cast<float (*)(float)>(::sqrtf))},
        {"ceilf", reinterpret_cast<void*>(static_cast<float (*)(float)>(::ceilf))},
        {"floorf", reinterpret_cast<void*>(static_cast<float (*)(float)>(::floorf))},
        {"roundf", reinterpret_cast<void*>(static_cast<float (*)(float)>(::roundf))},
        {"truncf", reinterpret_cast<void*>(static_cast<float (*)(float)>(::truncf))},
        {"fabsf", reinterpret_cast<void*>(static_cast<float (*)(float)>(::fabsf))},
        {"rintf", reinterpret_cast<void*>(static_cast<float (*)(float)>(::rintf))},
        {"nearbyintf", reinterpret_cast<void*>(static_cast<float (*)(float)>(::nearbyintf))},
        {"atan2", reinterpret_cast<void*>(static_cast<double (*)(double, double)>(::atan2))},
        {"pow", reinterpret_cast<void*>(static_cast<double (*)(double, double)>(::pow))},
        {"hypot", reinterpret_cast<void*>(static_cast<double (*)(double, double)>(::hypot))},
        {"fmod", reinterpret_cast<void*>(static_cast<double (*)(double, double)>(::fmod))},
        {"fmin", reinterpret_cast<void*>(static_cast<double (*)(double, double)>(::fmin))},
        {"fmax", reinterpret_cast<void*>(static_cast<double (*)(double, double)>(::fmax))},
        {"atan2f", reinterpret_cast<void*>(static_cast<float (*)(float, float)>(::atan2f))},
        {"powf", reinterpret_cast<void*>(static_cast<float (*)(float, float)>(::powf))},
        {"hypotf", reinterpret_cast<void*>(static_cast<float (*)(float, float)>(::hypotf))},
        {"fmodf", reinterpret_cast<void*>(static_cast<float (*)(float, float)>(::fmodf))},
        {"fminf", reinterpret_cast<void*>(static_cast<float (*)(float, float)>(::fminf))},
        {"fmaxf", reinterpret_cast<void*>(static_cast<float (*)(float, float)>(::fmaxf))},
        {"CFAbsoluteTimeGetCurrent", reinterpret_cast<void*>(darwin_absolute_time)},
        {"arc4random", reinterpret_cast<void*>(arc4random)},
        {"access", reinterpret_cast<void*>(access)},
        {"chmod", reinterpret_cast<void*>(chmod)},
        {"close", reinterpret_cast<void*>(close)},
        {"dup", reinterpret_cast<void*>(dup)},
        {"dup2", reinterpret_cast<void*>(dup2)},
        {"fsync", reinterpret_cast<void*>(fsync)},
        {"getenv", reinterpret_cast<void*>(getenv)},
        {"getcwd", reinterpret_cast<void*>(getcwd)},
        {"atof", reinterpret_cast<void*>(atof)},
        {"calloc", reinterpret_cast<void*>(calloc)},
        {"abort", reinterpret_cast<void*>(abort)},
        {"exit", reinterpret_cast<void*>(exit)},
        {"gettimeofday", reinterpret_cast<void*>(gettimeofday)},
        {"clock_gettime", reinterpret_cast<void*>(clock_gettime)},
        {"os_unfair_lock_lock", reinterpret_cast<void*>(darwin_lock_noop)},
        {"os_unfair_lock_unlock", reinterpret_cast<void*>(darwin_lock_noop)},
        {"os_unfair_lock_trylock", reinterpret_cast<void*>(darwin_trylock_succeeds)},
        {"os_unfair_lock_assert_owner", reinterpret_cast<void*>(darwin_lock_noop)},
        {"os_unfair_lock_assert_not_owner", reinterpret_cast<void*>(darwin_lock_noop)},
        {"pthread_mutex_init", reinterpret_cast<void*>(darwin_mutex_ok)},
        {"pthread_mutex_destroy", reinterpret_cast<void*>(darwin_mutex_ok)},
        {"pthread_mutex_lock", reinterpret_cast<void*>(darwin_mutex_ok)},
        {"pthread_mutex_unlock", reinterpret_cast<void*>(darwin_mutex_ok)},
        {"pthread_mutex_trylock", reinterpret_cast<void*>(darwin_mutex_ok)},
        {"pthread_mutexattr_init", reinterpret_cast<void*>(darwin_mutex_ok)},
        {"pthread_mutexattr_settype", reinterpret_cast<void*>(darwin_mutex_ok)},
        {"pthread_mutexattr_destroy", reinterpret_cast<void*>(darwin_mutex_ok)},
        {"dlsym", reinterpret_cast<void*>(darwin_dlsym)},
        {"__isPlatformVersionAtLeast", reinterpret_cast<void*>(platform_version_at_least)},
        {"fopen", reinterpret_cast<void*>(fopen)},
        {"fclose", reinterpret_cast<void*>(fclose)},
        {"fread", reinterpret_cast<void*>(fread)},
        {"fseek", reinterpret_cast<void*>(fseek)},
        {"ftell", reinterpret_cast<void*>(ftell)},
        {"rewind", reinterpret_cast<void*>(rewind)},
        {"malloc", reinterpret_cast<void*>(malloc)},
        {"free", reinterpret_cast<void*>(free)},
        {"malloc_size", reinterpret_cast<void*>(darwin_malloc_size)},
        {"memcpy", reinterpret_cast<void*>(memcpy)},
        {"memmove", reinterpret_cast<void*>(memmove)},
        {"bzero", reinterpret_cast<void*>(darwin_bzero)},
    };
    return table;
}

// A framework enum's case tag only has to be distinct: its values are opaque tag words to the guest and to these shims.
void* framework_enum_case_tag(std::string_view name) {
    constexpr std::string_view kCaseTagSuffix = "WC";
    if (name.size() <= kCaseTagSuffix.size() || name.substr(0, 2) != "$s" || name.substr(name.size() - kCaseTagSuffix.size()) != kCaseTagSuffix) return nullptr;
    static std::unordered_map<std::string, int32_t*> tags;
    int32_t*& tag = tags[std::string(name)];
    if (tag == nullptr) tag = new int32_t(static_cast<int32_t>(tags.size()));
    return tag;
}

}  // namespace

void* resolve_symbol(const char* name) {
    const auto& table = symbol_table();
    const auto found = table.find(name);
    if (found != table.end()) return found->second;
    if (void* symbol = swift_runtime_symbol(name)) return symbol;
    if (void* symbol = swiftui_symbol(name)) return symbol;
    if (void* symbol = objc_runtime_symbol(name)) return symbol;
    if (void* symbol = foundation_symbol(name)) return symbol;
    if (void* symbol = codable_symbol(name)) return symbol;
    if (void* symbol = coredata_symbol(name)) return symbol;
    if (void* symbol = dispatch_symbol(name)) return symbol;
    if (void* symbol = combine_symbol(name)) return symbol;
    if (void* symbol = regex_symbol(name)) return symbol;
    if (void* symbol = stdlib_symbol(name)) return symbol;
    if (void* symbol = concurrency_symbol(name)) return symbol;
    if (void* symbol = type_system_symbol(name)) return symbol;
    return framework_enum_case_tag(name);
}

void runtime_attach_image(const MachImage& image) {
    g_reported_os = image.minimum_os;
    foundation_register_classes();
    coredata_register_classes();
    objc_attach_image(image);
    coredata_attach_image(image);
    swiftui_attach_image(image);
}
