// Guard-page allocator: every block ends at a guard page and freed blocks stay inaccessible, so an overrun or
// use-after-free faults at the instruction that made it. Linked with --wrap, by host-test.sh and the
// iosrunner.efence Gradle property. Anything allocated before it starts guarding comes from the real allocator.
#include <errno.h>
#include <pthread.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include <cstddef>
#include <cstdint>

extern "C" {
void* __real_malloc(size_t);
void __real_free(void*);
void* __real_realloc(void*, size_t);
size_t __real_malloc_usable_size(const void*);
}

#ifdef IOSRUNNER_EFENCE_AFTER
bool g_efence_guarding = false;
#else
bool g_efence_guarding = true;
#endif

namespace {

constexpr size_t kPage = 4096;
constexpr size_t kAlignment = 16;
constexpr size_t kRememberedSlots = 1 << 21;
constexpr unsigned kSlotHashShift = 43;
constexpr uint64_t kSlotHashMultiplier = 11400714819323198485ULL;
// A process only gets so many mappings, so the oldest freed blocks really are returned. Past this, use-after-free
// of them reads as an ordinary fault rather than one with a guard behind it.
constexpr size_t kQuarantinedBlocks = 4000;

struct Header {
    size_t size;
    void* mapping;
    size_t mapping_size;
};

// Reading before a libc allocation can fault, so guarded blocks are told apart by this table.
void* g_guarded[kRememberedSlots];
pthread_mutex_t g_guarded_lock = PTHREAD_MUTEX_INITIALIZER;

size_t slot_of(const void* pointer) { return ((reinterpret_cast<uintptr_t>(pointer) >> 4) * kSlotHashMultiplier) >> kSlotHashShift; }

void remember(void* pointer) {
    pthread_mutex_lock(&g_guarded_lock);
    size_t slot = slot_of(pointer);
    while (g_guarded[slot] != nullptr) slot = (slot + 1) % kRememberedSlots;
    g_guarded[slot] = pointer;
    pthread_mutex_unlock(&g_guarded_lock);
}

bool is_guarded(const void* pointer) {
    pthread_mutex_lock(&g_guarded_lock);
    bool found = false;
    for (size_t slot = slot_of(pointer); g_guarded[slot] != nullptr; slot = (slot + 1) % kRememberedSlots) {
        if (g_guarded[slot] == pointer) {
            found = true;
            break;
        }
    }
    pthread_mutex_unlock(&g_guarded_lock);
    return found;
}

void* real_zeroed(size_t size) {
    void* block = __real_malloc(size);
    if (block != nullptr) memset(block, 0, size);
    return block;
}

void* allocate(size_t size, size_t alignment) {
    if (!g_efence_guarding) return real_zeroed(size);
    const size_t rounded = (size + alignment - 1) & ~(alignment - 1);
    const size_t body = (rounded + sizeof(Header) + alignment + kPage - 1) & ~(kPage - 1);
    const size_t total = body + kPage;
    auto* mapping = static_cast<char*>(mmap(nullptr, total, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
    if (mapping == MAP_FAILED) return real_zeroed(size);
    mprotect(mapping + body, kPage, PROT_NONE);
    char* user = mapping + body - rounded;
    user = reinterpret_cast<char*>(reinterpret_cast<uintptr_t>(user) & ~(alignment - 1));
    *(reinterpret_cast<Header*>(user) - 1) = {size, mapping, total};
    remember(user);
    return user;
}

Header* header_of(void* pointer) { return static_cast<Header*>(pointer) - 1; }

}  // namespace

extern "C" {

void* __wrap_malloc(size_t size) { return allocate(size == 0 ? 1 : size, kAlignment); }
void* __wrap_calloc(size_t count, size_t size) { return allocate(count * size == 0 ? 1 : count * size, kAlignment); }

void __wrap_free(void* pointer) {
    if (pointer == nullptr) return;
    if (!is_guarded(pointer)) {
        __real_free(pointer);
        return;
    }
    const Header header = *header_of(pointer);
    mprotect(header.mapping, header.mapping_size, PROT_NONE);
    static struct {
        void* mapping;
        size_t size;
    } quarantine[kQuarantinedBlocks];
    static size_t next = 0;
    auto& oldest = quarantine[next++ % kQuarantinedBlocks];
    if (oldest.mapping != nullptr) munmap(oldest.mapping, oldest.size);
    oldest = {header.mapping, header.mapping_size};
}

void* __wrap_realloc(void* pointer, size_t size) {
    if (pointer != nullptr && !is_guarded(pointer)) return __real_realloc(pointer, size);
    void* moved = __wrap_malloc(size);
    if (pointer != nullptr && moved != nullptr) {
        const size_t old_size = header_of(pointer)->size;
        memcpy(moved, pointer, old_size < size ? old_size : size);
        __wrap_free(pointer);
    }
    return moved;
}

int __wrap_posix_memalign(void** out, size_t alignment, size_t size) {
    *out = allocate(size == 0 ? 1 : size, alignment < kAlignment ? kAlignment : alignment);
    return *out == nullptr ? ENOMEM : 0;
}

size_t __wrap_malloc_usable_size(const void* pointer) {
    if (!is_guarded(pointer)) return __real_malloc_usable_size(pointer);
    return header_of(const_cast<void*>(pointer))->size;
}

}
