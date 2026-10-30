#pragma once
#include <cstdint>
#include <string>
#include <string_view>

void* foundation_symbol(std::string_view name);
void foundation_register_classes();

// A Swift String as its two ABI words.
struct SwiftString {
    uint64_t count_and_flags;
    uint64_t object;
};

std::string swift_string_to_utf8(SwiftString value);
SwiftString swift_string_from_utf8(const std::string& text);

// The text of a Foundation.URL held in its runtime box.
std::string foundation_url_text(const void* url);

// The id of a guest WKWebView, or -1 for any other object.
int foundation_web_view_id(const void* object);

// Host callbacks, all safe from any thread.
void foundation_js_finished(int call, const std::string& json, bool failed);
void foundation_web_url_changed(int web_view, const std::string& url);
void foundation_cookies_changed();
