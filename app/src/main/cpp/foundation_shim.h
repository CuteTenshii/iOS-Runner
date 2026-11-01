#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include <string_view>

void* foundation_symbol(std::string_view name);
void foundation_register_classes();

// A Swift String as its two ABI words.
struct SwiftString {
    uint64_t count_and_flags;
    uint64_t object;
};

// Foundation.Data as its two ABI words.
struct FoundationData {
    uint64_t range;
    uint64_t storage;
};

std::string foundation_data_bytes(FoundationData data);
FoundationData foundation_make_data(const std::string& bytes);

// An Error existential carrying `message`.
void* foundation_make_error(const std::string& message, int64_t code);

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

// UserDefaults values for @AppStorage, with the fallback used until a key has been stored.
bool foundation_defaults_lookup_bool(const std::string& key, bool fallback);
int64_t foundation_defaults_lookup_integer(const std::string& key, int64_t fallback);
std::string foundation_defaults_lookup_string(const std::string& key, const std::string& fallback);
void foundation_defaults_store_bool(const std::string& key, bool value);
void foundation_defaults_store_integer(const std::string& key, int64_t value);
void foundation_defaults_store_string(const std::string& key, const std::string& value);

// A string, URL or date object as text that foundation_decode_object restores, or empty for anything else.
std::string foundation_encode_object(void* object);
void* foundation_decode_object(const std::string& encoded);

// The text of an NSString or NSURL, or empty for anything else.
std::string foundation_object_text(void* object);

// Seconds since the reference date, if `object` is an NSDate.
bool foundation_object_date(void* object, double& seconds);

// The native array behind an NSArray that was bridged from Swift, or null.
void* foundation_native_array(void* object);

// A Foundation.URL value, as one reference to its box.
void* foundation_make_url(const std::string& text);

// Parses "yyyy-MM-ddTHH:mm:ss" with an optional fraction and offset, to seconds since 2001.
bool foundation_parse_iso8601(const std::string& text, double& seconds);

// The message of an error made by these shims, which has no Swift type behind it.
SwiftString foundation_error_message(void* error);

// A new NSArray holding these objects.
void* foundation_new_array(const std::vector<void*>& items);
