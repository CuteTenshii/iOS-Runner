#include "combine_shim.h"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <memory>
#include <new>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "dispatch_shim.h"
#include "engine.h"
#include "key_path.h"
#include "macho.h"
#include "objc_runtime.h"
#include "stdlib_shim.h"
#include "swift_abi.h"
#include "swift_runtime.h"
#include "symbols.h"
#include "type_system.h"

extern "C" void rt_registrar_mark_dirty_c();
extern "C" void* rt_swift_bridgeObjectRetain(void*);
extern "C" void rt_swift_bridgeObjectRelease(void*);

namespace {

struct Sink;
using SinkRef = std::shared_ptr<Sink>;
using Cancel = std::function<void()>;
using Hold = std::shared_ptr<void>;

// One subscription's downstream. `type` is the value's runtime type, null only for a CombineLatest tuple.
struct Sink {
    std::function<void(void* value, void* type)> value;
    // A null error means the upstream finished.
    std::function<void(void* error, void* error_type)> complete;
};

struct Publisher;

struct PublisherState {
    // Null for a hot publisher, such as a subject, which keeps its sinks here instead.
    std::function<Cancel(const SinkRef&)> subscribe;
    std::vector<std::pair<uint64_t, SinkRef>> sinks;
    void* output_type = nullptr;
    struct PublishedBox* box = nullptr;
    bool finished = false;
    void* error = nullptr;
    void* error_type = nullptr;
};

// Every framework publisher value is one reference to this, whichever operator built it.
struct Publisher {
    HeapObject header;
    PublisherState* state;
};

void destroy_publisher(void* raw);
const HeapMetadata kPublisherMetadata = {destroy_publisher, nullptr, 0};

std::unordered_set<const void*>& live_publishers() {
    static std::unordered_set<const void*> publishers;
    return publishers;
}

void destroy_publisher(void* raw) {
    live_publishers().erase(raw);
    delete static_cast<Publisher*>(raw)->state;
    free(raw);
}

Publisher* new_publisher() {
    auto* publisher = static_cast<Publisher*>(calloc(1, sizeof(Publisher)));
    publisher->header.metadata = const_cast<uint64_t*>(&kPublisherMetadata.kind);
    new (&publisher->header.references) std::atomic<uint64_t>(1);
    publisher->state = new PublisherState();
    live_publishers().insert(publisher);
    return publisher;
}

Hold hold(void* object) {
    if (object != nullptr) rt_swift_retain(object);
    return Hold(object, [](void* held) {
        if (held != nullptr) rt_swift_release(held);
    });
}

// A subject or future is an object the app allocated, so its publisher hangs off a side table.
Publisher* publisher_of(void* word) {
    if (live_publishers().count(word) != 0) return static_cast<Publisher*>(word);
    static std::unordered_map<void*, Publisher*> subjects;
    Publisher*& publisher = subjects[word];
    if (publisher == nullptr) publisher = new_publisher();
    return publisher;
}

void* new_value(void* type) {
    const size_t size = witnesses_of(type)->size;
    return calloc(1, size == 0 ? 1 : size);
}

void* copy_value(const void* value, void* type) {
    void* copy = new_value(type);
    witnesses_of(type)->initialize_with_copy(copy, const_cast<void*>(value), type);
    return copy;
}

void free_value(void* value, void* type) {
    witnesses_of(type)->destroy(value, type);
    free(value);
}

void* require_type(void* type, const char* operation) {
    if (type == nullptr) {
        engine_log("STOP: Combine %s needs the runtime type of a value that has none", operation);
        abort();
    }
    return type;
}

void* type_argument(const void* metadata, size_t index) { return static_cast<void* const*>(metadata)[2 + index]; }

Cancel attach(Publisher* publisher, const SinkRef& sink);

void send_value(Publisher* publisher, void* value, void* type) {
    const auto current = publisher->state->sinks;
    for (const auto& [id, sink] : current) sink->value(value, type);
    rt_registrar_mark_dirty_c();
}

void send_completion(Publisher* publisher, void* error, void* error_type) {
    PublisherState* state = publisher->state;
    if (state->finished) return;
    state->finished = true;
    state->error = error;
    state->error_type = error_type;
    const auto current = state->sinks;
    state->sinks.clear();
    for (const auto& [id, sink] : current) sink->complete(error, error_type);
    rt_registrar_mark_dirty_c();
}

SinkRef sink_of(std::function<void(void*, void*)> value, std::function<void(void*, void*)> complete) {
    auto sink = std::make_shared<Sink>();
    sink->value = std::move(value);
    sink->complete = std::move(complete);
    return sink;
}

// An operator that changes values and passes the upstream's completion through unchanged.
SinkRef value_sink(const SinkRef& downstream, std::function<void(void*, void*)> value) {
    return sink_of(std::move(value), [downstream](void* error, void* type) { downstream->complete(error, type); });
}

struct Upstream {
    Publisher* publisher;
    Hold keep;
};

Upstream upstream_of_word(void* word) { return {publisher_of(word), hold(word)}; }

Publisher* operator_publisher(void* output_type, std::function<Cancel(const SinkRef&)> subscribe) {
    Publisher* publisher = new_publisher();
    publisher->state->output_type = output_type;
    publisher->state->subscribe = std::move(subscribe);
    return publisher;
}

// Published<T> is one reference to this box, which stands in for the framework's storage.
struct PublishedBox {
    HeapObject header;
    void* type;
    void* value;
    Publisher* publisher;
};

Cancel attach(Publisher* publisher, const SinkRef& sink) {
    PublisherState* state = publisher->state;
    if (state->subscribe) return state->subscribe(sink);
    // Published replays its current value to each new subscriber.
    if (state->box != nullptr) sink->value(state->box->value, state->box->type);
    if (state->finished) {
        sink->complete(state->error, state->error_type);
        return [] {};
    }
    static uint64_t next_id = 1;
    const uint64_t id = next_id++;
    state->sinks.emplace_back(id, sink);
    const Hold keep = hold(publisher);
    return [publisher, id, keep] {
        auto& sinks = publisher->state->sinks;
        sinks.erase(std::remove_if(sinks.begin(), sinks.end(), [id](const auto& entry) { return entry.first == id; }), sinks.end());
    };
}

std::unordered_map<void*, void*>& future_results() {
    static std::unordered_map<void*, void*> results;
    return results;
}

// A publisher's Failure, from its generic arguments. An app's own publisher isn't covered, and counts as Never.
void* failure_type_of(void* type) {
    static void* const never = resolve_mangled_type("s5NeverO", nullptr);
    for (;;) {
        const char* symbol = foreign_type_name(type);
        if (symbol == nullptr) return never;
        const std::string_view name = symbol;
        const auto is = [&name](std::string_view part) { return name.find(part) != std::string_view::npos; };
        if (is("12AnyPublisherV") || is("18PassthroughSubjectC") || is("6FutureC") || is("O8SequenceV")) return type_argument(type, 1);
        if (is("4JustV") || is("9PublishedV") || is("NSNotificationCenter")) return never;
        type = type_argument(type, is("O7FlatMapV") || is("O5CatchV") ? 1 : 0);
        if (type == nullptr) return never;
    }
}

void call_completion(const void* function, void* context, void* failure, void* error);

void destroy_published(void* raw) {
    auto* box = static_cast<PublishedBox*>(raw);
    witnesses_of(box->type)->destroy(box->value, box->type);
    free(box->value);
    free(box);
}

const HeapMetadata kPublishedMetadata = {destroy_published, nullptr, 0};

SWIFTCC void published_init(SWIFT_OUT PublishedBox** out, void* value, void* type) {
    auto* box = static_cast<PublishedBox*>(calloc(1, sizeof(PublishedBox)));
    box->header.metadata = const_cast<uint64_t*>(&kPublishedMetadata.kind);
    new (&box->header.references) std::atomic<uint64_t>(1);
    box->type = type;
    const size_t size = witnesses_of(type)->size;
    box->value = calloc(1, size == 0 ? 1 : size);
    witnesses_of(type)->initialize_with_take(box->value, value, type);
    *out = box;
}

// The caller's reference stays with the instance's stored property, so this only borrows the box.
PublishedBox* box_of(void* instance, void* storage_key_path) {
    PublishedBox* box = nullptr;
    key_path_get(storage_key_path, &instance, &box, key_path_value_type(storage_key_path));
    rt_swift_release(box);
    return box;
}

SWIFTCC void published_get(SWIFT_OUT void* out, void* instance, void*, void* storage_key_path) {
    PublishedBox* box = box_of(instance, storage_key_path);
    witnesses_of(box->type)->initialize_with_copy(out, box->value, box->type);
}

SWIFTCC void published_set(void* value, void* instance, void* wrapped_key_path, void* storage_key_path) {
    PublishedBox* box = box_of(instance, storage_key_path);
    witnesses_of(box->type)->assign_with_take(box->value, value, box->type);
    if (box->publisher != nullptr) send_value(box->publisher, box->value, box->type);
    rt_registrar_mark_dirty_c();
    rt_swift_release(instance);
    rt_swift_release(wrapped_key_path);
    rt_swift_release(storage_key_path);
}

struct ModifyFrame {
    void* instance;
    void* wrapped_key_path;
    void* storage_key_path;
};

SWIFTCC void published_modify_finish(ModifyFrame* frame, bool) {
    PublishedBox* box = box_of(frame->instance, frame->storage_key_path);
    if (box->publisher != nullptr) send_value(box->publisher, box->value, box->type);
    rt_registrar_mark_dirty_c();
    rt_swift_release(frame->instance);
    rt_swift_release(frame->wrapped_key_path);
    rt_swift_release(frame->storage_key_path);
}

struct ModifyYield {
    void (*resume)(ModifyFrame*, bool) SWIFTCC;
    void* value;
};

// A yield-once coroutine: the frame buffer is the caller's, and the value is modified in place.
SWIFTCC ModifyYield published_modify(ModifyFrame* frame, void* instance, void* wrapped_key_path, void* storage_key_path) {
    *frame = {instance, wrapped_key_path, storage_key_path};
    return {published_modify_finish, box_of(instance, storage_key_path)->value};
}

// System notifications reach the app through its other observers, so this publisher never fires.
SWIFTCC void silent_publisher(SWIFT_OUT Publisher** out) { *out = new_publisher(); }

SWIFTCC void published_projected(SWIFT_OUT Publisher** out, void*, SWIFT_CONTEXT PublishedBox** self) {
    PublishedBox* box = *self;
    if (box->publisher == nullptr) {
        box->publisher = new_publisher();
        box->publisher->state->box = box;
        box->publisher->state->output_type = box->type;
    }
    *out = static_cast<Publisher*>(rt_swift_retain(box->publisher));
}

SWIFTCC void published_projected_set(void*, void*, SWIFT_CONTEXT void*) {}

// A publisher the app wrote itself: it gets a Subscriber that feeds a Sink, through the thunks below.
struct NativeSubscriber {
    HeapObject header;
    SinkRef* sink;
    uint64_t subscription[5];
    bool has_subscription;
};

void destroy_native_subscriber(void* raw) {
    delete static_cast<NativeSubscriber*>(raw)->sink;
    free(raw);
}

const HeapMetadata kNativeSubscriberMetadata = {destroy_native_subscriber, nullptr, 0};

constexpr uint64_t kDemandUnlimited = 1ULL << 63;
constexpr char kPublisherReceive[] = "_$s7Combine9PublisherP7receive10subscriberyqd___tAA10SubscriberRd__7FailureQyd__AGRtz5InputQyd__6OutputRtzlFTq";
constexpr char kSubscriptionRequest[] = "_$s7Combine12SubscriptionP7requestyyAA11SubscribersO6DemandVFTq";
constexpr char kSubscriptionCancellable[] = "_$s7Combine12SubscriptionPAA11CancellableTb";
constexpr char kCancellableCancel[] = "_$s7Combine11CancellableP6cancelyyFTq";

// The value inside an existential's buffer, which is either inline or in a box behind its first word.
void* project_existential(uint64_t* buffer, void* type) {
    const auto* witnesses = witnesses_of(type);
    if ((witnesses->flags & kFlagNonInline) == 0) return buffer;
    const size_t mask = witnesses->flags & 0xff;
    return reinterpret_cast<char*>(buffer[0]) + ((sizeof(HeapObject) + mask) & ~mask);
}

using GuestPublisherReceive = SWIFTCC void (*)(void*, void*, void*, void*, void*, SWIFT_CONTEXT void*);

Upstream upstream_of_guest(void* value, void* type, void* table) {
    const void* receive = swift_runtime_find_witness_named(static_cast<const char*>(static_cast<void**>(table)[0]), kPublisherReceive);
    if (receive == nullptr) {
        engine_log("STOP: a Combine publisher value of an unknown type");
        abort();
    }
    void* copy = copy_value(value, type);
    Publisher* publisher = operator_publisher(nullptr, [receive, copy, type, table](const SinkRef& sink) -> Cancel {
        auto* subscriber = static_cast<NativeSubscriber*>(calloc(1, sizeof(NativeSubscriber)));
        subscriber->header.metadata = const_cast<uint64_t*>(&kNativeSubscriberMetadata.kind);
        new (&subscriber->header.references) std::atomic<uint64_t>(1);
        subscriber->sink = new SinkRef(sink);
        void* word = subscriber;
        static void* placeholder[2] = {};
        reinterpret_cast<GuestPublisherReceive>(const_cast<void*>(receive))(&word, type, type_system_symbol("$syXlN"), table, placeholder, copy);
        const Hold keep = Hold(subscriber, [](void* held) { rt_swift_release(held); });
        return [keep, subscriber] {
            if (!subscriber->has_subscription) return;
            auto* subscription_type = reinterpret_cast<void*>(subscriber->subscription[3]);
            auto* subscription_table = reinterpret_cast<void**>(subscriber->subscription[4]);
            const auto* base_slot = reinterpret_cast<void* const*>(swift_runtime_find_witness_named(static_cast<const char*>(subscription_table[0]), kSubscriptionCancellable));
            if (base_slot == nullptr) return;
            auto* base = const_cast<void*>(static_cast<const void*>(base_slot));
            const void* cancel = swift_runtime_find_witness_named(static_cast<const char*>(static_cast<void**>(base)[0]), kCancellableCancel);
            if (cancel == nullptr) return;
            using CancelWitness = SWIFTCC void (*)(void*, void*, SWIFT_CONTEXT void*);
            reinterpret_cast<CancelWitness>(const_cast<void*>(cancel))(subscription_type, base, project_existential(subscriber->subscription, subscription_type));
        };
    });
    return {publisher, Hold(publisher, [](void* held) { rt_swift_release(held); })};
}

// A publisher value of type `type`, framework ones being one word.
Upstream upstream_of(void* value, void* type, void* table) {
    const auto conformance = reinterpret_cast<uintptr_t>(static_cast<void**>(table)[0]);
    if (foreign_type_name(type) != nullptr || unresolved_symbol_at(conformance) != nullptr) return upstream_of_word(*static_cast<void**>(value));
    return upstream_of_guest(value, type, table);
}

// Each Subscriber thunk takes `self` by address, and every subscriber reaching them is a NativeSubscriber.
NativeSubscriber* native_subscriber(void** self) { return static_cast<NativeSubscriber*>(*self); }

SWIFTCC void subscriber_receive_subscription(uint64_t* existential, void*, void*, SWIFT_CONTEXT void** self) {
    NativeSubscriber* subscriber = native_subscriber(self);
    auto* type = reinterpret_cast<void*>(existential[3]);
    auto* table = reinterpret_cast<void**>(existential[4]);
    memcpy(subscriber->subscription, existential, sizeof subscriber->subscription);
    witnesses_of(type)->initialize_buffer_with_copy(subscriber->subscription, existential, type);
    subscriber->has_subscription = true;
    using Request = SWIFTCC void (*)(uint64_t, void*, void*, SWIFT_CONTEXT void*);
    const void* request = swift_runtime_find_witness_named(static_cast<const char*>(table[0]), kSubscriptionRequest);
    if (request != nullptr) reinterpret_cast<Request>(const_cast<void*>(request))(kDemandUnlimited, type, table, project_existential(subscriber->subscription, type));
}

SWIFTCC uint64_t subscriber_receive_input(void* input, void*, void*, SWIFT_CONTEXT void** self) {
    (*native_subscriber(self)->sink)->value(input, nullptr);
    return 0;
}

// The Failure of an app's own publisher isn't known here, so any completion it sends counts as finishing.
SWIFTCC void subscriber_receive_completion(void*, void*, void*, SWIFT_CONTEXT void** self) { (*native_subscriber(self)->sink)->complete(nullptr, nullptr); }

SWIFTCC uint64_t demand_unlimited() { return kDemandUnlimited; }
SWIFTCC bool demand_equal(uint64_t left, uint64_t right) { return left == right; }

using ValueFunction = std::function<void(void*, void*)>;

SWIFTCC void publisher_receive_on(SWIFT_OUT void** out, void*, void*, void* type, void*, void* table, void*, SWIFT_CONTEXT void* self) {
    // Delivery stays synchronous: every queue here runs on the one guest thread.
    const Upstream upstream = upstream_of(self, type, table);
    *out = operator_publisher(upstream.publisher->state->output_type, [upstream](const SinkRef& sink) { return attach(upstream.publisher, sink); });
}

SWIFTCC void* publisher_erase_impl(void* type, void* table, void* self) {
    const Upstream upstream = upstream_of(self, type, table);
    return operator_publisher(upstream.publisher->state->output_type, [upstream](const SinkRef& sink) { return attach(upstream.publisher, sink); });
}

SWIFTCC void* publisher_erase(void* type, void* table, SWIFT_CONTEXT void* self) { return publisher_erase_impl(type, table, self); }

SWIFTCC void publisher_map(SWIFT_OUT void** out, const void* function, void* context, void* type, void* result_type, void* table, SWIFT_CONTEXT void* self) {
    const Upstream upstream = upstream_of(self, type, table);
    const Hold keep = hold(context);
    *out = operator_publisher(result_type, [upstream, function, context, keep, result_type](const SinkRef& sink) {
        return attach(upstream.publisher, value_sink(sink, [sink, function, context, result_type](void* value, void*) {
                          void* mapped = new_value(result_type);
                          rt_call_witness(function, mapped, context, value, nullptr);
                          sink->value(mapped, result_type);
                          free_value(mapped, result_type);
                      }));
    });
}

SWIFTCC void publisher_map_key_path(SWIFT_OUT void** out, void* key_path, void* type, void* table, SWIFT_CONTEXT void* self) {
    const Upstream upstream = upstream_of(self, type, table);
    const Hold keep = hold(key_path);
    void* result_type = key_path_value_type(key_path);
    *out = operator_publisher(result_type, [upstream, key_path, keep, result_type](const SinkRef& sink) {
        return attach(upstream.publisher, value_sink(sink, [sink, key_path, result_type](void* value, void*) {
                          void* read = new_value(result_type);
                          key_path_get(key_path, value, read, result_type);
                          sink->value(read, result_type);
                          free_value(read, result_type);
                      }));
    });
}

Publisher* compact_map_publisher(const Upstream& upstream, const void* function, void* context, void* result_type) {
    const Hold keep = hold(context);
    return operator_publisher(result_type, [upstream, function, context, keep, result_type](const SinkRef& sink) {
        return attach(upstream.publisher, value_sink(sink, [sink, function, context, result_type](void* value, void*) {
                          const auto* witnesses = witnesses_of(result_type);
                          // Room for the Optional's tag byte, for payloads without extra inhabitants.
                          void* mapped = calloc(1, witnesses->size + sizeof(void*));
                          rt_call_witness(function, mapped, context, value, nullptr);
                          if (witnesses->get_enum_tag_single_payload(mapped, 1, result_type) == 0) {
                              sink->value(mapped, result_type);
                              witnesses->destroy(mapped, result_type);
                          }
                          free(mapped);
                      }));
    });
}

SWIFTCC void publisher_compact_map(SWIFT_OUT void** out, const void* function, void* context, void* type, void* result_type, void* table, SWIFT_CONTEXT void* self) {
    *out = compact_map_publisher(upstream_of(self, type, table), function, context, result_type);
}

// CompactMap and Sequence have their own map methods, which behave as if chained.
SWIFTCC void compact_map_compact_map(SWIFT_OUT void** out, const void* function, void* context, void* result_type, void*, SWIFT_CONTEXT void** self) {
    *out = compact_map_publisher(upstream_of_word(*self), function, context, result_type);
}

SWIFTCC void sequence_compact_map(SWIFT_OUT void** out, const void* function, void* context, void* result_type, void*, SWIFT_CONTEXT void** self) {
    *out = compact_map_publisher(upstream_of_word(*self), function, context, result_type);
}

SWIFTCC void sequence_map(SWIFT_OUT void** out, const void* function, void* context, void* result_type, void*, SWIFT_CONTEXT void** self) {
    const Upstream upstream = upstream_of_word(*self);
    const Hold keep = hold(context);
    *out = operator_publisher(result_type, [upstream, function, context, keep, result_type](const SinkRef& sink) {
        return attach(upstream.publisher, value_sink(sink, [sink, function, context, result_type](void* value, void*) {
                          void* mapped = new_value(result_type);
                          rt_call_witness(function, mapped, context, value, nullptr);
                          sink->value(mapped, result_type);
                          free_value(mapped, result_type);
                      }));
    });
}

SWIFTCC void publisher_filter(SWIFT_OUT void** out, const void* function, void* context, void* type, void* table, SWIFT_CONTEXT void* self) {
    const Upstream upstream = upstream_of(self, type, table);
    const Hold keep = hold(context);
    *out = operator_publisher(upstream.publisher->state->output_type, [upstream, function, context, keep](const SinkRef& sink) {
        return attach(upstream.publisher, value_sink(sink, [sink, function, context](void* value, void* value_type) {
                          if ((reinterpret_cast<uintptr_t>(rt_call_witness(function, nullptr, context, value, nullptr)) & 1) != 0) sink->value(value, value_type);
                      }));
    });
}

SWIFTCC void publisher_drop_first(SWIFT_OUT void** out, int64_t count, void* type, void* table, SWIFT_CONTEXT void* self) {
    const Upstream upstream = upstream_of(self, type, table);
    *out = operator_publisher(upstream.publisher->state->output_type, [upstream, count](const SinkRef& sink) {
        auto remaining = std::make_shared<int64_t>(count);
        return attach(upstream.publisher, value_sink(sink, [sink, remaining](void* value, void* value_type) {
                          if (*remaining > 0) {
                              --*remaining;
                              return;
                          }
                          sink->value(value, value_type);
                      }));
    });
}

SWIFTCC void publisher_remove_duplicates(SWIFT_OUT void** out, void* type, void* table, void* equatable, SWIFT_CONTEXT void* self) {
    const Upstream upstream = upstream_of(self, type, table);
    *out = operator_publisher(upstream.publisher->state->output_type, [upstream, equatable](const SinkRef& sink) {
        auto last = std::make_shared<void*>(nullptr);
        return attach(upstream.publisher, value_sink(sink, [sink, equatable, last](void* value, void* value_type) {
                          require_type(value_type, "removeDuplicates");
                          using Equal = SWIFTCC bool (*)(const void*, const void*, void*, void*, SWIFT_CONTEXT void*);
                          static const auto equal = reinterpret_cast<Equal>(resolve_symbol("$sSQ2eeoiySbx_xtFZTj"));
                          if (*last != nullptr && equal(*last, value, value_type, equatable, value_type)) return;
                          if (*last != nullptr) free_value(*last, value_type);
                          *last = copy_value(value, value_type);
                          sink->value(value, value_type);
                      }));
    });
}

SWIFTCC void publisher_collect(SWIFT_OUT void** out, void* type, void* table, SWIFT_CONTEXT void* self) {
    const Upstream upstream = upstream_of(self, type, table);
    *out = operator_publisher(nullptr, [upstream](const SinkRef& sink) {
        auto items = std::make_shared<std::vector<void*>>();
        auto element = std::make_shared<void*>(upstream.publisher->state->output_type);
        return attach(upstream.publisher, sink_of(
                                              [items, element](void* value, void* value_type) {
                                                  *element = require_type(value_type, "collect");
                                                  items->push_back(copy_value(value, value_type));
                                              },
                                              [sink, items, element](void* error, void* error_type) {
                                                  if (error == nullptr) {
                                                      void* array_type = nullptr;
                                                      void* storage = resolve_symbol("_swiftEmptyArrayStorage");
                                                      if (*element != nullptr) {
                                                          void* generics[] = {*element};
                                                          array_type = resolve_mangled_type("SayxG", generics);
                                                          char* elements = nullptr;
                                                          storage = make_array_storage(*element, items->size(), &elements);
                                                          const size_t stride = witnesses_of(*element)->stride;
                                                          for (size_t i = 0; i < items->size(); ++i) witnesses_of(*element)->initialize_with_take(elements + i * stride, (*items)[i], *element);
                                                      }
                                                      sink->value(&storage, array_type);
                                                  }
                                                  sink->complete(error, error_type);
                                              }));
    });
}

SWIFTCC void publisher_prepend(SWIFT_OUT void** out, void* elements_storage, void* type, void* table, SWIFT_CONTEXT void* self) {
    const Upstream upstream = upstream_of(self, type, table);
    void* output_type = upstream.publisher->state->output_type;
    const Hold keep(rt_swift_bridgeObjectRetain(elements_storage), [](void* held) { rt_swift_bridgeObjectRelease(held); });
    *out = operator_publisher(output_type, [upstream, elements_storage, keep, output_type](const SinkRef& sink) {
        char* elements = nullptr;
        const size_t count = array_storage_contents(elements_storage, output_type, &elements);
        if (count != 0) require_type(output_type, "prepend");
        for (size_t i = 0; i < count; ++i) sink->value(elements + i * witnesses_of(output_type)->stride, output_type);
        return attach(upstream.publisher, sink);
    });
}

Publisher* merged_publisher(const std::vector<Upstream>& upstreams) {
    void* output_type = upstreams.empty() ? nullptr : upstreams[0].publisher->state->output_type;
    return operator_publisher(output_type, [upstreams](const SinkRef& sink) -> Cancel {
        auto remaining = std::make_shared<size_t>(upstreams.size());
        auto cancels = std::make_shared<std::vector<Cancel>>();
        if (upstreams.empty()) sink->complete(nullptr, nullptr);
        for (const Upstream& upstream : upstreams) {
            cancels->push_back(attach(upstream.publisher, sink_of(
                                                              [sink](void* value, void* type) { sink->value(value, type); },
                                                              [sink, remaining, cancels](void* error, void* error_type) {
                                                                  if (error != nullptr) {
                                                                      sink->complete(error, error_type);
                                                                      return;
                                                                  }
                                                                  if (--*remaining == 0) sink->complete(nullptr, nullptr);
                                                              })));
        }
        return [cancels] {
            for (const Cancel& cancel : *cancels) cancel();
        };
    });
}

SWIFTCC void merge_init(SWIFT_OUT void** out, void* first, void* second, void* first_type, void* second_type, void* first_table, void* second_table) {
    *out = merged_publisher({upstream_of(first, first_type, first_table), upstream_of(second, second_type, second_table)});
    witnesses_of(first_type)->destroy(first, first_type);
    witnesses_of(second_type)->destroy(second, second_type);
}

// Sequences reach Combine as arrays or sets, whose elements this visits in order.
void visit_sequence(void* word, void* sequence_type, const std::function<void(void*, void*)>& visit) {
    const auto storage = reinterpret_cast<uint64_t>(word) & kBridgePointerMask;
    if (void* element = array_element_type(sequence_type)) {
        char* elements = nullptr;
        const size_t count = array_storage_contents(reinterpret_cast<void*>(storage), element, &elements);
        for (size_t i = 0; i < count; ++i) visit(elements + i * witnesses_of(element)->stride, element);
        return;
    }
    void* element = type_argument(sequence_type, 0);
    visit_set_elements(reinterpret_cast<void*>(storage), [&](void* item) { visit(item, element); });
}

SWIFTCC void merge_many_init(SWIFT_OUT void** out, void* sequence, void* upstream_type, void* sequence_type, void* upstream_table, void*) {
    void* word = *static_cast<void**>(sequence);
    std::vector<Upstream> upstreams;
    visit_sequence(word, sequence_type, [&](void* item, void*) { upstreams.push_back(upstream_of(item, upstream_type, upstream_table)); });
    *out = merged_publisher(upstreams);
    witnesses_of(sequence_type)->destroy(sequence, sequence_type);
}

SWIFTCC void sequence_publisher(SWIFT_OUT void** out, void* sequence_type, void*, SWIFT_CONTEXT void** self) {
    void* word = *self;
    const Hold keep(rt_swift_bridgeObjectRetain(word), [](void* held) { rt_swift_bridgeObjectRelease(held); });
    void* output_type = array_element_type(sequence_type);
    if (output_type == nullptr) output_type = type_argument(sequence_type, 0);
    *out = operator_publisher(output_type, [word, keep, sequence_type](const SinkRef& sink) -> Cancel {
        visit_sequence(word, sequence_type, [&](void* item, void* element) { sink->value(item, element); });
        sink->complete(nullptr, nullptr);
        return [] {};
    });
}

SWIFTCC void just_init(SWIFT_OUT void** out, void* value, void* type) {
    void* copy = new_value(type);
    witnesses_of(type)->initialize_with_take(copy, value, type);
    *out = operator_publisher(type, [copy, type](const SinkRef& sink) -> Cancel {
        sink->value(copy, type);
        sink->complete(nullptr, nullptr);
        return [] {};
    });
}

SWIFTCC void publisher_flat_map(SWIFT_OUT void** out, uint64_t, const void* function, void* context, void* type, void* inner_type, void* table, void* inner_table, SWIFT_CONTEXT void* self) {
    const Upstream upstream = upstream_of(self, type, table);
    const Hold keep = hold(context);
    *out = operator_publisher(nullptr, [upstream, function, context, keep, inner_type, inner_table](const SinkRef& sink) -> Cancel {
        auto active = std::make_shared<size_t>(1);
        auto cancels = std::make_shared<std::vector<Cancel>>();
        auto finish_one = [sink, active](void* error, void* error_type) {
            if (error != nullptr) {
                sink->complete(error, error_type);
                return;
            }
            if (--*active == 0) sink->complete(nullptr, nullptr);
        };
        cancels->push_back(attach(upstream.publisher, sink_of(
                                                          [sink, function, context, inner_type, inner_table, active, cancels, finish_one](void* value, void*) {
                                                              void* inner = new_value(inner_type);
                                                              rt_call_witness(function, inner, context, value, nullptr);
                                                              const Upstream created = upstream_of(inner, inner_type, inner_table);
                                                              free_value(inner, inner_type);
                                                              ++*active;
                                                              const Hold created_keep = created.keep;
                                                              cancels->push_back(attach(created.publisher, sink_of(
                                                                                                               [sink, created_keep](void* item, void* item_type) { sink->value(item, item_type); },
                                                                                                               finish_one)));
                                                          },
                                                          finish_one)));
        return [cancels] {
            for (const Cancel& cancel : *cancels) cancel();
        };
    });
}

SWIFTCC void publisher_catch(SWIFT_OUT void** out, const void* function, void* context, void* type, void* inner_type, void* table, void* inner_table, SWIFT_CONTEXT void* self) {
    const Upstream upstream = upstream_of(self, type, table);
    const Hold keep = hold(context);
    *out = operator_publisher(upstream.publisher->state->output_type, [upstream, function, context, keep, inner_type, inner_table](const SinkRef& sink) -> Cancel {
        auto replacement = std::make_shared<Cancel>();
        Cancel first = attach(upstream.publisher, sink_of(
                                                      [sink](void* value, void* value_type) { sink->value(value, value_type); },
                                                      [sink, function, context, inner_type, inner_table, replacement](void* error, void*) {
                                                          if (error == nullptr) {
                                                              sink->complete(nullptr, nullptr);
                                                              return;
                                                          }
                                                          void* inner = new_value(inner_type);
                                                          rt_call_witness(function, inner, context, error, nullptr);
                                                          const Upstream created = upstream_of(inner, inner_type, inner_table);
                                                          free_value(inner, inner_type);
                                                          const Hold created_keep = created.keep;
                                                          *replacement = attach(created.publisher, sink_of(
                                                                                                       [sink, created_keep](void* value, void* value_type) { sink->value(value, value_type); },
                                                                                                       [sink](void* inner_error, void* inner_error_type) { sink->complete(inner_error, inner_error_type); }));
                                                      }));
        return [first, replacement] {
            first();
            if (*replacement) (*replacement)();
        };
    });
}

// Only the output, completion and cancel events exist here: there is no demand to observe.
SWIFTCC void publisher_handle_events(SWIFT_OUT void** out, const void*, void*, const void* output, void* output_context, const void* completion, void* completion_context, const void* cancel, void* cancel_context, const void*, void*, void* type, void* table, SWIFT_CONTEXT void* self) {
    const Upstream upstream = upstream_of(self, type, table);
    const auto keep = std::make_shared<std::vector<Hold>>(std::vector<Hold>{hold(output_context), hold(completion_context), hold(cancel_context)});
    void* failure = failure_type_of(type);
    *out = operator_publisher(upstream.publisher->state->output_type, [upstream, output, output_context, completion, completion_context, cancel, cancel_context, keep, failure](const SinkRef& sink) -> Cancel {
        Cancel inner = attach(upstream.publisher, sink_of(
                                                      [sink, output, output_context](void* value, void* value_type) {
                                                          if (output != nullptr) rt_call_witness(output, nullptr, output_context, value, nullptr);
                                                          sink->value(value, value_type);
                                                      },
                                                      [sink, completion, completion_context, failure](void* error, void* error_type) {
                                                          if (completion != nullptr) call_completion(completion, completion_context, failure, error);
                                                          sink->complete(error, error_type);
                                                      }));
        return [inner, cancel, cancel_context] {
            if (cancel != nullptr) rt_call_witness(cancel, nullptr, cancel_context, nullptr, nullptr);
            inner();
        };
    });
}

SWIFTCC void* publisher_share(void* type, void* table, SWIFT_CONTEXT void* self) {
    const Upstream upstream = upstream_of(self, type, table);
    Publisher* shared = new_publisher();
    shared->state->output_type = upstream.publisher->state->output_type;
    auto connection = std::make_shared<Cancel>();
    // Connects to the upstream for the first subscriber, and drops it when the last one leaves.
    shared->state->subscribe = [shared, upstream, connection](const SinkRef& sink) -> Cancel {
        static uint64_t next_id = 1;
        const uint64_t id = next_id++;
        PublisherState* state = shared->state;
        if (state->finished) {
            sink->complete(state->error, state->error_type);
            return [] {};
        }
        state->sinks.emplace_back(id, sink);
        if (state->sinks.size() == 1) {
            *connection = attach(upstream.publisher, sink_of(
                                                         [shared](void* value, void* value_type) { send_value(shared, value, value_type); },
                                                         [shared](void* error, void* error_type) { send_completion(shared, error, error_type); }));
        }
        return [shared, id, connection] {
            auto& sinks = shared->state->sinks;
            sinks.erase(std::remove_if(sinks.begin(), sinks.end(), [id](const auto& entry) { return entry.first == id; }), sinks.end());
            if (sinks.empty() && *connection) {
                const Cancel disconnect = *connection;
                *connection = nullptr;
                disconnect();
            }
        };
    };
    return shared;
}

struct DebounceTick {
    SinkRef sink;
    std::shared_ptr<uint64_t> generation;
    uint64_t expected;
    void* value;
    void* type;
};

void fire_debounce(void* raw) {
    std::unique_ptr<DebounceTick> tick(static_cast<DebounceTick*>(raw));
    if (*tick->generation == tick->expected) tick->sink->value(tick->value, tick->type);
    free_value(tick->value, tick->type);
}

// A dispatch stride is nanoseconds and a run loop's is seconds, each in one word.
int64_t stride_nanoseconds(const void* stride, void* scheduler_type) {
    constexpr double kNanosecondsPerSecond = 1e9;
    uint64_t word;
    memcpy(&word, stride, sizeof word);
    if (objc_framework_class_name(scheduler_type) == "OS_dispatch_queue") return static_cast<int64_t>(word);
    double seconds;
    memcpy(&seconds, &word, sizeof seconds);
    return static_cast<int64_t>(seconds * kNanosecondsPerSecond);
}

SWIFTCC void publisher_debounce(SWIFT_OUT void** out, const void* stride, void*, void*, void* type, void* scheduler_type, void* table, void*, SWIFT_CONTEXT void* self) {
    const Upstream upstream = upstream_of(self, type, table);
    const int64_t delay = stride_nanoseconds(stride, scheduler_type);
    *out = operator_publisher(upstream.publisher->state->output_type, [upstream, delay](const SinkRef& sink) {
        auto generation = std::make_shared<uint64_t>(0);
        return attach(upstream.publisher, sink_of(
                                              [sink, generation, delay](void* value, void* value_type) {
                                                  require_type(value_type, "debounce");
                                                  const uint64_t expected = ++*generation;
                                                  dispatch_enqueue_after(delay, fire_debounce, new DebounceTick{sink, generation, expected, copy_value(value, value_type), value_type});
                                              },
                                              [sink, generation](void* error, void* error_type) {
                                                  ++*generation;
                                                  sink->complete(error, error_type);
                                              }));
    });
}

// Assigning into a Published keeps the upstream connected for as long as the app runs.
SWIFTCC void publisher_assign_to(void** published_publisher, void* type, void* table, SWIFT_CONTEXT void* self) {
    const Upstream upstream = upstream_of(self, type, table);
    Publisher* target = publisher_of(*published_publisher);
    auto* connection = new Cancel();
    *connection = attach(upstream.publisher, sink_of(
                                                 [target](void* value, void*) {
                                                     PublishedBox* box = target->state->box;
                                                     if (box == nullptr) return;
                                                     witnesses_of(box->type)->assign_with_copy(box->value, value, box->type);
                                                     send_value(target, box->value, box->type);
                                                 },
                                                 [](void*, void*) {}));
}

// A Result's payloads share one buffer, followed by a byte saying which one it holds.
void future_settle(void* future, const void* result) {
    Publisher* publisher = publisher_of(future);
    if (publisher->state->finished) return;
    const void* isa = *static_cast<void**>(future);
    void* output = type_argument(isa, 0);
    void* failure = type_argument(isa, 1);
    const size_t largest = std::max(witnesses_of(output)->size, witnesses_of(failure)->size);
    const bool succeeded = static_cast<const uint8_t*>(result)[largest] == 0;
    if (succeeded) {
        void* value = copy_value(result, output);
        publisher->state->output_type = output;
        future_results()[future] = value;
        send_value(publisher, value, output);
        send_completion(publisher, nullptr, nullptr);
    } else {
        send_completion(publisher, copy_value(result, failure), failure);
    }
}

struct PromiseContext {
    HeapObject header;
    void* future;
};

void destroy_promise(void* raw) {
    rt_swift_release(static_cast<PromiseContext*>(raw)->future);
    free(raw);
}

const HeapMetadata kPromiseMetadata = {destroy_promise, nullptr, 0};

SWIFTCC void promise_call(const void* result, SWIFT_CONTEXT PromiseContext* context) { future_settle(context->future, result); }

SWIFTCC void* future_init(const void* attempt, void* attempt_context, SWIFT_CONTEXT void* self) {
    Publisher* publisher = publisher_of(self);
    publisher->state->output_type = type_argument(*static_cast<void**>(self), 0);
    // A future delivers its result to anyone who subscribes, however late.
    publisher->state->subscribe = [publisher, self](const SinkRef& sink) -> Cancel {
        PublisherState* state = publisher->state;
        if (state->finished) {
            const auto found = future_results().find(self);
            if (found != future_results().end()) sink->value(found->second, state->output_type);
            sink->complete(state->error, state->error_type);
            return [] {};
        }
        static uint64_t next_id = 1;
        const uint64_t id = next_id++;
        state->sinks.emplace_back(id, sink);
        return [publisher, id] {
            auto& sinks = publisher->state->sinks;
            sinks.erase(std::remove_if(sinks.begin(), sinks.end(), [id](const auto& entry) { return entry.first == id; }), sinks.end());
        };
    };
    auto* promise = static_cast<PromiseContext*>(calloc(1, sizeof(PromiseContext)));
    promise->header.metadata = const_cast<uint64_t*>(&kPromiseMetadata.kind);
    new (&promise->header.references) std::atomic<uint64_t>(1);
    promise->future = rt_swift_retain(self);
    rt_call_witness(attempt, nullptr, attempt_context, reinterpret_cast<void*>(promise_call), promise);
    rt_swift_release(promise);
    return self;
}

// Hands a closure a Subscribers.Completion, which is laid out as an Optional of its Failure.
void call_completion(const void* function, void* context, void* failure, void* error) {
    const auto* witnesses = witnesses_of(failure);
    void* completion = calloc(1, witnesses->size + sizeof(void*));
    if (error != nullptr) {
        witnesses->initialize_with_copy(completion, error, failure);
        witnesses->store_enum_tag_single_payload(completion, 0, 1, failure);
    } else {
        witnesses->store_enum_tag_single_payload(completion, 1, 1, failure);
    }
    rt_call_witness(function, nullptr, context, completion, nullptr);
    if (error != nullptr) witnesses->destroy(completion, failure);
    free(completion);
}

struct CancellableObject {
    HeapObject header;
    Cancel* cancel;
};

void destroy_cancellable(void* raw) {
    auto* cancellable = static_cast<CancellableObject*>(raw);
    if (cancellable->cancel != nullptr) {
        const Cancel cancel = *cancellable->cancel;
        delete cancellable->cancel;
        cancellable->cancel = nullptr;
        if (cancel) cancel();
    }
    free(raw);
}

void* cancellable_class_metadata() {
    static void* block[12] = {};
    return sized_class_metadata(block, reinterpret_cast<void*>(destroy_cancellable), sizeof(CancellableObject));
}

struct MetadataResponse {
    void* metadata;
    size_t state;
};

SWIFTCC MetadataResponse cancellable_access(size_t) { return {cancellable_class_metadata(), 0}; }

void* new_cancellable(Cancel cancel) {
    auto* cancellable = static_cast<CancellableObject*>(calloc(1, sizeof(CancellableObject)));
    cancellable->header.metadata = static_cast<uint64_t*>(cancellable_class_metadata());
    new (&cancellable->header.references) std::atomic<uint64_t>(1);
    cancellable->cancel = new Cancel(std::move(cancel));
    return cancellable;
}

SWIFTCC void* publisher_sink_value(const void* receive, void* context, void* type, void* table, SWIFT_CONTEXT void* self) {
    const Upstream upstream = upstream_of(self, type, table);
    const Hold keep = hold(context);
    const Cancel cancel = attach(upstream.publisher, sink_of(
                                                         [receive, context, keep, upstream](void* value, void*) {
                                                             rt_call_witness(receive, nullptr, context, value, nullptr);
                                                             rt_registrar_mark_dirty_c();
                                                         },
                                                         [](void*, void*) {}));
    return new_cancellable(cancel);
}

SWIFTCC void* publisher_sink_completion(const void* completion, void* completion_context, const void* receive, void* context, void* type, void* table, SWIFT_CONTEXT void* self) {
    const Upstream upstream = upstream_of(self, type, table);
    const auto keep = std::make_shared<std::vector<Hold>>(std::vector<Hold>{hold(completion_context), hold(context)});
    void* failure = failure_type_of(type);
    const Cancel cancel = attach(upstream.publisher, sink_of(
                                                         [receive, context, keep, upstream](void* value, void*) {
                                                             rt_call_witness(receive, nullptr, context, value, nullptr);
                                                             rt_registrar_mark_dirty_c();
                                                         },
                                                         [completion, completion_context, failure, keep](void* error, void*) {
                                                             call_completion(completion, completion_context, failure, error);
                                                             rt_registrar_mark_dirty_c();
                                                         }));
    return new_cancellable(cancel);
}

// `AnyCancellable { ... }` is allocated by the app, from the metadata above.
SWIFTCC void* cancellable_init(const void* function, void* context, SWIFT_CONTEXT void* self) {
    const Hold keep = hold(context);
    static_cast<CancellableObject*>(self)->cancel = new Cancel([function, context, keep] { rt_call_witness(function, nullptr, context, nullptr, nullptr); });
    return self;
}

SWIFTCC void cancellable_cancel(SWIFT_CONTEXT void* self) {
    auto* cancellable = static_cast<CancellableObject*>(self);
    if (cancellable->cancel == nullptr || !*cancellable->cancel) return;
    const Cancel cancel = *cancellable->cancel;
    *cancellable->cancel = nullptr;
    cancel();
}

// Nothing hashes these here, so the set itself isn't used: storing only keeps the subscription alive.
SWIFTCC void cancellable_store(void*, SWIFT_CONTEXT void* self) { rt_swift_retain(self); }

SWIFTCC void* subject_init(SWIFT_CONTEXT void* self) { return self; }
SWIFTCC void subject_send(void* value, SWIFT_CONTEXT void* self) { send_value(publisher_of(self), value, type_argument(*static_cast<void**>(self), 0)); }

SWIFTCC void subject_complete(void* completion, SWIFT_CONTEXT void* self) {
    void* failure = type_argument(*static_cast<void**>(self), 1);
    const bool failed = witnesses_of(failure)->get_enum_tag_single_payload(completion, 1, failure) == 0;
    send_completion(publisher_of(self), failed ? copy_value(completion, failure) : nullptr, failed ? failure : nullptr);
}

SWIFTCC void combine_latest_init(SWIFT_OUT void** out, void* first, void* second, void* first_type, void* second_type, void* first_table, void* second_table) {
    const Upstream left = upstream_of(first, first_type, first_table);
    const Upstream right = upstream_of(second, second_type, second_table);
    witnesses_of(first_type)->destroy(first, first_type);
    witnesses_of(second_type)->destroy(second, second_type);
    *out = operator_publisher(nullptr, [left, right](const SinkRef& sink) -> Cancel {
        struct Latest {
            void* value[2] = {nullptr, nullptr};
            void* type[2] = {nullptr, nullptr};
            size_t remaining = 2;
        };
        auto latest = std::make_shared<Latest>();
        const auto store = [sink, latest](size_t side, void* value, void* type) {
            require_type(type, "combineLatest");
            if (latest->value[side] != nullptr) free_value(latest->value[side], type);
            latest->value[side] = copy_value(value, type);
            latest->type[side] = type;
            if (latest->value[0] == nullptr || latest->value[1] == nullptr) return;
            const auto* a = witnesses_of(latest->type[0]);
            const auto* b = witnesses_of(latest->type[1]);
            const size_t mask = b->flags & 0xff;
            const size_t offset = (a->size + mask) & ~mask;
            void* tuple = calloc(1, offset + b->size + 1);
            a->initialize_with_copy(tuple, latest->value[0], latest->type[0]);
            b->initialize_with_copy(static_cast<char*>(tuple) + offset, latest->value[1], latest->type[1]);
            sink->value(tuple, nullptr);
            a->destroy(tuple, latest->type[0]);
            b->destroy(static_cast<char*>(tuple) + offset, latest->type[1]);
            free(tuple);
        };
        const auto finish = [sink, latest](void* error, void* error_type) {
            if (error != nullptr) {
                sink->complete(error, error_type);
                return;
            }
            if (--latest->remaining == 0) sink->complete(nullptr, nullptr);
        };
        Cancel first_cancel = attach(left.publisher, sink_of([store](void* value, void* type) { store(0, value, type); }, finish));
        Cancel second_cancel = attach(right.publisher, sink_of([store](void* value, void* type) { store(1, value, type); }, finish));
        return [first_cancel, second_cancel] {
            first_cancel();
            second_cancel();
        };
    });
}

SWIFTCC void publisher_deferred(SWIFT_OUT void** out, const void* create, void* context, void* type, void* table) {
    const Hold keep = hold(context);
    *out = operator_publisher(nullptr, [create, context, keep, type, table](const SinkRef& sink) {
        void* value = new_value(type);
        rt_call_witness(create, value, context, nullptr, nullptr);
        const Upstream created = upstream_of(value, type, table);
        free_value(value, type);
        const Hold created_keep = created.keep;
        const Cancel cancel = attach(created.publisher, sink);
        return Cancel([cancel, created_keep] { cancel(); });
    });
}

SWIFTCC void* object_will_change(void*, void*, SWIFT_CONTEXT void*) {
    static Publisher* publisher = new_publisher();
    return rt_swift_retain(publisher);
}

SWIFTCC void object_will_change_send(SWIFT_CONTEXT void* self) {
    uint8_t nothing = 0;
    send_value(publisher_of(self), &nothing, type_system_symbol("$sytN"));
    rt_registrar_mark_dirty_c();
}

SWIFTCC void run_loop_stride_from_float(SWIFT_OUT double* out, double seconds) { *out = seconds; }

}  // namespace

