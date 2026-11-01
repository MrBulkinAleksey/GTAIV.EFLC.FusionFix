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

    // Row-vector affine transforms. Remove only twist about the wheel axle, not the
    // hub displacement or the shortest rotation between the two axle directions.
    // The caller supplies a known axle, or -1 to learn it from an unambiguous spin.
    // Failure leaves both the transform and the learned axle unchanged.
    inline bool SuppressWheelTwist(const float* current, float* previous, int& axle)
    {
        float unit[2][3][3]{}, lengths[2][3]{};
        const float* matrices[] = { current, previous };
        for (int pose = 0; pose < 2; ++pose)
        {
            for (int i = 0; i < 16; ++i)
                if (!std::isfinite(matrices[pose][i]))
                    return false;
            if (std::abs(matrices[pose][3]) > 1e-5f || std::abs(matrices[pose][7]) > 1e-5f ||
                std::abs(matrices[pose][11]) > 1e-5f || std::abs(matrices[pose][15] - 1.0f) > 1e-5f)
                return false;
            for (int r = 0; r < 3; ++r)
            {
                const auto* v = matrices[pose] + r * 4;
                float length = std::sqrt(v[0]*v[0] + v[1]*v[1] + v[2]*v[2]);
                if (!(length > 1e-6f) || !std::isfinite(length))
                    return false;
                lengths[pose][r] = length;
                for (int k = 0; k < 3; ++k)
                    unit[pose][r][k] = v[k] / length;
            }
            for (int r = 0; r < 3; ++r)
            {
                float dot = 0;
                for (int k = 0; k < 3; ++k)
                    dot += unit[pose][r][k] * unit[pose][(r + 1) % 3][k];
                if (std::abs(dot) > 1e-3f)
                    return false; // A sheared basis is not a wheel rotation.
            }
        }
        // A reflection between poses cannot be represented by a rotation quaternion.
        auto handedness = [&](int pose)
        {
            const auto& u = unit[pose];
            return (u[0][1]*u[1][2] - u[0][2]*u[1][1])*u[2][0] +
                   (u[0][2]*u[1][0] - u[0][0]*u[1][2])*u[2][1] +
                   (u[0][0]*u[1][1] - u[0][1]*u[1][0])*u[2][2];
        };
        if (handedness(0) * handedness(1) < 0)
            return false;
        float change[3]{};
        for (int r = 0; r < 3; ++r)
            for (int k = 0; k < 3; ++k)
                change[r] += (unit[0][r][k] - unit[1][r][k]) * (unit[0][r][k] - unit[1][r][k]);
        int candidate = static_cast<int>(std::min_element(change, change + 3) - change);
        int chosen = axle;
        // Learn only when both other axes moved distinctly more (at least ~6 degrees).
        if (chosen < 0 || chosen > 2)
        {
            if (change[(candidate + 1) % 3] < 0.01f || change[(candidate + 2) % 3] < 0.01f ||
                change[candidate] * 8 > std::min(change[(candidate + 1) % 3], change[(candidate + 2) % 3]))
                return false;
            chosen = candidate;
        }
        const auto* a = unit[0][chosen];
        const auto* b = unit[1][chosen];
        float cosine = a[0]*b[0] + a[1]*b[1] + a[2]*b[2];
        if (!(cosine > 0.5f))
            return false; // Discontinuity, not ordinary steering.
        // Quaternion taking the current axle to the previous axle, with zero twist.
        float q[4] = { a[1]*b[2] - a[2]*b[1], a[2]*b[0] - a[0]*b[2], a[0]*b[1] - a[1]*b[0], 1 + cosine };
        float norm = std::sqrt(q[0]*q[0] + q[1]*q[1] + q[2]*q[2] + q[3]*q[3]);
        for (auto& x : q) x /= norm;
        for (int r = 0; r < 3; ++r)
        {
            if (r == chosen) continue;
            const auto* v = unit[0][r];
            float t[3] = { 2*(q[1]*v[2] - q[2]*v[1]), 2*(q[2]*v[0] - q[0]*v[2]), 2*(q[0]*v[1] - q[1]*v[0]) };
            float rotated[3] = { v[0] + q[3]*t[0] + q[1]*t[2] - q[2]*t[1],
                                 v[1] + q[3]*t[1] + q[2]*t[0] - q[0]*t[2],
                                 v[2] + q[3]*t[2] + q[0]*t[1] - q[1]*t[0] };
            for (int k = 0; k < 3; ++k)
                previous[r*4 + k] = rotated[k] * lengths[1][r];
        }
        axle = chosen;
        return true;
    }
}
