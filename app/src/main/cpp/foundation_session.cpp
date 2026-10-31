#include "foundation_internal.h"

using namespace foundation;

// A task from dataTask(with:completionHandler:): the request happens once resume is called.
struct DataTask {
    HeapObject header;
    std::string* url;
    std::string* method;
    std::string* headers;
    std::string* body;
    void* completion;
    void* response;
};

void* new_data_task(std::string url, std::string method, std::string headers, std::string body, void* completion) {
    auto* task = static_cast<DataTask*>(objc_framework_object("NSURLSessionDataTask"));
    task->url = new std::string(std::move(url));
    task->method = new std::string(std::move(method));
    task->headers = new std::string(std::move(headers));
    task->body = new std::string(std::move(body));
    task->completion = darwin_block_copy(completion);
    return task;
}

void* session_task_url(void*, const char*, void* url, void* completion) { return new_data_task(nsstring_text(url), "GET", "", "", completion); }

void* task_for_request(void* request, void* completion) {
    const auto found = bridged_requests().find(request);
    if (found == bridged_requests().end()) return new_data_task(nsstring_text(request), "GET", "", "", completion);
    const URLRequestBox* box = found->second;
    return new_data_task(*box->url, *box->method, *box->headers, *box->body, completion);
}

void* session_task_request(void*, const char*, void* request, void* completion) { return task_for_request(request, completion); }

void disposition_handler(void*, int64_t) {}

// A completion block for didReceiveResponse: the transfer has already finished, so the answer changes nothing.
struct GlobalBlock {
    void* isa;
    int32_t flags, reserved;
    void* invoke;
    const void* descriptor;
};

constexpr int32_t kBlockGlobalFlag = 1 << 28;

const uintptr_t kBlockDescriptor[2] = {0, sizeof(GlobalBlock)};

GlobalBlock g_disposition_block = {nullptr, kBlockGlobalFlag, 0, reinterpret_cast<void*>(disposition_handler), kBlockDescriptor};

using MessageSend = void (*)(void*, const char*, void*, void*, void*, void*);

void run_delegate_task(DataTask* task, const AsyncDataResult& result) {
    auto send = reinterpret_cast<MessageSend>(objc_runtime_symbol("objc_msgSend"));
    void* session = framework_singleton("NSURLSession");
    void* delegate = g_url_session_delegate;
    if (result.response != nullptr) {
        send(delegate, "URLSession:dataTask:didReceiveResponse:completionHandler:", session, task, result.response, &g_disposition_block);
        send(delegate, "URLSession:dataTask:didReceiveData:", session, task, make_nsdata(data_bytes(result.data.range, result.data.storage)), nullptr);
    }
    send(delegate, "URLSession:task:didCompleteWithError:", session, task, result.error, nullptr);
}

void* session_task_request_delegate(void*, const char*, void* request) { return task_for_request(request, nullptr); }

void run_data_task(void* raw) {
    auto* task = static_cast<DataTask*>(raw);
    const AsyncDataResult result = perform_request(*task->url, *task->method, *task->headers, *task->body);
    task->response = result.response;
    if (task->completion == nullptr) {
        run_delegate_task(task, result);
        return;
    }
    struct Literal {
        void* isa;
        int32_t flags, reserved;
        void (*invoke)(void*, void*, void*, void*);
    };
    void* data = result.error == nullptr ? make_nsdata(data_bytes(result.data.range, result.data.storage)) : nullptr;
    static_cast<Literal*>(task->completion)->invoke(task->completion, data, result.response, result.error);
}

void session_ignore_word(void*, const char*, void*) {}

int64_t http_response_expected_length(void*, const char*) { return -1; }

void session_task_resume(void* self, const char*) { dispatch_enqueue(run_data_task, self); }

void session_task_cancel(void*, const char*) {}

void* session_task_response(void* self, const char*) { return static_cast<DataTask*>(self)->response; }

SWIFTCC void date_unbridge(SWIFT_OUT double* out, void* object) {
    double seconds = 0;
    if (object != nullptr) foundation_object_date(object, seconds);
    *out = seconds;
}

double seconds_now_since_2001() {
    timespec now{};
    clock_gettime(CLOCK_REALTIME, &now);
    return static_cast<double>(now.tv_sec) + static_cast<double>(now.tv_nsec) / 1e9 - kSecondsFrom1970To2001;
}

