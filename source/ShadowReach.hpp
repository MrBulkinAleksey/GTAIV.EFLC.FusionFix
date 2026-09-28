// OnyxOak modification project: Extra Night Shadows Fix and Better Headlights.
// Project direction, integration and visual testing by OnyxOak; Codex-assisted development.
// Modification notice: 2026-09-27. See ATTRIBUTION.md for upstream credits and GPL-3.0.
// Official release: https://www.nexusmods.com/gta4/mods/1459

#pragma once
#include <cmath>
namespace fusionfix::shadows {
inline int ShadowReachFeet(int step) noexcept { return (step < 0 ? 0 : step > 40 ? 40 : step) * 5; }
inline bool WithinShadowReach(float distanceSquared, int step) noexcept {
    const float meters = ShadowReachFeet(step) * 0.3048f;
    return meters > 0 && std::isfinite(distanceSquared) && distanceSquared >= 0 && distanceSquared <= meters * meters;
}
}
