#pragma once
#include "ShadowReceiver.hpp"
#include "ShadowViewPriority.hpp"
#include <array>
#include <cstdint>

namespace fusionfix::shadows {
// What a dynamic slot adds over no slot: nothing the cache does not already
// show, a shadow off screen, or a shadow in view. Ranks above slot history.
enum class SlotGain : std::uint8_t { None, OffScreen, InView };
// Where the moving shadow casters, vehicles and peds, are this frame. A lamp's
// cached map holds the static world only, so a dynamic slot shows something
// the cache does not just where one of them stands in the lamp's light.
struct ShadowCasterPresence {
    static constexpr unsigned Capacity = 384;
    static constexpr float VehicleExtent = 4.0f, PedExtent = 1.0f; // Bounding radii with a margin; a bus is longer.
    static constexpr float Range = 150.0f; // Around the player; lamp reach stays well inside.
    // CasterMinLightHeight: a light less than this many metres above a car's or ped's position (some 3 m over the
    // ground) gets no claim from it where a higher light with a shadow reaches it too (covered, from the caller): a
    // lamp over a door, 2 m above the pavement, took slots for cars 10 m off under the street lamps and threw a
    // second, long and low shadow of them and of the lamp posts. A low light with no lamp above the caster keeps
    // its claim. 0: any height.
    static inline float MinLightAbove = 0.0f;
    struct NoCover { bool operator()(unsigned) const noexcept { return false; } };
    std::array<Vec3, Capacity> points{};
    std::array<float, Capacity> extents{};
    std::array<bool, Capacity> inView{};
    std::array<std::uintptr_t, Capacity> vehicles{}; // the car, 0 for a ped
    unsigned count = 0;
    std::uint32_t timeMs = 0;
    bool valid = false, overflow = false; // Both pools read in full, or every lamp counts as lighting a caster.
    // Vehicle box shadows: cars that came under a lamp while it had no slot keep their box from it until they leave,
    // and its map leaves them out; they give that lamp no reason to take a slot.
    struct BoxedCar { Vec3 lamp; std::uintptr_t car; };
    static constexpr unsigned BoxedCapacity = 256;
    std::array<BoxedCar, BoxedCapacity> boxed{};
    unsigned boxedCount = 0;

    void Add(Vec3 point, float extent, std::uintptr_t car = 0) noexcept {
        if (count < Capacity) { points[count] = point; extents[count] = extent; vehicles[count] = car; ++count; }
        else overflow = true; // A missed caster could cost its shadow.
    }
    bool Boxed(std::uintptr_t car, Vec3 lamp) const noexcept {
        for (unsigned i = 0; i < boxedCount; ++i) {
            const auto& b = boxed[i];
            const float x = b.lamp.x - lamp.x, y = b.lamp.y - lamp.y, z = b.lamp.z - lamp.z;
            if (b.car == car && x * x + y * y + z * z < 0.25f) return true;
        }
        return false;
    }
    // Its shadow falls around it, so a caster in or just outside the frame
    // counts as looked at. Without a camera every caster does.
    void Look(const ShadowView& view) noexcept {
        for (unsigned i = 0; i < count; ++i) inView[i] = !view.valid || ViewSample(view, points[i]) > 0;
    }
    // The best caster a point or spot light's volume reaches: one in view
    // over one off screen.
    // which: the caster that decided, -1 for none (or when every lamp counts).
    template <typename Covered = NoCover>
    SlotGain Lights(Vec3 source, Vec3 direction, int type, float radius, float outerCos, int* which = nullptr,
                    Covered covered = {}) const noexcept {
        if (which) *which = -1;
        if (!valid || overflow || !std::isfinite(radius) || radius <= 0) return SlotGain::InView;
        auto gain = SlotGain::None;
        const float axis = direction.x * direction.x + direction.y * direction.y + direction.z * direction.z;
        const bool spot = type == 2 && std::isfinite(outerCos) && outerCos > 0 && outerCos <= 1 &&
            std::isfinite(axis) && axis > 0.0001f; // Otherwise the whole sphere.
        for (unsigned i = 0; i < count; ++i) {
            const auto& p = points[i];
            if (gain == SlotGain::OffScreen && !inView[i]) continue;
            bool lit;
            if (spot) lit = BeamTouchesReceiver(p, extents[i], source, direction, outerCos, radius);
            else {
                const float x = p.x - source.x, y = p.y - source.y, z = p.z - source.z;
                const float reach = radius + extents[i];
                lit = x * x + y * y + z * z <= reach * reach;
            }
            if (!lit || (vehicles[i] && boxedCount && Boxed(vehicles[i], source)) ||
                (source.z - p.z < MinLightAbove && covered(i))) continue;
            if (which) *which = int(i);
            if (inView[i]) return SlotGain::InView;
            gain = SlotGain::OffScreen;
        }
        return gain;
    }
};

// Keeps a light's gain for a while after it drops, so a caster on the edge of
// a lamp's light or of the frame does not hand the slot back and forth every
// frame. A rise counts at once. Ranking history only: keys, never pointers.
class SlotGainHold {
    struct Entry { std::uint32_t key{}; std::uint64_t generation{}; std::uint32_t time{}; SlotGain gain{}; };
    std::array<Entry, 128> entries_{};
public:
    std::uint32_t holdMs = 0;
    SlotGain Apply(std::uint32_t key, std::uint64_t generation, SlotGain gain, std::uint32_t now) noexcept {
        if (!holdMs || !key) return gain;
        Entry* oldest = &entries_[0];
        for (auto& e : entries_) {
            if (e.key == key && e.generation == generation) {
                if (gain < e.gain && now - e.time <= holdMs) return e.gain;
                e = {key, generation, now, gain};
                return gain;
            }
            if (!e.key || now - e.time > now - oldest->time) oldest = &e;
            if (!e.key) break;
        }
        if (gain != SlotGain::None) *oldest = {key, generation, now, gain};
        return gain;
    }
    void Reset() noexcept { entries_ = {}; }
};
}
