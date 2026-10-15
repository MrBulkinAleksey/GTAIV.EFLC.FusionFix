#pragma once
#include "ShadowViewPriority.hpp"
namespace fusionfix::shadows {
// Ranking distance only. Both native static-cache and dynamic selection read
// this value in the same invocation; physical light radius/intensity is intact.
inline float NativeLampPriorityDistance(float nativeDistance,float playerDistanceSquared,
                                        float viewWeight,float reach,bool retained=false) noexcept {
    if(!std::isfinite(nativeDistance)||nativeDistance<0 || !std::isfinite(playerDistanceSquared)||playerDistanceSquared<0 ||
       !std::isfinite(viewWeight)||viewWeight<1||viewWeight>3 || !std::isfinite(reach)||reach<=0) return nativeDistance;
    const float distance=std::sqrt(playerDistanceSquared);
    if(distance>=reach || viewWeight<=1) return nativeDistance;
    const float local=SmoothUnit((distance-8)/8);
    const float reachBlend=SmoothUnit((reach-distance)/6);
    const float benefit=(viewWeight-1)*0.5f;
    // At most35% preference, continuous at the local, camera and reach edges.
    // A currently selected, still visible lamp needs a meaningful challenger.
    // This biases the native input BEFORE cache allocation; it never keeps a
    // stale map alive or moves a lamp after the engine has assigned its cache.
    // Apply retention near the player too, where the distance preference fades.
    const float continuity=retained ? 0.50f*reachBlend*benefit : 0.0f;
    return nativeDistance/(1.0f+0.35f*local*reachBlend*benefit+continuity);
}
}
