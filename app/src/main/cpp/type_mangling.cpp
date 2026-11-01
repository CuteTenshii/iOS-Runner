#include "type_system_internal.h"

namespace type_system {

struct NominalReference {
    const void* descriptor = nullptr;
    const char* foreign_symbol = nullptr;
};

struct Node {
    enum class Tag { Type,
                     Nominal,
                     Protocol,
                     ListMarker,
                     FirstElement } tag = Tag::Type;
    Metadata* type = nullptr;
    NominalReference nominal;
};

using MetadataAccessor = void* (*)(uintptr_t, void*, void*, void*);

Metadata* metadata_for_descriptor(const NominalReference& nominal, const std::vector<void*>& parameters) {
    if (nominal.foreign_symbol != nullptr) return foreign_nominal(nominal.foreign_symbol, parameters);
    const auto* descriptor = static_cast<const char*>(nominal.descriptor);
    if (descriptor == nullptr) {
        engine_log("STOP: a mangled name refers to a type with no descriptor, in \"%s\"", printable(g_current_name, g_current_length).c_str());
        abort();
    }
    const std::vector<void*> arguments = key_arguments_for(descriptor, parameters);
    int32_t offset;
    memcpy(&offset, descriptor + 12, sizeof offset);
    auto accessor = reinterpret_cast<MetadataAccessor>(const_cast<char*>(descriptor + 12 + offset));
    if (arguments.size() <= 3) {
        void* first = arguments.size() > 0 ? arguments[0] : nullptr;
        void* second = arguments.size() > 1 ? arguments[1] : nullptr;
        void* third = arguments.size() > 2 ? arguments[2] : nullptr;
        return static_cast<Metadata*>(accessor(0, first, second, third));
    }
    return static_cast<Metadata*>(accessor(0, const_cast<void**>(arguments.data()), nullptr, nullptr));
}

constexpr uint32_t kContextKindExtension = 1;

constexpr uint32_t kContextKindAnonymous = 2;

constexpr uint32_t kContextKindOpaque = 4;

constexpr uint32_t kContextKindEnum = 18;

constexpr size_t kExtensionGenericOffset = 12;

constexpr size_t kAnonymousGenericOffset = 8;

constexpr size_t kOpaqueGenericOffset = 8;

constexpr size_t kClassGenericOffset = 44;

constexpr size_t kValueTypeGenericOffset = 28;

constexpr size_t kContextParentOffset = 4;

// From the outermost generic context in, the running parameter count at each generic depth.
std::vector<uint64_t> generic_depth_bases(const void* descriptor) {
    std::vector<uint64_t> bases;
    const auto* context = static_cast<const char*>(descriptor);
    while (context != nullptr && unresolved_symbol_at(reinterpret_cast<uintptr_t>(context)) == nullptr) {
        uint32_t flags;
        memcpy(&flags, context, sizeof flags);
        size_t header = 0;
        switch (flags & kDescriptorKindMask) {
            case kContextKindExtension:
                header = kExtensionGenericOffset;
                break;
            case kContextKindAnonymous:
                header = kAnonymousGenericOffset;
                break;
            case kContextKindOpaque:
                header = kOpaqueGenericOffset;
                break;
            case kContextKindClass:
                header = kClassGenericOffset;
                break;
            case kContextKindStruct:
            case kContextKindEnum:
                header = kValueTypeGenericOffset;
                break;
            default:
                break;
        }
        if (header != 0 && (flags & kDescriptorGeneric)) {
            uint16_t params;
            memcpy(&params, context + header, sizeof params);
            bases.insert(bases.begin(), params);
        }
        int32_t offset;
        memcpy(&offset, context + kContextParentOffset, sizeof offset);
        if (offset == 0) break;
        const char* field = context + kContextParentOffset + (offset & ~1);
        const char* parent = field;
        if (offset & 1) memcpy(&parent, field, sizeof parent);
        context = parent;
    }
    return bases;
}

class MangledName {
public:
    MangledName(const char* text, size_t length, void* const* generics) : cursor_(text), end_(text + length), generics_(generics) {}

