#include "type_system_internal.h"

namespace type_system {

struct SingletonInitialization {
    int32_t cache;
    int32_t incomplete;
    int32_t completion;
};

using CompletionFunction = void* (*)(void*, void*, void*);

const void* relative_target(const int32_t* field) { return reinterpret_cast<const char*>(field) + *field; }

constexpr uint64_t kKindObjCClassWrapper = 0x305;

// The metadata form of an Objective-C class: lets it stand as a generic argument, stored as one retained word.
extern "C" void* rt_getObjCClassMetadata(void* cls) {
    const std::lock_guard guard(g_types_lock);
    if (unresolved_symbol_at(reinterpret_cast<uintptr_t>(cls)) == nullptr) return cls;
    const std::string key = "objc" + pointer_key(cls);
    if (const auto found = g_named_types.find(key); found != g_named_types.end()) return found->second;
    auto* metadata = make_metadata(kKindObjCClassWrapper, cls, {}, new Layout{LayoutKind::NativeReference, {}, nullptr}, kPointerSize, kPointerSize, kPointerExtraInhabitants);
    g_named_types[key] = metadata;
    return metadata;
}

extern "C" void* rt_getObjCClassFromMetadata(Metadata* metadata) {
    return metadata->kind == kKindObjCClassWrapper ? const_cast<void*>(metadata->descriptor) : metadata;
}

extern "C" MetadataResponse rt_getSingletonMetadata(size_t, const char* descriptor) {
    static pthread_mutex_t lock = PTHREAD_RECURSIVE_MUTEX_INITIALIZER_NP;
    uint32_t flags;
    memcpy(&flags, descriptor, sizeof flags);
    size_t header = (flags & kKindMask) == kContextClass ? kClassHeaderSize : kValueTypeHeaderSize;
    if (flags & kFlagGeneric) {
        engine_log("STOP: swift_getSingletonMetadata on a generic descriptor");
        abort();
    }
    if ((flags & kKindMask) == kContextClass && (flags & kClassHasResilientSuperclass)) header += sizeof(int32_t);
    if ((flags & kSingletonInitializationMask) != kSingletonInitialization) {
        engine_log("STOP: swift_getSingletonMetadata on a descriptor without singleton initialization");
        abort();
    }
    const auto* initialization = reinterpret_cast<const SingletonInitialization*>(descriptor + header);
    auto** cache = const_cast<void**>(static_cast<void* const*>(relative_target(&initialization->cache)));

    pthread_mutex_lock(&lock);
    if (cache[0] == nullptr) {
        void* metadata = const_cast<void*>(relative_target(&initialization->incomplete));
        auto completion = reinterpret_cast<CompletionFunction>(const_cast<void*>(relative_target(&initialization->completion)));
        cache[0] = metadata;
        void* dependency = completion(metadata, nullptr, nullptr);
        if (dependency != nullptr) {
            engine_log("STOP: metadata completion reported an unresolved dependency");
            abort();
        }
    }
    void* result = cache[0];
    pthread_mutex_unlock(&lock);
    return {result, 0};
}

const ValueWitnessTable* witnesses_from_type_layout(const void* layout) {
    return reinterpret_cast<const ValueWitnessTable*>(static_cast<const char*>(layout) - kValueWitnessSizeOffset);
}

extern "C" void rt_initStructMetadata(void** metadata, size_t, size_t field_count, const void* const* field_layouts, uint32_t* field_offsets) {
    size_t offset = 0;
    size_t alignment_mask = 0;
    uint32_t flags = 0;
    uint32_t inhabitants = 0;
    for (size_t i = 0; i < field_count; ++i) {
        const auto* field = witnesses_from_type_layout(field_layouts[i]);
        const size_t mask = field->flags & 0xff;
        offset = (offset + mask) & ~mask;
        field_offsets[i] = static_cast<uint32_t>(offset);
        offset += field->size;
        if (mask > alignment_mask) alignment_mask = mask;
        flags |= field->flags & (kFlagNonPod | kFlagNonInline);
        if (field->extra_inhabitants > inhabitants) inhabitants = field->extra_inhabitants;
    }
    auto* updated = new ValueWitnessTable(*static_cast<ValueWitnessTable*>(metadata[-1]));
    updated->size = offset;
    updated->stride = offset == 0 ? 1 : (offset + alignment_mask) & ~alignment_mask;
    updated->flags = static_cast<uint32_t>(alignment_mask) | (flags & kFlagNonPod);
    if (offset > kInlineBufferSize || alignment_mask >= kPointerSize) updated->flags |= kFlagNonInline;
    updated->extra_inhabitants = inhabitants;
    metadata[-1] = updated;
}

// Generic metadata instantiation for the app's own generic structs and enums, per the Swift ABI.
constexpr size_t kStructGenericHeaderOffset = 28;

constexpr size_t kEnumGenericHeaderOffset = 28;

constexpr size_t kClassGenericHeaderOffset = 44;

constexpr size_t kPatternOffsetInHeader = 4;

constexpr size_t kNumKeyArgumentsOffset = 12;  // After the two relative pointers and NumParams, NumRequirements.

constexpr uint32_t kPatternHasExtraData = 1;

constexpr unsigned kPatternValueKindShift = 21;

constexpr uint32_t kPatternValueKindMask = 0x7ff;

constexpr size_t kPatternFlagsOffset = 8;

constexpr size_t kValuePatternWitnessesOffset = 12;

constexpr size_t kValuePatternExtraOffset = 16;

constexpr size_t kValueMetadataHeaderBytes = 3 * kPointerSize;

const char* relative_address(const char* field) {
    int32_t offset;
    memcpy(&offset, field, sizeof offset);
    return offset == 0 ? nullptr : field + offset;
}

size_t generic_header_offset(const char* descriptor) {
    uint32_t flags;
    memcpy(&flags, descriptor, sizeof flags);
    switch (flags & kDescriptorKindMask) {
        case kContextKindClass:
            return kClassGenericHeaderOffset;
        case kContextKindStruct:
            return kStructGenericHeaderOffset;
        default:
            return kEnumGenericHeaderOffset;
    }
}

uint16_t key_argument_count(const char* descriptor) {
    uint16_t count;
    memcpy(&count, descriptor + generic_header_offset(descriptor) + kNumKeyArgumentsOffset, sizeof count);
    return count;
}

constexpr size_t kGenericHeaderBytes = 16;

constexpr size_t kNumParamsOffset = 8;

constexpr size_t kNumRequirementsOffset = 10;

constexpr size_t kRequirementBytes = 12;

// A generic type's accessor takes its parameters, then one witness table per protocol requirement.
std::vector<void*> key_arguments_for(const char* descriptor, const std::vector<void*>& parameters) {
    std::vector<void*> arguments = parameters;
    uint32_t flags;
    memcpy(&flags, descriptor, sizeof flags);
    if (!(flags & kDescriptorGeneric)) return arguments;
    const char* header = descriptor + generic_header_offset(descriptor);
    uint16_t param_count, requirement_count;
    memcpy(&param_count, header + kNumParamsOffset, sizeof param_count);
    memcpy(&requirement_count, header + kNumRequirementsOffset, sizeof requirement_count);
    const char* requirements = header + kGenericHeaderBytes + ((param_count + 3u) & ~3u);
    for (uint16_t i = 0; i < requirement_count; ++i) {
        const char* requirement = requirements + i * kRequirementBytes;
        uint32_t requirement_flags;
        memcpy(&requirement_flags, requirement, sizeof requirement_flags);
        if (!(requirement_flags & kRequirementHasKeyArgument) || (requirement_flags & kRequirementKindMask) != kRequirementKindProtocol) continue;
        const char* subject_name = relative_address(requirement + sizeof(int32_t));
        void* subject = rt_getTypeByMangledNameInContext2(subject_name, strlen(subject_name), descriptor, parameters.data());
        int32_t offset;
        memcpy(&offset, requirement + 2 * sizeof(int32_t), sizeof offset);
        // The low bit marks an indirect reference; a set second bit would mean an ObjC protocol, which has no table.
        const char* protocol_field = requirement + 2 * sizeof(int32_t) + (offset & ~3);
        const void* protocol = offset & 1 ? *reinterpret_cast<const void* const*>(protocol_field) : protocol_field;
        arguments.push_back(swift_runtime_find_conformance(subject, protocol));
    }
    return arguments;
}

extern "C" void* rt_allocateGenericValueMetadata(const char* descriptor, void* const* arguments, const char* pattern, size_t extra_size) {
    auto* block = static_cast<void**>(calloc(1, kValueMetadataHeaderBytes + extra_size));
    void** metadata = block + 1;
    uint32_t flags;
    memcpy(&flags, pattern + kPatternFlagsOffset, sizeof flags);
    metadata[-1] = const_cast<char*>(relative_address(pattern + kValuePatternWitnessesOffset));
    metadata[0] = reinterpret_cast<void*>(static_cast<uintptr_t>((flags >> kPatternValueKindShift) & kPatternValueKindMask));
    metadata[1] = const_cast<char*>(descriptor);
    memcpy(metadata + 2, arguments, key_argument_count(descriptor) * sizeof(void*));
    if (flags & kPatternHasExtraData) {
        const char* partial = pattern + kValuePatternExtraOffset;
        uint16_t offset_in_words, size_in_words;
        memcpy(&offset_in_words, partial + 4, sizeof offset_in_words);
        memcpy(&size_in_words, partial + 6, sizeof size_in_words);
        // The pattern's offset counts from after the kind and descriptor words.
        memcpy(metadata + 2 + offset_in_words, relative_address(partial), size_in_words * sizeof(void*));
    }
    return metadata;
}

struct CachedGeneric {
    const char* descriptor;
    std::vector<void*> arguments;
    bool operator<(const CachedGeneric& other) const { return descriptor != other.descriptor ? descriptor < other.descriptor : arguments < other.arguments; }
};

extern "C" MetadataResponse rt_getGenericMetadata(size_t, void* const* arguments, const char* descriptor) {
    static std::map<CachedGeneric, void*> cache;
    const uint16_t count = key_argument_count(descriptor);
    CachedGeneric key{descriptor, std::vector<void*>(arguments, arguments + count)};
    {
        const std::lock_guard guard(g_types_lock);
        if (const auto found = cache.find(key); found != cache.end()) return {static_cast<Metadata*>(found->second), 0};
    }
    const char* pattern = relative_address(descriptor + generic_header_offset(descriptor) + kPatternOffsetInHeader);
    using Instantiate = void* (*)(const void*, void* const*, const void*);
    using Complete = MetadataResponse (*)(void*, void*, const void*);
    auto instantiate = reinterpret_cast<Instantiate>(const_cast<char*>(relative_address(pattern)));
    void* metadata = instantiate(descriptor, arguments, pattern);
    {
        // Recursive types look themselves up while completing.
        const std::lock_guard guard(g_types_lock);
        cache[key] = metadata;
    }
    if (const char* completion = relative_address(pattern + sizeof(int32_t))) {
        void* context[4] = {};
        reinterpret_cast<Complete>(const_cast<char*>(completion))(metadata, context, pattern);
    }
    return {static_cast<Metadata*>(metadata), 0};
}

// Arguments arrive complete, witness tables included, so unlike the mangled form nothing needs looking up.
extern "C" MetadataResponse rt_getOpaqueTypeMetadata(size_t, void* const* arguments, const char* descriptor, size_t ordinal) {
    if (const char* symbol = unresolved_symbol_at(reinterpret_cast<uintptr_t>(descriptor))) {
        const std::lock_guard guard(g_types_lock);
        return {foreign_nominal(symbol, {}), 0};
    }
    uint32_t flags;
    memcpy(&flags, descriptor, sizeof flags);
    const char* trailing = descriptor + 2 * sizeof(int32_t);
    if (flags & kDescriptorGeneric) {
        uint16_t params, requirements;
        memcpy(&params, trailing, sizeof params);
        memcpy(&requirements, trailing + 2, sizeof requirements);
        trailing += kGenericHeaderSize + ((params + 3u) & ~3u) + requirements * kGenericRequirementSize;
    }
    const char* underlying = relative_address(trailing + ordinal * sizeof(int32_t));
    const size_t length = mangled_name_length(underlying);
    return {static_cast<Metadata*>(rt_getTypeByMangledNameInContext2(underlying, length, descriptor, arguments)), 0};
}

// Types the app builds at runtime instead of naming them.
constexpr size_t kOpaqueExistentialWords = 4;

constexpr size_t kClassConstraintClass = 0;

extern "C" Metadata* rt_getExistentialTypeMetadata(size_t class_constraint, const void*, size_t protocol_count, const void* const*) {
    const std::lock_guard guard(g_types_lock);
    if (class_constraint == kClassConstraintClass) {
        if (protocol_count == 0) return native_type();
        const std::string key = "ClassExistential" + std::to_string(protocol_count);
        auto* layout = new Layout{LayoutKind::Aggregate, {{native_type(), 0}}, nullptr};
        return named_type(key, kKindStruct, layout, (1 + protocol_count) * kPointerSize, kPointerSize, kPointerExtraInhabitants);
    }
    if (protocol_count <= 1) return any_type();
    const std::string key = "OpaqueExistential" + std::to_string(protocol_count);
    return named_type(key, kKindStruct, new Layout{LayoutKind::OpaqueExistential, {}, nullptr}, (kOpaqueExistentialWords + protocol_count) * kPointerSize, kPointerSize, kPointerExtraInhabitants);
}

extern "C" Metadata* rt_getFunctionTypeMetadata() {
    const std::lock_guard guard(g_types_lock);
    return thick_function_type();
}

extern "C" Metadata* rt_getMetatypeMetadata(void*) {
    const std::lock_guard guard(g_types_lock);
    return pod_type("metatype", kPointerSize);
}

extern "C" MetadataResponse rt_getTupleTypeMetadata2(size_t, Metadata* first, Metadata* second) {
    const std::lock_guard guard(g_types_lock);
    return {tuple_of({first, second}), 0};
}

extern "C" MetadataResponse rt_getTupleTypeMetadata3(size_t, Metadata* first, Metadata* second, Metadata* third) {
    const std::lock_guard guard(g_types_lock);
    return {tuple_of({first, second, third}), 0};
}

constexpr size_t kTupleCountMask = 0xffff;

extern "C" MetadataResponse rt_getTupleTypeMetadata(size_t, size_t flags, Metadata* const* elements) {
    const std::lock_guard guard(g_types_lock);
    return {tuple_of(std::vector<Metadata*>(elements, elements + (flags & kTupleCountMask))), 0};
}

// Lays out tuple fields for code that has only the fields' layouts, and returns their offsets.
void tuple_layout(TypeLayoutValue* out, size_t* offsets, const TypeLayoutValue* const* fields, size_t count) {
    size_t offset = 0;
    size_t mask = 0;
    uint32_t flags = 0;
    for (size_t i = 0; i < count; ++i) {
        const size_t field_mask = fields[i]->flags & 0xff;
        offset = (offset + field_mask) & ~field_mask;
        if (offsets != nullptr) offsets[i] = offset;
        offset += fields[i]->size;
        if (field_mask > mask) mask = field_mask;
        flags |= fields[i]->flags & kFlagNonPod;
    }
    *out = {offset, offset == 0 ? 1 : (offset + mask) & ~mask, static_cast<uint32_t>(mask) | flags, 0};
}

extern "C" size_t rt_getTupleTypeLayout2(TypeLayoutValue* out, const TypeLayoutValue* first, const TypeLayoutValue* second) {
    const TypeLayoutValue* fields[] = {first, second};
    size_t offsets[2];
    tuple_layout(out, offsets, fields, 2);
    return offsets[1];
}

extern "C" TupleOffsets rt_getTupleTypeLayout3(TypeLayoutValue* out, const TypeLayoutValue* first, const TypeLayoutValue* second, const TypeLayoutValue* third) {
    const TypeLayoutValue* fields[] = {first, second, third};
    size_t offsets[3];
    tuple_layout(out, offsets, fields, 3);
    return {offsets[1], offsets[2]};
}

extern "C" void rt_getTupleTypeLayout(TypeLayoutValue* out, uint32_t* offsets, size_t, size_t count, const TypeLayoutValue* const* fields) {
    std::vector<size_t> computed(count);
    tuple_layout(out, computed.data(), fields, count);
    if (offsets != nullptr) {
        for (size_t i = 0; i < count; ++i) offsets[i] = static_cast<uint32_t>(computed[i]);
    }
}

extern "C" MetadataResponse rt_checkMetadataState(size_t, void* metadata) { return {static_cast<Metadata*>(metadata), 0}; }

// How many bytes after the payload an enum needs to tell `tags` cases apart.
size_t enum_tag_bytes(size_t payload_size, uint64_t empty_cases, uint64_t payload_cases) {
    uint64_t tags = payload_cases;
    if (empty_cases > 0) {
        if (payload_size >= 4) {
            tags += 1;
        } else {
            const uint64_t per_tag = 1ULL << (payload_size * 8);
            tags += (empty_cases + per_tag - 1) / per_tag;
        }
    }
    if (tags <= 1) return 0;
    if (tags <= 0x100) return 1;
    if (tags <= 0x10000) return 2;
    return 4;
}

void install_enum_witnesses(void** metadata, const ValueWitnessTable* base, size_t size, size_t alignment_mask, uint32_t pod_flags, uint32_t inhabitants) {
    auto* updated = new ValueWitnessTable(*base);
    updated->size = size;
    updated->stride = size == 0 ? 1 : (size + alignment_mask) & ~alignment_mask;
    updated->flags = static_cast<uint32_t>(alignment_mask) | (pod_flags & kFlagNonPod);
    if (size > kInlineBufferSize || alignment_mask >= kPointerSize) updated->flags |= kFlagNonInline;
    updated->extra_inhabitants = inhabitants;
    metadata[-1] = updated;
}

extern "C" void rt_initEnumMetadataSingleCase(void** metadata, size_t, const void* payload_layout) {
    const auto* payload = witnesses_from_type_layout(payload_layout);
    install_enum_witnesses(metadata, static_cast<ValueWitnessTable*>(metadata[-1]), payload->size, payload->flags & 0xff, payload->flags, payload->extra_inhabitants);
}

extern "C" void rt_initEnumMetadataSinglePayload(void** metadata, size_t, const void* payload_layout, uint32_t empty_cases) {
    const auto* payload = witnesses_from_type_layout(payload_layout);
    const uint32_t available = payload->extra_inhabitants;
    if (empty_cases <= available) {
        install_enum_witnesses(metadata, static_cast<ValueWitnessTable*>(metadata[-1]), payload->size, payload->flags & 0xff, payload->flags, available - empty_cases);
        return;
    }
    const size_t tag = enum_tag_bytes(payload->size, empty_cases - available, 1);
    install_enum_witnesses(metadata, static_cast<ValueWitnessTable*>(metadata[-1]), payload->size + tag, payload->flags & 0xff, payload->flags, 0);
}

constexpr size_t kEnumPayloadCasesOffset = 20;

constexpr size_t kEnumEmptyCasesOffset = 24;

constexpr unsigned kEnumPayloadSizeOffsetShift = 24;

constexpr uint32_t kEnumPayloadCasesMask = 0xffffff;

extern "C" void rt_initEnumMetadataMultiPayload(void** metadata, size_t, size_t payload_count, const void* const* payload_layouts) {
    size_t largest = 0;
    size_t alignment_mask = 0;
    uint32_t flags = 0;
    for (size_t i = 0; i < payload_count; ++i) {
        const auto* payload = witnesses_from_type_layout(payload_layouts[i]);
        if (payload->size > largest) largest = payload->size;
        if ((payload->flags & 0xff) > alignment_mask) alignment_mask = payload->flags & 0xff;
        flags |= payload->flags;
    }
    const auto* descriptor = static_cast<const char*>(metadata[1]);
    uint32_t cases_and_offset, empty_cases;
    memcpy(&cases_and_offset, descriptor + kEnumPayloadCasesOffset, sizeof cases_and_offset);
    memcpy(&empty_cases, descriptor + kEnumEmptyCasesOffset, sizeof empty_cases);
    const size_t tag = enum_tag_bytes(largest, empty_cases, cases_and_offset & kEnumPayloadCasesMask);
    if (const uint32_t offset = cases_and_offset >> kEnumPayloadSizeOffsetShift) metadata[offset] = reinterpret_cast<void*>(largest);
    install_enum_witnesses(metadata, static_cast<ValueWitnessTable*>(metadata[-1]), largest + tag, alignment_mask, flags, 0);
}

constexpr size_t kClassInstanceSizeOffset = 48;

constexpr size_t kClassInstanceAlignOffset = 52;

constexpr size_t kRoIvarsOffset = 48;

constexpr uint32_t kRootInstanceSize = sizeof(HeapObject);

extern "C" void* rt_updateClassMetadata2(char* metadata, size_t, size_t field_count, const void* const* field_layouts, size_t* field_offsets) {
    uint32_t size = kRootInstanceSize;
    uint16_t alignment_mask = 15;
    void* super;
    memcpy(&super, metadata + kClassSuperclassOffset, sizeof super);
    if (super != nullptr && unresolved_symbol_at(reinterpret_cast<uintptr_t>(super)) == nullptr) {
        memcpy(&size, static_cast<char*>(super) + kClassInstanceSizeOffset, sizeof size);
        uint16_t inherited;
        memcpy(&inherited, static_cast<char*>(super) + kClassInstanceAlignOffset, sizeof inherited);
        if (inherited > alignment_mask) alignment_mask = inherited;
    }
    for (size_t i = 0; i < field_count; ++i) {
        const auto* field = witnesses_from_type_layout(field_layouts[i]);
        const size_t mask = field->flags & 0xff;
        size = static_cast<uint32_t>((size + mask) & ~mask);
        field_offsets[i] = size;
        size += static_cast<uint32_t>(field->size);
    }
    memcpy(metadata + kClassInstanceSizeOffset, &size, sizeof size);
    memcpy(metadata + kClassInstanceAlignOffset, &alignment_mask, sizeof alignment_mask);

    // The guest reads field offsets from globals that the ObjC runtime fills in through the ivar list.
    uintptr_t data;
    memcpy(&data, metadata + kClassDataOffset, sizeof data);
    auto* read_only = reinterpret_cast<char*>(data & ~static_cast<uintptr_t>(7));
    memcpy(read_only + kRoInstanceSizeOffset, &size, sizeof size);
    const char* ivars;
    memcpy(&ivars, read_only + kRoIvarsOffset, sizeof ivars);
    if (ivars != nullptr) {
        uint32_t entry_size, count;
        memcpy(&entry_size, ivars, sizeof entry_size);
        memcpy(&count, ivars + sizeof entry_size, sizeof count);
        const char* entries = ivars + 2 * sizeof(uint32_t);
        for (uint32_t i = 0; i < count && i < field_count; ++i) {
            uint64_t* offset_global;
            memcpy(&offset_global, entries + i * entry_size, sizeof offset_global);
            if (offset_global == nullptr) continue;
            // Offsets the compiler could prove are read-only constants, and must already agree.
            if (image_address_is_writable(reinterpret_cast<uintptr_t>(offset_global))) {
                *offset_global = field_offsets[i];
            } else if (*offset_global != field_offsets[i]) {
                engine_log("STOP: field %u is compiled at offset %llu but laid out at %zu", i, static_cast<unsigned long long>(*offset_global), field_offsets[i]);
                abort();
            }
        }
    }
    return nullptr;
}

// Generic class instantiation, per the Swift ABI: fixed metadata words, then immediate members, then extra data.
constexpr size_t kClassNegativeSizeOffset = 24;

constexpr size_t kClassPositiveSizeOffset = 28;

constexpr size_t kClassImmediateMembersOffset = 32;

constexpr size_t kClassSuperclassTypeOffset = 20;

constexpr size_t kPatternDestroyOffset = 12;

constexpr size_t kPatternIvarDestroyerOffset = 16;

constexpr size_t kPatternClassFlagsOffset = 20;

constexpr size_t kPatternRoOffset = 24;

constexpr size_t kPatternMetaclassOffset = 26;

constexpr size_t kPatternMetaclassRoOffset = 28;

constexpr size_t kClassPatternTrailerOffset = 32;

constexpr uint32_t kPatternHasImmediateMembers = 1u << 31;

constexpr size_t kPartialPatternBytes = 8;

constexpr size_t kClassFixedWords = 10;

constexpr size_t kClassFlagsWord = 5;

constexpr size_t kClassDataWord = 4;

constexpr size_t kClassDescriptionWord = 8;

constexpr size_t kClassIvarDestroyerWord = 9;

constexpr size_t kSwiftClassDataBits = 3;

void copy_partial_pattern(void** target, const char* partial) {
    uint16_t offset, size;
    memcpy(&offset, partial + 4, sizeof offset);
    memcpy(&size, partial + 6, sizeof size);
    memcpy(target + offset, relative_address(partial), size * sizeof(void*));
}

uint32_t read_u32_at(const char* at) {
    uint32_t value;
    memcpy(&value, at, sizeof value);
    return value;
}

extern "C" void* rt_allocateGenericClassMetadata(const char* descriptor, void* const* arguments, const char* pattern) {
    const uint32_t negative = read_u32_at(descriptor + kClassNegativeSizeOffset);
    const uint32_t positive = read_u32_at(descriptor + kClassPositiveSizeOffset);
    const uint32_t immediate = read_u32_at(descriptor + kClassImmediateMembersOffset);
    const uint32_t flags = read_u32_at(pattern + kPatternFlagsOffset);
    const char* trailer = pattern + kClassPatternTrailerOffset;
    const char* immediate_pattern = nullptr;
    if (flags & kPatternHasImmediateMembers) {
        immediate_pattern = trailer;
        trailer += kPartialPatternBytes;
    }
    const char* extra_pattern = flags & kPatternHasExtraData ? trailer : nullptr;
    size_t extra_words = 0;
    if (extra_pattern != nullptr) {
        uint16_t offset, size;
        memcpy(&offset, extra_pattern + 4, sizeof offset);
        memcpy(&size, extra_pattern + 6, sizeof size);
        extra_words = offset + size;
    }
    auto* block = static_cast<void**>(calloc(negative + positive + extra_words, sizeof(void*)));
    void** metadata = block + negative;
    void** extra = metadata + positive;
    metadata[-2] = const_cast<char*>(relative_address(pattern + kPatternDestroyOffset));
    metadata[-1] = const_cast<void*>(static_cast<const void*>(witnesses_of(native_type())));
    metadata[kClassDescriptionWord] = const_cast<char*>(descriptor);
    metadata[kClassIvarDestroyerWord] = const_cast<char*>(relative_address(pattern + kPatternIvarDestroyerOffset));
    memcpy(&metadata[kClassFlagsWord], pattern + kPatternClassFlagsOffset, sizeof(uint32_t));
    const uint32_t class_size = (negative + positive) * sizeof(void*);
    const uint32_t address_point = negative * sizeof(void*);
    memcpy(static_cast<void*>(metadata + 7), &class_size, sizeof class_size);
    memcpy(reinterpret_cast<char*>(metadata + 7) + sizeof class_size, &address_point, sizeof address_point);
    void** members = metadata + positive - immediate;
    memcpy(members, arguments, key_argument_count(descriptor) * sizeof(void*));
    if (immediate_pattern != nullptr) copy_partial_pattern(members, immediate_pattern);
    if (extra_pattern != nullptr) {
        copy_partial_pattern(extra, extra_pattern);
        uint16_t ro, meta, meta_ro;
        memcpy(&ro, pattern + kPatternRoOffset, sizeof ro);
        memcpy(&meta, pattern + kPatternMetaclassOffset, sizeof meta);
        memcpy(&meta_ro, pattern + kPatternMetaclassRoOffset, sizeof meta_ro);
        // These three offsets are in words from the start of the extra data.
        metadata[kClassDataWord] = reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(extra + ro) | kSwiftClassDataBits);
        auto** metaclass = reinterpret_cast<void**>(extra + meta);
        metaclass[kClassDataWord] = reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(extra + meta_ro) | kSwiftClassDataBits);
        metadata[0] = metaclass;
    }
    return metadata;
}

