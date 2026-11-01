#include "foundation_internal.h"

#include <sys/statvfs.h>
#include "macho.h"

#include <poll.h>

#include <future>
#include <thread>

namespace foundation {

SWIFTCC void url_documents(SWIFT_OUT URLBox** out) { *out = new_url("file://" + engine_bundle_dir() + "/.Documents/"); }

std::string appended_component(const std::string& text, const std::string& component, bool directory) {
    const std::string base = text_without_query(text);
    std::string joined = base;
    if (joined.empty() || joined.back() != '/') joined += '/';
    joined += component;
    if (directory && (joined.empty() || joined.back() != '/')) joined += '/';
    return joined;
}

SWIFTCC void url_appending_component(SWIFT_OUT URLBox** out, SwiftString component, SWIFT_CONTEXT URLBox** self) {
    *out = new_url(appended_component(*(*self)->text, swift_string_to_utf8(component), false));
}

SWIFTCC void url_appending_directory(SWIFT_OUT URLBox** out, SwiftString component, bool directory, SWIFT_CONTEXT URLBox** self) {
    *out = new_url(appended_component(*(*self)->text, swift_string_to_utf8(component), directory));
}

SWIFTCC void url_appending_extension(SWIFT_OUT URLBox** out, SwiftString extension, SWIFT_CONTEXT URLBox** self) {
    std::string base = text_without_query(*(*self)->text);
    while (base.size() > 1 && base.back() == '/') base.pop_back();
    *out = new_url(base + "." + swift_string_to_utf8(extension));
}

SWIFTCC void url_deleting_extension(SWIFT_OUT URLBox** out, SWIFT_CONTEXT URLBox** self) {
    std::string base = text_without_query(*(*self)->text);
    while (base.size() > 1 && base.back() == '/') base.pop_back();
    const size_t dot = base.rfind('.');
    const size_t slash = base.rfind('/');
    if (dot != std::string::npos && (slash == std::string::npos || dot > slash + 1)) base.erase(dot);
    *out = new_url(base);
}

std::string file_url_text(const std::string& path);

// Removes "." and ".." segments, keeping a trailing slash, the one change standardizing makes that callers can see.
std::string standardized_path(const std::string& path) {
    std::vector<std::string> parts;
    std::stringstream stream(path);
    std::string part;
    while (std::getline(stream, part, '/')) {
        if (part.empty() || part == ".") continue;
        if (part == "..") {
            if (!parts.empty()) parts.pop_back();
        } else {
            parts.push_back(part);
        }
    }
    std::string out;
    for (const auto& piece : parts) out += "/" + piece;
    if (out.empty() || (!path.empty() && path.back() == '/')) out += "/";
    return out;
}

SWIFTCC void url_standardized(SWIFT_OUT URLBox** out, SWIFT_CONTEXT URLBox** self) {
    const std::string& text = *(*self)->text;
    if (text.rfind("file:", 0) != 0) {
        *out = static_cast<URLBox*>(rt_swift_retain(*self));
        return;
    }
    *out = new_url(file_url_text(standardized_path(url_path(text))));
}

SWIFTCC void url_resolving_symlinks(SWIFT_OUT URLBox** out, SWIFT_CONTEXT URLBox** self) {
    char resolved[PATH_MAX];
    const std::string path = url_path(*(*self)->text);
    *out = new_url(file_url_text(realpath(path.c_str(), resolved) != nullptr ? std::string(resolved) : standardized_path(path)));
}

SWIFTCC bool url_is_reachable(SWIFT_CONTEXT URLBox** self, SWIFT_ERROR void** error) {
    if (access(url_path(*(*self)->text).c_str(), F_OK) == 0) return true;
    *error = make_error("The file doesn't exist.", ENOENT);
    return false;
}

SWIFTCC void url_stop_accessing(SWIFT_CONTEXT URLBox**) {}

SWIFTCC void url_file_with_path_directory(SWIFT_OUT URLBox** out, SwiftString path, bool directory) {
    std::string text = swift_string_to_utf8(path);
    if (directory && (text.empty() || text.back() != '/')) text += "/";
    *out = new_url(file_url_text(text));
}

// URLResourceValues: one reference to what stat and statvfs said when it was read.
struct ResourceValues {
    HeapObject header;
    std::string* name;
    bool exists, is_directory, is_regular;
    int64_t size, allocated, volume_total, volume_available;
    double access_time, modification_time;
    bool has_access_time;
};

void destroy_resource_values(void* raw) {
    delete static_cast<ResourceValues*>(raw)->name;
    free(raw);
}

ResourceValues* new_resource_values() {
    static void* cls = objc_native_class("URLResourceValues", destroy_resource_values);
    auto* values = static_cast<ResourceValues*>(calloc(1, sizeof(ResourceValues)));
    values->header.metadata = cls;
    values->header.references = 1;
    return values;
}

double seconds_since_2001(const timespec& time) { return static_cast<double>(time.tv_sec) + static_cast<double>(time.tv_nsec) / 1e9 - kSecondsFrom1970To2001; }

// Every value is filled whichever keys were asked for; callers only read the ones they named.
SWIFTCC void url_resource_values(SWIFT_OUT ResourceValues** out, void*, SWIFT_CONTEXT URLBox** self, SWIFT_ERROR void** error) {
    const std::string path = url_path(*(*self)->text);
    struct stat info {};
    if (lstat(path.c_str(), &info) != 0) {
        *error = make_error("The file doesn't exist.", ENOENT);
        return;
    }
    auto* values = new_resource_values();
    values->exists = true;
    values->name = new std::string(path_last_component(path));
    values->is_directory = S_ISDIR(info.st_mode);
    values->is_regular = S_ISREG(info.st_mode);
    values->size = info.st_size;
    constexpr int64_t kStatBlockBytes = 512;
    values->allocated = info.st_blocks * kStatBlockBytes;
    values->access_time = seconds_since_2001(info.st_atim);
    values->modification_time = seconds_since_2001(info.st_mtim);
    struct statvfs volume {};
    if (statvfs(path.c_str(), &volume) == 0) {
        values->volume_total = static_cast<int64_t>(volume.f_blocks * volume.f_frsize);
        values->volume_available = static_cast<int64_t>(volume.f_bavail * volume.f_frsize);
    }
    *out = values;
}

SWIFTCC void resource_values_empty(SWIFT_OUT ResourceValues** out) { *out = new_resource_values(); }

struct ResourceOptionalInt {
    int64_t value;
    uint64_t is_nil;
};

struct ResourceOptionalDate {
    double value;
    uint8_t is_nil;
};

constexpr uint8_t kOptionalBoolNil = 2;

SWIFTCC uint8_t resource_is_directory(SWIFT_CONTEXT ResourceValues** self) { return (*self)->exists ? (*self)->is_directory : kOptionalBoolNil; }
SWIFTCC uint8_t resource_is_regular(SWIFT_CONTEXT ResourceValues** self) { return (*self)->exists ? (*self)->is_regular : kOptionalBoolNil; }
SWIFTCC SwiftString resource_name(SWIFT_CONTEXT ResourceValues** self) { return (*self)->name != nullptr ? swift_string_from_utf8(*(*self)->name) : SwiftString{0, 0}; }

template <int64_t ResourceValues::*field>
SWIFTCC ResourceOptionalInt resource_integer(SWIFT_CONTEXT ResourceValues** self) { return {(*self)->*field, (*self)->exists ? 0ULL : 1ULL}; }

template <double ResourceValues::*field>
SWIFTCC void resource_date(SWIFT_OUT ResourceOptionalDate* out, SWIFT_CONTEXT ResourceValues** self) { *out = {(*self)->*field, static_cast<uint8_t>((*self)->exists ? 0 : 1)}; }

SWIFTCC void resource_set_access_date(const ResourceOptionalDate* date, SWIFT_CONTEXT ResourceValues** self) {
    (*self)->has_access_time = !date->is_nil;
    (*self)->access_time = date->value;
}

SWIFTCC void url_set_resource_values(ResourceValues** values, SWIFT_CONTEXT URLBox** self, SWIFT_ERROR void** error) {
    if (!(*values)->has_access_time) return;
    const std::string path = url_path(*(*self)->text);
    struct stat info {};
    if (stat(path.c_str(), &info) != 0) {
        *error = make_error("The file doesn't exist.", ENOENT);
        return;
    }
    const double seconds = (*values)->access_time + kSecondsFrom1970To2001;
    timespec times[2];
    times[0].tv_sec = static_cast<time_t>(seconds);
    times[0].tv_nsec = 0;
    times[1] = info.st_mtim;
    utimensat(AT_FDCWD, path.c_str(), times, 0);
}

SWIFTCC SwiftString url_last_component(SWIFT_CONTEXT URLBox** self) { return swift_string_from_utf8(path_last_component(url_path(*(*self)->text))); }

SWIFTCC SwiftString url_path_extension(SWIFT_CONTEXT URLBox** self) {
    const std::string name = path_last_component(url_path(*(*self)->text));
    const size_t dot = name.rfind('.');
    return swift_string_from_utf8(dot == std::string::npos || dot == 0 ? std::string() : name.substr(dot + 1));
}

SWIFTCC SwiftString url_path_percent(bool, SWIFT_CONTEXT URLBox** self) { return swift_string_from_utf8(url_path(*(*self)->text)); }

SWIFTCC bool url_is_file(SWIFT_CONTEXT URLBox** self) { return (*self)->text->rfind("file:", 0) == 0; }

SWIFTCC bool url_has_directory_path(SWIFT_CONTEXT URLBox** self) {
    const std::string base = text_without_query(*(*self)->text);
    return !base.empty() && base.back() == '/';
}

SWIFTCC bool url_equal(URLBox** left, URLBox** right) { return *(*left)->text == *(*right)->text; }

SWIFTCC bool url_start_security_scope(SWIFT_CONTEXT void*) { return true; }

SWIFTCC void url_absolute(SWIFT_OUT URLBox** out, SWIFT_CONTEXT URLBox** self) { *out = new_url(*(*self)->text); }

SWIFTCC void url_no_base(SWIFT_OUT URLBox** out, SWIFT_CONTEXT void*) { *out = nullptr; }

struct UrlAuthority {
    std::string host;
    int64_t port = -1;
};

UrlAuthority url_authority(const std::string& text) {
    UrlAuthority result;
    const size_t scheme = text.find("://");
    if (scheme == std::string::npos) return result;
    const size_t start = scheme + 3;
    std::string authority = text.substr(start, text.find_first_of("/?#", start) - start);
    if (const size_t at = authority.rfind('@'); at != std::string::npos) authority.erase(0, at + 1);
    const size_t colon = authority.rfind(':');
    if (colon != std::string::npos && authority.find(']', colon) == std::string::npos) {
        result.port = strtol(authority.c_str() + colon + 1, nullptr, 10);
        authority.erase(colon);
    }
    result.host = authority;
    return result;
}

SWIFTCC void url_string_relative(SWIFT_OUT URLBox** out, SwiftString value, URLBox** base) {
    const std::string relative = swift_string_to_utf8(value);
    if (base == nullptr || *base == nullptr || relative.find("://") != std::string::npos) {
        *out = new_url(relative);
        return;
    }
    *out = new_url(appended_component(*(*base)->text, relative, false));
}

struct OptionalIntResult {
    int64_t value;
    bool is_none;
};

SWIFTCC OptionalIntResult url_port(SWIFT_CONTEXT URLBox** self) {
    const int64_t port = url_authority(*(*self)->text).port;
    return {port < 0 ? 0 : port, port < 0};
}

SWIFTCC void url_application_support(SWIFT_OUT URLBox** out) {
    const std::string directory = engine_bundle_dir() + "/.Library/Application Support";
    make_directories(directory);
    *out = new_url(kFileScheme + directory + "/");
}

SWIFTCC void url_from_string(SWIFT_OUT URLBox** out, SwiftString value) {
    const std::string text = swift_string_to_utf8(value);
    *out = text.empty() ? nullptr : new_url(text);
}

SWIFTCC SwiftString url_absolute_string(SWIFT_CONTEXT URLBox** self) { return swift_string_from_utf8(*(*self)->text); }

SWIFTCC SwiftString url_path_getter(SWIFT_CONTEXT URLBox** self) { return swift_string_from_utf8(url_path(*(*self)->text)); }

SWIFTCC OptionalSwiftString url_host(SWIFT_CONTEXT URLBox** self) {
    const std::string host = url_authority(*(*self)->text).host;
    if (host.empty()) return {0, 0};
    const SwiftString value = swift_string_from_utf8(host);
    return {value.count_and_flags, value.object};
}

SWIFTCC OptionalSwiftString url_query(SWIFT_CONTEXT URLBox** self) {
    const std::string& text = *(*self)->text;
    const size_t question = text.find('?');
    if (question == std::string::npos) return {0, 0};
    const SwiftString value = swift_string_from_utf8(text.substr(question + 1, text.find('#', question) - question - 1));
    return {value.count_and_flags, value.object};
}

SWIFTCC OptionalSwiftString url_scheme(SWIFT_CONTEXT URLBox** self) {
    const size_t end = (*self)->text->find(':');
    if (end == std::string::npos) return {0, 0};
    const SwiftString scheme = swift_string_from_utf8((*self)->text->substr(0, end));
    return {scheme.count_and_flags, scheme.object};
}

SWIFTCC void url_appending_path(SWIFT_OUT URLBox** out, const SwiftString* component, const void*, void*, void*, SWIFT_CONTEXT URLBox** self) {
    std::string base = *(*self)->text;
    if (!base.empty() && base.back() != '/') base += '/';
    *out = new_url(base + swift_string_to_utf8(*component));
}

SWIFTCC void url_deleting_last_component(SWIFT_OUT URLBox** out, SWIFT_CONTEXT URLBox** self) {
    std::string text = *(*self)->text;
    if (!text.empty() && text.back() == '/') text.pop_back();
    const size_t slash = text.rfind('/');
    *out = new_url(slash == std::string::npos ? text : text.substr(0, slash + 1));
}

void* g_nsurl_class = nullptr;

URLBox* make_url_box(const std::string& text) { return new_url(text); }

void* new_nsurl(const std::string& text) {
    auto* object = static_cast<NSStringObject*>(new_nsstring(text));
    object->header.metadata = g_nsurl_class;
    return object;
}

std::string file_url_text(const std::string& path) { return "file://" + path; }

std::string nsobject_path(void* object) {
    const std::string& text = nsstring_text(object);
    return static_cast<HeapObject*>(object)->metadata == g_nsurl_class ? url_path(text) : text;
}

void set_cocoa_error(void** error, const std::string& message) {
    if (error != nullptr) *error = foundation_make_error(message, errno);
}

void* bundle_url(void*, const char*) { return new_nsurl(file_url_text(engine_bundle_dir() + "/")); }

void* bundle_path(void*, const char*) { return new_nsstring(engine_bundle_dir()); }

std::string executable_file() { return engine_images().back()->path; }

void* bundle_executable_path(void*, const char*) { return new_nsstring(executable_file()); }
void* bundle_executable_url(void*, const char*) { return new_nsurl(file_url_text(executable_file())); }
void* bundle_plugins_url(void*, const char*) { return new_nsurl(file_url_text(engine_bundle_dir() + "/PlugIns/")); }

void* bundle_path_for_resource(void*, const char*, void* name, void* type) {
    std::string path = engine_bundle_dir() + "/" + (name != nullptr ? nsstring_text(name) : std::string());
    if (type != nullptr && !nsstring_text(type).empty()) path += "." + nsstring_text(type);
    return access(path.c_str(), F_OK) == 0 ? new_nsstring(path) : nullptr;
}

void* bundle_urls_with_extension(void*, const char*, void* extension, void* subdirectory) {
    auto* array = static_cast<NSArrayObject*>(new_nsarray());
    std::string directory = engine_bundle_dir();
    if (subdirectory != nullptr && !nsstring_text(subdirectory).empty()) directory += "/" + nsstring_text(subdirectory);
    const std::string suffix = "." + nsstring_text(extension);
    if (DIR* listing = opendir(directory.c_str())) {
        while (const dirent* entry = readdir(listing)) {
            const std::string name = entry->d_name;
            if (name.size() > suffix.size() && name.compare(name.size() - suffix.size(), suffix.size(), suffix) == 0) array->items->push_back(new_nsurl(file_url_text(directory + "/" + name)));
        }
        closedir(listing);
    }
    return array;
}

// Only the app's own bundle exists here, so every way of asking for a bundle finds that one.
void* bundle_main_for_any(void* self, const char*, void*) { return self; }

void* bundle_with_identifier(void*, const char*, void* identifier) {
    if (identifier != nullptr && nsstring_text(identifier) != bundle_info_string("CFBundleIdentifier")) return nullptr;
    using MainBundle = void* (*)(void*, const char*);
    void* cls = objc_class_named("NSBundle");
    return reinterpret_cast<MainBundle>(const_cast<void*>(objc_method_for(cls, "mainBundle")))(cls, "mainBundle");
}

// Only the app's own bundle can be loaded; another directory has nothing a shim could read as a bundle yet.
void* bundle_with_url(void*, const char*, void* url) {
    std::string path = nsobject_path(url);
    while (path.size() > 1 && path.back() == '/') path.pop_back();
    return path == engine_bundle_dir() ? bundle_with_identifier(nullptr, nullptr, nullptr) : nullptr;
}

void* bundle_all(void*, const char*) {
    auto* array = static_cast<NSArrayObject*>(new_nsarray());
    array->items->push_back(bundle_with_identifier(nullptr, nullptr, nullptr));
    return array;
}

void* bundle_localized_string(void*, const char*, void* key, void* value, void*) {
    const std::string found = localization_lookup(nsstring_text(key));
    if (found != nsstring_text(key)) return new_nsstring(found);
    return value != nullptr && !nsstring_text(value).empty() ? value : key;
}

void* bundle_url_for_resource(void*, const char*, void* name, void* extension) {
    std::string path = engine_bundle_dir() + "/" + (name == nullptr ? std::string() : nsstring_text(name));
    if (extension != nullptr && !nsstring_text(extension).empty()) path += "." + nsstring_text(extension);
    return access(path.c_str(), F_OK) == 0 ? new_nsurl(file_url_text(path)) : nullptr;
}

constexpr int64_t kDocumentDirectory = 9;

constexpr int64_t kCachesDirectory = 13;

std::string sandbox_directory(int64_t directory) {
    switch (directory) {
        case kDocumentDirectory:
            return engine_bundle_dir() + "/.Documents";
        case kCachesDirectory:
            return engine_bundle_dir() + "/.Library/Caches";
        default:
            return engine_bundle_dir() + "/.Library/Application Support";
    }
}

void make_directory_tree(const std::string& path) {
    for (size_t at = path.find('/', 1); at != std::string::npos; at = path.find('/', at + 1)) mkdir(path.substr(0, at).c_str(), 0700);
    mkdir(path.c_str(), 0700);
}

void* file_manager_urls(void*, const char*, int64_t directory, uint64_t) {
    const std::string path = sandbox_directory(directory);
    make_directory_tree(path);
    auto* array = static_cast<NSArrayObject*>(new_nsarray());
    array->items->push_back(new_nsurl(file_url_text(path + "/")));
    return array;
}

void* file_manager_temporary_directory(void*, const char*) {
    const std::string path = engine_bundle_dir() + "/.tmp";
    make_directory_tree(path);
    return new_nsurl(file_url_text(path + "/"));
}

bool file_manager_exists_directory(void*, const char*, void* path, bool* is_directory) {
    struct stat info {};
    if (stat(nsstring_text(path).c_str(), &info) != 0) return false;
    if (is_directory != nullptr) *is_directory = S_ISDIR(info.st_mode);
    return true;
}

bool file_manager_create_file(void*, const char*, void* path, void* contents, void*) {
    std::ofstream output(nsstring_text(path), std::ios::binary | std::ios::trunc);
    if (!output) return false;
    if (contents != nullptr) output << *static_cast<NSDataObject*>(contents)->bytes;
    return output.good();
}

void* file_manager_contents_at_path(void*, const char*, void* path) {
    std::ifstream input(nsstring_text(path), std::ios::binary);
    if (!input) return nullptr;
    return make_nsdata(std::string(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()));
}

bool file_manager_is_readable(void*, const char*, void* path) { return access(nsstring_text(path).c_str(), R_OK) == 0; }

bool file_manager_move_path(void*, const char*, void* from, void* to, void** error) {
    if (rename(nsstring_text(from).c_str(), nsstring_text(to).c_str()) == 0) return true;
    set_cocoa_error(error, "Could not move " + nsstring_text(from));
    return false;
}

bool file_manager_create_symlink(void*, const char*, void* path, void* destination, void** error) {
    if (symlink(nsstring_text(destination).c_str(), nsstring_text(path).c_str()) == 0) return true;
    set_cocoa_error(error, "Could not link " + nsstring_text(path));
    return false;
}

void* file_manager_link_destination(void*, const char*, void* path, void** error) {
    char target[PATH_MAX];
    const ssize_t length = readlink(nsstring_text(path).c_str(), target, sizeof target);
    if (length >= 0) return new_nsstring(std::string(target, static_cast<size_t>(length)));
    set_cocoa_error(error, "No link at " + nsstring_text(path));
    return nullptr;
}

void collect_subpaths(const std::string& root, const std::string& prefix, NSArrayObject* array) {
    DIR* directory = opendir((root + "/" + prefix).c_str());
    if (directory == nullptr) return;
    while (const dirent* entry = readdir(directory)) {
        const std::string name = entry->d_name;
        if (name == "." || name == "..") continue;
        const std::string relative = prefix.empty() ? name : prefix + "/" + name;
        array->items->push_back(new_nsstring(relative));
        if (entry->d_type == DT_DIR) collect_subpaths(root, relative, array);
    }
    closedir(directory);
}

void* file_manager_subpaths(void*, const char*, void* path, void**) {
    auto* array = static_cast<NSArrayObject*>(new_nsarray());
    collect_subpaths(nsstring_text(path), "", array);
    return array;
}

// File attributes here are only permissions and dates, which this sandbox doesn't keep apart from the file.
bool file_manager_set_attributes(void*, const char*, void*, void*, void**) { return true; }

const char* file_manager_representation(void*, const char*, void* path) { return strdup(nsstring_text(path).c_str()); }

void* file_manager_url_for_directory(void*, const char*, int64_t directory, uint64_t, void*, bool, void**) {
    const std::string path = sandbox_directory(directory);
    make_directory_tree(path);
    return new_nsurl(file_url_text(path + "/"));
}

// An app group's container is one more private directory, since nothing else shares this sandbox.
void* file_manager_group_container(void*, const char*, void* group) {
    const std::string path = engine_bundle_dir() + "/.AppGroups/" + nsstring_text(group);
    make_directory_tree(path);
    return new_nsurl(file_url_text(path + "/"));
}

// A file handle's descriptor lives in the first word after its object header.
constexpr size_t kFileHandleDescriptorWord = 2;

int& file_handle_descriptor(void* handle) { return *reinterpret_cast<int*>(&static_cast<void**>(handle)[kFileHandleDescriptorWord]); }

void* make_file_handle(int descriptor) {
    void* handle = objc_framework_object("NSFileHandle");
    file_handle_descriptor(handle) = descriptor;
    return handle;
}

void* open_file_handle(const std::string& path, int flags, void** error) {
    const int descriptor = open(path.c_str(), flags | O_CLOEXEC);
    if (descriptor >= 0) return make_file_handle(descriptor);
    if (error != nullptr) set_cocoa_error(error, "Could not open " + path);
    return nullptr;
}

void* handle_writing_url(void*, const char*, void* url, void** error) { return open_file_handle(nsobject_path(url), O_WRONLY, error); }
void* handle_reading_url(void*, const char*, void* url, void** error) { return open_file_handle(nsobject_path(url), O_RDONLY, error); }
void* handle_updating_url(void*, const char*, void* url, void** error) { return open_file_handle(nsobject_path(url), O_RDWR, error); }
void* handle_writing_path(void*, const char*, void* path) { return open_file_handle(nsstring_text(path), O_WRONLY, nullptr); }
void* handle_reading_path(void*, const char*, void* path) { return open_file_handle(nsstring_text(path), O_RDONLY, nullptr); }
void* handle_updating_path(void*, const char*, void* path) { return open_file_handle(nsstring_text(path), O_RDWR, nullptr); }
void* handle_standard_output(void*, const char*) { return make_file_handle(STDOUT_FILENO); }
void* handle_standard_error(void*, const char*) { return make_file_handle(STDERR_FILENO); }
void* handle_standard_input(void*, const char*) { return make_file_handle(STDIN_FILENO); }
void* handle_null_device(void*, const char*) { return make_file_handle(open("/dev/null", O_RDWR | O_CLOEXEC)); }

void* handle_init_descriptor(void* self, const char*, int descriptor) {
    file_handle_descriptor(self) = descriptor;
    return self;
}

int handle_get_descriptor(void* self, const char*) { return file_handle_descriptor(self); }

void handle_close(void* self, const char*) {
    close(file_handle_descriptor(self));
    file_handle_descriptor(self) = -1;
}

bool handle_close_error(void* self, const char*, void**) {
    handle_close(self, nullptr);
    return true;
}

bool write_all(int descriptor, const std::string& bytes) {
    size_t written = 0;
    while (written < bytes.size()) {
        const ssize_t count = write(descriptor, bytes.data() + written, bytes.size() - written);
        if (count <= 0) return false;
        written += static_cast<size_t>(count);
    }
    return true;
}

void handle_write(void* self, const char*, void* data) { write_all(file_handle_descriptor(self), *static_cast<NSDataObject*>(data)->bytes); }

bool handle_write_error(void* self, const char*, void* data, void** error) {
    if (write_all(file_handle_descriptor(self), *static_cast<NSDataObject*>(data)->bytes)) return true;
    set_cocoa_error(error, "Could not write to the file handle");
    return false;
}

std::string read_up_to(int descriptor, size_t limit) {
    std::string bytes;
    char buffer[65536];
    while (bytes.size() < limit) {
        const size_t wanted = limit - bytes.size() < sizeof buffer ? limit - bytes.size() : sizeof buffer;
        const ssize_t count = read(descriptor, buffer, wanted);
        if (count <= 0) break;
        bytes.append(buffer, static_cast<size_t>(count));
    }
    return bytes;
}

void* handle_read_to_end(void* self, const char*) { return make_nsdata(read_up_to(file_handle_descriptor(self), SIZE_MAX)); }
void* handle_read_to_end_error(void* self, const char*, void**) { return make_nsdata(read_up_to(file_handle_descriptor(self), SIZE_MAX)); }
void* handle_read_length(void* self, const char*, uint64_t length) { return make_nsdata(read_up_to(file_handle_descriptor(self), length)); }
void* handle_read_length_error(void* self, const char*, uint64_t length, void**) { return make_nsdata(read_up_to(file_handle_descriptor(self), length)); }

uint64_t handle_seek_end(void* self, const char*) { return static_cast<uint64_t>(lseek(file_handle_descriptor(self), 0, SEEK_END)); }

bool handle_seek_end_error(void* self, const char*, uint64_t* offset, void**) {
    const off_t position = lseek(file_handle_descriptor(self), 0, SEEK_END);
    if (offset != nullptr) *offset = static_cast<uint64_t>(position);
    return position >= 0;
}

void handle_seek(void* self, const char*, uint64_t offset) { lseek(file_handle_descriptor(self), static_cast<off_t>(offset), SEEK_SET); }

bool handle_seek_error(void* self, const char*, uint64_t offset, void**) { return lseek(file_handle_descriptor(self), static_cast<off_t>(offset), SEEK_SET) >= 0; }

uint64_t handle_offset(void* self, const char*) { return static_cast<uint64_t>(lseek(file_handle_descriptor(self), 0, SEEK_CUR)); }

void handle_sync(void* self, const char*) { fsync(file_handle_descriptor(self)); }

bool handle_sync_error(void* self, const char*, void**) { return fsync(file_handle_descriptor(self)) == 0; }

// DataProtocol values reaching this are always Data, the only conformer that gets written to a file handle.
SWIFTCC void handle_write_contents(const FoundationData* data, void*, void*, SWIFT_CONTEXT void* self, SWIFT_ERROR void** error) {
    if (!write_all(file_handle_descriptor(self), foundation_data_bytes(*data))) set_cocoa_error(error, "Could not write to the file handle");
}

void handle_truncate(void* self, const char*, uint64_t offset) { ftruncate(file_handle_descriptor(self), static_cast<off_t>(offset)); }

void* handle_available_data(void* self, const char*) {
    char buffer[65536];
    const ssize_t count = read(file_handle_descriptor(self), buffer, sizeof buffer);
    return make_nsdata(count > 0 ? std::string(buffer, static_cast<size_t>(count)) : std::string());
}

struct ReadabilityWatch {
    std::atomic<void*> block{nullptr};
    bool thread_started = false;
};

std::mutex g_watch_lock;
std::map<void*, ReadabilityWatch*> g_watches;

// Blocks a next delivery until the handler has drained what woke it, as a level-triggered poll would otherwise spin.
void watch_readability(void* handle, ReadabilityWatch* watch) {
    constexpr size_t kBlockInvokeWord = 2;
    using Invoke = void (*)(void*, void*);
    pollfd descriptor{file_handle_descriptor(handle), POLLIN, 0};
    while (poll(&descriptor, 1, -1) > 0) {
        if (descriptor.revents & (POLLERR | POLLNVAL)) return;
        auto done = std::make_shared<std::promise<void>>();
        engine_post_event([handle, watch, done] {
            if (void* block = watch->block.load()) reinterpret_cast<Invoke>(static_cast<void**>(block)[kBlockInvokeWord])(block, handle);
            done->set_value();
        });
        done->get_future().wait();
        if (descriptor.revents & POLLHUP) return;
    }
}

void handle_set_readability(void* self, const char*, void* block) {
    const std::lock_guard guard(g_watch_lock);
    ReadabilityWatch*& watch = g_watches[self];
    if (watch == nullptr) watch = new ReadabilityWatch;
    watch->block.store(block != nullptr ? darwin_block_copy(block) : nullptr);
    if (block != nullptr && !watch->thread_started) {
        watch->thread_started = true;
        std::thread(watch_readability, self, watch).detach();
    }
}

void* handle_get_readability(void* self, const char*) {
    const std::lock_guard guard(g_watch_lock);
    const auto found = g_watches.find(self);
    return found == g_watches.end() ? nullptr : found->second->block.load();
}

constexpr size_t kPipeReadWord = 2, kPipeWriteWord = 3;

void* pipe_init(void* self, const char*) {
    int ends[2];
    if (pipe(ends) != 0) return nullptr;
    static_cast<void**>(self)[kPipeReadWord] = make_file_handle(ends[0]);
    static_cast<void**>(self)[kPipeWriteWord] = make_file_handle(ends[1]);
    return self;
}

void* pipe_make(void*, const char*) { return pipe_init(objc_framework_object("NSPipe"), nullptr); }
void* pipe_reading(void* self, const char*) { return static_cast<void**>(self)[kPipeReadWord]; }
void* pipe_writing(void* self, const char*) { return static_cast<void**>(self)[kPipeWriteWord]; }

// No other process shares these files, so coordination just runs the accessor at once.
constexpr size_t kIntentUrlWord = 2;
constexpr size_t kBlockInvokeWord = 2;

void* intent_make(void*, const char*, void* url, uint64_t) {
    auto** intent = static_cast<void**>(objc_framework_object("NSFileAccessIntent"));
    intent[kIntentUrlWord] = rt_swift_retain(url);
    return intent;
}

void* intent_url(void* self, const char*) { return static_cast<void**>(self)[kIntentUrlWord]; }

void* coordinator_init(void* self, const char*, void*) { return self; }

void coordinate_intents(void*, const char*, void*, void*, void* accessor) {
    using Invoke = void (*)(void*, void*);
    reinterpret_cast<Invoke>(static_cast<void**>(accessor)[kBlockInvokeWord])(accessor, nullptr);
}

void coordinate_item(void*, const char*, void* url, uint64_t, void**, void* accessor) {
    using Invoke = void (*)(void*, void*);
    reinterpret_cast<Invoke>(static_cast<void**>(accessor)[kBlockInvokeWord])(accessor, url);
}

bool file_manager_create_path(void*, const char*, void* path, bool, void*, void**) {
    make_directory_tree(nsobject_path(path));
    return true;
}

bool remove_recursively(const std::string& path) {
    struct stat info {};
    if (lstat(path.c_str(), &info) != 0) return false;
    if (S_ISDIR(info.st_mode)) {
        if (DIR* directory = opendir(path.c_str())) {
            while (const dirent* entry = readdir(directory)) {
                const std::string name = entry->d_name;
                if (name != "." && name != "..") remove_recursively(path + "/" + name);
            }
            closedir(directory);
        }
        return rmdir(path.c_str()) == 0;
    }
    return unlink(path.c_str()) == 0;
}

bool file_manager_remove(void*, const char*, void* item, void** error) {
    if (remove_recursively(nsobject_path(item))) return true;
    set_cocoa_error(error, "Could not remove " + nsobject_path(item));
    return false;
}

bool file_manager_move(void*, const char*, void* from, void* to, void** error) {
    if (rename(nsobject_path(from).c_str(), nsobject_path(to).c_str()) == 0) return true;
    set_cocoa_error(error, "Could not move " + nsobject_path(from));
    return false;
}

// The app's data containers live inside its bundle directory here, unlike on iOS, where a bundle holds only the app.
// String(contentsOf:): an unreadable file is an error, as it is there.
SWIFTCC SwiftString string_contents_of_url(URLBox** url, SWIFT_CONTEXT void*, SWIFT_ERROR void** error) {
    constexpr int64_t kCocoaFileReadNoSuchFile = 260;
    std::ifstream file(url_path(*(*url)->text), std::ios::binary);
    if (!file) {
        *error = make_error("The file couldn't be opened because there is no such file.", kCocoaFileReadNoSuchFile);
        return {0, 0};
    }
    return swift_string_from_utf8(std::string(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()));
}

bool is_data_container(const std::string& path) {
    const std::string& bundle = engine_bundle_dir();
    if (path.rfind(bundle + "/", 0) != 0) return false;
    const std::string rest = path.substr(bundle.size() + 1);
    return rest.find('/') == std::string::npos && !rest.empty() && rest[0] == '.';
}

bool copy_recursively(const std::string& from, const std::string& to) {
    if (to == from || is_data_container(from)) return true;
    struct stat info {};
    if (stat(from.c_str(), &info) != 0) return false;
    if (S_ISDIR(info.st_mode)) {
        make_directory_tree(to);
        DIR* directory = opendir(from.c_str());
        if (directory == nullptr) return false;
        bool copied = true;
        while (const dirent* entry = readdir(directory)) {
            const std::string name = entry->d_name;
            if (name == "." || name == "..") continue;
            const std::string child = from + "/" + name;
            // Copying a directory into itself would never end, as each copy adds more to copy.
            if (to == child || to.rfind(child + "/", 0) == 0) continue;
            copied = copy_recursively(child, to + "/" + name) && copied;
        }
        closedir(directory);
        return copied;
    }
    FILE* input = fopen(from.c_str(), "rb");
    if (input == nullptr) return false;
    FILE* output = fopen(to.c_str(), "wb");
    if (output == nullptr) {
        fclose(input);
        return false;
    }
    char chunk[65536];
    for (size_t got; (got = fread(chunk, 1, sizeof chunk, input)) > 0;) fwrite(chunk, 1, got, output);
    fclose(input);
    return fclose(output) == 0;
}

// A directory URL ends in a slash, which would hide a child path from copy_recursively's prefix checks.
std::string without_trailing_slashes(std::string path) {
    while (path.size() > 1 && path.back() == '/') path.pop_back();
    return path;
}

bool file_manager_copy(void*, const char*, void* from, void* to, void** error) {
    if (copy_recursively(without_trailing_slashes(nsobject_path(from)), without_trailing_slashes(nsobject_path(to)))) return true;
    set_cocoa_error(error, "Could not copy " + nsobject_path(from));
    return false;
}

void* directory_listing(const std::string& path, bool as_urls) {
    auto* array = static_cast<NSArrayObject*>(new_nsarray());
    if (DIR* directory = opendir(path.c_str())) {
        while (const dirent* entry = readdir(directory)) {
            const std::string name = entry->d_name;
            if (name == "." || name == "..") continue;
            array->items->push_back(as_urls ? new_nsurl(file_url_text(path + "/" + name)) : new_nsstring(name));
        }
        closedir(directory);
    }
    return array;
}

void* file_manager_contents_path(void*, const char*, void* path, void**) { return directory_listing(nsobject_path(path), false); }

void* file_manager_contents_url(void*, const char*, void* url, void*, uint64_t, void**) {
    std::string path = nsobject_path(url);
    while (path.size() > 1 && path.back() == '/') path.pop_back();
    return directory_listing(path, true);
}

SWIFTCC void* url_bridge_to_objc(SWIFT_CONTEXT URLBox** self) {
    auto* object = static_cast<NSStringObject*>(new_nsstring(*(*self)->text));
    object->header.metadata = g_nsurl_class;
    return object;
}

SWIFTCC void url_bridge_from_objc(SWIFT_OUT URLBox** out, void* object) { *out = new_url(nsstring_text(object)); }

const int32_t kDirectoryHintInferFromPath = 2;

bool file_manager_create_directory(void*, const char*, void* url, bool, void*, void** error) {
    if (error != nullptr) *error = nullptr;
    return make_directories(url_path(nsstring_text(url)));
}

SWIFTCC void localization_value_literal(SWIFT_OUT LocalizedBox** out, SwiftString literal) { *out = new_localized_box(swift_string_to_utf8(literal)); }

SWIFTCC void localization_interpolation_init(SWIFT_OUT LocalizedBox** out, int64_t, int64_t) { *out = new_localized_box(""); }

SWIFTCC void localization_append_literal(SwiftString literal, SWIFT_CONTEXT LocalizedBox** self) { *(*self)->key += swift_string_to_utf8(literal); }

SWIFTCC void localization_append_string(SwiftString value, SWIFT_CONTEXT LocalizedBox** self) {
    *(*self)->key += "%@";
    (*self)->arguments->push_back(swift_string_to_utf8(value));
}

SWIFTCC void localization_append_integer(const int64_t* value, SwiftString specifier, void*, void*, SWIFT_CONTEXT LocalizedBox** self) {
    *(*self)->key += swift_string_to_utf8(specifier);
    (*self)->arguments->push_back(std::to_string(*value));
}

// Interpolations are passed indirectly and consumed: ownership of the box moves to the new value.
SWIFTCC void localization_value_interpolation(SWIFT_OUT LocalizedBox** out, LocalizedBox** interpolation) { *out = *interpolation; }

LocalizedBox* copy_localized(const LocalizedBox* source) {
    auto* copy = new_localized_box(*source->key);
    *copy->arguments = *source->arguments;
    return copy;
}

SWIFTCC void localized_resource_init(SWIFT_OUT LocalizedBox** out, LocalizedBox** value) { *out = copy_localized(*value); }

SWIFTCC void localized_resource_init_default(SWIFT_OUT LocalizedBox** out, const char* key, int64_t length, uint64_t, LocalizedBox** default_value) {
    *out = copy_localized(*default_value);
    *(*out)->key = std::string(key, static_cast<size_t>(length));
}

SWIFTCC SwiftString string_localized(LocalizedBox** resource) { return swift_string_from_utf8(localized_text(*resource)); }

// A Locale value is one reference to its NSLocale, whose identifier is the only state formatters could ask for.
SWIFTCC void locale_with_identifier(SWIFT_OUT void** out, uint64_t, uint64_t) { *out = objc_framework_object("NSLocale"); }

// Locale.current is an immediate word rather than an object, and formatters only store what this returns.
SWIFTCC void* locale_bridge_to_objc(SWIFT_CONTEXT void** self) { return reinterpret_cast<intptr_t>(*self) > 0 ? rt_swift_retain(*self) : objc_framework_object("NSLocale"); }

SWIFTCC void locale_current(SWIFT_OUT uint64_t* out) { *out = kLocaleImmediate; }

const int32_t kBundleDescriptionForClass = 0;

void destroy_data_storage(void* raw) {
    auto* storage = static_cast<DataStorage*>(raw);
    free(storage->bytes);
    free(storage);
}

const HeapMetadata kDataHeapMetadata = {destroy_data_storage, nullptr, 0};

DataValue make_data(const std::string& bytes) {
    if (bytes.empty()) return {0, kDataEmptyTag};
    auto* storage = static_cast<DataStorage*>(calloc(1, sizeof(DataStorage)));
    storage->metadata = const_cast<uint64_t*>(&kDataHeapMetadata.kind);
    new (&storage->references) std::atomic<uint64_t>(1);
    storage->bytes = static_cast<uint8_t*>(malloc(bytes.size()));
    memcpy(storage->bytes, bytes.data(), bytes.size());
    storage->length = static_cast<int64_t>(bytes.size());
    storage->capacity = storage->length;
    // Range<Int32>: lower bound in the low half, upper bound in the high half.
    const uint64_t range = static_cast<uint64_t>(bytes.size()) << 32;
    return {range, reinterpret_cast<uint64_t>(storage) | kDataSliceTag};
}

// __DataStorage allocated by the guest's inlined Data initializers, which read its size from the class.
void* data_storage_class_metadata() {
    static void* block[12] = {};
    return sized_class_metadata(block, reinterpret_cast<void*>(destroy_data_storage), sizeof(DataStorage));
}

struct DataMetadataResponse {
    void* metadata;
    size_t state;
};

SWIFTCC DataMetadataResponse data_storage_access(size_t) { return {data_storage_class_metadata(), 0}; }

void data_storage_fill(DataStorage* self, const void* bytes, int64_t length, int64_t offset) {
    self->bytes = static_cast<uint8_t*>(calloc(1, length > 0 ? static_cast<size_t>(length) : 1));
    if (bytes != nullptr && length > 0) memcpy(self->bytes, bytes, static_cast<size_t>(length));
    self->length = length;
    self->capacity = length;
    self->offset = offset;
}

SWIFTCC DataStorage* data_storage_init_bytes(const void* bytes, int64_t length, SWIFT_CONTEXT DataStorage* self) {
    data_storage_fill(self, bytes, length, 0);
    return self;
}

SWIFTCC DataStorage* data_storage_init_length(int64_t length, SWIFT_CONTEXT DataStorage* self) {
    data_storage_fill(self, nullptr, length, 0);
    return self;
}

using DataDeallocator = SWIFTCC void (*)(void*, int64_t, SWIFT_CONTEXT void*);

SWIFTCC DataStorage* data_storage_init_full(void* bytes, int64_t length, bool, DataDeallocator deallocator, void* deallocator_context, int64_t offset, SWIFT_CONTEXT DataStorage* self) {
    data_storage_fill(self, bytes, length, offset);
    if (deallocator != nullptr) deallocator(bytes, length, deallocator_context);
    return self;
}

SWIFTCC uint8_t* data_storage_bytes(SWIFT_CONTEXT DataStorage* self) { return self->bytes; }

SWIFTCC int64_t data_storage_length(SWIFT_CONTEXT DataStorage* self) { return self->length; }

SWIFTCC int64_t data_storage_offset(SWIFT_CONTEXT DataStorage* self) { return self->offset; }

void destroy_error_box(void* raw) {
    auto* box = static_cast<ErrorBox*>(raw);
    delete box->message;
    free(box);
}

const HeapMetadata kErrorBoxMetadata = {destroy_error_box, nullptr, 0};

void* make_error(const std::string& message, int64_t code) {
    auto* box = static_cast<ErrorBox*>(calloc(1, sizeof(ErrorBox)));
    static void* error_class = objc_native_class("NSError", destroy_error_box);
    box->metadata = error_class;
    new (&box->references) std::atomic<uint64_t>(1);
    box->message = new std::string(message);
    box->code = code;
    return box;
}

void add_foundation_url_symbols(SymbolTable& table) {
    table.insert({
        {"$s10Foundation3URLV18documentsDirectoryACvgZ", reinterpret_cast<void*>(url_documents)},
        {"$s10Foundation3URLV22appendingPathComponentyACSSF", reinterpret_cast<void*>(url_appending_component)},
        {"$s10Foundation3URLV22appendingPathComponent_11isDirectoryACSS_SbtF", reinterpret_cast<void*>(url_appending_directory)},
        {"$s10Foundation3URLV22appendingPathExtensionyACSSF", reinterpret_cast<void*>(url_appending_extension)},
        {"$s10Foundation3URLV21deletingPathExtensionACyF", reinterpret_cast<void*>(url_deleting_extension)},
        {"$s10Foundation3URLV17lastPathComponentSSvg", reinterpret_cast<void*>(url_last_component)},
        {"$s10Foundation3URLV13pathExtensionSSvg", reinterpret_cast<void*>(url_path_extension)},
        {"$s10Foundation3URLV12relativePathSSvg", reinterpret_cast<void*>(url_path_getter)},
        {"$s10Foundation3URLV4path14percentEncodedSSSb_tF", reinterpret_cast<void*>(url_path_percent)},
        {"$s10Foundation3URLV06isFileB0Sbvg", reinterpret_cast<void*>(url_is_file)},
        {"$s10Foundation3URLV16hasDirectoryPathSbvg", reinterpret_cast<void*>(url_has_directory_path)},
        {"$s10Foundation3URLV2eeoiySbAC_ACtFZ", reinterpret_cast<void*>(url_equal)},
        {"$s10Foundation3URLV36startAccessingSecurityScopedResourceSbyF", reinterpret_cast<void*>(url_start_security_scope)},
        {"$s10Foundation3URLV08absoluteB0ACvg", reinterpret_cast<void*>(url_absolute)},
        {"$s10Foundation3URLV04baseB0ACSgvg", reinterpret_cast<void*>(url_no_base)},
        {"$s10Foundation3URLV4hostSSSgvg", reinterpret_cast<void*>(url_host)},
        {"$s10Foundation3URLV5querySSSgvg", reinterpret_cast<void*>(url_query)},
        {"$s10Foundation3URLV4portSiSgvg", reinterpret_cast<void*>(url_port)},
        {"$s10Foundation3URLV6string10relativeToACSgSSh_AFhtcfC", reinterpret_cast<void*>(url_string_relative)},
        {"$s10Foundation3URLV27applicationSupportDirectoryACvgZ", reinterpret_cast<void*>(url_application_support)},
        {"$s10Foundation3URLV6stringACSgSSh_tcfC", reinterpret_cast<void*>(url_from_string)},
        {"$s10Foundation3URLV14absoluteStringSSvg", reinterpret_cast<void*>(url_absolute_string)},
        {"$s10Foundation3URLV4pathSSvg", reinterpret_cast<void*>(url_path_getter)},
        {"$s10Foundation3URLV6schemeSSSgvg", reinterpret_cast<void*>(url_scheme)},
        {"$s10Foundation3URLV9appending4path13directoryHintACx_AC09DirectoryF0OtSyRzlF", reinterpret_cast<void*>(url_appending_path)},
        {"$s10Foundation3URLV25deletingLastPathComponentACyF", reinterpret_cast<void*>(url_deleting_last_component)},
        {"$s10Foundation3URLV19_bridgeToObjectiveCSo5NSURLCyF", reinterpret_cast<void*>(url_bridge_to_objc)},
        {"$sSS10FoundationE10contentsOfSSAA3URLVh_tKcfC", reinterpret_cast<void*>(string_contents_of_url)},
        {"$s10Foundation3URLV36_unconditionallyBridgeFromObjectiveCyACSo5NSURLCSgFZ", reinterpret_cast<void*>(url_bridge_from_objc)},
        {"$sSS10FoundationE17LocalizationValueV13stringLiteralACSS_tcfC", reinterpret_cast<void*>(localization_value_literal)},
        {"$sSS10FoundationE17LocalizationValueV19StringInterpolationV15literalCapacity18interpolationCountAESi_SitcfC", reinterpret_cast<void*>(localization_interpolation_init)},
        {"$sSS10FoundationE17LocalizationValueV19StringInterpolationV13appendLiteralyySSF", reinterpret_cast<void*>(localization_append_literal)},
        {"$sSS10FoundationE17LocalizationValueV19StringInterpolationV06appendE0yySSF", reinterpret_cast<void*>(localization_append_string)},
        {"$sSS10FoundationE17LocalizationValueV19StringInterpolationV06appendE0_9specifieryx_SStAA18_FormatSpecifiableRzlF", reinterpret_cast<void*>(localization_append_integer)},
        {"$sSS10FoundationE17LocalizationValueV19stringInterpolationA2C06StringE0V_tcfC", reinterpret_cast<void*>(localization_value_interpolation)},
        {"$s10Foundation23LocalizedStringResourceV_5table6locale6bundle7commentACSSAAE17LocalizationValueV_SSSgAA6LocaleVAC17BundleDescriptionOs06StaticC0VSgtcfC", reinterpret_cast<void*>(localized_resource_init)},
        {"$s10Foundation23LocalizedStringResourceV_12defaultValue5table6locale6bundle7commentACs06StaticC0V_SSAAE012LocalizationF0VSSSgAA6LocaleVAC17BundleDescriptionOAJSgtcfC", reinterpret_cast<void*>(localized_resource_init_default)},
        {"$sSS10FoundationE9localizedSSAA23LocalizedStringResourceV_tcfC", reinterpret_cast<void*>(string_localized)},
        {"$s10Foundation3URLV14resourceValues7forKeysAA011URLResourceD0VShySo16NSURLResourceKeyaG_tKF", reinterpret_cast<void*>(url_resource_values)},
        {"$s10Foundation3URLV17setResourceValuesyyAA011URLResourceE0VKF", reinterpret_cast<void*>(url_set_resource_values)},
        {"$s10Foundation17URLResourceValuesVACycfC", reinterpret_cast<void*>(resource_values_empty)},
        {"$s10Foundation17URLResourceValuesV11isDirectorySbSgvg", reinterpret_cast<void*>(resource_is_directory)},
        {"$s10Foundation17URLResourceValuesV13isRegularFileSbSgvg", reinterpret_cast<void*>(resource_is_regular)},
        {"$s10Foundation17URLResourceValuesV4nameSSSgvg", reinterpret_cast<void*>(resource_name)},
        {"$s10Foundation17URLResourceValuesV8fileSizeSiSgvg", reinterpret_cast<void*>(resource_integer<&ResourceValues::size>)},
        {"$s10Foundation17URLResourceValuesV22totalFileAllocatedSizeSiSgvg", reinterpret_cast<void*>(resource_integer<&ResourceValues::allocated>)},
        {"$s10Foundation17URLResourceValuesV19volumeTotalCapacitySiSgvg", reinterpret_cast<void*>(resource_integer<&ResourceValues::volume_total>)},
        {"$s10Foundation17URLResourceValuesV23volumeAvailableCapacitySiSgvg", reinterpret_cast<void*>(resource_integer<&ResourceValues::volume_available>)},
        {"$s10Foundation17URLResourceValuesV40volumeAvailableCapacityForImportantUsages5Int64VSgvg", reinterpret_cast<void*>(resource_integer<&ResourceValues::volume_available>)},
        {"$s10Foundation17URLResourceValuesV17contentAccessDateAA0F0VSgvg", reinterpret_cast<void*>(resource_date<&ResourceValues::access_time>)},
        {"$s10Foundation17URLResourceValuesV23contentModificationDateAA0F0VSgvg", reinterpret_cast<void*>(resource_date<&ResourceValues::modification_time>)},
        {"$s10Foundation17URLResourceValuesV17contentAccessDateAA0F0VSgvs", reinterpret_cast<void*>(resource_set_access_date)},
        {"$s10Foundation3URLV016standardizedFileB0ACvg", reinterpret_cast<void*>(url_standardized)},
        {"$s10Foundation3URLV12standardizedACvg", reinterpret_cast<void*>(url_standardized)},
        {"$s10Foundation3URLV23resolvingSymlinksInPathACyF", reinterpret_cast<void*>(url_resolving_symlinks)},
        {"$s10Foundation3URLV24checkResourceIsReachableSbyKF", reinterpret_cast<void*>(url_is_reachable)},
        {"$s10Foundation3URLV35stopAccessingSecurityScopedResourceyyF", reinterpret_cast<void*>(url_stop_accessing)},
        {"$s10Foundation3URLV15fileURLWithPath11isDirectoryACSSh_SbtcfC", reinterpret_cast<void*>(url_file_with_path_directory)},
        {"$sSo12NSFileHandleC10FoundationE5write10contentsOfyx_tKAC12DataProtocolRzlF", reinterpret_cast<void*>(handle_write_contents)},
        {"$s10Foundation6LocaleV10identifierACSS_tcfC", reinterpret_cast<void*>(locale_with_identifier)},
        {"$s10Foundation6LocaleV19_bridgeToObjectiveCSo8NSLocaleCyF", reinterpret_cast<void*>(locale_bridge_to_objc)},
        {"$s10Foundation6LocaleV7currentACvgZ", reinterpret_cast<void*>(locale_current)},
        {"$s10Foundation6LocaleV19autoupdatingCurrentACvgZ", reinterpret_cast<void*>(locale_current)},
        {"$s10Foundation13__DataStorageCMa", reinterpret_cast<void*>(data_storage_access)},
        {"$s10Foundation13__DataStorageC5bytes6lengthACSVSg_Sitcfc", reinterpret_cast<void*>(data_storage_init_bytes)},
        {"$s10Foundation13__DataStorageC6lengthACSi_tcfc", reinterpret_cast<void*>(data_storage_init_length)},
        {"$s10Foundation13__DataStorageC5bytes6length4copy11deallocator6offsetACSvSg_SiSbySv_SitcSgSitcfc", reinterpret_cast<void*>(data_storage_init_full)},
        {"$s10Foundation13__DataStorageC6_bytesSvSgvg", reinterpret_cast<void*>(data_storage_bytes)},
        {"$s10Foundation13__DataStorageC7_lengthSivg", reinterpret_cast<void*>(data_storage_length)},
        {"$s10Foundation13__DataStorageC7_offsetSivg", reinterpret_cast<void*>(data_storage_offset)},
    });
}

void register_foundation_url_classes() {
    objc_register_method("-[NSBundle bundleURL]", reinterpret_cast<const void*>(bundle_url));
    objc_register_method("-[NSBundle resourceURL]", reinterpret_cast<const void*>(bundle_url));
    objc_register_method("-[NSBundle bundlePath]", reinterpret_cast<const void*>(bundle_path));
    objc_register_method("-[NSBundle URLForResource:withExtension:]", reinterpret_cast<const void*>(bundle_url_for_resource));
    objc_register_method("+[NSFileHandle fileHandleForWritingToURL:error:]", reinterpret_cast<const void*>(handle_writing_url));
    objc_register_method("+[NSFileHandle fileHandleForReadingFromURL:error:]", reinterpret_cast<const void*>(handle_reading_url));
    objc_register_method("+[NSFileHandle fileHandleForUpdatingURL:error:]", reinterpret_cast<const void*>(handle_updating_url));
    objc_register_method("+[NSFileHandle fileHandleForWritingAtPath:]", reinterpret_cast<const void*>(handle_writing_path));
    objc_register_method("+[NSFileHandle fileHandleForReadingAtPath:]", reinterpret_cast<const void*>(handle_reading_path));
    objc_register_method("+[NSFileHandle fileHandleForUpdatingAtPath:]", reinterpret_cast<const void*>(handle_updating_path));
    objc_register_method("+[NSFileHandle fileHandleWithStandardOutput]", reinterpret_cast<const void*>(handle_standard_output));
    objc_register_method("+[NSFileHandle fileHandleWithStandardError]", reinterpret_cast<const void*>(handle_standard_error));
    objc_register_method("+[NSFileHandle fileHandleWithStandardInput]", reinterpret_cast<const void*>(handle_standard_input));
    objc_register_method("+[NSFileHandle fileHandleWithNullDevice]", reinterpret_cast<const void*>(handle_null_device));
    objc_register_method("-[NSFileHandle initWithFileDescriptor:]", reinterpret_cast<const void*>(handle_init_descriptor));
    objc_register_method("-[NSFileHandle fileDescriptor]", reinterpret_cast<const void*>(handle_get_descriptor));
    objc_register_method("-[NSFileHandle closeFile]", reinterpret_cast<const void*>(handle_close));
    objc_register_method("-[NSFileHandle closeAndReturnError:]", reinterpret_cast<const void*>(handle_close_error));
    objc_register_method("-[NSFileHandle writeData:]", reinterpret_cast<const void*>(handle_write));
    objc_register_method("-[NSFileHandle writeData:error:]", reinterpret_cast<const void*>(handle_write_error));
    objc_register_method("-[NSFileHandle readDataToEndOfFile]", reinterpret_cast<const void*>(handle_read_to_end));
    objc_register_method("-[NSFileHandle availableData]", reinterpret_cast<const void*>(handle_available_data));
    objc_register_method("-[NSFileHandle setReadabilityHandler:]", reinterpret_cast<const void*>(handle_set_readability));
    objc_register_method("-[NSFileHandle readabilityHandler]", reinterpret_cast<const void*>(handle_get_readability));
    objc_register_method("+[NSPipe pipe]", reinterpret_cast<const void*>(pipe_make));
    objc_register_method("-[NSPipe init]", reinterpret_cast<const void*>(pipe_init));
    objc_register_method("-[NSPipe fileHandleForReading]", reinterpret_cast<const void*>(pipe_reading));
    objc_register_method("-[NSPipe fileHandleForWriting]", reinterpret_cast<const void*>(pipe_writing));
    objc_register_method("-[NSFileHandle readToEndAndReturnError:]", reinterpret_cast<const void*>(handle_read_to_end_error));
    objc_register_method("-[NSFileHandle readDataOfLength:]", reinterpret_cast<const void*>(handle_read_length));
    objc_register_method("-[NSFileHandle readDataUpToLength:error:]", reinterpret_cast<const void*>(handle_read_length_error));
    objc_register_method("-[NSFileHandle seekToEndOfFile]", reinterpret_cast<const void*>(handle_seek_end));
    objc_register_method("-[NSFileHandle seekToEndReturningOffset:error:]", reinterpret_cast<const void*>(handle_seek_end_error));
    objc_register_method("-[NSFileHandle seekToFileOffset:]", reinterpret_cast<const void*>(handle_seek));
    objc_register_method("-[NSFileHandle seekToOffset:error:]", reinterpret_cast<const void*>(handle_seek_error));
    objc_register_method("-[NSFileHandle offsetInFile]", reinterpret_cast<const void*>(handle_offset));
    objc_register_method("-[NSFileHandle synchronizeFile]", reinterpret_cast<const void*>(handle_sync));
    objc_register_method("-[NSFileHandle synchronizeAndReturnError:]", reinterpret_cast<const void*>(handle_sync_error));
    objc_register_method("-[NSFileHandle truncateFileAtOffset:]", reinterpret_cast<const void*>(handle_truncate));
    objc_register_method("+[NSFileAccessIntent writingIntentWithURL:options:]", reinterpret_cast<const void*>(intent_make));
    objc_register_method("+[NSFileAccessIntent readingIntentWithURL:options:]", reinterpret_cast<const void*>(intent_make));
    objc_register_method("-[NSFileAccessIntent URL]", reinterpret_cast<const void*>(intent_url));
    objc_register_method("-[NSFileCoordinator initWithFilePresenter:]", reinterpret_cast<const void*>(coordinator_init));
    objc_register_method("-[NSFileCoordinator coordinateAccessWithIntents:queue:byAccessor:]", reinterpret_cast<const void*>(coordinate_intents));
    objc_register_method("-[NSFileCoordinator coordinateReadingItemAtURL:options:error:byAccessor:]", reinterpret_cast<const void*>(coordinate_item));
    objc_register_method("-[NSFileCoordinator coordinateWritingItemAtURL:options:error:byAccessor:]", reinterpret_cast<const void*>(coordinate_item));
    objc_register_method("-[NSBundle executablePath]", reinterpret_cast<const void*>(bundle_executable_path));
    objc_register_method("-[NSBundle executableURL]", reinterpret_cast<const void*>(bundle_executable_url));
    objc_register_method("-[NSBundle builtInPlugInsURL]", reinterpret_cast<const void*>(bundle_plugins_url));
    objc_register_method("-[NSBundle resourcePath]", reinterpret_cast<const void*>(bundle_path));
    objc_register_method("-[NSBundle pathForResource:ofType:]", reinterpret_cast<const void*>(bundle_path_for_resource));
    objc_register_method("-[NSBundle URLsForResourcesWithExtension:subdirectory:]", reinterpret_cast<const void*>(bundle_urls_with_extension));
    objc_register_method("-[NSBundle initWithURL:]", reinterpret_cast<const void*>(bundle_main_for_any));
    objc_register_method("+[NSBundle bundleWithURL:]", reinterpret_cast<const void*>(bundle_with_url));
    objc_register_method("+[NSBundle bundleWithIdentifier:]", reinterpret_cast<const void*>(bundle_with_identifier));
    objc_register_method("+[NSBundle allBundles]", reinterpret_cast<const void*>(bundle_all));
    objc_register_method("-[NSBundle localizedStringForKey:value:table:]", reinterpret_cast<const void*>(bundle_localized_string));
    objc_register_method("-[NSFileManager createFileAtPath:contents:attributes:]", reinterpret_cast<const void*>(file_manager_create_file));
    objc_register_method("-[NSFileManager contentsAtPath:]", reinterpret_cast<const void*>(file_manager_contents_at_path));
    objc_register_method("-[NSFileManager isReadableFileAtPath:]", reinterpret_cast<const void*>(file_manager_is_readable));
    objc_register_method("-[NSFileManager moveItemAtPath:toPath:error:]", reinterpret_cast<const void*>(file_manager_move_path));
    objc_register_method("-[NSFileManager createSymbolicLinkAtPath:withDestinationPath:error:]", reinterpret_cast<const void*>(file_manager_create_symlink));
    objc_register_method("-[NSFileManager destinationOfSymbolicLinkAtPath:error:]", reinterpret_cast<const void*>(file_manager_link_destination));
    objc_register_method("-[NSFileManager subpathsOfDirectoryAtPath:error:]", reinterpret_cast<const void*>(file_manager_subpaths));
    objc_register_method("-[NSFileManager setAttributes:ofItemAtPath:error:]", reinterpret_cast<const void*>(file_manager_set_attributes));
    objc_register_method("-[NSFileManager fileSystemRepresentationWithPath:]", reinterpret_cast<const void*>(file_manager_representation));
    objc_register_method("-[NSFileManager URLForDirectory:inDomain:appropriateForURL:create:error:]", reinterpret_cast<const void*>(file_manager_url_for_directory));
    objc_register_method("-[NSFileManager containerURLForSecurityApplicationGroupIdentifier:]", reinterpret_cast<const void*>(file_manager_group_container));
    objc_register_method("+[NSPersistentContainer defaultDirectoryURL]", reinterpret_cast<const void*>(+[](void*, const char*) -> void* {
                             constexpr int64_t kApplicationSupportDirectory = 14;
                             const std::string path = sandbox_directory(kApplicationSupportDirectory);
                             make_directory_tree(path);
                             return new_nsurl(file_url_text(path + "/"));
                         }));
    objc_register_method("-[NSFileManager URLsForDirectory:inDomains:]", reinterpret_cast<const void*>(file_manager_urls));
    objc_register_method("-[NSFileManager temporaryDirectory]", reinterpret_cast<const void*>(file_manager_temporary_directory));
    objc_register_method("-[NSFileManager fileExistsAtPath:isDirectory:]", reinterpret_cast<const void*>(file_manager_exists_directory));
    objc_register_method("-[NSFileManager createDirectoryAtPath:withIntermediateDirectories:attributes:error:]", reinterpret_cast<const void*>(file_manager_create_path));
    objc_register_method("-[NSFileManager removeItemAtPath:error:]", reinterpret_cast<const void*>(file_manager_remove));
    objc_register_method("-[NSFileManager removeItemAtURL:error:]", reinterpret_cast<const void*>(file_manager_remove));
    objc_register_method("-[NSFileManager moveItemAtURL:toURL:error:]", reinterpret_cast<const void*>(file_manager_move));
    objc_register_method("-[NSFileManager copyItemAtURL:toURL:error:]", reinterpret_cast<const void*>(file_manager_copy));
    for (const std::string cls : {"NSDictionary", "NSArray", "__NSJSONValue"}) {
        objc_register_method(("-[" + cls + " writeToURL:error:]").c_str(), reinterpret_cast<const void*>(+[](void* self, const char*, void* url, void** error) {
                                 if (write_property_list_object(self, nsobject_path(url), false)) return true;
                                 set_cocoa_error(error, "Could not write " + nsobject_path(url));
                                 return false;
                             }));
        objc_register_method(("-[" + cls + " writeToURL:atomically:]").c_str(), reinterpret_cast<const void*>(+[](void* self, const char*, void* url, bool) { return write_property_list_object(self, nsobject_path(url), true); }));
        objc_register_method(("-[" + cls + " writeToFile:atomically:]").c_str(), reinterpret_cast<const void*>(+[](void* self, const char*, void* path, bool) { return write_property_list_object(self, nsobject_path(path), true); }));
    }
    objc_register_method("-[NSDictionary initWithContentsOfURL:]", reinterpret_cast<const void*>(+[](void*, const char*, void* url) { return property_list_object_from_file(nsobject_path(url), true); }));
    objc_register_method("-[NSDictionary initWithContentsOfFile:]", reinterpret_cast<const void*>(+[](void*, const char*, void* path) { return property_list_object_from_file(nsobject_path(path), true); }));
    objc_register_method("+[NSDictionary dictionaryWithContentsOfURL:]", reinterpret_cast<const void*>(+[](void*, const char*, void* url) { return property_list_object_from_file(nsobject_path(url), true); }));
    objc_register_method("+[NSDictionary dictionaryWithContentsOfFile:]", reinterpret_cast<const void*>(+[](void*, const char*, void* path) { return property_list_object_from_file(nsobject_path(path), true); }));
    objc_register_method("-[NSArray initWithContentsOfURL:]", reinterpret_cast<const void*>(+[](void*, const char*, void* url) { return property_list_object_from_file(nsobject_path(url), false); }));
    objc_register_method("-[NSArray initWithContentsOfFile:]", reinterpret_cast<const void*>(+[](void*, const char*, void* path) { return property_list_object_from_file(nsobject_path(path), false); }));
    objc_register_method("+[NSArray arrayWithContentsOfURL:]", reinterpret_cast<const void*>(+[](void*, const char*, void* url) { return property_list_object_from_file(nsobject_path(url), false); }));
    objc_register_method("+[NSArray arrayWithContentsOfFile:]", reinterpret_cast<const void*>(+[](void*, const char*, void* path) { return property_list_object_from_file(nsobject_path(path), false); }));
    objc_register_method("-[NSFileManager contentsOfDirectoryAtPath:error:]", reinterpret_cast<const void*>(file_manager_contents_path));
    objc_register_method("-[NSFileManager contentsOfDirectoryAtURL:includingPropertiesForKeys:options:error:]", reinterpret_cast<const void*>(file_manager_contents_url));
    objc_register_method("-[NSFileManager createDirectoryAtURL:withIntermediateDirectories:attributes:error:]", reinterpret_cast<const void*>(file_manager_create_directory));
}

}  // namespace foundation

void foundation_clear_temporary_directory() { foundation::remove_recursively(engine_bundle_dir() + "/.tmp"); }

void* foundation_file_url_object(const std::string& path) { return foundation::new_nsurl(foundation::file_url_text(path)); }
