#include "thug2/runtime.hpp"
#include <bit>
#include <iostream>
#include <stdexcept>

namespace {
using namespace thug2;using namespace thug2::qb;
void check(bool ok,const char* why){if(!ok)throw std::runtime_error(why);}
void word(Bytes& b,std::uint32_t n){for(unsigned i=0;i<4;++i)b.push_back(n>>(i*8));}
void name(Bytes& b,std::uint32_t n){b.push_back(0x16);word(b,n);}
void number(Bytes& b,std::int32_t n){b.push_back(0x17);word(b,std::bit_cast<std::uint32_t>(n));}
void param(Bytes& b,std::uint32_t n){b.push_back(0x2d);name(b,n);}
void script(Bytes& file,std::uint32_t n,Bytes body){file.push_back(0x23);name(file,n);file.insert(file.end(),body.begin(),body.end());file.push_back(0x24);file.push_back(1);}
std::uint32_t field(ValuePtr p,std::uint32_t n){for(auto it=p->members.rbegin();it!=p->members.rend();++it)if(it->name==n)return it->value->bits;throw std::runtime_error("Missing result member");}
}
int main(){
 try{
    Bytes file;
    // Callee default x=2, caller supplies x=7; return y=(<x>+1).
    Bytes callee;name(callee,10);callee.push_back(7);number(callee,2);callee.push_back(1);
    callee.push_back(0x29);name(callee,11);callee.insert(callee.end(),{7,0x0e});param(callee,10);callee.push_back(11);number(callee,1);callee.insert(callee.end(),{0x0f,1});
    script(file,2,callee);
    Bytes main{1};name(main,2);name(main,10);main.push_back(7);number(main,7);main.push_back(1);
    main.push_back(0x29);name(main,12);main.push_back(7);param(main,11);main.push_back(1);script(file,1,main);
    // A loop modifies its count parameter inside the body; repeat count is read
    // once on the first repeat, and must not be re-read on later iterations.
    Bytes loop{1};name(loop,20);loop.push_back(7);number(loop,0);loop.push_back(1);
    name(loop,21);loop.push_back(7);number(loop,3);loop.push_back(1);loop.push_back(0x20);loop.push_back(1);
    name(loop,20);loop.insert(loop.end(),{7,0x0e});param(loop,20);loop.push_back(11);number(loop,1);loop.insert(loop.end(),{0x0f,1});
    name(loop,100);loop.push_back(1);loop.push_back(0x21);param(loop,21);loop.push_back(1);script(file,3,loop);
    Bytes endless{1,0x20,1};name(endless,100);endless.insert(endless.end(),{1,0x21});number(endless,0);endless.push_back(1);script(file,4,endless);
    Bytes breaking{1,0x20,1,0x20,1,0x22,1,0x21,1,0x22,1,0x21,1};script(file,5,breaking);
    Bytes recurse{1};name(recurse,6);recurse.push_back(1);script(file,6,recurse);
    // Compiled if/else offsets are relative to the offset field, not token end.
    Bytes branch{1,0x47,0,0};name(branch,101);branch.push_back(1);
    name(branch,102);branch.push_back(1);auto otherwise=branch.size();branch.insert(branch.end(),{0x48,0,0});
    auto else_body=branch.size();name(branch,103);branch.push_back(1);auto endif=branch.size();branch.insert(branch.end(),{0x28,1});
    auto false_jump=else_body-2;branch[2]=false_jump&255;branch[3]=false_jump>>8;
    auto else_jump=endif-(otherwise+1);branch[otherwise+1]=else_jump&255;branch[otherwise+2]=else_jump>>8;script(file,7,branch);
    // Unknown host, and runtime features must stop rather than acting as no-ops.
    Bytes unknown{1};name(unknown,999);unknown.push_back(1);script(file,8,unknown);
    Bytes spread{1,0x29,0x2c,1};script(file,9,spread);
    auto qb_condition=branch;for(unsigned i=0;i<4;++i)qb_condition[5+i]=std::uint32_t(2)>>(i*8);
    script(file,10,qb_condition);
    auto negated=qb_condition;negated.insert(negated.begin()+4,0x39);
    unsigned jump=unsigned(negated[2])|(unsigned(negated[3])<<8);++jump;negated[2]=jump&255;negated[3]=jump>>8;
    script(file,11,negated);
    file.push_back(0);SymbolTable table;table.load_bytes("runtime-fixtures",file);Interpreter vm(table);
    Bytes zero;name(zero,0);zero.insert(zero.end(),{7,3});name(zero,77);zero.push_back(7);number(zero,5);zero.insert(zero.end(),{4,0});
    Document arguments(zero);ValueEvaluator reader(table);auto parsed=reader.arguments(arguments,0,zero.size()-1);
    check(parsed.value->members.size()==1&&parsed.value->members[0].name==0&&
          parsed.value->members[0].value->type==ValueType::Structure,"Explicit zero-name parameter must not spread its structure");
    auto result=vm.run(1);check(result.status==RunStatus::Finished&&field(result.parameters,12)==8,"Call/defaults/return merge");
    result=vm.run(2,structure_value({{10,integer_value(9)}}));
    check(result.status==RunStatus::Finished&&field(result.parameters,11)==10,"Entry arguments override defaults");
    unsigned calls=0;
    vm.bind(100,[&](const HostCall&){++calls;return HostResult{true,structure_value({{21,integer_value(5)}})};});
    result=vm.run(3); // First host callback changes count to 5, so repeat caches 5.
    check(result.status==RunStatus::Finished&&calls==5&&field(result.parameters,20)==5,"Repeat caches its first count");
    calls=0;RunLimits limited;limited.fuel=200;result=vm.run(4,{},limited);
    check(result.status==RunStatus::Limit&&calls>0,"repeat 0 must wrap, not finish immediately");
    check(vm.run(5).status==RunStatus::Finished,"Nested break");
    RunLimits shallow;shallow.calls=3;check(vm.run(6,{},shallow).status==RunStatus::Limit,"Call-depth bound");
    RunLimits tiny_code;tiny_code.code_tokens=3;check(vm.run(1,{},tiny_code).status==RunStatus::Limit,"Compiled token-count bound");
    tiny_code.code_tokens=100000;tiny_code.code_bytes=1;check(vm.run(1,{},tiny_code).status==RunStatus::Limit,"Compiled byte bound");
    std::vector<unsigned> events;bool condition=false;
    vm.bind(101,[&](const HostCall&){return HostResult{condition,{}};});
    vm.bind(102,[&](const HostCall&){events.push_back(102);return HostResult{};});
    vm.bind(103,[&](const HostCall&){events.push_back(103);return HostResult{};});
    check(vm.run(7).status==RunStatus::Finished&&events==std::vector<unsigned>{103},"False branch target");
    condition=true;events.clear();check(vm.run(7).status==RunStatus::Finished&&events==std::vector<unsigned>{102},"True branch/else skip");
    events.clear();auto called_condition=vm.run(10);
    check(called_condition.status==RunStatus::Finished&&events==std::vector<unsigned>{102}&&field(called_condition.parameters,11)==3,
          "A QB call condition counts as true and executes its callee first");
    check(vm.run(11).failure==EvalFailure::Unsupported,"Negated QB-call legacy behavior must be explicit");
    auto missing=vm.run(8);check(missing.status==RunStatus::Fault&&missing.failure==EvalFailure::Missing,"Missing host must fault");
    auto spread_result=vm.run(9,structure_value({{77,integer_value(9)}}));
    check(spread_result.status==RunStatus::Finished&&field(spread_result.parameters,77)==9,"All-parameter spread");
    vm.bind(999,[](const HostCall&)->HostResult{throw std::runtime_error("test host failure");});
    check(vm.run(8).message=="test host failure"&&vm.run(1).status==RunStatus::Finished,"Host fault containment and run reset");
    RunLimits trace;trace.trace=1;auto truncated=vm.run(1,{},trace);check(truncated.trace.size()==1&&truncated.trace_truncated,"Bounded trace");
    table.clear();check(vm.run(1).status==RunStatus::Finished,"Interpreter snapshot lifetime");
    // A script from a serialized image enters the same executor.
    Bytes body{1,0x29};name(body,77);body.push_back(7);number(body,5);body.insert(body.end(),{1,0x24});
    auto crc=std::uint32_t(0xffffffff);
    for(std::size_t p=0;p<body.size();){auto token=read_token(body,p);if(token.opcode!=1&&token.opcode!=2&&token.opcode!=0x24)
        crc=extend_crc(crc,std::span(body).subspan(p,token.size));p+=token.size;}
    Bytes image{0x6f,0xef,0x75,0xb0,0,0,0,0,0x1c,8,2,4,0x10,4,8,0xc,0xc,8,2,4,0x14,2,4,0xc,0x10,0x10,0xc,0};
    word(image,7<<16);word(image,20);word(image,0);word(image,48);word(image,0);
    word(image,crc);word(image,body.size());word(image,body.size());image.insert(image.end(),body.begin(),body.end());
    while(image.size()%4)image.push_back(0);for(unsigned i=0;i<4;++i)image[4+i]=image.size()>>(i*8);
    table.load_bytes("image-runtime",image);Interpreter image_vm(table);
    auto image_result=image_vm.run(20);check(image_result.status==RunStatus::Finished&&field(image_result.parameters,77)==5,"Serialized script execution");
    std::cout<<"QB calls, defaults, returns, parameters, loops, if/else, hosts and execution bounds passed\n";
 }catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}