constexpr uint32_t kClassHasVTable = 0x80000000;

constexpr uint32_t kClassHasOverrides = 0x40000000;

constexpr size_t kVTableHeaderBytes = 8;

constexpr size_t kMethodDescriptorBytes = 8;

constexpr size_t kMethodImplementationOffset = 4;

constexpr size_t kOverrideEntryBytes = 12;

constexpr size_t kResilientSuperclassBytes = 4;

constexpr uint32_t kMetadataInitializationShift = 16;

constexpr uint32_t kMetadataInitializationMask = 3;

constexpr uint32_t kSingletonMetadataInitialization = 1;

constexpr size_t kSingletonInitializationBytes = 12;

const char* indirect_relative(const char* field) {
    int32_t offset;
    memcpy(&offset, field, sizeof offset);
    if (offset == 0) return nullptr;
    const char* target = field + (offset & ~1);
    return offset & 1 ? *reinterpret_cast<const char* const*>(target) : target;
}

// Where a class descriptor's trailing vtable starts: after its generic requirements.
const char* class_vtable_header(const char* descriptor) {
    const char* cursor = descriptor + kClassGenericHeaderOffset;
    if (read_u32_at(descriptor) & kDescriptorGeneric) {
        uint16_t params, requirements;
        memcpy(&params, cursor + kNumParamsOffset, sizeof params);
        memcpy(&requirements, cursor + kNumRequirementsOffset, sizeof requirements);
        cursor += kGenericHeaderBytes + ((params + 3u) & ~3u) + requirements * kRequirementBytes;
    }
    const uint32_t flags = read_u32_at(descriptor);
    if (flags & kClassHasResilientSuperclass) cursor += kResilientSuperclassBytes;
    if (((flags >> kMetadataInitializationShift) & kMetadataInitializationMask) == kSingletonMetadataInitialization) cursor += kSingletonInitializationBytes;
    return cursor;
}

