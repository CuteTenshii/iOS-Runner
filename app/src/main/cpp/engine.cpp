#include "engine.h"

#include "darwin_shim.h"
#include "objc_runtime.h"

#include <dlfcn.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <sys/mman.h>
#include <ucontext.h>
#include <unistd.h>

#include <chrono>
#include <condition_variable>
#include <cstdarg>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <exception>
#include <map>
#include <mutex>

#include "macho.h"
#include "symbols.h"

#ifndef IOSRUNNER_HOST
#include <android/log.h>
#endif

namespace {

constexpr size_t kGuestStackSize = 64 * 1024 * 1024;
constexpr size_t kSignalStackSize = 256 * 1024;
constexpr int kStackScanWords = 128;
constexpr int kStackScanReports = 6;

int g_log_fd = -1;
std::string g_bundle_dir;
std::vector<std::unique_ptr<MachImage>> g_images;

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
        case SIGSEGV:
            return "SIGSEGV";
        case SIGBUS:
            return "SIGBUS";
        case SIGILL:
            return "SIGILL";
        case SIGTRAP:
            return "SIGTRAP";
        case SIGABRT:
            return "SIGABRT";
        case SIGFPE:
            return "SIGFPE";
        default:
            return "signal";
    }
}

void describe_address(char* out, size_t size, uintptr_t address) {
    for (const auto& image : g_images) {
        if (address < image->base || address >= image->base + image->size) continue;
        snprintf(out, size, "%s+0x%zx", image.get() == g_images.back().get() ? "app" : image->name.c_str(), static_cast<size_t>(address - image->base));
        return;
    }
    {
#ifdef IOSRUNNER_HOST
        snprintf(out, size, "0x%zx", static_cast<size_t>(address));
#else
        Dl_info info{};
        if (dladdr(reinterpret_cast<void*>(address), &info) != 0 && info.dli_fbase != nullptr) {
            const char* slash = info.dli_fname != nullptr ? strrchr(info.dli_fname, '/') : nullptr;
            snprintf(out, size, "%s+0x%zx", slash != nullptr ? slash + 1 : "lib", static_cast<size_t>(address - reinterpret_cast<uintptr_t>(info.dli_fbase)));
        } else {
            snprintf(out, size, "0x%zx", static_cast<size_t>(address));
        }
#endif
    }
}

void fault_handler(int number, siginfo_t* info, void* raw_context) {
    auto* context = static_cast<ucontext_t*>(raw_context);
    const auto& machine = context->uc_mcontext;
    const auto fault = reinterpret_cast<uintptr_t>(info->si_addr);
    const uintptr_t pc = machine.pc;
    const uintptr_t lr = machine.regs[30];

    char pc_text[96], lr_text[96];
    describe_address(pc_text, sizeof pc_text, pc);
    describe_address(lr_text, sizeof lr_text, lr);

    if (const char* called = unresolved_symbol_at(pc)) {
        engine_log("STOP: called unimplemented function %s (from %s)", called, lr_text);
    } else if (const char* read = unresolved_symbol_at(fault)) {
        engine_log("STOP: accessed unimplemented data %s (at %s, called from %s)", read, pc_text, lr_text);
    } else {
        engine_log("CRASH: %s at %s, address 0x%zx, lr %s", signal_name(number), pc_text, static_cast<size_t>(fault), lr_text);
    }
    engine_log("  x0=%llx x1=%llx x2=%llx x8=%llx x19=%llx x20=%llx x21=%llx x22=%llx",
               static_cast<unsigned long long>(machine.regs[0]), static_cast<unsigned long long>(machine.regs[1]),
               static_cast<unsigned long long>(machine.regs[2]), static_cast<unsigned long long>(machine.regs[8]),
               static_cast<unsigned long long>(machine.regs[19]), static_cast<unsigned long long>(machine.regs[20]),
               static_cast<unsigned long long>(machine.regs[21]), static_cast<unsigned long long>(machine.regs[22]));
    engine_log("  x23=%llx x24=%llx x25=%llx x26=%llx x27=%llx x28=%llx sp=%llx",
               static_cast<unsigned long long>(machine.regs[23]), static_cast<unsigned long long>(machine.regs[24]),
               static_cast<unsigned long long>(machine.regs[25]), static_cast<unsigned long long>(machine.regs[26]),
               static_cast<unsigned long long>(machine.regs[27]), static_cast<unsigned long long>(machine.regs[28]),
               static_cast<unsigned long long>(machine.sp));
    // Leaf thunks keep no frame, so return addresses on the stack are the only trace of their callers.
    const auto* stack = reinterpret_cast<const uintptr_t*>(machine.sp);
    for (int slot = 0, reported = 0; slot < kStackScanWords && reported < kStackScanReports; ++slot) {
        const uintptr_t word = stack[slot];
        for (const auto& image : g_images) {
            const MachSection* code = image->section("__TEXT", "__text");
            if (code == nullptr || word < code->address || word >= code->address + code->size) continue;
            char where[96];
            describe_address(where, sizeof where, word);
            engine_log("  stack[%d] %s", slot, where);
            ++reported;
            break;
        }
    }
    uintptr_t frame = machine.regs[29];
    for (int depth = 0; depth < 16 && frame != 0 && (frame & 7) == 0; ++depth) {
        uintptr_t next, ret;
        if (frame < 0x1000) break;
        memcpy(&next, reinterpret_cast<void*>(frame), sizeof next);
        memcpy(&ret, reinterpret_cast<void*>(frame + 8), sizeof ret);
        char ret_text[96];
        describe_address(ret_text, sizeof ret_text, ret);
        engine_log("  #%d %s", depth, ret_text);
        if (next <= frame) break;
        frame = next;
    }
    _exit(128 + number);
}

