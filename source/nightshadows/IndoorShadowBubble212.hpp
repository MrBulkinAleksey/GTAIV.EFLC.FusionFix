#pragma once
// Liberty's Shadows 2.0.4 (OnyxOak, GPL-3.0): IndoorBubble212. On foot, the lamps of the room around
// the player (interior and room > 0) within 35 ft keep their shadows and rank by how much they show.
#include "VisibleReceiverRanking133.hpp"
#include <algorithm>
#include <cmath>

namespace fusionfix::shadows {
inline constexpr float IndoorBubbleFull212=30.0f*0.3048f, IndoorBubbleEnd212=35.0f*0.3048f;

// 1 within 30 ft of the lamp, down to 0 at 35 ft; -1 further.
inline float IndoorBubbleStrength212(Vec3 player, Vec3 source) noexcept {
    const float x=player.x-source.x, y=player.y-source.y, z=player.z-source.z;
    const float d2=x*x+y*y+z*z;
    if(!std::isfinite(d2) || d2>=IndoorBubbleEnd212*IndoorBubbleEnd212) return -1.0f;
    return 1.0f-SmoothUnit((std::sqrt(d2)-IndoorBubbleFull212)/(IndoorBubbleEnd212-IndoorBubbleFull212));
}

// 0..4: how much a room's lamp shows, from its light reaching the view, its size on screen and its
// brightness; -1 when it cannot say.
inline float IndoorBubbleConsequence212(const ShadowView& view, Vec3 source, Vec3 direction,
                                        float radius, float intensity, Vec3 rgb, bool spot) noexcept {
    if(!view.valid || !std::isfinite(radius) || radius<=0 || !VisibleReceiverEmission133(intensity,rgb)) return -1.0f;
    const float visible=std::clamp((ShadowViewWeight(view,source,direction,radius,spot,true)-1.0f)*0.5f,0.0f,1.0f);
    const float p[4]{source.x,source.y,source.z,1};
    float eye[4]{};
    for(int k=0;k<4;++k) for(int n=0;n<4;++n) eye[k]+=p[n]*view.view[n][k];
    for(float f:eye) if(!std::isfinite(f)) return -1.0f;
    const float scale=(std::min)(std::fabs(view.projection[0][0]),std::fabs(view.projection[1][1]));
    if(!std::isfinite(scale) || scale<=0) return -1.0f;
    const float apparent=std::clamp(radius*scale/(std::max)(std::fabs(eye[2]),radius*0.25f),0.0f,1.0f);
    const float brightness=SceneBrightness44(intensity,rgb), contrast=brightness/(brightness+4.0f);
    return 4.0f*contrast*(0.15f+0.85f*visible)*(0.25f+0.75f*apparent);
}

// The native sort distance of such a lamp: up to 5 times nearer by what it shows, blended in by the bubble.
inline float IndoorBubbleNativeDistance212(float distance, float consequence, float bubble) noexcept {
    if(!std::isfinite(distance) || distance<0 || !std::isfinite(consequence) || consequence<0 ||
       !std::isfinite(bubble) || bubble<=0) return distance;
    const float room=(std::min)(distance,distance/(1.0f+consequence));
    return distance+(room-distance)*(std::min)(bubble,1.0f);
}
}
