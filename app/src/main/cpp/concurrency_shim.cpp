#include "concurrency_shim.h"

#include "dispatch_shim.h"

#include <time.h>

#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <map>
#include <new>
#include <set>
#include <unordered_map>

#include "engine.h"
#include "foundation_shim.h"
#include "swift_abi.h"
#include "swift_runtime.h"

namespace {

const HeapMetadata kTaskMetadata = {free, nullptr, 0};
const HeapMetadata kMainActorMetadata = {free, nullptr, 0};

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
    const void* owner;
};

// The Task handle whose code is running, which is what a cancel of that handle has to find.
const void* g_current_task = nullptr;
std::set<const void*> g_cancelled_tasks;

std::deque<ReadyTask>& ready_tasks() {
    static std::deque<ReadyTask> queue;
    return queue;
}

struct TaskWaiter {
    void* out;
    void* context;
};

// What a finished Task keeps for anyone awaiting its value.
struct TaskCompletion {
    bool finished = false;
    void* error = nullptr;
    void* result = nullptr;
    std::vector<TaskWaiter> waiters;
};

struct TaskObject {
    HeapObject header;
    TaskCompletion* completion;
};

struct TaskCreation {
    void* task;
    void* context;
};

extern "C" void rt_task_finished();
extern "C" void rt_start_task(const void* function, void* result, void* closure_context, void* async_context);
extern "C" void rt_resume_task(const void* function, void* async_context, void* error);

// The ABI's ContinuationAsyncContext; its first two words are the usual parent and resume function.
struct ContinuationContext {
    void* parent;
    const void* resume;
    uint64_t state;
    void* error;
    void* normal_result;
};

constexpr uint64_t kContinuationAwaited = 1;
constexpr uint64_t kContinuationResumed = 2;

// The guest reaches the context through the returned task, at the offset of AsyncTask::ResumeContext.
constexpr size_t kTaskResumeContextOffset = 0x40;
constexpr size_t kContinuationTaskBytes = kTaskResumeContextOffset + 2 * sizeof(void*);

void continuation_resume(char* task);

void* continuation_init(ContinuationContext* context, size_t) {
    context->state = 0;
    context->error = nullptr;
    context->normal_result = nullptr;
    auto* task = static_cast<char*>(calloc(1, kContinuationTaskBytes));
    memcpy(task + kTaskResumeContextOffset, &context, sizeof context);
    memcpy(task + kTaskResumeContextOffset + sizeof(void*), &g_current_task, sizeof g_current_task);
    return task;
}

void continuation_resume_with_error(char* task, void* error) {
    ContinuationContext* context;
    memcpy(&context, task + kTaskResumeContextOffset, sizeof context);
    context->error = error;
    continuation_resume(task);
}

// CheckedContinuation is the unsafe one, with the checking left out.
SWIFTCC void checked_continuation_init(SWIFT_OUT void** out, void* unsafe_continuation) { *out = unsafe_continuation; }

constexpr size_t kCheckedValueTypeWord = 2;

SWIFTCC void checked_resume_returning(void* value, void* metadata, SWIFT_CONTEXT char** self) {
    ContinuationContext* context;
    memcpy(&context, *self + kTaskResumeContextOffset, sizeof context);
    void* type = static_cast<void**>(metadata)[kCheckedValueTypeWord];
    using Take = void* (*)(void*, void*, void*);
    Take take;
    memcpy(&take, static_cast<const char*>(static_cast<void* const*>(type)[-1]) + 0x20, sizeof take);
    if (context->normal_result != nullptr) take(context->normal_result, value, type);
    continuation_resume(*self);
}

SWIFTCC void checked_resume_throwing(void** error, void*, SWIFT_CONTEXT char** self) { continuation_resume_with_error(*self, *error); }

extern "C" void rt_with_checked_continuation();

// The caller's context is the callee frame this allocates by the record below, laid out as a continuation's.
extern "C" void rt_checked_begin(void* result, const void* body, void* body_context, ContinuationContext* context) {
    void* task = continuation_init(context, 0);
    context->normal_result = result;
    context->state = kContinuationAwaited;
    using Body = SWIFTCC void (*)(void*, SWIFT_CONTEXT void*);
    reinterpret_cast<Body>(const_cast<void*>(body))(&task, body_context);
}

AsyncFunctionPointer g_checked_continuation_record;
AsyncFunctionPointer g_group_await_all_record;
AsyncFunctionPointer g_group_next_record;

// The iterator is just the group's one word.
SWIFTCC void group_make_iterator(SWIFT_OUT void** out, void*, SWIFT_CONTEXT void** self) { *out = *self; }

