#pragma once
#include "thug2/values.hpp"
#include <functional>

namespace thug2::qb {
struct HostCall {
    std::uint32_t name, script;
    ValuePtr arguments, parameters;
    std::size_t depth;
};
struct HostResult { bool condition=true; ValuePtr outputs; };
using HostFunction = std::function<HostResult(const HostCall&)>;
struct RunLimits { std::size_t fuel=10000, calls=32, loops=32, code_tokens=100000, code_bytes=4194304, trace=256; };
enum class RunStatus { Finished, Fault, Limit };
std::string_view run_status_name(RunStatus status);
struct TraceEntry { std::uint32_t script; std::size_t offset, depth; std::uint8_t token; };
struct RunResult {
    RunStatus status;
    ValuePtr parameters;
    std::size_t work;
    std::vector<TraceEntry> trace;
    bool trace_truncated=false;
    std::optional<EvalFailure> failure;
    std::string message;
};

// Synchronous, bounded QB subset. No hosts are registered automatically.
// The definition snapshot is fixed; each run owns its stack and parameters.
class Interpreter {
public:
    explicit Interpreter(const SymbolTable& symbols, EvalLimits evaluation = {});
    void bind(std::uint32_t name, HostFunction function);
    RunResult run(std::uint32_t script, ValuePtr parameters = {}, RunLimits limits = {});
private:
    struct Function;
    struct Execution;
    std::shared_ptr<const Function> compile(std::uint32_t name,std::size_t max_tokens,std::size_t max_bytes);
    ValueEvaluator values_;
    std::unordered_map<std::uint32_t,std::shared_ptr<const Function>> functions_;
    std::unordered_map<std::uint32_t,HostFunction> hosts_;
    bool running_=false;
};
} // namespace thug2::qb
