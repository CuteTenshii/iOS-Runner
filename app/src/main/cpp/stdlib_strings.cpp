#include "stdlib_internal.h"

namespace stdlib {

struct SubstringValue {
    uint64_t start;
    uint64_t end;
    SwiftString guts;
};

std::string map_ascii_case(std::string text, bool upper) {
    for (char& c : text) {
        if (upper && c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
        if (!upper && c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    }
    return text;
}

SWIFTCC void string_append(SwiftString other, SWIFT_CONTEXT SwiftString* self) {
    *self = swift_string_from_utf8(swift_string_to_utf8(*self) + swift_string_to_utf8(other));
}

SWIFTCC void string_guts_grow(int64_t capacity, SWIFT_CONTEXT SwiftString* self) {
    // Rewrites the string into native storage with room for `capacity` bytes, as the guest's inline append expects.
    std::string text = swift_string_to_utf8(*self);
    const size_t requested = capacity < static_cast<int64_t>(kMinimumGrownCapacity) ? kMinimumGrownCapacity : static_cast<size_t>(capacity);
    const size_t size = text.size();
    text.resize(requested > kMinimumGrownCapacity ? requested : kMinimumGrownCapacity + 1);
    SwiftString grown = swift_string_from_utf8(text);
    grown.count_and_flags = (grown.count_and_flags & ~kCountMask) | size;
    memcpy(reinterpret_cast<char*>(grown.object) + kStorageCountOffset, &grown.count_and_flags, sizeof grown.count_and_flags);
    *self = grown;
}

SWIFTCC SwiftString string_lowercased(SwiftString self) { return swift_string_from_utf8(map_ascii_case(swift_string_to_utf8(self), false)); }

SWIFTCC SwiftString string_uppercased(SwiftString self) { return swift_string_from_utf8(map_ascii_case(swift_string_to_utf8(self), true)); }

SWIFTCC bool string_has_prefix(SwiftString prefix, SwiftString self) {
    const std::string text = swift_string_to_utf8(self);
    const std::string start = swift_string_to_utf8(prefix);
    return text.size() >= start.size() && text.compare(0, start.size(), start) == 0;
}

SWIFTCC bool string_has_suffix(SwiftString suffix, SwiftString self) {
    const std::string text = swift_string_to_utf8(self);
    const std::string end = swift_string_to_utf8(suffix);
    return text.size() >= end.size() && text.compare(text.size() - end.size(), end.size(), end) == 0;
}

SWIFTCC uint64_t string_index_after(uint64_t index, SwiftString self) { return make_index(next_scalar(swift_string_to_utf8(self), index_offset(index))); }

SWIFTCC uint64_t string_index_before(uint64_t index, SwiftString self) { return make_index(previous_scalar(swift_string_to_utf8(self), index_offset(index))); }

SWIFTCC SwiftString string_subscript_character(uint64_t index, SwiftString self) {
    const std::string text = swift_string_to_utf8(self);
    const size_t start = index_offset(index);
    return swift_string_from_utf8(text.substr(start, next_scalar(text, start) - start));
}

extern "C" void* rt_swift_bridgeObjectRetain(void*);

// `self` is borrowed but the Substring owns its guts, so it takes its own reference.
SWIFTCC SubstringValue string_subscript_range(uint64_t start, uint64_t end, SwiftString self) {
    rt_swift_bridgeObjectRetain(reinterpret_cast<void*>(self.object));
    return {start, end, self};
}

SWIFTCC SwiftString string_from_substring(uint64_t start, uint64_t end, SwiftString guts) {
    const std::string text = swift_string_to_utf8(guts);
    return swift_string_from_utf8(text.substr(index_offset(start), index_offset(end) - index_offset(start)));
}

SWIFTCC StringTriple string_from_utf8_repairing(const char* bytes, int64_t count) {
    return {swift_string_from_utf8(std::string(bytes, static_cast<size_t>(count))), 0};
}

constexpr size_t kArrayStorageElements = 32;

constexpr size_t kStaticStringStride = 24;

SWIFTCC int64_t find_string_switch_case(const char* cases, SwiftString value) {
    const std::string text = swift_string_to_utf8(value);
    int64_t count;
    memcpy(&count, cases + kArrayCountOffset, sizeof count);
    for (int64_t i = 0; i < count; ++i) {
        const char* entry = cases + kArrayStorageElements + static_cast<size_t>(i) * kStaticStringStride;
        const char* start;
        uint64_t length;
        memcpy(&start, entry, sizeof start);
        memcpy(&length, entry + sizeof start, sizeof length);
        if (text.size() == length && memcmp(text.data(), start, length) == 0) return i;
    }
    return -1;
}

SWIFTCC SwiftString int_description(SWIFT_CONTEXT const int64_t* self) { return swift_string_from_utf8(std::to_string(*self)); }

// Live Activities have no Android equivalent: the app sees none, and that they are switched off.
void* activity_list(void*) { return empty_array_singleton(); }

SWIFTCC void* activity_authorization_init(SWIFT_CONTEXT void* self) { return self; }

SWIFTCC bool activity_enabled(SWIFT_CONTEXT void*) { return false; }

void add_stdlib_strings_symbols(SymbolTable& table) {
    table.insert({
        {"$sSS6appendyySSF", reinterpret_cast<void*>(string_append)},
        {"$ss11_StringGutsV4growyySiF", reinterpret_cast<void*>(string_guts_grow)},
        {"$sSS10lowercasedSSyF", reinterpret_cast<void*>(string_lowercased)},
        {"$sSS10uppercasedSSyF", reinterpret_cast<void*>(string_uppercased)},
        {"$sSS9hasPrefixySbSSF", reinterpret_cast<void*>(string_has_prefix)},
        {"$sSS9hasSuffixySbSSF", reinterpret_cast<void*>(string_has_suffix)},
        {"$sSS5index5afterSS5IndexVAD_tF", reinterpret_cast<void*>(string_index_after)},
        {"$sSS5index6beforeSS5IndexVAD_tF", reinterpret_cast<void*>(string_index_before)},
        {"$sSSySJSS5IndexVcig", reinterpret_cast<void*>(string_subscript_character)},
        {"$sSSySsSnySS5IndexVGcig", reinterpret_cast<void*>(string_subscript_range)},
        {"$sSS14_fromSubstringySSSshFZ", reinterpret_cast<void*>(string_from_substring)},
        {"$sSS18_fromUTF8RepairingySS6result_Sb11repairsMadetSRys5UInt8VGFZ", reinterpret_cast<void*>(string_from_utf8_repairing)},
        {"$ss21_findStringSwitchCase5cases6stringSiSays06StaticB0VG_SStF", reinterpret_cast<void*>(find_string_switch_case)},
        {"$s11ActivityKit0A0C10activitiesSayACyxGGvgZ", reinterpret_cast<void*>(activity_list)},
        {"$s11ActivityKit0A17AuthorizationInfoCACycfc", reinterpret_cast<void*>(activity_authorization_init)},
        {"$s11ActivityKit0A17AuthorizationInfoC20areActivitiesEnabledSbvg", reinterpret_cast<void*>(activity_enabled)},
    });
}

}  // namespace stdlib
