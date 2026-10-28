#include "random_fixture.hpp"
#include <iostream>

namespace {
using namespace random_fixture;
void check(bool ok,const char* why){if(!ok)throw std::runtime_error(why);}
Bytes choice(std::vector<Bytes> values,std::uint8_t op=0x2f){auto b=random_block(op,std::vector<std::uint16_t>(values.size(),1),values);b.pop_back();return b;}
Bytes array(Bytes content){Bytes b{5};append(b,content);b.push_back(6);return b;}
Bytes assigned(unsigned n,Bytes value){auto b=name(n);b.push_back(7);append(b,value);return b;}
Bytes returned(Bytes value){Bytes b{0x29};append(b,assigned(20,value));b.push_back(1);return b;}
ValuePtr member(ValuePtr v,unsigned n){for(auto it=v->members.rbegin();it!=v->members.rend();++it)if(it->name==n)return it->value;throw std::runtime_error("Missing array fixture member");}
std::uint32_t seed_for(int first,int second){for(std::uint32_t seed=0;seed<1000;++seed){RandomStreams rng(seed);if(rng.draw(2)==first&&rng.draw(2)==second)return seed;}throw std::runtime_error("No matching seed");}
Bytes text(std::string_view s,bool local=false){Bytes b{static_cast<std::uint8_t>(local?0x1c:0x1b)};word(b,s.size()+1);b.insert(b.end(),s.begin(),s.end());b.push_back(0);return b;}
}
int main(){try{
    Bytes file;
    auto numbers=choice({integer(11),integer(22)});
    script(file,1,returned(array(numbers)));
    auto two=integer(31);append(two,integer(32));script(file,2,returned(array(choice({integer(11),two}))));
    script(file,3,returned(array(choice({integer(16777217),floating(2.5f)}))));
    script(file,4,returned(array(array(numbers))));
    Bytes structure{3};append(structure,assigned(50,numbers));structure.push_back(4);script(file,5,returned(array(structure)));
    auto empty=returned(array(choice({{},integer(11)})));empty.pop_back();append(empty,assigned(30,numbers));empty.push_back(1);script(file,6,empty);
    script(file,7,returned(array(choice({text("a"),text("b")}))));
    script(file,8,returned(array(choice({name(1000),name(2000)}))));
    auto nested=choice({array(choice({integer(11),integer(22)})),array(choice({integer(33),integer(44)}))});script(file,9,returned(array(nested)));
    auto pointer_two=text("b");append(pointer_two,text("c"));script(file,10,returned(array(choice({text("a"),pointer_two}))));
    Bytes cyclic{0x2e};word(cyclic,0xfffffffb);script(file,11,returned(array(cyclic)));
    script(file,12,returned(array(choice({Bytes{0x2d,0x16,9,0,0,0},integer(11)}))));
    script(file,13,returned(array(choice({integer(11),integer(22)},0x40))));
    script(file,14,returned(array(choice({integer(11),integer(22),integer(33)},0x41))));
    auto promoted=floating(1.5f);append(promoted,integer(16777217));script(file,15,returned(array(promoted)));
    script(file,16,returned(array(choice({text("a",true),text("b",true)}))));
    auto coordinates=[](std::uint8_t op,std::initializer_list<float> values){Bytes b{op};for(auto v:values)word(b,std::bit_cast<std::uint32_t>(v));return b;};
    script(file,17,returned(array(choice({coordinates(0x1e,{1,2,3}),coordinates(0x1e,{4,5,6})}))));
    script(file,18,returned(array(choice({coordinates(0x1f,{1,2}),coordinates(0x1f,{3,4})}))));
    auto names=name(31);append(names,name(32));script(file,19,returned(array(choice({name(11),names}))));
    auto floats=floating(3.5f);append(floats,floating(4.5f));script(file,20,returned(array(choice({floating(1.5f),floats}))));
    for(unsigned opcode:{0x40,0x41}){
        auto values=std::vector<Bytes>{integer(11),integer(22)};if(opcode==0x41)values.push_back(integer(33));
        Bytes body{0x20,1};append(body,name(100));append(body,assigned(20,array(choice(values,static_cast<std::uint8_t>(opcode)))));body.push_back(1);
        append(body,name(name_crc("wait")));append(body,integer(1));append(body,name(name_crc("gameframes")));body.push_back(1);
        body.push_back(0x21);append(body,integer(4));body.push_back(1);script(file,opcode==0x40?21:22,body);
    }
    append(file,name(1000));file.push_back(7);append(file,integer(99));file.push_back(1);
    file.push_back(0);SymbolTable table;table.load_bytes("array-random-fixtures",file);Interpreter vm(table);
    auto run=[&](unsigned script,std::uint32_t seed){vm.seed_random(seed);return vm.run(script);};
    auto value=[&](unsigned script,std::uint32_t seed){auto r=run(script,seed);check(r.status==RunStatus::Finished,r.message.c_str());auto v=member(r.parameters,20);check(v->type==ValueType::Array,"Result must be array");return v;};
    for(std::uint32_t seed:{0u,1u,123u,0xffffffffu}){
        RandomStreams rng(seed);rng.draw(2);auto second=rng.draw(2);auto v=value(1,seed);check(v->element_type==ValueType::Integer&&v->elements.size()==1&&v->elements[0]->bits==static_cast<unsigned>((second+1)*11),"Array population uses second random selection");
        v=value(4,seed);check(v->element_type==ValueType::Array&&v->elements[0]->elements[0]->bits==static_cast<unsigned>((second+1)*11),"Outer sizing skips nested array without consuming inner random");
        RandomStreams first_rng(seed);auto first=first_rng.draw(2);v=value(5,seed);check(v->element_type==ValueType::Structure&&member(v->elements[0],50)->bits==static_cast<unsigned>((first+1)*11),"Structure fields evaluated only during population");
        v=value(7,seed);check(v->element_type==ValueType::String&&v->elements[0]->text==(second?"b":"a"),"Random string array");
        v=value(8,seed);check(v->element_type==ValueType::Name&&v->elements[0]->bits==(second?2000u:1000u),"Checksum arrays keep global names unresolved");
        RandomStreams nested_rng(seed);nested_rng.draw(2);auto chosen_outer=nested_rng.draw(2);nested_rng.draw(2);auto chosen_inner=nested_rng.draw(2);v=value(9,seed);
        check(v->elements[0]->elements[0]->bits==static_cast<unsigned>((chosen_outer*2+chosen_inner+1)*11),"Each nested array sizes and populates independently");
        v=value(16,seed);check(v->element_type==ValueType::LocalString&&v->elements[0]->text==(second?"b":"a"),"Localized strings preserve type");
        v=value(17,seed);check(v->element_type==ValueType::Vector&&v->elements[0]->coordinates[0]==std::bit_cast<std::uint32_t>(second?4.0f:1.0f),"Random vector array");
        v=value(18,seed);check(v->element_type==ValueType::Pair&&v->elements[0]->coordinates[0]==std::bit_cast<std::uint32_t>(second?3.0f:1.0f),"Random pair array");
    }
    auto shrink=seed_for(1,0),grow=seed_for(0,1);
    auto v=value(2,shrink);check(v->elements.size()==2&&v->elements[0]->bits==11&&v->elements[1]->bits==0,"Short scalar population retains sized zero tail");
    v=value(19,shrink);check(v->element_type==ValueType::Name&&v->elements[1]->bits==0,"Short checksum population leaves typed zero");
    v=value(20,shrink);check(v->element_type==ValueType::Float&&v->elements[1]->bits==0,"Short float population leaves positive zero");
    check(run(2,grow).failure==EvalFailure::Type,"Population overflow diagnosed before writing extra elements");
    v=value(3,shrink);check(v->element_type==ValueType::Float&&v->elements[0]->bits==std::bit_cast<std::uint32_t>(16777216.0f),"Sizing type controls int-to-float promotion");
    check(run(3,grow).failure==EvalFailure::Type,"Incompatible population type diagnosed");
    auto r=run(6,seed_for(0,1));v=member(r.parameters,20);RandomStreams rng(seed_for(0,1));rng.draw(2);auto next=rng.draw(2);
    check(r.status==RunStatus::Finished&&v->element_type==ValueType::None&&v->elements.empty()&&member(r.parameters,30)->bits==static_cast<unsigned>((next+1)*11),"Empty sizing path skips second pass");
    check(run(10,shrink).failure==EvalFailure::Unsupported,"Unfilled pointer slots remain explicit");
    RunLimits limits;limits.fuel=100;vm.seed_random(0);check(vm.run(11,{},limits).status==RunStatus::Limit,"Sizing jump cycles consume lifetime fuel");
    check(run(12,seed_for(0,0)).failure==EvalFailure::Unsupported,"Parameter references are not literal array elements");
    for(unsigned opcode:{0x40,0x41}){
        Interpreter fresh(table);fresh.seed_random(77);auto result=fresh.run(opcode==0x40?13:14);check(result.status==RunStatus::Finished,"Stored choices supported in array passes");
        RandomSelector model;model.seed(77);std::vector<std::uint16_t> weights(opcode==0x40?2:3,1);model.choose(static_cast<std::uint8_t>(opcode),1,0,weights,1);auto selected=model.choose(static_cast<std::uint8_t>(opcode),1,0,weights,1);
        check(member(result.parameters,20)->elements[0]->bits==(selected+1)*11,"Both passes share no-repeat/permutation history");
    }
    v=value(15,0);check(v->element_type==ValueType::Float&&v->elements[1]->bits==std::bit_cast<std::uint32_t>(16777216.0f),"Mixed numeric literals retain float array type");
    for(unsigned opcode:{0x40,0x41}){
        Interpreter scheduled(table);scheduled.seed_random(77);scheduled.install_wait();std::vector<unsigned> observed;
        scheduled.bind(100,[&](const HostCall& c){observed.push_back(member(c.arguments,20)->elements.at(0)->bits);return HostResult{};});
        auto task=scheduled.start(opcode==0x40?21:22);
        for(std::uint64_t frame=0;frame<5;++frame)check(scheduled.update(task,{frame,1.0/60,static_cast<std::uint32_t>(frame*16)}).status==(frame<4?RunStatus::Waiting:RunStatus::Finished),"Stored array choices resume through waits");
        RandomSelector model;model.seed(77);std::vector<std::uint16_t> weights(opcode==0x40?2:3,1);std::vector<unsigned> expected;
        for(unsigned i=0;i<4;++i){model.choose(static_cast<std::uint8_t>(opcode),1,0,weights,1);expected.push_back(static_cast<unsigned>((model.choose(static_cast<std::uint8_t>(opcode),1,0,weights,1)+1)*11));}
        check(observed==expected,"No-repeat/permutation histories span both passes and frames");
    }
    EvalLimits tiny;tiny.nodes=1;Interpreter bounded(table,tiny);bounded.seed_random(shrink);
    check(bounded.run(2).status==RunStatus::Limit,"Sizing count respects value allocation bounds");
    std::cout<<"Two-pass random arrays, nested values, sizing types, zero tails and bounds passed\n";
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
