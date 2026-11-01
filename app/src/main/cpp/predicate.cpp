#include "predicate.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <regex>
#include <utility>

PredicateValue PredicateValue::of_bool(bool value) {
    PredicateValue out;
    out.kind = Kind::Boolean;
    out.boolean = value;
    return out;
}

PredicateValue PredicateValue::of_number(double value) {
    PredicateValue out;
    out.kind = Kind::Number;
    out.number = value;
    return out;
}

PredicateValue PredicateValue::of_string(std::string value) {
    PredicateValue out;
    out.kind = Kind::String;
    out.text = std::move(value);
    return out;
}

PredicateValue PredicateValue::of_object(void* value) {
    PredicateValue out;
    out.kind = value != nullptr ? Kind::Object : Kind::Null;
    out.object = value;
    return out;
}

enum class CompareOp { Equal,
                       NotEqual,
                       Less,
                       LessEqual,
                       Greater,
                       GreaterEqual,
                       In,
                       Contains,
                       BeginsWith,
                       EndsWith,
                       Like,
                       Matches,
                       Between };
enum class Quantifier { None,
                        Any,
                        All,
                        NoneOf };

struct PredicateExpr {
    enum class Kind { Constant,
                      Self,
                      Variable,
                      Key,
                      Subquery };
    Kind kind = Kind::Constant;
    PredicateValue constant;
    std::string name;  // A variable's name, a key, or a subquery's variable.
    std::shared_ptr<const PredicateExpr> base;
    PredicateRef filter;
};
using ExprRef = std::shared_ptr<const PredicateExpr>;

struct PredicateNode {
    enum class Kind { Constant,
                      And,
                      Or,
                      Not,
                      Compare };
    Kind kind = Kind::Constant;
    bool constant = false;
    std::vector<PredicateRef> operands;
    Quantifier quantifier = Quantifier::None;
    CompareOp op = CompareOp::Equal;
    bool ignore_case = false;
    ExprRef left, right;
};

namespace {

std::string lowercase(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return static_cast<char>(tolower(c)); });
    return text;
}

class Parser {
public:
    Parser(const std::string& format, const std::vector<PredicateValue>& arguments) : text_(format), arguments_(arguments) {}

    PredicateRef parse(std::string& error) {
        PredicateRef result = parse_or();
        skip_space();
        if (result != nullptr && at_ != text_.size()) fail("unexpected text");
        error = error_;
        return error_.empty() ? result : nullptr;
    }

private:
    const std::string& text_;
    const std::vector<PredicateValue>& arguments_;
    size_t at_ = 0;
    size_t next_argument_ = 0;
    std::string error_;

    void fail(const std::string& message) {
        if (error_.empty()) error_ = message + " at offset " + std::to_string(at_);
    }

    void skip_space() {
        while (at_ < text_.size() && isspace(static_cast<unsigned char>(text_[at_]))) ++at_;
    }

    static bool is_word_char(char c) { return isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '@'; }

    bool take_symbol(const char* symbol) {
        skip_space();
        const size_t length = strlen(symbol);
        if (text_.compare(at_, length, symbol) != 0) return false;
        at_ += length;
        return true;
    }

    bool take_word(const char* word) {
        skip_space();
        const size_t length = strlen(word);
        if (at_ + length > text_.size() || lowercase(text_.substr(at_, length)) != lowercase(word)) return false;
        if (at_ + length < text_.size() && is_word_char(text_[at_ + length])) return false;
        at_ += length;
        return true;
    }

    PredicateRef parse_or() {
        std::vector<PredicateRef> operands{parse_and()};
        while (error_.empty() && (take_word("OR") || take_symbol("||"))) operands.push_back(parse_and());
        return operands.size() == 1 ? operands[0] : predicate_or(std::move(operands));
    }

    PredicateRef parse_and() {
        std::vector<PredicateRef> operands{parse_not()};
        while (error_.empty() && (take_word("AND") || take_symbol("&&"))) operands.push_back(parse_not());
        return operands.size() == 1 ? operands[0] : predicate_and(std::move(operands));
    }

