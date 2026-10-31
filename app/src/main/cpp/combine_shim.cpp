#include "combine_shim.h"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <new>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "key_path.h"
#include "swift_abi.h"


extern "C" void rt_registrar_mark_dirty_c();

namespace {

struct Subscriber {
    uint64_t id;
    const void* receive;
    void* context;
};

// Everything a publisher chain does here is forward values to its sinks, so each chain shares one of these.
struct Publisher {
    HeapObject header;
    std::vector<Subscriber>* subscribers;
    const void* create;  // Set for Deferred: builds the upstream publisher on first subscription.
    void* create_context;
    void* create_type;
};

void destroy_publisher(void* raw) {
    delete static_cast<Publisher*>(raw)->subscribers;
    free(raw);
}

const HeapMetadata kPublisherMetadata = {destroy_publisher, nullptr, 0};

std::unordered_set<const void*>& live_publishers() {
    static std::unordered_set<const void*> publishers;
    return publishers;
}

Publisher* new_publisher() {
    auto* publisher = static_cast<Publisher*>(calloc(1, sizeof(Publisher)));
    publisher->header.metadata = const_cast<uint64_t*>(&kPublisherMetadata.kind);
    new (&publisher->header.references) std::atomic<uint64_t>(1);
    publisher->subscribers = new std::vector<Subscriber>();
    live_publishers().insert(publisher);
    return publisher;
}

// A subject is an object the app allocated, so its publisher hangs off a side table.
Publisher* publisher_of(void* word) {
    if (live_publishers().count(word) != 0) return static_cast<Publisher*>(word);
    static std::unordered_map<void*, Publisher*> subjects;
    Publisher*& publisher = subjects[word];
    if (publisher == nullptr) publisher = new_publisher();
    return publisher;
}

void publish(Publisher* publisher, void* value) {
    const std::vector<Subscriber> current = *publisher->subscribers;
    for (const Subscriber& subscriber : current) rt_call_witness(subscriber.receive, nullptr, subscriber.context, value, nullptr);
    rt_registrar_mark_dirty_c();
}

// Published<T> is one reference to this box, which stands in for the framework's storage.
struct PublishedBox {
    HeapObject header;
    void* type;
    void* value;
    Publisher* publisher;
};

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
    if (box->publisher != nullptr) publish(box->publisher, box->value);
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
    if (box->publisher != nullptr) publish(box->publisher, box->value);
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
    if (box->publisher == nullptr) box->publisher = new_publisher();
    *out = static_cast<Publisher*>(rt_swift_retain(box->publisher));
}

SWIFTCC void published_projected_set(void*, void*, SWIFT_CONTEXT void*) {}

// Self is any publisher here: all of them are one word that identifies the chain.
SWIFTCC void publisher_receive_on(SWIFT_OUT Publisher** out, void*, void*, void*, void*, void*, void*, SWIFT_CONTEXT void** self) {
    Publisher* publisher = publisher_of(*self);
    *out = static_cast<Publisher*>(rt_swift_retain(publisher));
}

SWIFTCC Publisher* publisher_erase(void*, void*, SWIFT_CONTEXT void** self) { return static_cast<Publisher*>(rt_swift_retain(publisher_of(*self))); }

SWIFTCC void publisher_deferred(SWIFT_OUT Publisher** out, const void* create, void* context, void* type, void*) {
    Publisher* publisher = new_publisher();
    publisher->create = create;
    publisher->create_context = context;
    publisher->create_type = type;
    *out = publisher;
}

struct Cancellable {
    HeapObject header;
    Publisher* publisher;
    uint64_t id;
};

void destroy_cancellable(void* raw) { free(raw); }
const HeapMetadata kCancellableMetadata = {destroy_cancellable, nullptr, 0};

std::unordered_set<const void*>& live_cancellables() {
    static std::unordered_set<const void*> cancellables;
    return cancellables;
}

void* subscribe(void* word, const void* receive, void* context) {
    Publisher* publisher = publisher_of(word);
    if (publisher->create != nullptr) {
        void* upstream = nullptr;
        rt_call_witness(publisher->create, &upstream, publisher->create_context, nullptr, nullptr);
        publisher = publisher_of(upstream);
    }
    static uint64_t next_id = 1;
    const uint64_t id = next_id++;
    publisher->subscribers->push_back({id, receive, rt_swift_retain(context)});
    auto* cancellable = static_cast<Cancellable*>(calloc(1, sizeof(Cancellable)));
    cancellable->header.metadata = const_cast<uint64_t*>(&kCancellableMetadata.kind);
    new (&cancellable->header.references) std::atomic<uint64_t>(1);
    cancellable->publisher = publisher;
    cancellable->id = id;
    live_cancellables().insert(cancellable);
    return cancellable;
}

SWIFTCC void* publisher_sink_value(const void* receive, void* context, void*, void*, SWIFT_CONTEXT void** self) { return subscribe(*self, receive, context); }
SWIFTCC void* publisher_sink_completion(const void*, void*, const void* receive, void* context, void*, void*, SWIFT_CONTEXT void** self) { return subscribe(*self, receive, context); }

// `AnyCancellable { ... }` allocates its own object, so its closure hangs off a side table.
struct CancelClosure {
    const void* function;
    void* context;
};

std::unordered_map<void*, CancelClosure>& cancel_closures() {
    static std::unordered_map<void*, CancelClosure> closures;
    return closures;
}

SWIFTCC void* cancellable_init(const void* function, void* context, SWIFT_CONTEXT void* self) {
    cancel_closures()[self] = {function, context};
    return self;
}

SWIFTCC void cancellable_cancel(SWIFT_CONTEXT void* self) {
    if (live_cancellables().count(self) != 0) {
        auto* cancellable = static_cast<Cancellable*>(self);
        auto& subscribers = *cancellable->publisher->subscribers;
        subscribers.erase(std::remove_if(subscribers.begin(), subscribers.end(), [cancellable](const Subscriber& s) { return s.id == cancellable->id; }), subscribers.end());
        return;
    }
    const auto found = cancel_closures().find(self);
    if (found == cancel_closures().end()) return;
    const CancelClosure closure = found->second;
    cancel_closures().erase(found);
    rt_call_witness(closure.function, nullptr, closure.context, nullptr, nullptr);
}

SWIFTCC void* subject_init(SWIFT_CONTEXT void* self) { return self; }
SWIFTCC void subject_send(void* value, SWIFT_CONTEXT void* self) { publish(publisher_of(self), value); }
SWIFTCC void subject_complete(void*, SWIFT_CONTEXT void*) {}

SWIFTCC void combine_latest_init(SWIFT_OUT Publisher** out) { *out = new_publisher(); }

SWIFTCC void* object_will_change(void*, void*, SWIFT_CONTEXT void*) {
    static Publisher* publisher = new_publisher();
    return rt_swift_retain(publisher);
}

SWIFTCC void object_will_change_send(SWIFT_CONTEXT void*) { rt_registrar_mark_dirty_c(); }

}  // namespace

void* combine_symbol(std::string_view name) {
    static const std::unordered_map<std::string_view, void*> table = {
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
