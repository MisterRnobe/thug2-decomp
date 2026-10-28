#pragma once
#include "thug2/values.hpp"
#include "thug2/wait.hpp"
#include "thug2/random.hpp"
#include <functional>
#include <map>

namespace thug2::qb {
struct HostCall {
    std::uint32_t name, script;
    ValuePtr arguments, parameters;
    std::size_t depth;
};
struct HostResult { bool condition=true; ValuePtr outputs; std::optional<WaitRequest> wait; std::size_t work=0; };
using HostFunction = std::function<HostResult(const HostCall&)>;
struct RunLimits { std::size_t fuel=10000, calls=32, loops=32, code_tokens=100000, code_bytes=4194304, trace=256; };
enum class RunStatus { Finished, Fault, Limit, Ready, Waiting };
using TaskId = std::uint64_t;
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
    std::optional<WaitState> wait;
};

// Bounded QB subset. No hosts are registered automatically. Clocks are explicit.
class Interpreter {
public:
    explicit Interpreter(const SymbolTable& symbols, EvalLimits evaluation = {});
    ~Interpreter();
    Interpreter(const Interpreter&)=delete;
    Interpreter& operator=(const Interpreter&)=delete;
    void bind(std::uint32_t name, HostFunction function);
    void install_wait();
    void install_data_hosts(); // GetArraySize and StructureContains, explicitly opt-in.
    void seed_random(std::uint32_t seed);
    RunResult run(std::uint32_t script, ValuePtr parameters = {}, RunLimits limits = {});
    TaskId start(std::uint32_t script, ValuePtr parameters = {}, RunLimits limits = {});
    RunResult update(TaskId task, FrameTick tick);
    std::vector<std::pair<TaskId,RunResult>> update_all(FrameTick tick);
    RunResult inspect(TaskId task) const;
    bool cancel(TaskId task);
private:
    struct Function;
    struct Execution;
    void accept_tick(FrameTick tick);
    std::shared_ptr<const Function> compile(std::uint32_t name,std::size_t max_tokens,std::size_t max_bytes);
    ValueEvaluator values_;
    std::unordered_map<std::uint32_t,std::shared_ptr<const Function>> functions_;
    std::unordered_map<std::uint32_t,HostFunction> hosts_;
    bool running_=false;
    RandomSelector random_;
    std::map<TaskId,std::unique_ptr<Execution>> tasks_;
    TaskId next_task_=1;
    std::optional<FrameTick> tick_;
    bool one_per_frame_released_=false;
};
} // namespace thug2::qb
