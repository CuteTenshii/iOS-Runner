#pragma once
#include <string_view>
#include <unordered_map>

// Darwin's libSystem, on top of bionic: what differs is flag values, struct layouts and the variadic convention.
void* darwin_symbol(std::string_view name);

using DarwinSymbolTable = std::unordered_map<std::string_view, void*>;
void add_darwin_libc_symbols(DarwinSymbolTable& table);
void add_darwin_io_symbols(DarwinSymbolTable& table);
void add_darwin_thread_symbols(DarwinSymbolTable& table);
void add_darwin_system_symbols(DarwinSymbolTable& table);

// pthread_main_np answers true on the thread that calls this.
void darwin_mark_main_thread();
