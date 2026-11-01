#include "stdlib_internal.h"
#include "symbols.h"

using namespace stdlib;

namespace stdlib {

struct BoxMetadataWords {
    void (*destroyer)(void*);
    const void* witnesses;
    uint64_t kind;
};

const BoxMetadataWords kValueBoxMetadata = {free, nullptr, 0};

SWIFTCC void any_hashable_init(SWIFT_OUT AnyHashableValue* out, void* value, void* type, void* hashable) {
    const auto* witnesses = witnesses_of(type);
    memset(out, 0, sizeof *out);
    out->type = type;
    out->witnesses = hashable;
    if (witnesses->flags & kFlagNonInline) {
        const size_t mask = witnesses->flags & 0xff;
        const size_t offset = align_up(kBoxHeaderBytes, mask);
        auto* box = static_cast<char*>(calloc(1, offset + witnesses->size));
        auto* header = reinterpret_cast<HashedHeader*>(box);
        header->metadata = const_cast<uint64_t*>(&kValueBoxMetadata.kind);
        new (&header->references) std::atomic<uint64_t>(1);
        witnesses->initialize_with_take(box + offset, value, type);
        out->buffer[0] = reinterpret_cast<uint64_t>(box);
    } else {
        witnesses->initialize_with_take(out, value, type);
    }
}

// Headers and JSON keys are the only AnyHashable values this app builds: both are strings.
const SwiftString& any_hashable_string(const AnyHashableValue* value) {
    if (foreign_type_name(value->type) != nullptr) {
        engine_log("STOP: AnyHashable holding a non-string value");
        abort();
    }
    return *reinterpret_cast<const SwiftString*>(value);
}

SWIFTCC int64_t any_hashable_raw_hash(int64_t seed, SWIFT_CONTEXT const AnyHashableValue* self) {
    HasherState hasher;
    hasher_seed(&hasher, seed);
    string_hash_into(&hasher, any_hashable_string(self));
    return hasher_finalize(&hasher);
}

SWIFTCC bool any_hashable_equal(const AnyHashableValue* left, const AnyHashableValue* right) {
    return swift_string_to_utf8(any_hashable_string(left)) == swift_string_to_utf8(any_hashable_string(right));
}

// The run length of the standard library's merge sort, computed as it does.
SWIFTCC int64_t minimum_merge_run_length(int64_t count) {
    constexpr int kBitsToUse = 6;
    constexpr int kWordBits = 64;
    if (count < (1 << kBitsToUse)) return count;
    const int offset = (kWordBits - kBitsToUse) - __builtin_clzll(static_cast<uint64_t>(count));
    const int64_t high = count >> offset;
    const int64_t low_mask = (1LL << offset) - 1;
    return (count & low_mask) == 0 ? high : high + 1;
}

// _HashTable is two words: the bitmap's address and the bucket mask.
SWIFTCC int64_t hash_table_start_bucket(const uint64_t* words, uint64_t mask) {
    for (uint64_t bucket = 0; bucket <= mask; ++bucket) {
        if ((words[bucket / kWordBits] >> (bucket % kWordBits)) & 1) return static_cast<int64_t>(bucket);
    }
    return static_cast<int64_t>(mask + 1);
}

SWIFTCC int64_t hash_table_previous_hole(int64_t before, const uint64_t* words, uint64_t mask) {
    uint64_t bucket = static_cast<uint64_t>(before);
    for (;;) {
        bucket = (bucket - 1) & mask;
        if (!((words[bucket / kWordBits] >> (bucket % kWordBits)) & 1)) return static_cast<int64_t>(bucket);
    }
}

constexpr size_t kOptionalIndexNil = 0;

struct OptionalIndex {
    uint64_t index;
    uint64_t is_nil;
};

SWIFTCC OptionalIndex string_index_offset_limited(uint64_t index, int64_t distance, uint64_t limit, SwiftString self) {
    const std::string text = swift_string_to_utf8(self);
    size_t offset = index_offset(index);
    const size_t bound = index_offset(limit);
    for (int64_t i = 0; i < distance; ++i) {
        if (offset == bound) return {kOptionalIndexNil, 1};
        offset = next_scalar(text, offset);
    }
    for (int64_t i = 0; i > distance; --i) {
        if (offset == bound) return {kOptionalIndexNil, 1};
        offset = previous_scalar(text, offset);
    }
    return {make_index(offset), 0};
}

SWIFTCC SwiftString string_remove_at(uint64_t index, SWIFT_CONTEXT SwiftString* self) {
    std::string text = swift_string_to_utf8(*self);
    const size_t start = index_offset(index);
    const size_t end = next_scalar(text, start);
    const std::string removed = text.substr(start, end - start);
    text.erase(start, end - start);
    *self = swift_string_from_utf8(text);
    return swift_string_from_utf8(removed);
}

SWIFTCC void string_remove_subrange(uint64_t start, uint64_t end, SWIFT_CONTEXT SwiftString* self) {
    std::string text = swift_string_to_utf8(*self);
    text.erase(index_offset(start), index_offset(end) - index_offset(start));
    *self = swift_string_from_utf8(text);
}

SWIFTCC uint64_t substring_index_after(uint64_t index, uint64_t, uint64_t, SwiftString guts) {
    return make_index(next_scalar(swift_string_to_utf8(guts), index_offset(index)));
}

SWIFTCC SwiftString substring_subscript(uint64_t index, uint64_t, uint64_t, SwiftString guts) {
    const std::string text = swift_string_to_utf8(guts);
    const size_t start = index_offset(index);
    return swift_string_from_utf8(text.substr(start, next_scalar(text, start) - start));
}

SWIFTCC int64_t substring_utf8_distance(uint64_t from, uint64_t to, uint64_t, uint64_t, SwiftString) {
    return static_cast<int64_t>(index_offset(to)) - static_cast<int64_t>(index_offset(from));
}

SWIFTCC bool character_is_whitespace(SwiftString character) {
    const std::string text = swift_string_to_utf8(character);
    return text == " " || text == "\n" || text == "\t" || text == "\r";
}

SWIFTCC SwiftString type_name(void* type, bool) {
    const char* name = foreign_type_name(type);
    return swift_string_from_utf8(name != nullptr ? name : "Type");
}

void* get_object_type(void* object) { return *static_cast<void**>(object); }

constexpr size_t kClassSuperOffset = 8;

void* dynamic_cast_objc_class(void* object, void* target) {
    if (object == nullptr) return nullptr;
    if (objc_is_framework_class(object, target)) return object;
    const char* cls = static_cast<const char*>(*static_cast<void**>(object));
    while (cls != nullptr) {
        if (cls == target) return object;
        if (unresolved_symbol_at(reinterpret_cast<uintptr_t>(cls)) != nullptr) return nullptr;
        if (foreign_type_name(cls) != nullptr) return nullptr;
        memcpy(&cls, cls + kClassSuperOffset, sizeof cls);
    }
    return nullptr;
}

void array_destroy(char* start, size_t count, void* type) {
    const auto* witnesses = witnesses_of(type);
    for (size_t i = 0; i < count; ++i) witnesses->destroy(start + i * witnesses->stride, type);
}

void* array_init_with_copy(char* destination, char* source, size_t count, void* type) {
    const auto* witnesses = witnesses_of(type);
    for (size_t i = 0; i < count; ++i) witnesses->initialize_with_copy(destination + i * witnesses->stride, source + i * witnesses->stride, type);
    return destination;
}

constexpr size_t kEnumPayloadInfoOffset = 20;

constexpr uint32_t kEnumPayloadCaseMask = 0x00ffffff;

constexpr uint32_t kEnumPayloadOffsetShift = 24;

constexpr size_t kSmallPayloadLimit = 4;

struct EnumShape {
    uint32_t payload_cases;
    size_t payload_size;
    size_t tag_bytes;
};

EnumShape enum_shape(void* type) {
    auto** metadata = static_cast<void**>(type);
    const auto* descriptor = static_cast<const char*>(metadata[kMetadataDescriptorWord]);
    uint32_t info;
    memcpy(&info, descriptor + kEnumPayloadInfoOffset, sizeof info);
    const uint32_t offset = info >> kEnumPayloadOffsetShift;
    const size_t payload_size = reinterpret_cast<size_t>(metadata[offset]);
    return {info & kEnumPayloadCaseMask, payload_size, witnesses_of(type)->size - payload_size};
}

// Framework enums have no descriptor in this runtime: their tag is simply the last byte.
bool is_framework_enum(void* type) { return static_cast<void**>(type)[kMetadataDescriptorWord] == nullptr; }

uint32_t get_enum_case_multi_payload(const char* value, void* type) {
    if (is_framework_enum(type)) return static_cast<uint8_t>(value[witnesses_of(type)->size - 1]);
    const auto shape = enum_shape(type);
    uint32_t tag = 0;
    memcpy(&tag, value + shape.payload_size, shape.tag_bytes < 4 ? shape.tag_bytes : 4);
    if (tag < shape.payload_cases) return tag;
    uint32_t low = 0;
    memcpy(&low, value, shape.payload_size < kSmallPayloadLimit ? shape.payload_size : kSmallPayloadLimit);
    if (shape.payload_size >= kSmallPayloadLimit) return shape.payload_cases + low;
    return shape.payload_cases + (((tag - shape.payload_cases) << (shape.payload_size * 8)) | low);
}

void store_enum_tag_multi_payload(char* value, void* type, uint32_t which) {
    if (is_framework_enum(type)) {
        value[witnesses_of(type)->size - 1] = static_cast<char>(which);
        return;
    }
    const auto shape = enum_shape(type);
    uint32_t tag = which;
    if (which >= shape.payload_cases) {
        const uint32_t index = which - shape.payload_cases;
        if (shape.payload_size >= kSmallPayloadLimit) {
            tag = shape.payload_cases;
            memset(value, 0, shape.payload_size);
            memcpy(value, &index, kSmallPayloadLimit);
        } else {
            tag = shape.payload_cases + (index >> (shape.payload_size * 8));
            memcpy(value, &index, shape.payload_size);
        }
    }
    memcpy(value + shape.payload_size, &tag, shape.tag_bytes < 4 ? shape.tag_bytes : 4);
}

uint32_t tag_bytes_for(size_t payload_size, uint32_t empty_cases) {
    if (payload_size >= kSmallPayloadLimit) return 1;
    const uint64_t cases = ((empty_cases + (1ULL << (payload_size * 8)) - 1) >> (payload_size * 8)) + 1;
    return cases <= 0x100 ? 1 : cases <= 0x10000 ? 2
                                                 : 4;
}

using GetExtraInhabitant = uint32_t (*)(const void*, uint32_t, void*);

using StoreExtraInhabitant = void (*)(void*, uint32_t, uint32_t, void*);

uint32_t get_enum_tag_single_payload_generic(const char* value, uint32_t empty_cases, void* payload, GetExtraInhabitant get) {
    const auto* witnesses = witnesses_of(payload);
    const size_t size = witnesses->size;
    const uint32_t inhabitants = witnesses->extra_inhabitants;
    if (empty_cases > inhabitants) {
        const uint32_t count = tag_bytes_for(size, empty_cases - inhabitants);
        uint32_t extra = 0;
        memcpy(&extra, value + size, count);
        if (extra != 0) {
            const uint32_t high = size >= kSmallPayloadLimit ? 0 : (extra - 1) << (size * 8);
            uint32_t low = 0;
            memcpy(&low, value, size < kSmallPayloadLimit ? size : kSmallPayloadLimit);
            return (high | low) + inhabitants + 1;
        }
    }
    return inhabitants == 0 ? 0 : get(value, inhabitants, payload);
}

void store_enum_tag_single_payload_generic(char* value, uint32_t which, uint32_t empty_cases, void* payload, StoreExtraInhabitant store) {
    const auto* witnesses = witnesses_of(payload);
    const size_t size = witnesses->size;
    const uint32_t inhabitants = witnesses->extra_inhabitants;
    const uint32_t count = empty_cases > inhabitants ? tag_bytes_for(size, empty_cases - inhabitants) : 0;
    if (which <= inhabitants) {
        if (count != 0) memset(value + size, 0, count);
        if (which != 0) store(value, which, inhabitants, payload);
        return;
    }
    const uint32_t index = which - inhabitants - 1;
    uint32_t extra = 1;
    if (size < kSmallPayloadLimit) extra += index >> (size * 8);
    memcpy(value + size, &extra, count);
    memset(value, 0, size);
    memcpy(value, &index, size < kSmallPayloadLimit ? size : kSmallPayloadLimit);
}

// swift_getAtKeyPath receives the result pointer in x8, like any indirect Swift return.
SWIFTCC void get_at_key_path(SWIFT_OUT void* out, void* root, void* key_path, void* value_type) { key_path_get(key_path, root, out, value_type); }

void swift_will_throw(void*) {}

[[noreturn]] void deleted_method_error() {
    engine_log("STOP: guest called a deleted method");
    abort();
}

}  // namespace stdlib