void* cancellation_handler_add(const void*, void*) { return nullptr; }
void cancellation_handler_remove(void*) {}

void continuation_resume(char* task) {
    ContinuationContext* context;
    memcpy(&context, task + kTaskResumeContextOffset, sizeof context);
    const void* owner;
    memcpy(&owner, task + kTaskResumeContextOffset + sizeof(void*), sizeof owner);
    free(task);
    if (context->state == kContinuationAwaited) {
        // The resume function loads its own frame through the context's parent word.
        ready_tasks().push_back({context->resume, context->error, nullptr, context, true, owner});
    } else {
        context->state = kContinuationResumed;
    }
}

constexpr size_t kMinimumResultBytes = 16;

// One `async let`: the guest's storage holds a pointer to this, and its task's parent word points back here.
struct AsyncLetState {
    bool finished;
    void* error;
    const void* waiter;
    void* waiter_context;
};

extern "C" void rt_async_let_task_finished();

void async_let_begin(void** storage, void*, void*, const AsyncFunctionPointer* entry, void* closure_context, void* result) {
    auto* state = static_cast<AsyncLetState*>(calloc(1, sizeof(AsyncLetState)));
    *storage = state;
    auto* context = static_cast<AsyncContextHeader*>(calloc(1, entry->context_size));
    context->parent = state;
    context->resume_parent = reinterpret_cast<void*>(rt_async_let_task_finished);
    const char* base = reinterpret_cast<const char*>(&entry->function);
    ready_tasks().push_back({base + entry->function, closure_context, result, context, false, g_current_task});
}

// Returns true if the task already finished, in which case the caller continues immediately.
extern "C" bool rt_async_let_ready(void** storage, const void* resume, void* resume_context) {
    auto* state = static_cast<AsyncLetState*>(*storage);
    if (state->finished) return true;
    state->waiter = resume;
    state->waiter_context = resume_context;
    return false;
}

extern "C" void* rt_async_let_error(void** storage) { return static_cast<AsyncLetState*>(*storage)->error; }

extern "C" void rt_async_let_note_finished(AsyncLetState* state, void* error) {
    state->finished = true;
    state->error = error;
    if (state->waiter != nullptr) ready_tasks().push_back({state->waiter, state->error, nullptr, state->waiter_context, true, g_current_task});
}

// A task group: child tasks run on this thread and queue up as they finish, for `next` to take in order.
struct GroupChild {
    void* result;
    void* error;
};

struct TaskGroupState {
    void* result_type;
    int64_t pending = 0;
    std::deque<GroupChild> finished;
    void* waiter_result = nullptr;
    const void* waiter_resume = nullptr;
    void* waiter_context = nullptr;
    void* drain_context = nullptr;
};

// A child's context parent word points here, as an async let's does.
struct GroupChildContext {
    TaskGroupState* group;
    void* result;
};

extern "C" void rt_group_task_finished();

constexpr size_t kWitnessInitializeWithTakeOffset = 0x20;

// Writes Optional<T>: some(taken from `value`) or, for null, none.
void group_write_result(void* out, void* value, void* type) {
    const char* witnesses = static_cast<const char*>(static_cast<void* const*>(type)[-1]);
    if (value != nullptr) {
        using Take = void* (*)(void*, void*, void*);
        Take take;
        memcpy(&take, witnesses + kWitnessInitializeWithTakeOffset, sizeof take);
        take(out, value, type);
    }
    using Store = void (*)(void*, uint32_t, uint32_t, void*);
    Store store;
    memcpy(&store, witnesses + kWitnessStoreEnumTagOffset, sizeof store);
    store(out, value == nullptr ? 1 : 0, 1, type);
}

void task_group_initialize(void** group, void* result_type) {
    auto* state = new TaskGroupState();
    state->result_type = result_type;
    group[0] = state;
}

void task_group_destroy(void** group) { delete static_cast<TaskGroupState*>(group[0]); }

extern "C" void rt_group_note_finished(GroupChildContext* child, void* error) {
    TaskGroupState* group = child->group;
    --group->pending;
    // awaitAllRemainingTasks: results are dropped, and its caller continues after the last one.
    if (group->drain_context != nullptr) {
        if (group->pending <= 0) {
            void* context = group->drain_context;
            group->drain_context = nullptr;
            ready_tasks().push_back({static_cast<AsyncContextHeader*>(context)->resume_parent, nullptr, nullptr, context, true, g_current_task});
        }
        delete child;
        return;
    }
    if (group->waiter_resume != nullptr) {
        const ReadyTask resume{group->waiter_resume, error, nullptr, group->waiter_context, true, g_current_task};
        if (error == nullptr) group_write_result(group->waiter_result, child->result, group->result_type);
        group->waiter_resume = nullptr;
        ready_tasks().push_back(resume);
    } else {
        group->finished.push_back({child->result, error});
    }
    delete child;
}

