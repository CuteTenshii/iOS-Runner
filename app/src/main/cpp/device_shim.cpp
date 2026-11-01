#include <cmath>
#include <cstdint>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "engine.h"
#include "foundation_internal.h"
#include "json.h"
#include "symbols.h"
#include "uikit_internal.h"

using namespace uikit;

// CoreLocation and UserNotifications, answered by Android's own services through the host.
namespace {

constexpr size_t kBlockInvokeWord = 2;

void* block_copy(void* block) { return block != nullptr ? foundation::darwin_block_copy(block) : nullptr; }

template <typename... Arguments>
void invoke_block(void* block, Arguments... arguments) {
    if (block == nullptr) return;
    using Invoke = void (*)(void*, Arguments...);
    reinterpret_cast<Invoke>(static_cast<void**>(block)[kBlockInvokeWord])(block, arguments...);
}

Json json_object(std::vector<std::pair<std::string, Json>> members) {
    Json out;
    out.kind = Json::Kind::Object;
    out.members = std::move(members);
    return out;
}

Json json_int(int64_t value) {
    Json out;
    out.kind = Json::Kind::Integer;
    out.integer = value;
    return out;
}

Json json_real(double value) {
    Json out;
    out.kind = Json::Kind::Number;
    out.number = value;
    return out;
}

double number_at(const Json& json, const char* name) {
    const Json* member = json.member(name);
    if (member == nullptr) return 0;
    return member->kind == Json::Kind::Integer ? static_cast<double>(member->integer) : member->number;
}

bool flag_at(const Json& json, const char* name) {
    const Json* member = json.member(name);
    return member != nullptr && member->flag;
}

void request_permission(const char* group, std::function<void(bool)> done) {
    const int id = engine_host_register(
        [done = std::move(done)](const std::string& payload) {
            Json reply;
            done(json_parse(payload, reply) && flag_at(reply, "granted"));
        },
        false);
    engine_emit("permission", json_write(json_object({{"id", json_int(id)}, {"group", json_text(group)}})));
}

std::string text_property(void* object, const char* key) {
    void* value = property_get_object(object, key);
    return value != nullptr ? foundation::nsstring_text(value) : std::string();
}

void* notification_center(void*, const char*) {
    static void* center = objc_framework_object("UNUserNotificationCenter");
    return center;
}

void notifications_request_authorization(void*, const char*, uint64_t, void* handler) {
    void* copy = block_copy(handler);
    request_permission("notifications", [copy](bool granted) { invoke_block<bool, void*>(copy, granted, nullptr); });
}

void* notification_request_make(void*, const char*, void* identifier, void* content, void* trigger) {
    void* request = objc_framework_object("UNNotificationRequest");
    property_set_object(request, "identifier", identifier);
    property_set_object(request, "content", content);
    property_set_object(request, "trigger", trigger);
    return request;
}

void* time_trigger_make(void*, const char*, bool repeats, double interval) {
    void* trigger = objc_framework_object("UNTimeIntervalNotificationTrigger");
    property_set_floats(trigger, "timeInterval", interval, 0);
    property_set_word(trigger, "repeats", repeats);
    return trigger;
}

void notifications_add(void*, const char*, void* request, void* handler) {
    void* content = property_get_object(request, "content");
    void* trigger = property_get_object(request, "trigger");
    const double delay = trigger != nullptr ? property_get_float(trigger, "timeInterval") : 0;
    engine_emit("notify", json_write(json_object({{"identifier", json_text(text_property(request, "identifier"))},
                                                  {"title", json_text(text_property(content, "title"))},
                                                  {"subtitle", json_text(text_property(content, "subtitle"))},
                                                  {"body", json_text(text_property(content, "body"))},
                                                  {"delay", json_real(delay)}})));
    invoke_block<void*>(handler, nullptr);
}

void notifications_remove_pending(void*, const char*, void* identifiers) {
    for (void* identifier : object_array_items(identifiers)) {
        engine_emit("notify-cancel", json_write(json_object({{"identifier", json_text(foundation::nsstring_text(identifier))}})));
    }
}

void notifications_get_pending(void*, const char*, void* handler) { invoke_block<void*>(handler, foundation::new_nsarray()); }

constexpr int64_t kNotificationDenied = 1, kNotificationAuthorized = 2, kNotificationUndetermined = 0;

void notifications_get_settings(void*, const char*, void* handler) {
    const std::string status = engine_host_query("permission-status", "notifications");
    void* settings = objc_framework_object("UNNotificationSettings");
    property_set_word(settings, "authorizationStatus", status == "granted" ? kNotificationAuthorized : status == "denied" ? kNotificationDenied
                                                                                                                          : kNotificationUndetermined);
    invoke_block<void*>(handler, settings);
}

constexpr int32_t kLocationUndetermined = 0, kLocationDenied = 2, kLocationWhenInUse = 4;

int32_t location_status() {
    const std::string status = engine_host_query("permission-status", "location");
    return status == "granted" ? kLocationWhenInUse : status == "denied" ? kLocationDenied
                                                                         : kLocationUndetermined;
}

int32_t location_status_message(void*, const char*) { return location_status(); }

bool location_services_enabled(void*, const char*) { return true; }

using DelegateMessage1 = void (*)(void*, const char*, void*);
using DelegateMessage2 = void (*)(void*, const char*, void*, void*);
using DelegateStatusMessage = void (*)(void*, const char*, void*, int32_t);

void location_tell_status(void* manager) {
    void* delegate = property_get_object(manager, "delegate");
    if (delegate == nullptr) return;
    if (objc_responds(delegate, "locationManagerDidChangeAuthorization:")) {
        reinterpret_cast<DelegateMessage1>(const_cast<void*>(objc_method_for(delegate, "locationManagerDidChangeAuthorization:")))(delegate, "locationManagerDidChangeAuthorization:", manager);
    }
    if (objc_responds(delegate, "locationManager:didChangeAuthorizationStatus:")) {
        reinterpret_cast<DelegateStatusMessage>(const_cast<void*>(objc_method_for(delegate, "locationManager:didChangeAuthorizationStatus:")))(delegate, "locationManager:didChangeAuthorizationStatus:", manager, location_status());
    }
}

void location_request_authorization(void* self, const char*) {
    request_permission("location", [self](bool) { location_tell_status(self); });
}

constexpr double kEarthRadiusMeters = 6371000.0;
constexpr double kRadiansPerDegree = M_PI / 180.0;

void* location_init_coordinates(void* self, const char*, double latitude, double longitude) {
    property_set_floats(self, "coordinate", latitude, longitude);
    return self;
}

void* location_from_fix(const Json& fix) {
    void* location = objc_framework_object("CLLocation");
    property_set_floats(location, "coordinate", number_at(fix, "latitude"), number_at(fix, "longitude"));
    property_set_floats(location, "altitude", number_at(fix, "altitude"), 0);
    property_set_floats(location, "horizontalAccuracy", number_at(fix, "accuracy"), 0);
    property_set_floats(location, "verticalAccuracy", number_at(fix, "verticalAccuracy"), 0);
    property_set_floats(location, "speed", number_at(fix, "speed"), 0);
    property_set_floats(location, "course", number_at(fix, "course"), 0);
    property_set_object(location, "timestamp", new_date_object(number_at(fix, "time")));
    return location;
}

double location_distance(void* self, const char*, void* other) {
    double from[2], to[2];
    property_get_floats(self, "coordinate", from);
    property_get_floats(other, "coordinate", to);
    const double delta_latitude = (to[0] - from[0]) * kRadiansPerDegree;
    const double delta_longitude = (to[1] - from[1]) * kRadiansPerDegree;
    const double a = sin(delta_latitude / 2) * sin(delta_latitude / 2) + cos(from[0] * kRadiansPerDegree) * cos(to[0] * kRadiansPerDegree) * sin(delta_longitude / 2) * sin(delta_longitude / 2);
    return 2 * kEarthRadiusMeters * atan2(sqrt(a), sqrt(1 - a));
}

// The running subscription of each manager, by host request id.
std::map<void*, int>& location_subscriptions() {
    static std::map<void*, int> subscriptions;
    return subscriptions;
}

void location_stop(void* self, const char*) {
    const auto found = location_subscriptions().find(self);
    if (found == location_subscriptions().end()) return;
    engine_emit("location-stop", json_write(json_object({{"id", json_int(found->second)}})));
    engine_host_unregister(found->second);
    location_subscriptions().erase(found);
}

void location_deliver(void* manager, const std::string& payload, bool once) {
    Json fix;
    if (!json_parse(payload, fix)) return;
    void* delegate = property_get_object(manager, "delegate");
    if (fix.member("error") != nullptr) {
        location_stop(manager, nullptr);
        constexpr int64_t kLocationErrorDenied = 1, kLocationErrorUnknown = 0;
        const bool denied = fix.member("error")->text == "denied";
        void* error = foundation_make_error(denied ? "Location access was denied" : "No location provider is available", denied ? kLocationErrorDenied : kLocationErrorUnknown);
        if (delegate != nullptr && objc_responds(delegate, "locationManager:didFailWithError:")) {
            reinterpret_cast<DelegateMessage2>(const_cast<void*>(objc_method_for(delegate, "locationManager:didFailWithError:")))(delegate, "locationManager:didFailWithError:", manager, error);
        }
        return;
    }
    void* location = location_from_fix(fix);
    property_set_object(manager, "location", location);
    if (once) location_stop(manager, nullptr);
    if (delegate == nullptr || !objc_responds(delegate, "locationManager:didUpdateLocations:")) return;
    auto* array = static_cast<foundation::NSArrayObject*>(foundation::new_nsarray());
    array->items->push_back(location);
    reinterpret_cast<DelegateMessage2>(const_cast<void*>(objc_method_for(delegate, "locationManager:didUpdateLocations:")))(delegate, "locationManager:didUpdateLocations:", manager, array);
}

void location_start(void* self, bool once) {
    location_stop(self, nullptr);
    const int id = engine_host_register([self, once](const std::string& payload) { location_deliver(self, payload, once); }, true);
    location_subscriptions()[self] = id;
    engine_emit("location-start", json_write(json_object({{"id", json_int(id)}})));
}

void location_start_updating(void* self, const char*) { location_start(self, false); }
void location_request_one(void* self, const char*) { location_start(self, true); }

// An NWPath is an immediate word holding these bits: never an object, so copies need no reference counting.
constexpr uint64_t kPathImmediate = 1ULL << 63, kPathSatisfied = 1, kPathWifi = 2, kPathCellular = 4, kPathWired = 8;

uint64_t path_from_json(const std::string& text) {
    Json state;
    uint64_t path = kPathImmediate;
    if (!json_parse(text, state)) return path;
    if (flag_at(state, "satisfied")) path |= kPathSatisfied;
    if (flag_at(state, "wifi")) path |= kPathWifi;
    if (flag_at(state, "cellular")) path |= kPathCellular;
    if (flag_at(state, "wired")) path |= kPathWired;
    return path;
}

using PathHandler = SWIFTCC void (*)(const uint64_t*, SWIFT_CONTEXT void*);

struct PathMonitor {
    PathHandler handler = nullptr;
    void* context = nullptr;
    int subscription = 0;
    uint64_t path = kPathImmediate;
};

std::map<void*, PathMonitor>& path_monitors() {
    static std::map<void*, PathMonitor> monitors;
    return monitors;
}

SWIFTCC void* monitor_init(SWIFT_CONTEXT void* self) {
    path_monitors()[self].path = path_from_json(engine_host_query("network-status", ""));
    return self;
}

SWIFTCC void monitor_set_handler(PathHandler handler, void* context, SWIFT_CONTEXT void* self) {
    auto& monitor = path_monitors()[self];
    monitor.handler = handler;
    monitor.context = context;
}

SWIFTCC void monitor_start(void*, SWIFT_CONTEXT void* self) {
    auto& monitor = path_monitors()[self];
    if (monitor.subscription != 0) return;
    monitor.subscription = engine_host_register(
        [self](const std::string& payload) {
            auto& current = path_monitors()[self];
            current.path = path_from_json(payload);
            if (current.handler != nullptr) current.handler(&current.path, current.context);
        },
        true);
    engine_emit("network-start", json_write(json_object({{"id", json_int(monitor.subscription)}})));
}

SWIFTCC void monitor_cancel(SWIFT_CONTEXT void* self) {
    auto& monitor = path_monitors()[self];
    if (monitor.subscription == 0) return;
    engine_emit("network-stop", json_write(json_object({{"id", json_int(monitor.subscription)}})));
    engine_host_unregister(monitor.subscription);
    monitor.subscription = 0;
}

SWIFTCC void monitor_current_path(SWIFT_OUT uint64_t* out, SWIFT_CONTEXT void* self) { *out = path_monitors()[self].path; }

uint64_t satisfied_status_tag() { return static_cast<uint64_t>(*static_cast<const int32_t*>(resolve_symbol("$s7Network6NWPathV6StatusO9satisfiedyA2EmFWC"))); }

constexpr uint64_t kPathUnsatisfiedStatus = UINT64_MAX;

SWIFTCC void path_status(SWIFT_OUT uint64_t* out, SWIFT_CONTEXT const uint64_t* self) { *out = (*self & kPathSatisfied) ? satisfied_status_tag() : kPathUnsatisfiedStatus; }

bool status_equal(const uint64_t* left, const uint64_t* right) { return *left == *right; }

SWIFTCC bool path_uses_interface(const uint64_t* type, SWIFT_CONTEXT const uint64_t* self) {
    const auto matches = [type](const char* symbol) { return static_cast<uint64_t>(*static_cast<const int32_t*>(resolve_symbol(symbol))) == *type; };
    if (matches("$s7Network11NWInterfaceV13InterfaceTypeO4wifiyA2EmFWC")) return (*self & kPathWifi) != 0;
    if (matches("$s7Network11NWInterfaceV13InterfaceTypeO8cellularyA2EmFWC")) return (*self & kPathCellular) != 0;
    if (matches("$s7Network11NWInterfaceV13InterfaceTypeO13wiredEthernetyA2EmFWC")) return (*self & kPathWired) != 0;
    return false;
}

}  // namespace

