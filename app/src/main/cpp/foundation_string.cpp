#include "foundation_internal.h"

using namespace foundation;

std::string foundation_data_bytes(FoundationData data) { return data_bytes(data.range, data.storage); }

FoundationData foundation_make_data(const std::string& bytes) {
    const DataValue data = make_data(bytes);
    return {data.range, data.storage};
}

void* foundation_new_array(const std::vector<void*>& items) {
    auto* array = static_cast<NSArrayObject*>(new_nsarray());
    for (void* item : items) array->items->push_back(rt_swift_retain(item));
    return array;
}

void* foundation_make_error(const std::string& message, int64_t code) { return make_error(message, code); }

int foundation_web_view_id(const void* object) {
    const auto found = web_views().find(object);
    return found == web_views().end() ? -1 : found->second.id;
}

void foundation_js_finished(int call, const std::string& json, bool failed) {
    engine_post_event([call, json, failed] {
        const auto found = pending_script_calls().find(call);
        if (found == pending_script_calls().end()) return;
        const PendingScriptCall pending = found->second;
        pending_script_calls().erase(found);
        void* error = nullptr;
        if (failed)
            error = make_error("The script could not run.", 1);
        else
            write_script_result(pending.out, json);
        concurrency_resume_call(pending.context, error);
    });
}

void foundation_web_url_changed(int web_view, const std::string& url) {
    engine_post_event([web_view, url] {
        for (auto& [object, state] : web_views()) {
            if (state.id == web_view) state.url = url;
        }
    });
}

void foundation_cookies_changed() {
    engine_post_event([] {
        if (g_cookie_observer != nullptr) objc_send(g_cookie_observer, "cookiesDidChangeInCookieStore:", framework_singleton("WKHTTPCookieStore"));
    });
}

std::string foundation_url_text(const void* url) { return *static_cast<const URLBox*>(url)->text; }

bool foundation_parse_iso8601(const std::string& text, double& seconds) {
    tm parts{};
    int consumed = 0;
    if (sscanf(text.c_str(), "%d-%d-%dT%d:%d:%d%n", &parts.tm_year, &parts.tm_mon, &parts.tm_mday, &parts.tm_hour, &parts.tm_min, &parts.tm_sec, &consumed) < 3) return false;
    parts.tm_year -= 1900;
    parts.tm_mon -= 1;
    double result = static_cast<double>(timegm(&parts));
    const char* rest = text.c_str() + consumed;
    if (consumed > 0 && *rest == '.') {
        char* end;
        result += strtod(rest, &end);
        rest = end;
    }
    int hours = 0, minutes = 0;
    if ((*rest == '+' || *rest == '-') && sscanf(rest + 1, "%d:%d", &hours, &minutes) >= 1) {
        const double offset = hours * 3600.0 + minutes * 60.0;
        result += *rest == '+' ? -offset : offset;
    }
    seconds = result - kSecondsFrom1970To2001;
    return true;
}

void* foundation_make_url(const std::string& text) { return make_url_box(text); }

void* foundation_native_array(void* object) {
    void* value = nullptr;
    return object != nullptr && bridged_collection_value(object, value) ? value : nullptr;
}

struct FormatArgument {
    void* buffer[3];
    void* type;
    void* table;
};

std::string format_argument_text(const FormatArgument& argument, const std::string& specifier) {
    char text[64];
    if (argument.type == type_system_symbol("$sSSN")) return swift_string_to_utf8(*reinterpret_cast<const SwiftString*>(argument.buffer));
    if (argument.type == type_system_symbol("$sSdN")) {
        snprintf(text, sizeof text, specifier.c_str(), *reinterpret_cast<const double*>(argument.buffer));
        return text;
    }
    if (specifier.back() == '@') {
        if (argument.type == type_system_symbol("$sSiN")) return std::to_string(reinterpret_cast<int64_t>(argument.buffer[0]));
        const std::string object_text = foundation_object_text(argument.buffer[0]);
        return object_text;
    }
    std::string wide = specifier;
    wide.erase(std::remove_if(wide.begin(), wide.end(), [](char c) { return c == 'l' || c == 'z' || c == 'q'; }), wide.end());
    wide.insert(wide.size() - 1, "ll");
    snprintf(text, sizeof text, wide.c_str(), reinterpret_cast<long long>(argument.buffer[0]));
    return text;
}

