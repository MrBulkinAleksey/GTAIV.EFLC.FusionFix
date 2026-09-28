// OnyxOak modification project: Extra Night Shadows Fix and Better Headlights.
// Project direction, integration and visual testing by OnyxOak; Codex-assisted development.
// Modification notice: 2026-09-27. See ATTRIBUTION.md for upstream credits and GPL-3.0.
// Official release: https://www.nexusmods.com/gta4/mods/1459

#pragma once
#include "StableHeadlightSelector.hpp"
#include <algorithm>
namespace fusionfix::shadows {
// Conservative receiver volumes, not engine model bounds: Niko's body and
// a passenger vehicle footprint. The source cone/radius still bound eligibility.
inline float ReceiverDistanceSquared(Vec3 receiver, Vec3 source, float extent) noexcept {
    const float d2=EvaluateGeometry(receiver,source).distanceSquared;
    if(!std::isfinite(d2)) return d2;
    const float d=(std::max)(0.0f,std::sqrt(d2)-extent);
    return d*d;
}
inline bool BeamTouchesReceiver(Vec3 receiver, float extent, Vec3 source,
                                Vec3 direction, float outerCos, float radius) noexcept {
    if(!std::isfinite(radius)||radius<=0||!std::isfinite(extent)||extent<0||
       !std::isfinite(outerCos)||outerCos<=0||outerCos>1) return false;
    const double x=double(receiver.x)-source.x,y=double(receiver.y)-source.y,z=double(receiver.z)-source.z;
    const double d2=x*x+y*y+z*z;
    const double n2=double(direction.x)*direction.x+double(direction.y)*direction.y+double(direction.z)*direction.z;
    if(!std::isfinite(d2)||!std::isfinite(n2)||n2<0.0001||d2>double(radius+extent)*(radius+extent)) return false;
    const double axial=(x*direction.x+y*direction.y+z*direction.z)/std::sqrt(n2);
    if(axial < -extent) return false;
    const double radial=std::sqrt((std::max)(0.0,d2-axial*axial));
    return radial*outerCos-axial*std::sqrt((std::max)(0.0,1.0-double(outerCos)*outerCos))<=extent;
}
}