namespace {

// Android has no home screen widgets of an iOS app's to refresh, so reloading them does nothing.
struct WidgetCenterMetadataResponse {
    void* metadata;
    size_t state;
};

void* widget_center_metadata() {
    static void* block[12] = {};
    return sized_class_metadata(block, reinterpret_cast<void*>(free), 16);
}

SWIFTCC WidgetCenterMetadataResponse widget_center_access(size_t) { return {widget_center_metadata(), 0}; }

SWIFTCC void* widget_center_shared() {
    static auto* const center = [] {
        auto* object = static_cast<void**>(calloc(2, sizeof(void*)));
        object[0] = widget_center_metadata();
        object[1] = reinterpret_cast<void*>(uintptr_t{1});
        return object;
    }();
    return rt_swift_retain(center);
}

SWIFTCC void widget_center_reload(SWIFT_CONTEXT void*) {}

}  // namespace

void* device_symbol(std::string_view name) {
    static const std::unordered_map<std::string_view, void*> table = {
        {"$s9WidgetKit0A6CenterCMa", reinterpret_cast<void*>(widget_center_access)},
        {"$s9WidgetKit0A6CenterC6sharedACvgZ", reinterpret_cast<void*>(widget_center_shared)},
        {"$s9WidgetKit0A6CenterC18reloadAllTimelinesyyFTj", reinterpret_cast<void*>(widget_center_reload)},
        {"$s7Network13NWPathMonitorCACycfc", reinterpret_cast<void*>(monitor_init)},
        {"$s7Network13NWPathMonitorC17pathUpdateHandleryAA0B0VcSgvs", reinterpret_cast<void*>(monitor_set_handler)},
        {"$s7Network13NWPathMonitorC5start5queueySo012OS_dispatch_E0C_tF", reinterpret_cast<void*>(monitor_start)},
        {"$s7Network13NWPathMonitorC6cancelyyF", reinterpret_cast<void*>(monitor_cancel)},
        {"$s7Network13NWPathMonitorC11currentPathAA0B0Vvg", reinterpret_cast<void*>(monitor_current_path)},
        {"$s7Network6NWPathV6statusAC6StatusOvg", reinterpret_cast<void*>(path_status)},
        {"$s7Network6NWPathV6StatusO2eeoiySbAE_AEtFZ", reinterpret_cast<void*>(status_equal)},
        {"$s7Network6NWPathV17usesInterfaceTypeySbAA11NWInterfaceV0dE0OF", reinterpret_cast<void*>(path_uses_interface)},
    };
    const auto found = table.find(name);
    return found == table.end() ? nullptr : found->second;
}