constexpr double kDistantPast = -63114076800.0;

SWIFTCC void date_distant_past(SWIFT_OUT double* out) { *out = kDistantPast; }

SWIFTCC double date_interval_since(const double* other, SWIFT_CONTEXT const double* self) { return *self - *other; }

SWIFTCC void date_adding(SWIFT_OUT double* out, double interval, SWIFT_CONTEXT const double* self) { *out = *self + interval; }

SWIFTCC void date_plus(SWIFT_OUT double* out, const double* date, double interval) { *out = *date + interval; }

SWIFTCC void date_from_interval_since(SWIFT_OUT double* out, double interval, const double* date) { *out = *date + interval; }

SWIFTCC bool date_greater(const double* left, const double* right) { return *left > *right; }

SWIFTCC bool date_less(const double* left, const double* right) { return *left < *right; }

SWIFTCC bool date_equal(const double* left, const double* right) { return *left == *right; }

SWIFTCC void date_from_since_now(SWIFT_OUT double* out, double interval) { *out = seconds_now_since_2001() + interval; }

SWIFTCC double date_since_now(SWIFT_CONTEXT const double* self) { return *self - seconds_now_since_2001(); }

SWIFTCC void date_from_1970(SWIFT_OUT double* out, double interval) { *out = interval - kSecondsFrom1970To2001; }

SWIFTCC double date_since_1970(SWIFT_CONTEXT const double* self) { return *self + kSecondsFrom1970To2001; }

// `result` is a String? written in place.
SWIFTCC bool string_conditional_bridge(void* object, SwiftString* result, void*) {
    *result = swift_string_from_utf8(nsstring_text(object));
    return true;
}

SWIFTCC void string_force_bridge(void* object, SwiftString* result, void*) { *result = swift_string_from_utf8(nsstring_text(object)); }

void* bundle_identifier_object(void*, const char*) { return new_nsstring(bundle_info_string("CFBundleIdentifier")); }

struct ScannerObject {
    HeapObject header;
    std::string* text;
};

void* scanner_init(void* self, const char*, void* text) {
    static_cast<ScannerObject*>(self)->text = new std::string(nsstring_text(text));
    return self;
}

void* scanner_with_string(void*, const char*, void* text) { return scanner_init(objc_framework_object("NSScanner"), nullptr, text); }

bool scanner_scan_hex(void* self, const char*, uint64_t* out) {
    const std::string& text = *static_cast<ScannerObject*>(self)->text;
    char* end;
    const uint64_t value = strtoull(text.c_str(), &end, 16);
    if (end == text.c_str()) return false;
    if (out != nullptr) *out = value;
    return true;
}

// The host's HTTP stack does its own caching, so a URL cache is an object nothing is ever stored in.
void* url_cache_init(void* self, const char*, uint64_t, uint64_t, void*) { return self; }

void* url_cache_shared(void*, const char*) { return framework_singleton("NSURLCache"); }

void url_cache_ignore(void*, const char*) {}

void url_cache_set_word(void*, const char*, uint64_t) {}

// Operation queues run on the guest thread, like every Dispatch queue: operations go through its run loop.
void operation_ignore_word(void*, const char*, int64_t) {}

struct Block0 {
    void* isa;
    int32_t flags, reserved;
    void (*invoke)(void*);
};

void run_operation_block(void* block) { static_cast<Block0*>(block)->invoke(block); }

void queue_add_block(void*, const char*, void* block) { dispatch_enqueue(run_operation_block, darwin_block_copy(block)); }

void run_operation(void* operation) {
    objc_send(operation, "start", nullptr);
}

void queue_add_operation(void*, const char*, void* operation) {
    rt_swift_retain(operation);
    dispatch_enqueue(run_operation, operation);
}

void* session_delegate_queue(void*, const char*) { return framework_singleton("NSOperationQueue"); }

// A guest subclass's instance has no room for the base class's state, so cancellation lives here.
std::set<const void*>& cancelled_operations() {
    static std::set<const void*> operations;
    return operations;
}

void* operation_init(void* self, const char*) {
    cancelled_operations().erase(self);
    return self;
}

void operation_mark_cancelled(void* self, const char*) { cancelled_operations().insert(self); }

