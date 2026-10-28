#pragma once
#include "thug2/runtime.hpp"
#include <bit>
#include <stdexcept>

namespace switch_fixture {
using namespace thug2;using namespace thug2::qb;
inline void word(Bytes& b,std::uint32_t n){for(unsigned i=0;i<4;++i)b.push_back(n>>(8*i));}
inline Bytes name(std::uint32_t n){Bytes b{0x16};word(b,n);return b;}
inline Bytes integer(std::int32_t n){Bytes b{0x17};word(b,std::bit_cast<std::uint32_t>(n));return b;}
inline Bytes floating(float n){Bytes b{0x1a};word(b,std::bit_cast<std::uint32_t>(n));return b;}
inline Bytes param(std::uint32_t n){auto b=name(n);b.insert(b.begin(),0x2d);return b;}
inline Bytes command(std::uint32_t n){auto b=name(n);b.push_back(1);return b;}
inline void append(Bytes& a,const Bytes& b){a.insert(a.end(),b.begin(),b.end());}
struct Case {Bytes value,body;};
inline void jump(Bytes& b,std::size_t field,std::size_t target){
    if(target<field||target-field>65535)throw std::runtime_error("Fixture jump out of range");
    auto delta=target-field;b[field]=delta&255;b[field+1]=delta>>8;
}
inline Bytes block(Bytes value,std::vector<Case> cases,Bytes fallback={},bool has_default=true){
    Bytes b{0x3c};append(b,value);b.push_back(1);
    std::vector<std::size_t> exits;std::optional<std::size_t> previous;
    for(const auto& c:cases){
        auto begin=b.size();if(previous)jump(b,*previous,begin);
        b.insert(b.end(),{0x3e,0x49,0,0});previous=begin+2;
        append(b,c.value);b.push_back(1);append(b,c.body);
        auto end=b.size();b.insert(b.end(),{0x49,0,0});exits.push_back(end+1);
    }
    if(previous)jump(b,*previous,b.size());
    if(has_default){b.insert(b.end(),{0x3f,0x49,0,0});exits.push_back(b.size()-2);append(b,fallback);}
    auto end=b.size();b.insert(b.end(),{0x3d,1});for(auto field:exits)jump(b,field,end);
    return b;
}
inline void script(Bytes& file,std::uint32_t n,const Bytes& body){
    file.push_back(0x23);append(file,name(n));file.push_back(1);append(file,body);file.insert(file.end(),{0x24,1});
}
} // namespace switch_fixture
