#include "concurrency_shim.h"

#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <new>
#include <unordered_map>

#include "engine.h"
#include "swift_runtime.h"

namespace {

struct HeapHeader {
    void* metadata;
    std::atomic<uint64_t> references;
};

struct ObjectMetadata {
    void (*destroyer)(void*);
    const void* witnesses;
    uint64_t kind;
};

const ObjectMetadata kTaskMetadata = {free, nullptr, 0};
const ObjectMetadata kMainActorMetadata = {free, nullptr, 0};

constexpr uint64_t kImmortal = 1ULL << 63;

struct AsyncFunctionPointer {
    int32_t function;
    uint32_t context_size;
};

// The ABI's AsyncContext prefix: the parent, then the function a callee returns through.
struct AsyncContextHeader {
    void* parent;
    void* resume_parent;
};

struct ReadyTask {
    const void* function;
    void* closure_context;
    void* result;
    void* async_context;
    bool is_resume;
};

std::deque<ReadyTask>& ready_tasks() {
    static std::deque<ReadyTask> queue;
    return queue;
}

struct TaskObject {
    HeapHeader header;
};

struct TaskCreation {
    void* task;
    void* context;
};

extern "C" void rt_task_finished();
extern "C" void rt_start_task(const void* function, void* result, void* closure_context, void* async_context);
extern "C" void rt_resume_task(const void* function, void* async_context);

// The ABI's ContinuationAsyncContext; its first two words are the usual parent and resume function.
struct ContinuationContext {
    void* parent;
    const void* resume;
    uint64_t state;
    void* error;
    void* result;
};

constexpr uint64_t kContinuationAwaited = 1;
constexpr uint64_t kContinuationResumed = 2;

void* continuation_init(ContinuationContext* context, size_t) {
    context->state = 0;
    context->error = nullptr;
    return context;
}

void continuation_resume(ContinuationContext* context) {
    if (context->state == kContinuationAwaited) {
        ready_tasks().push_back({context->resume, nullptr, nullptr, context->parent, true});
    } else {
        context->state = kContinuationResumed;
    }
}

constexpr size_t kMinimumResultBytes = 16;
constexpr size_t kValueWitnessSizeOffset = 0x40;

TaskCreation task_create(size_t, void*, void* result_type, const AsyncFunctionPointer* entry, void* closure_context) {
    auto* task = static_cast<TaskObject*>(calloc(1, sizeof(TaskObject)));
    task->header.metadata = const_cast<uint64_t*>(&kTaskMetadata.kind);
    new (&task->header.references) std::atomic<uint64_t>(2);

    auto* context = static_cast<AsyncContextHeader*>(calloc(1, entry->context_size));
    context->resume_parent = reinterpret_cast<void*>(rt_task_finished);

    size_t result_size = kMinimumResultBytes;
    if (result_type != nullptr) {
        memcpy(&result_size, static_cast<const char*>(static_cast<void* const*>(result_type)[-1]) + kValueWitnessSizeOffset, sizeof result_size);
        if (result_size < kMinimumResultBytes) result_size = kMinimumResultBytes;
    }
    const char* base = reinterpret_cast<const char*>(&entry->function);
    ready_tasks().push_back({base + entry->function, closure_context, calloc(1, result_size), context, false});
    return {task, context};
}

void* task_alloc(size_t size) {
    void* memory = malloc(size);
#ifdef IOSRUNNER_HOST
    memset(memory, 0x5b, size);
#endif
    return memory;
}
void task_dealloc(void* pointer) { free(pointer); }
bool task_is_current_executor(void*, void*) { return true; }
void task_report_unexpected_executor(void*, void*, void*, void*, void*) {}

void default_actor_initialize(void*) {}
void default_actor_destroy(void*) {}
void default_actor_deallocate(void* actor) { free(actor); }

void* main_actor_shared() {
    static auto* actor = [] {
        auto* header = static_cast<HeapHeader*>(calloc(1, sizeof(HeapHeader)));
        header->metadata = const_cast<uint64_t*>(&kMainActorMetadata.kind);
        new (&header->references) std::atomic<uint64_t>(kImmortal);
        return header;
    }();
    return actor;
}

struct ExecutorReference {
    uint64_t identity;
    uint64_t implementation;
};

ExecutorReference main_actor_executor() { return {0, 0}; }

constexpr uint8_t kTaskPriorityUserInitiated = 25;
uint8_t task_priority_user_initiated() { return kTaskPriorityUserInitiated; }
uint8_t task_priority_raw_value(uint8_t self) { return self; }

}  // namespace