// The case name reflection records hold for a value of an app enum, or empty for anything else.
std::string enum_case_name(const void* value, const void* type) {
    constexpr uint64_t kKindEnum = 0x201;
    constexpr size_t kGetEnumTagOffset = 0x58;
    constexpr size_t kFieldHeaderBytes = 16;
    constexpr size_t kFieldCountOffset = 12;
    constexpr size_t kRecordNameOffset = 8;
    const auto* words = static_cast<void* const*>(type);
    if (reinterpret_cast<uint64_t>(words[0]) != kKindEnum) return {};
    const auto* descriptor = static_cast<const char*>(words[1]);
    const auto relative = [](const char* at) {
        int32_t offset;
        memcpy(&offset, at, sizeof offset);
        return offset == 0 ? nullptr : at + offset;
    };
    const char* fields = relative(descriptor + kDescriptorFieldsOffset);
    if (fields == nullptr) return {};
    uint32_t count;
    memcpy(&count, fields + kFieldCountOffset, sizeof count);
    using GetTag = uint32_t (*)(const void*, const void*);
    GetTag get_tag;
    memcpy(&get_tag, reinterpret_cast<const char*>(witnesses_of(const_cast<void*>(type))) + kGetEnumTagOffset, sizeof get_tag);
    const uint32_t tag = get_tag(value, type);
    if (tag >= count) return {};
    const char* name = relative(fields + kFieldHeaderBytes + tag * kFieldRecordBytes + kRecordNameOffset);
    return name != nullptr ? name : "";
}