    PredicateRef parse_not() {
        if (take_word("NOT")) return predicate_not(parse_not());
        // "!=" can't begin a predicate, so a leading "!" is always negation.
        if (take_symbol("!")) return predicate_not(parse_not());
        return parse_atom();
    }

    PredicateRef parse_atom() {
        if (take_word("TRUEPREDICATE")) return predicate_constant(true);
        if (take_word("FALSEPREDICATE")) return predicate_constant(false);
        skip_space();
        if (at_ < text_.size() && text_[at_] == '(') {
            // Parentheses group either a predicate or an expression, which only trying can tell apart.
            const size_t start = at_, start_argument = next_argument_;
            ++at_;
            PredicateRef inner = parse_or();
            if (error_.empty() && take_symbol(")")) {
                skip_space();
                if (!starts_comparison()) return inner;
            }
            at_ = start;
            next_argument_ = start_argument;
            error_.clear();
        }
        return parse_comparison();
    }

    // Peeks, for a parenthesis that turned out to hold an expression.
    bool starts_comparison() {
        const size_t start = at_;
        CompareOp op;
        const bool found = at_ < text_.size() && (text_[at_] == '.' || take_operator(op));
        at_ = start;
        return found;
    }

    bool take_operator(CompareOp& op) {
        if (take_symbol("==") || take_symbol("=<") || take_symbol("=>")) {
            const std::string taken = text_.substr(at_ - 2, 2);
            op = taken == "==" ? CompareOp::Equal : taken == "=<" ? CompareOp::LessEqual
                                                                  : CompareOp::GreaterEqual;
        } else if (take_symbol("!=") || take_symbol("<>")) {
            op = CompareOp::NotEqual;
        } else if (take_symbol("<=")) {
            op = CompareOp::LessEqual;
        } else if (take_symbol(">=")) {
            op = CompareOp::GreaterEqual;
        } else if (take_symbol("=")) {
            op = CompareOp::Equal;
        } else if (take_symbol("<")) {
            op = CompareOp::Less;
        } else if (take_symbol(">")) {
            op = CompareOp::Greater;
        } else if (take_word("IN")) {
            op = CompareOp::In;
        } else if (take_word("CONTAINS")) {
            op = CompareOp::Contains;
        } else if (take_word("BEGINSWITH")) {
            op = CompareOp::BeginsWith;
        } else if (take_word("ENDSWITH")) {
            op = CompareOp::EndsWith;
        } else if (take_word("LIKE")) {
            op = CompareOp::Like;
        } else if (take_word("MATCHES")) {
            op = CompareOp::Matches;
        } else if (take_word("BETWEEN")) {
            op = CompareOp::Between;
        } else {
            return false;
        }
        return true;
    }

    PredicateRef parse_comparison() {
        auto node = std::make_shared<PredicateNode>();
        node->kind = PredicateNode::Kind::Compare;
        if (take_word("ANY") || take_word("SOME"))
            node->quantifier = Quantifier::Any;
        else if (take_word("ALL"))
            node->quantifier = Quantifier::All;
        else if (take_word("NONE"))
            node->quantifier = Quantifier::NoneOf;
        node->left = parse_expression();
        if (!error_.empty()) return nullptr;
        skip_space();
        if (!take_operator(node->op)) {
            fail("expected a comparison");
            return nullptr;
        }
        skip_space();
        if (at_ < text_.size() && text_[at_] == '[') {
            const size_t end = text_.find(']', at_);
            if (end == std::string::npos) {
                fail("unterminated options");
                return nullptr;
            }
            // Diacritic folding needs Unicode tables, and these comparisons only fold ASCII case.
            node->ignore_case = text_.substr(at_, end - at_).find('c') != std::string::npos;
            at_ = end + 1;
        }
        node->right = parse_expression();
        return error_.empty() ? node : nullptr;
    }

    const PredicateValue* take_argument() {
        if (next_argument_ >= arguments_.size()) {
            fail("too few arguments");
            return nullptr;
        }
        return &arguments_[next_argument_++];
    }

