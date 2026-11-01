#include "foundation_internal.h"

using namespace foundation;

static constexpr uint64_t kImmortalReferences = 1ULL << 63;

// CFBoolean constants resolve before classes register, so they get their class here.
static NSNumberObject g_true_number{}, g_false_number{};
static void* g_true_constant = &g_true_number;
static void* g_false_constant = &g_false_number;

void foundation_register_classes() {
    if (g_nsstring_class == nullptr) g_nsstring_class = objc_native_class("NSString", destroy_nsstring);
    for (void* constant : string_constants()) static_cast<HeapObject*>(constant)->metadata = g_nsstring_class;
    const auto make_record = [](AsyncFunctionRecord& record, void (*entry)()) {
        record.context_size = kUrlSessionContextBytes;
        record.function = static_cast<int32_t>(reinterpret_cast<intptr_t>(entry) - reinterpret_cast<intptr_t>(&record));
    };
    make_record(g_url_session_url_record, rt_url_session_data_url);
    make_record(g_url_session_request_record, rt_url_session_data_request);
    g_http_response_class = objc_native_class("NSHTTPURLResponse", free);
    g_defaults_class = objc_native_class("NSUserDefaults", free);
    g_nsarray_class = objc_native_class("NSArray", destroy_nsarray);
    g_bridged_collection_class = objc_native_class("NSDictionary", destroy_bridged_collection);
    g_cookie_class = objc_native_class("NSHTTPCookie", free);
    g_web_script_record.context_size = kUrlSessionContextBytes;
    g_web_script_record.function = static_cast<int32_t>(reinterpret_cast<intptr_t>(rt_web_view_call_script_entry) - reinterpret_cast<intptr_t>(&g_web_script_record));
    g_nsdata_class = objc_native_class("NSData", destroy_nsdata);
    g_nsdate_class = objc_native_class("NSDate", free);
    g_json_value_class = objc_native_class("__NSJSONValue", destroy_json_value);
    objc_register_method("-[NSProcessInfo physicalMemory]", reinterpret_cast<const void*>(+[](void*, const char*) -> uint64_t { return static_cast<uint64_t>(sysconf(_SC_PHYS_PAGES)) * static_cast<uint64_t>(sysconf(_SC_PAGESIZE)); }));
    g_nsurl_class = objc_native_class("NSURL", destroy_nsstring);
    g_nsnumber_class = objc_native_class("NSNumber", free);
    for (auto* number : {&g_true_number, &g_false_number}) {
        number->header.metadata = g_nsnumber_class;
        number->header.references = kImmortalReferences;
        number->is_bool = true;
    }
    g_true_number.value = 1;
    register_foundation_request_classes();
    register_foundation_web_classes();
    register_foundation_objects_classes();
    register_foundation_date_classes();
    register_foundation_url_classes();
    register_foundation_session_classes();
}

// Darwin notifications cross processes on iOS; a sandboxed Android app only ever hears its own.
struct DarwinObserver {
    void* observer;
    void (*callback)(void*, void*, void*, const void*, void*);
    std::string name;
};

static std::vector<DarwinObserver>& darwin_observers() {
    static std::vector<DarwinObserver> observers;
    return observers;
}

static void* darwin_center() {
    static void* center = objc_framework_object("__CFNotificationCenter");
    return center;
}

static void darwin_add_observer(void*, void* observer, void (*callback)(void*, void*, void*, const void*, void*), void* name, const void*, int64_t) {
    darwin_observers().push_back({observer, callback, name != nullptr ? nsstring_text(name) : std::string()});
}

static void darwin_remove_observer(void*, void* observer, void* name, const void*) {
    const std::string text = name != nullptr ? nsstring_text(name) : std::string();
    auto& observers = darwin_observers();
    observers.erase(std::remove_if(observers.begin(), observers.end(), [&](const DarwinObserver& o) { return o.observer == observer && (text.empty() || o.name == text); }), observers.end());
}

static void darwin_post(void* center, void* name, const void* object, void* info, bool) {
    const std::string text = nsstring_text(name);
    const auto observers = darwin_observers();
    for (const auto& observer : observers) {
        if (observer.name.empty() || observer.name == text) observer.callback(center, observer.observer, name, object, info);
    }
}

static void* home_directory() { return new_nsstring(engine_bundle_dir()); }

static void* temporary_directory() {
    const std::string path = engine_bundle_dir() + "/.tmp/";
    mkdir(path.c_str(), 0700);
    return new_nsstring(path);
}

