#pragma once
#include "ShadowReceiver.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <vector>

namespace fusionfix::shadows {
// Lamps in a row a few metres apart (tunnels: about 10 m) cast nearly the same
// shadows, so seven slots on seven neighbours cover a few metres of road. From
// the nearest out, a lamp that keeps its rank lowers the rank of the lamps
// within the spacing around it that shine the same way; those still take a
// slot when nothing else wants one. Not chained: the lamps it lowered lower no
// one, so a tunnel keeps every second or third lamp, not just one.
class ShadowLampSpacing {
public:
    struct Lamp {
        std::uint32_t index{};
        Vec3 position{}, direction{};
        bool spot{};
        float distanceSquared{};
    };
    float spacing = 0.0f;              // metres, 0 is off

    void Begin(std::uint32_t count) {
        lamps_.clear();
        spacedOut_.assign((std::min)(count, 4096u), 0);
    }
    void Add(const Lamp& lamp) { if (lamp.index < spacedOut_.size()) lamps_.push_back(lamp); }

    // Returns how many lamps were lowered.
    unsigned Resolve() {
        if (spacing <= 0 || lamps_.size() < 2) return 0;
        std::sort(lamps_.begin(), lamps_.end(), [](const Lamp& a, const Lamp& b) { return a.distanceSquared < b.distanceSquared; });
        const float limit = spacing * spacing;
        unsigned lowered = 0;
        kept_.clear();
        for (const auto& lamp : lamps_) {
            bool beside = false;
            for (const auto* other : kept_) {
                const float x = lamp.position.x - other->position.x, y = lamp.position.y - other->position.y,
                    z = lamp.position.z - other->position.z;
                if (x * x + y * y + z * z > limit || lamp.spot != other->spot) continue;
                if (lamp.spot && lamp.direction.x * other->direction.x + lamp.direction.y * other->direction.y +
                    lamp.direction.z * other->direction.z < 0.7f * Length(lamp.direction) * Length(other->direction)) continue;
                beside = true;
                break;
            }
            if (beside) { spacedOut_[lamp.index] = 1; ++lowered; }
            else kept_.push_back(&lamp);
        }
        return lowered;
    }
    bool SpacedOut(std::uint32_t index) const { return index < spacedOut_.size() && spacedOut_[index]; }

private:
    static float Length(Vec3 v) { return std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z); }
    std::vector<Lamp> lamps_;
    std::vector<const Lamp*> kept_;
    std::vector<std::uint8_t> spacedOut_;
};
}
