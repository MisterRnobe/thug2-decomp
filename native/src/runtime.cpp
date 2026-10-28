// Control-flow/parameter conventions: script/cscript_exec.cpp, cscript_core.cpp
// and cscript_wait.cpp at 2d82ed2. Native ownership, host interface and fuel new.
#include "thug2/runtime.hpp"
#include <algorithm>
#include <bit>
#include <cmath>
#include <limits>
#include <sstream>

namespace thug2::qb {
namespace {
[[noreturn]] void fault(EvalFailure k,const std::string& why){throw EvalError(k,why);}
std::uint32_t word(std::span<const std::uint8_t> b,std::size_t p,unsigned n=4){
    if(p>b.size()||n>b.size()-p)fault(EvalFailure::Syntax,"Truncated runtime operand");
    std::uint32_t v=0;for(unsigned i=0;i<n;++i)v|=std::uint32_t(b[p+i])<<(8*i);return v;
}
ValuePtr merge(ValuePtr a,ValuePtr b){
    if(!b)return a;if(!a)return b;
    if(a->type!=ValueType::Structure||b->type!=ValueType::Structure)fault(EvalFailure::Type,"Parameters/results must be structures");
    return binary_value(11,std::move(a),std::move(b));
}
bool condition(ValuePtr v){
    if(v->type==ValueType::Integer||v->type==ValueType::Float||v->type==ValueType::Name)return v->bits!=0;
    return v->type!=ValueType::None; // Original ExecuteCommand tests the raw word, not GetBool.
}
}
std::string_view run_status_name(RunStatus s){
    switch(s){case RunStatus::Finished:return "finished";case RunStatus::Fault:return "fault";case RunStatus::Limit:return "limit";case RunStatus::Ready:return "ready";case RunStatus::Waiting:return "waiting";}return "unknown";
}
struct Interpreter::Function {
    std::uint32_t name;
    Document doc;
    std::size_t end;
    std::unordered_map<std::size_t,std::size_t> boundaries, repeats;
};
void Interpreter::bind(std::uint32_t name,HostFunction fn){
    if(running_)throw std::logic_error("Cannot change host bindings during execution");
    if(!fn)throw std::invalid_argument("Empty host function");hosts_[name]=std::move(fn);
}
std::shared_ptr<const Interpreter::Function> Interpreter::compile(std::uint32_t name,std::size_t max_tokens,std::size_t max_bytes){
    if(auto found=functions_.find(name);found!=functions_.end()){
        if(found->second->doc.tokens().size()>max_tokens)fault(EvalFailure::Limit,"Script token limit");
        if(found->second->end-6>max_bytes)fault(EvalFailure::Limit,"Script byte limit");
        return found->second;
    }
    auto value=values_.global(name);
    if(value->type!=ValueType::Script)fault(EvalFailure::Type,"Requested symbol is not a QB script");
    // Reject oversized code before allocating one metadata entry per token.
    if(value->code.size()>max_bytes)fault(EvalFailure::Limit,"Script byte limit");
    std::size_t tokens=3; // Synthetic script/name header and EOF.
    if(tokens>max_tokens)fault(EvalFailure::Limit,"Script token limit");
    for(std::size_t p=0;p<value->code.size();){
        auto t=read_token(value->code,p);p+=t.size;
        if(++tokens>max_tokens)fault(EvalFailure::Limit,"Script token limit");
    }
    Bytes bytes{0x23,0x16};for(unsigned i=0;i<4;++i)bytes.push_back(name>>(8*i));
    bytes.insert(bytes.end(),value->code.begin(),value->code.end());bytes.push_back(0);
    auto f=std::make_shared<Function>(Function{name,Document(std::move(bytes)),6+value->code.size(),{}, {}});
    if(f->doc.tokens().size()>max_tokens)fault(EvalFailure::Limit,"Script token limit");
    for(const auto& j:f->doc.jumps())if(j.target<6||j.target>=f->end)fault(EvalFailure::Syntax,"Jump escapes function body");
    std::vector<std::size_t> loops;
    for(std::size_t i=0;i<f->doc.tokens().size();++i){
        const auto& t=f->doc.tokens()[i];f->boundaries.emplace(t.offset,i);
        if(t.offset<6)continue;
        if(t.opcode==0x20)loops.push_back(t.offset);
        if(t.opcode==0x21){if(loops.empty())fault(EvalFailure::Syntax,"repeat without begin");f->repeats[loops.back()]=t.offset;loops.pop_back();}
    }
    if(!loops.empty())fault(EvalFailure::Syntax,"begin without repeat");
    functions_[name]=f;return f;
}
struct Interpreter::Execution {
    struct Loop {std::size_t start,repeat,end=0;bool initialized=false,counted=false;std::uint32_t remaining=0;};
    struct Frame {std::shared_ptr<const Function> function;std::size_t pc;ValuePtr params;std::vector<Loop> loops;WaitState wait;};
    Interpreter& vm;RunLimits limits;std::vector<Frame> stack;
    RunResult result{RunStatus::Finished,structure_value({}),0,{},false,{}, {},{}};
    bool persistent=false;
    TaskId owner=0;
    std::optional<std::uint64_t> last_frame;
    FrameTick tick{};
    explicit Execution(Interpreter& interpreter,RunLimits bounds,TaskId id):vm(interpreter),limits(bounds),owner(id){}
    ~Execution(){vm.random_.forget(owner);}
    void charge(std::size_t n=1){
        if(n>limits.fuel-result.work)fault(EvalFailure::Limit,"Execution fuel exhausted");result.work+=n;
    }
    std::size_t resolve(const Function& f,std::size_t pc){
        for(;;){
            auto found=f.boundaries.find(pc);
            if(found==f.boundaries.end()||pc<6||pc>=f.end)fault(EvalFailure::Syntax,"Value PC outside function or not a token boundary");
            auto t=f.doc.tokens()[found->second];
            if(t.opcode==0x49){charge();pc+=1+word(f.doc.bytes(),pc+1,2);continue;}
            if(t.opcode==0x2e){charge();auto delta=std::bit_cast<std::int32_t>(word(f.doc.bytes(),pc+1));pc=static_cast<std::size_t>(std::int64_t(pc+5)+delta);continue;}
            if(t.opcode!=0x2f&&t.opcode!=0x37&&t.opcode!=0x40&&t.opcode!=0x41)return pc;
            charge();auto count=word(f.doc.bytes(),pc+1);
            if(!count||count>65535)fault(EvalFailure::Syntax,"Random choice count must be 1..65535");
            std::vector<std::uint16_t> weights;weights.reserve(count);
            for(std::size_t i=0;i<count;++i){charge();weights.push_back(static_cast<std::uint16_t>(word(f.doc.bytes(),pc+5+i*2,2)));}
            auto choice=vm.random_.choose(t.opcode,f.name,pc,weights,owner,[&](std::size_t n){charge(n);});
            auto field=pc+5+count*2+choice*4;auto delta=std::bit_cast<std::int32_t>(word(f.doc.bytes(),field));
            pc=static_cast<std::size_t>(std::int64_t(field+4)+delta);
        }
    }
    ValueFlow flow(const Function& f){return [this,&f](std::size_t p){return resolve(f,p);};}
    ParsedValue args(const Function& f,std::size_t p,ValuePtr params){
        auto parsed=vm.values_.runtime_arguments(f.doc,p,f.end,std::move(params),flow(f));charge(parsed.work);return parsed;
    }
    ValuePtr joined(ValuePtr a,ValuePtr b){
        if(a&&b&&(a->members.size()>250000||b->members.size()>250000-a->members.size()))fault(EvalFailure::Limit,"Parameter count limit");
        return merge(std::move(a),std::move(b));
    }
    void push(std::uint32_t name,ValuePtr params){
        if(stack.size()>=limits.calls)fault(EvalFailure::Limit,"Call depth limit");
        bool cached=vm.functions_.count(name)!=0;
        auto f=vm.compile(name,limits.code_tokens,limits.code_bytes);charge(cached?1:vm.values_.last_work());
        auto defaults=args(*f,6,{});
        stack.push_back({std::move(f),defaults.next,joined(defaults.value,std::move(params)),{}, {}});
    }
    std::uint32_t read_name(Frame& frame,std::size_t end){
        auto b=frame.function->doc.bytes();auto t=read_token(b,frame.pc);
        if(t.opcode==0x16){auto n=word(b,frame.pc+1);frame.pc+=5;return n;}
        if(t.opcode==0x2d||t.opcode==0x2c){
            auto v=vm.values_.read(frame.function->doc,frame.pc,end,frame.params,flow(*frame.function));charge(v.work);frame.pc=v.next;
            ValuePtr value=v.value;
            if(t.opcode==0x2c){value=vm.values_.member(frame.params,0);charge(vm.values_.last_work());}
            if(!value||value->type!=ValueType::Name)fault(EvalFailure::Type,"Dynamic command name is not a checksum");
            return value->bits;
        }
        fault(EvalFailure::Unsupported,"Unsupported command-name token");
    }
    void command(std::optional<std::size_t> false_target={},bool negate=false){
        auto index=stack.size()-1;auto& frame=stack[index];auto f=frame.function;
        frame.pc=resolve(*f,frame.pc);auto end=f->end;auto b=f->doc.bytes();
        auto branch=[&](bool value){if(false_target&&(value==negate))stack[index].pc=*false_target;};
        std::uint32_t name;
        if(b[frame.pc]==0x0e){
            auto v=vm.values_.read(f->doc,frame.pc,end,frame.params,flow(*f));charge(v.work);frame.pc=v.next;
            if(v.value->type!=ValueType::Name){branch(condition(v.value));return;}
            name=v.value->bits;if(!name)name=read_name(frame,end);
        }else name=read_name(frame,end);
        if(frame.pc<end&&b[frame.pc]==7){
            ++frame.pc;auto value=vm.values_.read(f->doc,frame.pc,end,frame.params,flow(*f));charge(value.work);
            frame.pc=resolve(*f,value.next);
            auto next=b[frame.pc];if(next!=1&&next!=2&&next!=0x24&&next!=0)fault(EvalFailure::Unsupported,"Extra tokens after assignment");
            if(value.value->type!=ValueType::None)frame.params=joined(frame.params,structure_value({{name,value.value}}));
            branch(true);return;
        }
        auto parsed=args(*f,frame.pc,frame.params);auto arguments=parsed.value;frame.pc=parsed.next;
        if(auto host=vm.hosts_.find(name);host!=vm.hosts_.end()){
            auto function=host->second;auto response=function({name,f->name,arguments,frame.params,stack.size()});
            charge(response.work);
            if(response.wait){
                if(!persistent)fault(EvalFailure::Unsupported,"Wait requires start/update execution");
                frame.wait=make_wait(*response.wait,tick);
            }
            if(response.outputs)frame.params=joined(frame.params,response.outputs);
            branch(response.condition);return;
        }
        ValuePtr target;
        try{target=vm.values_.global(name);charge(vm.values_.last_work());}
        catch(const EvalError& e){if(e.kind==EvalFailure::Missing)fault(EvalFailure::Missing,"Unbound command 0x"+hex(name));throw;}
        if(target->type!=ValueType::Script)fault(EvalFailure::Unsupported,"Command has no host binding or QB implementation");
        if(false_target&&negate)fault(EvalFailure::Unsupported,"Negated QB-call condition has unsupported legacy control behavior");
        branch(true);push(name,std::move(arguments));
    }
    static std::string hex(std::uint32_t n){std::ostringstream s;s<<std::hex<<n;return s.str();}
    void finish(ValuePtr returned={}){
        auto params=stack.back().params;
        if(stack.size()==1){result.parameters=joined(params,returned);stack.pop_back();}
        else{stack.pop_back();if(returned)stack.back().params=joined(stack.back().params,returned);}
    }
    const Token& checked_token(const Function& f,std::size_t pc){
        auto found=f.boundaries.find(pc);
        if(found==f.boundaries.end()||pc<6||pc>=f.end)fault(EvalFailure::Syntax,"Switch PC outside function or not a token boundary");
        charge();return f.doc.tokens()[found->second];
    }
    void skip_lines(const Function& f,std::size_t& pc){
        for(;;){const auto& t=checked_token(f,pc);if(t.opcode!=1&&t.opcode!=2)return;pc+=t.size;}
    }
    void skip_switch(const Function& f,std::size_t& pc){
        // Iterative: even deeply nested skipped switches cannot exhaust C++'s stack.
        std::size_t depth=1;
        while(depth){
            const auto& t=checked_token(f,pc);
            if(t.opcode==0x24||t.opcode==0)fault(EvalFailure::Syntax,"Switch has no endswitch");
            if(t.opcode==0x3c)++depth;
            if(t.opcode==0x3d)--depth;
            pc+=t.size;
        }
    }
    void switch_statement(){
        auto& frame=stack.back();const auto& f=*frame.function;++frame.pc;
        auto parameters=[&](){
            auto result=args(f,frame.pc,frame.params);frame.pc=result.next;
            return result.value->members.empty()?ValuePtr{}:result.value->members.front().value;
        };
        auto value=parameters();
        for(;;){
            const auto& t=checked_token(f,frame.pc);
            if(t.opcode==0x24||t.opcode==0)fault(EvalFailure::Syntax,"Switch has no endswitch");
            if(t.opcode==0x3c){++frame.pc;skip_switch(f,frame.pc);continue;}
            if(t.opcode==0x3d){++frame.pc;return;}
            if(t.opcode==0x3f){
                if(checked_token(f,frame.pc+1).opcode!=0x49)fault(EvalFailure::Syntax,"Default lacks compiled jump");
                frame.pc+=4;return;
            }
            if(t.opcode!=0x3e){frame.pc+=t.size;continue;}
            if(checked_token(f,frame.pc+1).opcode!=0x49)fault(EvalFailure::Syntax,"Case lacks compiled jump");
            auto next=frame.pc+2+word(f.doc.bytes(),frame.pc+2,2);frame.pc+=4;
            auto match=parameters();bool equal=false;
            if(value&&match){
                auto a=vm.values_.resolved(value);charge(vm.values_.last_work());
                auto b=vm.values_.resolved(match);charge(vm.values_.last_work());
                // The shared equality covers scalar, string and coordinate values.
                // Mutable engine identity (arrays/structures/scripts) remains explicit.
                if(a->type==ValueType::String||a->type==ValueType::LocalString)charge(a->text.size());
                if(b->type==ValueType::String||b->type==ValueType::LocalString)charge(b->text.size());
                equal=binary_value(7,std::move(a),std::move(b))->bits!=0;
            }
            if(!equal){frame.pc=next;continue;}
            skip_lines(f,frame.pc);
            while(checked_token(f,frame.pc).opcode==0x49){
                const auto& following=checked_token(f,frame.pc+3);
                if(following.opcode!=0x3e)break;
                if(checked_token(f,frame.pc+4).opcode!=0x49)fault(EvalFailure::Syntax,"Grouped case lacks compiled jump");
                frame.pc+=7;parameters();skip_lines(f,frame.pc);
            }
            return;
        }
    }
    void update(){
        auto& frame=stack.back();auto f=frame.function;
        auto found=f->boundaries.find(frame.pc);
        if(found==f->boundaries.end()||frame.pc<6||frame.pc>=f->end)fault(EvalFailure::Syntax,"PC outside function body");
        auto t=f->doc.tokens()[found->second];charge();
        if(result.trace.size()<limits.trace)result.trace.push_back({f->name,t.offset-6,stack.size(),t.opcode});else result.trace_truncated=true;
        switch(t.opcode){
        case 1:case 2:case 0x28:case 0x3d:frame.pc+=t.size;return;
        case 0x3c:switch_statement();return;
        case 0x2f:case 0x37:case 0x40:case 0x41:frame.pc=resolve(*f,frame.pc);return;
        case 0x3e:case 0x3f:fault(EvalFailure::Syntax,"Case/default reached outside switch dispatch");
        case 0x24:finish();return;
        case 0x29:{auto returned=args(*f,frame.pc+1,frame.params);finish(std::move(returned.value));return;}
        case 0x47:{auto target=frame.pc+1+word(f->doc.bytes(),frame.pc+1,2);frame.pc+=3;
            bool negate=frame.pc<f->end&&f->doc.bytes()[frame.pc]==0x39;if(negate)++frame.pc;command(target,negate);return;}
        case 0x48:case 0x49:frame.pc+=1+word(f->doc.bytes(),frame.pc+1,2);return;
        case 0x2e:{auto delta=std::bit_cast<std::int32_t>(word(f->doc.bytes(),frame.pc+1));frame.pc=static_cast<std::size_t>(std::int64_t(frame.pc+5)+delta);return;}
        case 0x20:{
            if(frame.loops.size()>=limits.loops)fault(EvalFailure::Limit,"Loop depth limit");
            auto repeat=f->repeats.find(frame.pc);if(repeat==f->repeats.end())fault(EvalFailure::Syntax,"Missing repeat");
            frame.loops.push_back({frame.pc+1,repeat->second});++frame.pc;return;}
        case 0x21:{
            if(frame.loops.empty()||frame.loops.back().repeat!=frame.pc)fault(EvalFailure::Syntax,"repeat without active loop");
            auto& loop=frame.loops.back();
            if(!loop.initialized){
                auto params=args(*f,frame.pc+1,frame.params);loop.end=params.next;
                auto count=vm.values_.member(params.value,0);charge(vm.values_.last_work());
                if(count){count=vm.values_.resolved(count);charge(vm.values_.last_work());}
                if(count&&count->type==ValueType::Integer){loop.counted=true;loop.remaining=count->bits;}
                loop.initialized=true;
            }
            if(!loop.counted||--loop.remaining!=0)frame.pc=loop.start;
            else{frame.pc=loop.end;frame.loops.pop_back();}return;}
        case 0x22:{if(frame.loops.empty())fault(EvalFailure::Syntax,"break outside loop");
            frame.pc=args(*f,frame.loops.back().repeat+1,{}).next;frame.loops.pop_back();return;}
        case 0x16:case 0x2c:case 0x2d:case 0x0e:command();return;
        default:fault(EvalFailure::Unsupported,"Unsupported runtime token "+std::to_string(t.opcode));
        }
    }
};
Interpreter::Interpreter(const SymbolTable& symbols,EvalLimits evaluation):values_(symbols,evaluation){}
Interpreter::~Interpreter()=default;
namespace {
struct RunningGuard {
    bool& running;
    explicit RunningGuard(bool& flag):running(flag){
        if(flag)throw std::logic_error("Reentrant execution is unsupported");running=true;
    }
    ~RunningGuard(){running=false;}
};
}
RunResult Interpreter::run(std::uint32_t script,ValuePtr parameters,RunLimits limits){
    if(running_)return {RunStatus::Fault,structure_value({}),0,{},false,EvalFailure::Unsupported,"Reentrant execution is unsupported",{}};
    RunningGuard guard(running_);
    if(next_task_==0)throw std::length_error("Task IDs exhausted");
    Execution execution(*this,limits,next_task_++);
    try{
        if(parameters&&parameters->type!=ValueType::Structure)fault(EvalFailure::Type,"Entry parameters must be a structure");
        execution.push(script,std::move(parameters));
        while(!execution.stack.empty())execution.update();
    }catch(const EvalError& e){execution.result.status=e.kind==EvalFailure::Limit?RunStatus::Limit:RunStatus::Fault;
        execution.result.failure=e.kind;execution.result.message=e.what();}
    catch(const std::exception& e){execution.result.status=RunStatus::Fault;execution.result.message=e.what();}
    if(!execution.stack.empty())execution.result.parameters=execution.stack.front().params;
    return std::move(execution.result);
}
TaskId Interpreter::start(std::uint32_t script,ValuePtr parameters,RunLimits limits){
    RunningGuard guard(running_);
    if(tasks_.size()>=256||next_task_==0)throw std::length_error("Task capacity exhausted (cancel completed tasks)");
    auto id=next_task_++;
    auto execution=std::make_unique<Execution>(*this,limits,id);execution->persistent=true;
    execution->result.status=RunStatus::Ready;
    try{
        if(parameters&&parameters->type!=ValueType::Structure)fault(EvalFailure::Type,"Entry parameters must be a structure");
        execution->push(script,std::move(parameters));
        execution->result.parameters=execution->stack.front().params;
    }catch(const EvalError& e){execution->result.status=e.kind==EvalFailure::Limit?RunStatus::Limit:RunStatus::Fault;
        execution->result.failure=e.kind;execution->result.message=e.what();}
    catch(const std::exception& e){execution->result.status=RunStatus::Fault;execution->result.message=e.what();}
    tasks_.emplace(id,std::move(execution));return id;
}
void Interpreter::accept_tick(FrameTick tick){
    if(!std::isfinite(tick.seconds)||tick.seconds<0)throw std::invalid_argument("Invalid frame duration");
    if(tick_){
        if(tick.frame<tick_->frame)throw std::invalid_argument("Frame number went backwards");
        if(tick.frame==tick_->frame){
            if(tick.seconds!=tick_->seconds||tick.real_ms!=tick_->real_ms)throw std::invalid_argument("Conflicting clocks for the same frame");
            return;
        }
    }
    tick_=tick;one_per_frame_released_=false;
}
RunResult Interpreter::update(TaskId task,FrameTick tick){
    RunningGuard guard(running_);
    auto& execution=*tasks_.at(task);accept_tick(tick);
    auto& r=execution.result;
    if(execution.last_frame==tick.frame|| (r.status!=RunStatus::Ready&&r.status!=RunStatus::Waiting))return r;
    execution.last_frame=tick.frame;execution.tick=tick;r.wait.reset();r.status=RunStatus::Finished;
    try{
        while(!execution.stack.empty()){
            auto& wait=execution.stack.back().wait;
            if(wait.kind!=WaitKind::None){
                execution.charge();
                if(advance_wait(wait,tick,one_per_frame_released_)){r.status=RunStatus::Waiting;r.wait=wait;break;}
            }
            execution.update();
        }
    }catch(const EvalError& e){r.status=e.kind==EvalFailure::Limit?RunStatus::Limit:RunStatus::Fault;r.failure=e.kind;r.message=e.what();}
    catch(const std::exception& e){r.status=RunStatus::Fault;r.message=e.what();}
    if(!execution.stack.empty())r.parameters=execution.stack.front().params;
    return r;
}
std::vector<std::pair<TaskId,RunResult>> Interpreter::update_all(FrameTick tick){
    if(running_)throw std::logic_error("Reentrant execution is unsupported");
    accept_tick(tick);
    std::vector<std::pair<TaskId,RunResult>> results;results.reserve(tasks_.size());
    for(const auto& [id,execution]:tasks_){(void)execution;results.emplace_back(id,update(id,tick));}
    return results;
}
RunResult Interpreter::inspect(TaskId task) const{return tasks_.at(task)->result;}
bool Interpreter::cancel(TaskId task){
    if(running_)throw std::logic_error("Cannot cancel during execution");return tasks_.erase(task)!=0;
}
void Interpreter::seed_random(std::uint32_t seed){
    if(running_)throw std::logic_error("Cannot seed during execution");random_.seed(seed);
}
void Interpreter::install_wait(){
    bind(name_crc("Wait"),[this](const HostCall& call){
        std::size_t work=0;
        float value=0;
        // Search(0, FLOAT) keeps the last numeric match; later flag names do
        // not overwrite a numeric argument. Name-0 structures are not spread.
        for(const auto& member:call.arguments->members){
            if(++work>250000)fault(EvalFailure::Limit,"Wait argument search limit");
            if(member.name)continue;
            auto number=member.value;
            if(number->type==ValueType::Name){
                try{number=values_.global(number->bits);work+=values_.last_work();}
                catch(const EvalError& e){if(e.kind!=EvalFailure::Missing)throw;continue;}
            }
            if(number->type==ValueType::Integer)value=static_cast<float>(std::bit_cast<std::int32_t>(number->bits));
            if(number->type==ValueType::Float)value=std::bit_cast<float>(number->bits);
        }
        std::function<bool(ValuePtr,std::uint32_t,std::size_t)> contains;
        contains=[&](ValuePtr structure,std::uint32_t flag,std::size_t depth){
            if(depth>128)fault(EvalFailure::Limit,"Wait flag nesting limit");
            for(const auto& m:structure->members){
                if(++work>250000)fault(EvalFailure::Limit,"Wait flag search limit");
                if(m.name||m.value->type!=ValueType::Name)continue;
                if(m.value->bits==flag)return true;
                ValuePtr included;
                try{included=values_.global(m.value->bits);work+=values_.last_work();}
                catch(const EvalError& e){if(e.kind!=EvalFailure::Missing)throw;}
                if(included&&included->type==ValueType::Structure&&contains(included,flag,depth+1))return true;
            }
            return false;
        };
        auto flag=[&](std::uint32_t crc){return contains(call.arguments,crc,0);};
        auto kind=flag(0xc847f2b9)?WaitKind::RealTimer:WaitKind::Timer;
        double amount=value;
        if(flag(0xd029f619)||flag(0x49e0ee96))amount=double(value)*1000.0;
        else if(flag(0xfc37df05)||flag(0xb99ae3d6)||flag(0xdcd4ce73))kind=WaitKind::Frames;
        else if(flag(0x4a07c332)||flag(0x019176c5))amount=double(value)*double(16.666666f);
        else if(flag(0xecaa3345))kind=WaitKind::OnePerFrame;
        // Match ftol's truncation/low 32 bits for finite representable inputs.
        if(!std::isfinite(amount)||amount<-0x1p63||amount>=0x1p63)fault(EvalFailure::Arithmetic,"Wait duration outside int64");
        auto count=static_cast<std::uint32_t>(static_cast<std::int64_t>(amount));
        return HostResult{true,{},WaitRequest{kind,count},work};
    });
}
} // namespace thug2::qb
