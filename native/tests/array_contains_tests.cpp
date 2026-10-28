#include "random_fixture.hpp"
#include <iostream>

namespace {
using namespace random_fixture;
void check(bool ok,const char* why){if(!ok)throw std::runtime_error(why);}
Bytes assigned(unsigned n,Bytes value){auto b=name(n);b.push_back(7);append(b,value);return b;}
ValuePtr member(ValuePtr v,unsigned n){for(const auto& m:v->members)if(m.name==n)return m.value;return {};}
}
int main(){try{
    Bytes file,body{0x47,0,0};append(body,name(name_crc("ArrayContains")));append(body,assigned(name_crc("array"),param(10)));
    append(body,assigned(name_crc("contains"),param(11)));append(body,param(12));body.push_back(1);
    auto result=[&](int n){body.push_back(0x29);append(body,assigned(20,integer(n)));body.push_back(1);};
    result(1);jump(body,1,body.size());result(0);script(file,1,body);
    Bytes list{5};append(list,integer(-1));append(list,integer(0));list.push_back(6);
    append(file,assigned(800,list));file.push_back(1);append(file,assigned(801,name(800)));file.push_back(1);
    append(file,assigned(802,integer(-1)));file.push_back(1);file.push_back(0);
    SymbolTable table;table.load_bytes("array-contains-fixtures",file);Interpreter vm(table);
    check(vm.run(1).failure==EvalFailure::Missing,"ArrayContains opt-in");vm.install_data_hosts();unsigned cases=0;
    auto run=[&](ValuePtr array,ValuePtr target,ValuePtr spread={}){
        std::vector<Member> p{{30,integer_value(77)}};if(array)p.push_back({10,array});if(target)p.push_back({11,target});if(spread)p.push_back({12,spread});return vm.run(1,structure_value(p));
    };
    auto has=[&](ValuePtr array,ValuePtr target,bool expected,ValuePtr spread={}){
        auto r=run(array,target,spread);check(r.status==RunStatus::Finished,r.message.c_str());
        check(member(r.parameters,20)->bits==unsigned(expected),"Unexpected ArrayContains condition");check(member(r.parameters,30)->bits==77,"Caller parameter retained");++cases;
    };
    auto ints=array_value({integer_value(1),integer_value(-1),integer_value(0)});
    has(ints,integer_value(-1),true);has(ints,integer_value(2),false);has(ints,float_value(1),true); // Wrong-type query defaults to zero.
    has(array_value({integer_value(1)}),float_value(1),false);has(ints,{},true);has(name_value(801),name_value(802),true);
    has(array_value({name_value(800),name_value(0)}),name_value(800),true);
    has(array_value({name_value(800)}),name_value(801),false); // Name queries remain raw, not alias-resolved.
    has(array_value({name_value(0)}),integer_value(9),true);
    auto strings=array_value({string_value("Alpha"),string_value("BETA"),string_value("")});
    has(strings,string_value("aLPHa"),true);has(strings,string_value("beta",true),true);has(strings,string_value("bet"),false);
    has(strings,{},true);has(array_value({string_value("a")}),string_value("aa"),false);
    has(array_value({string_value(std::string("A\0tail",6))}),string_value("a"),true);
    has(array_value({string_value(std::string("\xc0",1))}),string_value(std::string("\xe0",1)),false);
    has(array_value({string_value("beta")}),string_value("beta",true),false,structure_value({{name_crc("contains"),string_value("alpha")}}));
    has(array_value({string_value("alpha")}),string_value("beta"),true,structure_value({{name_crc("contains"),string_value("alpha",true)},{name_crc("contains"),string_value("ALPHA")}}));
    for(auto array:{array_value({}),array_value({float_value(1)}),array_value({string_value("a",true)}),array_value({ints}),array_value({structure_value({})})})has(array,integer_value(1),false);
    auto r=run({},integer_value(0));check(r.failure==EvalFailure::Type,"Missing array faults");
    r=run(integer_value(1),integer_value(1));check(r.failure==EvalFailure::Type,"Wrong array faults");
    auto malformed=std::make_shared<Value>();malformed->type=ValueType::Array;malformed->element_type=ValueType::Integer;malformed->elements={string_value("x")};
    check(run(malformed,integer_value(1)).failure==EvalFailure::Type,"Malformed array faults");
    auto huge=array_value({string_value(std::string(250001,'a'))});r=run(huge,string_value(std::string(250001,'A')));check(r.status==RunStatus::Limit,"String comparison work bounded");
    check(ints->elements.size()==3&&ints->elements[1]->bits==0xffffffff,"Arrays unchanged");
    std::cout<<"ArrayContains: "<<cases<<" condition cases plus shape, work and immutable input checks passed\n";
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