// Returns true if there's an answer already, with any thrown error in `*error`.
// Returns true if nothing is left to wait for.
extern "C" bool rt_group_drain(void** group_slot, void* context) {
    auto* group = static_cast<TaskGroupState*>(group_slot[0]);
    group->finished.clear();
    if (group->pending <= 0) return true;
    group->drain_context = context;
    return false;
}

extern "C" bool rt_group_wait(void* out, void** group_slot, const void* resume, void** context, void* caller, void** error) {
    auto* group = static_cast<TaskGroupState*>(group_slot[0]);
    context[0] = caller;
    context[1] = const_cast<void*>(resume);
    if (!group->finished.empty()) {
        const GroupChild child = group->finished.front();
        group->finished.pop_front();
        *error = child.error;
        if (child.error == nullptr) group_write_result(out, child.result, group->result_type);
        return true;
    }
    if (group->pending <= 0) {
        *error = nullptr;
        group_write_result(out, nullptr, group->result_type);
        return true;
    }
    group->waiter_result = out;
    group->waiter_resume = resume;
    // The resume function runs in the caller's frame, so that is what x22 must hold.
    group->waiter_context = caller;
    return false;
}

constexpr size_t kOptionKindGroup = 1;
constexpr size_t kOptionKindMask = 0xff;

// Task options are a linked list of {flags, parent, ...}; a group's record then holds the group.
struct TaskOptionRecord {
    size_t flags;
    TaskOptionRecord* parent;
    void** group;
};

extern "C" void rt_task_note_finished();

void deliver_task_value(const TaskCompletion& completion, void* out, void* type) {
    if (completion.error != nullptr || type == nullptr) return;
    witnesses_of(type)->initialize_with_copy(out, completion.result, type);
}

struct TaskValueReady {
    uint64_t ready;
    void* error;
};

// Parks the awaiting call unless the task is done; a parked one resumes through its own context's return address.
extern "C" TaskValueReady rt_task_value_begin(void* out, TaskObject* task, void* type, void* context) {
    TaskCompletion& completion = *task->completion;
    if (completion.finished) {
        deliver_task_value(completion, out, type);
        // Every awaiter owns the error it catches, and the task keeps its own reference.
        return {1, completion.error != nullptr ? rt_swift_retain(completion.error) : nullptr};
    }
    static_cast<void**>(context)[2] = type;
    completion.waiters.push_back({out, context});
    return {0, nullptr};
}

extern "C" void rt_task_completed(TaskCompletion* completion, void* error) {
    completion->finished = true;
    completion->error = error;
    for (const TaskWaiter& waiter : completion->waiters) {
        deliver_task_value(*completion, waiter.out, static_cast<void**>(waiter.context)[2]);
        ready_tasks().push_back({static_cast<AsyncContextHeader*>(waiter.context)->resume_parent, error != nullptr ? rt_swift_retain(error) : nullptr, nullptr, waiter.context, true, g_current_task});
    }
    completion->waiters.clear();
}

extern "C" void rt_task_value();

TaskCreation task_create(size_t, TaskOptionRecord* options, void* result_type, const AsyncFunctionPointer* entry, void* closure_context) {
    auto* task = static_cast<TaskObject*>(calloc(1, sizeof(TaskObject)));
    task->header.metadata = const_cast<uint64_t*>(&kTaskMetadata.kind);
    new (&task->header.references) std::atomic<uint64_t>(2);

    auto* context = static_cast<AsyncContextHeader*>(calloc(1, entry->context_size));
    task->completion = new TaskCompletion;
    context->parent = task->completion;
    context->resume_parent = reinterpret_cast<void*>(rt_task_note_finished);

    size_t result_size = kMinimumResultBytes;
    if (result_type != nullptr) {
        memcpy(&result_size, static_cast<const char*>(static_cast<void* const*>(result_type)[-1]) + kValueWitnessSizeOffset, sizeof result_size);
        if (result_size < kMinimumResultBytes) result_size = kMinimumResultBytes;
    }
    const char* base = reinterpret_cast<const char*>(&entry->function);
    void* result = calloc(1, result_size);
    task->completion->result = result;
    for (; options != nullptr; options = options->parent) {
        if ((options->flags & kOptionKindMask) != kOptionKindGroup) continue;
        auto* group = static_cast<TaskGroupState*>(options->group[0]);
        ++group->pending;
        context->parent = new GroupChildContext{group, result};
        context->resume_parent = reinterpret_cast<void*>(rt_group_task_finished);
        break;
    }
    ready_tasks().push_back({base + entry->function, closure_context, result, context, false, task});
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
        auto* header = static_cast<HeapObject*>(calloc(1, sizeof(HeapObject)));
        header->metadata = const_cast<uint64_t*>(&kMainActorMetadata.kind);
        new (&header->references) std::atomic<uint64_t>(kImmortal);
        return header;
    }();
    return actor;
}