namespace {

void open_url(const std::string& url, std::function<void(bool)> done) {
    const int id = engine_host_register(
        [done = std::move(done)](const std::string& payload) {
            Json reply;
            done(json_parse(payload, reply) && flag_at(reply, "opened"));
        },
        false);
    engine_emit("open-url", json_write(json_object({{"id", json_int(id)}, {"url", json_text(url)}})));
}

void application_open_url_completion(void*, const char*, void* url, void*, void* completion) {
    void* block = completion != nullptr ? darwin_block_copy(completion) : nullptr;
    open_url(foundation::nsstring_text(url), [block](bool opened) { invoke_block<bool>(block, opened); });
}

bool application_open_url(void*, const char*, void* url) {
    open_url(foundation::nsstring_text(url), [](bool) {});
    return true;
}

bool application_can_open_url(void*, const char*, void* url) { return engine_host_query("can-open-url", foundation::nsstring_text(url)) == "true"; }

void* safari_init_with_url(void* self, const char*, void* url) {
    property_set_object(self, "safariURL", url);
    return self;
}

}  // namespace

void register_device_classes() {
    objc_register_method("-[UIApplication openURL:options:completionHandler:]", reinterpret_cast<const void*>(application_open_url_completion));
    objc_register_method("-[UIApplication openURL:]", reinterpret_cast<const void*>(application_open_url));
    objc_register_method("-[UIApplication canOpenURL:]", reinterpret_cast<const void*>(application_can_open_url));
    objc_register_method("-[SFSafariViewController initWithURL:]", reinterpret_cast<const void*>(safari_init_with_url));
    objc_register_method("-[SFSafariViewController initWithURL:configuration:]", reinterpret_cast<const void*>(safari_init_with_url));
    objc_register_method("-[SFSafariViewController initWithURL:entersReaderIfAvailable:]", reinterpret_cast<const void*>(safari_init_with_url));
    objc_register_method("+[UNUserNotificationCenter currentNotificationCenter]", reinterpret_cast<const void*>(notification_center));
    objc_register_method("-[UNUserNotificationCenter requestAuthorizationWithOptions:completionHandler:]", reinterpret_cast<const void*>(notifications_request_authorization));
    objc_register_method("-[UNUserNotificationCenter addNotificationRequest:withCompletionHandler:]", reinterpret_cast<const void*>(notifications_add));
    objc_register_method("-[UNUserNotificationCenter removePendingNotificationRequestsWithIdentifiers:]", reinterpret_cast<const void*>(notifications_remove_pending));
    objc_register_method("-[UNUserNotificationCenter getPendingNotificationRequestsWithCompletionHandler:]", reinterpret_cast<const void*>(notifications_get_pending));
    objc_register_method("-[UNUserNotificationCenter getNotificationSettingsWithCompletionHandler:]", reinterpret_cast<const void*>(notifications_get_settings));
    objc_register_method("+[UNNotificationRequest requestWithIdentifier:content:trigger:]", reinterpret_cast<const void*>(notification_request_make));
    objc_register_method("+[UNTimeIntervalNotificationTrigger triggerWithTimeInterval:repeats:]", reinterpret_cast<const void*>(time_trigger_make));
    objc_register_method("+[CLLocationManager authorizationStatus]", reinterpret_cast<const void*>(location_status_message));
    objc_register_method("-[CLLocationManager authorizationStatus]", reinterpret_cast<const void*>(location_status_message));
    objc_register_method("+[CLLocationManager locationServicesEnabled]", reinterpret_cast<const void*>(location_services_enabled));
    objc_register_method("-[CLLocationManager requestWhenInUseAuthorization]", reinterpret_cast<const void*>(location_request_authorization));
    objc_register_method("-[CLLocationManager requestAlwaysAuthorization]", reinterpret_cast<const void*>(location_request_authorization));
    objc_register_method("-[CLLocationManager startUpdatingLocation]", reinterpret_cast<const void*>(location_start_updating));
    objc_register_method("-[CLLocationManager stopUpdatingLocation]", reinterpret_cast<const void*>(location_stop));
    objc_register_method("-[CLLocationManager requestLocation]", reinterpret_cast<const void*>(location_request_one));
    objc_register_method("-[CLLocation initWithLatitude:longitude:]", reinterpret_cast<const void*>(location_init_coordinates));
    objc_register_method("-[CLLocation distanceFromLocation:]", reinterpret_cast<const void*>(location_distance));
}
