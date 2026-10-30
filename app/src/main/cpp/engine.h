#pragma once
#include <string>

// Runs the app bundle's main executable. Never returns control to a crashed guest: faults end the process.
int run_app_bundle(const std::string& bundle_dir, const std::string& executable, const std::string& log_path);

void engine_log(const char* format, ...) __attribute__((format(printf, 1, 2)));

// The running app's bundle directory.
const std::string& engine_bundle_dir();
