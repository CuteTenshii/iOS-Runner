#pragma once
#include <string_view>

void* concurrency_symbol(std::string_view name);

// Runs queued tasks on the guest thread until none are ready.
void concurrency_drain();
