#include "foundation_internal.h"

using namespace foundation;

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
    register_foundation_request_classes();
    register_foundation_web_classes();
    register_foundation_objects_classes();
    register_foundation_date_classes();
    register_foundation_url_classes();
    register_foundation_session_classes();
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
    static const SymbolTable table = [] {
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
        return all;
    }();
    const auto found = table.find(name);
    return found == table.end() ? nullptr : found->second;
}
