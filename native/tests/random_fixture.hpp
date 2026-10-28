#pragma once
#include "switch_fixture.hpp"
namespace random_fixture {
using namespace switch_fixture;
inline Bytes random_block(std::uint8_t opcode,std::vector<std::uint16_t> weights,const std::vector<Bytes>& branches){
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
} // namespace random_fixture