    static ExprRef constant(PredicateValue value) {
        auto out = std::make_shared<PredicateExpr>();
        out->constant = std::move(value);
        return out;
    }

    static ExprRef key_of(ExprRef base, const std::string& path) {
        size_t start = 0;
        for (;;) {
            const size_t dot = path.find('.', start);
            auto out = std::make_shared<PredicateExpr>();
            out->kind = PredicateExpr::Kind::Key;
            out->base = std::move(base);
            out->name = path.substr(start, dot == std::string::npos ? std::string::npos : dot - start);
            base = out;
            if (dot == std::string::npos) return base;
            start = dot + 1;
        }
    }

    static ExprRef self_expression() {
        auto out = std::make_shared<PredicateExpr>();
        out->kind = PredicateExpr::Kind::Self;
        return out;
    }

    // A %K argument, which is a key path even though it arrives as a string.
    ExprRef key_argument(ExprRef base) {
        const PredicateValue* argument = take_argument();
        if (argument == nullptr) return nullptr;
        if (argument->kind != PredicateValue::Kind::String) {
            fail("%K needs a string");
            return nullptr;
        }
        return key_of(std::move(base), argument->text);
    }

    ExprRef parse_expression() {
        ExprRef value = parse_primary();
        while (error_.empty() && value != nullptr && at_ < text_.size() && text_[at_] == '.') {
            ++at_;
            if (text_.compare(at_, 2, "%K") == 0) {
                at_ += 2;
                value = key_argument(std::move(value));
            } else {
                const std::string word = take_identifier();
                if (word.empty())
                    fail("expected a key");
                else
                    value = key_of(std::move(value), word);
            }
        }
        return value;
    }

    std::string take_identifier() {
        const size_t start = at_;
        while (at_ < text_.size() && is_word_char(text_[at_])) ++at_;
        return text_.substr(start, at_ - start);
    }

    ExprRef parse_format_specifier() {
        ++at_;
        if (text_.compare(at_, 1, "K") == 0) {
            ++at_;
            return key_argument(self_expression());
        }
        const size_t start = at_;
        while (at_ < text_.size() && (text_[at_] == 'l' || text_[at_] == 'h' || text_[at_] == 'q')) ++at_;
        if (at_ < text_.size() && strchr("@dDiuUxXoOfeEgGsc", text_[at_]) != nullptr) {
            ++at_;
            const PredicateValue* argument = take_argument();
            return argument != nullptr ? constant(*argument) : nullptr;
        }
        at_ = start;
        fail("unsupported format specifier");
        return nullptr;
    }

    ExprRef parse_string() {
        const char quote = text_[at_++];
        std::string out;
        while (at_ < text_.size() && text_[at_] != quote) {
            if (text_[at_] == '\\' && at_ + 1 < text_.size()) ++at_;
            out += text_[at_++];
        }
        if (at_ >= text_.size()) {
            fail("unterminated string");
            return nullptr;
        }
        ++at_;
        return constant(PredicateValue::of_string(std::move(out)));
    }

    ExprRef parse_list() {
        ++at_;
        PredicateValue list;
        list.kind = PredicateValue::Kind::List;
        if (take_symbol("}")) return constant(std::move(list));
        do {
            const ExprRef item = parse_expression();
            if (!error_.empty()) return nullptr;
            if (item->kind != PredicateExpr::Kind::Constant) {
                fail("lists hold only constants here");
                return nullptr;
            }
            list.items.push_back(item->constant);
        } while (take_symbol(","));
        if (!take_symbol("}")) {
            fail("unterminated list");
            return nullptr;
        }
        return constant(std::move(list));
    }

