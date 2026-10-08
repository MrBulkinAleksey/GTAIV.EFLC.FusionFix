#pragma once
#include "ShadowCasterPresence.hpp"
#include "ShadowViewPriority.hpp"
#include "FusionLog.hpp"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <format>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace fusionfix::shadows {
// Text trace of who gets the seven dynamic slots (CasterAwareLampPriorityLog = 1),
// to GTAIV.EFLC.FusionFix.NightShadows.SlotTrace.log next to the plugin, for
// tuning the caster aware priority: how far and how long after a lamp has a
// caster in view it takes a slot, how long it holds it, how often gains flip.
// Selection side only (render thread), written out once a second from the game's.
class ShadowSlotTrace {
public:
    struct Light {
        std::uint32_t key{};
        std::uint8_t kind{};          // budget::Kind: 0 lamp, 1 own beam, 2 other beam
        SlotGain raw{}, gain{};       // before and after the hold
        bool cached{}, visible{};
        Vec3 position{};
        float distanceSquared{}, viewWeight{}, priorityDistanceSquared{-1.0f};
        bool spacedOut{};             // lowered for a nearer lamp beside it (LampSpacing)
        bool observed{};
    };
    struct Selected { std::uint32_t key{}; int index{-1}; };

    bool enabled = false;

    void Begin(std::uint32_t frame, std::uint32_t timeMs, Vec3 player, bool driving, const ShadowView& view, std::uint32_t count) {
        frame_ = frame; time_ = timeMs; player_ = player; driving_ = driving;
        cameraValid_ = view.valid;
        if (view.valid) {
            // Row-vector view matrix: the camera is -t R^T, it looks down the
            // third column, negated when the projection is right-handed.
            for (int i = 0; i < 3; ++i) {
                camera_[i] = -(view.view[3][0] * view.view[i][0] + view.view[3][1] * view.view[i][1] + view.view[3][2] * view.view[i][2]);
                forward_[i] = view.view[i][2];
            }
            if (view.projection[2][3] < 0)
                for (float& f : forward_) f = -f;
            const float length = std::sqrt(forward_[0] * forward_[0] + forward_[1] * forward_[1] + forward_[2] * forward_[2]);
            if (length > 0.0001f) for (float& f : forward_) f /= length;
        }
        lights_.assign((std::min)(count, 4096u), Light{});
    }

    void Observe(std::uint32_t index, const Light& light) {
        if (index >= lights_.size()) return;
        lights_[index] = light;
        lights_[index].observed = true;
        auto& k = keys_[light.key];
        k.lastSeen = time_;
        if (light.raw != k.raw) {
            if (k.known && light.kind == 0)
                Line(std::format("G {} key={:08x} {} raw={}->{} held={}{} slot={}", When(), light.key, Kind(light.kind),
                    Gain(k.raw), Gain(light.raw), Gain(light.gain), Where(light), k.slot >= 0 ? 1 : 0));
            if (light.raw == SlotGain::InView) k.inViewSince = time_;
            else if (light.raw == SlotGain::None) k.inViewSince = 0;
            k.raw = light.raw;
        }
        k.known = true;
    }

    // After the pass's seven are final: who came in, who went out, and a summary.
    void Commit(const std::array<Selected, 7>& selection) {
        bool changed = false;
        for (unsigned slot = 0; slot < 7; ++slot) {
            const auto& now = selection[slot];
            if (!now.key || Holds(previous_, now.key)) continue;
            changed = true;
            auto& k = keys_[now.key];
            const Light* light = LightAt(now.index);
            std::string waited = k.inViewSince ? std::format(" waited={}", time_ - k.inViewSince) : std::string(" waited=-");
            std::string sinceLost = k.lostAt ? std::format(" since_lost={}", time_ - k.lostAt) : std::string();
            if (light)
                Line(std::format("+ {} slot={} key={:08x} {} gain={} raw={} cached={} spaced={}{} weight={:.2f} prio={}{}{}", When(), slot, now.key,
                    Kind(light->kind), Gain(light->gain), Gain(light->raw), light->cached ? 1 : 0, light->spacedOut ? 1 : 0, Where(*light), light->viewWeight,
                    light->priorityDistanceSquared >= 0 ? std::format("{:.1f}", std::sqrt(light->priorityDistanceSquared)) : std::string("-"),
                    waited, sinceLost));
            else
                Line(std::format("+ {} slot={} key={:08x} unobserved", When(), slot, now.key));
            k.slot = int(slot); k.slotSince = time_;
        }
        for (const auto& old : previous_) {
            if (!old.key || Holds(selection, old.key)) continue;
            changed = true;
            auto& k = keys_[old.key];
            const Light* light = FindLight(old.key);
            Line(std::format("- {} key={:08x} held_for={}{}", When(), old.key, time_ - k.slotSince,
                light ? std::format(" {} gain={} raw={}{}", Kind(light->kind), Gain(light->gain), Gain(light->raw), Where(*light))
                      : std::string(" gone")));
            k.slot = -1; k.lostAt = time_;
        }
        previous_ = selection;
        if (changed) ++changes_;

        if (changed || time_ - lastSummary_ >= 500u) {
            unsigned none = 0, off = 0, in = 0, wanted = 0, lamps = 0, beams = 0, spaced = 0;
            float nearestWanted = -1.0f;
            for (const auto& light : lights_) {
                if (!light.observed) continue;
                (light.gain == SlotGain::None ? none : light.gain == SlotGain::OffScreen ? off : in)++;
                spaced += light.spacedOut;
                if (light.gain == SlotGain::InView && !Holds(selection, light.key)) {
                    ++wanted;
                    const float d = std::sqrt((std::max)(light.distanceSquared, 0.0f));
                    if (nearestWanted < 0 || d < nearestWanted) nearestWanted = d;
                }
            }
            for (const auto& s : selection)
                if (const Light* light = LightAt(s.index); s.key && light) (light->kind == 0 ? lamps : beams)++;
            float heading = 0, pitch = 0, speed = 0;
            if (cameraValid_) {
                heading = std::atan2(-forward_[0], forward_[1]) * 57.29578f;
                pitch = std::asin(std::clamp(forward_[2], -1.0f, 1.0f)) * 57.29578f;
            }
            if (lastSummary_ && time_ > lastSummary_) {
                const float x = player_.x - lastPlayer_.x, y = player_.y - lastPlayer_.y, z = player_.z - lastPlayer_.z;
                speed = std::sqrt(x * x + y * y + z * z) * 1000.0f / float(time_ - lastSummary_);
            }
            Line(std::format("P {} player={:.1f},{:.1f},{:.1f} speed={:.1f} driving={} heading={:.0f} pitch={:.0f} lights={} "
                "in_view={} off_screen={} none={} spaced={} wanted={} nearest_wanted={} slots={} lamps={} beams={} changes={}",
                When(), player_.x, player_.y, player_.z, speed, driving_ ? 1 : 0, heading, pitch, none + off + in, in, off, none, spaced, wanted,
                nearestWanted >= 0 ? std::format("{:.1f}", nearestWanted) : std::string("-"), lamps + beams, lamps, beams, changes_));
            changes_ = 0;
            lastSummary_ = time_; lastPlayer_ = player_;
        }

        // Keys not seen for a while, so the map stays small
        if (time_ - lastPrune_ > 5000u) {
            std::erase_if(keys_, [&](const auto& e) { return e.second.slot < 0 && time_ - e.second.lastSeen > 10000u; });
            lastPrune_ = time_;
        }
    }

