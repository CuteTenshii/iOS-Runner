#pragma once
#include <string>
#include <string_view>

// Objective-C message dispatch for the guest, and the framework classes implemented by this runtime.
void* objc_runtime_symbol(std::string_view name);

struct MachImage;
void objc_attach_image(const MachImage& image);

// Runs the +load methods of the image's classes and categories.
void objc_run_load_methods(const MachImage& image);

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

// Whether the receiver, a guest or framework object or class, has an implementation of the selector.
bool objc_responds(void* receiver, const char* selector);

// The implementation a message to `receiver` would run.
const void* objc_method_for(void* receiver, const char* selector);

// A class's name as NSStringFromClass gives it: Swift classes read "Module.Name".
std::string objc_class_name(const void* cls);

// The guest or framework class with this name, or null if there is none.
void* objc_class_named(const std::string& name);

// True if the address is a class this runtime knows.
bool objc_is_class_address(const void* address);

// From now on unknown UIKit messages go to `resolve` before being answered with nil.
void objc_set_uikit_fallback(const void* (*resolve)(const char* selector));
void objc_set_uikit_active();

// [cls alloc], without init.
void* objc_allocate(void* cls);

// The framework class at the root of an object's class chain, or empty.
std::string objc_framework_root_of(const void* object);

// Stores into a guest object's instance variable of this name, retaining the value. False if it has none.
bool objc_set_ivar_named(void* object, const std::string& name, void* value);