    Metadata* parse() {
        while (cursor_ < end_ && failure_.empty()) step();
        if (failure_.empty() && (stack_.size() != 1 || stack_.back().tag == Node::Tag::ListMarker || stack_.back().tag == Node::Tag::FirstElement)) fail("unbalanced type stack");
        if (!failure_.empty()) return nullptr;
        return as_type(stack_.back());
    }
    const std::string& failure() const { return failure_; }

    // Parameters are stored flat: these are the cumulative counts of each enclosing generic context.
    void use_context(const void* descriptor) { depth_bases_ = generic_depth_bases(descriptor); }
    void limit_generics(size_t count) { generic_limit_ = count; }

private:
    void fail(const std::string& reason) {
        if (failure_.empty()) failure_ = reason;
    }

    Metadata* as_type(const Node& node) {
        if (node.tag == Node::Tag::Type) return node.type;
        if (node.tag == Node::Tag::Nominal) return metadata_for_descriptor(node.nominal, {});
        if (node.tag == Node::Tag::Protocol) return protocol_existential(node.nominal.foreign_symbol);
        fail("list marker used as a type");
        return nullptr;
    }

    Node pop() {
        if (stack_.empty()) {
            fail("type stack underflow");
            return {};
        }
        Node node = stack_.back();
        stack_.pop_back();
        return node;
    }

    void push_type(Metadata* type) {
        Node node;
        node.type = type;
        stack_.push_back(node);
    }

    void substitute(const Node& node) { substitutions_.push_back(node); }

    uint64_t read_natural() {
        uint64_t value = 0;
        while (cursor_ < end_ && *cursor_ >= '0' && *cursor_ <= '9') value = value * 10 + static_cast<uint64_t>(*cursor_++ - '0');
        return value;
    }

    void symbolic(uint8_t code) {
        int32_t offset;
        memcpy(&offset, cursor_, sizeof offset);
        const void* target = cursor_ + offset;
        cursor_ += 4;
        if (code == 2) target = *static_cast<void* const*>(target);
        Node node;
        node.tag = Node::Tag::Nominal;
        if (const char* foreign = unresolved_symbol_at(reinterpret_cast<uintptr_t>(target)))
            node.nominal.foreign_symbol = foreign;
        else
            node.nominal.descriptor = target;
        if (node.nominal.foreign_symbol != nullptr && strstr(node.nominal.foreign_symbol, "Mp") != nullptr &&
            strcmp(node.nominal.foreign_symbol + strlen(node.nominal.foreign_symbol) - 2, "Mp") == 0) {
            node.tag = Node::Tag::Protocol;
        } else if (cursor_ < end_ && *cursor_ == '_' && cursor_ + 1 < end_ && cursor_[1] == 'p') {
            node.tag = Node::Tag::Protocol;
        }
        stack_.push_back(node);
        if (!names_opaque_declaration(node.nominal)) substitute(node);
    }

    // An opaque type descriptor names a declaration rather than a type, so it takes no substitution slot.
    static bool names_opaque_declaration(const NominalReference& nominal) {
        if (nominal.foreign_symbol != nullptr) {
            const std::string_view symbol(nominal.foreign_symbol);
            return symbol.size() >= 4 && symbol.substr(symbol.size() - 4) == "QOMQ";
        }
        if (nominal.descriptor == nullptr) return false;
        uint32_t flags;
        memcpy(&flags, nominal.descriptor, sizeof flags);
        return (flags & kDescriptorKindMask) == kDescriptorKindOpaque;
    }

