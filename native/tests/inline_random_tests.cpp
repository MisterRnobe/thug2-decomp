#include "random_fixture.hpp"
#include <iostream>

namespace {
using namespace random_fixture;
void check(bool ok,const char* why){if(!ok)throw std::runtime_error(why);}
Bytes choice(std::vector<Bytes> values,std::uint8_t op=0x2f){std::vector<std::uint16_t> weights(values.size(),1);auto b=random_block(op,weights,values);b.pop_back();return b;}
Bytes assigned(std::uint32_t n,Bytes value){auto b=name(n);b.push_back(7);append(b,value);return b;}
Bytes returned(std::uint32_t n,Bytes value){Bytes b{0x29};append(b,assigned(n,value));b.push_back(1);return b;}
ValuePtr member(ValuePtr v,std::uint32_t n){for(auto it=v->members.rbegin();it!=v->members.rend();++it)if(it->name==n)return it->value;throw std::runtime_error("Missing fixture member");}
}
int main(){try{
    Bytes file;
    auto numbers=choice({integer(11),integer(22)});
    auto assignment=assigned(10,numbers);assignment.push_back(1);append(assignment,returned(20,param(10)));script(file,1,assignment);
    auto host=name(100);append(host,assigned(20,numbers));append(host,assigned(21,integer(7)));host.push_back(1);script(file,2,host);
    Bytes expression{0x0e};append(expression,numbers);expression.push_back(11);append(expression,integer(5));expression.push_back(0x0f);script(file,3,returned(20,expression));
    // A random default is parsed before entry arguments override it.
    file.push_back(0x23);append(file,name(4));append(file,assigned(10,numbers));file.push_back(1);append(file,returned(20,param(10)));file.insert(file.end(),{0x24,1});
    script(file,5,returned(30,param(10)));
    auto call=name(5);append(call,assigned(10,numbers));call.push_back(1);append(call,returned(20,param(30)));script(file,6,call);
    Bytes structure{3};append(structure,assigned(50,numbers));structure.insert(structure.end(),{4});
    auto struct_host=name(100);append(struct_host,assigned(20,structure));struct_host.push_back(1);script(file,7,struct_host);
    Bytes array{5};append(array,numbers);array.push_back(6);script(file,8,returned(20,array));
    auto flag_host=name(100);append(flag_host,name(700));append(flag_host,numbers);flag_host.push_back(1);script(file,9,flag_host);
    script(file,10,block(numbers,{{integer(11),command(101)},{integer(22),command(102)}},command(199)));
    auto random_case=block(integer(11),{{numbers,command(101)}},command(102));script(file,11,random_case);
    // Count is read only at first repeat even when it contains random control flow.
    Bytes loop{0x20,1};append(loop,command(101));loop.push_back(0x21);append(loop,choice({integer(2),integer(3)}));loop.push_back(1);script(file,12,loop);
    Bytes cyclic{0x2e};word(cyclic,0xfffffffb);script(file,13,returned(20,cyclic));
    // One selected path ends on a different physical line; do not pre-scan to
    // the first branch's EOL when the second branch was selected.
    auto line_branch=random_block(0x2f,{0,1},{returned(20,integer(1)),returned(20,integer(2))});script(file,14,line_branch);
    // Named return, preserving other tail arguments after a selected branch.
    auto tail=returned(20,numbers);tail.pop_back();append(tail,assigned(21,integer(7)));tail.push_back(1);script(file,15,tail);
    // Bare names in a selected branch are flags, including their lookahead.
    auto flags=name(100);append(flags,choice({name(701),name(702)}));flags.push_back(1);script(file,16,flags);
    // Permutation history inside a return/callee must persist in one task.
    auto permute=choice({integer(11),integer(22),integer(33)},0x41);
    auto scheduled=name(100);append(scheduled,assigned(20,permute));scheduled.push_back(1);
    auto wait=name(name_crc("wait"));append(wait,integer(1));append(wait,name(name_crc("gameframes")));wait.push_back(1);
    Bytes schedule{0x20,1};append(schedule,scheduled);append(schedule,wait);schedule.push_back(0x21);append(schedule,integer(6));schedule.push_back(1);script(file,17,schedule);
    // A plain argument line must not consume the following command.
    auto plain=command(100);append(plain,command(101));script(file,18,plain);
    file.push_back(0);SymbolTable table;table.load_bytes("inline-random-fixtures",file);Interpreter vm(table);
    ValuePtr captured;unsigned host_calls=0;std::vector<unsigned> events;
    vm.bind(100,[&](const HostCall& call){captured=call.arguments;++host_calls;return HostResult{};});
    for(unsigned n:{101,102,199})vm.bind(n,[&,n](const HostCall&){events.push_back(n);return HostResult{};});
    auto expected=[&](std::uint32_t seed,unsigned draws=1){RandomStreams rng(seed);int index=0;while(draws--)index=rng.draw(2);return static_cast<std::uint32_t>((index+1)*11);};
    for(std::uint32_t seed:{0u,1u,123u,0xffffffffu}){
        vm.seed_random(seed);auto r=vm.run(1);check(r.status==RunStatus::Finished&&member(r.parameters,20)->bits==expected(seed),"Inline assignment follows selected value");
        vm.seed_random(seed);r=vm.run(2);check(r.status==RunStatus::Finished&&member(captured,20)->bits==expected(seed)&&member(captured,21)->bits==7,"Host argument continues at shared tail");
        vm.seed_random(seed);r=vm.run(3);check(r.status==RunStatus::Finished&&member(r.parameters,20)->bits==expected(seed)+5,"Random expression operand");
        vm.seed_random(seed);r=vm.run(4);check(r.status==RunStatus::Finished&&member(r.parameters,20)->bits==expected(seed),"Random script default");
        vm.seed_random(seed);r=vm.run(4,structure_value({{10,integer_value(99)}}));check(r.status==RunStatus::Finished&&member(r.parameters,20)->bits==99,"Entry input overrides random default");
        vm.seed_random(seed);r=vm.run(6);check(r.status==RunStatus::Finished&&member(r.parameters,20)->bits==expected(seed),"Random QB call argument and return");
        vm.seed_random(seed);r=vm.run(7);check(r.status==RunStatus::Finished&&member(member(captured,20),50)->bits==expected(seed),"Random named structure field");
        vm.seed_random(seed);r=vm.run(9);check(r.status==RunStatus::Finished&&member(captured,0)->bits==expected(seed,2),"Bare flag lookahead consumes an extra random choice like original");
        vm.seed_random(seed);events.clear();r=vm.run(10);check(r.status==RunStatus::Finished&&events==std::vector<unsigned>{expected(seed)==11?101u:102u},"Random switch selector");
        vm.seed_random(seed);events.clear();r=vm.run(11);check(r.status==RunStatus::Finished&&events==std::vector<unsigned>{expected(seed)==11?101u:102u},"Random case argument");
        vm.seed_random(seed);r=vm.run(15);check(r.status==RunStatus::Finished&&member(r.parameters,20)->bits==expected(seed)&&member(r.parameters,21)->bits==7,"Return arguments include selected/common parts");
        vm.seed_random(seed);r=vm.run(16);check(r.status==RunStatus::Finished&&member(captured,0)->type==ValueType::Name&&member(captured,0)->bits==static_cast<unsigned>(expected(seed)/11+700),"Selected bare flag is preserved");
    }
    check(vm.run(8).failure==EvalFailure::Unsupported,"Array control flow remains explicit until two-pass semantics exist");
    events.clear();vm.seed_random(0);auto r=vm.run(12);check(r.status==RunStatus::Finished&&events.size()==3,"Random repeat count read once");
    RunLimits bounds;bounds.fuel=100;check(vm.run(13,{},bounds).status==RunStatus::Limit,"Inline cyclic jump consumes lifetime fuel");
    r=vm.run(14);check(r.status==RunStatus::Finished&&member(r.parameters,20)->bits==2,"Selected physical line determines argument end");
    std::vector<unsigned> values;vm.bind(100,[&](const HostCall& c){values.push_back(member(c.arguments,20)->bits);return HostResult{};});vm.install_wait();vm.seed_random(77);auto task=vm.start(17);
    for(std::uint64_t i=0;i<7;++i)check(vm.update(task,{i,1.0/60,static_cast<std::uint32_t>(i*16)}).status==(i<6?RunStatus::Waiting:RunStatus::Finished),"Inline permutation resumes across frames");
    check(values.size()==6&&values[2]!=values[3]&&values[3]!=values[4]&&values[2]!=values[4],"Inline permutation keeps order across wait");
    vm.bind(100,[&](const HostCall& c){check(c.arguments->members.empty(),"Empty line has empty parameters");++host_calls;return HostResult{};});
    events.clear();check(vm.run(18).status==RunStatus::Finished&&events==std::vector<unsigned>{101},"Runtime argument parser stops on top-level EOL");
    auto static_bytes=numbers;static_bytes.push_back(0);Document static_doc(static_bytes);ValueEvaluator static_reader(table);
    bool static_rejected=false;try{static_reader.arguments(static_doc,0,static_bytes.size()-1);}catch(const EvalError& e){static_rejected=e.kind==EvalFailure::Unsupported;}
    check(static_rejected,"Static value APIs do not implicitly execute random control flow");
    Bytes raw=integer(7);raw.push_back(0);Document doc(raw);ValueEvaluator reader(table);
    bool escaped=false;try{reader.read(doc,0,raw.size()-1,{},[&](std::size_t){return raw.size();});}catch(const EvalError& e){escaped=e.kind==EvalFailure::Syntax;}check(escaped,"External value flow cannot escape its source");
    std::cout<<"Inline random arguments, assignments, expressions, defaults, calls, structures, switch, loops and waits passed\n";
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
