#include "type_system_internal.h"

using namespace type_system;

namespace type_system {

// Accessors take generic arguments in registers, so each needs its arity; anything unlisted is non-generic.
size_t accessor_generic_arity(std::string_view symbol) {
    static const std::unordered_map<std::string_view, size_t> arities = {
        {"$s7SwiftUI5StateVMa", 1},
        {"$s7SwiftUI7BindingVMa", 1},
        {"$s7SwiftUI12FetchRequestVMa", 1},
        {"$s7SwiftUI5GroupVMa", 1},
        {"$s7SwiftUI4FormVMa", 1},
        {"$s7SwiftUI10ScrollViewVMa", 1},
        {"$s7SwiftUI6ZStackVMa", 1},
        {"$s7SwiftUI9LazyVGridVMa", 1},
        {"$s7SwiftUI28UIApplicationDelegateAdaptorVMa", 1},
        {"$s7SwiftUI4ListVMa", 2},
        {"$s7SwiftUI4MenuVMa", 2},
        {"$s7SwiftUI15NavigationStackVMa", 2},
        {"$s7SwiftUI11ToolbarItemVMa", 2},
        {"$s7SwiftUI19_ConditionalContentVMa", 2},
        {"$s7SwiftUI19_ConditionalContentV7StorageOMa", 2},
        {"$s7SwiftUI7SectionVMa", 3},
        {"$sScCMa", 2},
        {"$s7SwiftUI20_ValueActionModifierVMa", 1},
        {"$s7Combine9PublishedVMa", 1},
        {"$s7Combine8DeferredVMa", 1},
        {"$s7Combine11SubscribersO10CompletionOMa", 1},
        {"$s7Combine12AnyPublisherVMa", 2},
        {"$s7Combine18PassthroughSubjectCMa", 2},
        {"$s7Combine6FutureCMa", 2},
        {"$sSaMa", 1},
        {"$sShMa", 1},
        {"$sSDMa", 2},
        {"$ss6ResultOMa", 2},
    };
    if (const auto found = arities.find(symbol); found != arities.end()) return found->second;
    if (symbol == "$s7SwiftUI15ModifiedContentVMa") return 2;
    if (symbol == "$s7SwiftUI8BindableVMa" || symbol == "$s7SwiftUI9LazyStateVMa" || symbol == "$sSqMa" ||
        symbol == "$s7SwiftUI11EnvironmentV7ContentOMa" ||
        symbol == "$s7SwiftUI16TagValueTraitKeyVMa" || symbol == "$s7SwiftUI21_TraitWritingModifierVMa" ||
        symbol == "$s7SwiftUI24_TagTraitWritingModifierVMa") {
        return 1;
    }
    return 0;
}

extern "C" MetadataResponse rt_foreign_access(size_t, void* first, void* second, void* third, const char* symbol) {
    const std::lock_guard guard(g_types_lock);
    const std::string_view name(symbol);
    if (name == "$sSqMa") return {optional_of(static_cast<Metadata*>(first)), 0};
    if (name == "$s10Foundation4DateVMa") return {pod_type("Date", 8), 0};
    const size_t arity = accessor_generic_arity(name);
    std::vector<void*> arguments;
    if (arity > 0) arguments.push_back(first);
    if (arity > 1) arguments.push_back(second);
    if (arity > 2) arguments.push_back(third);
    if (name == "$s7SwiftUI15ModifiedContentVMa") return {foreign_nominal("_$s7SwiftUI15ModifiedContentVMn", arguments), 0};
    std::string descriptor = std::string("_") + symbol;
    descriptor.replace(descriptor.size() - 2, 2, "Mn");
    return {foreign_nominal(descriptor, arguments), 0};
}

extern "C" void rt_foreign_access_entry();

__asm__(
    ".text\n"
    ".globl rt_foreign_access_entry\n"
    "rt_foreign_access_entry:\n"
    "  mov x4, x16\n"
    "  b rt_foreign_access\n");

void* make_accessor_stub(const std::string& symbol) {
    return make_code_stub(reinterpret_cast<void*>(rt_foreign_access_entry), strdup(symbol.c_str()));
}

}  // namespace type_system

