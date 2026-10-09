// OnyxOak modification project: Extra Night Shadows Fix and Better Headlights.
// Project direction, integration and visual testing by OnyxOak; Codex-assisted development.
// Modification notice: 2026-09-27. See ATTRIBUTION.md for upstream credits and GPL-3.0.
// Official release: https://www.nexusmods.com/gta4/mods/1459

#pragma once
#include "StableHeadlightSelector.hpp"
#include <algorithm>

namespace fusionfix::shadows {
// Predict priority from observed player-car displacement, without reading any
// unverified vehicle velocity fields. This changes ranking, never native reach.
class ShadowDrivingFocus {
    Vec3 previous{}, velocity{}, focus{};
    std::uintptr_t owner{};
    std::uint32_t lastTime{};
    bool ready{};
public:
    void Reset() noexcept { previous={};velocity={};focus={};owner=0;lastTime=0;ready=false; }
    // The car's speed over the ground in m/s, smoothed over about 0.3 s; 0 on foot.
    float Speed() const noexcept { return ready ? std::sqrt(velocity.x*velocity.x+velocity.y*velocity.y) : 0.0f; }
    Vec3 Velocity() const noexcept { return ready ? velocity : Vec3{}; }
    Vec3 Update(Vec3 position,std::uintptr_t vehicle,std::uint32_t time) noexcept {
        focus=position;
        if(!vehicle || !std::isfinite(position.x) || !std::isfinite(position.y) || !std::isfinite(position.z)) {
            Reset();return position;
        }
        const auto elapsed=time-lastTime;
        if(!ready || owner!=vehicle || elapsed>1000) {
            owner=vehicle;previous=position;velocity={};lastTime=time;ready=true;return focus;
        }
        if(elapsed) {
            const float dt=elapsed*0.001f;
            const Vec3 observed{(position.x-previous.x)/dt,(position.y-previous.y)/dt,0};
            const float speed2=observed.x*observed.x+observed.y*observed.y;
            if(!std::isfinite(speed2) || speed2>100.0f*100.0f) velocity={};
            else {
                const float alpha=std::clamp(dt/0.3f,0.0f,1.0f);
                velocity.x+=(observed.x-velocity.x)*alpha;
                velocity.y+=(observed.y-velocity.y)*alpha;
            }
            previous=position;lastTime=time;
        }
        const float speed=std::sqrt(velocity.x*velocity.x+velocity.y*velocity.y);
        if(speed>1.0f) {
            const float advance=std::min(18.0f,(speed-1.0f)*0.8f);
            focus.x+=velocity.x*advance/speed;focus.y+=velocity.y*advance/speed;
        }
        return focus;
    }
};
inline float DrivingLampPriorityDistance(Vec3 receiver,Vec3 focus,Vec3 light,float physicalDistance,float viewWeight=3.0f) noexcept {
    // The current receiver remains an anchor; an anticipatory focus never
    // penalizes nearby lamps. Offscreen lamps receive no forward preference.
    const float x=light.x-focus.x,y=light.y-focus.y,z=light.z-focus.z;
    const float anticipated=std::min(physicalDistance,x*x+y*y+z*z);
    const float visible=std::clamp((viewWeight-1.0f)*0.5f,0.0f,1.0f);
    return physicalDistance+(anticipated-physicalDistance)*visible;
}
}