// Duration and ContinuousClock.Instant are one 128-bit count of attoseconds, in two words.
struct ClockValue {
    uint64_t low;
    int64_t high;
};

__int128 attoseconds(ClockValue value) { return (static_cast<__int128>(value.high) << 64) | value.low; }
ClockValue clock_value(__int128 value) { return {static_cast<uint64_t>(value), static_cast<int64_t>(value >> 64)}; }

constexpr __int128 kAttosecondsPerNanosecond = 1000000000;

ClockValue instant_now() {
    timespec now{};
    clock_gettime(CLOCK_MONOTONIC, &now);
    return clock_value((static_cast<__int128>(now.tv_sec) * 1000000000 + now.tv_nsec) * kAttosecondsPerNanosecond);
}

SWIFTCC void continuous_clock_init(SWIFT_OUT void*) {}
SWIFTCC void clock_now(SWIFT_OUT ClockValue* out) { *out = instant_now(); }
SWIFTCC void instant_advanced(SWIFT_OUT ClockValue* out, uint64_t low, int64_t high, SWIFT_CONTEXT const ClockValue* self) {
    *out = clock_value(attoseconds(*self) + attoseconds({low, high}));
}
SWIFTCC void instant_protocol_advanced(SWIFT_OUT ClockValue* out, const ClockValue* by, void*, void*, SWIFT_CONTEXT const ClockValue* self) {
    *out = clock_value(attoseconds(*self) + attoseconds(*by));
}
SWIFTCC ClockValue instant_duration_to(const ClockValue* other, SWIFT_CONTEXT const ClockValue* self) {
    return clock_value(attoseconds(*other) - attoseconds(*self));
}
bool duration_less(uint64_t left_low, int64_t left_high, uint64_t right_low, int64_t right_high) {
    return attoseconds({left_low, left_high}) < attoseconds({right_low, right_high});
}
// ContinuousClock.Instant is the only Comparable this runtime sees through the dispatch thunk.
bool instant_greater(const ClockValue* left, const ClockValue* right) { return attoseconds(*left) > attoseconds(*right); }

struct Sleeper {
    void* context;
    const void* owner;
};

std::multimap<int64_t, Sleeper>& sleepers() {
    static std::multimap<int64_t, Sleeper> timers;
    return timers;
}

void* cancellation_error() { return foundation_make_error("The task was cancelled.", 1); }

// Records the sleeper; returning to the scheduler is how a task suspends.
extern "C" void rt_clock_sleep_begin(const ClockValue* until, void* context) {
    if (g_cancelled_tasks.count(g_current_task) != 0) {
        ready_tasks().push_back({static_cast<AsyncContextHeader*>(context)->resume_parent, cancellation_error(), nullptr, context, true, g_current_task});
        return;
    }
    sleepers().insert({static_cast<int64_t>(attoseconds(*until) / kAttosecondsPerNanosecond), {context, g_current_task}});
}

SWIFTCC void task_cancel(SWIFT_CONTEXT const void* task) {
    g_cancelled_tasks.insert(task);
    auto& timers = sleepers();
    for (auto entry = timers.begin(); entry != timers.end();) {
        if (entry->second.owner == task) {
            void* context = entry->second.context;
            ready_tasks().push_back({static_cast<AsyncContextHeader*>(context)->resume_parent, cancellation_error(), nullptr, context, true, task});
            entry = timers.erase(entry);
        } else {
            ++entry;
        }
    }
}

bool task_equal(const void* left, const void* right) { return left == right; }

// Isolated deinit: there is one guest thread, so it already runs on the right executor.
SWIFTCC void task_deinit_on_executor(void* object, SWIFTCC void (*work)(void*), void*, void*, size_t) { work(object); }

AsyncFunctionPointer g_clock_sleep_record;
AsyncFunctionPointer g_task_value_record;

struct ExecutorReference {
    uint64_t identity;
    uint64_t implementation;
};

ExecutorReference main_actor_executor() { return {0, 0}; }

// TaskPriority is reached through its metadata here, so it is one tagged word returned indirectly.
constexpr uint64_t kTaskPriorityUserInitiated = 25;
constexpr uint64_t kImmediateTag = 1ULL << 63;

