#pragma once
#include "ShadowReceiver.hpp"
#include "ShadowViewPriority.hpp"
#include <array>

namespace fusionfix::shadows {
// Copied receiver geometry only. These numeric identities are never
// dereferenced in the render/submission callback. Visibility is a broad
// camera-frustum heuristic, not an occlusion result or exact model bounds.
struct NearbyVehicleReceivers36 {
    static constexpr unsigned Capacity=32;
    static constexpr float Reach=24.384f; // 80 feet around the on-foot player.
    static constexpr float Extent=3.0f;
    struct Receiver { Vec3 position{}; std::uintptr_t identity{}; float distanceSquared{}; };
    std::array<Receiver,Capacity> receivers{};
    unsigned count{};
    std::uintptr_t session{};
    std::uint32_t frame{},timeMs{};
    Vec3 origin{};
    ShadowView view{};

    void Add(Vec3 position,std::uintptr_t identity) noexcept {
        const float d2=EvaluateGeometry(origin,position).distanceSquared;
        if(!identity || !std::isfinite(d2) || d2>Reach*Reach) return;
        for(unsigned i=0;i<count;++i) if(receivers[i].identity==identity) return;
        unsigned at=0;
        while(at<count && (receivers[at].distanceSquared<d2 ||
            (receivers[at].distanceSquared==d2 && receivers[at].identity<identity))) ++at;
        if(at==Capacity) return;
        const unsigned last=(std::min)(count,Capacity-1);
        for(unsigned i=last;i>at;--i) receivers[i]=receivers[i-1];
        receivers[at]={position,identity,d2}; if(count<Capacity)++count;
    }

    float Score(std::uintptr_t player,std::uint32_t currentFrame,std::uint32_t now,
                 Vec3 currentPosition,Vec3 source,Vec3 direction,float outerCos,float radius,
                 std::uintptr_t beamKey) const noexcept {
        if(!session || session!=player || currentFrame-frame>2 || now-timeMs>100 ||
            !view.valid || !beamKey || count>Capacity) return 0.0f;
        float best=0.0f;
        for(unsigned i=0;i<count;++i) {
            const auto& r=receivers[i];
            if(beamKey==r.identity || (r.identity<UINTPTR_MAX && beamKey==r.identity+1)) continue;
            const float d2=EvaluateGeometry(currentPosition,r.position).distanceSquared;
            if(!std::isfinite(d2) || d2>Reach*Reach) continue;
            if(!BeamTouchesReceiver(r.position,Extent,source,direction,outerCos,radius)) continue;
            float visible=ViewSample(view,r.position);
            for(Vec3 offset : {Vec3{Extent,0,0},Vec3{-Extent,0,0},Vec3{0,Extent,0},
                              Vec3{0,-Extent,0},Vec3{0,0,Extent},Vec3{0,0,-Extent}})
                visible=(std::max)(visible,ViewSample(view,{r.position.x+offset.x,r.position.y+offset.y,r.position.z+offset.z}));
            if(visible>0.05f) best=(std::max)(best,visible*(0.5f+0.5f*(1.0f-SmoothUnit(d2/(Reach*Reach)))));
        }
        return best;
    }
    bool Touches(std::uintptr_t player,std::uint32_t currentFrame,std::uint32_t now,
                 Vec3 currentPosition,Vec3 source,Vec3 direction,float outerCos,float radius,
                 std::uintptr_t beamKey) const noexcept {
        return Score(player,currentFrame,now,currentPosition,source,direction,outerCos,radius,beamKey)>0;
    }
};
}
