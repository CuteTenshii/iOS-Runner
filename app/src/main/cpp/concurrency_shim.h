#pragma once
#include <cstdint>
#include <string_view>

void* concurrency_symbol(std::string_view name);

// Runs queued tasks on the guest thread until none are ready.
void concurrency_drain();

// When the earliest sleeping task wakes, on CLOCK_MONOTONIC in nanoseconds, or 0 if none sleeps.
int64_t concurrency_next_deadline();

// Queues every sleeping task whose deadline has passed.
void concurrency_fire_timers();

// Queues an async closure, given its async function pointer and context, to start on the guest thread.
void concurrency_start(const void* async_function, void* context);

// Resumes a suspended async call whose callee returned to the scheduler: `context` is the callee's own context.
void concurrency_resume_call(void* context, void* error);