// printf-style formatting over [CVarArg], whose elements are existentials holding their value inline.
SWIFTCC SwiftString string_format_arguments(SwiftString format, void* arguments) {
    const std::string pattern = swift_string_to_utf8(format);
    int64_t count = 0;
    memcpy(&count, static_cast<char*>(arguments) + kArrayCountOffset, sizeof count);
    const auto* values = reinterpret_cast<const FormatArgument*>(static_cast<char*>(arguments) + kArrayHeaderBytes);
    std::string out;
    int64_t next = 0;
    for (size_t i = 0; i < pattern.size(); ++i) {
        if (pattern[i] != '%') {
            out += pattern[i];
            continue;
        }
        if (i + 1 < pattern.size() && pattern[i + 1] == '%') {
            out += '%';
            ++i;
            continue;
        }
        size_t end = i + 1;
        while (end < pattern.size() && strchr("0123456789.+-# lzq$", pattern[end]) != nullptr) ++end;
        if (end >= pattern.size()) break;
        const std::string specifier = pattern.substr(i, end - i + 1);
        if (next < count) out += format_argument_text(values[next++], specifier);
        i = end;
    }
    return swift_string_from_utf8(out);
}

// StringProtocol's Foundation API: Self and its generic arguments are only ever String here, passed by address.
SWIFTCC bool string_contains(const SwiftString* other, void*, void*, void*, void*, SWIFT_CONTEXT const SwiftString* self) {
    return swift_string_to_utf8(*self).find(swift_string_to_utf8(*other)) != std::string::npos;
}

std::string ascii_lowered(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return static_cast<char>(tolower(c)); });
    return text;
}

SWIFTCC bool string_contains_any_case(const SwiftString* other, void*, void*, void*, void*, SWIFT_CONTEXT const SwiftString* self) {
    return ascii_lowered(swift_string_to_utf8(*self)).find(ascii_lowered(swift_string_to_utf8(*other))) != std::string::npos;
}

constexpr int64_t kOrderedAscending = -1;

constexpr int64_t kOrderedDescending = 1;

SWIFTCC int64_t string_compare_any_case(const SwiftString* other, void*, void*, void*, void*, SWIFT_CONTEXT const SwiftString* self) {
    const int order = ascii_lowered(swift_string_to_utf8(*self)).compare(ascii_lowered(swift_string_to_utf8(*other)));
    return order < 0 ? kOrderedAscending : order > 0 ? kOrderedDescending
                                                     : 0;
}

SWIFTCC SwiftString string_replacing(const SwiftString* target, const SwiftString* replacement, uint64_t, uint64_t, uint64_t, uint64_t, void*, void*, SWIFT_CONTEXT const SwiftString* self) {
    std::string text = swift_string_to_utf8(*self);
    const std::string from = swift_string_to_utf8(*target);
    const std::string to = swift_string_to_utf8(*replacement);
    if (!from.empty()) {
        for (size_t at = text.find(from); at != std::string::npos; at = text.find(from, at + to.size())) text.replace(at, from.size(), to);
    }
    return swift_string_from_utf8(text);
}

// CharacterSet is one reference to a set of scalars, or its complement. Letter classes only cover ASCII.
struct CharacterSetBox {
    HeapObject header;
    std::u32string* members;
    bool inverted;
};

void destroy_character_set(void* raw) {
    delete static_cast<CharacterSetBox*>(raw)->members;
    free(raw);
}

const HeapMetadata kCharacterSetMetadata = {destroy_character_set, nullptr, 0};

struct Utf8Scalar {
    char32_t scalar;
    size_t offset;
    size_t length;
};

std::vector<Utf8Scalar> utf8_scalars(const std::string& text) {
    std::vector<Utf8Scalar> scalars;
    for (size_t i = 0; i < text.size();) {
        const auto first = static_cast<unsigned char>(text[i]);
        size_t length = first < 0x80 ? 1 : first < 0xe0 ? 2 : first < 0xf0 ? 3 : 4;
        if (i + length > text.size()) length = text.size() - i;
        char32_t scalar = length == 1 ? first : first & (0xff >> (length + 1));
        for (size_t j = 1; j < length; ++j) scalar = (scalar << 6) | (static_cast<unsigned char>(text[i + j]) & 0x3f);
        scalars.push_back({scalar, i, length});
        i += length;
    }
    return scalars;
}