    void standard(char code) {
        switch (code) {
            case 'S':
                push_type(string_type());
                return;
            case 'i':
                push_type(pod_type("Si", 8));
                return;
            case 'u':
                push_type(pod_type("Su", 8));
                return;
            case 'b':
                push_type(named_type("Sb", kKindStruct, new Layout{LayoutKind::Pod, {}, nullptr}, 1, 1, 254));
                return;
            case 'd':
                push_type(pod_type("Sd", 8));
                return;
            case 'f':
                push_type(pod_type("Sf", 4));
                return;
            case 'J':
                push_type(two_word_type("SJ", word_type(), bridge_word_type()));
                return;
            case 's':
                push_type(named_type("Ss", kKindStruct, new Layout{LayoutKind::Aggregate, {{word_type(), 0}, {word_type(), 8}, {word_type(), 16}, {bridge_word_type(), 24}}, nullptr}, 32, 8, kPointerExtraInhabitants));
                return;
            case 'g': {
                Metadata* payload = as_type(pop());
                if (payload == nullptr) return;
                Node node;
                node.type = optional_of(payload);
                stack_.push_back(node);
                substitute(node);
                return;
            }
            case 'o': {
                imported_type();
                return;
            }
            case 'p':
            case 'P':
            case 'r':
            case 'R': {
                // Unsafe pointers: one plain word, whatever they point at.
                Node node;
                node.tag = Node::Tag::Nominal;
                node.nominal.foreign_symbol = "$Pointer";
                stack_.push_back(node);
                return;
            }
            case 'a':
            case 'D':
            case 'h': {
                Node node;
                node.tag = Node::Tag::Nominal;
                node.nominal.foreign_symbol = code == 'a' ? "$Array" : code == 'D' ? "$Dictionary"
                                                                                   : "$Set";
                stack_.push_back(node);
                return;
            }
            case 'c': {
                if (cursor_ >= end_) break;
                const char concurrency = *cursor_++;
                if (concurrency == 'A') {
                    Node node;
                    node.tag = Node::Tag::Protocol;
                    node.nominal.foreign_symbol = "_$sScAMp";
                    stack_.push_back(node);
                    return;
                }
                if (concurrency == 'c') {
                    Node node;
                    node.tag = Node::Tag::Nominal;
                    node.nominal.foreign_symbol = "$CheckedContinuation";
                    stack_.push_back(node);
                    return;
                }
                // Task, TaskGroup, ThrowingTaskGroup, AsyncStream, AsyncThrowingStream and CancellationError.
                if (strchr("TGgSsE", concurrency) != nullptr) {
                    static const std::map<char, const char*> kSymbols = {{'T', "_$sScTMn"}, {'G', "_$sScGMn"}, {'g', "_$sScgMn"}, {'S', "_$sScSMn"}, {'s', "_$sScsMn"}, {'E', "_$sScEMn"}};
                    Node node;
                    node.tag = Node::Tag::Nominal;
                    node.nominal.foreign_symbol = kSymbols.at(concurrency);
                    stack_.push_back(node);
                    return;
                }
                if (concurrency == 'C') {
                    Node node;
                    node.tag = Node::Tag::Nominal;
                    node.nominal.foreign_symbol = "_$sScCMn";
                    stack_.push_back(node);
                    return;
                }
                if (concurrency == 'M') {
                    push_type(native_type());
                    return;
                }
                if (concurrency == 'P') {
                    push_type(pod_type("ScP", 1));
                    return;
                }
                fail(std::string("unknown concurrency type Sc") + concurrency);
                return;
            }
            case 'O':
                push_type(pod_type("ObjectIdentifier", kPointerSize));
                return;
            default:
                break;
        }
        fail(std::string("unknown standard type S") + code);
    }

    std::vector<void*> pop_generic_arguments() {
        std::vector<void*> arguments;
        while (!stack_.empty() && stack_.back().tag != Node::Tag::ListMarker) {
            Node top = pop();
            if (top.tag == Node::Tag::FirstElement) continue;
            arguments.insert(arguments.begin(), as_type(top));
        }
        if (stack_.empty())
            fail("generic arguments without a list start");
        else
            pop();
        return arguments;
    }

