#pragma once
#include <string>
#include <string_view>

// Objective-C message dispatch for the guest, and the framework classes implemented by this runtime.
void* objc_runtime_symbol(std::string_view name);

struct MachImage;
void objc_attach_image(const MachImage& image);

// Registers a framework method under its full name, for example "+[NSBundle mainBundle]".
void objc_register_method(const char* description, const void* implementation);

// Supplies methods the class generates at runtime, asked after its registered methods.
void objc_register_fallback(const char* class_name, const void* (*resolve)(const char* selector));

// The name of a framework class, or empty for a class the guest defines.
std::string objc_framework_class_name(const void* cls);

// A new instance of a framework class, with room for its own state after the header.
void* objc_framework_object(const char* class_name);

// Creates a class object for framework instances; its address goes in each instance's first word.
// `destroyer` runs when an instance's last reference is released, and must free it.
void* objc_native_class(const char* name, void (*destroyer)(void*));

// True if an object's class is, or names the same framework class as, `cls`.
bool objc_is_framework_class(const void* object, const void* cls);

// Sends a one-argument message to a guest object.
void objc_send(void* receiver, const char* selector, void* argument);

// [[cls alloc] init] for a class the guest defines.
void* objc_alloc_init(void* cls);
