#pragma once
#include <array>
#include <cstdint>

namespace fusionfix::shadows {
struct NativeCacheEntry44 {
    float distance{};
    std::int32_t age{};
    std::uint32_t key{};
    std::int32_t state{};
};
// Audited CE 925D40: prefer the oldest unused (state -1) entry, otherwise
// choose the occupied entry strictly farther than the candidate. Preserve
// native tie order. The only added rule is an explicit dependency exclusion.
inline int NativeCacheChoice44(float candidateDistance,
    const std::array<NativeCacheEntry44,8>& cache,std::uint8_t excluded=0) noexcept {
    int unused=-1,furthest=-1;
    std::int32_t oldest=0;
    float distance=candidateDistance;
    for(unsigned i=0;i<cache.size();++i) {
        if(excluded&(1u<<i))continue;
        const auto& e=cache[i];
        if(e.state==-1) {
            if(unused==-1 || e.age>oldest) {unused=static_cast<int>(i);oldest=e.age;}
        } else if(e.distance>distance) {distance=e.distance;furthest=static_cast<int>(i);}
    }
    return unused!=-1?unused:furthest;
}
inline int ProtectNativeCacheChoice44(int nativeResult,float candidateDistance,
    const std::array<NativeCacheEntry44,8>& cache,std::uint8_t dependencies) noexcept {
    if(nativeResult<0 || nativeResult>=8 || !(dependencies&(1u<<nativeResult))) return nativeResult;
    return NativeCacheChoice44(candidateDistance,cache,dependencies);
}
}
