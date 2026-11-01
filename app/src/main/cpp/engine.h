#pragma once
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

// Runs the app bundle's main executable. Never returns control to a crashed guest: faults end the process.
int run_app_bundle(const std::string& bundle_dir, const std::string& executable, const std::string& log_path);

struct MachImage;
// Every loaded image in initialization order; the app is last.
const std::vector<std::unique_ptr<MachImage>>& engine_images();

void engine_log(const char* format, ...) __attribute__((format(printf, 1, 2)));

// The running app's bundle directory.
const std::string& engine_bundle_dir();

// Sends a message to the host UI: "tree" with a JSON view tree, or "open" with a URL.
void engine_emit(const char* kind, const std::string& payload);
void engine_set_message_sink(void (*sink)(const char* kind, const char* payload));

// Blocks the guest thread until the UI triggers an action, returning its id, or -1 if there will be no more.
// Blocks for the next action or host event; past `deadline_ns` on CLOCK_MONOTONIC, if positive, returns kEngineEventRan.
int engine_wait_for_action(int64_t deadline_ns = 0);
void engine_post_action(int action);
void engine_close_actions();

struct HttpResult {
    int status = -1;  // Negative when the request could not be made.
    std::string body;
};

// Performs a blocking HTTP request through the host. Headers are "Name: value" lines.
HttpResult engine_http(const std::string& url, const std::string& method, const std::string& headers, const std::string& body);
void engine_set_http_handler(HttpResult (*handler)(const std::string&, const std::string&, const std::string&, const std::string&));

// Runs `event` on the guest thread, waking it if it is idle. engine_wait_for_action then returns kEngineEventRan.
constexpr int kEngineEventRan = -2;
void engine_post_event(std::function<void()> event);

// Registers a reply handler for a request to the host, which runs on the guest thread. Returns the id the host replies to.
int engine_host_register(std::function<void(const std::string&)> handler, bool repeating);
void engine_host_unregister(int id);
void engine_host_reply(int id, const std::string& payload);

// Asks the host a question it can answer immediately, blocking the guest thread.
std::string engine_host_query(const std::string& kind, const std::string& argument);
void engine_set_host_query_handler(std::string (*handler)(const std::string&, const std::string&));

// Reads all cookies from the host as "domain\tname\tvalue" lines.
std::string engine_cookies();
void engine_set_cookie_handler(std::string (*handler)());