    void bound_generic() {
        std::vector<void*> arguments = pop_generic_arguments();
        if (!failure_.empty()) return;
        for (const void* argument : arguments) {
            if (argument == nullptr) {
                fail("generic argument did not resolve");
                return;
            }
        }
        Node base = pop();
        if (base.tag != Node::Tag::Nominal) {
            fail("bound generic base is not a nominal type");
            return;
        }
        Node node;
        const char* name = base.nominal.foreign_symbol;
        if (name != nullptr && name[0] == '$') {
            std::string key = name;
            for (const void* argument : arguments) key += pointer_key(argument);
            node.type = strcmp(name, "$Pointer") == 0 ? pod_type("Bp", kPointerSize) : bridge_type(key, arguments);
            if (strcmp(name, "$Array") == 0 && !arguments.empty()) g_array_elements[node.type] = arguments[0];
            if (strcmp(name, "$Dictionary") == 0 && arguments.size() == 2) g_dictionary_values[node.type] = arguments[1];
        } else {
            node.type = metadata_for_descriptor(base.nominal, arguments);
        }
        stack_.push_back(node);
        substitute(node);
    }

    void tuple() {
        std::vector<Metadata*> elements;
        // A one-element pack has no marker, and must stop at the enclosing generic list.
        while (!stack_.empty() && stack_.back().tag != Node::Tag::ListMarker) {
            Node top = pop();
            if (top.tag == Node::Tag::FirstElement) {
                elements.insert(elements.begin(), as_type(pop()));
                break;
            }
            elements.insert(elements.begin(), as_type(top));
        }
        if (!failure_.empty()) return;
        for (const auto* element : elements) {
            if (element == nullptr) {
                fail("tuple element did not resolve");
                return;
            }
        }
        push_type(tuple_of(elements));
    }

    // A generic argument buffer also holds the witness tables that the requirements make key arguments.
    void append_key_witnesses(const char* requirements, uint16_t count, std::vector<void*>& arguments) {
        const std::vector<void*> types = arguments;
        for (uint16_t i = 0; i < count; ++i) {
            const char* requirement = requirements + i * kGenericRequirementSize;
            uint32_t flags;
            memcpy(&flags, requirement, sizeof flags);
            if ((flags & kRequirementKindMask) != kRequirementKindProtocol || !(flags & kRequirementHasKeyArgument)) continue;
            int32_t param_offset, protocol_offset;
            memcpy(&param_offset, requirement + 4, sizeof param_offset);
            memcpy(&protocol_offset, requirement + 8, sizeof protocol_offset);
            // The low bit of the offset marks a mangled name.
            const char* param_name = requirement + 4 + (param_offset & ~1);
            const size_t length = mangled_name_length(param_name);
            MangledName parameter(param_name, length, types.data());
            parameter.use_context(context_for_requirements_);
            parameter.limit_generics(types.size());
            // A subject fixed by a same-type constraint is not passed, and its witness is only forwarded.
            Metadata* type = parameter.parse();
            const char* protocol_field = requirement + 8 + (protocol_offset & ~1);
            const void* protocol = protocol_field;
            if (protocol_offset & 1) memcpy(&protocol, protocol_field, sizeof protocol);
            arguments.push_back(type != nullptr ? swift_runtime_find_conformance(type, protocol) : swift_runtime_placeholder_conformance(protocol));
        }
    }

    void opaque_type() {
        if (cursor_ >= end_) {
            fail("truncated Q operator");
            return;
        }
        const char variety = *cursor_++;
        // A pack gathers its elements from the stack like a tuple, and is carried as one.
        if (variety == 'P') {
            tuple();
            return;
        }
        if (variety != 'o') {
            fail(std::string("unsupported Q operator ") + variety);
            return;
        }
        const uint64_t ordinal = cursor_ < end_ && *cursor_ == '_' ? 0 : read_natural() + 1;
        if (cursor_ >= end_ || *cursor_++ != '_') {
            fail("bad opaque type ordinal");
            return;
        }
        std::vector<void*> arguments = pop_generic_arguments();
        Node base = pop();
        if (base.tag != Node::Tag::Nominal) {
            fail("opaque type without a descriptor");
            return;
        }

        Node node;
        if (base.nominal.foreign_symbol != nullptr) {
            node.type = foreign_nominal(base.nominal.foreign_symbol, arguments);
        } else {
            const auto* descriptor = static_cast<const char*>(base.nominal.descriptor);
            const OpaqueRequirements found = opaque_requirements(descriptor);
            if (found.count != 0) {
                context_for_requirements_ = descriptor;
                append_key_witnesses(found.list, found.count, arguments);
            }
            const char* trailing = found.list + found.count * kGenericRequirementSize;
            int32_t offset;
            memcpy(&offset, trailing + ordinal * sizeof(int32_t), sizeof offset);
            const char* underlying = trailing + ordinal * sizeof(int32_t) + offset;
            const size_t length = mangled_name_length(underlying);
            MangledName nested(underlying, length, arguments.data());
            nested.use_context(descriptor);
            node.type = nested.parse();
            if (node.type == nullptr) {
                fail("opaque underlying type: " + nested.failure());
                return;
            }
        }
        stack_.push_back(node);
        substitute(node);
    }

