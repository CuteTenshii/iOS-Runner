#include "regex_shim.h"

#include <atomic>
#include <bitset>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <memory>
#include <new>
#include <string>
#include <unordered_map>
#include <vector>

#include "engine.h"
#include "foundation_shim.h"
#include "key_path.h"
#include "type_system.h"

#define SWIFTCC __attribute__((swiftcall))
#define SWIFT_OUT __attribute__((swift_indirect_result))
#define SWIFT_CTX __attribute__((swift_context))
#define SWIFT_ERROR __attribute__((swift_error_result))

extern "C" void* rt_swift_retain(void*);
extern "C" void rt_swift_bridgeObjectRelease(void*);
extern "C" void* rt_swift_bridgeObjectRetain(void*);

namespace {

constexpr int kUnbounded = -1;

struct Node {
    enum class Kind { Set, Start, End, Sequence, Alternation, Repeat, Capture };
    Kind kind = Kind::Sequence;
    std::bitset<256> set;
    std::vector<std::unique_ptr<Node>> children;
    int minimum = 0;
    int maximum = 0;
    int capture = 0;
};

class Parser {
public:
    explicit Parser(const std::string& pattern) : pattern_(pattern) {}

    std::unique_ptr<Node> parse(int& captures) {
        auto root = alternation();
        captures = captures_;
        if (cursor_ != pattern_.size()) fail("unexpected )");
        return root;
    }

private:
    [[noreturn]] void fail(const char* why) {
        engine_log("STOP: regex /%s/ is outside this runtime's syntax: %s", pattern_.c_str(), why);
        abort();
    }

    bool more() const { return cursor_ < pattern_.size(); }

    std::unique_ptr<Node> alternation() {
        auto first = sequence();
        if (!more() || pattern_[cursor_] != '|') return first;
        auto node = std::make_unique<Node>();
        node->kind = Node::Kind::Alternation;
        node->children.push_back(std::move(first));
        while (more() && pattern_[cursor_] == '|') {
            ++cursor_;
            node->children.push_back(sequence());
        }
        return node;
    }

    std::unique_ptr<Node> sequence() {
        auto node = std::make_unique<Node>();
        while (more() && pattern_[cursor_] != '|' && pattern_[cursor_] != ')') node->children.push_back(quantified());
        return node;
    }

    std::unique_ptr<Node> quantified() {
        auto atom_node = atom();
        if (!more()) return atom_node;
        int minimum;
        int maximum;
        const char c = pattern_[cursor_];
        if (c == '*') {
            minimum = 0;
            maximum = kUnbounded;
            ++cursor_;
        } else if (c == '+') {
            minimum = 1;
            maximum = kUnbounded;
            ++cursor_;
        } else if (c == '?') {
            minimum = 0;
            maximum = 1;
            ++cursor_;
        } else if (c == '{') {
            ++cursor_;
            minimum = number();
            maximum = minimum;
            if (more() && pattern_[cursor_] == ',') {
                ++cursor_;
                maximum = more() && pattern_[cursor_] != '}' ? number() : kUnbounded;
            }
            if (!more() || pattern_[cursor_] != '}') fail("unterminated {");
            ++cursor_;
        } else {
            return atom_node;
        }
        if (more() && (pattern_[cursor_] == '?' || pattern_[cursor_] == '+')) fail("lazy and possessive quantifiers");
        auto node = std::make_unique<Node>();
        node->kind = Node::Kind::Repeat;
        node->minimum = minimum;
        node->maximum = maximum;
        node->children.push_back(std::move(atom_node));
        return node;
    }

    int number() {
        int value = 0;
        while (more() && pattern_[cursor_] >= '0' && pattern_[cursor_] <= '9') value = value * 10 + (pattern_[cursor_++] - '0');
        return value;
    }

    static void add_range(std::bitset<256>& set, unsigned from, unsigned to) {
        for (unsigned c = from; c <= to; ++c) set.set(c);
    }

