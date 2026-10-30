// Host-test allocator: every block ends at a guard page and freed blocks stay inaccessible,
// so an overrun or use-after-free faults at the instruction that made it. Linked with --wrap.
#include <errno.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include <cstddef>
#include <cstdint>

namespace {

constexpr size_t kPage = 4096;
constexpr size_t kAlignment = 16;
constexpr size_t kHeaderMagic = 0x5ef0e5ef0e5ef0e5ULL;

struct Header {
    size_t magic;
    size_t size;
    void* mapping;
    size_t mapping_size;
};

void* allocate(size_t size, size_t alignment) {
    const size_t rounded = (size + alignment - 1) & ~(alignment - 1);
    const size_t body = (rounded + sizeof(Header) + alignment + kPage - 1) & ~(kPage - 1);
    const size_t total = body + kPage;
    auto* mapping = static_cast<char*>(mmap(nullptr, total, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
    if (mapping == MAP_FAILED) return nullptr;
    mprotect(mapping + body, kPage, PROT_NONE);
    char* user = mapping + body - rounded;
    user = reinterpret_cast<char*>(reinterpret_cast<uintptr_t>(user) & ~(alignment - 1));
    auto* header = reinterpret_cast<Header*>(user) - 1;
    *header = {kHeaderMagic, size, mapping, total};
    return user;
}

Header* header_of(void* pointer) { return static_cast<Header*>(pointer) - 1; }

}  // namespace

extern "C" {

void* __wrap_malloc(size_t size) { return allocate(size == 0 ? 1 : size, kAlignment); }
void* __wrap_calloc(size_t count, size_t size) { return allocate(count * size == 0 ? 1 : count * size, kAlignment); }

void __wrap_free(void* pointer) {
    if (pointer == nullptr) return;
    auto* header = header_of(pointer);
    if (header->magic != kHeaderMagic) {
        static const char message[] = "efence: free of a pointer that was not allocated\n";
        (void)!write(STDERR_FILENO, message, sizeof message - 1);
        __builtin_trap();
    }
    mprotect(header->mapping, header->mapping_size, PROT_NONE);
}

void* __wrap_realloc(void* pointer, size_t size) {
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

size_t __wrap_malloc_usable_size(const void* pointer) { return header_of(const_cast<void*>(pointer))->size; }

}