std::u32string scalars_of(const std::string& text) {
    std::u32string out;
    for (const Utf8Scalar& item : utf8_scalars(text)) out += item.scalar;
    return out;
}

CharacterSetBox* new_character_set(std::u32string members, bool inverted = false) {
    auto* box = static_cast<CharacterSetBox*>(calloc(1, sizeof(CharacterSetBox)));
    box->header.metadata = const_cast<uint64_t*>(&kCharacterSetMetadata.kind);
    box->header.references = 1;
    box->members = new std::u32string(std::move(members));
    box->inverted = inverted;
    return box;
}

CharacterSetBox* new_character_set(const std::string& members) { return new_character_set(scalars_of(members)); }

bool set_contains(const CharacterSetBox* set, char32_t scalar) { return (set->members->find(scalar) != std::u32string::npos) != set->inverted; }

// CharacterSet is a value type: a copy has to stop sharing the box before it changes.
CharacterSetBox* unique_character_set(CharacterSetBox** self) {
    CharacterSetBox* box = *self;
    if (box->header.references.load(std::memory_order_relaxed) == 1) return box;
    CharacterSetBox* copy = new_character_set(*box->members, box->inverted);
    rt_swift_release(box);
    *self = copy;
    return copy;
}

constexpr const char* kAlphanumerics = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";

SWIFTCC void set_whitespace(SWIFT_OUT CharacterSetBox** out) { *out = new_character_set(std::string(" \t")); }

SWIFTCC void set_newlines(SWIFT_OUT CharacterSetBox** out) { *out = new_character_set(std::string("\n\r")); }

SWIFTCC void set_whitespace_newlines(SWIFT_OUT CharacterSetBox** out) { *out = new_character_set(std::string(" \t\n\r")); }

SWIFTCC void set_alphanumerics(SWIFT_OUT CharacterSetBox** out) { *out = new_character_set(std::string(kAlphanumerics)); }

SWIFTCC void set_url_query(SWIFT_OUT CharacterSetBox** out) { *out = new_character_set(std::string(kAlphanumerics) + "-._~!$&'()*+,;=:@/?"); }

SWIFTCC void set_url_host(SWIFT_OUT CharacterSetBox** out) { *out = new_character_set(std::string(kAlphanumerics) + "-._~!$&'()*+,;=:[]"); }

SWIFTCC void set_characters_in(SWIFT_OUT CharacterSetBox** out, SwiftString text) { *out = new_character_set(swift_string_to_utf8(text)); }

SWIFTCC void set_inverted(SWIFT_OUT CharacterSetBox** out, SWIFT_CONTEXT CharacterSetBox** self) { *out = new_character_set(*(*self)->members, !(*self)->inverted); }

void add_to_set(CharacterSetBox* box, const std::u32string& scalars) {
    for (const char32_t scalar : scalars) {
        std::u32string& members = *box->members;
        if (box->inverted) members.erase(std::remove(members.begin(), members.end(), scalar), members.end());
        else if (members.find(scalar) == std::u32string::npos) members += scalar;
    }
}

void remove_from_set(CharacterSetBox* box, const std::u32string& scalars) {
    for (const char32_t scalar : scalars) {
        std::u32string& members = *box->members;
        if (!box->inverted) members.erase(std::remove(members.begin(), members.end(), scalar), members.end());
        else if (members.find(scalar) == std::u32string::npos) members += scalar;
    }
}

SWIFTCC void set_insert_characters(SwiftString text, SWIFT_CONTEXT CharacterSetBox** self) { add_to_set(unique_character_set(self), scalars_of(swift_string_to_utf8(text))); }

SWIFTCC void set_remove_characters(SwiftString text, SWIFT_CONTEXT CharacterSetBox** self) { remove_from_set(unique_character_set(self), scalars_of(swift_string_to_utf8(text))); }

SWIFTCC void set_union(SWIFT_OUT CharacterSetBox** out, CharacterSetBox** other, SWIFT_CONTEXT CharacterSetBox** self) {
    const CharacterSetBox* a = *self;
    const CharacterSetBox* b = *other;
    if (a->inverted || b->inverted) {
        // The complement of a union is what both complements share.
        std::u32string outside;
        const CharacterSetBox* source = a->inverted ? a : b;
        const CharacterSetBox* against = a->inverted ? b : a;
        for (const char32_t scalar : *source->members) {
            if (!set_contains(against, scalar)) outside += scalar;
        }
        *out = new_character_set(std::move(outside), true);
        return;
    }
    CharacterSetBox* merged = new_character_set(*a->members);
    add_to_set(merged, *b->members);
    *out = merged;
}