constexpr size_t kValueWitnessStrideOffset = 72;

SWIFTCC void print_unlocked(const void* value, SwiftString* stream, void* type);

// A thrown value prints as itself; an error made by these shims has no type, only its message.
static std::string error_description(void* error) {
    struct ErrorParts {
        void* value;
        void* type;
        void* witnesses;
    };
    using GetValue = void (*)(void*, void**, ErrorParts*);
    static const auto get_value = reinterpret_cast<GetValue>(resolve_symbol("swift_getErrorValue"));
    ErrorParts parts{};
    get_value(error, nullptr, &parts);
    if (parts.type == nullptr) return swift_string_to_utf8(foundation_error_message(error));
    SwiftString out = swift_string_from_utf8("");
    print_unlocked(parts.value, &out, parts.type);
    return swift_string_to_utf8(out);
}

// _print_unlocked into a DefaultStringInterpolation, which is a String at runtime.
SWIFTCC void print_unlocked(const void* value, SwiftString* stream, void* type) {
    std::string text;
    if (type == type_system_symbol("$sSSN"))
        text = swift_string_to_utf8(*static_cast<const SwiftString*>(value));
    else if (type == type_system_symbol("$sSiN"))
        text = std::to_string(*static_cast<const int64_t*>(value));
    else if (type == type_system_symbol("$sSbN"))
        text = *static_cast<const uint8_t*>(value) != 0 ? "true" : "false";
    else if (type == error_existential_type())
        text = error_description(*static_cast<void* const*>(value));
    else
        text = enum_case_name(value, type);
    if (text.empty() && type != type_system_symbol("$sSSN")) text = "(value)";
    *stream = swift_string_from_utf8(swift_string_to_utf8(*stream) + text);
}