    // True if the escape names a class; otherwise `literal` receives the escaped character.
    bool escape_into(std::bitset<256>& set, char& literal) {
        if (!more()) fail("trailing backslash");
        const char c = pattern_[cursor_++];
        std::bitset<256> named;
        switch (c) {
            case 'd': case 'D':
                add_range(named, '0', '9');
                break;
            case 'w': case 'W':
                add_range(named, '0', '9');
                add_range(named, 'a', 'z');
                add_range(named, 'A', 'Z');
                named.set('_');
                add_range(named, 0x80, 0xFF);
                break;
            case 's': case 'S':
                named.set(' ');
                add_range(named, 0x09, 0x0D);
                break;
            case 'n': literal = '\n'; return false;
            case 't': literal = '\t'; return false;
            case 'r': literal = '\r'; return false;
            default:
                if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) fail("unsupported escape");
                literal = c;
                return false;
        }
        set |= (c == 'D' || c == 'W' || c == 'S') ? ~named : named;
        return true;
    }

    std::unique_ptr<Node> atom() {
        auto node = std::make_unique<Node>();
        const char c = pattern_[cursor_++];
        node->kind = Node::Kind::Set;
        if (c == '^') {
            node->kind = Node::Kind::Start;
        } else if (c == '$') {
            node->kind = Node::Kind::End;
        } else if (c == '.') {
            node->set.set();
            node->set.reset('\n');
        } else if (c == '\\') {
            char literal;
            if (!escape_into(node->set, literal)) node->set.set(static_cast<unsigned char>(literal));
        } else if (c == '[') {
            character_class(node->set);
        } else if (c == '(') {
            int capture = -1;
            if (pattern_.compare(cursor_, 2, "?:") == 0) cursor_ += 2;
            else if (more() && pattern_[cursor_] == '?') fail("group options");
            else capture = ++captures_;
            auto inner = alternation();
            if (!more() || pattern_[cursor_] != ')') fail("unterminated (");
            ++cursor_;
            if (capture < 0) return inner;
            node->kind = Node::Kind::Capture;
            node->capture = capture;
            node->children.push_back(std::move(inner));
        } else {
            node->set.set(static_cast<unsigned char>(c));
        }
        return node;
    }

    void character_class(std::bitset<256>& set) {
        std::bitset<256> members;
        bool negated = false;
        if (more() && pattern_[cursor_] == '^') {
            negated = true;
            ++cursor_;
        }
        bool first = true;
        while (more() && (pattern_[cursor_] != ']' || first)) {
            first = false;
            char low = pattern_[cursor_++];
            if (low == '\\' && escape_into(members, low)) continue;
            if (cursor_ + 1 < pattern_.size() && pattern_[cursor_] == '-' && pattern_[cursor_ + 1] != ']') {
                ++cursor_;
                char high = pattern_[cursor_++];
                if (high == '\\' && escape_into(members, high)) fail("class in a range");
                add_range(members, static_cast<unsigned char>(low), static_cast<unsigned char>(high));
            } else {
                members.set(static_cast<unsigned char>(low));
            }
        }
        if (!more()) fail("unterminated [");
        ++cursor_;
        set = negated ? ~members : members;
    }

    const std::string& pattern_;
    size_t cursor_ = 0;
    int captures_ = 0;
};

using Spans = std::vector<std::pair<size_t, size_t>>;
using Continuation = std::function<bool(size_t)>;

constexpr size_t kUnsetSpan = static_cast<size_t>(-1);

// Backtracks through continuations; `text` is the subject and `spans` its captures so far.
bool match_node(const Node& node, const std::string& text, size_t at, Spans& spans, const Continuation& next);

bool match_sequence(const Node& node, size_t index, const std::string& text, size_t at, Spans& spans, const Continuation& next) {
    if (index == node.children.size()) return next(at);
    return match_node(*node.children[index], text, at, spans, [&, index](size_t after) { return match_sequence(node, index + 1, text, after, spans, next); });
}

bool match_repeat(const Node& node, int count, const std::string& text, size_t at, Spans& spans, const Continuation& next) {
    if (node.maximum == kUnbounded || count < node.maximum) {
        const bool matched = match_node(*node.children[0], text, at, spans, [&, count, at](size_t after) {
            // An empty iteration can repeat forever without consuming anything.
            if (after == at) return false;
            return match_repeat(node, count + 1, text, after, spans, next);
        });
        if (matched) return true;
    }
    return count >= node.minimum && next(at);
}

