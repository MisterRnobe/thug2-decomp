#include "random_fixture.hpp"
#include <cmath>
#include <iostream>
#include <limits>

namespace {
using namespace random_fixture;
void check(bool ok,const char* why){if(!ok)throw std::runtime_error(why);}
Bytes range(float a,float b,bool second=false){Bytes out{static_cast<std::uint8_t>(second?0x38:0x30),0x1f};word(out,std::bit_cast<std::uint32_t>(a));word(out,std::bit_cast<std::uint32_t>(b));return out;}
Bytes assigned(unsigned n,Bytes v){auto b=name(n);b.push_back(7);append(b,v);return b;}
Bytes returned(Bytes v){Bytes b{0x29};append(b,assigned(20,v));b.push_back(1);return b;}
ValuePtr member(ValuePtr v,unsigned n){for(const auto& m:v->members)if(m.name==n)return m.value;throw std::runtime_error("Missing range fixture member");}
}
int main(){try{
    RandomSelector a,b;a.seed(123);b.seed(123);
    auto equal=a.range(2,2);check(!equal.floating&&equal.bits==2,"Equal whole bounds give integer");
    RandomStreams model(123);model.draw(1);auto next=a.range(0,32767);check(next.bits==static_cast<unsigned>(model.draw(32768)),"Equal bounds still consume one draw");
    auto second=a.range(0,32767,true),untouched=b.range(0,32767,true);check(second.bits==untouched.bits,"Range streams are independent");
    a.seed(55);b.seed(55);auto normal=a.range(-5,6),reverse=b.range(6,-5);check(normal.floating==reverse.floating&&normal.bits==reverse.bits,"Reversed whole endpoints");
    auto tolerance=a.range(1e-5f,1e-5f);check(!tolerance.floating&&tolerance.bits==0,"Whole classification includes exact tolerance");
    auto near=std::nextafter(1e-5f,std::numeric_limits<float>::infinity());auto outside=a.range(near,near);check(outside.floating&&outside.bits==std::bit_cast<std::uint32_t>(near),"Next float outside tolerance remains float");
    a.seed(3264);auto upper=a.range(-0.5f,0.5f);
    check(upper.floating&&upper.bits==std::bit_cast<std::uint32_t>(std::nextafter(0.5f,std::numeric_limits<float>::infinity())),"Original float scale permits an upper-endpoint overshoot; do not clamp");
    auto fixed=a.range(0.5f,0.5f);check(fixed.floating&&fixed.bits==std::bit_cast<std::uint32_t>(0.5f),"Equal fractional bounds preserve value/type");
    auto extreme=a.range(-std::numeric_limits<float>::max(),std::numeric_limits<float>::max());check(extreme.floating&&std::isfinite(std::bit_cast<float>(extreme.bits)),"Large finite bounds keep finite x87 interpolation");
    a.range(-2147483648.0f,2147483520.0f); // UBSan: preserve wrapped width and sum without signed overflow.
    a.seed(77);b.seed(77);bool rejected=false;
    try{a.range(std::numeric_limits<float>::infinity(),0);}catch(const EvalError& e){rejected=e.kind==EvalFailure::Arithmetic;}
    check(rejected&&a.range(0,32767).bits==b.range(0,32767).bits,"Nonfinite bounds fault before consuming RNG");
    a.seed(88);b.seed(88);bool limited=false;
    try{a.range(1,3,false,[](std::size_t){throw EvalError(EvalFailure::Limit,"fuel");});}catch(const EvalError&){limited=true;}
    check(limited&&a.range(0,32767).bits==b.range(0,32767).bits,"Range charges fuel before drawing");

    Bytes file;script(file,1,returned(range(1,6)));script(file,2,returned(range(1,6,true)));
    auto assignment=assigned(10,range(-2.5f,2.5f));assignment.push_back(1);append(assignment,returned(param(10)));script(file,3,assignment);
    Bytes expression{0x0e};append(expression,range(1.5f,1.5f));expression.push_back(11);append(expression,integer(2));expression.push_back(0x0f);script(file,4,returned(expression));
    file.push_back(0x23);append(file,name(5));append(file,assigned(10,range(1,6)));file.push_back(1);append(file,returned(param(10)));file.insert(file.end(),{0x24,1});
    Bytes nested{5,3};append(nested,assigned(50,range(1,6)));nested.insert(nested.end(),{4,6});script(file,6,returned(nested));
    Bytes direct{5};append(direct,range(1,6));direct.push_back(6);script(file,7,returned(direct));
    Bytes wrong{0x30};append(wrong,integer(1));script(file,8,returned(wrong));
    Bytes range2_expression{0x0e};append(range2_expression,range(1,6,true));range2_expression.push_back(0x0f);script(file,9,returned(range2_expression));
    auto alternatives=random_block(0x2f,{0,1},{range(99,99),range(0.5f,0.5f,true)});alternatives.pop_back();script(file,10,returned(alternatives));
    file.push_back(0);SymbolTable table;table.load_bytes("range-fixtures",file);Interpreter vm(table);
    for(auto seed:{0u,1u,123u,0xffffffffu}){
        RandomSelector expected;expected.seed(seed);auto first=expected.range(1,6);
        vm.seed_random(seed);auto r=vm.run(1);check(r.status==RunStatus::Finished&&member(r.parameters,20)->bits==first.bits,"Range in return arguments");
        vm.seed_random(seed);r=vm.run(2);check(r.status==RunStatus::Finished&&member(r.parameters,20)->bits==first.bits,"Range2 has independently seeded second stream");
        expected.seed(seed);auto fraction=expected.range(-2.5f,2.5f);vm.seed_random(seed);r=vm.run(3);check(r.status==RunStatus::Finished&&member(r.parameters,20)->bits==fraction.bits,"Range in assignment");
        vm.seed_random(seed);r=vm.run(5,structure_value({{10,integer_value(99)}}));check(r.status==RunStatus::Finished&&member(r.parameters,20)->bits==99,"Entry inputs override random defaults");
        vm.seed_random(seed);r=vm.run(6);check(r.status==RunStatus::Finished&&member(member(r.parameters,20)->elements[0],50)->bits==first.bits,"Array sizing does not evaluate ranges in structure fields");
    }
    auto r=vm.run(4);check(r.status==RunStatus::Finished&&member(r.parameters,20)->type==ValueType::Float&&member(r.parameters,20)->bits==std::bit_cast<std::uint32_t>(3.5f),"Range in expression");
    check(vm.run(7).failure==EvalFailure::Unsupported,"Range itself is not a literal array element in original ParseArray");
    check(vm.run(8).failure==EvalFailure::Syntax,"Pair operand required");check(vm.run(9).failure==EvalFailure::Unsupported,"Original expression parser's Range2 limitation is explicit");
    r=vm.run(10);check(r.status==RunStatus::Finished&&member(r.parameters,20)->bits==std::bit_cast<std::uint32_t>(0.5f),"Only selected random branch evaluates its range");
    auto raw=range(1,6);raw.push_back(0);Document doc(raw);ValueEvaluator static_reader(table);bool static_rejected=false;
    try{static_reader.read(doc,0,raw.size()-1);}catch(const EvalError& e){static_rejected=e.kind==EvalFailure::Unsupported;}check(static_rejected,"Static parsing never consumes runtime ranges");
    std::cout<<"RandomRange types, tolerance, streams, extreme bounds, runtime contexts and limits passed\n";
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