// Every actor shares the one guest thread, so a hop just continues: the resume function is in x0, the context stays in x22.
__asm__(
    ".text\n"
    ".globl rt_task_switch\n"
    "rt_task_switch:\n"
    "  br x0\n"
    // Where a task's top-level function returns: back to rt_start_task's frame.
    ".globl rt_task_finished\n"
    "rt_task_finished:\n"
    "  ret\n"
    // Awaiting a continuation not resumed yet returns to whoever is running this task.
    ".globl rt_continuation_await\n"
    "rt_continuation_await:\n"
    "  ldr x9, [x0, #16]\n"
    "  cmp x9, #2\n"
    "  b.eq 1f\n"
    "  mov x9, #1\n"
    "  str x9, [x0, #16]\n"
    "  ret\n"
    "1:\n"
    "  ldr x22, [x0]\n"
    "  ldr x1, [x0, #8]\n"
    "  br x1\n"
    ".globl rt_resume_task\n"
    "rt_resume_task:\n"
    "  stp x29, x30, [sp, #-112]!\n"
    "  mov x29, sp\n"
    "  stp x19, x20, [sp, #16]\n"
    "  stp x21, x22, [sp, #32]\n"
    "  stp x23, x24, [sp, #48]\n"
    "  stp x25, x26, [sp, #64]\n"
    "  stp x27, x28, [sp, #80]\n"
    "  mov x22, x1\n"
    "  blr x0\n"
    "  ldp x19, x20, [sp, #16]\n"
    "  ldp x21, x22, [sp, #32]\n"
    "  ldp x23, x24, [sp, #48]\n"
    "  ldp x25, x26, [sp, #64]\n"
    "  ldp x27, x28, [sp, #80]\n"
    "  ldp x29, x30, [sp], #112\n"
    "  ret\n"
    ".globl rt_start_task\n"
    "rt_start_task:\n"
    "  stp x29, x30, [sp, #-112]!\n"
    "  mov x29, sp\n"
    "  stp x19, x20, [sp, #16]\n"
    "  stp x21, x22, [sp, #32]\n"
    "  stp x23, x24, [sp, #48]\n"
    "  stp x25, x26, [sp, #64]\n"
    "  stp x27, x28, [sp, #80]\n"
    "  mov x16, x0\n"
    "  mov x0, x1\n"
    "  mov x20, x2\n"
    "  mov x22, x3\n"
    "  blr x16\n"
    "  ldp x19, x20, [sp, #16]\n"
    "  ldp x21, x22, [sp, #32]\n"
    "  ldp x23, x24, [sp, #48]\n"
    "  ldp x25, x26, [sp, #64]\n"
    "  ldp x27, x28, [sp, #80]\n"
    "  ldp x29, x30, [sp], #112\n"
    "  ret\n");

extern "C" void rt_task_switch();
extern "C" void rt_continuation_await();

void concurrency_start(const void* async_function, void* context) {
    const auto* entry = static_cast<const AsyncFunctionPointer*>(async_function);
    auto* async_context = static_cast<AsyncContextHeader*>(calloc(1, entry->context_size));
    async_context->resume_parent = reinterpret_cast<void*>(rt_task_finished);
    const char* base = reinterpret_cast<const char*>(&entry->function);
    ready_tasks().push_back({base + entry->function, context, calloc(1, kMinimumResultBytes), async_context, false});
}

void concurrency_drain() {
    auto& queue = ready_tasks();
    while (!queue.empty()) {
        const ReadyTask task = queue.front();
        queue.pop_front();
        if (task.is_resume) rt_resume_task(task.function, task.async_context);
        else rt_start_task(task.function, task.result, task.closure_context, task.async_context);
    }
}

void* concurrency_symbol(std::string_view name) {
    static const bool registered = [] {
        register_foreign_witness("_$sScMScAsWP", "_$sScA15unownedExecutorScevgTq", reinterpret_cast<const void*>(main_actor_executor));
        return true;
    }();
    (void)registered;
    static const std::unordered_map<std::string_view, void*> table = {
        {"$sScM6sharedScMvgZ", reinterpret_cast<void*>(main_actor_shared)},
        {"$sScP13userInitiatedScPvgZ", reinterpret_cast<void*>(task_priority_user_initiated)},
        {"$sScP8rawValues5UInt8Vvg", reinterpret_cast<void*>(task_priority_raw_value)},
        {"swift_continuation_init", reinterpret_cast<void*>(continuation_init)},
        {"swift_continuation_await", reinterpret_cast<void*>(rt_continuation_await)},
        {"swift_continuation_resume", reinterpret_cast<void*>(continuation_resume)},
        {"swift_task_create", reinterpret_cast<void*>(task_create)},
        {"swift_task_switch", reinterpret_cast<void*>(rt_task_switch)},
        {"swift_task_alloc", reinterpret_cast<void*>(task_alloc)},
        {"swift_task_dealloc", reinterpret_cast<void*>(task_dealloc)},
        {"swift_task_isCurrentExecutor", reinterpret_cast<void*>(task_is_current_executor)},
        {"swift_task_reportUnexpectedExecutor", reinterpret_cast<void*>(task_report_unexpected_executor)},
        {"swift_defaultActor_initialize", reinterpret_cast<void*>(default_actor_initialize)},
        {"swift_defaultActor_destroy", reinterpret_cast<void*>(default_actor_destroy)},
        {"swift_defaultActor_deallocate", reinterpret_cast<void*>(default_actor_deallocate)},
    };
    const auto found = table.find(name);
    return found == table.end() ? nullptr : found->second;
}