void* combine_symbol(std::string_view name) {
    static const std::unordered_map<std::string_view, void*> table = {
        {"$s7Combine9PublisherPAAE3mapyAA10PublishersO3MapVy_xqd__Gqd__6OutputQzclF", reinterpret_cast<void*>(publisher_map)},
        {"$s7Combine9PublisherPAAE3mapyAA10PublishersO10MapKeyPathVy_xqd__Gs0fG0Cy6OutputQzqd__GlF", reinterpret_cast<void*>(publisher_map_key_path)},
        {"$s7Combine9PublisherPAAE10compactMapyAA10PublishersO07CompactD0Vy_xqd__Gqd__Sg6OutputQzclF", reinterpret_cast<void*>(publisher_compact_map)},
        {"$s7Combine10PublishersO10CompactMapV07compactD0yAEy_xqd__Gqd__Sgq_clF", reinterpret_cast<void*>(compact_map_compact_map)},
        {"$s7Combine10PublishersO8SequenceV3mapyAEy_Sayqd__Gq_Gqd__7ElementQzXElF", reinterpret_cast<void*>(sequence_map)},
        {"$s7Combine10PublishersO8SequenceV10compactMapyAEy_Sayqd__Gq_Gqd__Sg7ElementQzXElF", reinterpret_cast<void*>(sequence_compact_map)},
        {"$sST7CombineE9publisherAA10PublishersO8SequenceVy_xs5NeverOGvg", reinterpret_cast<void*>(sequence_publisher)},
        {"$s7Combine9PublisherPAAE6filteryAA10PublishersO6FilterVy_xGSb6OutputQzcF", reinterpret_cast<void*>(publisher_filter)},
        {"$s7Combine9PublisherPAAE9dropFirstyAA10PublishersO4DropVy_xGSiF", reinterpret_cast<void*>(publisher_drop_first)},
        {"$s7Combine9PublisherPAASQ6OutputRpzrlE16removeDuplicatesAA10PublishersO06RemoveE0Vy_xGyF", reinterpret_cast<void*>(publisher_remove_duplicates)},
        {"$s7Combine9PublisherPAAE7collectAA10PublishersO7CollectVy_xGyF", reinterpret_cast<void*>(publisher_collect)},
        {"$s7Combine9PublisherPAAE7prependyAA10PublishersO11ConcatenateVy_AF8SequenceVy_Say6OutputQzG7FailureQzGxGALd_tF", reinterpret_cast<void*>(publisher_prepend)},
        {"$s7Combine10PublishersO5MergeVyAEy_xq_Gx_q_tcfC", reinterpret_cast<void*>(merge_init)},
        {"$s7Combine10PublishersO9MergeManyVyAEy_xGqd__c7ElementQyd__RszSTRd__lufC", reinterpret_cast<void*>(merge_many_init)},
        {"$s7Combine4JustVyACyxGxcfC", reinterpret_cast<void*>(just_init)},
        {"$s7Combine6FutureCyACyxq_Gyys6ResultOyxq_Gcccfc", reinterpret_cast<void*>(future_init)},
        {"$s7Combine9PublisherPAAs5NeverO7FailureRtzrlE7flatMap13maxPublishers_AA0H0O04FlatF0Vy_qd__xGAA11SubscribersO6DemandV_qd__6OutputQzctAaBRd__AeFRtd__lF", reinterpret_cast<void*>(publisher_flat_map)},
        {"$s7Combine9PublisherPAAE5catchyAA10PublishersO5CatchVy_xqd__Gqd__7FailureQzcAaBRd__6OutputQyd__ALRtzlF", reinterpret_cast<void*>(publisher_catch)},
        {"$s7Combine9PublisherPAAE5shareAA10PublishersO5ShareCy_xGyF", reinterpret_cast<void*>(publisher_share)},
        {"$s7Combine9PublisherPAAs5NeverO7FailureRtzrlE6assign2toyAA9PublishedVABVy6OutputQz_Gz_tF", reinterpret_cast<void*>(publisher_assign_to)},
        {"$s7Combine14AnyCancellableCMa", reinterpret_cast<void*>(cancellable_access)},
        {"$s7Combine14AnyCancellableC5store2inyShyACGz_tF", reinterpret_cast<void*>(cancellable_store)},
        {"$s7Combine10SubscriberP7receive12subscriptionyAA12Subscription_p_tFTj", reinterpret_cast<void*>(subscriber_receive_subscription)},
        {"$s7Combine10SubscriberP7receiveyAA11SubscribersO6DemandV5InputQzFTj", reinterpret_cast<void*>(subscriber_receive_input)},
        {"$s7Combine10SubscriberP7receive10completionyAA11SubscribersO10CompletionOy_7FailureQzG_tFTj", reinterpret_cast<void*>(subscriber_receive_completion)},
        {"$s7Combine11SubscribersO6DemandV9unlimitedAEvgZ", reinterpret_cast<void*>(demand_unlimited)},
        {"$s7Combine11SubscribersO6DemandV2eeoiySbAE_AEtFZ", reinterpret_cast<void*>(demand_equal)},
        {"$sSo9NSRunLoopC10FoundationE17SchedulerTimeTypeV6StrideV12floatLiteralAGSd_tcfC", reinterpret_cast<void*>(run_loop_stride_from_float)},
        {"$s7Combine9PublisherPAAE12handleEvents19receiveSubscription0E6Output0E10Completion0E6Cancel0E7RequestAA10PublishersO06HandleD0Vy_xGyAA0F0_pcSg_y0G0QzcSgyAA11SubscribersO0H0Oy_7FailureQzGcSgyycSgyAU6DemandVcSgtF", reinterpret_cast<void*>(publisher_handle_events)},
        {"$s7Combine9PublisherPAAE8debounce3for9scheduler7optionsAA10PublishersO8DebounceVy_xqd__G17SchedulerTimeType_6StrideQYd___qd__0I7OptionsQyd__SgtAA0I0Rd__lF", reinterpret_cast<void*>(publisher_debounce)},
        {"$sSo20NSNotificationCenterC10FoundationE9publisher3for6objectAbCE9PublisherVSo0A4Namea_yXlSgtF", reinterpret_cast<void*>(silent_publisher)},
        {"$s7Combine9PublishedV14projectedValueAC9PublisherVyx_Gvg", reinterpret_cast<void*>(published_projected)},
        {"$s7Combine9PublishedV14projectedValueAC9PublisherVyx_Gvs", reinterpret_cast<void*>(published_projected_set)},
        {"$s7Combine9PublisherPAAE7receive2on7optionsAA10PublishersO9ReceiveOnVy_xqd__Gqd___16SchedulerOptionsQyd__SgtAA0I0Rd__lF", reinterpret_cast<void*>(publisher_receive_on)},
        {"$s7Combine9PublisherPAAE010eraseToAnyB0AA0eB0Vy6OutputQz7FailureQzGyF", reinterpret_cast<void*>(publisher_erase)},
        {"$s7Combine9PublisherPAAs5NeverO7FailureRtzrlE4sink12receiveValueAA14AnyCancellableCy6OutputQzc_tF", reinterpret_cast<void*>(publisher_sink_value)},
        {"$s7Combine9PublisherPAAE4sink17receiveCompletion0D5ValueAA14AnyCancellableCyAA11SubscribersO0E0Oy_7FailureQzGc_y6OutputQzctF", reinterpret_cast<void*>(publisher_sink_completion)},
        {"$s7Combine8DeferredV15createPublisherACyxGxyc_tcfC", reinterpret_cast<void*>(publisher_deferred)},
        {"$s7Combine14AnyCancellableCyACyyccfc", reinterpret_cast<void*>(cancellable_init)},
        {"$s7Combine14AnyCancellableC6cancelyyF", reinterpret_cast<void*>(cancellable_cancel)},
        {"$s7Combine18PassthroughSubjectCACyxq_Gycfc", reinterpret_cast<void*>(subject_init)},
        {"$s7Combine18PassthroughSubjectC4sendyyxF", reinterpret_cast<void*>(subject_send)},
        {"$s7Combine18PassthroughSubjectC4send10completionyAA11SubscribersO10CompletionOy_q_G_tF", reinterpret_cast<void*>(subject_complete)},
        {"$s7Combine10PublishersO0A6LatestVyAEy_xq_Gx_q_tcfC", reinterpret_cast<void*>(combine_latest_init)},
        {"$s7Combine16ObservableObjectPA2A0bC9PublisherC0c10WillChangeD0RtzrlE06objecteF0AEvg", reinterpret_cast<void*>(object_will_change)},
        {"$s7Combine25ObservableObjectPublisherC4sendyyF", reinterpret_cast<void*>(object_will_change_send)},
        {"$s7Combine9PublishedV12initialValueACyxGx_tcfC", reinterpret_cast<void*>(published_init)},
        {"$s7Combine9PublishedV18_enclosingInstance7wrapped7storagexqd___s24ReferenceWritableKeyPathCyqd__xGAHyqd__ACyxGGtcRld__CluigZ", reinterpret_cast<void*>(published_get)},
        {"$s7Combine9PublishedV18_enclosingInstance7wrapped7storagexqd___s24ReferenceWritableKeyPathCyqd__xGAHyqd__ACyxGGtcRld__CluisZ", reinterpret_cast<void*>(published_set)},
        {"$s7Combine9PublishedV18_enclosingInstance7wrapped7storagexqd___s24ReferenceWritableKeyPathCyqd__xGAHyqd__ACyxGGtcRld__CluiMZ", reinterpret_cast<void*>(published_modify)},
    };
    const auto found = table.find(name);
    return found == table.end() ? nullptr : found->second;
}
