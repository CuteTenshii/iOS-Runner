#include <malloc.h>

#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <thread>

#include "engine.h"
#include "foundation_shim.h"

// Static Bionic aborts at startup unless the TLS segment is 64-byte aligned.
alignas(64) thread_local volatile char g_tls_alignment[64];

namespace {

int g_probe_phase = 0;
bool g_tapped = false;

// Answers the app's page probes as a site's profile, menu and confirmation would appear in turn.
std::string probe_result() {
    const auto flag = [](bool value) { return value ? "true" : "false"; };
    const int phase = g_probe_phase;
    return std::string("{\"shows\":true,\"blocked\":") + flag(phase >= 3) + ",\"unavailable\":false,\"loggedOut\":false,\"overflow\":" + flag(phase == 0) +
           ",\"block\":" + flag(phase == 1) + ",\"confirm\":" + flag(phase == 2) + ",\"unblock\":false,\"gate\":false}";
}

// IOSRUNNER_DRIVE_SITE=<directory>: answers requests with the file named after the URL's last path component.
HttpResult serve_from_directory(const std::string& url, const std::string&, const std::string&, const std::string&) {
    std::ifstream file(std::string(getenv("IOSRUNNER_DRIVE_SITE")) + "/" + url.substr(url.rfind('/') + 1), std::ios::binary);
    HttpResult result;
    if (!file) return result;
    result.status = 200;
    result.body.assign(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
    return result;
}

// IOSRUNNER_DRIVE=<button text>: taps that button once it appears, and plays the web view's part until IOSRUNNER_DRIVE_SECONDS.
void drive_message(const char* kind, const char* payload) {
    const std::string text(payload);
    fprintf(stdout, "EMIT %s %s\n", kind, payload);
    fflush(stdout);
    if (strcmp(kind, "tree") == 0 && !g_tapped) {
        // Action ids change between trees, so wait for the one that has the loaded content.
        const char* ready = getenv("IOSRUNNER_DRIVE_WHEN");
        if (ready != nullptr && text.find(ready) == std::string::npos) return;
        const size_t label = text.find(std::string("\"") + getenv("IOSRUNNER_DRIVE") + "\"");
        if (label == std::string::npos) return;
        const size_t action = text.rfind("\"a\":", label);
        if (action == std::string::npos) return;
        g_tapped = true;
        engine_post_action(atoi(text.c_str() + action + 4));
    } else if (strcmp(kind, "webload") == 0) {
        g_probe_phase = 0;
    } else if (strcmp(kind, "webjs") == 0) {
        const size_t call_at = text.find("\"call\":");
        const int call = atoi(text.c_str() + call_at + 7);
        if (text.find("probe") != std::string::npos) {
            foundation_js_finished(call, probe_result(), false);
        } else {
            ++g_probe_phase;
            foundation_js_finished(call, "true", false);
        }
    }
}

}  // namespace

int main(int argc, char** argv) {
    g_tls_alignment[0] = 0;
    // Matches the manifest's allowNativeHeapPointerTagging="false": tags collide with Swift's spare bits.
    mallopt(M_BIONIC_SET_HEAP_TAGGING_LEVEL, M_HEAP_TAGGING_LEVEL_NONE);
    if (argc != 3) {
        fprintf(stderr, "usage: %s <bundle dir> <executable name>\n", argv[0]);
        return 2;
    }
    if (getenv("IOSRUNNER_DRIVE") != nullptr) {
        engine_set_message_sink(drive_message);
        if (getenv("IOSRUNNER_DRIVE_SITE") != nullptr) engine_set_http_handler(serve_from_directory);
        const int seconds = atoi(getenv("IOSRUNNER_DRIVE_SECONDS") != nullptr ? getenv("IOSRUNNER_DRIVE_SECONDS") : "60");
        std::thread([seconds] {
            sleep(static_cast<unsigned>(seconds));
            engine_close_actions();
        }).detach();
        return run_app_bundle(argv[1], argv[2], "");
    }
    // Without a UI there is nothing to act: print the first tree and let the guest finish.
    engine_set_message_sink(+[](const char* kind, const char* payload) { fprintf(stdout, "EMIT %s %s\n", kind, payload); fflush(stdout); });
    engine_close_actions();
    return run_app_bundle(argv[1], argv[2], "");
}
