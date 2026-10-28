#include "random_fixture.hpp"
#include <iostream>

namespace {
using namespace random_fixture;
void check(bool ok,const char* why){if(!ok)throw std::runtime_error(why);}
Bytes assigned(unsigned n,Bytes value){auto b=name(n);b.push_back(7);append(b,value);return b;}
ValuePtr member(ValuePtr v,unsigned n){for(auto it=v->members.rbegin();it!=v->members.rend();++it)if(it->name==n)return it->value;return {};}
}
int main(){try{
    Bytes file;
    for(unsigned dimensions=1;dimensions<=3;++dimensions){
        auto b=name(name_crc(dimensions==1?"GetNDArrayData":dimensions==2?"Get2DArrayData":"Get3DArrayData"));
        append(b,assigned(name_crc("arrayName"),param(10)));
        for(unsigned i=1;i<=dimensions&&dimensions!=1;++i)append(b,assigned(name_crc("index"+std::to_string(i)),param(10+i)));
        // An indices command argument must not override the current frame's indices.
        append(b,assigned(name_crc("indices"),param(14)));b.push_back(1);script(file,dimensions,b);
    }
    auto b=name(name_crc("Get2DArrayData"));b.push_back(1);script(file,4,b);
    Bytes row{5};append(row,integer(99));row.push_back(6);Bytes matrix{5};append(matrix,row);matrix.push_back(6);
    append(file,assigned(800,matrix));file.push_back(1);append(file,assigned(801,name(800)));file.push_back(1);
    append(file,assigned(802,integer(0)));file.push_back(1);file.push_back(0);
    SymbolTable table;table.load_bytes("array-data-fixtures",file);Interpreter vm(table);
    check(vm.run(1).failure==EvalFailure::Missing,"Array commands opt in");vm.install_data_hosts();
    const auto val=name_crc("val"),indices=name_crc("indices");
    auto run=[&](unsigned dimensions,ValuePtr array,ValuePtr path,ValuePtr i={},ValuePtr j={},ValuePtr k={}){
        std::vector<Member> p{{10,name_value(700)},{700,array},{val,integer_value(-7)}};
        if(path)p.push_back({indices,path});if(i)p.push_back({11,i});if(j)p.push_back({12,j});if(k)p.push_back({13,k});
        return vm.run(dimensions,structure_value(std::move(p)));
    };
    std::vector<ValuePtr> leaves{integer_value(-23),float_value(1.25f),name_value(123),string_value("plain"),string_value("local",true),structure_value({{42,integer_value(8)}}),array_value({integer_value(9)})};
    for(auto kind:{ValueType::Pair,ValueType::Vector}){auto v=std::make_shared<Value>();v->type=kind;v->coordinates={0x3f800000,0xc0000000,0x40400000};leaves.push_back(v);}
    unsigned cases=0;
    for(auto leaf:leaves){
        auto row_value=array_value({leaf});auto matrix_value=array_value({row_value});auto cube=array_value({matrix_value});
        for(unsigned d=1;d<=3;++d){
            auto r=run(d,d==1?row_value:d==2?matrix_value:cube,array_value({integer_value(0)}));
            check(r.status==RunStatus::Finished,r.message.c_str());auto out=member(r.parameters,val);
            check(out&&out->type==(leaf->type==ValueType::LocalString?ValueType::String:leaf->type),"Output type preserved except LocalString normalization");
            check(out->bits==leaf->bits&&out->text==leaf->text&&out->coordinates==leaf->coordinates&&out->elements==leaf->elements&&out->members.size()==leaf->members.size(),"Output payload preserved");
            check(row_value->elements[0]==leaf,"Input remains immutable");++cases;
        }
    }
    auto list=array_value({integer_value(30),integer_value(40)});auto mat=array_value({list});auto cube=array_value({mat});
    auto r=run(2,mat,{},name_value(802),integer_value(1));check(r.status==RunStatus::Finished&&member(r.parameters,val)->bits==40,"Indices resolve global integers");
    r=run(3,cube,{},integer_value(0),integer_value(0),integer_value(1));check(r.status==RunStatus::Finished&&member(r.parameters,val)->bits==40,"3D path selects final element");
    for(auto zero:{integer_value(0),float_value(0),name_value(0)}){r=run(1,mat,array_value({zero,zero}));check(r.status==RunStatus::Finished&&member(r.parameters,val)->bits==30,"ND consumes raw scalar words");}
    r=run(1,list,array_value({integer_value(1)}));check(r.status==RunStatus::Finished&&member(r.parameters,val)->bits==40,"ND reads frame indices");
    r=vm.run(1,structure_value({{10,name_value(700)},{700,list},{14,array_value({integer_value(0)})}}));check(r.failure==EvalFailure::Type,"ND ignores command indices");
    for(auto local:{ValuePtr{},integer_value(1),name_value(801),mat}){
        std::vector<Member> p{{10,name_value(800)}};if(local)p.push_back({800,local});r=vm.run(2,structure_value(p));
        check(r.status==RunStatus::Finished&&member(r.parameters,val)->bits==(local==mat?30u:99u),"Local typed array overrides global; wrong types fall back; aliases resolve");
    }
    r=vm.run(4,structure_value({{0,mat}}));check(r.status==RunStatus::Finished&&member(r.parameters,val)->bits==30,"Missing arrayName and indices default to zero");
    for(auto path:{ValuePtr{},array_value({}),array_value({string_value("bad")})})check(run(1,list,path).failure==EvalFailure::Type,"ND requires nonempty scalar indices");
    check(run(1,list,array_value({float_value(1)})).failure==EvalFailure::Arithmetic,"ND does not numerically convert float indices");
    check(run(2,mat,{},integer_value(-1)).failure==EvalFailure::Arithmetic,"Singleton arrays still enforce bounds");
    check(run(2,mat,{},integer_value(0),integer_value(2)).failure==EvalFailure::Arithmetic,"Leaf bounds enforced");
    check(run(2,list,{}).failure==EvalFailure::Type,"Intermediate element must be array");
    check(vm.run(2,structure_value({{10,name_value(999)}})).failure==EvalFailure::Type,"Missing array faults");
    auto opaque=std::make_shared<Value>();opaque->type=ValueType::Script;
    for(auto empty:{array_value({}),array_value({opaque})}){r=run(2,array_value({empty}),{},integer_value(0),integer_value(-1));check(r.status==RunStatus::Finished&&member(r.parameters,val)->bits==std::uint32_t(-7),"Unsupported leaf returns true without replacing val or reading index");}
    auto malformed=std::make_shared<Value>();malformed->type=ValueType::Array;malformed->element_type=ValueType::Integer;malformed->elements={string_value("bad")};
    check(run(1,malformed,array_value({integer_value(0)})).failure==EvalFailure::Type,"Malformed external arrays fault");
    RunLimits limits;limits.fuel=5;r=vm.run(2,structure_value({{10,name_value(800)}}),limits);check(r.status==RunStatus::Limit,"Host lookup charged to fuel");
    std::cout<<"Array data: "<<cases<<" typed dimension cases plus lookup, path, default, bounds and immutable output checks passed\n";
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