SWIFTCC void task_priority_user_initiated(SWIFT_OUT uint64_t* out) { *out = kImmediateTag | kTaskPriorityUserInitiated; }
SWIFTCC uint8_t task_priority_raw_value(SWIFT_CONTEXT const uint64_t* self) { return static_cast<uint8_t>(*self); }

}  // namespace

// Every actor shares the one guest thread, so a hop just continues: the resume function is in x0, the context stays in x22.
// Shared pieces of the async entry points below: x16 continues the caller, or the task has suspended.
#define ASYNC_FINISH              \
    "  ldp x29, x30, [sp], #48\n" \
    "  br x16\n"                  \
    "1:\n"                        \
    "  ldp x29, x30, [sp], #48\n" \
    "  ret\n"
#define GROUP_WAIT         \
    "  mov x4, x22\n"      \
    "  add x5, sp, #32\n"  \
    "  bl rt_group_wait\n" \
    "  cbz w0, 1f\n"       \
    "  ldr x20, [sp, #32]\n"
#define SAVE_CALLEE_REGISTERS        \
    "  stp x29, x30, [sp, #-112]!\n" \
    "  mov x29, sp\n"                \
    "  stp x19, x20, [sp, #16]\n"    \
    "  stp x21, x22, [sp, #32]\n"    \
    "  stp x23, x24, [sp, #48]\n"    \
    "  stp x25, x26, [sp, #64]\n"    \
    "  stp x27, x28, [sp, #80]\n"
#define RESTORE_CALLEE_REGISTERS  \
    "  ldp x19, x20, [sp, #16]\n" \
    "  ldp x21, x22, [sp, #32]\n" \
    "  ldp x23, x24, [sp, #48]\n" \
    "  ldp x25, x26, [sp, #64]\n" \
    "  ldp x27, x28, [sp, #80]\n" \
    "  ldp x29, x30, [sp], #112\n"

