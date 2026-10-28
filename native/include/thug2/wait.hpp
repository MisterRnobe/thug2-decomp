#pragma once
#include <cstdint>

namespace thug2::qb {
enum class WaitKind : std::uint32_t { None=0, Frames=1, Timer=2, RealTimer=4, OnePerFrame=11 };
struct FrameTick {
    std::uint64_t frame;
    double seconds; // Slowed game-frame duration, not elapsed wall time.
    std::uint32_t real_ms; // Game clock plus its offset, wrapping at 32 bits.
};
struct WaitRequest { WaitKind kind; std::uint32_t amount; };
struct WaitState {
    WaitKind kind=WaitKind::None;
    std::uint32_t counter=0;
    float timer=0;
    std::uint32_t start_ms=0;
    bool started=false;
};
WaitState make_wait(WaitRequest request, FrameTick tick);
// Returns true while waiting. The gate belongs to the interpreter/frame.
bool advance_wait(WaitState& state, FrameTick tick, bool& one_per_frame_released);
} // namespace thug2::qb
