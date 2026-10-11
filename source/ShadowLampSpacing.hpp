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
// The lamps kept last pass go first: picking from the nearest alone shifted the whole row by one
// lamp each time the nearest was passed, handing every slot over and back.
class ShadowLampSpacing {
public:
    struct Lamp {
        std::uint32_t index{};
        Vec3 position{}, direction{};
        bool spot{};
        float distanceSquared{};
        std::uint32_t key{};
    };
    float spacing = 0.0f;              // metres in use this pass, 0 is off
    float base = 0.0f;                 // LampSpacing: on foot and at low speed
    float seconds = 0.0f;              // LampSpacingSeconds: driving, speed times this, at least base
    // LampSpacingAlongRoad: the spacing counts along the way you drive, not in a straight line, and
    // lamps abreast of each other (tunnels light both sides) are kept or lowered together. A straight
    // line made the kept lamps zigzag from one side to the other, so a car ahead was lit from the left,
    // then from the right. Off while standing (no way to measure along).
    bool alongRoad = false;
    static constexpr float Abreast = 3.0f, Across = 15.0f; // same row-crossing within 3 m along; rows within 15 m
    void Direction(Vec3 velocity) {
        const float length = std::sqrt(velocity.x * velocity.x + velocity.y * velocity.y);
        axisValid_ = alongRoad && length > 1.0f;
        if (axisValid_) axis_ = {velocity.x / length, velocity.y / length, 0.0f};
    }

    // Driving past a row of lamps, a slot changes hands each time a lamp with one is passed,
    // speed / spacing times a second. The spacing grows with speed so a slot's lamp lasts about
    // `seconds`. Speed is smoothed over about a second, and the spacing moves only in steps of
    // Step metres, since a new spacing hands slots to other lamps too.
    static constexpr float Step = 5.0f, Max = 40.0f;
    static constexpr std::uint32_t HoldMs = 2000; // the spacing changes at most this often, but at once on stopping
    void Update(float speed, bool driving, std::uint32_t now) {
        const float dt = lastTime_ && now > lastTime_ ? (now - lastTime_) * 0.001f : 0.0f;
        lastTime_ = now;
        if (!std::isfinite(speed) || !driving) speed = 0.0f;
        smoothedSpeed_ += (speed - smoothedSpeed_) * std::clamp(dt, 0.0f, 1.0f);
        if (seconds <= 0.0f) { spacing = base; return; }
        const float target = std::clamp((std::max)(base, smoothedSpeed_ * seconds), 0.0f, Max);
        const bool stopped = target <= base && smoothedSpeed_ < 1.0f;
        if ((stopped && spacing != target) || (std::abs(target - spacing) >= Step && now - changedAt_ >= HoldMs)) {
            spacing = target;
            changedAt_ = now;
        }
    }
    float Speed() const { return smoothedSpeed_; }

    void Begin(std::uint32_t count) {
        lamps_.clear();
        spacedOut_.assign((std::min)(count, 4096u), 0);
    }
    void Add(const Lamp& lamp) { if (lamp.index < spacedOut_.size()) lamps_.push_back(lamp); }

    // Returns how many lamps were lowered.
    unsigned Resolve() {
        if (spacing <= 0 || lamps_.size() < 2) return 0;
        for (auto& lamp : lamps_) lamp.index |= WasKept(lamp.key) ? KeptBit : 0u;
        std::sort(lamps_.begin(), lamps_.end(), [](const Lamp& a, const Lamp& b) {
            const bool ka = a.index & KeptBit, kb = b.index & KeptBit;
            return ka != kb ? ka : a.distanceSquared < b.distanceSquared;
        });
        for (auto& lamp : lamps_) lamp.index &= ~KeptBit;
        const float limit = spacing * spacing;
        unsigned lowered = 0;
        kept_.clear();
        for (const auto& lamp : lamps_) {
            bool beside = false;
            for (const auto* other : kept_) {
                const float x = lamp.position.x - other->position.x, y = lamp.position.y - other->position.y,
                    z = lamp.position.z - other->position.z;
                if (axisValid_) {
                    const float along = std::abs(x * axis_.x + y * axis_.y);
                    const float across2 = x * x + y * y - along * along;
                    if (along > spacing || along < Abreast || across2 > Across * Across || lamp.spot != other->spot) continue;
                }
                else if (x * x + y * y + z * z > limit || lamp.spot != other->spot) continue;
                if (lamp.spot && lamp.direction.x * other->direction.x + lamp.direction.y * other->direction.y +
                    lamp.direction.z * other->direction.z < 0.7f * Length(lamp.direction) * Length(other->direction)) continue;
                beside = true;
                break;
            }
            if (beside) { spacedOut_[lamp.index] = 1; ++lowered; }
            else kept_.push_back(&lamp);
        }
        keptKeys_.clear();
        for (const auto* lamp : kept_) if (lamp->key) keptKeys_.push_back(lamp->key);
        std::sort(keptKeys_.begin(), keptKeys_.end());
        return lowered;
    }
    bool SpacedOut(std::uint32_t index) const { return index < spacedOut_.size() && spacedOut_[index]; }

private:
    static float Length(Vec3 v) { return std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z); }
    std::vector<Lamp> lamps_;
    std::vector<const Lamp*> kept_;
    std::vector<std::uint32_t> keptKeys_; // sorted, from the last Resolve
    static constexpr std::uint32_t KeptBit = 0x80000000u; // borrowed from index while sorting
    bool WasKept(std::uint32_t key) const {
        return key && std::binary_search(keptKeys_.begin(), keptKeys_.end(), key);
    }
    std::uint32_t changedAt_ = 0;
    Vec3 axis_{};
    bool axisValid_ = false;
    float smoothedSpeed_ = 0.0f;
    std::uint32_t lastTime_ = 0;
    std::vector<std::uint8_t> spacedOut_;
};
}
