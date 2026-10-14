// OnyxOak modification project: Extra Night Shadows Fix and Better Headlights.
// Project direction, integration and visual testing by OnyxOak; Codex-assisted development.
// Modification notice: 2026-09-27. See ATTRIBUTION.md for upstream credits and GPL-3.0.
// Official release: https://www.nexusmods.com/gta4/mods/1459

#pragma once
#include <cmath>
namespace fusionfix::shadows {
// Up to 500 ft (Liberty's Shadows 2.0; it was 200 ft): the reach decides which lights are considered, not their radius.
inline constexpr int MaxShadowReachStep = 100;
inline int ShadowReachFeet(int step) noexcept { return (step < 0 ? 0 : step > MaxShadowReachStep ? MaxShadowReachStep : step) * 5; }
inline bool WithinShadowReach(float distanceSquared, int step) noexcept {
    const float meters = ShadowReachFeet(step) * 0.3048f;
    return meters > 0 && std::isfinite(distanceSquared) && distanceSquared >= 0 && distanceSquared <= meters * meters;
}
}
