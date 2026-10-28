// Control-flow/parameter conventions: script/cscript_exec.cpp, cscript_core.cpp
// and cscript_wait.cpp at 2d82ed2. Native ownership, host interface and fuel new.
#include "thug2/runtime.hpp"
#include <algorithm>
#include <bit>
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
    switch(s){case RunStatus::Finished:return "finished";case RunStatus::Fault:return "fault";case RunStatus::Limit:return "limit";}return "unknown";
}
struct Interpreter::Function {
    std::uint32_t name;
    Document doc;
    std::size_t end;
    std::unordered_map<std::size_t,std::size_t> boundaries, repeats;
};
Interpreter::Interpreter(const SymbolTable& symbols,EvalLimits evaluation):values_(symbols,evaluation){}
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
    struct Frame {std::shared_ptr<const Function> function;std::size_t pc;ValuePtr params;std::vector<Loop> loops;};
    Interpreter& vm;RunLimits limits;std::vector<Frame> stack;
    RunResult result{RunStatus::Finished,structure_value({}),0,{},false,{}, {}};
    void charge(std::size_t n=1){
        if(n>limits.fuel-result.work)fault(EvalFailure::Limit,"Execution fuel exhausted");result.work+=n;
    }
    std::size_t statement_end(const Function& f,std::size_t p){
        auto found=f.boundaries.find(p);if(found==f.boundaries.end())fault(EvalFailure::Syntax,"PC is not a token boundary");
        std::size_t depth=0;
        for(std::size_t i=found->second;i<f.doc.tokens().size();++i){
            const auto& t=f.doc.tokens()[i];charge();
            if(!depth&&(t.opcode==1||t.opcode==2||t.opcode==0x24||t.opcode==0))return t.offset;
            if(t.opcode==3||t.opcode==5||t.opcode==0x0e)++depth;
            if(t.opcode==4||t.opcode==6||t.opcode==0x0f){if(!depth)fault(EvalFailure::Syntax,"Unmatched statement delimiter");--depth;}
        }
        fault(EvalFailure::Syntax,"Unterminated statement");
    }
    ValuePtr args(const Function& f,std::size_t p,std::size_t end,ValuePtr params){
        auto parsed=vm.values_.arguments(f.doc,p,end,std::move(params));charge(parsed.work);return parsed.value;
    }
    ValuePtr joined(ValuePtr a,ValuePtr b){
        if(a&&b&&(a->members.size()>250000||b->members.size()>250000-a->members.size()))fault(EvalFailure::Limit,"Parameter count limit");
        return merge(std::move(a),std::move(b));
    }
    void push(std::uint32_t name,ValuePtr params){
        if(stack.size()>=limits.calls)fault(EvalFailure::Limit,"Call depth limit");
        bool cached=vm.functions_.count(name)!=0;
        auto f=vm.compile(name,limits.code_tokens,limits.code_bytes);charge(cached?1:vm.values_.last_work());
        auto end=statement_end(*f,6);auto defaults=args(*f,6,end,{});
        stack.push_back({std::move(f),end,joined(defaults,std::move(params)),{}});
    }
    std::uint32_t read_name(Frame& frame,std::size_t end){
        auto b=frame.function->doc.bytes();auto t=read_token(b,frame.pc);
        if(t.opcode==0x16){auto n=word(b,frame.pc+1);frame.pc+=5;return n;}
        if(t.opcode==0x2d||t.opcode==0x2c){
            auto v=vm.values_.read(frame.function->doc,frame.pc,end,frame.params);charge(v.work);frame.pc=v.next;
            ValuePtr value=v.value;
            if(t.opcode==0x2c){value=vm.values_.member(frame.params,0);charge(vm.values_.last_work());}
            if(!value||value->type!=ValueType::Name)fault(EvalFailure::Type,"Dynamic command name is not a checksum");
            return value->bits;
        }
        fault(EvalFailure::Unsupported,"Unsupported command-name token");
    }
    void command(std::optional<std::size_t> false_target={},bool negate=false){
        auto index=stack.size()-1;auto& frame=stack[index];auto f=frame.function;
        auto end=statement_end(*f,frame.pc);auto b=f->doc.bytes();
        auto branch=[&](bool value){if(false_target&&(value==negate))stack[index].pc=*false_target;};
        std::uint32_t name;
        if(b[frame.pc]==0x0e){
            auto v=vm.values_.read(f->doc,frame.pc,end,frame.params);charge(v.work);frame.pc=v.next;
            if(v.value->type!=ValueType::Name){branch(condition(v.value));return;}
            name=v.value->bits;if(!name)name=read_name(frame,end);
        }else name=read_name(frame,end);
        if(frame.pc<end&&b[frame.pc]==7){
            ++frame.pc;auto value=vm.values_.read(f->doc,frame.pc,end,frame.params);charge(value.work);
            if(value.next!=end)fault(EvalFailure::Unsupported,"Extra tokens after assignment");
            frame.pc=end;
            if(value.value->type!=ValueType::None)frame.params=joined(frame.params,structure_value({{name,value.value}}));
            branch(true);return;
        }
        auto arguments=args(*f,frame.pc,end,frame.params);frame.pc=end;
        if(auto host=vm.hosts_.find(name);host!=vm.hosts_.end()){
            auto function=host->second;auto response=function({name,f->name,arguments,frame.params,stack.size()});
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
    void update(){
        auto& frame=stack.back();auto f=frame.function;
        auto found=f->boundaries.find(frame.pc);
        if(found==f->boundaries.end()||frame.pc<6||frame.pc>=f->end)fault(EvalFailure::Syntax,"PC outside function body");
        auto t=f->doc.tokens()[found->second];charge();
        if(result.trace.size()<limits.trace)result.trace.push_back({f->name,t.offset-6,stack.size(),t.opcode});else result.trace_truncated=true;
        switch(t.opcode){
        case 1:case 2:case 0x28:frame.pc+=t.size;return;
        case 0x24:finish();return;
        case 0x29:{auto end=statement_end(*f,frame.pc+1);auto returned=args(*f,frame.pc+1,end,frame.params);finish(std::move(returned));return;}
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
                loop.end=statement_end(*f,frame.pc+1);auto params=args(*f,frame.pc+1,loop.end,frame.params);
                auto count=vm.values_.member(params,0);charge(vm.values_.last_work());
                if(count){count=vm.values_.resolved(count);charge(vm.values_.last_work());}
                if(count&&count->type==ValueType::Integer){loop.counted=true;loop.remaining=count->bits;}
                loop.initialized=true;
            }
            if(!loop.counted||--loop.remaining!=0)frame.pc=loop.start;
            else{frame.pc=loop.end;frame.loops.pop_back();}return;}
        case 0x22:{if(frame.loops.empty())fault(EvalFailure::Syntax,"break outside loop");
            frame.pc=statement_end(*f,frame.loops.back().repeat+1);frame.loops.pop_back();return;}
        case 0x16:case 0x2c:case 0x2d:case 0x0e:command();return;
        default:fault(EvalFailure::Unsupported,"Unsupported runtime token "+std::to_string(t.opcode));
        }
    }
};
RunResult Interpreter::run(std::uint32_t script,ValuePtr parameters,RunLimits limits){
    if(running_)return {RunStatus::Fault,{},0,{},false,EvalFailure::Unsupported,"Reentrant execution is unsupported"};
    running_=true;struct Reset{bool& running;~Reset(){running=false;}} reset{running_};
    Execution execution{*this,limits,{}, {RunStatus::Finished,structure_value({}),0,{},false,{}, {}}};
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
} // namespace thug2::qb
