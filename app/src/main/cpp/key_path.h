#pragma once

// Interprets a key path from swift_getKeyPath. `root` points at the root value, as Swift's accessors expect.
void key_path_get(const void* key_path, void* root, void* out, void* value_type);
void key_path_set(const void* key_path, void* root, void* new_value, void* value_type);

// Creates the object swift_getKeyPath returns.
void* key_path_create(const void* pattern, const void* arguments);

// The type of the value this key path reads, as recorded in its pattern.
void* key_path_value_type(const void* key_path);

struct KeyPathModification {
    const void* resume;
    void* value;
};

// swift_modifyAtReferenceWritableKeyPath: yields a temporary that resuming writes back.
__attribute__((swiftcall)) KeyPathModification key_path_begin_modify(void** buffer, void* root, const void* key_path);
