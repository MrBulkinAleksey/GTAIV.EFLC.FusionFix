// Liberty's Shadows 2.0.1 (OnyxOak, GPL-3.0): VisibleReceiverRanking133, the part that fits the
// game's own lamp distance sort. Their scene scorer is not here; our slot claims replace it.
#pragma once
#include "ShadowViewPriority.hpp"
#include <algorithm>
#include <cmath>

namespace fusionfix::shadows {
inline float SceneBrightness44(float intensity, Vec3 rgb) noexcept {
    return std::clamp(intensity,0.0f,1000000.0f)*std::clamp(
        0.6f*(0.2126f*rgb.x+0.7152f*rgb.y+0.0722f*rgb.z)+0.4f*(std::max)({rgb.x,rgb.y,rgb.z}),0.0f,4.0f);
}
inline bool VisibleReceiverEmission133(float intensity, Vec3 rgb) noexcept {
    if(!std::isfinite(intensity) || intensity<=0 || !std::isfinite(rgb.x) || !std::isfinite(rgb.y) || !std::isfinite(rgb.z)) return false;
    const float brightness=SceneBrightness44(intensity,rgb);
    return std::isfinite(brightness) && brightness>0;
}
// How much a point or spot lamp whose light volume reaches the view may light something seen there,
// 0..1: full within the reach of its nearest edge, falling off to 0 at twice the reach; a quarter
// for a light only beside the frame. A ranking proxy, not visibility.
inline float VisibleReceiverBenefit133(bool viewValid, bool volumeVisible, float distanceSquared, float radius,
                                       float reach, float viewWeight, uint32_t type, float outerCos) noexcept {
    if(!viewValid || !volumeVisible) return 0.0f;
    if((type!=0 && type!=2) || !std::isfinite(distanceSquared) || distanceSquared<0 ||
       !std::isfinite(radius) || radius<=0 || !std::isfinite(reach) || reach<=0 ||
       !std::isfinite(viewWeight) || viewWeight<1.0f || viewWeight>3.0f ||
       (type==2 && (!std::isfinite(outerCos) || outerCos<=0 || outerCos>1))) return 0.0f;
    const float nearest=(std::max)(0.0f,std::sqrt(distanceSquared)-radius);
    const float shoulder=SmoothUnit((reach-nearest)/reach);
    const float visibility=0.25f+0.75f*std::clamp((viewWeight-1.0f)*0.5f,0.0f,1.0f);
    return shoulder*visibility;
}
inline float VisibleReceiverNativeDistance133(float original, float benefit) noexcept {
    if(!std::isfinite(original) || original<0 || !std::isfinite(benefit) || benefit<=0) return original;
    return original/(1.0f+0.15f*std::clamp(benefit,0.0f,1.0f));
}
// 0.5..1: how near the middle of the frame the lamp's light is; scales its view preference.
inline float SceneryReceiverFactor163(const ShadowView& view, Vec3 source, Vec3 direction, float radius, uint32_t type) noexcept {
    if(!view.valid) return 1.0f;
    bool homogeneous=false;
    for(unsigned i=0;i<4;++i) {
        if(!std::isfinite(view.projection[i][3])) return 1.0f;
        homogeneous=homogeneous || view.projection[i][3]!=0.0f;
    }
    if(!homogeneous) return 1.0f;
    const float weight=ShadowViewWeight(view,source,direction,radius,type==2,true);
    return std::clamp((weight-1.0f)*0.5f,0.5f,1.0f);
}
}