bool operation_is_cancelled(void* self, const char*) { return cancelled_operations().count(self) != 0; }

// Nothing observes key paths here: these calls only exist to notify observers.
void object_key_change_ignored(void*, const char*, void*) {}

struct BlockOperation {
    HeapObject header;
    void* block;
};

void* block_operation_with_block(void*, const char*, void* block) {
    auto* operation = static_cast<BlockOperation*>(objc_framework_object("NSBlockOperation"));
    operation->block = darwin_block_copy(block);
    return operation;
}

void block_operation_start(void* self, const char*) { run_operation_block(static_cast<BlockOperation*>(self)->block); }

// Every guest task runs on one thread, so a lock is never contended.
void lock_nothing(void*, const char*) {}

// NSCache that keeps everything: nothing here reacts to memory pressure.
struct CacheObject {
    HeapObject header;
    std::unordered_map<void*, void*>* entries;
};

std::unordered_map<void*, void*>& cache_entries(void* self) {
    auto* cache = static_cast<CacheObject*>(self);
    if (cache->entries == nullptr) cache->entries = new std::unordered_map<void*, void*>();
    return *cache->entries;
}

void* cache_object_for_key(void* self, const char*, void* key) {
    auto& entries = cache_entries(self);
    const auto found = entries.find(key);
    return found == entries.end() ? nullptr : found->second;
}

void cache_set_object(void* self, const char*, void* value, void* key) {
    rt_swift_retain(value);
    rt_swift_retain(key);
    cache_entries(self)[key] = value;
}

void cache_set_object_cost(void* self, const char* selector, void* value, void* key, uint64_t) { cache_set_object(self, selector, value, key); }

void cache_remove_object(void* self, const char*, void* key) { cache_entries(self).erase(key); }

void cache_remove_all(void* self, const char*) { cache_entries(self).clear(); }

void cache_ignore_word(void*, const char*, uint64_t) {}

void* iso_formatter_date(void*, const char*, void* text) {
    double seconds = 0;
    return text != nullptr && foundation_parse_iso8601(nsstring_text(text), seconds) ? new_date_object(seconds) : nullptr;
}

void formatter_ignore_word(void*, const char*, uint64_t) {}

// A date format pattern's fields, as strftime spells them; only numeric UTC formats reach this runtime.
std::string strftime_pattern(const std::string& format) {
    static const std::pair<const char*, const char*> kFields[] = {{"yyyy", "%Y"}, {"MMMM", "%B"}, {"MMM", "%b"}, {"MM", "%m"}, {"dd", "%d"}, {"HH", "%H"}, {"hh", "%I"}, {"mm", "%M"}, {"ss", "%S"}, {"a", "%p"}};
    std::string out;
    for (size_t i = 0; i < format.size();) {
        if (format[i] == '\'') {
            const size_t end = format.find('\'', i + 1);
            if (end == std::string::npos) break;
            out += format.substr(i + 1, end - i - 1);
            i = end + 1;
            continue;
        }
        bool matched = false;
        for (const auto& [field, spelled] : kFields) {
            if (format.compare(i, strlen(field), field) == 0) {
                out += spelled;
                i += strlen(field);
                matched = true;
                break;
            }
        }
        if (!matched) out += format[i++];
    }
    return out;
}

struct DateFormatterObject {
    HeapObject header;
    std::string* format;
};

void date_formatter_set_format(void* self, const char*, void* format) {
    auto* formatter = static_cast<DateFormatterObject*>(self);
    delete formatter->format;
    formatter->format = new std::string(format == nullptr ? std::string() : nsstring_text(format));
}

std::string date_formatter_pattern(void* self) {
    const auto* format = static_cast<DateFormatterObject*>(self)->format;
    return strftime_pattern(format == nullptr || format->empty() ? "yyyy-MM-dd HH:mm" : *format);
}

constexpr double kFormatterEpochOffset = 978307200;

void* date_formatter_date(void* self, const char*, void* text) {
    if (text == nullptr) return nullptr;
    tm parts{};
    if (strptime(nsstring_text(text).c_str(), date_formatter_pattern(self).c_str(), &parts) == nullptr) return nullptr;
    return new_date_object(static_cast<double>(timegm(&parts)) - kFormatterEpochOffset);
}