// Fills the class's own slots from its descriptor, then applies the overrides of inherited ones.
void install_class_vtable(void** metadata, const char* descriptor) {
    const uint32_t flags = read_u32_at(descriptor);
    if (!(flags & (kClassHasVTable | kClassHasOverrides))) return;
    const char* cursor = class_vtable_header(descriptor);
    if (flags & kClassHasVTable) {
        const uint32_t offset = read_u32_at(cursor);
        const uint32_t size = read_u32_at(cursor + 4);
        cursor += kVTableHeaderBytes;
        for (uint32_t i = 0; i < size; ++i) {
            const char* implementation = relative_address(cursor + kMethodImplementationOffset);
            metadata[offset + i] = const_cast<char*>(implementation);
            cursor += kMethodDescriptorBytes;
        }
    }
    if (!(flags & kClassHasOverrides)) return;
    const uint32_t count = read_u32_at(cursor);
    cursor += sizeof(uint32_t);
    for (uint32_t i = 0; i < count; ++i, cursor += kOverrideEntryBytes) {
        const char* base = indirect_relative(cursor);
        const char* method = indirect_relative(cursor + 4);
        if (base == nullptr || method == nullptr || unresolved_symbol_at(reinterpret_cast<uintptr_t>(base)) != nullptr) continue;
        const char* header = class_vtable_header(base);
        const uint32_t base_offset = read_u32_at(header);
        const size_t index = static_cast<size_t>(method - (header + kVTableHeaderBytes)) / kMethodDescriptorBytes;
        metadata[base_offset + index] = const_cast<char*>(relative_address(cursor + 8));
    }
}

