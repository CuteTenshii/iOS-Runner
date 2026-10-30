#include "engine.h"

#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <sys/mman.h>
#include <ucontext.h>
#include <unistd.h>

#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>

#include "macho.h"
#include "symbols.h"

#ifndef IOSRUNNER_HOST
#include <android/log.h>
#endif

namespace {

constexpr size_t kGuestStackSize = 64 * 1024 * 1024;
constexpr size_t kSignalStackSize = 256 * 1024;

int g_log_fd = -1;
std::string g_bundle_dir;
MachImage g_image;

void write_log_line(const char* text, size_t length) {
    if (g_log_fd >= 0) {
        (void)!write(g_log_fd, text, length);
        (void)!write(g_log_fd, "\n", 1);
    }
#ifndef IOSRUNNER_HOST
    __android_log_write(ANDROID_LOG_INFO, "iOSRunner", text);
#else
    (void)!write(STDERR_FILENO, text, length);
    (void)!write(STDERR_FILENO, "\n", 1);
#endif
}

const char* signal_name(int number) {
    switch (number) {
        case SIGSEGV: return "SIGSEGV";
        case SIGBUS: return "SIGBUS";
        case SIGILL: return "SIGILL";
        case SIGTRAP: return "SIGTRAP";
        case SIGABRT: return "SIGABRT";
        case SIGFPE: return "SIGFPE";
        default: return "signal";
    }
}

void describe_address(char* out, size_t size, uintptr_t address) {
    if (address >= g_image.base && address < g_image.base + g_image.size) {
        snprintf(out, size, "app+0x%zx", static_cast<size_t>(address - g_image.base));
    } else {
        snprintf(out, size, "0x%zx", static_cast<size_t>(address));
    }
}

void fault_handler(int number, siginfo_t* info, void* raw_context) {
    auto* context = static_cast<ucontext_t*>(raw_context);
    const auto& machine = context->uc_mcontext;
    const auto fault = reinterpret_cast<uintptr_t>(info->si_addr);
    const uintptr_t pc = machine.pc;
    const uintptr_t lr = machine.regs[30];

    char pc_text[48], lr_text[48];
    describe_address(pc_text, sizeof pc_text, pc);
    describe_address(lr_text, sizeof lr_text, lr);

    if (const char* called = unresolved_symbol_at(pc)) {
        engine_log("STOP: called unimplemented function %s (from %s)", called, lr_text);
    } else if (const char* read = unresolved_symbol_at(fault)) {
        engine_log("STOP: accessed unimplemented data %s (at %s, called from %s)", read, pc_text, lr_text);
    } else {
        engine_log("CRASH: %s at %s, address 0x%zx, lr %s", signal_name(number), pc_text, static_cast<size_t>(fault), lr_text);
    }
    uintptr_t frame = machine.regs[29];
    for (int depth = 0; depth < 16 && frame != 0 && (frame & 7) == 0; ++depth) {
        uintptr_t next, ret;
        if (frame < 0x1000) break;
        memcpy(&next, reinterpret_cast<void*>(frame), sizeof next);
        memcpy(&ret, reinterpret_cast<void*>(frame + 8), sizeof ret);
        char ret_text[48];
        describe_address(ret_text, sizeof ret_text, ret);
        engine_log("  #%d %s", depth, ret_text);
        if (next <= frame) break;
        frame = next;
    }
    _exit(128 + number);
}

void install_fault_handlers() {
    stack_t alternate {};
    alternate.ss_sp = mmap(nullptr, kSignalStackSize, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    alternate.ss_size = kSignalStackSize;
    sigaltstack(&alternate, nullptr);

    struct sigaction action {};
    action.sa_sigaction = fault_handler;
    action.sa_flags = SA_SIGINFO | SA_ONSTACK;
    sigemptyset(&action.sa_mask);
    for (int number : {SIGSEGV, SIGBUS, SIGILL, SIGTRAP, SIGABRT, SIGFPE}) sigaction(number, &action, nullptr);
}

struct GuestArguments {
    std::string bundle_dir;
    std::string executable;
    int status = 1;
};

void* guest_thread(void* raw) {
    auto* arguments = static_cast<GuestArguments*>(raw);
    install_fault_handlers();

    const std::string binary = arguments->bundle_dir + "/" + arguments->executable;
    std::string error;
    if (!load_macho(binary, resolve_symbol, g_image, error)) {
        engine_log("LOAD FAILED: %s", error.c_str());
        return nullptr;
    }

    std::map<std::string, std::pair<int, int>> per_library;
    int missing = 0;
    for (const auto& import : g_image.imports) {
        auto& counts = per_library[import.library.empty() ? "(flat)" : import.library];
        counts.second++;
        if (import.resolved) counts.first++;
        else missing++;
    }
    engine_log("LOADED: %s at 0x%zx, entry app+0x%zx", arguments->executable.c_str(), static_cast<size_t>(g_image.base),
               static_cast<size_t>(g_image.entry - g_image.base));
    engine_log("IMPORTS: %zu total, %d not implemented yet", g_image.imports.size(), missing);
    for (const auto& [library, counts] : per_library) {
        engine_log("  %s: %d/%d", library.c_str(), counts.first, counts.second);
    }

    runtime_attach_image(g_image);

    using GuestMain = int (*)(int, const char**, const char**, const char**);
    const char* guest_argv[] = {binary.c_str(), nullptr};
    const char* guest_envp[] = {nullptr};
    const char* guest_apple[] = {nullptr};
    engine_log("ENTER: calling guest main");
    arguments->status = reinterpret_cast<GuestMain>(g_image.entry)(1, guest_argv, guest_envp, guest_apple);
    engine_log("EXIT: guest main returned %d", arguments->status);
    return nullptr;
}

}  // namespace

void engine_log(const char* format, ...) {
    char buffer[1024];
    va_list arguments;
    va_start(arguments, format);
    const int length = vsnprintf(buffer, sizeof buffer, format, arguments);
    va_end(arguments);
    if (length > 0) write_log_line(buffer, strnlen(buffer, sizeof buffer));
}

const std::string& engine_bundle_dir() { return g_bundle_dir; }

int run_app_bundle(const std::string& bundle_dir, const std::string& executable, const std::string& log_path) {
    g_bundle_dir = bundle_dir;
    if (!log_path.empty()) g_log_fd = open(log_path.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_APPEND | O_CLOEXEC, 0600);

    GuestArguments arguments{bundle_dir, executable};
    pthread_attr_t attributes;
    pthread_attr_init(&attributes);
    pthread_attr_setstacksize(&attributes, kGuestStackSize);
    pthread_t thread;
    if (pthread_create(&thread, &attributes, guest_thread, &arguments) != 0) {
        engine_log("LOAD FAILED: cannot create the guest thread");
        return 1;
    }
    pthread_join(thread, nullptr);
    return arguments.status;
}
