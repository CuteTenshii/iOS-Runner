#pragma once
#include <cstdint>
#include <string_view>

// Dispatch on the one guest thread: every queue is serial work drained by the render loop.
void* dispatch_symbol(std::string_view name);

// Runs blocks that are due; returns true if any ran.
bool dispatch_run_ready();

// When the earliest delayed block is due, on CLOCK_MONOTONIC in nanoseconds, or 0 if none waits.
int64_t dispatch_next_deadline();

// Queues a function for the guest thread, to run outside whatever guest code is executing now.
void dispatch_enqueue(void (*function)(void*), void* context);