    void implementation_function() {
        size_t consumed = 0;
        while (cursor_ < end_ && *cursor_ != '_') {
            if (strchr("inxygelmrodukaR", *cursor_) != nullptr) ++consumed;
            ++cursor_;
        }
        if (cursor_ >= end_) {
            fail("unterminated function type");
            return;
        }
        ++cursor_;
        // The first convention letter describes the callee, which has no type on the stack.
        for (size_t i = 1; i < consumed && !stack_.empty(); ++i) pop();
        push_type(thick_function_type());
    }

    void step() {
        const char code = *cursor_++;
        const auto byte = static_cast<uint8_t>(code);
        // 0xFF only announces that a metadata access function follows.
        if (byte == kMarkerAccessFunction) return;
        if (byte == kSymbolicAccessFunction) {
            int32_t offset;
            memcpy(&offset, cursor_, sizeof offset);
            auto function = reinterpret_cast<void* (*)(void* const*)>(const_cast<char*>(cursor_ + offset));
            cursor_ += sizeof offset;
            push_type(static_cast<Metadata*>(function(generics_)));
            return;
        }
        if (byte >= 1 && byte <= 0x17) {
            if (byte != 1 && byte != 2) {
                fail("unsupported symbolic reference kind");
                return;
            }
            symbolic(byte);
            return;
        }
        // A tuple element's label: no effect on layout, but as an identifier it takes a substitution slot.
        if (code >= '1' && code <= '9') {
            --cursor_;
            cursor_ += read_natural();
            substitute(Node{});
            return;
        }
        switch (code) {
            case 'S':
                if (cursor_ >= end_) {
                    fail("truncated standard type");
                    return;
                }
                // `S2S` repeats the standard type that follows the count.
                if (*cursor_ >= '2' && *cursor_ <= '9') {
                    const uint64_t count = read_natural();
                    if (cursor_ >= end_) {
                        fail("truncated standard type");
                        return;
                    }
                    const char repeated = *cursor_++;
                    for (uint64_t i = 0; i < count; ++i) {
                        standard(repeated);
                        if (!failure_.empty()) return;
                    }
                    return;
                }
                standard(*cursor_++);
                return;
            case 'y':
                if (cursor_ < end_ && *cursor_ == 't') {
                    ++cursor_;
                    push_type(pod_type("yt", 0));
                } else if (cursor_ < end_ && *cursor_ == 'p') {
                    ++cursor_;
                    push_type(any_type());
                } else {
                    Node marker;
                    marker.tag = Node::Tag::ListMarker;
                    stack_.push_back(marker);
                }
                return;
            case 'G':
                bound_generic();
                return;
            case 'c':
                pop();
                pop();
                push_type(thick_function_type());
                return;
            case 't':
                tuple();
                return;
            case '_':
                if (cursor_ < end_ && *cursor_ == 'p') {
                    ++cursor_;
                    Node top = pop();
                    if (top.tag == Node::Tag::Nominal) top.tag = Node::Tag::Protocol;
                    push_type(as_type(top));
                } else {
                    Node marker;
                    marker.tag = Node::Tag::FirstElement;
                    stack_.push_back(marker);
                }
                return;
            case 'x':
                generic(0, 0);
                return;
            case 'q':
                generic_parameter();
                return;
            case 'A':
                substitution();
                return;
            case 'X':
                if (cursor_ < end_ && *cursor_ == 'l') {
                    // `yXl` is AnyObject: a composition of no protocols, whose empty list was just pushed.
                    ++cursor_;
                    if (!stack_.empty() && stack_.back().tag == Node::Tag::ListMarker) pop();
                    push_type(native_type());
                    return;
                }
                if (cursor_ < end_ && *cursor_ == 'p') {
                    // An existential metatype: for these class-bound protocols, one class pointer.
                    ++cursor_;
                    pop();
                    push_type(named_type("metatype", kKindMetatype, new Layout{LayoutKind::Pod, {}, nullptr}, kPointerSize, kPointerSize, 0));
                    return;
                }
                fail("unsupported X type");
                return;
            case 'm':
                pop();
                push_type(named_type("metatype", kKindMetatype, new Layout{LayoutKind::Pod, {}, nullptr}, kPointerSize, kPointerSize, 0));
                return;
            case 'B':
                builtin();
                return;
            case 'I':
                implementation_function();
                return;
            case 'Q':
                opaque_type();
                return;
            // inout: a parameter convention, and function types here are only ever laid out as closures.
            case 'z':
                return;
            default:
                fail(std::string("unsupported operator '") + code + "'");
                return;
        }
    }