// Completes a generic class: finds its superclass, inherits that class's slots, then lays out its fields.
extern "C" void* rt_initClassMetadata2(char* metadata, size_t flags, size_t field_count, const void* const* field_layouts, size_t* field_offsets) {
    auto** words = reinterpret_cast<void**>(metadata);
    const auto* descriptor = static_cast<const char*>(words[kClassDescriptionWord]);
    const uint32_t positive = read_u32_at(descriptor + kClassPositiveSizeOffset);
    const uint32_t immediate = read_u32_at(descriptor + kClassImmediateMembersOffset);
    void* const* arguments = words + positive - immediate;
    if (const char* name = relative_address(descriptor + kClassSuperclassTypeOffset)) {
        const size_t length = mangled_name_length(name);
        auto** super = static_cast<void**>(rt_getTypeByMangledNameInContext2(name, length, descriptor, arguments));
        words[1] = super;
        if (super != nullptr && unresolved_symbol_at(reinterpret_cast<uintptr_t>(super)) == nullptr && super[kClassDescriptionWord] != nullptr) {
            const uint32_t super_positive = read_u32_at(static_cast<const char*>(super[kClassDescriptionWord]) + kClassPositiveSizeOffset);
            memcpy(words + kClassFixedWords, super + kClassFixedWords, (super_positive - kClassFixedWords) * sizeof(void*));
        }
    }
    install_class_vtable(words, descriptor);
    return rt_updateClassMetadata2(metadata, flags, field_count, field_layouts, field_offsets);
}

}  // namespace type_system
