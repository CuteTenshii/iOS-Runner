#include "dispatch_shim.h"

#include <time.h>

#include <cstdlib>
#include <deque>
#include <map>
#include <string>
#include <unordered_map>
#include <vector>

#include "concurrency_shim.h"
#include "engine.h"
#include "objc_runtime.h"

#define SWIFTCC __attribute__((swiftcall))
#define SWIFT_OUT __attribute__((swift_indirect_result))
#define SWIFT_CONTEXT __attribute__((swift_context))

extern "C" void* rt_call_witness(const void* function, void* out, void* self, void* type, void* table);
extern "C" void* darwin_block_copy(const void*);
extern "C" void rt_registrar_mark_dirty_c();

namespace {

constexpr uint64_t kImmediate = 1ULL << 63;
constexpr int64_t kNanosecondsPerSecond = 1000000000;

struct BlockLiteral {
    void* isa;
    int32_t flags;
    int32_t reserved;
    void (*invoke)(void*);
};

// Work is either a block, or a plain C function with its context.
struct Work {
    void* block;
    void (*function)(void*);
    void* context;
};

std::deque<Work>& ready_work() {
    static std::deque<Work> work;
    return work;
}

std::multimap<int64_t, Work>& delayed_work() {
    static std::multimap<int64_t, Work> work;
    return work;
}

int64_t monotonic_now() {
    timespec now{};
    clock_gettime(CLOCK_MONOTONIC, &now);
    return now.tv_sec * kNanosecondsPerSecond + now.tv_nsec;
}

void perform(const Work& work) {
    if (work.block != nullptr) static_cast<BlockLiteral*>(work.block)->invoke(work.block);
    else work.function(work.context);
    rt_registrar_mark_dirty_c();
}

// A wait runs other queued work inline, because whatever it waits for can only happen on this thread.
template <typename Done>
void drain_until(Done done, int64_t deadline) {
    while (!done()) {
        concurrency_drain();
        if (done()) return;
        if (!dispatch_run_ready() && ready_work().empty()) {
            if (deadline != 0 && monotonic_now() >= deadline) return;
            timespec pause{0, 1000000};
            nanosleep(&pause, nullptr);
            concurrency_fire_timers();
        }
    }
}

void* queue_object(const std::string& label) {
    static std::map<std::string, void*> queues;
    void*& queue = queues[label];
    if (queue == nullptr) queue = objc_framework_object("OS_dispatch_queue");
    return queue;
}

void* queue_main() { return queue_object("main"); }
void* queue_global(uint64_t) { return queue_object("global"); }
SWIFTCC void* queue_label_init(uint64_t label_bits, uint64_t label_object, void*, void*, void*, void*) {
    return queue_object(std::to_string(label_bits) + ":" + std::to_string(label_object));
}

void enqueue_block(void* block) { ready_work().push_back({darwin_block_copy(block), nullptr, nullptr}); }

SWIFTCC void queue_async(void* group, void*, void*, void* block, SWIFT_CONTEXT void*);
SWIFTCC void queue_async_after(const uint64_t* deadline, void*, void*, void* block, SWIFT_CONTEXT void*) {
    delayed_work().insert({static_cast<int64_t>(*deadline), {darwin_block_copy(block), nullptr, nullptr}});
}

SWIFTCC void queue_sync(SWIFT_OUT void* out, const void* body, void* context, void*, SWIFT_CONTEXT void*) { rt_call_witness(body, out, context, nullptr, nullptr); }

SWIFTCC void time_now(SWIFT_OUT uint64_t* out) { *out = static_cast<uint64_t>(monotonic_now()); }
SWIFTCC void time_from_uptime(SWIFT_OUT uint64_t* out, uint64_t nanoseconds) { *out = nanoseconds; }
SWIFTCC uint64_t time_uptime(SWIFT_CONTEXT const uint64_t* self) { return *self; }
SWIFTCC void time_plus_seconds(SWIFT_OUT uint64_t* out, const uint64_t* time, double seconds) { *out = *time + static_cast<uint64_t>(seconds * kNanosecondsPerSecond); }

SWIFTCC void immediate_indirect(SWIFT_OUT uint64_t* out) { *out = kImmediate; }

struct GroupState {
    int64_t entered = 0;
    std::vector<Work> waiting;
};

std::unordered_map<void*, GroupState>& groups() {
    static std::unordered_map<void*, GroupState> states;
    return states;
}

void* group_create() { return objc_framework_object("OS_dispatch_group"); }
void group_enter(void* group) { ++groups()[group].entered; }
void group_leave(void* group) {
    GroupState& state = groups()[group];
    if (--state.entered > 0) return;
    for (const Work& work : state.waiting) ready_work().push_back(work);
    state.waiting.clear();
}

SWIFTCC void group_wait(SWIFT_CONTEXT void* group) {
    drain_until([group] { return groups()[group].entered <= 0; }, 0);
}

SWIFTCC void group_notify(void*, void*, void*, void* block, SWIFT_CONTEXT void* group) {
    Work work{darwin_block_copy(block), nullptr, nullptr};
    GroupState& state = groups()[group];
    if (state.entered <= 0) ready_work().push_back(work);
    else state.waiting.push_back(work);
}

// The block holds the group entered from enqueue until it has run.
struct GroupedBlock {
    void* group;
    void* block;
};

void run_grouped(void* raw) {
    auto* grouped = static_cast<GroupedBlock*>(raw);
    static_cast<BlockLiteral*>(grouped->block)->invoke(grouped->block);
    group_leave(grouped->group);
    delete grouped;
}

SWIFTCC void queue_async(void* group, void*, void*, void* block, SWIFT_CONTEXT void*) {
    if (group == nullptr) {
        enqueue_block(block);
        return;
    }
    group_enter(group);
    ready_work().push_back({nullptr, run_grouped, new GroupedBlock{group, darwin_block_copy(block)}});
}

struct Semaphore {
    void* isa;
    uint64_t references;
    int64_t value;
};

void* semaphore_create(int64_t value) {
    auto* semaphore = static_cast<Semaphore*>(objc_framework_object("OS_dispatch_semaphore"));
    semaphore->value = value;
    return semaphore;
}

int64_t semaphore_signal(Semaphore* semaphore) {
    ++semaphore->value;
    return 0;
}

constexpr uint64_t kDispatchTimeForever = ~0ULL;

int64_t semaphore_wait(Semaphore* semaphore, uint64_t timeout) {
    drain_until([semaphore] { return semaphore->value > 0; }, timeout == kDispatchTimeForever ? 0 : static_cast<int64_t>(timeout));
    if (semaphore->value <= 0) return 1;
    --semaphore->value;
    return 0;
}

uint64_t dispatch_time_after(uint64_t when, int64_t delta) {
    if (when == kDispatchTimeForever) return when;
    return (when == 0 ? static_cast<uint64_t>(monotonic_now()) : when) + static_cast<uint64_t>(delta);
}

void dispatch_sync_block(void*, void* block) { static_cast<BlockLiteral*>(block)->invoke(block); }
void dispatch_ignored(void*) {}

const int32_t kQosClassDefault = 0;
const int32_t kQosClassUserInitiated = 1;
const int32_t kQosClassUtility = 2;
const int32_t kFrequencyInherit = 0;
const int32_t kFrequencyWorkItem = 1;

}  // namespace