    ExprRef parse_subquery() {
        auto out = std::make_shared<PredicateExpr>();
        out->kind = PredicateExpr::Kind::Subquery;
        out->base = parse_expression();
        if (!error_.empty()) return nullptr;
        skip_space();
        if (!take_symbol(",") || !take_symbol("$")) {
            fail("expected a subquery variable");
            return nullptr;
        }
        out->name = take_identifier();
        if (!take_symbol(",")) {
            fail("expected a subquery predicate");
            return nullptr;
        }
        out->filter = parse_or();
        if (error_.empty() && !take_symbol(")")) fail("unterminated subquery");
        return error_.empty() ? out : nullptr;
    }

    ExprRef parse_primary() {
        skip_space();
        if (at_ >= text_.size()) {
            fail("expected a value");
            return nullptr;
        }
        const char c = text_[at_];
        if (c == '%') return parse_format_specifier();
        if (c == '\'' || c == '"') return parse_string();
        if (c == '{') return parse_list();
        if (c == '(') {
            ++at_;
            ExprRef inner = parse_expression();
            if (error_.empty() && !take_symbol(")")) fail("unterminated parenthesis");
            return error_.empty() ? inner : nullptr;
        }
        if (c == '$') {
            ++at_;
            auto out = std::make_shared<PredicateExpr>();
            out->kind = PredicateExpr::Kind::Variable;
            out->name = take_identifier();
            return out;
        }
        const bool negative_number = c == '-' && at_ + 1 < text_.size() && isdigit(static_cast<unsigned char>(text_[at_ + 1]));
        if (negative_number || isdigit(static_cast<unsigned char>(c))) {
            char* end = nullptr;
            const double number = strtod(text_.c_str() + at_, &end);
            at_ = static_cast<size_t>(end - text_.c_str());
            return constant(PredicateValue::of_number(number));
        }
        const std::string word = take_identifier();
        if (word.empty()) {
            fail("unexpected character");
            return nullptr;
        }
        const std::string lower = lowercase(word);
        if (lower == "nil" || lower == "null") return constant(PredicateValue{});
        if (lower == "yes" || lower == "true") return constant(PredicateValue::of_bool(true));
        if (lower == "no" || lower == "false") return constant(PredicateValue::of_bool(false));
        if (lower == "self") return self_expression();
        if (lower == "subquery" && take_symbol("(")) return parse_subquery();
        if (word[0] == '@') {
            fail("unsupported aggregate " + word);
            return nullptr;
        }
        return key_of(self_expression(), word);
    }
};

using Variables = std::map<std::string, PredicateValue>;

struct Evaluator {
    const PredicateKeyReader& read_key;

    PredicateValue key_of(const PredicateValue& base, const std::string& key) const {
        if (key == "@count") {
            if (base.kind == PredicateValue::Kind::List) return PredicateValue::of_number(static_cast<double>(base.items.size()));
            if (base.kind == PredicateValue::Kind::Dictionary) return PredicateValue::of_number(static_cast<double>(base.entries.size()));
            if (base.kind == PredicateValue::Kind::String) return PredicateValue::of_number(static_cast<double>(base.text.size()));
            return PredicateValue{};
        }
        switch (base.kind) {
            case PredicateValue::Kind::Object:
                return read_key(base.object, key);
            case PredicateValue::Kind::Dictionary: {
                const auto found = base.entries.find(key);
                return found == base.entries.end() ? PredicateValue{} : found->second;
            }
            case PredicateValue::Kind::List: {
                // A key path through a to-many gathers the key from each element.
                PredicateValue out;
                out.kind = PredicateValue::Kind::List;
                for (const auto& item : base.items) {
                    PredicateValue value = key_of(item, key);
                    if (value.kind == PredicateValue::Kind::List)
                        out.items.insert(out.items.end(), value.items.begin(), value.items.end());
                    else if (value.kind != PredicateValue::Kind::Null)
                        out.items.push_back(std::move(value));
                }
                return out;
            }
            default:
                return PredicateValue{};
        }
    }

