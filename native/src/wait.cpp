#include "thug2/wait.hpp"
#include "extended.hpp"
#include <stdexcept>

namespace thug2::qb {
WaitState make_wait(WaitRequest r,FrameTick tick){
    WaitState s;s.kind=r.kind;
    switch(r.kind){
    case WaitKind::None:break;
    case WaitKind::Frames:case WaitKind::OnePerFrame:s.counter=r.amount;break;
    case WaitKind::Timer:s.timer=detail::arithmetic(13,double(r.amount),double(0.001f));break;
    case WaitKind::RealTimer:s.start_ms=tick.real_ms;s.timer=static_cast<float>(r.amount);break;
    default:throw std::invalid_argument("Unsupported wait kind");
    }
    return s;
}
bool advance_wait(WaitState& s,FrameTick tick,bool& released){
    if(!std::isfinite(tick.seconds)||tick.seconds<0)throw std::invalid_argument("Invalid frame duration");
    switch(s.kind){
    case WaitKind::None:break;
    case WaitKind::Frames:if(s.counter)--s.counter;else s.kind=WaitKind::None;break;
    case WaitKind::OnePerFrame:
        if(s.counter)--s.counter;else if(!released){released=true;s.kind=WaitKind::None;}break;
    case WaitKind::Timer:
        if(tick.seconds>double(s.timer)){if(s.started)s.kind=WaitKind::None;else s.timer=0;}
        else s.timer=detail::arithmetic(10,double(s.timer),tick.seconds);
        s.started=true;break;
    case WaitKind::RealTimer:
        if(double(std::uint32_t(tick.real_ms-s.start_ms))>=double(s.timer))s.kind=WaitKind::None;break;
    default:throw std::invalid_argument("Unsupported wait kind");
    }
    return s.kind!=WaitKind::None;
}
} // namespace thug2::qb