__asm__(
    ".text\n"
    ".globl rt_task_switch\n"
    "rt_task_switch:\n"
    "  br x0\n"
    // Where a task's top-level function returns: back to rt_start_task's frame.
    ".globl rt_task_note_finished\n"
    "rt_task_note_finished:\n"
    "  stp x29, x30, [sp, #-16]!\n"
    "  mov x29, sp\n"
    "  ldr x0, [x22]\n"
    "  mov x1, x20\n"
    "  bl rt_task_completed\n"
    "  ldp x29, x30, [sp], #16\n"
    "  ret\n"
    // Task.value: (result, task, Success, ...), returning through its own context as any async callee does.
    ".globl rt_task_value\n"
    "rt_task_value:\n"
    "  stp x29, x30, [sp, #-16]!\n"
    "  mov x29, sp\n"
    "  mov x3, x22\n"
    "  bl rt_task_value_begin\n"
    "  ldp x29, x30, [sp], #16\n"
    "  cbz x0, 2f\n"
    "  mov x20, x1\n"
    "  ldr x16, [x22, #8]\n"
    "  br x16\n"
    "2:\n"
    "  ret\n"
    ".globl rt_task_finished\n"
    "rt_task_finished:\n"
    "  ret\n"
    // Awaiting a continuation not resumed yet returns to whoever is running this task; a resume passes its error in x20.
    ".globl rt_continuation_await\n"
    "rt_continuation_await:\n"
    "  ldr x9, [x0, #16]\n"
    "  cmp x9, #2\n"
    "  b.eq 1f\n"
    "  mov x9, #1\n"
    "  str x9, [x0, #16]\n"
    "  ret\n"
    "1:\n"
    "  mov x22, x0\n"
    "  ldr x20, [x0, #24]\n"
    "  ldr x1, [x0, #8]\n"
    "  br x1\n"
    // x22 is the finished task's own context, whose parent word holds the state; a thrown error arrives in x20.
    ".globl rt_async_let_task_finished\n"
    "rt_async_let_task_finished:\n"
    "  stp x29, x30, [sp, #-16]!\n"
    "  mov x29, sp\n"
    "  ldr x0, [x22]\n"
    "  mov x1, x20\n"
    "  bl rt_async_let_note_finished\n"
    "  ldp x29, x30, [sp], #16\n"
    "  ret\n"
    // get, get_throwing and finish: (let, buffer, resume function, resume context).
    ".globl rt_async_let_wait\n"
    "rt_async_let_wait:\n"
    // The resume function continues in the caller's frame, so x22 is what must survive; x3 is runtime scratch.
    "  stp x29, x30, [sp, #-48]!\n"
    "  mov x29, sp\n"
    "  stp x0, x2, [sp, #16]\n"
    "  mov x1, x2\n"
    "  mov x2, x22\n"
    "  bl rt_async_let_ready\n"
    "  cbz w0, 1f\n"
    "  ldr x0, [sp, #16]\n"
    "  bl rt_async_let_error\n"
    "  mov x20, x0\n"
    "  ldr x16, [sp, #24]\n" ASYNC_FINISH
    // Clock.sleep(until:tolerance:), with `until` in x0: it never finishes on the caller's stack.
    ".globl rt_clock_sleep\n"
    "rt_clock_sleep:\n"
    "  stp x29, x30, [sp, #-16]!\n"
    "  mov x29, sp\n"
    "  mov x1, x22\n"
    "  bl rt_clock_sleep_begin\n"
    "  ldp x29, x30, [sp], #16\n"
    "  ret\n"
    // As an async let's: x22 is the finished child's context, and x20 any error it threw.
    // withChecked(Throwing)Continuation: (result, isolation, isolation, function, function, body, context, T).
    // It suspends here; the resume runs the caller's own continuation, because the context is laid out for it.
    ".globl rt_with_checked_continuation\n"
    "rt_with_checked_continuation:\n"
    "  stp x29, x30, [sp, #-16]!\n"
    "  mov x29, sp\n"
    "  mov x1, x5\n"
    "  mov x2, x6\n"
    "  mov x3, x22\n"
    "  bl rt_checked_begin\n"
    "  ldp x29, x30, [sp], #16\n"
    "  ret\n"
    ".globl rt_group_task_finished\n"
    "rt_group_task_finished:\n"
    "  stp x29, x30, [sp, #-16]!\n"
    "  mov x29, sp\n"
    "  ldr x0, [x22]\n"
    "  mov x1, x20\n"
    "  bl rt_group_note_finished\n"
    "  ldp x29, x30, [sp], #16\n"
    "  ret\n"
    // (result, group, resume function, resume context): parks, or continues at once in the caller's frame, still in x22.
    // awaitAllRemainingTasks, an ordinary async function: self (x20) holds the group, and x22 is its own context.
    ".globl rt_group_await_all\n"
    "rt_group_await_all:\n"
    "  stp x29, x30, [sp, #-16]!\n"
    "  mov x29, sp\n"
    "  ldr x0, [x20]\n"
    "  mov x1, x22\n"
    "  bl rt_group_drain\n"
    "  ldp x29, x30, [sp], #16\n"
    "  cbz w0, 1f\n"
    "  mov x20, xzr\n"
    "  ldr x16, [x22, #8]\n"
    "  br x16\n"
    "1:\n"
    "  ret\n"
    // Iterator.next, also an ordinary async function: (result), with self in x20 holding the group.
    ".globl rt_group_iterator_next\n"
    "rt_group_iterator_next:\n"
    "  stp x29, x30, [sp, #-48]!\n"
    "  mov x29, sp\n"
    "  ldr x1, [x20]\n"
    "  ldr x2, [x22, #8]\n"
    "  add x3, sp, #16\n" GROUP_WAIT
    "  ldr x16, [x22, #8]\n" ASYNC_FINISH
    ".globl rt_group_wait_entry\n"
    "rt_group_wait_entry:\n"
    "  stp x29, x30, [sp, #-48]!\n"
    "  mov x29, sp\n"
    "  stp x2, x3, [sp, #16]\n" GROUP_WAIT
    "  ldr x16, [sp, #16]\n" ASYNC_FINISH
    ".globl rt_resume_task\n"
    "rt_resume_task:\n" SAVE_CALLEE_REGISTERS
    "  mov x22, x1\n"
    "  mov x20, x2\n"
    "  blr x0\n" RESTORE_CALLEE_REGISTERS
    "  ret\n"
    ".globl rt_start_task\n"
    "rt_start_task:\n" SAVE_CALLEE_REGISTERS
    "  mov x16, x0\n"
    "  mov x0, x1\n"
    "  mov x20, x2\n"
    "  mov x22, x3\n"
    "  blr x16\n" RESTORE_CALLEE_REGISTERS
    "  ret\n");

extern "C" void rt_task_switch();
extern "C" void rt_group_wait_entry();
extern "C" void rt_group_await_all();
extern "C" void rt_group_iterator_next();
extern "C" void rt_continuation_await();
extern "C" void rt_async_let_wait();
extern "C" void rt_clock_sleep();

int64_t concurrency_next_deadline() {
    const int64_t sleeper = sleepers().empty() ? 0 : sleepers().begin()->first;
    const int64_t work = dispatch_next_deadline();
    if (sleeper == 0 || work == 0) return sleeper + work;
    return sleeper < work ? sleeper : work;
}

