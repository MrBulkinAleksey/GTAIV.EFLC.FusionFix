#pragma once
#include <algorithm>
#include <cmath>

namespace fusionfix::motion
{
    // A toggled or rebound damage field has no matching previous surface. Retaining
    // the same identity only proves binding continuity, not unchanged texture contents.
    inline bool DamageHistoryMatches(bool current, bool previous, const void* texture,
        const void* previousTexture, float radius, float previousRadius)
    {
        return current == previous && (!current || (texture && texture == previousTexture &&
            std::isfinite(radius) && radius > 0 && radius == previousRadius));
    }

    // Rigid tyre shaders rotate by c64..67 and add only gWorld[3]. Shader-local
    // constants override c0, so the other world rows cannot be used as a basis.
    inline void WheelWorld(const float* world, const float* wheel, float* out)
    {
        const float translation[] = { world[12], world[13], world[14] };
        std::copy_n(wheel, 16, out);
        for (int k = 0; k < 3; ++k)
            out[12 + k] += translation[k];
    }
}