SWIFTCC bool set_contains_scalar(uint32_t scalar, SWIFT_CONTEXT CharacterSetBox** self) { return set_contains(*self, scalar); }

// NSCharacterSet is the same box: nothing here sends it messages.
SWIFTCC void* character_set_bridge_to_objc(SWIFT_CONTEXT CharacterSetBox** self) { return rt_swift_retain(*self); }
SWIFTCC void character_set_bridge_from_objc(SWIFT_OUT void** out, void* object) { *out = rt_swift_retain(object); }

SWIFTCC SwiftString string_trimming_set(CharacterSetBox** set, void*, void*, SWIFT_CONTEXT const SwiftString* self) {
    const std::string text = swift_string_to_utf8(*self);
    const auto scalars = utf8_scalars(text);
    size_t first = 0, last = scalars.size();
    while (first < last && set_contains(*set, scalars[first].scalar)) ++first;
    while (last > first && set_contains(*set, scalars[last - 1].scalar)) --last;
    if (first == last) return swift_string_from_utf8("");
    const size_t begin = scalars[first].offset;
    return swift_string_from_utf8(text.substr(begin, scalars[last - 1].offset + scalars[last - 1].length - begin));
}

void* string_array(const std::vector<std::string>& parts) {
    char* elements;
    void* storage = make_array_storage(type_system_symbol("$sSSN"), parts.size(), &elements);
    for (size_t i = 0; i < parts.size(); ++i) {
        const SwiftString part = swift_string_from_utf8(parts[i]);
        memcpy(elements + i * sizeof part, &part, sizeof part);
    }
    return storage;
}

SWIFTCC void* string_components_string(const SwiftString* separator, void*, void*, void*, void*, SWIFT_CONTEXT const SwiftString* self) {
    const std::string text = swift_string_to_utf8(*self);
    const std::string by = swift_string_to_utf8(*separator);
    std::vector<std::string> parts;
    size_t start = 0;
    for (size_t at = by.empty() ? std::string::npos : text.find(by); at != std::string::npos; at = text.find(by, start)) {
        parts.push_back(text.substr(start, at - start));
        start = at + by.size();
    }
    parts.push_back(text.substr(start));
    return string_array(parts);
}

SWIFTCC void* string_components_set(CharacterSetBox** set, void*, void*, SWIFT_CONTEXT const SwiftString* self) {
    const std::string text = swift_string_to_utf8(*self);
    std::vector<std::string> parts;
    size_t start = 0;
    for (const Utf8Scalar& item : utf8_scalars(text)) {
        if (!set_contains(*set, item.scalar)) continue;
        parts.push_back(text.substr(start, item.offset - start));
        start = item.offset + item.length;
    }
    parts.push_back(text.substr(start));
    return string_array(parts);
}

SWIFTCC OptionalSwiftString string_adding_percent(CharacterSetBox** set, void*, void*, SWIFT_CONTEXT const SwiftString* self) {
    std::string out;
    const std::string text = swift_string_to_utf8(*self);
    for (const Utf8Scalar& item : utf8_scalars(text)) {
        if (set_contains(*set, item.scalar)) {
            out.append(text, item.offset, item.length);
            continue;
        }
        for (size_t i = 0; i < item.length; ++i) {
            char escaped[4];
            snprintf(escaped, sizeof escaped, "%%%02X", static_cast<unsigned char>(text[item.offset + i]));
            out += escaped;
        }
    }
    const SwiftString value = swift_string_from_utf8(out);
    return {value.count_and_flags, value.object};
}

SWIFTCC OptionalSwiftString string_removing_percent(void*, void*, SWIFT_CONTEXT const SwiftString* self) {
    const SwiftString value = swift_string_from_utf8(percent_decoded(swift_string_to_utf8(*self)));
    return {value.count_and_flags, value.object};
}