void* type_system_symbol(std::string_view name) {
    if (name == "swift_getTypeByMangledNameInContext2") return reinterpret_cast<void*>(rt_getTypeByMangledNameInContext2);
    if (name == "swift_getTypeByMangledNameInContext") return reinterpret_cast<void*>(rt_getTypeByMangledNameInContext2);
    if (name == "swift_getTypeByMangledNameInContextInMetadataState") return reinterpret_cast<void*>(rt_getTypeByMangledNameInContextInMetadataState2);
    if (name == "swift_getTypeByMangledNameInContextInMetadataState2") return reinterpret_cast<void*>(rt_getTypeByMangledNameInContextInMetadataState2);
    if (name == "swift_getGenericMetadata") return reinterpret_cast<void*>(rt_getGenericMetadata);
    if (name == "swift_allocateGenericValueMetadata") return reinterpret_cast<void*>(rt_allocateGenericValueMetadata);
    if (name == "swift_getOpaqueTypeMetadata") return reinterpret_cast<void*>(rt_getOpaqueTypeMetadata);
    if (name == "swift_allocateGenericClassMetadata") return reinterpret_cast<void*>(rt_allocateGenericClassMetadata);
    if (name == "swift_initClassMetadata2") return reinterpret_cast<void*>(rt_initClassMetadata2);
    if (name == "swift_getExistentialTypeMetadata") return reinterpret_cast<void*>(rt_getExistentialTypeMetadata);
    if (name == "swift_getFunctionTypeMetadata") return reinterpret_cast<void*>(rt_getFunctionTypeMetadata);
    if (name == "swift_getFunctionTypeMetadata0") return reinterpret_cast<void*>(rt_getFunctionTypeMetadata);
    if (name == "swift_getFunctionTypeMetadata1") return reinterpret_cast<void*>(rt_getFunctionTypeMetadata);
    if (name == "swift_getFunctionTypeMetadata2") return reinterpret_cast<void*>(rt_getFunctionTypeMetadata);
    if (name == "swift_getMetatypeMetadata") return reinterpret_cast<void*>(rt_getMetatypeMetadata);
    if (name == "swift_getTupleTypeMetadata") return reinterpret_cast<void*>(rt_getTupleTypeMetadata);
    if (name == "swift_getTupleTypeMetadata2") return reinterpret_cast<void*>(rt_getTupleTypeMetadata2);
    if (name == "swift_getTupleTypeMetadata3") return reinterpret_cast<void*>(rt_getTupleTypeMetadata3);
    // Imported C types carry complete metadata in the image, and uniqueness only matters across images.
    if (name == "swift_getForeignTypeMetadata") return reinterpret_cast<void*>(+[](size_t, void* metadata) -> MetadataResponse { return {metadata, 0}; });
    if (name == "swift_getTupleTypeLayout") return reinterpret_cast<void*>(rt_getTupleTypeLayout);
    if (name == "swift_getTupleTypeLayout2") return reinterpret_cast<void*>(rt_getTupleTypeLayout2);
    if (name == "swift_getTupleTypeLayout3") return reinterpret_cast<void*>(rt_getTupleTypeLayout3);
    if (name == "swift_checkMetadataState") return reinterpret_cast<void*>(rt_checkMetadataState);
    if (name == "swift_initEnumMetadataSingleCase") return reinterpret_cast<void*>(rt_initEnumMetadataSingleCase);
    if (name == "swift_initEnumMetadataSinglePayload") return reinterpret_cast<void*>(rt_initEnumMetadataSinglePayload);
    if (name == "swift_initEnumMetadataMultiPayload") return reinterpret_cast<void*>(rt_initEnumMetadataMultiPayload);
    if (name == "swift_getObjCClassMetadata") return reinterpret_cast<void*>(rt_getObjCClassMetadata);
    if (name == "swift_getObjCClassFromMetadata") return reinterpret_cast<void*>(rt_getObjCClassFromMetadata);
    if (name == "swift_getSingletonMetadata") return reinterpret_cast<void*>(rt_getSingletonMetadata);
    if (name == "swift_initStructMetadata") return reinterpret_cast<void*>(rt_initStructMetadata);
    if (name == "swift_updateClassMetadata2") return reinterpret_cast<void*>(rt_updateClassMetadata2);
    if (name.size() > 3 && name.substr(0, 2) == "$s" && name.back() == 'N') {
        const std::lock_guard guard(g_types_lock);
        // Unlike nominal types, these two symbols name the full metadata, one word before its address point.
        if (name == "$sytN") return reinterpret_cast<char*>(pod_type("yt", 0)) - sizeof(void*);
        if (name == "$sypN") return reinterpret_cast<char*>(any_type()) - sizeof(void*);
        if (name == "$sSSN" || name == "$sSS8UTF8ViewVN" || name == "$ss11_StringGutsVN" || name == "$ss26DefaultStringInterpolationVN") return string_type();
        if (name == "$sSiN") return pod_type("Si", 8);
        if (name == "$sSbN") return named_type("Sb", kKindStruct, new Layout{LayoutKind::Pod, {}, nullptr}, 1, 1, 254);
        if (name == "$sSJN") return two_word_type("SJ", word_type(), bridge_word_type());
        if (name == "$sSsN" || name == "$sSs8UTF8ViewVN") return named_type("Ss", kKindStruct, new Layout{LayoutKind::Aggregate, {{word_type(), 0}, {word_type(), 8}, {word_type(), 16}, {bridge_word_type(), 24}}, nullptr}, 32, 8, kPointerExtraInhabitants);
        if (name == "$ss5NeverON") return pod_type("Never", 0);
        if (name == "$s7SwiftUI22HierarchicalShapeStyleVN") return pod_type(std::string(name), 4);
        if (name == "$s10Foundation4DateVN") return pod_type("Date", 8);
        if (name == "$s10Foundation4DataVN" || name == "$s10Foundation4DataV15_RepresentationON") return two_word_type("Data", word_type(), bridge_word_type());
        std::string descriptor = "_" + std::string(name);
        descriptor.back() = 'M';
        descriptor += 'n';
        return foreign_nominal(descriptor, {});
    }
    if (name.size() > 4 && name.substr(0, 2) == "$s" && name.substr(name.size() - 2) == "Ma") return make_accessor_stub(std::string(name));
    return nullptr;
}

