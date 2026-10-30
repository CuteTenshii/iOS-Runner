#include "symbols.h"

#include <malloc.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string_view>
#include <unordered_map>

#include "engine.h"
#include "concurrency_shim.h"
#include "foundation_shim.h"
#include "objc_runtime.h"
#include "stdlib_shim.h"
#include "swift_runtime.h"
#include "swiftui_shim.h"
#include "type_system.h"

extern "C" {
void darwin_chkstk();
uintptr_t darwin_stack_guard = 0x5bd1e9955bd1e995ULL;

[[noreturn]] void darwin_stack_chk_fail() {
    engine_log("STOP: guest stack protector failure");
    abort();
}
// The app is built for iOS 27 and this runtime answers as iOS 27, so every availability check passes.
bool darwin_availability_version_check(uint32_t, const void*) { return true; }

void* darwin_dlsym(void*, const char* name) { return resolve_symbol(name); }

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
        {"_availability_version_check", reinterpret_cast<void*>(darwin_availability_version_check)},
        {"dlsym", reinterpret_cast<void*>(darwin_dlsym)},
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

}  // namespace

void* resolve_symbol(const char* name) {
    const auto& table = symbol_table();
    const auto found = table.find(name);
    if (found != table.end()) return found->second;
    if (void* symbol = swift_runtime_symbol(name)) return symbol;
    if (void* symbol = swiftui_symbol(name)) return symbol;
    if (void* symbol = objc_runtime_symbol(name)) return symbol;
    if (void* symbol = foundation_symbol(name)) return symbol;
    if (void* symbol = stdlib_symbol(name)) return symbol;
    if (void* symbol = concurrency_symbol(name)) return symbol;
    return type_system_symbol(name);
}

void runtime_attach_image(const MachImage& image) {
    foundation_register_classes();
    swiftui_attach_image(image);
}
