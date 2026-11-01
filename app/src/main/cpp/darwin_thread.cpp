#include <errno.h>
#include <pthread.h>
#include <semaphore.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <utility>
#include <vector>

#include "darwin_shim.h"
#include "engine.h"
#include "macho.h"

namespace {

constexpr long kMutexSignature = 0x32AAABA7, kMutexErrorCheckSignature = 0x32AAABA1, kMutexRecursiveSignature = 0x32AAABA2, kMutexFirstFitSignature = 0x32AAABA3;
constexpr long kCondSignature = 0x3CB0B1BB;
constexpr long kRwlockSignature = 0x2DA8B3B4;
constexpr long kOnceSignature = 0x30B1BCBA;
constexpr int kDarwinMutexErrorCheck = 1, kDarwinMutexRecursive = 2;

std::mutex& conversion_lock() {
    static std::mutex lock;
    return lock;
}

// Darwin's static initializers are signatures, while bionic's are zero. Darwin's objects are always the larger ones.
void convert_mutex(pthread_mutex_t* mutex) {
    long word;
    memcpy(&word, mutex, sizeof word);
    if (word != kMutexSignature && word != kMutexErrorCheckSignature && word != kMutexRecursiveSignature && word != kMutexFirstFitSignature) return;
    const std::lock_guard guard(conversion_lock());
    memcpy(&word, mutex, sizeof word);
    if (word == kMutexRecursiveSignature || word == kMutexErrorCheckSignature) {
        pthread_mutexattr_t attributes;
        pthread_mutexattr_init(&attributes);
        pthread_mutexattr_settype(&attributes, word == kMutexRecursiveSignature ? PTHREAD_MUTEX_RECURSIVE : PTHREAD_MUTEX_ERRORCHECK);
        pthread_mutex_init(mutex, &attributes);
        pthread_mutexattr_destroy(&attributes);
    } else if (word == kMutexSignature || word == kMutexFirstFitSignature) {
        pthread_mutex_init(mutex, nullptr);
    }
}

void convert_cond(pthread_cond_t* cond) {
    long word;
    memcpy(&word, cond, sizeof word);
    if (word != kCondSignature) return;
    const std::lock_guard guard(conversion_lock());
    memcpy(&word, cond, sizeof word);
    if (word == kCondSignature) pthread_cond_init(cond, nullptr);
}

void convert_rwlock(pthread_rwlock_t* lock) {
    long word;
    memcpy(&word, lock, sizeof word);
    if (word != kRwlockSignature) return;
    const std::lock_guard guard(conversion_lock());
    memcpy(&word, lock, sizeof word);
    if (word == kRwlockSignature) pthread_rwlock_init(lock, nullptr);
}

int darwin_mutexattr_settype(pthread_mutexattr_t* attributes, int type) {
    return pthread_mutexattr_settype(attributes, type == kDarwinMutexRecursive ? PTHREAD_MUTEX_RECURSIVE : type == kDarwinMutexErrorCheck ? PTHREAD_MUTEX_ERRORCHECK
                                                                                                                                          : PTHREAD_MUTEX_NORMAL);
}

int darwin_mutex_lock(pthread_mutex_t* mutex) {
    convert_mutex(mutex);
    return pthread_mutex_lock(mutex);
}
int darwin_mutex_trylock(pthread_mutex_t* mutex) {
    convert_mutex(mutex);
    return pthread_mutex_trylock(mutex);
}
int darwin_mutex_unlock(pthread_mutex_t* mutex) {
    convert_mutex(mutex);
    return pthread_mutex_unlock(mutex);
}
int darwin_mutex_destroy(pthread_mutex_t* mutex) {
    convert_mutex(mutex);
    return pthread_mutex_destroy(mutex);
}
int darwin_cond_wait(pthread_cond_t* cond, pthread_mutex_t* mutex) {
    convert_cond(cond);
    convert_mutex(mutex);
    return pthread_cond_wait(cond, mutex);
}
int darwin_cond_timedwait(pthread_cond_t* cond, pthread_mutex_t* mutex, const timespec* deadline) {
    convert_cond(cond);
    convert_mutex(mutex);
    return pthread_cond_timedwait(cond, mutex, deadline);
}
int darwin_cond_signal(pthread_cond_t* cond) {
    convert_cond(cond);
    return pthread_cond_signal(cond);
}
int darwin_cond_broadcast(pthread_cond_t* cond) {
    convert_cond(cond);
    return pthread_cond_broadcast(cond);
}
int darwin_cond_destroy(pthread_cond_t* cond) {
    convert_cond(cond);
    return pthread_cond_destroy(cond);
}
int darwin_rwlock_rdlock(pthread_rwlock_t* lock) {
    convert_rwlock(lock);
    return pthread_rwlock_rdlock(lock);
}
int darwin_rwlock_wrlock(pthread_rwlock_t* lock) {
    convert_rwlock(lock);
    return pthread_rwlock_wrlock(lock);
}
int darwin_rwlock_tryrdlock(pthread_rwlock_t* lock) {
    convert_rwlock(lock);
    return pthread_rwlock_tryrdlock(lock);
}
int darwin_rwlock_trywrlock(pthread_rwlock_t* lock) {
    convert_rwlock(lock);
    return pthread_rwlock_trywrlock(lock);
}
int darwin_rwlock_unlock(pthread_rwlock_t* lock) {
    convert_rwlock(lock);
    return pthread_rwlock_unlock(lock);
}
int darwin_rwlock_destroy(pthread_rwlock_t* lock) {
    convert_rwlock(lock);
    return pthread_rwlock_destroy(lock);
}

struct DarwinOnce {
    long signature;
    std::atomic<int> state;
};

int darwin_once(DarwinOnce* once, void (*function)()) {
    constexpr int kDone = 1;
    if (once->state.load(std::memory_order_acquire) == kDone) return 0;
    static std::recursive_mutex lock;
    const std::lock_guard guard(lock);
    if (once->state.load(std::memory_order_relaxed) != kDone) {
        function();
        once->state.store(kDone, std::memory_order_release);
    }
    return 0;
}

int darwin_attr_setdetachstate(pthread_attr_t* attributes, int state) {
    constexpr int kDarwinDetached = 2;
    return pthread_attr_setdetachstate(attributes, state == kDarwinDetached ? PTHREAD_CREATE_DETACHED : PTHREAD_CREATE_JOINABLE);
}

int darwin_threadid_np(pthread_t thread, uint64_t* id) {
    *id = thread == 0 || thread == pthread_self() ? static_cast<uint64_t>(gettid()) : static_cast<uint64_t>(thread);
    return 0;
}

int darwin_setname_np(const char* name) {
    constexpr size_t kLinuxThreadNameBytes = 16;
    char limited[kLinuxThreadNameBytes];
    strlcpy(limited, name, sizeof limited);
    return pthread_setname_np(pthread_self(), limited);
}

pid_t g_main_thread = 0;

// Each thread's variables for each image, found through the key dyld keeps in the descriptor.
struct ThreadVariable {
    void* thunk;
    pthread_key_t key;
    uint64_t offset;
};

struct ThreadTemplate {
    const char* initial;
    size_t initial_size;
    size_t total_size;
    pthread_key_t key;
};

ThreadTemplate* template_for(uintptr_t address) {
    static std::mutex lock;
    static std::vector<std::pair<const MachImage*, ThreadTemplate*>> templates;
    const std::lock_guard guard(lock);
    for (const auto& image : engine_images()) {
        if (address < image->base || address >= image->base + image->size) continue;
        for (const auto& [known, entry] : templates) {
            if (known == image.get()) return entry;
        }
        constexpr uint32_t kSectionTypeMask = 0xff, kThreadRegular = 0x11, kThreadZeroFill = 0x12;
        uintptr_t start = 0, end = 0;
        size_t initial_size = 0;
        for (const auto& section : image->sections) {
            const uint32_t type = section.flags & kSectionTypeMask;
            if (type != kThreadRegular && type != kThreadZeroFill) continue;
            if (start == 0) start = section.address;
            end = section.address + section.size;
            if (type == kThreadRegular) initial_size = end - start;
        }
        auto* entry = new ThreadTemplate{reinterpret_cast<const char*>(start), initial_size, end - start, 0};
        pthread_key_create(&entry->key, free);
        templates.emplace_back(image.get(), entry);
        return entry;
    }
    return nullptr;
}

}  // namespace