SWIFTCC SwiftString array_description(void* storage, void* element_type) {
    char* elements;
    const size_t count = array_storage_contents(storage, element_type, &elements);
    const size_t stride = *reinterpret_cast<const size_t*>(reinterpret_cast<const char*>(witnesses_of(element_type)) + kValueWitnessStrideOffset);
    const bool strings = element_type == type_system_symbol("$sSSN");
    std::string text = "[";
    for (size_t i = 0; i < count; ++i) {
        if (i != 0) text += ", ";
        SwiftString part = swift_string_from_utf8("");
        print_unlocked(elements + i * stride, &part, element_type);
        text += strings ? "\"" + swift_string_to_utf8(part) + "\"" : swift_string_to_utf8(part);
    }
    return swift_string_from_utf8(text + "]");
}

void object_array_grow(void** self, int64_t minimum_capacity) {
    auto* old = static_cast<HashedHeader*>(*self);
    const int64_t old_capacity = old->capacity >> 1;
    int64_t capacity = old_capacity * 2;
    if (capacity < minimum_capacity) capacity = minimum_capacity;
    if (capacity < 1) capacity = 1;
    char* elements;
    auto* raw = make_array_storage(any_object_metadata(), static_cast<size_t>(capacity), &elements);
    auto* header = static_cast<HashedHeader*>(raw);
    header->count = old->count;
    char* old_elements;
    array_storage_contents(old, any_object_metadata(), &old_elements);
    for (int64_t i = 0; i < old->count; ++i) {
        void* element;
        memcpy(&element, old_elements + static_cast<size_t>(i) * sizeof element, sizeof element);
        rt_swift_retain(element);
        memcpy(elements + static_cast<size_t>(i) * sizeof element, &element, sizeof element);
    }
    rt_swift_release(old);
    *self = raw;
}

SWIFTCC void object_array_make_unique(SWIFT_CONTEXT void** self) {
    auto* header = static_cast<HashedHeader*>(*self);
    const int64_t capacity = header->capacity >> 1;
    if (header->references.load(std::memory_order_relaxed) != 1 || header->count >= capacity) object_array_grow(self, header->count + 1);
}

SWIFTCC void object_array_create_new_buffer(bool, int64_t minimum_capacity, bool, SWIFT_CONTEXT void** self) { object_array_grow(self, minimum_capacity); }

// A URL and a UUID are both one box holding their text, and compare by that text.
static bool is_text_box_type(const char* name) { return name != nullptr && (strcmp(name, "_$s10Foundation3URLVMn") == 0 || strcmp(name, "_$s10Foundation4UUIDVMn") == 0); }

SWIFTCC void object_array_append(int64_t old_count, void* element, SWIFT_CONTEXT void** self) {
    auto* header = static_cast<HashedHeader*>(*self);
    char* elements;
    array_storage_contents(header, any_object_metadata(), &elements);
    memcpy(elements + static_cast<size_t>(old_count) * sizeof element, &element, sizeof element);
    header->count = old_count + 1;
}

// Equatable's dispatch thunk; framework types only reach it as String, URL or UUID.
SWIFTCC bool equatable_equal(const void* left, const void* right, void* type, void* table) {
    if (type == type_system_symbol("$sSSN")) return swift_string_to_utf8(*static_cast<const SwiftString*>(left)) == swift_string_to_utf8(*static_cast<const SwiftString*>(right));
    if (is_text_box_type(foreign_type_name(type))) {
        return foundation_url_text(*static_cast<void* const*>(left)) == foundation_url_text(*static_cast<void* const*>(right));
    }
    // Framework values with no references, such as enum tags, are equal when their bytes are.
    if (const auto* witnesses = witnesses_of(type); foreign_type_name(type) != nullptr && (witnesses->flags & kFlagNonPod) == 0) return memcmp(left, right, witnesses->size) == 0;
    using Equal = SWIFTCC bool (*)(const void*, const void*, void*, void*, SWIFT_CONTEXT void*);
    const auto equal = reinterpret_cast<Equal>(const_cast<void*>(swift_runtime_find_witness_named(static_cast<const char*>(static_cast<void**>(table)[0]), "_$sSQ2eeoiySbx_xtFZTq")));
    if (equal == nullptr) {
        engine_log("STOP: == on a type with no witness");
        abort();
    }
    return equal(left, right, type, table, type);
}

// A framework OptionSet is its raw value, whatever the type's witness table says.
SWIFTCC void option_set_init(SWIFT_OUT void* out, const void* raw, void* type, void*) { memcpy(out, raw, witnesses_of(type)->size); }

// SetAlgebra(sequence): the only framework values built this way are detents and flags, which nothing reads back.
SWIFTCC void set_algebra_from_sequence(SWIFT_OUT void* out, void*, void* type, void*, void*, void*) {
    const size_t size = witnesses_of(type)->size;
    memset(out, 0, size);
    if (foreign_type_name(type) == nullptr && size == sizeof(void*)) {
        void* empty = resolve_symbol("_swiftEmptySetSingleton");
        memcpy(out, &empty, sizeof empty);
    }
}