SWIFTCC SwiftString string_capitalized(void*, void*, SWIFT_CONTEXT const SwiftString* self) {
    std::string text = ascii_lowered(swift_string_to_utf8(*self));
    bool start = true;
    for (char& c : text) {
        if (start) c = static_cast<char>(toupper(static_cast<unsigned char>(c)));
        start = isspace(static_cast<unsigned char>(c)) != 0;
    }
    return swift_string_from_utf8(text);
}

// String.Encoding is a raw NSStringEncoding; only UTF-8 compatible text passes through this runtime.
constexpr uint64_t kEncodingAscii = 1;

constexpr uint64_t kEncodingUtf8 = 4;

constexpr uint64_t kEncodingLatin1 = 5;

template <uint64_t Encoding>
SWIFTCC void encoding_getter(SWIFT_OUT uint64_t* out) { *out = Encoding; }

SWIFTCC void encoding_from_raw(SWIFT_OUT uint64_t* out, uint64_t raw) { *out = raw; }

// `Data?`'s none has this tag in its second word, where Data's own storage tags never reach.
constexpr uint64_t kOptionalDataNoneTag = 0xF000000000000000ULL;

SWIFTCC DataValue string_data_using(const uint64_t*, bool, void*, void*, SWIFT_CONTEXT const SwiftString* self) { return make_data(swift_string_to_utf8(*self)); }

SWIFTCC OptionalSwiftString string_from_data(uint64_t range, uint64_t storage, const uint64_t*) {
    if (storage == kOptionalDataNoneTag) return {0, 0};
    const SwiftString value = swift_string_from_utf8(data_bytes(range, storage));
    return {value.count_and_flags, value.object};
}

SWIFTCC SwiftString string_contents_of(URLBox** url, const uint64_t*, SWIFT_CONTEXT void*, SWIFT_ERROR void** error) {
    const std::string path = url_path(*(*url)->text);
    FILE* file = fopen(path.c_str(), "rb");
    if (file == nullptr) {
        *error = foundation_make_error("The file couldn't be opened.", 260);
        return {0, 0};
    }
    std::string text;
    char chunk[4096];
    for (size_t got; (got = fread(chunk, 1, sizeof chunk, file)) > 0;) text.append(chunk, got);
    fclose(file);
    return swift_string_from_utf8(text);
}

SWIFTCC void string_write_to(URLBox** url, bool, const uint64_t*, void*, void*, SWIFT_CONTEXT const SwiftString* self, SWIFT_ERROR void** error) {
    FILE* file = fopen(url_path(*(*url)->text).c_str(), "wb");
    if (file == nullptr) {
        *error = foundation_make_error("The file couldn't be saved.", 512);
        return;
    }
    const std::string text = swift_string_to_utf8(*self);
    fwrite(text.data(), 1, text.size(), file);
    fclose(file);
}

SWIFTCC SwiftString ns_localized_string(SwiftString key) { return swift_string_from_utf8(localization_lookup(swift_string_to_utf8(key))); }