extern "C" void* darwin_tlv_address(ThreadVariable* variable) {
    ThreadTemplate* entry = template_for(reinterpret_cast<uintptr_t>(variable));
    if (entry == nullptr) {
        engine_log("STOP: a thread variable outside every image");
        abort();
    }
    auto* block = static_cast<char*>(pthread_getspecific(entry->key));
    if (block == nullptr) {
        block = static_cast<char*>(calloc(1, entry->total_size == 0 ? 1 : entry->total_size));
        if (entry->initial_size != 0) memcpy(block, entry->initial, entry->initial_size);
        pthread_setspecific(entry->key, block);
    }
    return block + variable->offset;
}

extern "C" {
void darwin_tlv_bootstrap();
int darwin_setjmp(void*);
int darwin_sigsetjmp(void*, int);
[[noreturn]] void darwin_longjmp(void*, int);
}

// Guest code calls the thunk expecting only x0 to change, as dyld's does. Darwin's jmp_buf holds these 22 words.
__asm__(R"(
.text
.globl darwin_tlv_bootstrap
darwin_tlv_bootstrap:
    stp x29, x30, [sp, #-16]!
    mov x29, sp
    stp x1, x2, [sp, #-16]!
    stp x3, x4, [sp, #-16]!
    stp x5, x6, [sp, #-16]!
    stp x7, x8, [sp, #-16]!
    stp x9, x10, [sp, #-16]!
    stp x11, x12, [sp, #-16]!
    stp x13, x14, [sp, #-16]!
    stp x15, x16, [sp, #-16]!
    stp x17, x18, [sp, #-16]!
    stp q0, q1, [sp, #-32]!
    stp q2, q3, [sp, #-32]!
    stp q4, q5, [sp, #-32]!
    stp q6, q7, [sp, #-32]!
    bl darwin_tlv_address
    ldp q6, q7, [sp], #32
    ldp q4, q5, [sp], #32
    ldp q2, q3, [sp], #32
    ldp q0, q1, [sp], #32
    ldp x17, x18, [sp], #16
    ldp x15, x16, [sp], #16
    ldp x13, x14, [sp], #16
    ldp x11, x12, [sp], #16
    ldp x9, x10, [sp], #16
    ldp x7, x8, [sp], #16
    ldp x5, x6, [sp], #16
    ldp x3, x4, [sp], #16
    ldp x1, x2, [sp], #16
    ldp x29, x30, [sp], #16
    ret
.globl darwin_setjmp
.globl darwin_sigsetjmp
darwin_setjmp:
darwin_sigsetjmp:
    stp x19, x20, [x0, #0]
    stp x21, x22, [x0, #16]
    stp x23, x24, [x0, #32]
    stp x25, x26, [x0, #48]
    stp x27, x28, [x0, #64]
    stp x29, x30, [x0, #80]
    mov x9, sp
    str x9, [x0, #96]
    stp d8, d9, [x0, #104]
    stp d10, d11, [x0, #120]
    stp d12, d13, [x0, #136]
    stp d14, d15, [x0, #152]
    mov w0, #0
    ret
.globl darwin_longjmp
darwin_longjmp:
    ldp x19, x20, [x0, #0]
    ldp x21, x22, [x0, #16]
    ldp x23, x24, [x0, #32]
    ldp x25, x26, [x0, #48]
    ldp x27, x28, [x0, #64]
    ldp x29, x30, [x0, #80]
    ldr x9, [x0, #96]
    mov sp, x9
    ldp d8, d9, [x0, #104]
    ldp d10, d11, [x0, #120]
    ldp d12, d13, [x0, #136]
    ldp d14, d15, [x0, #152]
    cmp w1, #0
    csinc w0, w1, wzr, ne
    ret
)");

void darwin_mark_main_thread() { g_main_thread = gettid(); }

#define DARWIN_FN(name) {#name, reinterpret_cast<void*>(name)}

void add_darwin_thread_symbols(DarwinSymbolTable& table) {
    table.insert({
        {"_tlv_bootstrap", reinterpret_cast<void*>(darwin_tlv_bootstrap)},
        {"_tlv_atexit", reinterpret_cast<void*>(+[](void (*)(void*), void*) {})},
        {"setjmp", reinterpret_cast<void*>(darwin_setjmp)},
        {"sigsetjmp", reinterpret_cast<void*>(darwin_sigsetjmp)},
        {"longjmp", reinterpret_cast<void*>(darwin_longjmp)},
        {"siglongjmp", reinterpret_cast<void*>(darwin_longjmp)},
        {"pthread_mutexattr_settype", reinterpret_cast<void*>(darwin_mutexattr_settype)},
        {"pthread_mutex_lock", reinterpret_cast<void*>(darwin_mutex_lock)},
        {"pthread_mutex_trylock", reinterpret_cast<void*>(darwin_mutex_trylock)},
        {"pthread_mutex_unlock", reinterpret_cast<void*>(darwin_mutex_unlock)},
        {"pthread_mutex_destroy", reinterpret_cast<void*>(darwin_mutex_destroy)},
        {"pthread_cond_wait", reinterpret_cast<void*>(darwin_cond_wait)},
        {"pthread_cond_timedwait", reinterpret_cast<void*>(darwin_cond_timedwait)},
        {"pthread_cond_signal", reinterpret_cast<void*>(darwin_cond_signal)},
        {"pthread_cond_broadcast", reinterpret_cast<void*>(darwin_cond_broadcast)},
        {"pthread_cond_destroy", reinterpret_cast<void*>(darwin_cond_destroy)},
        {"pthread_rwlock_rdlock", reinterpret_cast<void*>(darwin_rwlock_rdlock)},
        {"pthread_rwlock_wrlock", reinterpret_cast<void*>(darwin_rwlock_wrlock)},
        {"pthread_rwlock_tryrdlock", reinterpret_cast<void*>(darwin_rwlock_tryrdlock)},
        {"pthread_rwlock_trywrlock", reinterpret_cast<void*>(darwin_rwlock_trywrlock)},
        {"pthread_rwlock_unlock", reinterpret_cast<void*>(darwin_rwlock_unlock)},
        {"pthread_rwlock_destroy", reinterpret_cast<void*>(darwin_rwlock_destroy)},
        {"pthread_once", reinterpret_cast<void*>(darwin_once)},
        {"pthread_attr_setdetachstate", reinterpret_cast<void*>(darwin_attr_setdetachstate)},
        {"pthread_threadid_np", reinterpret_cast<void*>(darwin_threadid_np)},
        {"pthread_setname_np", reinterpret_cast<void*>(darwin_setname_np)},
        {"pthread_main_np", reinterpret_cast<void*>(+[]() -> int { return gettid() == g_main_thread; })},
        {"pthread_mach_thread_np", reinterpret_cast<void*>(+[](pthread_t) -> uint32_t { return static_cast<uint32_t>(gettid()); })},
        {"pthread_cancel", reinterpret_cast<void*>(+[](pthread_t) -> int { return ESRCH; })},
        {"pthread_setcancelstate", reinterpret_cast<void*>(+[](int, int* previous) -> int {
             if (previous != nullptr) *previous = 0;
             return 0;
         })},
        DARWIN_FN(pthread_mutex_init),
        DARWIN_FN(pthread_mutexattr_init),
        DARWIN_FN(pthread_mutexattr_destroy),
        DARWIN_FN(pthread_cond_init),
        DARWIN_FN(pthread_rwlock_init),
        DARWIN_FN(pthread_attr_init),
        DARWIN_FN(pthread_attr_destroy),
        DARWIN_FN(pthread_attr_setstacksize),
        DARWIN_FN(pthread_atfork),
        DARWIN_FN(pthread_create),
        DARWIN_FN(pthread_detach),
        DARWIN_FN(pthread_join),
        DARWIN_FN(pthread_exit),
        DARWIN_FN(pthread_equal),
        DARWIN_FN(pthread_self),
        DARWIN_FN(pthread_getspecific),
        DARWIN_FN(pthread_setspecific),
        DARWIN_FN(pthread_key_create),
        DARWIN_FN(pthread_key_delete),
        DARWIN_FN(pthread_sigmask),
        DARWIN_FN(sched_yield),
    });
}

void* darwin_symbol(std::string_view name) {
    static const DarwinSymbolTable table = [] {
        DarwinSymbolTable all;
        add_darwin_libc_symbols(all);
        add_darwin_io_symbols(all);
        add_darwin_thread_symbols(all);
        add_darwin_system_symbols(all);
        return all;
    }();
    const auto found = table.find(name);
    return found == table.end() ? nullptr : found->second;
}