// Characters here are Unicode scalars, which is what the app's ASCII strings need.
int64_t scalar_count(const std::string& text) {
    int64_t count = 0;
    for (const char c : text) count += (static_cast<unsigned char>(c) & 0xC0) != 0x80 ? 1 : 0;
    return count;
}

SWIFTCC int64_t string_count(SwiftString text) { return scalar_count(swift_string_to_utf8(text)); }

SWIFTCC SwiftString string_describing(const void* value, void* type);

// The items array holds existentials: inline buffer words, then the value's type in the fourth.
SWIFTCC void swift_print(void* items, SwiftString separator, SwiftString terminator) {
    constexpr size_t kExistentialSize = 32, kExistentialTypeWord = 3, kArrayCountOffset = 16;
    int64_t count;
    memcpy(&count, static_cast<char*>(items) + kArrayCountOffset, sizeof count);
    std::string line;
    for (int64_t i = 0; i < count; ++i) {
        auto* item = reinterpret_cast<void**>(static_cast<char*>(items) + kArrayElementsOffset + static_cast<size_t>(i) * kExistentialSize);
        void* type = item[kExistentialTypeWord];
        const auto* witnesses = witnesses_of(type);
        const void* value = item;
        if (witnesses->flags & kFlagNonInline) {
            const size_t mask = witnesses->flags & 0xff;
            value = static_cast<char*>(item[0]) + ((sizeof(HeapObject) + mask) & ~mask);
        }
        if (i != 0) line += swift_string_to_utf8(separator);
        line += swift_string_to_utf8(string_describing(value, type));
    }
    engine_log("PRINT: %s", line.c_str());
    line += swift_string_to_utf8(terminator);
    fwrite(line.data(), 1, line.size(), stdout);
    fflush(stdout);
}

SWIFTCC SwiftString string_describing(const void* value, void* type) {
    SwiftString out = swift_string_from_utf8("");
    print_unlocked(value, &out, type);
    return out;
}

SWIFTCC SwiftString string_from_c_string(const char* text) { return swift_string_from_utf8(text); }

// os.Logger is one placeholder word: logging goes nowhere, and os_log_type_enabled already answers false.
SWIFTCC void logger_init(SWIFT_OUT uint64_t* out) { *out = 1ULL << 63; }

SWIFTCC uint64_t logger_object() { return 1ULL << 63; }

template <uint8_t Level>
SWIFTCC uint8_t log_type_getter() { return Level; }

void array_take_forward(char* to, char* from, size_t count, void* type) {
    const auto* witnesses = witnesses_of(type);
    for (size_t i = 0; i < count; ++i) witnesses->initialize_with_take(to + i * witnesses->stride, from + i * witnesses->stride, type);
}

void array_take_backward(char* to, char* from, size_t count, void* type) {
    const auto* witnesses = witnesses_of(type);
    for (size_t i = count; i-- > 0;) witnesses->initialize_with_take(to + i * witnesses->stride, from + i * witnesses->stride, type);
}

// A box holds its type, then the value at the alignment the type needs.
struct BoxHeader {
    void* metadata;
    std::atomic<uint64_t> references;
    void* type;
};

size_t box_value_offset(void* type) {
    const size_t mask = witnesses_of(type)->flags & 0xff;
    return (sizeof(BoxHeader) + mask) & ~mask;
}

void destroy_boxed(void* raw) {
    auto* box = static_cast<BoxHeader*>(raw);
    if (box->type != nullptr) witnesses_of(box->type)->destroy(static_cast<char*>(raw) + box_value_offset(box->type), box->type);
    free(raw);
}

struct BoxMetadataLayout {
    void (*destroyer)(void*);
    const void* witnesses;
    uint64_t kind;
};

const BoxMetadataLayout kBoxLayout = {destroy_boxed, nullptr, 0};

struct BoxPair {
    void* box;
    void* value;
};

BoxPair alloc_box(void* type) {
    const size_t offset = box_value_offset(type);
    auto* raw = static_cast<char*>(calloc(1, offset + witnesses_of(type)->size + 1));
    auto* box = reinterpret_cast<BoxHeader*>(raw);
    box->metadata = const_cast<uint64_t*>(&kBoxLayout.kind);
    new (&box->references) std::atomic<uint64_t>(1);
    box->type = type;
    return {box, raw + offset};
}

void* project_box(void* box) { return static_cast<char*>(box) + box_value_offset(static_cast<BoxHeader*>(box)->type); }

constexpr size_t kSuperclassOffset = 8;

// Walks a class's ancestry; a framework superclass is a guard address that can't be read, only compared.
bool class_inherits(const void* cls, const void* target) {
    while (cls != nullptr) {
        if (cls == target) return true;
        if (unresolved_symbol_at(reinterpret_cast<uintptr_t>(cls)) != nullptr) return false;
        memcpy(&cls, static_cast<const char*>(cls) + kSuperclassOffset, sizeof cls);
    }
    return false;
}

void* dynamic_cast_class(void* object, const void* target) {
    if (object == nullptr) return nullptr;
    const void* cls;
    memcpy(&cls, object, sizeof cls);
    return class_inherits(cls, target) ? object : nullptr;
}

void* dynamic_cast_class_unconditional(void* object, const void* target, const char*, uint32_t, uint32_t) {
    void* result = dynamic_cast_class(object, target);
    if (result == nullptr) {
        engine_log("STOP: an unconditional class cast failed");
        abort();
    }
    return result;
}