    // Game thread, about once a second.
    void Flush() {
        std::string text;
        {
            std::lock_guard lock(mutex_);
            text.swap(text_);
        }
        if (text.empty() || full_) return;
        written_ += text.size();
        if (written_ > Limit) { full_ = true; text += std::format("trace stopped at {} MB\n", Limit >> 20); }
        if (!text.empty() && text.back() == '\n') text.pop_back();
        FusionLog::WriteText("NightShadows.SlotTrace", "Slots", text);
    }

private:
    static constexpr std::size_t Limit = 64u << 20;
    struct KeyState {
        SlotGain raw{};
        bool known{};
        int slot = -1;
        std::uint32_t inViewSince{}, slotSince{}, lostAt{}, lastSeen{};
    };

    static const char* Kind(std::uint8_t kind) { return kind == 0 ? "lamp" : kind == 1 ? "own_beam" : "beam"; }
    static const char* Gain(SlotGain gain) { return gain == SlotGain::None ? "none" : gain == SlotGain::OffScreen ? "off" : "in"; }
    static bool Holds(const std::array<Selected, 7>& selection, std::uint32_t key) {
        for (const auto& s : selection) if (s.key == key) return true;
        return false;
    }
    const Light* LightAt(int index) const {
        return index >= 0 && std::size_t(index) < lights_.size() && lights_[index].observed ? &lights_[index] : nullptr;
    }
    const Light* FindLight(std::uint32_t key) const {
        for (const auto& light : lights_) if (light.observed && light.key == key) return &light;
        return nullptr;
    }
    std::string When() const { return std::format("t={} f={}", time_, frame_); }
    // Where a light is: from the player (d) and off the camera's view (ang, 0 straight ahead)
    std::string Where(const Light& light) const {
        const float x = light.position.x - player_.x, y = light.position.y - player_.y, z = light.position.z - player_.z;
        std::string angle = "-";
        if (cameraValid_) {
            const float cx = light.position.x - camera_[0], cy = light.position.y - camera_[1], cz = light.position.z - camera_[2];
            const float length = std::sqrt(cx * cx + cy * cy + cz * cz);
            if (length > 0.01f)
                angle = std::format("{:.0f}", std::acos(std::clamp((cx * forward_[0] + cy * forward_[1] + cz * forward_[2]) / length, -1.0f, 1.0f)) * 57.29578f);
        }
        return std::format(" pos={:.1f},{:.1f},{:.1f} d={:.1f} ang={}", light.position.x, light.position.y, light.position.z,
            std::sqrt(x * x + y * y + z * z), angle);
    }
    void Line(std::string&& line) {
        if (full_) return;
        std::lock_guard lock(mutex_);
        text_ += line;
        text_ += '\n';
    }

    std::uint32_t frame_{}, time_{}, lastSummary_{}, lastPrune_{};
    unsigned changes_{};
    Vec3 player_{}, lastPlayer_{};
    bool driving_{}, cameraValid_{};
    float camera_[3]{}, forward_[3]{};
    std::vector<Light> lights_;
    std::unordered_map<std::uint32_t, KeyState> keys_;
    std::array<Selected, 7> previous_{};
    std::mutex mutex_;
    std::string text_;
    std::size_t written_{};
    bool full_{};
};
}