    // An imported C or Objective-C type named inline: classes and protocols are references, enums are tag words.
    void imported_type() {
        const uint64_t length = read_natural();
        if (static_cast<uint64_t>(end_ - cursor_) < length + 1) {
            fail("truncated imported type");
            return;
        }
        const std::string name(cursor_, length);
        cursor_ += length;
        const char kind = *cursor_++;
        Node node;
        if (kind == 'C' || kind == 'a') {
            node.type = native_type();
        } else if (kind == 'O') {
            node.type = pod_type("So" + name, kPointerSize);
        } else if (kind == 'V') {
            static const std::unordered_map<std::string_view, size_t> kPlainStructs = {
                {"CGRect", 32},
                {"CGPoint", 16},
                {"CGSize", 16},
                {"CGVector", 16},
                {"CGAffineTransform", 48},
                {"UIEdgeInsets", 32},
                {"NSDirectionalEdgeInsets", 32},
                {"UIOffset", 16},
                {"_NSRange", 16},
            };
            const auto found = kPlainStructs.find(name);
            if (found == kPlainStructs.end()) {
                fail("unsupported imported struct " + name);
                return;
            }
            node.type = named_type("So" + name, kKindStruct, new Layout{LayoutKind::Pod, {}, nullptr}, found->second, kPointerSize, 0);
        } else if (kind == '_' && cursor_ < end_ && *cursor_ == 'p') {
            // Its existential is one reference. The protocol and the existential each take a substitution slot.
            ++cursor_;
            node.type = native_type();
            substitute(node);
        } else {
            fail("unsupported imported type " + name);
            return;
        }
        stack_.push_back(node);
        substitute(node);
    }

    void builtin() {
        if (cursor_ >= end_) {
            fail("truncated builtin");
            return;
        }
        const char code = *cursor_++;
        switch (code) {
            case 'D':
                push_type(pod_type("BD", 96));
                return;
            case 'o':
            case 'O':
                push_type(native_type());
                return;
            case 'b':
                push_type(bridge_word_type());
                return;
            case 'w':
                push_type(word_type());
                return;
            case 'p':
                push_type(word_type());
                return;
            default:
                fail(std::string("unknown builtin B") + code);
        }
    }

    void generic(uint64_t depth, uint64_t index) {
        if (generics_ == nullptr) {
            fail("generic parameter without generic arguments");
            return;
        }
        uint64_t base = 0;
        if (depth != 0) {
            if (depth > depth_bases_.size()) {
                fail("generic parameter deeper than its context");
                return;
            }
            base = depth_bases_[depth - 1];
        }
        if (generic_limit_ != kNoGenericLimit && base + index >= generic_limit_) {
            fail("generic parameter is not passed");
            return;
        }
        push_type(static_cast<Metadata*>(generics_[base + index]));
    }

