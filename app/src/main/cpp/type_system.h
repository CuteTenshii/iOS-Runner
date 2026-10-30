#pragma once
#include <cstddef>
#include <string_view>
#include <vector>

// Runtime type lookup: mangled-name resolution and metadata for standard library and framework types.
void* type_system_symbol(std::string_view name);

// Names the framework type behind runtime metadata, or nullptr for a type defined by the app.
const char* foreign_type_name(const void* metadata);

// Resolves a mangled type name, as swift_getTypeByMangledNameInContext2 does.
void* resolve_mangled_type(const char* name, void* const* generics);

// Metadata of a generic standard library storage class, whose prefix holds its destroyer.
void* storage_class_metadata(const char* symbol, void* const* arguments, size_t count);

// Generates executable code that puts `data` in x16 and jumps to `entry`, preserving all argument registers.
void* make_code_stub(void* entry, const void* data);

// Metadata of LazyState<T>, which the app stores inline: a Storage<T> followed by a location word.
void* lazy_state_metadata(void* value_type);

struct TypeField {
    void* type;
    size_t offset;
};

// Stored fields of a tuple or frozen framework type; empty for types this runtime keeps opaque.
std::vector<TypeField> type_fields(const void* metadata);

// Which branch an `if` view currently holds. Returns false if the type is not conditional.
bool conditional_active(const void* metadata, const void* value, TypeField& out);

// An Optional's payload type, or a null type when it is nil. Returns false if the type is not optional.
bool optional_payload(const void* metadata, const void* value, TypeField& out);

// The element type of an Array's metadata, or nullptr for any other type.
void* array_element_type(const void* metadata);

// Metadata of Optional<T>.
void* optional_metadata(void* payload);

// Metadata of Foundation.Date.
void* date_metadata();

// Metadata for a plain retained object reference, as stored in an Any.
void* any_object_metadata();

// swift_dynamicCast: `as?` between Any, its contained types and Optional.
bool swift_cast(void* destination, void* source, void* source_type, void* target_type, size_t flags);

// The value type of a [String: V] dictionary's metadata, or nullptr for any other type.
void* dictionary_value_type(const void* metadata);