void dispatch_enqueue(void (*function)(void*), void* context) { ready_work().push_back({nullptr, function, context}); }

bool dispatch_run_ready() {
    const int64_t now = monotonic_now();
    auto& delayed = delayed_work();
    while (!delayed.empty() && delayed.begin()->first <= now) {
        ready_work().push_back(delayed.begin()->second);
        delayed.erase(delayed.begin());
    }
    bool ran = false;
    // Only what is queued now: work that requeues itself must not starve the render loop.
    for (size_t pending = ready_work().size(); pending > 0; --pending) {
        const Work work = ready_work().front();
        ready_work().pop_front();
        perform(work);
        ran = true;
    }
    return ran;
}

int64_t dispatch_next_deadline() {
    if (!ready_work().empty()) return monotonic_now();
    return delayed_work().empty() ? 0 : delayed_work().begin()->first;
}

void* dispatch_symbol(std::string_view name) {
#define ENTRY(symbol, target) {symbol, reinterpret_cast<void*>(target)}
    static const std::unordered_map<std::string_view, void*> table = {
        ENTRY("$sSo17OS_dispatch_queueC8DispatchE4mainABvgZ", queue_main),
        ENTRY("$sSo17OS_dispatch_queueC8DispatchE6global3qosAbC0D3QoSV0G6SClassO_tFZ", queue_global),
        ENTRY("$sSo17OS_dispatch_queueC8DispatchE5label3qos10attributes20autoreleaseFrequency6targetABSS_AC0D3QoSVAbCE10AttributesVAbCE011AutoreleaseI0OABSgtcfC", queue_label_init),
        ENTRY("$sSo17OS_dispatch_queueC8DispatchE5async5group3qos5flags7executeySo0a1_b1_F0CSg_AC0D3QoSVAC0D13WorkItemFlagsVyyXBtF", queue_async),
        ENTRY("$sSo17OS_dispatch_queueC8DispatchE10asyncAfter8deadline3qos5flags7executeyAC0D4TimeV_AC0D3QoSVAC0D13WorkItemFlagsVyyXBtF", queue_async_after),
        ENTRY("$sSo17OS_dispatch_queueC8DispatchE4sync7executexxyKXE_tKlF", queue_sync),
        ENTRY("$s8Dispatch0A4TimeV3nowACyFZ", time_now),
        ENTRY("$s8Dispatch0A4TimeV17uptimeNanosecondsACs6UInt64V_tcfC", time_from_uptime),
        ENTRY("$s8Dispatch0A4TimeV17uptimeNanosecondss6UInt64Vvg", time_uptime),
        ENTRY("$s8Dispatch1poiyAA0A4TimeVAD_SdtF", time_plus_seconds),
        ENTRY("$s8Dispatch0A3QoSV11unspecifiedACvgZ", immediate_indirect),
        ENTRY("$s8Dispatch0A3QoSV7defaultACvgZ", immediate_indirect),
        ENTRY("$s8Dispatch0A3QoSV7utilityACvgZ", immediate_indirect),
        ENTRY("$s8Dispatch0A3QoSV10backgroundACvgZ", immediate_indirect),
        ENTRY("$s8Dispatch0A3QoSV13userInitiatedACvgZ", immediate_indirect),
        ENTRY("$s8Dispatch0A13WorkItemFlagsV10enforceQoSACvgZ", immediate_indirect),
        ENTRY("$s8Dispatch0A3QoSV0B6SClassO7defaultyA2EmFWC", const_cast<int32_t*>(&kQosClassDefault)),
        ENTRY("$s8Dispatch0A3QoSV0B6SClassO13userInitiatedyA2EmFWC", const_cast<int32_t*>(&kQosClassUserInitiated)),
        ENTRY("$s8Dispatch0A3QoSV0B6SClassO7utilityyA2EmFWC", const_cast<int32_t*>(&kQosClassUtility)),
        ENTRY("$sSo17OS_dispatch_queueC8DispatchE20AutoreleaseFrequencyO7inherityA2EmFWC", const_cast<int32_t*>(&kFrequencyInherit)),
        ENTRY("$sSo17OS_dispatch_queueC8DispatchE20AutoreleaseFrequencyO8workItemyA2EmFWC", const_cast<int32_t*>(&kFrequencyWorkItem)),
        ENTRY("$sSo17OS_dispatch_groupC8DispatchE4waityyF", group_wait),
        ENTRY("$sSo17OS_dispatch_groupC8DispatchE6notify3qos5flags5queue7executeyAC0D3QoSV_AC0D13WorkItemFlagsVSo0a1_b1_H0CyyXBtF", group_notify),
        ENTRY("dispatch_group_create", group_create),
        ENTRY("dispatch_group_enter", group_enter),
        ENTRY("dispatch_group_leave", group_leave),
        ENTRY("dispatch_semaphore_create", semaphore_create),
        ENTRY("dispatch_semaphore_signal", semaphore_signal),
        ENTRY("dispatch_semaphore_wait", semaphore_wait),
        ENTRY("dispatch_time", dispatch_time_after),
        ENTRY("dispatch_sync", dispatch_sync_block),
        ENTRY("dispatch_activate", dispatch_ignored),
        ENTRY("dispatch_release", dispatch_ignored),
    };
#undef ENTRY
    const auto found = table.find(name);
    return found == table.end() ? nullptr : found->second;
}
