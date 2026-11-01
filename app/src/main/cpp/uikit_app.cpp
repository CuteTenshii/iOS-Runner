#include <dirent.h>

#include <fstream>
#include <iterator>

#include "concurrency_shim.h"
#include "engine.h"
#include "foundation_internal.h"
#include "plist.h"
#include "uikit_internal.h"
#include "uikit_shim.h"
#include "uuid.h"

using foundation::new_nsstring;
using foundation::nsstring_text;

namespace uikit {

namespace {

void* g_application = nullptr;
void* g_app_delegate = nullptr;
void* g_scene_delegate = nullptr;
void* g_scene = nullptr;
void* g_window = nullptr;

void* ns_string_from_class(void* cls) { return cls == nullptr ? nullptr : new_nsstring(objc_class_name(cls)); }

void* ns_class_from_string(void* name) { return name == nullptr ? nullptr : objc_class_named(nsstring_text(name)); }

using Message1 = void* (*)(void*, const char*, void*);
using Message2 = void* (*)(void*, const char*, void*, void*);
using Message3 = void* (*)(void*, const char*, void*, void*, void*);

void* send1(void* receiver, const char* selector, void* first) { return reinterpret_cast<Message1>(const_cast<void*>(objc_method_for(receiver, selector)))(receiver, selector, first); }
void* send2(void* receiver, const char* selector, void* first, void* second) { return reinterpret_cast<Message2>(const_cast<void*>(objc_method_for(receiver, selector)))(receiver, selector, first, second); }
void* send3(void* receiver, const char* selector, void* first, void* second, void* third) { return reinterpret_cast<Message3>(const_cast<void*>(objc_method_for(receiver, selector)))(receiver, selector, first, second, third); }

void connect_scene() {
    PlistValue info;
    if (!plist_read_file(engine_bundle_dir() + "/Info.plist", info)) {
        engine_log("UIKIT: no readable Info.plist");
        return;
    }
    const PlistValue* manifest = info.get("UIApplicationSceneManifest");
    const PlistValue* configurations = manifest != nullptr ? manifest->get("UISceneConfigurations") : nullptr;
    const PlistValue* roles = configurations != nullptr ? configurations->get("UIWindowSceneSessionRoleApplication") : nullptr;
    if (roles == nullptr || roles->items.empty()) return;
    const std::string delegate_name = roles->items[0].string_at("UISceneDelegateClassName");
    void* delegate_class = objc_class_named(delegate_name);
    if (delegate_class == nullptr) {
        engine_log("UIKIT: no scene delegate class %s", delegate_name.c_str());
        return;
    }
    g_scene = objc_framework_object("UIWindowScene");
    void* session = objc_framework_object("UISceneSession");
    void* options = objc_framework_object("UISceneConnectionOptions");
    g_scene_delegate = objc_alloc_init(delegate_class);
    // As iOS does: the storyboard's window exists before the delegate hears about the connection.
    const std::string storyboard = roles->items[0].string_at("UISceneStoryboardFile");
    if (!storyboard.empty()) {
        void* window = objc_framework_object("UIWindow");
        property_set_object(window, "windowScene", g_scene);
        object_set_value(g_scene_delegate, "window", window);
        void* root = storyboard_create_initial(storyboard);
        engine_log("UIKIT: initial view controller %s", root != nullptr ? objc_class_name(*static_cast<void**>(root)).c_str() : "(none)");
        using SetRoot = void (*)(void*, const char*, void*);
        reinterpret_cast<SetRoot>(const_cast<void*>(objc_method_for(window, "setRootViewController:")))(window, "setRootViewController:", root);
        g_window = window;
    }
    property_set_object(g_scene, "delegate", g_scene_delegate);
    property_set_object(g_application, "connectedScenes", g_scene);
    engine_log("UIKIT: connecting scene delegate %s", delegate_name.c_str());
    if (objc_responds(g_scene_delegate, "scene:willConnectToSession:options:")) send3(g_scene_delegate, "scene:willConnectToSession:options:", g_scene, session, options);
    if (g_window != nullptr) {
        using MakeVisible = void (*)(void*, const char*);
        reinterpret_cast<MakeVisible>(const_cast<void*>(objc_method_for(g_window, "makeKeyAndVisible")))(g_window, "makeKeyAndVisible");
        // Boot work queued during launch may run long, and the first screen shouldn't wait for it.
        uikit_render_if_changed();
    }
}

// Every app installed on iOS has a signed profile, and some refuse to start without one. Signing isn't implemented, so
// a bundle gets one with values of the right form. It names no certificates and proves nothing.
constexpr const char* kProfileTeam = "RUNNER0000";

// Rewriting a profile must not change its identity, so a UUID already written is kept.
std::string profile_uuid(const std::string& path) {
    constexpr std::string_view kUuidKey = "<key>UUID</key><string>";
    constexpr size_t kUuidLength = 36;
    std::ifstream existing(path, std::ios::binary);
    const std::string contents((std::istreambuf_iterator<char>(existing)), std::istreambuf_iterator<char>());
    const size_t found = contents.find(kUuidKey);
    return found == std::string::npos ? uuid4() : contents.substr(found + kUuidKey.size(), kUuidLength);
}

// A profile of ours can be replaced when this format changes; a real one never is.
bool has_real_profile(const std::string& path) {
    std::ifstream existing(path, std::ios::binary);
    if (!existing) return false;
    const std::string contents((std::istreambuf_iterator<char>(existing)), std::istreambuf_iterator<char>());
    return contents.find(kProfileTeam) == std::string::npos;
}

void write_placeholder_profile(const std::string& bundle) {
    const std::string path = bundle + "/embedded.mobileprovision";
    if (has_real_profile(path)) return;
    PlistValue info;
    plist_read_file(bundle + "/Info.plist", info);
    const std::string identifier = info.string_at("CFBundleIdentifier");
    std::string name = info.string_at("CFBundleName");
    if (name.empty()) name = identifier;
    const std::string team = kProfileTeam;
    const std::string uuid = profile_uuid(path);
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    // A real profile's plist sits inside a CMS envelope, which opens with a DER sequence; parsers look past it.
    file.write("\x30\x80", 2);
    file << R"(<?xml version="1.0" encoding="UTF-8"?>
<plist version="1.0"><dict>
<key>AppIDName</key><string>)"
         << name << R"(</string>
<key>ApplicationIdentifierPrefix</key><array><string>)"
         << team << R"(</string></array>
<key>CreationDate</key><date>2020-01-01T00:00:00Z</date>
<key>Platform</key><array><string>iOS</string></array>
<key>IsXcodeManaged</key><false/>
<key>DeveloperCertificates</key><array></array>
<key>Entitlements</key><dict>
<key>application-identifier</key><string>)"
         << team << "." << identifier << R"(</string>