void install_fault_handlers() {
    stack_t alternate{};
    alternate.ss_sp = mmap(nullptr, kSignalStackSize, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    alternate.ss_size = kSignalStackSize;
    sigaltstack(&alternate, nullptr);

    struct sigaction action{};
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

// A C++ exception escaping a shim otherwise shows up as a bare abort with an unwinder's garbled stack.
void log_uncaught_exception() {
    try {
        if (std::current_exception()) std::rethrow_exception(std::current_exception());
    } catch (const std::exception& error) {
        engine_log("STOP: a runtime shim threw: %s", error.what());
    } catch (...) {
        engine_log("STOP: a runtime shim threw an unknown exception");
    }
    abort();
}

void* guest_thread(void* raw) {
    auto* arguments = static_cast<GuestArguments*>(raw);
    install_fault_handlers();
    std::set_terminate(log_uncaught_exception);

    const std::string binary = arguments->bundle_dir + "/" + arguments->executable;
    std::string error;
    if (!load_macho_graph(binary, resolve_symbol, g_images, error)) {
        engine_log("LOAD FAILED: %s", error.c_str());
        return nullptr;
    }
    const MachImage& app = *g_images.back();

    std::map<std::string, std::pair<int, int>> per_library;
    int missing = 0;
    size_t total_imports = 0;
    for (const auto& image : g_images) {
        for (const auto& import : image->imports) {
            auto& counts = per_library[import.library.empty() ? "(flat)" : import.library];
            counts.second++;
            total_imports++;
            if (import.resolved)
                counts.first++;
            else
                missing++;
#ifdef IOSRUNNER_HOST
            if (!import.resolved && !import.weak) engine_log("UNRESOLVED: %s", import.name.c_str());
#endif
        }
    }
    for (const auto& image : g_images) {
        if (image.get() != &app) engine_log("LOADED: %s at 0x%zx", image->name.c_str(), static_cast<size_t>(image->base));
    }
    engine_log("LOADED: %s at 0x%zx, entry app+0x%zx", arguments->executable.c_str(), static_cast<size_t>(app.base), static_cast<size_t>(app.entry - app.base));
    engine_log("IMPORTS: %zu total, %d not implemented yet", total_imports, missing);
    for (const auto& [library, counts] : per_library) {
        engine_log("  %s: %d/%d", library.c_str(), counts.first, counts.second);
    }

    runtime_attach_images(g_images);

    using GuestMain = int (*)(int, const char**, const char**, const char**);
    const char* guest_argv[] = {binary.c_str(), nullptr};
    const char* guest_envp[] = {nullptr};
    const char* guest_apple[] = {nullptr};
#ifdef IOSRUNNER_HOST
    // Host runs only: makes reads of uninitialized guest stack fail the same way every time.
    {
        constexpr size_t kPoisonBytes = 8 * 1024 * 1024;
        constexpr size_t kPoisonMargin = 4096;
        volatile char* below = static_cast<volatile char*>(__builtin_frame_address(0)) - kPoisonMargin;
        for (size_t i = 0; i < kPoisonBytes; ++i) below[-static_cast<ptrdiff_t>(i)] = static_cast<char>(0x5b);
    }
#endif
    darwin_mark_main_thread();
    for (const auto& image : g_images) {
        objc_run_load_methods(*image);
        run_image_initializers(*image, 1, guest_argv, guest_envp, guest_apple);
    }
    engine_log("ENTER: calling guest main");
    arguments->status = reinterpret_cast<GuestMain>(app.entry)(1, guest_argv, guest_envp, guest_apple);
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

const std::vector<std::unique_ptr<MachImage>>& engine_images() { return g_images; }

namespace {

void (*g_message_sink)(const char*, const char*) = nullptr;
std::mutex g_action_lock;
std::condition_variable g_action_ready;
std::deque<int> g_pending_actions;
std::deque<std::function<void()>> g_pending_events;
std::string (*g_cookie_handler)() = nullptr;
bool g_actions_closed = false;

}  // namespace

namespace {
HttpResult (*g_http_handler)(const std::string&, const std::string&, const std::string&, const std::string&) = nullptr;
}

void engine_set_http_handler(HttpResult (*handler)(const std::string&, const std::string&, const std::string&, const std::string&)) { g_http_handler = handler; }

HttpResult engine_http(const std::string& url, const std::string& method, const std::string& headers, const std::string& body) {
    return g_http_handler != nullptr ? g_http_handler(url, method, headers, body) : HttpResult{};
}

void engine_set_message_sink(void (*sink)(const char*, const char*)) { g_message_sink = sink; }

void engine_emit(const char* kind, const std::string& payload) {
    if (g_message_sink != nullptr) g_message_sink(kind, payload.c_str());
}

int engine_wait_for_action(int64_t deadline_ns) {
    std::unique_lock lock(g_action_lock);
    const auto ready = [] { return g_actions_closed || !g_pending_actions.empty() || !g_pending_events.empty(); };
    if (deadline_ns > 0) {
        const auto deadline = std::chrono::steady_clock::time_point(std::chrono::nanoseconds(deadline_ns));
        if (!g_action_ready.wait_until(lock, deadline, ready)) return kEngineEventRan;
    } else {
        g_action_ready.wait(lock, ready);
    }
    if (!g_pending_events.empty()) {
        const auto event = std::move(g_pending_events.front());
        g_pending_events.pop_front();
        lock.unlock();
        event();
        return kEngineEventRan;
    }
    if (g_pending_actions.empty()) return -1;
    const int action = g_pending_actions.front();
    g_pending_actions.pop_front();
    return action;
}

void engine_post_event(std::function<void()> event) {
    const std::lock_guard lock(g_action_lock);
    g_pending_events.push_back(std::move(event));
    g_action_ready.notify_one();
}

void engine_set_cookie_handler(std::string (*handler)()) { g_cookie_handler = handler; }
std::string engine_cookies() { return g_cookie_handler != nullptr ? g_cookie_handler() : std::string(); }

namespace {

struct HostRequest {
    std::function<void(const std::string&)> handler;
    bool repeating;
};

std::mutex g_host_lock;
std::map<int, HostRequest> g_host_requests;
int g_next_host_request = 1;
std::string (*g_host_query)(const std::string&, const std::string&) = nullptr;

}  // namespace

int engine_host_register(std::function<void(const std::string&)> handler, bool repeating) {
    const std::lock_guard guard(g_host_lock);
    const int id = g_next_host_request++;
    g_host_requests[id] = {std::move(handler), repeating};
    return id;
}

void engine_host_unregister(int id) {
    const std::lock_guard guard(g_host_lock);
    g_host_requests.erase(id);
}

void engine_host_reply(int id, const std::string& payload) {
    engine_post_event([id, payload] {
        std::function<void(const std::string&)> handler;
        {
            const std::lock_guard guard(g_host_lock);
            const auto found = g_host_requests.find(id);
            if (found == g_host_requests.end()) return;
            handler = found->second.handler;
            if (!found->second.repeating) g_host_requests.erase(found);
        }
        handler(payload);
    });
}

void engine_set_host_query_handler(std::string (*handler)(const std::string&, const std::string&)) { g_host_query = handler; }

std::string engine_host_query(const std::string& kind, const std::string& argument) { return g_host_query != nullptr ? g_host_query(kind, argument) : std::string(); }

void engine_post_action(int action) {
    const std::lock_guard lock(g_action_lock);
    g_pending_actions.push_back(action);
    g_action_ready.notify_one();
}

void engine_close_actions() {
    const std::lock_guard lock(g_action_lock);
    g_actions_closed = true;
    g_action_ready.notify_one();
}

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