bool match_node(const Node& node, const std::string& text, size_t at, Spans& spans, const Continuation& next) {
    switch (node.kind) {
        case Node::Kind::Set:
            return at < text.size() && node.set.test(static_cast<unsigned char>(text[at])) && next(at + 1);
        case Node::Kind::Start:
            return at == 0 && next(at);
        case Node::Kind::End:
            return at == text.size() && next(at);
        case Node::Kind::Sequence:
            return match_sequence(node, 0, text, at, spans, next);
        case Node::Kind::Alternation:
            for (const auto& child : node.children) {
                if (match_node(*child, text, at, spans, next)) return true;
            }
            return false;
        case Node::Kind::Repeat:
            return match_repeat(node, 0, text, at, spans, next);
        case Node::Kind::Capture: {
            const auto saved = spans[static_cast<size_t>(node.capture)];
            const bool matched = match_node(*node.children[0], text, at, spans, [&, at](size_t after) {
                const auto inner = spans[static_cast<size_t>(node.capture)];
                spans[static_cast<size_t>(node.capture)] = {at, after};
                if (next(after)) return true;
                spans[static_cast<size_t>(node.capture)] = inner;
                return false;
            });
            if (!matched) spans[static_cast<size_t>(node.capture)] = saved;
            return matched;
        }
    }
    return false;
}

struct StorageMetadata {
    void (*destroyer)(void*);
    const void* witnesses;
    uint64_t kind;
};

struct RegexBox {
    void* metadata;
    std::atomic<uint64_t> references;
    Node* root;
    int captures;
};

void destroy_regex(void* raw) {
    delete static_cast<RegexBox*>(raw)->root;
    free(raw);
}

const StorageMetadata kRegexMetadata = {destroy_regex, nullptr, 0};

// Regex.Match: the Output value, laid out as the app's compiled key paths expect.
struct MatchBox {
    void* metadata;
    std::atomic<uint64_t> references;
    void* output;
};

void destroy_match(void* raw) {
    free(static_cast<MatchBox*>(raw)->output);
    free(raw);
}

const StorageMetadata kMatchMetadata = {destroy_match, nullptr, 0};

SWIFTCC void regex_init(SWIFT_OUT RegexBox** out, SwiftString pattern, int64_t) {
    auto* box = static_cast<RegexBox*>(calloc(1, sizeof(RegexBox)));
    box->metadata = const_cast<uint64_t*>(&kRegexMetadata.kind);
    new (&box->references) std::atomic<uint64_t>(1);
    // The compiler passes the literal as written, delimiters included.
    std::string source = swift_string_to_utf8(pattern);
    size_t hashes = 0;
    while (hashes < source.size() && source[hashes] == '#') ++hashes;
    if (source.size() >= 2 * hashes + 2 && source[hashes] == '/') source = source.substr(hashes + 1, source.size() - 2 * hashes - 2);
    box->root = Parser(source).parse(box->captures).release();
    *out = box;
}

SWIFTCC void regex_component_regex(SWIFT_OUT RegexBox** out, void*, void*, SWIFT_CTX RegexBox** self) {
    *out = static_cast<RegexBox*>(rt_swift_retain(*self));
}

// Substring's two indices are UTF-8 offsets above the low flag bits.
constexpr unsigned kIndexOffsetShift = 16;
constexpr uint64_t kIndexFlags = 0xf;

struct SubstringWords {
    uint64_t start;
    uint64_t end;
    SwiftString guts;
};

SubstringWords substring_of(const SwiftString& guts, size_t start, size_t end) {
    rt_swift_bridgeObjectRetain(reinterpret_cast<void*>(guts.object));
    return {(static_cast<uint64_t>(start) << kIndexOffsetShift) | kIndexFlags, (static_cast<uint64_t>(end) << kIndexOffsetShift) | kIndexFlags, guts};
}

