#include "switch_fixture.hpp"
#include <iostream>
#include <limits>

namespace {
using namespace switch_fixture;
void check(bool ok,const char* why){if(!ok)throw std::runtime_error(why);}
Bytes text(std::string_view s,bool local=false){Bytes b{static_cast<std::uint8_t>(local?0x1c:0x1b)};word(b,s.size()+1);b.insert(b.end(),s.begin(),s.end());b.push_back(0);return b;}
}
int main(){try{
    Bytes file;
    auto selector=param(10);
    script(file,1,block(selector,{{integer(1),command(101)},{integer(2),command(102)}},command(103)));
    script(file,2,block(selector,{{integer(1),{}},{integer(2),{}},{integer(3),command(101)}},command(103)));
    script(file,3,block(selector,{{integer(1),command(101)}},{},false));
    // Nested switch in an untaken pre-case region is skipped as a whole.
    auto nested=block(integer(9),{{integer(9),command(199)}},command(199));
    auto outer=Bytes{0x3c};append(outer,integer(2));outer.push_back(1);append(outer,nested);
    auto cases=block(integer(2),{{integer(1),command(199)},{integer(2),command(102)}},command(103));
    append(outer,Bytes(cases.begin()+7,cases.end()));script(file,4,outer);
    script(file,5,block(integer(1),{{integer(1),block(integer(2),{{integer(2),command(102)}},command(199))}},command(199)));
    script(file,6,block(text("hello"),{{text("world"),command(199)},{text("hello"),command(101)}},command(103)));
    script(file,7,block(text("hello",true),{{text("hello"),command(199)}},command(103)));
    script(file,8,block(floating(std::numeric_limits<float>::quiet_NaN()),{{floating(std::numeric_limits<float>::quiet_NaN()),command(199)}},command(103)));
    script(file,9,block(floating(-0.0f),{{integer(0),command(101)}},command(103)));
    script(file,10,block(integer(16777217),{{floating(16777216.0f),command(199)}},command(103)));
    script(file,11,block(name(900),{{integer(2),command(102)}},command(199)));
    append(file,name(900));file.push_back(7);append(file,integer(2));file.push_back(1);
    script(file,12,block(name(999),{{name(999),command(101)}},command(199)));
    script(file,13,block({},{{integer(0),command(199)}},command(103)));
    auto multiple=integer(1);append(multiple,name(777));multiple.push_back(7);append(multiple,integer(2));
    script(file,14,block(multiple,{{integer(1),command(101)}},command(199)));
    auto wait=command(name_crc("wait"));wait.pop_back();append(wait,integer(1));append(wait,name(name_crc("gameframes")));wait.push_back(1);append(wait,command(101));
    script(file,15,block(integer(1),{{integer(1),wait}},command(199)));
    // Parse grouped labels even when a preceding label already matched.
    auto bad_label=Bytes{0x42};script(file,16,block(integer(1),{{integer(1),{}},{bad_label,command(101)}},command(199)));
    Bytes unterminated{0x3c};append(unterminated,integer(1));unterminated.push_back(1);script(file,17,unterminated);
    script(file,18,block(integer(1),{{integer(1),command(199)}},command(199)));
    script(file,19,Bytes{0x3e,1});script(file,20,Bytes{0x3c,1,0x3e,1,0x3d,1});
    // A loop and QB call in a case must not need an extra switch stack.
    Bytes called;append(called,command(1));script(file,21,block(integer(2),{{integer(2),called}},command(199)));
    auto coordinates=[](std::uint8_t token,std::initializer_list<float> values){Bytes b{token};for(float v:values)word(b,std::bit_cast<std::uint32_t>(v));return b;};
    script(file,22,block(coordinates(0x1f,{1,2}),{{coordinates(0x1f,{1,2}),command(101)}},command(199)));
    script(file,23,block(coordinates(0x1e,{1,2,3}),{{coordinates(0x1e,{1,2,4}),command(199)},{coordinates(0x1e,{1,2,3}),command(102)}},command(199)));
    script(file,24,block(Bytes{5,0x17,1,0,0,0,6},{{Bytes{5,0x17,1,0,0,0,6},command(101)}},command(199)));
    Bytes deep{0x3c};append(deep,integer(2));deep.push_back(1);
    for(unsigned i=0;i<2000;++i)deep.insert(deep.end(),{0x3c,1});
    for(unsigned i=0;i<2000;++i)deep.insert(deep.end(),{0x3d,1});
    append(deep,Bytes(cases.begin()+7,cases.end()));script(file,25,deep);
    auto missing_case=block(integer(1),{{param(777),command(199)}},command(103));script(file,26,missing_case);
    auto malformed_default=block(integer(1),{},command(101));malformed_default[8]=0x47;script(file,27,malformed_default);
    file.push_back(0);SymbolTable symbols;symbols.load_bytes("switch-fixtures",file);Interpreter vm(symbols);
    std::vector<unsigned> events;
    for(unsigned n:{101,102,103,199})vm.bind(n,[&,n](const HostCall&){events.push_back(n);return HostResult{};});
    auto expect=[&](unsigned script,ValuePtr params,std::vector<unsigned> expected){events.clear();auto r=vm.run(script,params);check(r.status==RunStatus::Finished,r.message.c_str());check(events==expected,"Switch dispatch events differ");};
    expect(1,structure_value({{10,integer_value(1)}}),{101});expect(1,structure_value({{10,integer_value(2)}}),{102});
    expect(1,structure_value({{10,integer_value(4)}}),{103});expect(1,{}, {103});
    for(int i=1;i<=3;++i)expect(2,structure_value({{10,integer_value(i)}}),{101});
    expect(3,structure_value({{10,integer_value(2)}}),{});
    expect(4,{}, {102});expect(5,{}, {102});expect(6,{}, {101});expect(7,{}, {103});expect(8,{}, {103});
    expect(9,{}, {101});expect(10,{}, {103});expect(11,{}, {102});expect(12,{}, {101});expect(13,{}, {103});expect(14,{}, {101});
    vm.install_wait();auto task=vm.start(15);events.clear();check(vm.update(task,{0,1.0/60,0}).status==RunStatus::Waiting&&events.empty(),"Case yields at wait");
    check(vm.update(task,{1,1.0/60,16}).status==RunStatus::Finished&&events==std::vector<unsigned>{101},"Case resumes after wait");
    check(vm.run(16).failure==EvalFailure::Unsupported,"Matched grouped labels still parse arguments");
    check(vm.run(17).failure==EvalFailure::Syntax,"Missing endswitch must fault instead of original infinite scan");
    check(vm.run(19).failure==EvalFailure::Syntax&&vm.run(20).failure==EvalFailure::Syntax,"Orphan/malformed case rejected");
    events.clear();RunLimits limited;limited.fuel=20;check(vm.run(18,{},limited).status==RunStatus::Limit&&events.empty(),"Switch scan consumes fuel");
    expect(22,{}, {101});expect(23,{}, {102});
    check(vm.run(24).failure==EvalFailure::Unsupported,"Array identity remains explicit");
    expect(25,{}, {102});expect(26,{}, {103});
    check(vm.run(27).failure==EvalFailure::Syntax,"Default requires compiled jump header");
    expect(21,structure_value({{10,integer_value(2)}}),{103}); // Call has its own empty parameters.
    std::cout<<"Switch cases/defaults, grouped labels, nested scans, equality, calls, waits and bounds passed\n";
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