    void generic_parameter() {
        if (cursor_ < end_ && *cursor_ == 'd') {
            ++cursor_;
            const uint64_t depth = read_natural() + 1;
            if (cursor_ >= end_ || *cursor_++ != '_') {
                fail("bad generic depth");
                return;
            }
            const uint64_t index = cursor_ < end_ && *cursor_ == '_' ? 0 : read_natural() + 1;
            if (cursor_ >= end_ || *cursor_++ != '_') {
                fail("bad generic index");
                return;
            }
            generic(depth, index);
            return;
        }
        const uint64_t index = cursor_ < end_ && *cursor_ == '_' ? 0 : read_natural() + 1;
        if (cursor_ >= end_ || *cursor_++ != '_') {
            fail("bad generic parameter");
            return;
        }
        generic(0, index + 1);
    }

    void substitution() {
        for (;;) {
            bool has_count = false;
            uint64_t count = 0;
            if (cursor_ < end_ && *cursor_ >= '0' && *cursor_ <= '9') {
                count = read_natural();
                has_count = true;
            }
            if (cursor_ >= end_) {
                fail("truncated substitution");
                return;
            }
            const char letter = *cursor_++;
            if (letter == '_') {
                // Indexes past the alphabet: `A_` is 26, `A<n>_` is n + 27.
                const uint64_t index = has_count ? count + 27 : 26;
                if (index >= substitutions_.size()) {
                    fail("substitution " + std::to_string(index) + " out of range, have " + std::to_string(substitutions_.size()));
                    return;
                }
                stack_.push_back(substitutions_[index]);
                return;
            }
            const bool last = letter >= 'A' && letter <= 'Z';
            if (!last && !(letter >= 'a' && letter <= 'z')) {
                fail("malformed substitution");
                return;
            }
            const uint64_t index = static_cast<uint64_t>(last ? letter - 'A' : letter - 'a');
            if (index >= substitutions_.size()) {
                fail("substitution " + std::to_string(index) + " out of range, have " + std::to_string(substitutions_.size()));
                return;
            }
            for (uint64_t i = 0; i < (has_count ? count : 1); ++i) stack_.push_back(substitutions_[index]);
            if (last) return;
        }
    }

    const char* cursor_;
    const char* end_;
    void* const* generics_;
    std::vector<Node> stack_;
    std::vector<Node> substitutions_;
    std::string failure_;
    std::vector<uint64_t> depth_bases_;
    const void* context_for_requirements_ = nullptr;
    static constexpr uint64_t kNoGenericLimit = UINT64_MAX;
    uint64_t generic_limit_ = kNoGenericLimit;
};

std::string printable(const char* text, size_t length) {
    std::string out;
    for (size_t i = 0; i < length; ++i) {
        const auto byte = static_cast<uint8_t>(text[i]);
        if (byte >= 1 && byte <= 0x17) {
            int32_t offset;
            memcpy(&offset, text + i + 1, sizeof offset);
            const void* target = text + i + 1 + offset;
            if (byte == 2) target = *static_cast<void* const*>(target);
            const char* symbol = unresolved_symbol_at(reinterpret_cast<uintptr_t>(target));
            out += symbol != nullptr ? std::string("<") + symbol + ">" : std::string("<app>");
            i += 4;
        } else if (byte >= 0x20 && byte < 0x7f) {
            out += static_cast<char>(byte);
        } else {
            out += '?';
        }
    }
    return out;
}

extern "C" void* rt_getTypeByMangledNameInContext2(const char* name, size_t length, const void* context, void* const* generics) {
    const std::lock_guard guard(g_types_lock);
    const char* outer_name = g_current_name;
    const size_t outer_length = g_current_length;
    g_current_name = name;
    g_current_length = length;
    MangledName parser(name, length, generics);
    if (context != nullptr) parser.use_context(context);
    Metadata* result = parser.parse();
    g_current_name = outer_name;
    g_current_length = outer_length;
    if (result == nullptr) {
        engine_log("STOP: cannot resolve type: %s, in \"%s\"", parser.failure().c_str(), printable(name, length).c_str());
        abort();
    }
    return result;
}

extern "C" void* rt_getTypeByMangledNameInContextInMetadataState2(size_t, const char* name, size_t length, const void* context, void* const* generics) {
    return rt_getTypeByMangledNameInContext2(name, length, context, generics);
}

}  // namespace type_system