static SymbolTable all_foundation_symbols() {
    SymbolTable all = {
        {"$s10Foundation3URLV13DirectoryHintO13inferFromPathyA2EmFWC", const_cast<int32_t*>(&kDirectoryHintInferFromPath)},
        {"$s10Foundation23LocalizedStringResourceV17BundleDescriptionO8forClassyAEyXlXpcAEmFWC", const_cast<int32_t*>(&kBundleDescriptionForClass)},
        {"$sSo12NSURLSessionC10FoundationE4data3for8delegateAC4DataV_So13NSURLResponseCtAC10URLRequestV_So0A12TaskDelegate_pSgtYaKFTu", &g_url_session_request_record},
        {"$sSo12NSURLSessionC10FoundationE4data4from8delegateAC4DataV_So13NSURLResponseCtAC3URLV_So0A12TaskDelegate_pSgtYaKFTu", &g_url_session_url_record},
        {"$sSo9WKWebViewC6WebKitE19callAsyncJavaScript_9arguments2in12contentWorldypSgSS_SDySSypGSo11WKFrameInfoCSgSo09WKContentL0CtYaKFTu", &g_web_script_record},
        {"kCLLocationAccuracyKilometer", const_cast<double*>(&kLocationAccuracyKilometer)},
    };
    add_foundation_string_symbols(all);
    add_foundation_objects_symbols(all);
    add_foundation_url_symbols(all);
    add_foundation_session_symbols(all);
    add_foundation_request_symbols(all);
    add_foundation_date_symbols(all);
    add_foundation_web_symbols(all);
    all.insert({
        {"CFNotificationCenterGetDarwinNotifyCenter", reinterpret_cast<void*>(darwin_center)},
        {"CFNotificationCenterAddObserver", reinterpret_cast<void*>(darwin_add_observer)},
        {"CFNotificationCenterRemoveObserver", reinterpret_cast<void*>(darwin_remove_observer)},
        {"CFNotificationCenterPostNotification", reinterpret_cast<void*>(darwin_post)},
        {"kCFBooleanTrue", &g_true_constant},
        {"kCFBooleanFalse", &g_false_constant},
        {"NSHomeDirectory", reinterpret_cast<void*>(home_directory)},
        {"NSTemporaryDirectory", reinterpret_cast<void*>(temporary_directory)},
        // Nothing raises Objective-C exceptions through the runtime, and a crash report is already logged.
        {"NSSetUncaughtExceptionHandler", reinterpret_cast<void*>(+[](void*) {})},
    });
    return all;
}

// Apple's framework constants are compared by value, and apps only ever pass them back, so each holds its own name.
static bool is_framework_string_constant(std::string_view name) {
    static constexpr std::string_view kPrefixes[] = {"NSSQLite", "NSMigratePersistentStores", "NSInferMappingModel", "NSPersistentHistory", "NSPersistentStoreRemote", "NSReadOnlyPersistentStore", "NSInMemoryStore", "NSBinaryStore", "NSManagedObjectContextDid", "NSManagedObjectContextWill", "NSInsertedObjects", "NSUpdatedObjects", "NSDeletedObjects", "NSRefreshedObjects", "NSInvalidatedObjects", "kSec", "kCGImage", "kCFStream", "kCFBundle", "kCACornerCurve", "kCAMediaTimingFunction", "NSLocalized", "NSFile", "UIFontTextStyle", "UITextContentType", "UICollectionElementKind", "UIAccessibilitySpeechAttribute", "UISheetPresentationControllerDetentIdentifier", "UIImagePickerController"};
    static constexpr std::string_view kSuffixes[] = {"ErrorDomain", "ErrorKey", "RunLoopMode", "RunLoopModes", "AttributeName", "ChangeNewKey", "ErrorCode"};
    const auto named_function = [&] { return name.size() > 3 && name.substr(0, 3) == "Sec"; };
    if (named_function()) return false;
    for (const auto prefix : kPrefixes) {
        if (name.rfind(prefix, 0) == 0) return true;
    }
    for (const auto suffix : kSuffixes) {
        if (name.size() > suffix.size() && name.substr(name.size() - suffix.size()) == suffix) return true;
    }
    return name.rfind("NSURL", 0) == 0 && name.size() > 3 && name.substr(name.size() - 3) == "Key";
}

static std::string string_constant_value(std::string_view name) {
    if (name == "kCFBundleIdentifierKey") return "CFBundleIdentifier";
    if (name == "kCFBundleVersionKey") return "CFBundleVersion";
    return std::string(name);
}

void* foundation_symbol(std::string_view name) {
    // UIKit and Foundation name their notifications with NSString constants holding the same text.
    constexpr std::string_view kNotificationSuffix = "Notification";
    if ((name.rfind("UI", 0) == 0 || name.rfind("NS", 0) == 0) && name.size() > kNotificationSuffix.size() && name.substr(name.size() - kNotificationSuffix.size()) == kNotificationSuffix) {
        static std::unordered_map<std::string, void*> constants;
        void*& slot = constants[std::string(name)];
        if (slot == nullptr) {
            slot = new_nsstring(std::string(name));
            string_constants().push_back(slot);
        }
        return &slot;
    }
    static const SymbolTable table = all_foundation_symbols();
    if (void* symbol = lookup_symbol(table, name)) return symbol;
    if (!is_framework_string_constant(name)) return nullptr;
    static std::unordered_map<std::string, void*> constants;
    void*& slot = constants[std::string(name)];
    if (slot == nullptr) {
        slot = new_nsstring(string_constant_value(name));
        string_constants().push_back(slot);
    }
    return &slot;
}