// `regex_type` is Regex<Output>, whose first generic argument is the Output.
SWIFTCC void regex_whole_match(SWIFT_OUT MatchBox** out, uint64_t start, uint64_t end, SwiftString guts, void* regex_type, SWIFT_CTX RegexBox** self, SWIFT_ERROR void**) {
    const std::string whole = swift_string_to_utf8(guts);
    const size_t low = start >> kIndexOffsetShift;
    const size_t high = end >> kIndexOffsetShift;
    const std::string text = whole.substr(low, high - low);
    Spans spans(static_cast<size_t>((*self)->captures) + 1, {kUnsetSpan, kUnsetSpan});
    const bool matched = match_node(*(*self)->root, text, 0, spans, [&](size_t after) { return after == text.size(); });
    if (!matched) {
        *out = nullptr;
        return;
    }
    spans[0] = {0, text.size()};
    void* output_type = static_cast<void**>(regex_type)[2];
    constexpr size_t kValueWitnessSizeOffset = 0x40;
    size_t size;
    memcpy(&size, static_cast<const char*>(static_cast<void**>(output_type)[-1]) + kValueWitnessSizeOffset, sizeof size);
    auto* output = static_cast<char*>(calloc(1, size));
    const auto fields = type_fields(output_type);
    if (fields.empty()) {
        const SubstringWords value = substring_of(guts, low, high);
        memcpy(output, &value, sizeof value);
    } else {
        for (size_t i = 0; i < fields.size() && i < spans.size(); ++i) {
            const size_t from = spans[i].first == kUnsetSpan ? low : low + spans[i].first;
            const size_t to = spans[i].first == kUnsetSpan ? low : low + spans[i].second;
            const SubstringWords value = substring_of(guts, from, to);
            memcpy(output + fields[i].offset, &value, sizeof value);
        }
    }
    auto* box = static_cast<MatchBox*>(calloc(1, sizeof(MatchBox)));
    box->metadata = const_cast<uint64_t*>(&kMatchMetadata.kind);
    new (&box->references) std::atomic<uint64_t>(1);
    box->output = output;
    *out = box;
}

SWIFTCC void regex_match_member(SWIFT_OUT void* out, const void* key_path, void* value_type, void*, SWIFT_CTX MatchBox** self) {
    key_path_get(key_path, (*self)->output, out, value_type);
}

// String.replacing(_:with:maxReplacements:); the app only replaces in String, with a String.
SWIFTCC void regex_replacing(SWIFT_OUT SwiftString* out, RegexBox** regex, const SwiftString* replacement, int64_t maximum, void*, void*, void*, void*, void*, SWIFT_CTX const SwiftString* self) {
    const std::string text = swift_string_to_utf8(*self);
    const std::string with = swift_string_to_utf8(*replacement);
    std::string result;
    size_t at = 0;
    int64_t done = 0;
    while (at <= text.size()) {
        Spans spans(static_cast<size_t>((*regex)->captures) + 1, {kUnsetSpan, kUnsetSpan});
        size_t end = at;
        const bool matched = done < maximum && match_node(*(*regex)->root, text, at, spans, [&](size_t after) {
            end = after;
            return true;
        });
        if (matched && end > at) {
            result += with;
            at = end;
            ++done;
        } else {
            if (at < text.size()) result += text[at];
            ++at;
        }
    }
    *out = swift_string_from_utf8(result);
}

}  // namespace

void* regex_symbol(std::string_view name) {
    static const std::unordered_map<std::string_view, void*> table = {
        {"$s17_StringProcessing5RegexV06_regexA07versionACyxGSS_SitcfC", reinterpret_cast<void*>(regex_init)},
        {"$s17_StringProcessing14RegexComponentP5regexAA0C0Vy0C6OutputQzGvgTj", reinterpret_cast<void*>(regex_component_regex)},
        {"$s17_StringProcessing5RegexV10wholeMatch2inAC0E0Vyx_GSgSs_tKF", reinterpret_cast<void*>(regex_whole_match)},
        {"$s17_StringProcessing5RegexV5MatchV13dynamicMemberqd__s7KeyPathCyxqd__G_tcluig", reinterpret_cast<void*>(regex_match_member)},
    };
    if (name.rfind("$sSm17_StringProcessingSs11SubSequenceRtzrlE9replacing_4with15maxReplacements", 0) == 0) return reinterpret_cast<void*>(regex_replacing);
    const auto found = table.find(name);
    return found == table.end() ? nullptr : found->second;
}