namespace foundation {

void add_foundation_string_symbols(SymbolTable& table) {
    table.insert({
        {"$sSS10FoundationE6format9argumentsS2Sh_Says7CVarArg_pGhtcfC", reinterpret_cast<void*>(string_format_arguments)},
        {"$sSS10FoundationE6format_S2Sh_s7CVarArg_pdtcfC", reinterpret_cast<void*>(string_format_arguments)},
        {"$sSy10FoundationE8containsySbqd__SyRd__lF", reinterpret_cast<void*>(string_contains)},
        {"$sSy10FoundationE32localizedCaseInsensitiveContainsySbqd__SyRd__lF", reinterpret_cast<void*>(string_contains_any_case)},
        {"$sSy10FoundationE31localizedCaseInsensitiveCompareySo18NSComparisonResultVqd__SyRd__lF", reinterpret_cast<void*>(string_compare_any_case)},
        {"$sSy10FoundationE22caseInsensitiveCompareySo18NSComparisonResultVqd__SyRd__lF", reinterpret_cast<void*>(string_compare_any_case)},
        {"$sSy10FoundationE20replacingOccurrences2of4with7options5rangeSSqd___qd_0_So22NSStringCompareOptionsVSnySS5IndexVGSgtSyRd__SyRd_0_r0_lF", reinterpret_cast<void*>(string_replacing)},
        {"$sSy10FoundationE18trimmingCharacters2inSSAA12CharacterSetV_tF", reinterpret_cast<void*>(string_trimming_set)},
        {"$sSy10FoundationE10components11separatedBySaySSGqd___tSyRd__lF", reinterpret_cast<void*>(string_components_string)},
        {"$sSy10FoundationE10components11separatedBySaySSGAA12CharacterSetV_tF", reinterpret_cast<void*>(string_components_set)},
        {"$sSy10FoundationE21addingPercentEncoding21withAllowedCharactersSSSgAA12CharacterSetV_tF", reinterpret_cast<void*>(string_adding_percent)},
        {"$sSy10FoundationE23removingPercentEncodingSSSgvg", reinterpret_cast<void*>(string_removing_percent)},
        {"$sSy10FoundationE11capitalizedSSvg", reinterpret_cast<void*>(string_capitalized)},
        {"$sSy10FoundationE4data5using20allowLossyConversionAA4DataVSgSSAAE8EncodingV_SbtF", reinterpret_cast<void*>(string_data_using)},
        {"$sSS10FoundationE4data8encodingSSSgAA4DataVh_SSAAE8EncodingVtcfC", reinterpret_cast<void*>(string_from_data)},
        {"$sSS10FoundationE10contentsOf8encodingSSAA3URLVh_SSAAE8EncodingVtKcfC", reinterpret_cast<void*>(string_contents_of)},
        {"$sSy10FoundationE5write2to10atomically8encodingyAA3URLV_SbSSAAE8EncodingVtKF", reinterpret_cast<void*>(string_write_to)},
        {"$sSS10FoundationE8EncodingV4utf8ACvgZ", reinterpret_cast<void*>(encoding_getter<kEncodingUtf8>)},
        {"$sSS10FoundationE8EncodingV5asciiACvgZ", reinterpret_cast<void*>(encoding_getter<kEncodingAscii>)},
        {"$sSS10FoundationE8EncodingV9isoLatin1ACvgZ", reinterpret_cast<void*>(encoding_getter<kEncodingLatin1>)},
        {"$sSS10FoundationE8EncodingV8rawValueACSu_tcfC", reinterpret_cast<void*>(encoding_from_raw)},
        {"$s10Foundation12CharacterSetV11whitespacesACvgZ", reinterpret_cast<void*>(set_whitespace)},
        {"$s10Foundation12CharacterSetV8newlinesACvgZ", reinterpret_cast<void*>(set_newlines)},
        {"$s10Foundation12CharacterSetV22whitespacesAndNewlinesACvgZ", reinterpret_cast<void*>(set_whitespace_newlines)},
        {"$s10Foundation12CharacterSetV13alphanumericsACvgZ", reinterpret_cast<void*>(set_alphanumerics)},
        {"$s10Foundation12CharacterSetV15urlQueryAllowedACvgZ", reinterpret_cast<void*>(set_url_query)},
        {"$s10Foundation12CharacterSetV12charactersInACSSh_tcfC", reinterpret_cast<void*>(set_characters_in)},
        {"$s10Foundation12CharacterSetV8invertedACvg", reinterpret_cast<void*>(set_inverted)},
        {"$s10Foundation12CharacterSetV5unionyA2CF", reinterpret_cast<void*>(set_union)},
        {"$s10Foundation12CharacterSetV8containsySbs7UnicodeO6ScalarVF", reinterpret_cast<void*>(set_contains_scalar)},
        {"$s10Foundation12CharacterSetV14urlHostAllowedACvgZ", reinterpret_cast<void*>(set_url_host)},
        {"$s10Foundation12CharacterSetV19_bridgeToObjectiveCSo011NSCharacterC0CyF", reinterpret_cast<void*>(character_set_bridge_to_objc)},
        {"$s10Foundation12CharacterSetV36_unconditionallyBridgeFromObjectiveCyACSo011NSCharacterC0CSgFZ", reinterpret_cast<void*>(character_set_bridge_from_objc)},
        {"$s10Foundation12CharacterSetV6insert12charactersInySS_tF", reinterpret_cast<void*>(set_insert_characters)},
        {"$s10Foundation12CharacterSetV6remove12charactersInySS_tF", reinterpret_cast<void*>(set_remove_characters)},
        {"$s10Foundation17NSLocalizedString_9tableName6bundle5value7commentS2S_SSSgSo8NSBundleCS2StF", reinterpret_cast<void*>(ns_localized_string)},
    });
}

}  // namespace foundation