    PredicateValue value_of(const ExprRef& expr, const PredicateValue& self, const Variables& variables) const {
        switch (expr->kind) {
            case PredicateExpr::Kind::Constant:
                return expr->constant;
            case PredicateExpr::Kind::Self:
                return self;
            case PredicateExpr::Kind::Variable: {
                const auto found = variables.find(expr->name);
                return found == variables.end() ? PredicateValue{} : found->second;
            }
            case PredicateExpr::Kind::Key:
                return key_of(value_of(expr->base, self, variables), expr->name);
            case PredicateExpr::Kind::Subquery: {
                const PredicateValue collection = value_of(expr->base, self, variables);
                PredicateValue out;
                out.kind = PredicateValue::Kind::List;
                Variables inner = variables;
                for (const auto& item : collection.items) {
                    inner[expr->name] = item;
                    if (holds(expr->filter, self, inner)) out.items.push_back(item);
                }
                return out;
            }
        }
        return PredicateValue{};
    }

    static bool as_number(const PredicateValue& value, double& out) {
        if (value.kind == PredicateValue::Kind::Number)
            out = value.number;
        else if (value.kind == PredicateValue::Kind::Boolean)
            out = value.boolean ? 1 : 0;
        else
            return false;
        return true;
    }

    static std::string folded(const std::string& text, bool ignore_case) { return ignore_case ? lowercase(text) : text; }

    static bool equal(const PredicateValue& left, const PredicateValue& right, bool ignore_case) {
        double a, b;
        if (as_number(left, a) && as_number(right, b)) return a == b;
        if (left.kind != right.kind) return false;
        switch (left.kind) {
            case PredicateValue::Kind::Null:
                return true;
            case PredicateValue::Kind::String:
                return folded(left.text, ignore_case) == folded(right.text, ignore_case);
            case PredicateValue::Kind::Object:
                return left.object == right.object;
            case PredicateValue::Kind::List:
                return left.items.size() == right.items.size() && std::equal(left.items.begin(), left.items.end(), right.items.begin(), [&](const auto& x, const auto& y) { return equal(x, y, ignore_case); });
            case PredicateValue::Kind::Dictionary: {
                if (left.entries.size() != right.entries.size()) return false;
                for (const auto& [key, value] : left.entries) {
                    const auto found = right.entries.find(key);
                    if (found == right.entries.end() || !equal(value, found->second, ignore_case)) return false;
                }
                return true;
            }
            default:
                return false;
        }
    }

    // Negative, zero or positive, or false if the two have no order.
    static bool ordered(const PredicateValue& left, const PredicateValue& right, bool ignore_case, int& out) {
        double a, b;
        if (as_number(left, a) && as_number(right, b)) {
            out = (a > b) - (a < b);
            return true;
        }
        if (left.kind == PredicateValue::Kind::String && right.kind == PredicateValue::Kind::String) {
            out = folded(left.text, ignore_case).compare(folded(right.text, ignore_case));
            return true;
        }
        return false;
    }

    static bool list_contains(const PredicateValue& list, const PredicateValue& item, bool ignore_case) {
        return std::any_of(list.items.begin(), list.items.end(), [&](const PredicateValue& candidate) { return equal(candidate, item, ignore_case); });
    }

    static bool glob_matches(const char* text, const char* pattern) {
        if (*pattern == '\0') return *text == '\0';
        if (*pattern == '*') return glob_matches(text, pattern + 1) || (*text != '\0' && glob_matches(text + 1, pattern));
        return *text != '\0' && (*pattern == '?' || *pattern == *text) && glob_matches(text + 1, pattern + 1);
    }