void* date_formatter_string(void* self, const char*, void* date) {
    double seconds = 0;
    if (date == nullptr || !foundation_object_date(date, seconds)) return new_nsstring("");
    const time_t clock = static_cast<time_t>(seconds + kFormatterEpochOffset);
    tm parts{};
    gmtime_r(&clock, &parts);
    char text[128];
    strftime(text, sizeof text, date_formatter_pattern(self).c_str(), &parts);
    return new_nsstring(text);
}

std::string foundation_object_text(void* object) {
    const void* isa = static_cast<HeapObject*>(object)->metadata;
    return isa == g_nsstring_class || isa == g_nsurl_class ? nsstring_text(object) : std::string();
}

bool foundation_object_date(void* object, double& seconds) {
    if (static_cast<HeapObject*>(object)->metadata != g_nsdate_class) return false;
    seconds = static_cast<NSDateObject*>(object)->seconds;
    return true;
}

std::string foundation_encode_object(void* object) {
    const void* isa = static_cast<HeapObject*>(object)->metadata;
    if (isa == g_nsstring_class) return "s" + nsstring_text(object);
    if (isa == g_nsurl_class) return "u" + nsstring_text(object);
    if (isa == g_nsdate_class) return "d" + std::to_string(static_cast<NSDateObject*>(object)->seconds);
    return {};
}

void* foundation_decode_object(const std::string& encoded) {
    if (encoded.empty()) return nullptr;
    const std::string body = encoded.substr(1);
    if (encoded[0] == 'd') return new_date_object(strtod(body.c_str(), nullptr));
    auto* object = static_cast<NSStringObject*>(new_nsstring(body));
    if (encoded[0] == 'u') object->header.metadata = g_nsurl_class;
    return object;
}

bool foundation_defaults_lookup_bool(const std::string& key, bool fallback) {
    void* name = new_nsstring(key);
    const bool known = defaults_object_for_key(nullptr, nullptr, name) != nullptr;
    return known ? defaults_bool_for_key(nullptr, nullptr, name) : fallback;
}

int64_t foundation_defaults_lookup_integer(const std::string& key, int64_t fallback) {
    void* name = new_nsstring(key);
    const bool known = defaults_object_for_key(nullptr, nullptr, name) != nullptr;
    return known ? defaults_integer_for_key(nullptr, nullptr, name) : fallback;
}

std::string foundation_defaults_lookup_string(const std::string& key, const std::string& fallback) {
    void* value = defaults_string_for_key(nullptr, nullptr, new_nsstring(key));
    return value == nullptr ? fallback : nsstring_text(value);
}

void foundation_defaults_store_bool(const std::string& key, bool value) { defaults_set_bool(nullptr, nullptr, value, new_nsstring(key)); }

void foundation_defaults_store_integer(const std::string& key, int64_t value) { defaults_set_integer(nullptr, nullptr, value, new_nsstring(key)); }

void foundation_defaults_store_string(const std::string& key, const std::string& value) { defaults_set_object(nullptr, nullptr, new_nsstring(value), new_nsstring(key)); }

