#include "random_fixture.hpp"
#include <iostream>

namespace {
using namespace random_fixture;
void check(bool ok,const char* why){if(!ok)throw std::runtime_error(why);}
Bytes assigned(unsigned n,Bytes value){auto b=name(n);b.push_back(7);append(b,value);return b;}
Bytes returned(unsigned n,Bytes value){Bytes b{0x29};append(b,assigned(n,value));b.push_back(1);return b;}
ValuePtr member(ValuePtr v,unsigned n){for(auto it=v->members.rbegin();it!=v->members.rend();++it)if(it->name==n)return it->value;throw std::runtime_error("Missing host fixture member");}
Bytes branch(Bytes condition){Bytes b{0x47,0,0};append(b,condition);b.push_back(1);append(b,returned(20,integer(1)));auto otherwise=b.size();append(b,returned(20,integer(0)));jump(b,1,otherwise);return b;}
void global(Bytes& b,unsigned n,Bytes value){append(b,assigned(n,value));b.push_back(1);}
}
int main(){try{
    Bytes file;auto size=name(name_crc("GetArraySize"));append(size,param(10));append(size,param(11));size.push_back(1);script(file,1,size);
    auto indexed=name(name_crc("GetArraySize"));append(indexed,param(10));for(unsigned i=1;i<=3;++i)append(indexed,assigned(name_crc("index"+std::to_string(i)),param(10+i)));indexed.push_back(1);script(file,2,indexed);
    auto contains=name(name_crc("StructureContains"));append(contains,assigned(name_crc("Structure"),param(10)));append(contains,param(11));script(file,3,branch(contains));
    auto ref_assignment=param(10);ref_assignment.push_back(7);append(ref_assignment,integer(77));ref_assignment.push_back(1);append(ref_assignment,returned(20,param(10)));script(file,4,ref_assignment);
    Bytes array{5};append(array,integer(1));append(array,integer(2));array.push_back(6);global(file,820,array);global(file,821,name(820));global(file,825,integer(1));
    Bytes included{3};append(included,name(700));included.push_back(4);global(file,800,included);
    Bytes cycle_a{3};append(cycle_a,name(901));cycle_a.push_back(4);global(file,900,cycle_a);Bytes cycle_b{3};append(cycle_b,name(900));cycle_b.push_back(4);global(file,901,cycle_b);
    file.push_back(0);SymbolTable table;table.load_bytes("data-host-fixtures",file);Interpreter vm(table);
    check(vm.run(1).failure==EvalFailure::Missing,"Data hosts are opt-in");vm.install_data_hosts();
    auto list=array_value({integer_value(1),integer_value(2),integer_value(3)});
    auto params=structure_value({{10,list},{11,name_value(999)}});auto r=vm.run(1,params);
    check(r.status==RunStatus::Finished&&member(r.parameters,name_crc("array_size"))->bits==3,"Typed array search ignores a later unknown flag");
    check(list->elements.size()==3,"Host input array is immutable");
    r=vm.run(1,structure_value({{10,name_value(821)}}));check(r.status==RunStatus::Finished&&member(r.parameters,name_crc("array_size"))->bits==2,"Array search resolves global aliases");
    auto nested=array_value({array_value({integer_value(1)}),list});
    r=vm.run(2,structure_value({{10,nested},{11,name_value(825)}}));check(r.status==RunStatus::Finished&&member(r.parameters,name_crc("array_size"))->bits==3,"Array index may resolve a global integer");
    r=vm.run(2,structure_value({{10,nested},{12,integer_value(100)}}));check(r.status==RunStatus::Finished&&member(r.parameters,name_crc("array_size"))->bits==2,"Index2 ignored without Index1");
    auto cube=array_value({array_value({nested})});r=vm.run(2,structure_value({{10,cube},{11,integer_value(0)},{12,integer_value(0)},{13,integer_value(1)}}));
    check(r.status==RunStatus::Finished&&member(r.parameters,name_crc("array_size"))->bits==3,"All three nested indices");
    check(vm.run(2,structure_value({{10,nested},{11,integer_value(-1)}})).failure==EvalFailure::Arithmetic,"Negative array indices are bounded");
    check(vm.run(2,structure_value({{10,list},{11,integer_value(0)}})).failure==EvalFailure::Type,"Indexed element must be an array");
    check(vm.run(1).failure==EvalFailure::Type,"Missing array faults instead of original null dereference");
    auto has=[&](ValuePtr structure,unsigned name){auto result=vm.run(3,structure_value({{10,structure},{11,name_value(name)}}));check(result.status==RunStatus::Finished,result.message.c_str());return member(result.parameters,20)->bits!=0;};
    check(has(structure_value({{700,integer_value(0)}}),700),"Contains tests field presence, including zero values");
    check(has(structure_value({{0,name_value(700)}}),700),"Contains sees unnamed flags");
    check(has(structure_value({{0,name_value(800)}}),700),"Contains traverses included global structures");
    check(has(name_value(800),700),"Structure argument resolves a global name");
    check(has(structure_value({{0,name_value(820)}}),820),"Requested checksum remains raw even when it names an array");
    check(has(structure_value({{0,name_value(700)}}),800),"StructureContains skips global-structure query and tests unnamed checksum zero");
    check(!has(structure_value({{800,integer_value(0)}}),800),"Skipped structure name must not be treated as raw target checksum");
    check(!has(structure_value({}),700)&&!has(integer_value(3),700),"Absent field/structure gives false");
    r=vm.run(3,structure_value({{10,name_value(900)},{11,name_value(999)}}));check(r.failure==EvalFailure::Cycle,"Included-structure cycles are bounded");
    for(auto initial:{ValuePtr{},integer_value(1),name_value(999)}){
        r=vm.run(4,initial?structure_value({{10,initial}}):ValuePtr{});check(r.status==RunStatus::Finished&&member(r.parameters,20)->bits==77,"<param>=value assigns written checksum, not previous content");
    }
    table.clear();r=vm.run(1,structure_value({{10,name_value(821)}}));check(r.status==RunStatus::Finished,"Data host shares interpreter definition snapshot");
    std::cout<<"Data host typed lookup, arrays, flags, inclusions, snapshots and reference assignments passed\n";
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