const void* dynamic_cast_metatype(const void* type, const void* target) { return class_inherits(type, target) ? type : nullptr; }

void* identity_cast(void* object) { return object; }

constexpr uint64_t kKindExistentialMetadata = 0x303;

// Only a class existential's value carries its own dynamic type here.
void* dynamic_type_of(void* value, void* type, bool) {
    if (*static_cast<uint64_t*>(type) == kKindExistentialMetadata) {
        void* object;
        memcpy(&object, value, sizeof object);
        void* cls;
        memcpy(&cls, object, sizeof cls);
        return cls;
    }
    return type;
}

bool is_uniquely_referenced(void* object) { return object != nullptr && static_cast<BoxHeader*>(object)->references.load(std::memory_order_relaxed) == 1; }

void release_n(void* object, uint32_t count) {
    for (uint32_t i = 0; i < count; ++i) rt_swift_release(object);
}

void* retain_n(void* object, uint32_t count) {
    for (uint32_t i = 0; i < count; ++i) rt_swift_retain(object);
    return object;
}

// Weak references are strong here: an object stays alive rather than a read finding it gone, which only costs memory.
void* weak_init(void** slot, void* object) {
    *slot = rt_swift_retain(object);
    return slot;
}

void* weak_assign(void** slot, void* object) {
    rt_swift_retain(object);
    rt_swift_release(*slot);
    *slot = object;
    return slot;
}

void* weak_load_strong(void** slot) { return rt_swift_retain(*slot); }

void weak_destroy(void** slot) { rt_swift_release(*slot); }

void* weak_copy_init(void** slot, void** from) {
    *slot = rt_swift_retain(*from);
    return slot;
}

void* weak_take_init(void** slot, void** from) {
    *slot = *from;
    return slot;
}

void* weak_copy_assign(void** slot, void** from) { return weak_assign(slot, *from); }

void* weak_take_assign(void** slot, void** from) {
    rt_swift_release(*slot);
    *slot = *from;
    return slot;
}

bool escaping_closure_check(const void*, const void*, int32_t, int32_t, int32_t) { return false; }

[[noreturn]] void unexpected_error(void*, const char*, int64_t, bool, const char*, int64_t, bool, const char*, int64_t, bool, uint64_t) {
    engine_log("STOP: an error reached a try! expression");
    abort();
}

// A sequence of Characters, which in this runtime means a String or an array of them.
SWIFTCC void string_append_sequence(const void* sequence, void* type, void*, SWIFT_CONTEXT SwiftString* self) {
    std::string text = swift_string_to_utf8(*self);
    if (type == type_system_symbol("$sSSN")) {
        text += swift_string_to_utf8(*static_cast<const SwiftString*>(sequence));
    } else if (void* element = array_element_type(type)) {
        uint64_t word;
        memcpy(&word, sequence, sizeof word);
        char* elements;
        const size_t count = array_storage_contents(reinterpret_cast<void*>(word & kBridgePointerMask), element, &elements);
        for (size_t i = 0; i < count; ++i) text += swift_string_to_utf8(*reinterpret_cast<const SwiftString*>(elements + i * sizeof(SwiftString)));
    } else {
        engine_log("STOP: append(contentsOf:) on a sequence other than String or Array");
        abort();
    }
    *self = swift_string_from_utf8(text);
}

// Hashable's dispatch thunk: framework values hash by what they hold, and the app's types by their own witness.
SWIFTCC int64_t hashable_raw_value(int64_t seed, void* type, void* table, SWIFT_CONTEXT const void* self) {
    if (type == type_system_symbol("$sSSN")) return seed ^ static_cast<int64_t>(std::hash<std::string>{}(swift_string_to_utf8(*static_cast<const SwiftString*>(self))));
    const char* name = foreign_type_name(type);
    if (is_text_box_type(name)) return seed ^ static_cast<int64_t>(std::hash<std::string>{}(foundation_url_text(*static_cast<void* const*>(self))));
    if (const auto* witnesses = witnesses_of(type); name != nullptr || (witnesses->flags & kFlagNonPod) == 0) {
        const std::string bytes(static_cast<const char*>(self), witnesses->size);
        return seed ^ static_cast<int64_t>(std::hash<std::string>{}(bytes));
    }
    using RawHash = SWIFTCC int64_t (*)(int64_t, void*, void*, SWIFT_CONTEXT const void*);
    const auto raw = reinterpret_cast<RawHash>(const_cast<void*>(swift_runtime_find_witness_named(static_cast<const char*>(static_cast<void**>(table)[0]), "_$sSH13_rawHashValue4seedS2i_tFTq")));
    if (raw == nullptr) {
        engine_log("STOP: hash of a type with no witness");
        abort();
    }
    return raw(seed, type, table, self);
}

// BidirectionalCollection<String>.joined: an Array is the only such collection this runtime hands out.
SWIFTCC SwiftString strings_joined(SwiftString separator, void* type, void*, SWIFT_CONTEXT const uint64_t* self) {
    if (array_element_type(type) == nullptr) {
        engine_log("STOP: joined(separator:) on a collection other than Array");
        abort();
    }
    char* elements;
    const size_t count = array_storage_contents(reinterpret_cast<void*>(*self & kBridgeAddressMask), type_system_symbol("$sSSN"), &elements);
    const std::string glue = swift_string_to_utf8(separator);
    std::string out;
    for (size_t i = 0; i < count; ++i) {
        if (i != 0) out += glue;
        out += swift_string_to_utf8(reinterpret_cast<const SwiftString*>(elements)[i]);
    }
    return swift_string_from_utf8(out);
}