namespace foundation {

void add_foundation_session_symbols(SymbolTable& table) {
    table.insert({
        {"$s10Foundation4DateV36_unconditionallyBridgeFromObjectiveCyACSo6NSDateCSgFZ", reinterpret_cast<void*>(date_unbridge)},
        {"$s10Foundation4DateV11distantPastACvgZ", reinterpret_cast<void*>(date_distant_past)},
        {"$s10Foundation4DateV17timeIntervalSinceySdACF", reinterpret_cast<void*>(date_interval_since)},
        {"$s10Foundation4DateV18addingTimeIntervalyACSdF", reinterpret_cast<void*>(date_adding)},
        {"$s10Foundation4DateV1poiyA2C_SdtFZ", reinterpret_cast<void*>(date_plus)},
        {"$s10Foundation4DateV12timeInterval5sinceACSd_ACtcfC", reinterpret_cast<void*>(date_from_interval_since)},
        {"$s10Foundation4DateV1goiySbAC_ACtFZ", reinterpret_cast<void*>(date_greater)},
        {"$s10Foundation4DateV1loiySbAC_ACtFZ", reinterpret_cast<void*>(date_less)},
        {"$s10Foundation4DateV2eeoiySbAC_ACtFZ", reinterpret_cast<void*>(date_equal)},
        {"$s10Foundation4DateV20timeIntervalSinceNowACSd_tcfC", reinterpret_cast<void*>(date_from_since_now)},
        {"$s10Foundation4DateV20timeIntervalSinceNowSdvg", reinterpret_cast<void*>(date_since_now)},
        {"$s10Foundation4DateV21timeIntervalSince1970ACSd_tcfC", reinterpret_cast<void*>(date_from_1970)},
        {"$s10Foundation4DateV21timeIntervalSince1970Sdvg", reinterpret_cast<void*>(date_since_1970)},
        {"$sSS10FoundationE34_conditionallyBridgeFromObjectiveC_6resultSbSo8NSStringC_SSSgztFZ", reinterpret_cast<void*>(string_conditional_bridge)},
        {"$sSS10FoundationE26_forceBridgeFromObjectiveC_6resultySo8NSStringC_SSSgztFZ", reinterpret_cast<void*>(string_force_bridge)},
    });
}

void register_foundation_session_classes() {
    objc_register_method("-[NSURLSession setSessionDescription:]", reinterpret_cast<const void*>(session_ignore_word));
    objc_register_method("-[NSURLSession dataTaskWithRequest:]", reinterpret_cast<const void*>(session_task_request_delegate));
    objc_register_method("-[NSHTTPURLResponse expectedContentLength]", reinterpret_cast<const void*>(http_response_expected_length));
    objc_register_method("-[NSURLSession dataTaskWithURL:completionHandler:]", reinterpret_cast<const void*>(session_task_url));
    objc_register_method("-[NSURLSession dataTaskWithRequest:completionHandler:]", reinterpret_cast<const void*>(session_task_request));
    objc_register_method("-[NSURLSessionDataTask resume]", reinterpret_cast<const void*>(session_task_resume));
    objc_register_method("-[NSURLSessionDataTask cancel]", reinterpret_cast<const void*>(session_task_cancel));
    objc_register_method("-[NSURLSessionDataTask response]", reinterpret_cast<const void*>(session_task_response));
    objc_register_method("-[NSURLSession delegateQueue]", reinterpret_cast<const void*>(session_delegate_queue));
    objc_register_method("-[NSURLSessionDataTask setPrefersIncrementalDelivery:]", reinterpret_cast<const void*>(url_cache_set_word));
    objc_register_method("-[NSURLSessionDataTask setPriority:]", reinterpret_cast<const void*>(url_cache_set_word));
    objc_register_method("-[NSISO8601DateFormatter setFormatOptions:]", reinterpret_cast<const void*>(formatter_ignore_word));
    objc_register_method("-[NSISO8601DateFormatter dateFromString:]", reinterpret_cast<const void*>(iso_formatter_date));
    objc_register_method("-[NSDateFormatter setDateFormat:]", reinterpret_cast<const void*>(date_formatter_set_format));
    objc_register_method("-[NSDateFormatter setLocale:]", reinterpret_cast<const void*>(formatter_ignore_word));
    objc_register_method("-[NSDateFormatter setTimeZone:]", reinterpret_cast<const void*>(formatter_ignore_word));
    objc_register_method("-[NSDateFormatter setCalendar:]", reinterpret_cast<const void*>(formatter_ignore_word));
    objc_register_method("-[NSDateFormatter setDateStyle:]", reinterpret_cast<const void*>(formatter_ignore_word));
    objc_register_method("-[NSDateFormatter setTimeStyle:]", reinterpret_cast<const void*>(formatter_ignore_word));
    objc_register_method("-[NSDateFormatter dateFromString:]", reinterpret_cast<const void*>(date_formatter_date));
    objc_register_method("-[NSDateFormatter stringFromDate:]", reinterpret_cast<const void*>(date_formatter_string));
    objc_register_method("-[NSBundle bundleIdentifier]", reinterpret_cast<const void*>(bundle_identifier_object));
    objc_register_method("-[NSScanner initWithString:]", reinterpret_cast<const void*>(scanner_init));
    objc_register_method("+[NSScanner scannerWithString:]", reinterpret_cast<const void*>(scanner_with_string));
    objc_register_method("-[NSScanner scanHexLongLong:]", reinterpret_cast<const void*>(scanner_scan_hex));
    objc_register_method("-[NSURLCache initWithMemoryCapacity:diskCapacity:diskPath:]", reinterpret_cast<const void*>(url_cache_init));
    objc_register_method("+[NSURLCache sharedURLCache]", reinterpret_cast<const void*>(url_cache_shared));
    objc_register_method("-[NSURLCache removeAllCachedResponses]", reinterpret_cast<const void*>(url_cache_ignore));
    objc_register_method("-[NSURLCache setDiskCapacity:]", reinterpret_cast<const void*>(url_cache_set_word));
    objc_register_method("-[NSURLCache setMemoryCapacity:]", reinterpret_cast<const void*>(url_cache_set_word));
    objc_register_method("-[NSURLSessionConfiguration setURLCache:]", reinterpret_cast<const void*>(url_cache_set_word));
    objc_register_method("-[NSOperationQueue setMaxConcurrentOperationCount:]", reinterpret_cast<const void*>(operation_ignore_word));
    objc_register_method("-[NSOperationQueue setQualityOfService:]", reinterpret_cast<const void*>(operation_ignore_word));
    objc_register_method("-[NSOperationQueue setUnderlyingQueue:]", reinterpret_cast<const void*>(operation_ignore_word));
    objc_register_method("-[NSOperationQueue setName:]", reinterpret_cast<const void*>(operation_ignore_word));
    objc_register_method("-[NSOperationQueue addOperationWithBlock:]", reinterpret_cast<const void*>(queue_add_block));
    objc_register_method("-[NSOperationQueue addOperation:]", reinterpret_cast<const void*>(queue_add_operation));
    objc_register_method("+[NSBlockOperation blockOperationWithBlock:]", reinterpret_cast<const void*>(block_operation_with_block));
    objc_register_method("-[NSBlockOperation start]", reinterpret_cast<const void*>(block_operation_start));
    objc_register_method("-[NSOperation setQueuePriority:]", reinterpret_cast<const void*>(operation_ignore_word));
    objc_register_method("-[NSOperation init]", reinterpret_cast<const void*>(operation_init));
    objc_register_method("-[NSOperation cancel]", reinterpret_cast<const void*>(operation_mark_cancelled));
    objc_register_method("-[NSOperation isCancelled]", reinterpret_cast<const void*>(operation_is_cancelled));
    objc_register_method("-[NSOperationQueue setSuspended:]", reinterpret_cast<const void*>(operation_ignore_word));
    objc_register_method("-[NSObject willChangeValueForKey:]", reinterpret_cast<const void*>(object_key_change_ignored));
    objc_register_method("-[NSObject didChangeValueForKey:]", reinterpret_cast<const void*>(object_key_change_ignored));
    objc_register_method("-[NSOperation setQualityOfService:]", reinterpret_cast<const void*>(operation_ignore_word));
    objc_register_method("-[NSLock lock]", reinterpret_cast<const void*>(lock_nothing));
    objc_register_method("-[NSLock unlock]", reinterpret_cast<const void*>(lock_nothing));
    objc_register_method("-[NSRecursiveLock lock]", reinterpret_cast<const void*>(lock_nothing));
    objc_register_method("-[NSRecursiveLock unlock]", reinterpret_cast<const void*>(lock_nothing));
    objc_register_method("-[NSCondition lock]", reinterpret_cast<const void*>(lock_nothing));
    objc_register_method("-[NSCondition unlock]", reinterpret_cast<const void*>(lock_nothing));
    objc_register_method("-[NSCache objectForKey:]", reinterpret_cast<const void*>(cache_object_for_key));
    objc_register_method("-[NSCache setObject:forKey:]", reinterpret_cast<const void*>(cache_set_object));
    objc_register_method("-[NSCache setObject:forKey:cost:]", reinterpret_cast<const void*>(cache_set_object_cost));
    objc_register_method("-[NSCache removeObjectForKey:]", reinterpret_cast<const void*>(cache_remove_object));
    objc_register_method("-[NSCache removeAllObjects]", reinterpret_cast<const void*>(cache_remove_all));
    objc_register_method("-[NSCache setTotalCostLimit:]", reinterpret_cast<const void*>(cache_ignore_word));
    objc_register_method("-[NSCache setCountLimit:]", reinterpret_cast<const void*>(cache_ignore_word));
}

}  // namespace foundation
