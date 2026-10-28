#include "switch_fixture.hpp"
#include <algorithm>
#include <iostream>
#include <set>

namespace {
using namespace switch_fixture;
void check(bool ok,const char* why){if(!ok)throw std::runtime_error(why);}
template<class F> void rejects(F fn,const char* why){bool caught=false;try{fn();}catch(const EvalError&){caught=true;}check(caught,why);}
Bytes random_block(std::uint8_t opcode,std::vector<std::uint16_t> weights,const std::vector<Bytes>& branches){
    Bytes b{opcode};word(b,weights.size());for(auto w:weights){b.push_back(w&255);b.push_back(w>>8);}
    auto offsets=b.size();for(std::size_t i=0;i<weights.size();++i)word(b,0);
    std::vector<std::size_t> ends;
    for(std::size_t i=0;i<branches.size();++i){
        auto field=offsets+4*i;auto delta=b.size()-(field+4);for(unsigned j=0;j<4;++j)b[field+j]=delta>>(8*j);
        append(b,branches[i]);b.push_back(0x2e);ends.push_back(b.size());word(b,0);
    }
    b.push_back(1);auto end=b.size()-1;
    for(auto field:ends){auto delta=end-(field+4);for(unsigned j=0;j<4;++j)b[field+j]=delta>>(8*j);}
    return b;
}
}
int main(){try{
    RandomStreams a(123),b(123);for(int i=0;i<100;++i)check(a.draw(100)==b.draw(100),"Seed reproduces first stream");
    RandomStreams separate(123),baseline(123);separate.draw(100);check(separate.draw(100,true)==baseline.draw(100,true),"Streams independent");
    RandomSelector selector;selector.seed(55);std::array<std::uint16_t,3> weights{0,1,0};
    for(int i=0;i<100;++i)check(selector.choose(0x2f,1,0,weights,1)==1,"Zero-weight alternatives not selected for nonoverflowing total");
    std::array<std::uint16_t,2> two{1,1};std::size_t previous=2;
    for(int i=0;i<100;++i){auto pick=selector.choose(0x40,1,1,two,1);check(pick<2&&pick!=previous,"NoRepeat consecutive picks");previous=pick;}
    std::array<std::uint16_t,1> one{0};unsigned draws=0;
    check(selector.choose(0x40,1,2,one,1,[&](std::size_t n){draws+=static_cast<unsigned>(n);})==0&&draws>=102,"Single no-repeat choice reaches bounded retry fallback");
    RandomSelector permute;permute.seed(123);std::vector<std::size_t> picks;
    for(int i=0;i<20;++i)picks.push_back(permute.choose(0x41,1,0,weights,1));
    for(std::size_t i=2;i+3<=picks.size();i+=3){auto cycle=std::vector<std::size_t>(picks.begin()+i,picks.begin()+i+3);std::sort(cycle.begin(),cycle.end());check(cycle==std::vector<std::size_t>{0,1,2},"Complete permutation cycles");check(picks[i]!=picks[i-1],"Permutation boundary avoids last choice");}
    rejects([&]{selector.choose(0x2f,1,0,{},1);},"Empty table rejected");
    rejects([&]{selector.choose(0x2f,1,0,one,1);},"Zero total rejected");
    std::array<std::uint16_t,1> negative{0xffff};rejects([&]{selector.choose(0x2f,1,0,negative,1);},"Negative weights rejected");
    rejects([&]{selector.choose(0x41,1,0,one,1);},"Unsafe single-element permutation rejected");
    std::vector<std::uint16_t> large(67,1);rejects([&]{selector.choose(0x41,1,0,large,1);},"Original 66-entry permutation bound");
    RandomSelector owners;owners.choose(0x40,1,0,two,1);owners.choose(0x40,1,0,two,2);check(owners.stored_count()==2,"Concurrent owners have separate histories");
    owners.forget(1);owners.choose(0x40,1,0,two,3);check(owners.stored_count()==2,"Released history can be reused");
    for(std::size_t i=1;i<110;++i)owners.choose(0x40,1,i,two,3);check(owners.stored_count()==100,"Stored list capped at 100");
    RandomSelector interrupted;bool failed=false;unsigned charges=0;
    try{interrupted.choose(0x41,1,0,weights,1,[&](std::size_t){if(++charges>1)throw std::runtime_error("fuel");});}catch(const std::runtime_error&){failed=true;}
    check(failed&&interrupted.stored_count()==0,"Aborted initialization leaves no invalid record");

    Bytes file;for(auto opcode:{0x2f,0x37,0x40,0x41})script(file,opcode,random_block(static_cast<std::uint8_t>(opcode),{1,1,1},{command(101),command(102),command(103)}));
    Bytes loop{0x20,1};append(loop,random_block(0x40,{1,1},{command(101),command(102)}));loop.push_back(0x21);append(loop,integer(8));loop.push_back(1);script(file,1,loop);
    auto wait=command(name_crc("wait"));wait.pop_back();append(wait,integer(1));append(wait,name(name_crc("gameframes")));wait.push_back(1);
    Bytes scheduled{0x20,1};append(scheduled,random_block(0x41,{1,1,1},{command(101),command(102),command(103)}));append(scheduled,wait);scheduled.push_back(0x21);append(scheduled,integer(6));scheduled.push_back(1);script(file,2,scheduled);
    script(file,3,random_block(0x2f,{0,0},{command(101),command(102)}));
    Bytes cycle{0x2e};word(cycle,0xfffffffb);script(file,4,cycle);
    file.push_back(0);SymbolTable table;table.load_bytes("random-fixtures",file);Interpreter vm(table);vm.seed_random(321);
    std::vector<unsigned> events;for(unsigned n:{101,102,103})vm.bind(n,[&,n](const HostCall&){events.push_back(n);return HostResult{};});
    for(unsigned opcode:{0x2f,0x37,0x40,0x41}){events.clear();check(vm.run(opcode).status==RunStatus::Finished&&events.size()==1,"Random statement dispatches one branch");}
    events.clear();check(vm.run(1).status==RunStatus::Finished&&events.size()==8,"Random inside repeat");
    for(std::size_t i=1;i<events.size();++i)check(events[i]!=events[i-1],"Repeated site keeps no-repeat history");
    vm.install_wait();auto task=vm.start(2);events.clear();
    for(std::uint64_t frame=0;frame<7;++frame){auto r=vm.update(task,{frame,1.0/60,static_cast<std::uint32_t>(frame*16)});check(r.status==(frame<6?RunStatus::Waiting:RunStatus::Finished),"Permutation resumes through waits");}
    check(events.size()==6,"One random branch per resumed loop iteration");
    check(std::set<unsigned>(events.begin()+2,events.begin()+5).size()==3,"Permutation state survives wait");
    check(vm.run(3).failure==EvalFailure::Syntax,"Malformed weighted runtime table faults");
    RunLimits fuel;fuel.fuel=100;check(vm.run(4,{},fuel).status==RunStatus::Limit,"Cyclic jumps consume fuel");
    Interpreter clone(table);clone.seed_random(777);vm.seed_random(777);std::vector<unsigned> other;
    for(unsigned n:{101,102,103})clone.bind(n,[&,n](const HostCall&){other.push_back(n);return HostResult{};});
    events.clear();for(int i=0;i<20;++i){vm.run(0x2f);clone.run(0x2f);}check(events==other,"Interpreter seed reproduces weighted execution");
    std::cout<<"QB random streams, weighted/no-repeat/permuted choices, task histories and bounds passed\n";
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
