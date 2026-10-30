#pragma once
#include <string_view>

// Objective-C message dispatch for the guest, and the framework classes implemented by this runtime.
void* objc_runtime_symbol(std::string_view name);

// Registers a framework method under its full name, for example "+[NSBundle mainBundle]".
void objc_register_method(const char* description, const void* implementation);

// Creates a class object for framework instances; its address goes in each instance's first word.
void* objc_native_class(const char* name);
