#pragma once
#include "ShadowReceiver.hpp"
#include <array>
#include <cstdint>

namespace fusionfix::shadows {
// Where the moving shadow casters, vehicles and peds, are this frame. A lamp's
// cached map holds the static world only, so a dynamic slot shows something
// the cache does not just where one of them stands in the lamp's light.
struct ShadowCasterPresence {
    static constexpr unsigned Capacity = 384;
    static constexpr float VehicleExtent = 4.0f, PedExtent = 1.0f; // Bounding radii with a margin; a bus is longer.
    static constexpr float Range = 150.0f; // Around the player; lamp reach stays well inside.
    std::array<Vec3, Capacity> points{};
    std::array<float, Capacity> extents{};
    unsigned count = 0;
    std::uint32_t timeMs = 0;
    bool valid = false, overflow = false; // Both pools read in full, or every lamp counts as lighting a caster.

    void Add(Vec3 point, float extent) noexcept {
        if (count < Capacity) { points[count] = point; extents[count] = extent; ++count; }
        else overflow = true; // A missed caster could cost its shadow.
    }
    // A point or spot light whose volume reaches a caster's bounding sphere.
    bool Lights(Vec3 source, Vec3 direction, int type, float radius, float outerCos) const noexcept {
        if (!valid || overflow || !std::isfinite(radius) || radius <= 0) return true;
        const float axis = direction.x * direction.x + direction.y * direction.y + direction.z * direction.z;
        const bool spot = type == 2 && std::isfinite(outerCos) && outerCos > 0 && outerCos <= 1 &&
            std::isfinite(axis) && axis > 0.0001f; // Otherwise the whole sphere.
        for (unsigned i = 0; i < count; ++i) {
            const auto& p = points[i];
            if (spot) {
                if (BeamTouchesReceiver(p, extents[i], source, direction, outerCos, radius)) return true;
                continue;
            }
            const float x = p.x - source.x, y = p.y - source.y, z = p.z - source.z;
            const float reach = radius + extents[i];
            if (x * x + y * y + z * z <= reach * reach) return true;
        }
        return false;
    }
};
}
