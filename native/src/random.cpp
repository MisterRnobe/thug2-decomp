// Rnd/Rnd2 and stored-random behavior: mth/random.cpp, script/parse.cpp and
// script/qb.cpp at 2d82ed2. Native keys, ownership and bounds are new.
#include "thug2/random.hpp"
#include "thug2/values.hpp"
#include <algorithm>
#include <bit>

namespace thug2::qb {
namespace {
std::uint32_t shift(std::uint32_t x,unsigned n){return (x>>n)|((0u-(x>>31))<<(32-n));}
[[noreturn]] void invalid(const char* why){throw EvalError(EvalFailure::Syntax,why);}
}
void RandomStreams::reset(std::uint32_t seed){for(auto& s:states_)s={0x0aa2fb3f,0x12b9b0a1,seed};}
std::int32_t RandomStreams::draw(std::int32_t range,bool second){
    auto& s=states_[second?1:0];s.value=s.mul*s.value+s.add;
    s.mul=(s.mul^s.value)+shift(s.value,4);s.add=s.add-0x10101010u+shift(s.value,3);
    auto product=(s.value&0xffffu)*static_cast<std::uint32_t>(range);
    return std::bit_cast<std::int32_t>(shift(product,16));
}
void RandomSelector::forget(std::uint64_t owner){for(auto& s:stored_)if(s.owner==owner)s.owner=0;}
std::size_t RandomSelector::choose(std::uint8_t opcode,std::uint32_t script,std::size_t offset,
                                 std::span<const std::uint16_t> weights,std::uint64_t owner,const Charge& charge){
    auto cost=[&](std::size_t n=1){if(charge)charge(n);};
    auto draw=[&](std::size_t n,bool second=false){cost();return streams_.draw(static_cast<std::int32_t>(n),second);};
    const auto count=weights.size();if(!count||count>65535)invalid("Random choice count must be 1..65535");
    if(opcode==0x2f||opcode==0x37){
        std::int32_t total=0;
        for(auto w:weights){cost();if(w&0x8000)invalid("Negative random weights are unsafe");total+=w;}
        if(!total)invalid("Random table has zero total weight");
        auto pick=draw(static_cast<std::size_t>(total),opcode==0x37);
        // Keep original signed Scale overflow behavior; a negative draw picks
        // entry zero even if that entry has zero weight.
        for(std::size_t i=0;i<count;++i){cost();pick-=weights[i];if(pick<0)return i;}
        invalid("Random choice escaped its table");
    }
    if(opcode!=0x40&&opcode!=0x41)invalid("Unsupported random opcode");
    if(opcode==0x41&&(count<2||count>66))invalid("RandomPermute requires 2..66 choices");
    Stored* state=nullptr;
    for(auto& s:stored_){cost();if(s.script==script&&s.offset==offset&&s.opcode==opcode&&s.count==count&&(s.owner==owner||s.owner==0)){state=&s;break;}}
    auto shuffle=[&](Stored& s,bool avoid){
        auto last=s.order.back();
        for(std::size_t n=count*10;n;--n){auto a=draw(count),b=draw(count);std::swap(s.order[a],s.order[b]);}
        if(avoid&&s.order.front()==last){auto other=draw(count-1)+1;std::swap(s.order.front(),s.order[other]);}
    };
    if(!state){
        cost(count);if(stored_.size()==100)stored_.pop_back();
        stored_.push_front({script,offset,owner,opcode,count,0,0,{}});state=&stored_.front();
        // If fuel fails partway through initialization, discard this new entry.
        try{
            if(opcode==0x40)state->last=static_cast<std::uint32_t>(draw(count));
            else{state->order.resize(count);for(std::size_t i=0;i<count;++i)state->order[i]=static_cast<std::uint8_t>(i);shuffle(*state,false);}
        }catch(...){stored_.pop_front();throw;}
    }
    state->owner=owner;
    if(opcode==0x40){
        auto pick=draw(count);
        for(unsigned tries=0;static_cast<std::uint16_t>(pick)==state->last;){if(++tries>100)break;pick=draw(count);}
        auto choice=static_cast<std::uint16_t>(pick);if(choice>=count)invalid("RandomNoRepeat choice escaped its table");
        state->last=choice;return choice;
    }
    if(++state->index>=count){
        // Restore a valid index on terminal fuel/host faults during shuffling.
        state->index=0;shuffle(*state,true);
    }
    return state->order[state->index];
}
} // namespace thug2::qb
