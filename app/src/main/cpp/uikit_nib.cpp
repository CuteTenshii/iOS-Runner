#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "engine.h"
#include "foundation_internal.h"
#include "plist.h"
#include "swift_abi.h"
#include "uikit_internal.h"

using foundation::new_nsarray;
using foundation::new_nsstring;
using foundation::NSArrayObject;
using foundation::nsstring_text;

namespace uikit {

namespace {

constexpr size_t kNibHeaderSize = 50;
constexpr uint8_t kPackedDoublesTag = 7;

enum class ValueType : uint8_t { Int8,
                                 Int16,
                                 Int32,
                                 Int64,
                                 True,
                                 False,
                                 Float,
                                 Double,
                                 Data,
                                 Null,
                                 Object };

struct NibValue {
    std::string key;
    ValueType type = ValueType::Null;
    int64_t integer = 0;
    double real = 0;
    std::string data;
    uint32_t object = 0;
};

struct NibObject {
    std::string class_name;
    std::vector<NibValue> values;
    void* instance = nullptr;
    bool built = false;
};

struct NibArchive {
    std::vector<NibObject> objects;
    void* owner = nullptr;
    void* storyboard = nullptr;
    std::string directory;
    std::vector<void*> created;
};

uint64_t read_varint(const std::string& data, size_t& at) {
    uint64_t value = 0;
    int shift = 0;
    while (at < data.size()) {
        const auto byte = static_cast<uint8_t>(data[at++]);
        value |= static_cast<uint64_t>(byte & 0x7f) << shift;
        shift += 7;
        if (byte & 0x80) break;
    }
    return value;
}

template <typename T>
T read_le(const std::string& data, size_t& at) {
    T value{};
    if (at + sizeof(T) <= data.size()) memcpy(&value, data.data() + at, sizeof(T));
    at += sizeof(T);
    return value;
}

bool parse_archive(const std::string& data, NibArchive& archive) {
    if (data.size() < kNibHeaderSize || data.compare(0, 10, "NIBArchive") != 0) return false;
    size_t at = 18;
    const uint32_t object_count = read_le<uint32_t>(data, at), object_offset = read_le<uint32_t>(data, at);
    const uint32_t key_count = read_le<uint32_t>(data, at), key_offset = read_le<uint32_t>(data, at);
    const uint32_t value_count = read_le<uint32_t>(data, at), value_offset = read_le<uint32_t>(data, at);
    const uint32_t class_count = read_le<uint32_t>(data, at), class_offset = read_le<uint32_t>(data, at);

    std::vector<std::string> keys;
    at = key_offset;
    for (uint32_t i = 0; i < key_count; ++i) {
        const uint64_t length = read_varint(data, at);
        keys.push_back(data.substr(at, length));
        at += length;
    }
    std::vector<NibValue> values(value_count);
    at = value_offset;
    for (auto& value : values) {
        value.key = keys[read_varint(data, at)];
        value.type = static_cast<ValueType>(static_cast<uint8_t>(data[at++]));
        switch (value.type) {
            case ValueType::Int8:
                value.integer = static_cast<uint8_t>(data[at++]);
                break;
            case ValueType::Int16:
                value.integer = read_le<uint16_t>(data, at);
                break;
            case ValueType::Int32:
                value.integer = read_le<int32_t>(data, at);
                break;
            case ValueType::Int64:
                value.integer = read_le<int64_t>(data, at);
                break;
            case ValueType::True:
                value.integer = 1;
                break;
            case ValueType::False:
            case ValueType::Null:
                break;
            case ValueType::Float:
                value.real = read_le<float>(data, at);
                break;
            case ValueType::Double:
                value.real = read_le<double>(data, at);
                break;
            case ValueType::Data: {
                const uint64_t length = read_varint(data, at);
                value.data = data.substr(at, length);
                at += length;
                break;
            }
            case ValueType::Object:
                value.object = read_le<uint32_t>(data, at);
                break;
            default:
                return false;
        }
    }
    std::vector<std::string> classes;
    at = class_offset;
    for (uint32_t i = 0; i < class_count; ++i) {
        const uint64_t length = read_varint(data, at);
        const uint64_t extras = read_varint(data, at);
        at += extras * sizeof(int32_t);
        classes.emplace_back(data.c_str() + at);
        at += length;
    }
    archive.objects.resize(object_count);
    at = object_offset;
    for (auto& object : archive.objects) {
        object.class_name = classes[read_varint(data, at)];
        const uint64_t start = read_varint(data, at);
        const uint64_t count = read_varint(data, at);
        object.values.assign(values.begin() + static_cast<long>(start), values.begin() + static_cast<long>(start + count));
    }
    return true;
}

const NibValue* value_named(const NibObject& object, const std::string& key) {
    for (const auto& value : object.values) {
        if (value.key == key) return &value;
    }
    return nullptr;
}

struct DecodePosition {
    NibArchive* archive;
    uint32_t index;
};

std::unordered_map<void*, std::string>& embedded_nibs() {
    static std::unordered_map<void*, std::string> table;
    return table;
}

std::unordered_map<void*, DecodePosition>& decoders() {
    static std::unordered_map<void*, DecodePosition> table;
    return table;
}

void* build_object(NibArchive& archive, uint32_t index);

std::string text_of_object(NibArchive& archive, uint32_t index) {
    const NibValue* bytes = value_named(archive.objects[index], "NS.bytes");
    return bytes != nullptr ? bytes->data : std::string();
}

void* build_array(NibArchive& archive, const NibObject& object) {
    auto* array = static_cast<NSArrayObject*>(new_nsarray());
    for (const auto& value : object.values) {
        if (value.key == "UINibEncoderEmptyKey" && value.type == ValueType::Object) array->items->push_back(build_object(archive, value.object));
    }
    return array;
}

std::string property_name_for_key(const std::string& key) {
    size_t start = 0;
    if (key.rfind("UI", 0) == 0 || key.rfind("NS", 0) == 0) start = 2;
    std::string name = key.substr(start);
    if (!name.empty()) name[0] = static_cast<char>(tolower(static_cast<unsigned char>(name[0])));
    return name;
}

void apply_values(void* self, NibArchive& archive, uint32_t index) {
    const NibObject& object = archive.objects[index];
    for (const auto& value : object.values) {
        const std::string name = property_name_for_key(value.key);
        switch (value.type) {
            case ValueType::Object: {
                if (value.key == "UINibEncoderEmptyKey") break;
                void* built = build_object(archive, value.object);
                property_set_object(self, name, built);
                break;
            }
            case ValueType::Int8:
            case ValueType::Int16:
            case ValueType::Int32:
            case ValueType::Int64:
            case ValueType::True:
            case ValueType::False:
                property_set_word(self, name, value.integer);
                break;
            case ValueType::Float:
            case ValueType::Double:
                property_set_floats(self, name, value.real, 0);
                break;
            case ValueType::Data:
                if (!value.data.empty() && static_cast<uint8_t>(value.data[0]) == kPackedDoublesTag) {
                    double parts[4] = {0, 0, 0, 0};
                    const size_t count = (value.data.size() - 1) / sizeof(double);
                    memcpy(parts, value.data.data() + 1, (count > 4 ? 4 : count) * sizeof(double));
                    property_set_rect(self, name, parts);
                }
                break;
            default:
                break;
        }
    }
    finish_decoded_object(self, object.class_name);
}

std::string object_data_bytes(const NibArchive& archive, uint32_t index) {
    const NibValue* bytes = value_named(archive.objects[index], "NS.bytes");
    return bytes != nullptr ? bytes->data : std::string();
}

void* build_object(NibArchive& archive, uint32_t index) {
    if (index >= archive.objects.size()) return nullptr;
    NibObject& object = archive.objects[index];
    if (object.built) return object.instance;
    object.built = true;
    const std::string& name = object.class_name;
    if (name == "NSString" || name == "NSMutableString") {
        object.instance = new_nsstring(text_of_object(archive, index));
    } else if (name == "NSArray" || name == "NSMutableArray" || name == "NSSet" || name == "NSMutableSet" || name == "NSDictionary" || name == "NSMutableDictionary") {
        object.instance = build_array(archive, object);
    } else if (name == "UIProxyObject") {
        const NibValue* identifier = value_named(object, "UIProxiedObjectIdentifier");
        const std::string proxied = identifier != nullptr ? text_of_object(archive, identifier->object) : std::string();
        if (proxied == "IBFilesOwner")
            object.instance = archive.owner;
        else if (proxied == "UIStoryboardPlaceholder")
            object.instance = archive.storyboard;
    } else {
        std::string class_name = name;
        if (name == "UIClassSwapper") {
            const NibValue* swapped = value_named(object, "UIClassName");
            const NibValue* original = value_named(object, "UIOriginalClassName");
            class_name = swapped != nullptr ? text_of_object(archive, swapped->object) : std::string();
            if (objc_class_named(class_name) == nullptr && original != nullptr) class_name = text_of_object(archive, original->object);
        }
        void* cls = objc_class_named(class_name);
        object.instance = cls != nullptr && objc_framework_class_name(cls).empty() ? objc_allocate(cls) : objc_framework_object(class_name.c_str());
        archive.created.push_back(object.instance);
        if (objc_framework_class_name(cls).empty() && cls != nullptr) {
            void* decoder = objc_framework_object("UINibDecoder");
            decoders()[decoder] = {&archive, index};
            using InitWithCoder = void* (*)(void*, const char*, void*);
            object.instance = reinterpret_cast<InitWithCoder>(const_cast<void*>(objc_method_for(object.instance, "initWithCoder:")))(object.instance, "initWithCoder:", decoder);
        } else {
            apply_values(object.instance, archive, index);
            if (const NibValue* contents = name == "UINib" ? value_named(object, "archiveData") : nullptr) {
                embedded_nibs()[object.instance] = object_data_bytes(archive, contents->object);
                property_set_object(object.instance, "storyboard", archive.storyboard);
            }
        }
    }
    return object.instance;
}

// The superclass end of every guest initWithCoder:.
void* init_with_coder(void* self, const char*, void* decoder) {
    const auto found = decoders().find(decoder);
    if (found != decoders().end()) apply_values(self, *found->second.archive, found->second.index);
    return self;
}

const NibValue* coded_value(void* decoder, void* key) {
    const auto found = decoders().find(decoder);
    if (found == decoders().end()) return nullptr;
    return value_named(found->second.archive->objects[found->second.index], nsstring_text(key));
}

void* decode_object(void* self, const char*, void* key) {
    const NibValue* value = coded_value(self, key);
    if (value == nullptr || value->type != ValueType::Object) return nullptr;
    return build_object(*decoders()[self].archive, value->object);
}

bool decode_bool(void* self, const char*, void* key) {
    const NibValue* value = coded_value(self, key);
    return value != nullptr && value->integer != 0;
}

int64_t decode_integer(void* self, const char*, void* key) {
    const NibValue* value = coded_value(self, key);
    return value != nullptr ? value->integer : 0;
}

double decode_double(void* self, const char*, void* key) {
    const NibValue* value = coded_value(self, key);
    return value != nullptr ? value->real : 0;
}

float decode_float(void* self, const char*, void* key) {
    const NibValue* value = coded_value(self, key);
    return value != nullptr ? static_cast<float>(value->real) : 0;
}

bool contains_value(void* self, const char*, void* key) { return coded_value(self, key) != nullptr; }

bool returns_true(void*, const char*) { return true; }

void connect_outlet(void* source, void* destination, const std::string& label) {
    if (source == nullptr) return;
    object_set_value(source, label, destination);
}

void run_connections(NibArchive& archive, uint32_t root) {
    const NibValue* connections = value_named(archive.objects[root], "UINibConnectionsKey");
    if (connections == nullptr || connections->type != ValueType::Object) return;
    for (const auto& entry : archive.objects[connections->object].values) {
        if (entry.key != "UINibEncoderEmptyKey") continue;
        const NibObject& connection = archive.objects[entry.object];
        const NibValue* source = value_named(connection, "UISource");
        const NibValue* destination = value_named(connection, "UIDestination");
        const NibValue* label = value_named(connection, "UILabel");
        if (source == nullptr || destination == nullptr || label == nullptr) continue;
        void* from = build_object(archive, source->object);
        void* to = build_object(archive, destination->object);
        const std::string text = text_of_object(archive, label->object);
        if (connection.class_name == "UIRuntimeOutletConnection") {
            connect_outlet(from, to, text);
        } else if (connection.class_name == "UIRuntimeEventConnection") {
            const NibValue* mask = value_named(connection, "UIEventMask");
            control_add_target(from, to, text, mask != nullptr ? static_cast<uint64_t>(mask->integer) : 0);
        }
    }
}

}  // namespace

namespace {
std::vector<void*> instantiate_archive(const std::string& data, const std::string& path, void* owner, void* storyboard);
}

std::vector<void*> nib_instantiate(const std::string& path, void* owner, void* storyboard) {
    // A nib built for several OS versions is a directory of archives.
    const std::string file_path = std::filesystem::is_directory(path) ? path + "/objects-13.0+.nib" : path;
    std::ifstream file(file_path, std::ios::binary);
    if (!file) {
        engine_log("UIKIT: no nib at %s", file_path.c_str());
        return {};
    }
    const std::string data((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    return instantiate_archive(data, path, owner, storyboard);
}

std::vector<void*> nib_instantiate_object(void* nib, void* owner) {
    if (void* path = property_get_object(nib, "nibPath")) return nib_instantiate(nsstring_text(path), owner, nullptr);
    const auto embedded = embedded_nibs().find(nib);
    if (embedded == embedded_nibs().end()) {
        engine_log("UIKIT: a UINib has no contents");
        return {};
    }
    return instantiate_archive(embedded->second, "an embedded nib", owner, property_get_object(nib, "storyboard"));
}

namespace {
std::vector<void*> instantiate_archive(const std::string& data, const std::string& path, void* owner, void* storyboard) {
    // Decoders of guest objects may outlive this call, so an archive is kept for the life of the process.
    auto* archive = new NibArchive;
    if (!parse_archive(data, *archive) || archive->objects.empty()) {
        engine_log("UIKIT: unreadable nib at %s", path.c_str());
        return {};
    }
    archive->owner = owner;
    archive->storyboard = storyboard;
    const NibValue* top = value_named(archive->objects[0], "UINibTopLevelObjectsKey");
    std::vector<void*> top_objects;
    if (top != nullptr) {
        for (const auto& entry : archive->objects[top->object].values) {
            if (entry.key != "UINibEncoderEmptyKey" || entry.type != ValueType::Object) continue;
            void* built = build_object(*archive, entry.object);
            if (archive->objects[entry.object].class_name != "UIProxyObject" && built != nullptr) top_objects.push_back(built);
        }
    }
    run_connections(*archive, 0);
    for (void* created : archive->created) {
        if (objc_responds(created, "awakeFromNib")) {
            using Awake = void (*)(void*, const char*);
            reinterpret_cast<Awake>(const_cast<void*>(objc_method_for(created, "awakeFromNib")))(created, "awakeFromNib");
        }
    }
    return top_objects;
}
}  // namespace

void register_nib_classes() {
    objc_register_method("-[UINibDecoder decodeObjectForKey:]", reinterpret_cast<const void*>(decode_object));
    objc_register_method("-[UINibDecoder decodeBoolForKey:]", reinterpret_cast<const void*>(decode_bool));
    objc_register_method("-[UINibDecoder decodeIntegerForKey:]", reinterpret_cast<const void*>(decode_integer));
    objc_register_method("-[UINibDecoder decodeIntForKey:]", reinterpret_cast<const void*>(decode_integer));
    objc_register_method("-[UINibDecoder decodeInt32ForKey:]", reinterpret_cast<const void*>(decode_integer));
    objc_register_method("-[UINibDecoder decodeInt64ForKey:]", reinterpret_cast<const void*>(decode_integer));
    objc_register_method("-[UINibDecoder decodeDoubleForKey:]", reinterpret_cast<const void*>(decode_double));
    objc_register_method("-[UINibDecoder decodeFloatForKey:]", reinterpret_cast<const void*>(decode_float));
    objc_register_method("-[UINibDecoder containsValueForKey:]", reinterpret_cast<const void*>(contains_value));
    objc_register_method("-[UINibDecoder allowsKeyedCoding]", reinterpret_cast<const void*>(returns_true));
}

const void* nib_init_with_coder() { return reinterpret_cast<const void*>(init_with_coder); }

}  // namespace uikit
