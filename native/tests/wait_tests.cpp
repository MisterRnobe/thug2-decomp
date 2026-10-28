#include "thug2/runtime.hpp"
#include <bit>
#include <cmath>
#include <iostream>
#include <limits>
#include <stdexcept>

namespace {
using namespace thug2;using namespace thug2::qb;
void check(bool ok,const char* why){if(!ok)throw std::runtime_error(why);}
template<class F> void rejects(F fn,const char* why){bool caught=false;try{fn();}catch(const std::exception&){caught=true;}check(caught,why);}
void word(Bytes& b,std::uint32_t n){for(unsigned i=0;i<4;++i)b.push_back(n>>(8*i));}
void name(Bytes& b,std::uint32_t n){b.push_back(0x16);word(b,n);}
void integer(Bytes& b,std::int32_t n){b.push_back(0x17);word(b,std::bit_cast<std::uint32_t>(n));}
void script(Bytes& file,std::uint32_t n,Bytes body){file.push_back(0x23);name(file,n);file.insert(file.end(),body.begin(),body.end());file.insert(file.end(),{0x24,1});}
Bytes waiting(std::int32_t n,std::string_view flag){Bytes b{1};name(b,name_crc("wait"));integer(b,n);if(!flag.empty())name(b,name_crc(flag));b.push_back(1);return b;}
}
int main(){try{
    FrameTick tick{0,1.0/60,0};bool gate=false;
    auto s=make_wait({WaitKind::Frames,0},tick);check(!advance_wait(s,tick,gate),"Wait 0 gameframes is immediate");
    s=make_wait({WaitKind::Frames,1},tick);check(advance_wait(s,tick,gate)&&s.counter==0,"New frame wait is checked immediately");
    check(!advance_wait(s,tick,gate),"Frame wait resumes at next check");
    s=make_wait({WaitKind::Timer,0},tick);check(advance_wait(s,tick,gate)&&s.started,"Zero timer still yields once");
    check(!advance_wait(s,tick,gate),"Zero timer releases on next positive frame");
    s=make_wait({WaitKind::Timer,0},tick);for(int i=0;i<3;++i)check(advance_wait(s,{0,0,0},gate),"Paused game timer stays waiting");
    s=make_wait({WaitKind::Timer,1000},tick);double exact=s.timer;
    check(advance_wait(s,{0,exact,0},gate)&&s.timer==0,"Equality subtracts to zero but still waits");
    check(!advance_wait(s,tick,gate),"Timer equality releases next frame");
    s=make_wait({WaitKind::RealTimer,10},{0,0,0xfffffffau});
    check(advance_wait(s,{0,0,3},gate)&&!advance_wait(s,{1,0,4},gate),"Real clock wraps at 32 bits");
    s=make_wait({WaitKind::RealTimer,0},tick);check(!advance_wait(s,tick,gate),"Real zero is immediate");
    rejects([&]{advance_wait(s,{0,std::numeric_limits<double>::infinity(),0},gate);},"Invalid clock rejected");
    rejects([&]{make_wait({static_cast<WaitKind>(3),0},tick);},"Unimplemented blocked wait rejected");

    Bytes file;
    auto body=waiting(1,"gameframes");name(body,100);body.push_back(1);script(file,1,body);
    script(file,2,waiting(0,"gameframes"));script(file,3,waiting(0,""));script(file,4,waiting(0,"one_per_frame"));
    auto twice=waiting(0,"one_per_frame");auto second=waiting(0,"one_per_frame");twice.insert(twice.end(),second.begin(),second.end());script(file,5,twice);
    // A nested call waits, then returns a value. The caller's loop survives.
    auto child=waiting(1,"gameframe");child.push_back(0x29);name(child,77);child.push_back(7);integer(child,9);child.push_back(1);script(file,6,child);
    Bytes loop{1,0x20,1};name(loop,6);loop.insert(loop.end(),{1,0x21});integer(loop,2);loop.push_back(1);script(file,7,loop);
    auto forever=waiting(1,"gameframes");forever.insert(forever.begin()+1,0x20);forever.insert(forever.end(),{0x21,1});script(file,8,forever);
    auto precedence=waiting(1,"gameframes");precedence.pop_back();name(precedence,name_crc("seconds"));precedence.push_back(1);script(file,9,precedence);
    auto real=waiting(0,"");real.pop_back();name(real,0xc847f2b9);real.push_back(1);script(file,10,real);
    script(file,11,waiting(-1,"gameframes"));
    Bytes nan{1};name(nan,name_crc("wait"));nan.push_back(0x1a);word(nan,0x7fc00000);nan.push_back(1);script(file,12,nan);
    script(file,13,waiting(16777217,"gameframes"));
    script(file,14,waiting(2,"frames"));
    auto wrong=waiting(2,"gameframes");wrong.pop_back();wrong.push_back(0x1b);word(wrong,2);wrong.insert(wrong.end(),{'x',0,1});script(file,15,wrong);
    auto multiple=waiting(2,"gameframes");multiple.pop_back();integer(multiple,3);multiple.push_back(1);script(file,16,multiple);
    Bytes flags{1};name(flags,name_crc("wait"));name(flags,name_crc("gameframes"));flags.push_back(1);script(file,17,flags);
    // Flags may come from a global structure referenced by an unnamed name.
    name(file,900);file.insert(file.end(),{7,3});name(file,name_crc("gameframes"));file.insert(file.end(),{4,1});
    Bytes included{1};name(included,name_crc("wait"));integer(included,1);name(included,900);included.push_back(1);script(file,18,included);
    file.push_back(0);SymbolTable symbols;symbols.load_bytes("wait-fixtures",file);Interpreter vm(symbols);vm.install_wait();
    unsigned calls=0;vm.bind(100,[&](const HostCall&){++calls;rejects([&]{vm.start(1);},"Reentrant start");rejects([&]{vm.cancel(1);},"Reentrant cancel");return HostResult{};});
    check(vm.run(1).failure==EvalFailure::Unsupported,"Synchronous waits must require continuation API");
    auto id=vm.start(1);check(vm.inspect(id).status==RunStatus::Ready,"Start prepares without dispatch");
    auto r=vm.update(id,tick);check(r.status==RunStatus::Waiting&&calls==0&&r.wait->counter==0,"Wait yields before following host");
    auto duplicate=vm.update(id,tick);check(duplicate.work==r.work&&calls==0,"Same frame update is idempotent");
    r=vm.update(id,{1,1.0/60,16});check(r.status==RunStatus::Finished&&calls==1,"Next frame resumes PC");
    check(vm.update(id,{1,1.0/60,16}).work==r.work,"Completed update is idempotent");
    rejects([&]{vm.update(id,{0,1.0/60,0});},"Backward tick");
    rejects([&]{vm.update(id,{1,0,16});},"Conflicting same-frame clocks");
    rejects([&]{vm.update(id,{2,-1,20});},"Negative duration");
    check(vm.cancel(id)&&!vm.cancel(id),"Cancel releases task");rejects([&]{vm.inspect(id);},"Unknown task");
    auto zero=vm.start(2);check(vm.update(zero,{2,1.0/60,33}).status==RunStatus::Finished,"Zero frame wait does not yield");
    auto timer=vm.start(3);check(vm.update(timer,{2,1.0/60,33}).status==RunStatus::Waiting,"Zero timer yields");
    check(vm.update(timer,{3,1.0/60,50}).status==RunStatus::Finished,"Zero timer resumes");
    auto a=vm.start(4),b=vm.start(4);auto all=vm.update_all({4,1.0/60,66});(void)all;
    check(vm.inspect(a).status==RunStatus::Finished&&vm.inspect(b).status==RunStatus::Waiting,"One-per-frame gate shared across tasks in creation order");
    check(vm.update(b,{5,1.0/60,83}).status==RunStatus::Finished,"Gate resets next frame");
    auto two=vm.start(5);check(vm.update(two,{6,1.0/60,100}).status==RunStatus::Waiting,"Multiple waits share the same frame gate");
    check(vm.update(two,{7,1.0/60,116}).status==RunStatus::Finished,"Second gate resumes next frame");
    auto nested=vm.start(7);check(vm.update(nested,{8,1.0/60,133}).status==RunStatus::Waiting,"Callee first wait");
    check(vm.update(nested,{9,1.0/60,150}).status==RunStatus::Waiting,"Loop/call continuation reaches second wait");
    auto finished=vm.update(nested,{10,1.0/60,166});
    check(finished.status==RunStatus::Finished&&finished.parameters->members.back().value->bits==9,"Return parameters survive callee wait");
    RunLimits fuel;fuel.fuel=100;auto infinite=vm.start(8,{},fuel);
    for(std::uint64_t i=11;i<100;++i){r=vm.update(infinite,{i,1.0/60,static_cast<std::uint32_t>(i*16)});if(r.status!=RunStatus::Waiting)break;}
    check(r.status==RunStatus::Limit&&r.work<=100,"Wait cannot replenish lifetime fuel");
    auto precedence_id=vm.start(9);r=vm.update(precedence_id,{100,1.0/60,1600});check(r.wait&&r.wait->kind==WaitKind::Timer&&r.wait->timer>0.9f,"seconds takes precedence over gameframes");
    auto real_id=vm.start(10);check(vm.update(real_id,{100,1.0/60,1600}).status==RunStatus::Finished,"Real-time flag selects real timer");
    auto negative=vm.start(11);r=vm.update(negative,{100,1.0/60,1600});check(r.wait&&r.wait->counter==0xfffffffeu,"Negative count wraps unsigned");
    auto invalid=vm.start(12);check(vm.update(invalid,{100,1.0/60,1600}).failure==EvalFailure::Arithmetic,"NaN duration diagnosed");
    auto rounded=vm.start(13);r=vm.update(rounded,{100,1.0/60,1600});check(r.wait&&r.wait->counter==16777215,"GetFloat converts integer before truncating frames");
    Interpreter parse_vm(symbols);parse_vm.install_wait();
    auto frame_units=parse_vm.start(14);r=parse_vm.update(frame_units,{0,0,0});check(r.wait&&r.wait->kind==WaitKind::Timer&&r.wait->timer>0.032f&&r.wait->timer<0.034f,"frames uses truncated 60Hz milliseconds");
    auto wrong_type=parse_vm.start(15);r=parse_vm.update(wrong_type,{0,0,0});check(r.wait&&r.wait->counter==1,"Wrong type after numeric argument does not erase GetFloat match");
    auto last_number=parse_vm.start(16);r=parse_vm.update(last_number,{0,0,0});check(r.wait&&r.wait->counter==2,"Last numeric match wins");
    auto no_number=parse_vm.start(17);check(parse_vm.update(no_number,{0,0,0}).status==RunStatus::Finished,"Missing numeric argument defaults to zero");
    auto structure_flag=parse_vm.start(18);r=parse_vm.update(structure_flag,{0,0,0});check(r.wait&&r.wait->kind==WaitKind::Frames,"Included structure supplies flags");
    Interpreter capacity(symbols);for(unsigned i=0;i<256;++i)capacity.start(2);
    rejects([&]{capacity.start(2);},"Task capacity bounded");capacity.cancel(1);check(capacity.start(2)==257,"Cancellation frees capacity without reusing IDs");
    std::cout<<"Wait clocks, persistent calls/loops, task scheduling, cancellation and bounds passed\n";
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