const char* foreign_type_name(const void* metadata) {
    const std::lock_guard guard(g_types_lock);
    const auto found = g_foreign_names.find(metadata);
    return found == g_foreign_names.end() ? nullptr : found->second;
}

void* resolve_mangled_type(const char* name, void* const* generics) {
    const size_t length = mangled_name_length(name);
    return rt_getTypeByMangledNameInContext2(name, length, nullptr, generics);
}

void* storage_class_metadata(const char* symbol, void* const* arguments, size_t count) {
    const std::lock_guard guard(g_types_lock);
    return foreign_nominal(symbol, std::vector<void*>(arguments, arguments + count));
}

void* make_code_stub(void* entry, const void* data) {
    static char* pool = nullptr;
    static size_t used = 0;
    if (pool == nullptr) {
        pool = static_cast<char*>(mmap(nullptr, kAccessorPoolSize, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
        if (pool == MAP_FAILED) {
            pool = nullptr;
            return nullptr;
        }
    }
    if (used + kAccessorStubSize > kAccessorPoolSize) return nullptr;
    auto* stub = reinterpret_cast<uint32_t*>(pool + used);
    used += kAccessorStubSize;
    stub[0] = 0x58000090;  // ldr x16, #16
    stub[1] = 0x580000b1;  // ldr x17, #20
    stub[2] = 0xd61f0220;  // br x17
    stub[3] = 0xd503201f;  // nop, keeps the literals 8-byte aligned
    memcpy(stub + 4, &data, sizeof data);
    memcpy(stub + 6, &entry, sizeof entry);
    __builtin___clear_cache(reinterpret_cast<char*>(stub), reinterpret_cast<char*>(stub + 8));
    return stub;
}

void* lazy_state_metadata(void* value_type) {
    const std::lock_guard guard(g_types_lock);
    return lazy_state_of(static_cast<Metadata*>(value_type));
}

std::vector<TypeField> type_fields(const void* metadata) {
    std::vector<TypeField> result;
    const std::lock_guard guard(g_types_lock);
    if (g_foreign_names.find(metadata) == g_foreign_names.end() && static_cast<const Metadata*>(metadata)->kind != kKindTuple) return result;
    const auto* layout = layout_of(metadata);
    if (layout == nullptr) return result;
    for (const auto& field : layout->fields) result.push_back({field.type, field.offset});
    return result;
}

bool conditional_active(const void* metadata, const void* value, TypeField& out) {
    const auto* layout = layout_of(metadata);
    if (g_foreign_names.find(metadata) == g_foreign_names.end() || layout->kind != LayoutKind::Conditional) return false;
    out = {layout->fields[state_storage_tag(value, metadata)].type, 0};
    return true;
}

bool optional_payload(const void* metadata, const void* value, TypeField& out) {
    if (static_cast<const Metadata*>(metadata)->kind != kKindOptional) return false;
    auto* payload = layout_of(metadata)->payload;
    if (witnesses_of(payload)->get_enum_tag_single_payload(value, 1, payload) != 0)
        out = {nullptr, 0};
    else
        out = {payload, 0};
    return true;
}

void* array_element_type(const void* metadata) {
    const std::lock_guard guard(g_types_lock);
    const auto found = g_array_elements.find(metadata);
    return found == g_array_elements.end() ? nullptr : found->second;
}

void* optional_metadata(void* payload) {
    const std::lock_guard guard(g_types_lock);
    return optional_of(static_cast<Metadata*>(payload));
}

void* any_object_metadata() {
    const std::lock_guard guard(g_types_lock);
    return native_type();
}

namespace type_system {

constexpr size_t kCastUnconditional = 1;

constexpr size_t kCastTakeOnSuccess = 2;

constexpr size_t kCastDestroyOnFailure = 4;

constexpr size_t kAnyTypeWord = 3;

}  // namespace type_system

// `as? [T]` and `as? [String: T]` convert element by element, failing as a whole if one element does not fit.
bool cast_collection(void* destination, void* value, void* contained, void* target_type) {
    uint64_t word;
    void* target_element = array_element_type(target_type);
    void* source_element = array_element_type(contained);
    if (target_element != nullptr && source_element != nullptr) {
        memcpy(&word, value, sizeof word);
        char* from;
        const size_t count = array_storage_contents(reinterpret_cast<void*>(word & kBridgePointerMask), source_element, &from);
        char* to;
        void* storage = make_array_storage(target_element, count, &to);
        const size_t from_stride = witnesses_of(source_element)->stride;
        const size_t to_stride = witnesses_of(target_element)->stride;
        for (size_t i = 0; i < count; ++i) {
            if (!swift_cast(to + i * to_stride, from + i * from_stride, source_element, target_element, 0)) {
                for (size_t done = 0; done < i; ++done) witnesses_of(target_element)->destroy(to + done * to_stride, target_element);
                free(storage);
                return false;
            }
        }
        memcpy(destination, &storage, sizeof storage);
        return true;
    }
    void* target_value = dictionary_value_type(target_type);
    void* source_value = dictionary_value_type(contained);
    if (target_value == nullptr || source_value == nullptr) return false;
    memcpy(&word, value, sizeof word);
    const size_t stride = witnesses_of(target_value)->stride;
    std::vector<std::pair<std::string, size_t>> converted;
    std::vector<char> bytes;
    bool failed = false;
    visit_string_dictionary(reinterpret_cast<void*>(word & kBridgePointerMask), [&](const std::string& key, void* member) {
        if (failed) return;
        const size_t at = bytes.size();
        bytes.resize(at + stride);
        std::vector<char> slot(stride + 8);
        if (!swift_cast(slot.data(), member, source_value, target_value, 0)) {
            failed = true;
            return;
        }
        memcpy(bytes.data() + at, slot.data(), stride);
        converted.emplace_back(key, at);
    });
    if (failed) {
        for (const auto& entry : converted) witnesses_of(target_value)->destroy(bytes.data() + entry.second, target_value);
        return false;
    }
    std::vector<std::pair<std::string, const void*>> entries;
    for (const auto& [key, at] : converted) entries.emplace_back(key, bytes.data() + at);
    void* storage = make_string_dictionary(target_value, entries);
    memcpy(destination, &storage, sizeof storage);
    return true;
}

bool swift_cast(void* destination, void* source, void* source_type, void* target_type, size_t flags) {
    const auto* source_metadata = static_cast<Metadata*>(source_type);
    void* value = source;
    void* contained = source_type;
    // Open an Any: its value sits inline, or in a box for large types.
    if (source_metadata->kind == kKindExistential && layout_of(source_type)->kind == LayoutKind::OpaqueExistential) {
        auto** words = static_cast<void**>(source);
        contained = words[kAnyTypeWord];
        if (contained == nullptr) goto fail;
        if (witnesses_of(contained)->flags & kFlagNonInline) {
            const size_t mask = witnesses_of(contained)->flags & 0xff;
            value = static_cast<char*>(words[0]) + ((sizeof(HeapObject) + mask) & ~mask);
        }
    }
    // An Optional target holds the value with no case to store, when it uses spare bits.
    if (static_cast<Metadata*>(target_type)->kind == kKindOptional) {
        Metadata* payload = layout_of(target_type)->payload;
        if (swift_cast(destination, value, contained, payload, 0)) {
            witnesses_of(payload)->store_enum_tag_single_payload(destination, 0, 1, payload);
            if (flags & kCastTakeOnSuccess) witnesses_of(source_type)->destroy(source, source_type);
            return true;
        }
        goto fail;
    }
    if (contained == target_type) {
        witnesses_of(target_type)->initialize_with_copy(destination, value, target_type);
        if (flags & kCastTakeOnSuccess) witnesses_of(source_type)->destroy(source, source_type);
        return true;
    }
    if (static_cast<Metadata*>(target_type)->kind == kKindExistential && layout_of(target_type)->kind == LayoutKind::OpaqueExistential && witnesses_of(target_type)->size == 4 * kPointerSize) {
        auto** words = static_cast<void**>(destination);
        memset(words, 0, 4 * kPointerSize);
        if (witnesses_of(contained)->flags & kFlagNonInline) {
            engine_log("STOP: cast of a large value into Any");
            abort();
        }
        witnesses_of(contained)->initialize_with_copy(destination, value, contained);
        words[kAnyTypeWord] = contained;
        if (flags & kCastTakeOnSuccess) witnesses_of(source_type)->destroy(source, source_type);
        return true;
    }
    if (cast_collection(destination, value, contained, target_type)) {
        if (flags & kCastTakeOnSuccess) witnesses_of(source_type)->destroy(source, source_type);
        return true;
    }
fail:
    if (flags & kCastUnconditional) {
        engine_log("STOP: an unconditional cast failed");
        abort();
    }
    if (flags & kCastDestroyOnFailure) witnesses_of(source_type)->destroy(source, source_type);
    return false;
}

void* date_metadata() {
    const std::lock_guard guard(g_types_lock);
    return pod_type("Date", 8);
}

void* dictionary_value_type(const void* metadata) {
    const std::lock_guard guard(g_types_lock);
    const auto found = g_dictionary_values.find(metadata);
    return found == g_dictionary_values.end() ? nullptr : found->second;
}