<key>com.apple.developer.team-identifier</key><string>)"
         << team << R"(</string>
<key>get-task-allow</key><true/>
<key>keychain-access-groups</key><array><string>)"
         << team << R"(.*</string></array>
</dict>
<key>ExpirationDate</key><date>2100-01-01T00:00:00Z</date>
<key>Name</key><string>)"
         << name << R"(</string>
<key>ProvisionedDevices</key><array></array>
<key>TeamIdentifier</key><array><string>)"
         << team << R"(</string></array>
<key>TeamName</key><string>)"
         << team << R"(</string>
<key>TimeToLive</key><integer>29200</integer>
<key>UUID</key><string>)"
         << uuid << R"(</string>
<key>Version</key><integer>1</integer>
</dict></plist>
)";
}

// App extensions are checked for a profile of their own.
void write_placeholder_profiles() {
    write_placeholder_profile(engine_bundle_dir());
    const std::string plugins = engine_bundle_dir() + "/PlugIns";
    DIR* directory = opendir(plugins.c_str());
    if (directory == nullptr) return;
    while (const dirent* entry = readdir(directory)) {
        const std::string name = entry->d_name;
        if (name.size() > 6 && name.compare(name.size() - 6, 6, ".appex") == 0) write_placeholder_profile(plugins + "/" + name);
    }
    closedir(directory);
}

int application_main(int, char**, void*, void* delegate_name) {
    objc_set_uikit_active();
    write_placeholder_profiles();
    engine_log("UIKIT: UIApplicationMain, delegate %s", delegate_name != nullptr ? nsstring_text(delegate_name).c_str() : "(none)");
    g_application = objc_framework_object("UIApplication");
    if (delegate_name != nullptr) {
        void* cls = objc_class_named(nsstring_text(delegate_name));
        if (cls == nullptr) {
            engine_log("STOP: no application delegate class %s", nsstring_text(delegate_name).c_str());
            abort();
        }
        g_app_delegate = objc_alloc_init(cls);
        property_set_object(g_application, "delegate", g_app_delegate);
    }
    if (g_app_delegate != nullptr && objc_responds(g_app_delegate, "application:willFinishLaunchingWithOptions:")) send2(g_app_delegate, "application:willFinishLaunchingWithOptions:", g_application, nullptr);
    if (g_app_delegate != nullptr && objc_responds(g_app_delegate, "application:didFinishLaunchingWithOptions:")) send2(g_app_delegate, "application:didFinishLaunchingWithOptions:", g_application, nullptr);
    engine_log("UIKIT: application finished launching");
    connect_scene();
    concurrency_drain();
    engine_log("UIKIT: entering the run loop");
    for (;;) {
        uikit_render_if_changed();
        const int action = engine_wait_for_action(concurrency_next_deadline());
        if (action == kEngineEventRan)
            concurrency_fire_timers();
        else if (action < 0)
            return 0;
        else
            uikit_run_action(action);
        concurrency_drain();
    }
}

void* shared_application(void*, const char*) { return g_application; }

}  // namespace

void* application_object() { return g_application; }

void add_app_symbols(SymbolMap& table) {
    table.insert({
        {"NSStringFromClass", reinterpret_cast<void*>(ns_string_from_class)},
        {"NSClassFromString", reinterpret_cast<void*>(ns_class_from_string)},
        {"UIApplicationMain", reinterpret_cast<void*>(application_main)},
    });
}

void register_app_classes() { objc_register_method("+[UIApplication sharedApplication]", reinterpret_cast<const void*>(shared_application)); }

}  // namespace uikit
