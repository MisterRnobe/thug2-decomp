#pragma once
#include "thug2/symbols.hpp"
#include <array>
#include <functional>
#include <stdexcept>
#include <unordered_set>

namespace thug2::qb {
struct Value;
using ValuePtr = std::shared_ptr<const Value>;
struct Member { std::uint32_t name; ValuePtr value; };
struct Value {
    ValueType type = ValueType::None;
    std::uint32_t bits = 0; // Meaningful for scalar/opaque types only.
    std::string text;
    std::array<std::uint32_t, 3> coordinates{};
    ValueType element_type = ValueType::None;
    std::vector<ValuePtr> elements;
    std::vector<Member> members;
    Bytes code;
};
enum class EvalFailure { Unsupported, Missing, Cycle, Limit, Type, Arithmetic, Syntax };
std::string_view eval_failure_name(EvalFailure failure);
class EvalError : public std::runtime_error {
public:
    EvalError(EvalFailure kind, const std::string& message) : std::runtime_error(message), kind(kind) {}
    EvalFailure kind;
};
ValuePtr integer_value(std::int32_t n);
ValuePtr float_value(float n);
ValuePtr name_value(std::uint32_t name);
ValuePtr string_value(std::string text, bool local = false);
ValuePtr array_value(std::vector<ValuePtr> elements);
ValuePtr structure_value(std::vector<Member> members);
// Pure operations, without global name resolution; operator numbers are QB tokens.
ValuePtr binary_value(std::uint8_t op, ValuePtr a, ValuePtr b);
int operator_precedence(std::uint8_t op);
std::string format_value(ValuePtr value, std::size_t max_items = 100);

struct EvalLimits { std::size_t depth=128, nodes=250000, steps=1000000; };
struct ParsedValue { ValuePtr value; std::size_t next, work; };
// Runtime-only transfer resolver. Static global materialization never uses it.
// Arrays parsed with it perform separate sizing and population passes.
using ValueFlow = std::function<std::size_t(std::size_t)>;
// Optional runtime numeric provider; static materialization never inherits it.
using ValueRange = std::function<ValuePtr(float,float,bool)>;
// A fixed snapshot of definitions. Reconstruct after changing the symbol table.
// Lazy materialization uses the final snapshot, not original load-time evaluation.
class ValueEvaluator {
public:
    explicit ValueEvaluator(const SymbolTable& table, EvalLimits limits = {});
    ValuePtr global(std::uint32_t name);
    ValuePtr member(ValuePtr structure, std::uint32_t name);
    ParsedValue read(const Document& doc, std::size_t begin, std::size_t end, ValuePtr params = {}, ValueFlow flow = {}, ValueRange range = {});
    ParsedValue arguments(const Document& doc, std::size_t begin, std::size_t end, ValuePtr params = {});
    // Unlike static arguments(), stop at a top-level EOL/endscript/EOF/close-paren.
    ParsedValue runtime_arguments(const Document& doc, std::size_t begin, std::size_t end,
                                  ValuePtr params = {}, ValueFlow flow = {}, ValueRange range = {});
    ValuePtr resolved(ValuePtr value);
    std::size_t last_work() const { return steps_; }
private:
    class Parser;
    struct Frame;
    void step();
    ValuePtr track(ValuePtr value);
    ValuePtr get(std::uint32_t name);
    ValuePtr image(const PrecompiledImage& image, std::size_t index);
    ValuePtr resolve_name(ValuePtr value);
    ValuePtr apply(std::uint8_t op, ValuePtr a, ValuePtr b);
    ValuePtr find_member(ValuePtr value, std::uint32_t name);
    std::unordered_map<std::uint32_t, std::shared_ptr<const Definition>> definitions_;
    std::unordered_map<std::uint32_t, ValuePtr> cache_;
    std::unordered_set<std::uint32_t> active_;
    EvalLimits limits_;
    std::size_t depth_=0, nodes_=0, steps_=0;
};
} // namespace thug2::qb
