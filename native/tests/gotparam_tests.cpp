#include "random_fixture.hpp"
#include <iostream>

namespace {
using namespace random_fixture;
void check(bool ok,const char* why){if(!ok)throw std::runtime_error(why);}
Bytes assigned(unsigned n,Bytes value){auto b=name(n);b.push_back(7);append(b,value);return b;}
Bytes returned(Bytes value){Bytes b{0x29};append(b,assigned(20,value));b.push_back(1);return b;}
Bytes branch(Bytes query){Bytes b{0x47,0,0};append(b,name(name_crc("GotParam")));append(b,query);b.push_back(1);append(b,returned(integer(1)));auto no=b.size();append(b,returned(integer(0)));jump(b,1,no);return b;}
ValuePtr member(ValuePtr v,unsigned n){for(const auto& m:v->members)if(m.name==n)return m.value;throw std::runtime_error("Missing GotParam result");}
void global(Bytes& b,unsigned n,Bytes value){append(b,assigned(n,value));b.push_back(1);}
}
int main(){try{
    Bytes file;script(file,1,branch(name(700)));script(file,2,branch(param(11)));script(file,3,branch(name(800)));
    auto caller=command(1);append(caller,returned(param(20)));script(file,4,caller);
    auto spread=name(1);spread.insert(spread.end(),{0x2c,1});append(spread,returned(param(20)));script(file,5,spread);
    file.push_back(0x23);append(file,name(6));append(file,assigned(700,integer(0)));file.push_back(1);append(file,branch(name(700)));file.insert(file.end(),{0x24,1});
    script(file,7,branch({}));auto two=name(700);append(two,name(701));script(file,8,branch(two));script(file,9,branch(integer(123)));
    auto skipped=name(700);append(skipped,name(800));script(file,10,branch(skipped));
    global(file,700,integer(99));Bytes included{3};append(included,name(700));included.push_back(4);global(file,800,included);global(file,801,name(800));
    Bytes cycle{3};append(cycle,name(901));cycle.push_back(4);global(file,900,cycle);cycle=Bytes{3};append(cycle,name(900));cycle.push_back(4);global(file,901,cycle);
    file.push_back(0);SymbolTable table;table.load_bytes("gotparam-fixtures",file);Interpreter vm(table);
    check(vm.run(1).failure==EvalFailure::Missing,"GotParam requires opt-in data hosts");vm.install_data_hosts();
    auto run=[&](unsigned script,ValuePtr params={}){auto r=vm.run(script,params);check(r.status==RunStatus::Finished,r.message.c_str());return member(r.parameters,20)->bits!=0;};
    check(!run(1),"Command query flag is not a caller parameter, nor does an existing global imply presence");
    check(run(1,structure_value({{700,integer_value(0)}})),"Zero-valued named parameter is present");
    check(run(1,structure_value({{700,float_value(0)}})),"Float zero is present");
    check(run(1,structure_value({{0,name_value(700)}})),"Unnamed flag is present");
    check(run(1,structure_value({{0,name_value(800)}}))&&run(1,structure_value({{0,name_value(801)}})),"Included global structures and aliases are searched");
    check(!run(1,structure_value({{800,structure_value({{700,integer_value(1)}})}})),"Named nested structures are not implicitly searched");
    check(!run(3,structure_value({{800,integer_value(0)}})),"Unnamed global structure query is skipped by GetChecksum");
    check(run(3,structure_value({{0,name_value(700)}})),"Skipped query falls back to zero, finding an unnamed caller parameter");
    check(run(10,structure_value({{700,integer_value(0)}})),"Skipped later inclusion does not erase prior valid query checksum");
    check(!run(4,structure_value({{700,integer_value(1)}})),"Callee does not inherit caller parameters implicitly");
    check(run(5,structure_value({{700,integer_value(1)}})),"Explicit spread forwards caller parameters");check(run(6),"Callee defaults count as present");
    check(!run(7)&&run(7,structure_value({{0,integer_value(0)}})),"Absent query defaults to checksum zero");
    check(!run(9)&&run(9,structure_value({{0,name_value(777)}})),"Wrong query type also falls back to zero");
    check(!run(8,structure_value({{700,integer_value(1)}}))&&run(8,structure_value({{701,integer_value(1)}})),"Last unnamed checksum query wins");
    check(run(2,structure_value({{11,name_value(700)},{700,integer_value(1)}})),"Dynamic query checksum comes from current parameters");
    auto r=vm.run(1,structure_value({{0,name_value(900)}}));check(r.failure==EvalFailure::Cycle,"Included cycles are diagnosed");
    check(run(1,structure_value({{700,integer_value(0)},{0,name_value(900)}})),"Found field short-circuits later cyclic inclusion");
    std::vector<Member> many;for(unsigned i=1000;i<2000;++i)many.push_back({i,integer_value(0)});many.push_back({700,integer_value(0)});
    RunLimits budget;budget.fuel=200;check(vm.run(1,structure_value(many),budget).status==RunStatus::Limit,"Parameter scanning charges execution fuel before branch effects");
    auto params=structure_value({{700,integer_value(0)}});run(1,params);check(params->members.size()==1,"Input parameter structure remains immutable");
    table.clear();check(run(1,structure_value({{0,name_value(800)}})),"GotParam uses the fixed interpreter snapshot");
    std::cout<<"GotParam presence, flags, queries, defaults, call frames, inclusions and bounds passed\n";
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
