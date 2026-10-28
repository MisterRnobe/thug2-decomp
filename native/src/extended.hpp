#pragma once
#include <cmath>
#include <limits>

namespace thug2::qb::detail {
// Binary64 error-free transforms carry the low bits needed for x87's 64-bit
// significand, then round explicitly before conversion to binary32. Intended
// for binary32/int32 QB operands, not a general software x87/environment model.
static_assert(std::numeric_limits<double>::is_iec559 && std::numeric_limits<double>::digits==53);
struct Extended { double high, low; };
inline Extended sum(double a,double b){
    double high=a+b;
    if(!std::isfinite(high))return {high,0};
    double part=high-a;
    return {high,(a-(high-part))+(b-part)};
}
inline Extended precision64(Extended v){
    if(!v.low||!std::isfinite(v.high))return v;
    v=sum(v.high,v.low);
    if(!v.high)return v;
    int exponent;std::frexp(v.high,&exponent);
    double step=std::ldexp(1.0,exponent-64);
    double scaled=v.low/step,base=std::floor(scaled),fraction=scaled-base;
    if(fraction>0.5 || (fraction==0.5 && std::fmod(base,2.0)!=0))base+=1;
    return sum(v.high,base*step);
}
inline Extended add(Extended a,Extended b){
    auto top=sum(a.high,b.high);
    return precision64(sum(top.high,top.low+a.low+b.low));
}
inline Extended multiply(double a,double b){
    double high=a*b;
    return precision64({high,std::isfinite(high)?std::fma(a,b,-high):0});
}
inline Extended divide(double a,double b){
    double high=a/b;
    return precision64({high,std::isfinite(high)?std::fma(-high,b,a)/b:0});
}
inline float single(Extended v){
    v=precision64(v);
    if(!v.low||!std::isfinite(v.high))return static_cast<float>(v.high);
    float rounded=static_cast<float>(v.high);
    if(std::isinf(rounded)){
        double threshold=double(std::numeric_limits<float>::max())+std::ldexp(1.0,103);
        if(std::fabs(v.high)==threshold && ((v.high>0&&v.low<0)||(v.high<0&&v.low>0)))
            return std::copysign(std::numeric_limits<float>::max(),rounded);
        return rounded;
    }
    double nearest=rounded;
    if(v.high==nearest)return rounded;
    float other=std::nextafter(rounded,v.high>nearest?std::numeric_limits<float>::infinity():-std::numeric_limits<float>::infinity());
    if(std::isinf(other))return rounded;
    double midpoint=(nearest+double(other))*0.5;
    if(v.high==midpoint){
        float lower=rounded<other?rounded:other,upper=rounded<other?other:rounded;
        return v.low>0?upper:lower;
    }
    return rounded;
}
inline float arithmetic(unsigned op,double a,double b){
    if(op==11)return single(precision64(sum(a,b)));
    if(op==10)return single(precision64(sum(a,-b)));
    if(op==13)return single(multiply(a,b));
    return single(divide(a,b));
}
} // namespace thug2::qb::detail