    static bool compare(CompareOp op, const PredicateValue& left, const PredicateValue& right, bool ignore_case) {
        int order = 0;
        const bool both_strings = left.kind == PredicateValue::Kind::String && right.kind == PredicateValue::Kind::String;
        const std::string a = both_strings ? folded(left.text, ignore_case) : std::string();
        const std::string b = both_strings ? folded(right.text, ignore_case) : std::string();
        switch (op) {
            case CompareOp::Equal:
                return equal(left, right, ignore_case);
            case CompareOp::NotEqual:
                return !equal(left, right, ignore_case);
            case CompareOp::Less:
                return ordered(left, right, ignore_case, order) && order < 0;
            case CompareOp::LessEqual:
                return ordered(left, right, ignore_case, order) && order <= 0;
            case CompareOp::Greater:
                return ordered(left, right, ignore_case, order) && order > 0;
            case CompareOp::GreaterEqual:
                return ordered(left, right, ignore_case, order) && order >= 0;
            case CompareOp::In:
                if (right.kind == PredicateValue::Kind::List) return list_contains(right, left, ignore_case);
                return both_strings && b.find(a) != std::string::npos;
            case CompareOp::Contains:
                if (left.kind == PredicateValue::Kind::List) return list_contains(left, right, ignore_case);
                return both_strings && a.find(b) != std::string::npos;
            case CompareOp::BeginsWith:
                return both_strings && a.compare(0, b.size(), b) == 0;
            case CompareOp::EndsWith:
                return both_strings && a.size() >= b.size() && a.compare(a.size() - b.size(), b.size(), b) == 0;
            case CompareOp::Like:
                return both_strings && glob_matches(a.c_str(), b.c_str());
            case CompareOp::Matches:
                if (!both_strings) return false;
                try {
                    return std::regex_match(a, std::regex(b));
                } catch (const std::regex_error&) {
                    return false;
                }
            case CompareOp::Between: {
                if (right.kind != PredicateValue::Kind::List || right.items.size() != 2) return false;
                int low = 0, high = 0;
                return ordered(left, right.items[0], ignore_case, low) && ordered(left, right.items[1], ignore_case, high) && low >= 0 && high <= 0;
            }
        }
        return false;
    }

    bool holds(const PredicateRef& node, const PredicateValue& self, const Variables& variables) const {
        switch (node->kind) {
            case PredicateNode::Kind::Constant:
                return node->constant;
            case PredicateNode::Kind::Not:
                return !holds(node->operands[0], self, variables);
            case PredicateNode::Kind::And:
                return std::all_of(node->operands.begin(), node->operands.end(), [&](const PredicateRef& operand) { return holds(operand, self, variables); });
            case PredicateNode::Kind::Or:
                return std::any_of(node->operands.begin(), node->operands.end(), [&](const PredicateRef& operand) { return holds(operand, self, variables); });
            case PredicateNode::Kind::Compare:
                break;
        }
        const PredicateValue left = value_of(node->left, self, variables);
        const PredicateValue right = value_of(node->right, self, variables);
        if (node->quantifier == Quantifier::None) return compare(node->op, left, right, node->ignore_case);
        const auto passes = [&](const PredicateValue& item) { return compare(node->op, item, right, node->ignore_case); };
        if (node->quantifier == Quantifier::Any) return std::any_of(left.items.begin(), left.items.end(), passes);
        if (node->quantifier == Quantifier::All) return std::all_of(left.items.begin(), left.items.end(), passes);
        return std::none_of(left.items.begin(), left.items.end(), passes);
    }
};

PredicateRef compound(PredicateNode::Kind kind, std::vector<PredicateRef> operands) {
    auto node = std::make_shared<PredicateNode>();
    node->kind = kind;
    node->operands = std::move(operands);
    return node;
}

}  // namespace

PredicateRef predicate_parse(const std::string& format, const std::vector<PredicateValue>& arguments, std::string& error) {
    Parser parser(format, arguments);
    return parser.parse(error);
}

PredicateRef predicate_constant(bool value) {
    auto node = std::make_shared<PredicateNode>();
    node->constant = value;
    return node;
}

PredicateRef predicate_not(PredicateRef operand) { return compound(PredicateNode::Kind::Not, {std::move(operand)}); }
PredicateRef predicate_and(std::vector<PredicateRef> operands) { return compound(PredicateNode::Kind::And, std::move(operands)); }
PredicateRef predicate_or(std::vector<PredicateRef> operands) { return compound(PredicateNode::Kind::Or, std::move(operands)); }

bool predicate_evaluate(const PredicateRef& predicate, const PredicateValue& self, const PredicateKeyReader& read_key) {
    return Evaluator{read_key}.holds(predicate, self, {});
}
