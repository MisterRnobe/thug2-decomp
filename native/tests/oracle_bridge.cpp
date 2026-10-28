// Test-only C ABI for the optional Python/x86 differential check.
#include "thug2/core.hpp"
#include "thug2/qb.hpp"
#include "thug2/precompiled.hpp"
#include "thug2/values.hpp"
#include "thug2/runtime.hpp"
#include "random_fixture.hpp"
#include <algorithm>
#include <bit>
#include <limits>
#if defined(_WIN32)
#define EXPORT __declspec(dllexport)
#else
#define EXPORT
#endif
extern "C" {
EXPORT void* decoder_create() { return new thug2::PreDecoder; }
EXPORT void decoder_destroy(void* decoder) { delete static_cast<thug2::PreDecoder*>(decoder); }
EXPORT int decoder_run(void* decoder, const std::uint8_t* data, std::size_t size,
                       std::uint8_t* output, std::size_t expected) {
    try {
        auto bytes = static_cast<thug2::PreDecoder*>(decoder)->decode({data, size}, expected);
        std::copy(bytes.begin(), bytes.end(), output);
        return 0;
    } catch (...) { return 1; }
}
EXPORT std::uint32_t checksum_name(const char* name) { return thug2::name_crc(name); }
EXPORT std::size_t qb_offsets(const std::uint8_t* data, std::size_t size,
                              std::uint64_t* output, std::size_t capacity) {
    try {
        thug2::qb::Document doc(thug2::Bytes(data, data+size));
        if (capacity < doc.tokens().size()) return std::numeric_limits<std::size_t>::max();
        for (std::size_t i = 0; i < doc.tokens().size(); ++i) output[i] = doc.tokens()[i].offset;
        return doc.tokens().size();
    } catch (...) { return std::numeric_limits<std::size_t>::max(); }
}
EXPORT std::size_t qb_script_info(const std::uint8_t* data, std::size_t size,
                                  std::uint64_t* output, std::size_t capacity) {
    try {
        thug2::qb::Document doc(thug2::Bytes(data, data+size));
        if (capacity/3 < doc.scripts().size()) return std::numeric_limits<std::size_t>::max();
        for (std::size_t i = 0; i < doc.scripts().size(); ++i) {
            const auto& s = doc.scripts()[i];
            output[i*3] = s.body_offset;
            output[i*3+1] = s.end_offset;
            output[i*3+2] = s.checksum;
        }
        return doc.scripts().size();
    } catch (...) { return std::numeric_limits<std::size_t>::max(); }
}
EXPORT std::size_t image_refs(const std::uint8_t* data, std::size_t size,
                              std::uint64_t* output, std::size_t capacity) {
    try {
        thug2::qb::PrecompiledImage image(thug2::Bytes(data, data+size));
        if (capacity/2 < image.references().size()) return std::numeric_limits<std::size_t>::max();
        for (std::size_t i=0;i<image.references().size();++i) {
            output[i*2]=image.references()[i].field; output[i*2+1]=image.references()[i].target;
        }
        return image.references().size();
    } catch (...) { return std::numeric_limits<std::size_t>::max(); }
}
EXPORT std::size_t image_symbols(const std::uint8_t* data, std::size_t size,
                                 std::uint64_t* output, std::size_t capacity) {
    try {
        thug2::qb::PrecompiledImage image(thug2::Bytes(data, data+size));
        if (capacity/4 < image.symbols().size()) return std::numeric_limits<std::size_t>::max();
        for (std::size_t i=0;i<image.symbols().size();++i) {
            const auto& s=image.symbols()[i];
            output[i*4]=s.offset; output[i*4+1]=s.end_offset; output[i*4+2]=s.name;
            output[i*4+3]=static_cast<unsigned>(image.values()[s.value].type);
        }
        return image.symbols().size();
    } catch (...) { return std::numeric_limits<std::size_t>::max(); }
}
EXPORT std::size_t image_script_code(const std::uint8_t* data, std::size_t size,
                                     std::size_t index, std::uint8_t* output, std::size_t capacity) {
    try {
        thug2::qb::PrecompiledImage image(thug2::Bytes(data, data+size));
        const auto& code=image.scripts().at(index).code;
        if (capacity < code.size()) return std::numeric_limits<std::size_t>::max();
        std::copy(code.begin(),code.end(),output); return code.size();
    } catch (...) { return std::numeric_limits<std::size_t>::max(); }
}
EXPORT int value_binary(unsigned op,unsigned type_a,std::uint32_t bits_a,
                         unsigned type_b,std::uint32_t bits_b,std::uint32_t* type,std::uint32_t* bits) {
    try {
        thug2::qb::Value a,b;a.type=static_cast<thug2::qb::ValueType>(type_a);a.bits=bits_a;
        b.type=static_cast<thug2::qb::ValueType>(type_b);b.bits=bits_b;
        auto v=thug2::qb::binary_value(static_cast<std::uint8_t>(op),
            std::make_shared<const thug2::qb::Value>(std::move(a)),std::make_shared<const thug2::qb::Value>(std::move(b)));
        *type=static_cast<unsigned>(v->type);*bits=v->bits;return 0;
    } catch(const thug2::qb::EvalError& e){return static_cast<int>(e.kind)+1;}
    catch(...){return -1;}
}
EXPORT int runtime_repeat_probe(std::uint32_t count,int counted) {
    try {
        thug2::Bytes file{0x23,0x16,1,0,0,0,1,0x20,1,0x16,100,0,0,0,1,0x21};
        if(counted){file.push_back(0x17);for(unsigned i=0;i<4;++i)file.push_back(count>>(i*8));}
        file.insert(file.end(),{1,0x16,101,0,0,0,1,0x24,0});
        thug2::qb::SymbolTable table;table.load_bytes("repeat-probe",std::move(file));thug2::qb::Interpreter vm(table);
        unsigned ticks=0;int event=0;
        vm.bind(100,[&](const thug2::qb::HostCall&){if(++ticks==2&&!event)event=2;return thug2::qb::HostResult{};});
        vm.bind(101,[&](const thug2::qb::HostCall&){if(!event)event=1;return thug2::qb::HostResult{};});
        thug2::qb::RunLimits limits;limits.fuel=200;auto result=vm.run(1,{},limits);
        return result.status==thug2::qb::RunStatus::Fault?-1:event;
    }catch(...){return -1;}
}
EXPORT int runtime_if_probe(int truth,int negate) {
    try {
        thug2::Bytes body{1,0x47,0,0};if(negate)body.push_back(0x39);
        body.insert(body.end(),{0x16,100,0,0,0,1,0x16,101,0,0,0,1});
        auto otherwise=body.size();body.insert(body.end(),{0x48,0,0});auto target=body.size();
        body.insert(body.end(),{0x16,102,0,0,0,1});auto end=body.size();body.insert(body.end(),{0x28,1,0x24});
        auto delta=target-2;body[2]=delta&255;body[3]=delta>>8;
        delta=end-otherwise-1;body[otherwise+1]=delta&255;body[otherwise+2]=delta>>8;
        thug2::Bytes file{0x23,0x16,1,0,0,0};file.insert(file.end(),body.begin(),body.end());file.push_back(0);
        thug2::qb::SymbolTable table;table.load_bytes("if-probe",std::move(file));thug2::qb::Interpreter vm(table);int event=0;
        vm.bind(100,[=](const thug2::qb::HostCall&){return thug2::qb::HostResult{truth!=0,{}};});
        vm.bind(101,[&](const thug2::qb::HostCall&){event=1;return thug2::qb::HostResult{};});
        vm.bind(102,[&](const thug2::qb::HostCall&){event=2;return thug2::qb::HostResult{};});
        auto result=vm.run(1);return result.status==thug2::qb::RunStatus::Finished?event:-1;
    }catch(...){return -1;}
}
// Five words: kind, counter, timer bits, start_ms, started. Gate is separate.
EXPORT int wait_make(unsigned kind,std::uint32_t amount,std::uint32_t now,std::uint32_t* words){
    try{
        auto s=thug2::qb::make_wait({static_cast<thug2::qb::WaitKind>(kind),amount},{0,0,now});
        words[0]=static_cast<unsigned>(s.kind);words[1]=s.counter;words[2]=std::bit_cast<std::uint32_t>(s.timer);words[3]=s.start_ms;words[4]=s.started;return 0;
    }catch(...){return -1;}
}
EXPORT int wait_advance(std::uint32_t* words,double seconds,std::uint32_t now,unsigned* gate){
    try{
        thug2::qb::WaitState s{static_cast<thug2::qb::WaitKind>(words[0]),words[1],std::bit_cast<float>(words[2]),words[3],words[4]!=0};
        bool released=*gate!=0;thug2::qb::advance_wait(s,{0,seconds,now},released);
        words[0]=static_cast<unsigned>(s.kind);words[1]=s.counter;words[2]=std::bit_cast<std::uint32_t>(s.timer);words[3]=s.start_ms;words[4]=s.started;*gate=released;return 0;
    }catch(...){return -1;}
}

EXPORT int runtime_switch_destination(const std::uint8_t* body,std::size_t size){
    try{
        thug2::Bytes file{0x23,0x16,1,0,0,0};file.insert(file.end(),body,body+size);file.push_back(0);
        thug2::qb::SymbolTable table;table.load_bytes("switch-probe",std::move(file));thug2::qb::Interpreter vm(table);
        auto r=vm.run(1);bool seen=false;
        for(const auto& t:r.trace){if(seen)return static_cast<int>(t.offset);if(t.token==0x3c)seen=true;}
        return -1;
    }catch(...){return -1;}
}

EXPORT void* random_selector_create(std::uint32_t seed){auto* p=new thug2::qb::RandomSelector;p->seed(seed);return p;}
EXPORT void random_selector_destroy(void* p){delete static_cast<thug2::qb::RandomSelector*>(p);}
EXPORT int random_selector_pick(void* p,unsigned opcode,const std::uint16_t* weights,std::size_t count){
    try{return static_cast<int>(static_cast<thug2::qb::RandomSelector*>(p)->choose(static_cast<std::uint8_t>(opcode),1,0,{weights,count},1));}
    catch(...){return -1;}
}
EXPORT void* random_stream_create(std::uint32_t seed){return new thug2::qb::RandomStreams(seed);}
EXPORT void random_stream_destroy(void* p){delete static_cast<thug2::qb::RandomStreams*>(p);}
EXPORT std::int32_t random_stream_draw(void* p,std::int32_t range,int second){return static_cast<thug2::qb::RandomStreams*>(p)->draw(range,second!=0);}
EXPORT void random_stream_state(void* p,int second,std::uint32_t* out){const auto& s=static_cast<thug2::qb::RandomStreams*>(p)->state(second!=0);out[0]=s.add;out[1]=s.mul;out[2]=s.value;}

EXPORT int runtime_inline_arguments(const std::uint8_t* body,std::size_t size,std::uint32_t seed,std::uint32_t* out,std::size_t capacity){
    try{
        using namespace random_fixture;
        Bytes file{0x23,0x16,1,0,0,0,1,0x16,100,0,0,0};file.insert(file.end(),body,body+size);
        for(unsigned i=0;i<8;++i){append(file,name(101));append(file,random_block(i%2?0x37:0x2f,{1,1},{integer(11),integer(22)}));}
        file.insert(file.end(),{0x24,0});SymbolTable symbols;symbols.load_bytes("inline-probe",std::move(file));Interpreter vm(symbols);vm.seed_random(seed);
        std::vector<Member> first;std::vector<std::uint32_t> probes;
        vm.bind(100,[&](const HostCall& c){first=c.arguments->members;return HostResult{};});
        vm.bind(101,[&](const HostCall& c){probes.push_back(c.arguments->members.at(0).value->bits);return HostResult{};});
        auto r=vm.run(1);if(r.status!=RunStatus::Finished||r.trace.size()<3||probes.size()!=8)return -1;
        auto needed=2+first.size()*3+probes.size();if(capacity<needed)return -1;
        out[0]=static_cast<std::uint32_t>(r.trace[2].offset-6);out[1]=static_cast<std::uint32_t>(first.size());std::size_t n=2;
        for(const auto& m:first){out[n++]=m.name;out[n++]=static_cast<unsigned>(m.value->type);out[n++]=m.value->bits;}
        for(auto value:probes)out[n++]=value;return static_cast<int>(n);
    }catch(...){return -1;}
}

}