namespace stdlib {

void add_stdlib_runtime_symbols(SymbolTable& table) {
    table.insert({
        {"$sSa034_makeUniqueAndReserveCapacityIfNotB0yyFyXl_Ts5", reinterpret_cast<void*>(object_array_make_unique)},
        {"$sSa16_createNewBuffer14bufferIsUnique15minimumCapacity13growForAppendySb_SiSbtFyXl_Ts5", reinterpret_cast<void*>(object_array_create_new_buffer)},
        {"$sSa37_appendElementAssumeUniqueAndCapacity_03newB0ySi_xntFyXl_Ts5", reinterpret_cast<void*>(object_array_append)},
        {"$sSQ2eeoiySbx_xtFZTj", reinterpret_cast<void*>(equatable_equal)},
        {"$ss10SetAlgebraPyxqd__ncSTRd__7ElementQyd__ACRtzlufCTj", reinterpret_cast<void*>(set_algebra_from_sequence)},
        {"$ss9OptionSetP8rawValuex03RawD0Qz_tcfCTj", reinterpret_cast<void*>(option_set_init)},
        {"$ss15_print_unlockedyyx_q_zts16TextOutputStreamR_r0_lF", reinterpret_cast<void*>(print_unlocked)},
        {"$sSa11descriptionSSvg", reinterpret_cast<void*>(array_description)},
        {"$sSKsSS7ElementRtzrlE6joined9separatorS2S_tF", reinterpret_cast<void*>(strings_joined)},
        // Signposts only feed Instruments: the ID stays null and the calls vanish.
        {"$sSo18os_signpost_type_ta0A0E5eventABvgZ", reinterpret_cast<void*>(log_type_getter<0x00>)},
        {"$sSo18os_signpost_type_ta0A0E5beginABvgZ", reinterpret_cast<void*>(log_type_getter<0x01>)},
        {"$sSo18os_signpost_type_ta0A0E3endABvgZ", reinterpret_cast<void*>(log_type_getter<0x02>)},
        {"$sSo9OS_os_logC0B0E9subsystem8categoryABSS_SStcfC", reinterpret_cast<void*>(+[](uint64_t, uint64_t, uint64_t, uint64_t) -> void* { return objc_framework_object("OS_os_log"); })},
        {"$s2os0A4_log_3dso0B04type_ys12StaticStringV_SVSgSo03OS_a1_B0CSo0a1_b1_D2_tas7CVarArg_pdtF", reinterpret_cast<void*>(+[]() {})},
        {"$s2os6LoggerV9subsystem8categoryACSS_SStcfC", reinterpret_cast<void*>(logger_init)},
        {"$s2os6LoggerV9logObjectSo03OS_a1_C0Cvg", reinterpret_cast<void*>(logger_object)},
        {"$sSo13os_log_type_ta0A0E5errorABvgZ", reinterpret_cast<void*>(log_type_getter<0x10>)},
        {"$sSo13os_log_type_ta0A0E4infoABvgZ", reinterpret_cast<void*>(log_type_getter<0x01>)},
        {"$sSo13os_log_type_ta0A0E5debugABvgZ", reinterpret_cast<void*>(log_type_getter<0x02>)},
        {"$sSo13os_log_type_ta0A0E7defaultABvgZ", reinterpret_cast<void*>(log_type_getter<0x00>)},
        {"$sSo13os_log_type_ta0A0E5faultABvgZ", reinterpret_cast<void*>(log_type_getter<0x11>)},
        {"$sSS6append10contentsOfyx_tSTRzSJ7ElementRtzlF", reinterpret_cast<void*>(string_append_sequence)},
        {"$sSH13_rawHashValue4seedS2i_tFTj", reinterpret_cast<void*>(hashable_raw_value)},
        {"$sSS5countSivg", reinterpret_cast<void*>(string_count)},
        {"$ss5print_9separator10terminatoryypd_S2StF", reinterpret_cast<void*>(swift_print)},
        {"$sSS10describingSSx_tclufC", reinterpret_cast<void*>(string_describing)},
        {"$sSS7cStringSSSPys4Int8VG_tcfC", reinterpret_cast<void*>(string_from_c_string)},
        {"$sSS7cStringSSSPys5UInt8VG_tcfC", reinterpret_cast<void*>(string_from_c_string)},
        {"$ss11AnyHashableVyABxcSHRzlufC", reinterpret_cast<void*>(any_hashable_init)},
        {"$ss11AnyHashableV13_rawHashValue4seedS2i_tF", reinterpret_cast<void*>(any_hashable_raw_hash)},
        {"$ss11AnyHashableV2eeoiySbAB_ABtFZ", reinterpret_cast<void*>(any_hashable_equal)},
        {"$ss22_minimumMergeRunLengthyS2iF", reinterpret_cast<void*>(minimum_merge_run_length)},
        {"swift_arrayInitWithTakeFrontToBack", reinterpret_cast<void*>(array_take_forward)},
        {"swift_arrayInitWithTakeBackToFront", reinterpret_cast<void*>(array_take_backward)},
        {"swift_allocBox", reinterpret_cast<void*>(alloc_box)},
        {"swift_projectBox", reinterpret_cast<void*>(project_box)},
        {"swift_dynamicCastClass", reinterpret_cast<void*>(dynamic_cast_class)},
        {"swift_dynamicCastClassUnconditional", reinterpret_cast<void*>(dynamic_cast_class_unconditional)},
        {"swift_dynamicCastMetatype", reinterpret_cast<void*>(dynamic_cast_metatype)},
        {"swift_dynamicCastObjCClassUnconditional", reinterpret_cast<void*>(identity_cast)},
        {"swift_dynamicCastObjCProtocolConditional", reinterpret_cast<void*>(identity_cast)},
        {"swift_dynamicCastObjCProtocolUnconditional", reinterpret_cast<void*>(identity_cast)},
        {"swift_getDynamicType", reinterpret_cast<void*>(dynamic_type_of)},
        {"swift_isUniquelyReferenced_native", reinterpret_cast<void*>(is_uniquely_referenced)},
        {"swift_isUniquelyReferencedNonObjC_nonNull", reinterpret_cast<void*>(is_uniquely_referenced)},
        {"swift_release_n", reinterpret_cast<void*>(release_n)},
        {"swift_unknownObjectRelease_n", reinterpret_cast<void*>(release_n)},
        {"swift_unknownObjectRetain_n", reinterpret_cast<void*>(retain_n)},
        {"swift_weakInit", reinterpret_cast<void*>(weak_init)},
        {"swift_weakAssign", reinterpret_cast<void*>(weak_assign)},
        {"swift_weakLoadStrong", reinterpret_cast<void*>(weak_load_strong)},
        {"swift_weakDestroy", reinterpret_cast<void*>(weak_destroy)},
        {"swift_unknownObjectWeakInit", reinterpret_cast<void*>(weak_init)},
        {"swift_unknownObjectWeakAssign", reinterpret_cast<void*>(weak_assign)},
        {"swift_unknownObjectWeakLoadStrong", reinterpret_cast<void*>(weak_load_strong)},
        {"swift_unknownObjectWeakDestroy", reinterpret_cast<void*>(weak_destroy)},
        {"swift_unknownObjectWeakCopyInit", reinterpret_cast<void*>(weak_copy_init)},
        {"swift_unknownObjectWeakTakeInit", reinterpret_cast<void*>(weak_take_init)},
        {"swift_unknownObjectWeakCopyAssign", reinterpret_cast<void*>(weak_copy_assign)},
        {"swift_unknownObjectWeakTakeAssign", reinterpret_cast<void*>(weak_take_assign)},
        {"swift_isEscapingClosureAtFileLocation", reinterpret_cast<void*>(escaping_closure_check)},
        {"swift_unexpectedError", reinterpret_cast<void*>(unexpected_error)},
        {"$ss10_HashTableV11startBucketAB0D0Vvg", reinterpret_cast<void*>(hash_table_start_bucket)},
        {"$ss10_HashTableV12previousHole6beforeAB6BucketVAF_tF", reinterpret_cast<void*>(hash_table_previous_hole)},
        {"$sSS5index_8offsetBy07limitedC0SS5IndexVSgAE_SiAEtF", reinterpret_cast<void*>(string_index_offset_limited)},
        {"$sSS6remove2atSJSS5IndexV_tF", reinterpret_cast<void*>(string_remove_at)},
        {"$sSS14removeSubrangeyySnySS5IndexVGF", reinterpret_cast<void*>(string_remove_subrange)},
        {"$sSs5index5afterSS5IndexVAD_tF", reinterpret_cast<void*>(substring_index_after)},
        {"$sSsySJSS5IndexVcig", reinterpret_cast<void*>(substring_subscript)},
        {"$sSs8UTF8ViewV8distance4from2toSiSS5IndexV_AGtF", reinterpret_cast<void*>(substring_utf8_distance)},
        {"$sSJ12isWhitespaceSbvg", reinterpret_cast<void*>(character_is_whitespace)},
        {"$ss9_typeName_9qualifiedSSypXp_SbtF", reinterpret_cast<void*>(type_name)},
        {"swift_getObjectType", reinterpret_cast<void*>(get_object_type)},
        {"swift_dynamicCastObjCClass", reinterpret_cast<void*>(dynamic_cast_objc_class)},
        {"swift_arrayDestroy", reinterpret_cast<void*>(array_destroy)},
        {"swift_arrayInitWithCopy", reinterpret_cast<void*>(array_init_with_copy)},
        {"swift_getEnumCaseMultiPayload", reinterpret_cast<void*>(get_enum_case_multi_payload)},
        {"swift_storeEnumTagMultiPayload", reinterpret_cast<void*>(store_enum_tag_multi_payload)},
        {"swift_getEnumTagSinglePayloadGeneric", reinterpret_cast<void*>(get_enum_tag_single_payload_generic)},
        {"swift_storeEnumTagSinglePayloadGeneric", reinterpret_cast<void*>(store_enum_tag_single_payload_generic)},
        {"swift_getAtKeyPath", reinterpret_cast<void*>(get_at_key_path)},
        {"swift_willThrow", reinterpret_cast<void*>(swift_will_throw)},
        {"swift_deletedMethodError", reinterpret_cast<void*>(deleted_method_error)},
    });
}

}  // namespace stdlib
