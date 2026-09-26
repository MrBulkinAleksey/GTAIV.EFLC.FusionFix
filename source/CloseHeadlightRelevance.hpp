#pragma once
#include <algorithm>
#include "StableHeadlightSelector.hpp"

namespace fusionfix::shadows
{
    // Candidate 19: conservative body relevance, not a change to the rendered
    // spotlight, caster visibility, or shadow-camera near plane.
    inline bool CloseHeadlightTouchesBody(Vec3 player, Vec3 light, Vec3 direction,
                                         float outerCos, float lightRadius) noexcept
    {
        constexpr double bodyRadius = 0.75;
        const double x = static_cast<double>(player.x) - light.x;
        const double y = static_cast<double>(player.y) - light.y;
        const double z = static_cast<double>(player.z) - light.z;
        const double distance2 = x*x + y*y + z*z;
        const double norm2 = static_cast<double>(direction.x)*direction.x +
            static_cast<double>(direction.y)*direction.y + static_cast<double>(direction.z)*direction.z;
        if (!std::isfinite(distance2) || distance2 > 16.0 ||
            !std::isfinite(norm2) || norm2 < 0.0001 ||
            !std::isfinite(outerCos) || outerCos <= 0 || outerCos > 1 ||
            !std::isfinite(lightRadius) || lightRadius <= 0)
            return false;
        const double axial = (x*direction.x + y*direction.y + z*direction.z) / std::sqrt(norm2);
        if (axial < 0 || std::sqrt(distance2) > static_cast<double>(lightRadius) + bodyRadius)
            return false;
        const double radial = std::sqrt((std::max)(0.0, distance2 - axial*axial));
        const double cosine = outerCos;
        const double sine = std::sqrt((std::max)(0.0, 1.0 - cosine*cosine));
        return radial*cosine - axial*sine <= bodyRadius;
    }
}