void concurrency_fire_timers() {
    auto& timers = sleepers();
    const int64_t now = static_cast<int64_t>(attoseconds(instant_now()) / kAttosecondsPerNanosecond);
    while (!timers.empty() && timers.begin()->first <= now) {
        const Sleeper sleeper = timers.begin()->second;
        timers.erase(timers.begin());
        ready_tasks().push_back({static_cast<AsyncContextHeader*>(sleeper.context)->resume_parent, nullptr, nullptr, sleeper.context, true, sleeper.owner});
    }
}

void concurrency_start(const void* async_function, void* context) {
    const auto* entry = static_cast<const AsyncFunctionPointer*>(async_function);
    auto* async_context = static_cast<AsyncContextHeader*>(calloc(1, entry->context_size));
    async_context->resume_parent = reinterpret_cast<void*>(rt_task_finished);
    const char* base = reinterpret_cast<const char*>(&entry->function);
    ready_tasks().push_back({base + entry->function, context, calloc(1, kMinimumResultBytes), async_context, false, g_current_task});
}

void concurrency_resume_call(void* context, void* error) {
    ready_tasks().push_back({static_cast<AsyncContextHeader*>(context)->resume_parent, error, nullptr, context, true, g_current_task});
}

void concurrency_drain() {
    auto& queue = ready_tasks();
    do {
        while (!queue.empty()) {
            const ReadyTask task = queue.front();
            queue.pop_front();
            g_current_task = task.owner;
            if (task.is_resume)
                rt_resume_task(task.function, task.async_context, task.closure_context);
            else
                rt_start_task(task.function, task.result, task.closure_context, task.async_context);
        }
    } while (dispatch_run_ready() && !queue.empty());
}

