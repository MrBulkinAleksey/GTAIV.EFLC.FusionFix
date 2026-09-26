#pragma once
#include "StableHeadlightSelector.hpp"
#include <algorithm>

namespace fusionfix::shadows {
// Camera relevance heuristic, not an occlusion query. Samples the light's
// influence volume so an off-screen source can still benefit visible scenery.
struct ShadowView {
    float view[4][4]{};
    float projection[4][4]{};
    bool valid{};
};
inline float ViewSample(const ShadowView& c, Vec3 p) noexcept {
    if (!c.valid) return 0;
    const float v[4]{p.x,p.y,p.z,1};
    float eye[4]{}, clip[4]{};
    for (int j=0;j<4;++j) for(int i=0;i<4;++i) eye[j]+=v[i]*c.view[i][j];
    for (int j=0;j<4;++j) for(int i=0;i<4;++i) clip[j]+=eye[i]*c.projection[i][j];
    for (float f:clip) if(!std::isfinite(f)) return 0;
    if(clip[3]<=0.01f || clip[2]<0 || clip[2]>clip[3]) return 0;
    const float x=clip[0]/clip[3], y=clip[1]/clip[3];
    const float edge=(std::max)(std::abs(x),std::abs(y));
    // Soft screen-edge rolloff, modest central emphasis; no hard rejection.
    return std::clamp((1.2f-edge)/1.2f,0.0f,1.0f);
}
inline float ShadowViewWeight(const ShadowView& c, Vec3 position, Vec3 direction,
                              float radius, bool beam) noexcept {
    if(!c.valid || !std::isfinite(radius) || radius<=0) return 1;
    float score=ViewSample(c,position);
    const float span=(std::min)(radius,60.96f);
    const float len=std::sqrt(direction.x*direction.x+direction.y*direction.y+direction.z*direction.z);
    if(beam && std::isfinite(len) && len>0.001f) {
        for(float fraction: {0.2f,0.45f,0.7f,0.95f}) {
            const float d=span*fraction/len;
            score=(std::max)(score,ViewSample(c,{position.x+direction.x*d,position.y+direction.y*d,position.z+direction.z*d}));
        }
    } else if(!beam) {
        const float d=span*0.5f;
        for(Vec3 offset: {Vec3{d,0,0},Vec3{-d,0,0},Vec3{0,d,0},Vec3{0,-d,0},Vec3{0,0,-d}})
            score=(std::max)(score,ViewSample(c,{position.x+offset.x,position.y+offset.y,position.z+offset.z}));
    }
    return 1.0f+2.0f*score; // Bounded preference: distance still matters.
}
}
