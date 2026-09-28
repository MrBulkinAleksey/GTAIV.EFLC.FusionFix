#pragma once
#include "ShadowViewPriority.hpp"

namespace fusionfix::shadows {
// Conservative sphere/frustum intersection for *retention*, not source-point
// visibility or an occlusion test. A source behind the camera can illuminate
// receivers in front of it. Six sparse volume samples cannot prove exclusion.
// The source's native radius bounds its light volume, including spotlights.
inline bool ShadowVolumeMayReachView(const ShadowView& view,Vec3 source,float radius) noexcept {
    if(!std::isfinite(radius) || radius<=0 || !std::isfinite(source.x) ||
       !std::isfinite(source.y) || !std::isfinite(source.z)) return false;
    // A missing camera sample is not evidence that an existing shadow left
    // view. The caller still requires current native eligibility and reach.
    if(!view.valid) return true;
    double m[4][4]{};
    for(int i=0;i<4;++i)for(int j=0;j<4;++j)for(int k=0;k<4;++k)
        m[i][j]+=static_cast<double>(view.view[i][k])*view.projection[k][j];
    for(const auto& row:m)for(double v:row)if(!std::isfinite(v))return true;
    for(int plane=0;plane<6;++plane) {
        double p[4]{};
        for(int row=0;row<4;++row) {
            if(plane<4) p[row]=m[row][3]+(plane%2?-1.0:1.0)*m[row][plane/2];
            else p[row]=plane==4?m[row][2]:m[row][3]-m[row][2];
        }
        const double norm=std::sqrt(p[0]*p[0]+p[1]*p[1]+p[2]*p[2]);
        if(norm<1e-12)continue; // Degenerate/infinite far plane is not a cull.
        const double signedDistance=p[0]*source.x+p[1]*source.y+p[2]*source.z+p[3];
        // One metre of exit margin avoids repeatedly releasing at the boundary.
        if(signedDistance<-(static_cast<double>(radius)+1.0)*norm)return false;
    }
    return true;
}
}
