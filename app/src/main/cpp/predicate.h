#pragma once
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

// A value an NSPredicate compares: what a key path or format argument holds.
struct PredicateValue {
    enum class Kind { Null,
                      Boolean,
                      Number,
                      String,
                      Object,
                      List,
                      Dictionary };
    Kind kind = Kind::Null;
    bool boolean = false;
    double number = 0;
    std::string text;
    void* object = nullptr;  // Compared by identity.
    std::vector<PredicateValue> items;
    std::map<std::string, PredicateValue> entries;

    static PredicateValue of_bool(bool value);
    static PredicateValue of_number(double value);
    static PredicateValue of_string(std::string value);
    static PredicateValue of_object(void* value);
};

struct PredicateNode;
using PredicateRef = std::shared_ptr<const PredicateNode>;

// Reads a key of an Object value, which only the caller knows how to do.
using PredicateKeyReader = std::function<PredicateValue(void* object, const std::string& key)>;

// Parses NSPredicate's format syntax, consuming `arguments` for %K, %@ and the numeric specifiers.
// Returns null and fills `error` if the format is malformed or uses syntax this doesn't implement.
PredicateRef predicate_parse(const std::string& format, const std::vector<PredicateValue>& arguments, std::string& error);

PredicateRef predicate_constant(bool value);
PredicateRef predicate_not(PredicateRef operand);
PredicateRef predicate_and(std::vector<PredicateRef> operands);
PredicateRef predicate_or(std::vector<PredicateRef> operands);

bool predicate_evaluate(const PredicateRef& predicate, const PredicateValue& self, const PredicateKeyReader& read_key);
