#include "combine_shim.h"

#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <new>
#include <string_view>
#include <unordered_map>

#include "key_path.h"

#define SWIFTCC __attribute__((swiftcall))
#define SWIFT_OUT __attribute__((swift_indirect_result))

extern "C" void* rt_swift_retain(void*);
extern "C" void rt_swift_release(void*);
extern "C" void rt_registrar_mark_dirty_c();

namespace {

struct ValueWitnesses {
    void* initialize_buffer_with_copy;
    void (*destroy)(void*, void*);
    void* (*initialize_with_copy)(void*, void*, void*);
    void* (*assign_with_copy)(void*, void*, void*);
    void* (*initialize_with_take)(void*, void*, void*);
    void* (*assign_with_take)(void*, void*, void*);
    void* get_enum_tag;
    void* store_enum_tag;
    size_t size;
};

const ValueWitnesses* witnesses_of(const void* metadata) {
    return static_cast<const ValueWitnesses*>(static_cast<void* const*>(metadata)[-1]);
}

struct HeapHeader {
    void* metadata;
    std::atomic<uint64_t> references;
};

struct HeapMetadata {
    void (*destroyer)(void*);
    const void* witnesses;
    uint64_t kind;
};

// Published<T> is one reference to this box, which stands in for the framework's storage.
struct PublishedBox {
    HeapHeader header;
    void* type;
    void* value;
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

// A publisher nothing subscribes to yet: `.onReceive` keeps the value but never starts a subscription.
SWIFTCC void silent_publisher(SWIFT_OUT uint64_t* out) { *out = 1ULL << 63; }

}  // namespace

void* combine_symbol(std::string_view name) {
    static const std::unordered_map<std::string_view, void*> table = {
        {"$sSo20NSNotificationCenterC10FoundationE9publisher3for6objectAbCE9PublisherVSo0A4Namea_yXlSgtF", reinterpret_cast<void*>(silent_publisher)},
        {"$s7Combine9PublishedV12initialValueACyxGx_tcfC", reinterpret_cast<void*>(published_init)},
        {"$s7Combine9PublishedV18_enclosingInstance7wrapped7storagexqd___s24ReferenceWritableKeyPathCyqd__xGAHyqd__ACyxGGtcRld__CluigZ", reinterpret_cast<void*>(published_get)},
        {"$s7Combine9PublishedV18_enclosingInstance7wrapped7storagexqd___s24ReferenceWritableKeyPathCyqd__xGAHyqd__ACyxGGtcRld__CluisZ", reinterpret_cast<void*>(published_set)},
        {"$s7Combine9PublishedV18_enclosingInstance7wrapped7storagexqd___s24ReferenceWritableKeyPathCyqd__xGAHyqd__ACyxGGtcRld__CluiMZ", reinterpret_cast<void*>(published_modify)},
    };
    const auto found = table.find(name);
    return found == table.end() ? nullptr : found->second;
}
