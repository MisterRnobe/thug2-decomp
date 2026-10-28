#include "thug2/values.hpp"
#include <bit>
#include <iostream>
#include <stdexcept>
#include <functional>

namespace {
using namespace thug2;using namespace thug2::qb;
void check(bool ok,const char* why){if(!ok)throw std::runtime_error(why);}
void rejects(EvalFailure k,const std::function<void()>& f){bool caught=false;try{f();}catch(const EvalError& e){caught=e.kind==k;}check(caught,"Expected evaluation failure");}
void word(Bytes& b,std::uint32_t n){for(unsigned i=0;i<4;++i)b.push_back(n>>(i*8));}
void number(Bytes& b,std::int32_t n){b.push_back(0x17);word(b,std::bit_cast<std::uint32_t>(n));}
void name(Bytes& b,std::uint32_t n){b.push_back(0x16);word(b,n);}
Bytes definition(std::uint32_t n,Bytes value){Bytes b;name(b,n);b.push_back(7);b.insert(b.end(),value.begin(),value.end());b.insert(b.end(),{1,0});return b;}
ValuePtr expression(Bytes code){SymbolTable t;code.insert(code.begin(),0x0e);code.push_back(0x0f);t.load_bytes("expr",definition(1,code));ValueEvaluator e(t);return e.global(1);}
}
int main(){
 try{
    check(binary_value(11,integer_value(INT32_MAX),integer_value(1))->bits==0x80000000,"Integer wrap");
    check(binary_value(51,integer_value(2),integer_value(1))->bits==0,"AND is bitwise");
    check(binary_value(50,integer_value(2),integer_value(1))->bits==3,"OR is bitwise");
    check(binary_value(18,string_value("zzz"),string_value("aaaa"))->bits==1,"String length comparison");
    check(binary_value(11,name_value(0xffffffff),integer_value(1))->bits==0,"Name addition");
    check(binary_value(10,integer_value(std::bit_cast<std::int32_t>(0xfb83fcc4u)),
        float_value(std::bit_cast<float>(0xaf1c9eeeu)))->bits==0xcc8f8067u,"x87 low bits must survive binary32 tie rounding");
    rejects(EvalFailure::Arithmetic,[]{binary_value(12,integer_value(INT32_MIN),integer_value(-1));});
    rejects(EvalFailure::Arithmetic,[]{binary_value(12,integer_value(1),float_value(1e-8f));});
    rejects(EvalFailure::Arithmetic,[]{binary_value(11,string_value(std::string(100,'x')),string_value(std::string(100,'y')));});
    auto array=array_value({integer_value(2),float_value(3)});
    check(array->element_type==ValueType::Float&&std::bit_cast<float>(array->elements[0]->bits)==2,"Numeric array promotion");
    rejects(EvalFailure::Type,[]{array_value({integer_value(1),name_value(2)});});
    rejects(EvalFailure::Arithmetic,[&]{binary_value(5,array,integer_value(-1));});
    auto structure=structure_value({{7,integer_value(1)},{7,float_value(2)},{0,name_value(8)},{0,name_value(9)}});
    check(structure->members.size()==3&&structure->members[0].value->type==ValueType::Float,"Component replacement and flags");
    Bytes code;number(code,8);code.push_back(12);number(code,2);code.push_back(13);number(code,2);
    check(expression(code)->bits==2,"Multiply binds more tightly than division");
    code.clear();number(code,10);number(code,-2);code.push_back(13);number(code,3);
    check(expression(code)->bits==4,"Implicit negative-literal subtraction");
    code.clear();number(code,1);code.push_back(11);number(code,2);code.push_back(13);number(code,3);
    check(expression(code)->bits==7,"Arithmetic precedence");
    SymbolTable t;
    Bytes a{5};number(a,10);number(a,20);a.push_back(6);t.load_bytes("array",definition(2,a));
    code={0x0e};name(code,2);code.push_back(5);number(code,1);code.insert(code.end(),{6,0x0f});t.load_bytes("index",definition(3,code));
    ValueEvaluator evaluator(t);check(evaluator.global(3)->bits==20,"Global array indexing");
    Bytes object{3};name(object,99);object.push_back(7);number(object,7);object.push_back(4);
    t.load_bytes("object",definition(4,object));
    Bytes ninety_nine;number(ninety_nine,100);t.load_bytes("selector-global",definition(99,ninety_nine));
    code={0x0e};name(code,4);code.push_back(8);name(code,99);code.push_back(0x0f);t.load_bytes("dot",definition(5,code));
    ValueEvaluator dot(t);check(dot.global(5)->bits==7,"Dot selector must not resolve as a global");
    check(dot.member(dot.global(4),99)->bits==7,"Typed structure lookup");
    Bytes script{0x23};name(script,7);script.insert(script.end(),{1,0x24,0});t.load_bytes("flag-script",script);
    ValueEvaluator flags(t);
    auto with_flag=structure_value({{0,name_value(7)},{99,integer_value(4)}});
    check(flags.member(with_flag,99)->bits==4,"Script-valued flags must not trigger calls during member lookup");
    Bytes one;number(one,1);t.load_bytes("first-number",definition(110,one));t.load_bytes("second-number",definition(111,one));
    Bytes left{3};name(left,99);left.push_back(7);name(left,110);left.push_back(4);
    Bytes right{3};name(right,99);right.push_back(7);name(right,111);right.push_back(4);
    t.load_bytes("left",definition(112,left));t.load_bytes("right",definition(113,right));
    code={0x0e};name(code,112);code.push_back(18);name(code,113);code.push_back(0x0f);t.load_bytes("subset",definition(114,code));
    ValueEvaluator subset(t);check(subset.global(114)->bits==1,"Structure subset comparisons resolve member names");
    // Identical literal data through the serialized-image adapter.
    Bytes image{0x6f,0xef,0x75,0xb0,64,0,0,0,0x1c,8,2,4,0x10,4,8,0xc,0xc,8,2,4,0x14,2,4,0xc,0x10,0x10,0xc,0};
    word(image,6<<16);word(image,6);word(image,0);word(image,48);word(image,0);
    word(image,0);word(image,0x3f800000);word(image,0x40000000);word(image,0x40400000);
    t.load_bytes("image",image);ValueEvaluator adapted(t);
    check(format_value(adapted.global(6))=="vector(1, 2, 3)","Precompiled value adapter");
    t.load_bytes("replace",definition(2,Bytes{5,6}));check(evaluator.global(2)->elements.size()==2,"Value snapshot changed");
    rejects(EvalFailure::Missing,[&]{evaluator.global(999);});
    SymbolTable cycle;Bytes reference;name(reference,8);cycle.load_bytes("a",definition(8,reference));ValueEvaluator ce(cycle);
    rejects(EvalFailure::Cycle,[&]{ce.global(8);});
    SymbolTable runtime;runtime.load_bytes("r",definition(1,Bytes{0x30,0x1f,0,0,0,0,0,0,0,0}));ValueEvaluator re(runtime);
    rejects(EvalFailure::Unsupported,[&]{re.global(1);});
    ValueEvaluator limited(t,{1,100,100});rejects(EvalFailure::Limit,[&]{limited.global(2);});
    ValueEvaluator nodes(t,{128,0,100});rejects(EvalFailure::Limit,[&]{nodes.global(2);});
    SymbolTable growing;Bytes seed{5};number(seed,1);seed.push_back(6);growing.load_bytes("seed",definition(200,seed));
    for(unsigned i=1;i<=20;++i){Bytes doubled{0x0e};name(doubled,199+i);doubled.push_back(11);name(doubled,199+i);doubled.push_back(0x0f);
        growing.load_bytes("double",definition(200+i,doubled));}
    ValueEvaluator bounded(growing,{128,64,10000});rejects(EvalFailure::Limit,[&]{bounded.global(220);});
    check(format_value(array)=="[2, 3]","Value formatting");
    std::cout<<"QB values, snapshots, arithmetic, precedence, arrays and bounded failures passed\n";
 }catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}