void* concurrency_symbol(std::string_view name) {
    static const bool registered = [] {
        register_foreign_witness("_$sScMScAsWP", "_$sScA15unownedExecutorScevgTq", reinterpret_cast<const void*>(main_actor_executor));
        register_foreign_witness("_$sScMScAsMc", "_$sScA15unownedExecutorScevgTq", reinterpret_cast<const void*>(main_actor_executor));
        g_clock_sleep_record.function = static_cast<int32_t>(reinterpret_cast<const char*>(rt_clock_sleep) - reinterpret_cast<const char*>(&g_clock_sleep_record.function));
        g_clock_sleep_record.context_size = sizeof(AsyncContextHeader);
        g_task_value_record.function = static_cast<int32_t>(reinterpret_cast<const char*>(rt_task_value) - reinterpret_cast<const char*>(&g_task_value_record.function));
        // Room past the header for the awaited type, kept for when the task finishes.
        g_task_value_record.context_size = sizeof(AsyncContextHeader) + sizeof(void*);
        g_checked_continuation_record.function = static_cast<int32_t>(reinterpret_cast<const char*>(rt_with_checked_continuation) - reinterpret_cast<const char*>(&g_checked_continuation_record.function));
        g_checked_continuation_record.context_size = sizeof(ContinuationContext);
        g_group_await_all_record.function = static_cast<int32_t>(reinterpret_cast<const char*>(rt_group_await_all) - reinterpret_cast<const char*>(&g_group_await_all_record.function));
        g_group_await_all_record.context_size = sizeof(AsyncContextHeader);
        g_group_next_record.function = static_cast<int32_t>(reinterpret_cast<const char*>(rt_group_iterator_next) - reinterpret_cast<const char*>(&g_group_next_record.function));
        g_group_next_record.context_size = sizeof(AsyncContextHeader);
        return true;
    }();
    (void)registered;
    static const std::unordered_map<std::string_view, void*> table = {
        {"$sS2cEycfC", reinterpret_cast<void*>(+[] {})},
        {"$sScM6sharedScMvgZ", reinterpret_cast<void*>(main_actor_shared)},
        {"$sScP13userInitiatedScPvgZ", reinterpret_cast<void*>(task_priority_user_initiated)},
        {"$sScP8rawValues5UInt8Vvg", reinterpret_cast<void*>(task_priority_raw_value)},
        {"$ss15ContinuousClockVABycfC", reinterpret_cast<void*>(continuous_clock_init)},
        {"$ss5ClockPss010ContinuousA0VRszrlE10continuousADvgZ", reinterpret_cast<void*>(continuous_clock_init)},
        {"$ss15ContinuousClockV3nowAB7InstantVvg", reinterpret_cast<void*>(clock_now)},
        {"$ss5ClockP3now7InstantQzvgTj", reinterpret_cast<void*>(clock_now)},
        {"$ss15ContinuousClockV7InstantV8advanced2byADs8DurationV_tF", reinterpret_cast<void*>(instant_advanced)},
        {"$ss15InstantProtocolP8advanced2byx8DurationQz_tFTj", reinterpret_cast<void*>(instant_protocol_advanced)},
        {"$ss15ContinuousClockV7InstantV8duration2tos8DurationVAD_tF", reinterpret_cast<void*>(instant_duration_to)},
        {"$ss8DurationV1loiySbAB_ABtFZ", reinterpret_cast<void*>(duration_less)},
        {"$sSL1goiySbx_xtFZTj", reinterpret_cast<void*>(instant_greater)},
        {"$ss5ClockP5sleep5until9tolerancey7InstantQz_8DurationQzSgtYaKFTj", reinterpret_cast<void*>(rt_clock_sleep)},
        {"$ss5ClockP5sleep5until9tolerancey7InstantQz_8DurationQzSgtYaKFTjTu", &g_clock_sleep_record},
        {"$sScT5valuexvg", reinterpret_cast<void*>(rt_task_value)},
        {"$sScT5valuexvgTu", &g_task_value_record},
        {"$sScTss5NeverORs_rlE5valuexvg", reinterpret_cast<void*>(rt_task_value)},
        {"$sScTss5NeverORs_rlE5valuexvgTu", &g_task_value_record},
        {"$sScT6cancelyyF", reinterpret_cast<void*>(task_cancel)},
        {"$sScT2eeoiySbScTyxq_G_ABtFZ", reinterpret_cast<void*>(task_equal)},
        {"swift_task_deinitOnExecutor", reinterpret_cast<void*>(task_deinit_on_executor)},
        {"swift_asyncLet_begin", reinterpret_cast<void*>(async_let_begin)},
        {"swift_asyncLet_get", reinterpret_cast<void*>(rt_async_let_wait)},
        {"swift_asyncLet_get_throwing", reinterpret_cast<void*>(rt_async_let_wait)},
        {"swift_asyncLet_finish", reinterpret_cast<void*>(rt_async_let_wait)},
        {"swift_continuation_init", reinterpret_cast<void*>(continuation_init)},
        {"swift_continuation_await", reinterpret_cast<void*>(rt_continuation_await)},
        {"swift_continuation_resume", reinterpret_cast<void*>(continuation_resume)},
        {"swift_task_create", reinterpret_cast<void*>(task_create)},
        {"$sScC12continuation8functionScCyxq_GSccyxq_G_SStcfC", reinterpret_cast<void*>(checked_continuation_init)},
        {"$sScC6resume9returningyxn_tF", reinterpret_cast<void*>(checked_resume_returning)},
        {"$sScC6resume8throwingyq_n_tF", reinterpret_cast<void*>(checked_resume_throwing)},
        {"$ss31withCheckedThrowingContinuation9isolation8function_xScA_pSgYi_SSyScCyxs5Error_pGXEtYaKlFTu", &g_checked_continuation_record},
        {"$ss23withCheckedContinuation9isolation8function_xScA_pSgYi_SSyScCyxs5NeverOGXEtYalFTu", &g_checked_continuation_record},
        {"$sScG22awaitAllRemainingTasksyyYaF", reinterpret_cast<void*>(rt_group_await_all)},
        {"$sScG22awaitAllRemainingTasksyyYaFTu", &g_group_await_all_record},
        {"$sScG22awaitAllRemainingTasks9isolationyScA_pSgYi_tYaF", reinterpret_cast<void*>(rt_group_await_all)},
        {"$sScG22awaitAllRemainingTasks9isolationyScA_pSgYi_tYaFTu", &g_group_await_all_record},
        {"$sScG17makeAsyncIteratorScG0C0Vyx_GyF", reinterpret_cast<void*>(group_make_iterator)},
        {"$sScG8IteratorV4nextxSgyYaF", reinterpret_cast<void*>(rt_group_iterator_next)},
        {"$sScG8IteratorV4nextxSgyYaFTu", &g_group_next_record},
        {"swift_taskGroup_initialize", reinterpret_cast<void*>(task_group_initialize)},
        {"swift_taskGroup_destroy", reinterpret_cast<void*>(task_group_destroy)},
        {"swift_taskGroup_wait_next_throwing", reinterpret_cast<void*>(rt_group_wait_entry)},
        {"swift_continuation_throwingResume", reinterpret_cast<void*>(continuation_resume)},
        {"swift_continuation_throwingResumeWithError", reinterpret_cast<void*>(continuation_resume_with_error)},
        {"swift_task_addCancellationHandler", reinterpret_cast<void*>(cancellation_handler_add)},
        {"swift_task_removeCancellationHandler", reinterpret_cast<void*>(cancellation_handler_remove)},
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
