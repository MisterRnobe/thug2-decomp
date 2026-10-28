#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <list>
#include <span>
#include <vector>

namespace thug2::qb {
struct RandomRange {bool floating;std::uint32_t bits;};
struct RandomState {std::uint32_t add, mul, value;};
class RandomStreams {
public:
    explicit RandomStreams(std::uint32_t seed=0){reset(seed);}
    void reset(std::uint32_t seed);
    std::int32_t draw(std::int32_t range,bool second=false);
    const RandomState& state(bool second=false) const{return states_[second?1:0];}
private:
    std::array<RandomState,2> states_{};
};
// A bounded native counterpart of the engine's stored-random list. Keys are
// script/offset/owner identities, never game pointers. Oldest insertion evicts.
class RandomSelector {
public:
    using Charge=std::function<void(std::size_t)>;
    void seed(std::uint32_t seed){streams_.reset(seed);} // Stored orders remain.
    std::size_t choose(std::uint8_t opcode,std::uint32_t script,std::size_t offset,
                       std::span<const std::uint16_t> weights,std::uint64_t owner,
                       const Charge& charge={});
    RandomRange range(float a,float b,bool second=false,const Charge& charge={});
    void forget(std::uint64_t owner);
    std::size_t stored_count() const{return stored_.size();}
private:
    struct Stored {
        std::uint32_t script;std::size_t offset;std::uint64_t owner;
        std::uint8_t opcode;std::size_t count,index=0,last=0;
        std::vector<std::uint8_t> order;
    };
    RandomStreams streams_;
    std::list<Stored> stored_; // Newest first; at most 100 entries, as original.
};
} // namespace thug2::qb
