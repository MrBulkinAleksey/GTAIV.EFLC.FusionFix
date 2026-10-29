// Liberty's Shadows 2.0.1 (OnyxOak, GPL-3.0): StaticConsumerReach136, the lamp shadow size gate,
// and from 2.0.4 its retention (198A), distance fade (198B) and indoor bubble (212).
//
// The deferred pass draws a light with its shadow map (CE 0xAC1030) only while the light is big on
// screen: radius / distance from the camera >= camera+2CC * [0x103EFDC], else without its shadow
// (setbe [esp+E] at 0xAC14A7). Far lamps lost their shadows while their light still lit the street.
// This lets a lamp that has its map (lookup [esp+D]) keep it until a light this many times smaller.
// Vehicle lights (flags 0x100/0x400) keep the game's limit: it draws no extra maps, only uses those
// it has.
//
// Before that test the pass binds the map (0x9268B0, called at 0xAC124A): at 0x926AD9 it hands the
// shader dShadowParam (c76) from the map's header, x always 0. For a lamp kept past the game's limit,
// x becomes -198 - s and local_light_with_shadow_size_fade.patch fades its shadow by s: full up to a
// third of the way to the reach, none at it, so a lamp's shadow no longer goes at once.

namespace LampShadowSizeGate
{
    static SafetyHookMid gateHook, binderHook;
    static float scale = 1.0f;
    static bool retention = false, fade = false, indoorBubble = false;
    static std::string status = "off in the ini";
    static std::atomic<uint32_t> kept{0}, retained{0}, faded{0}, indoor{0};

    // A lamp kept inside the reach stays kept to this much further for a moment (198A), so one at the
    // edge does not lose and regain its shadow from frame to frame.
    constexpr float RetentionScale = 1.40f / 1.30f;
    constexpr uint32_t RetentionMs = 180, RetentionFrames = 12;

    static bool Qualifies(const rage::CLightSource& light) noexcept
    {
        if ((light.mType != rage::LT_POINT && light.mType != rage::LT_SPOT) || (light.mFlags & 0x500) ||
            !light.mCastShadows || light.mCastShadows == -1) return false;
        return std::isfinite(light.mRadius) && light.mRadius > 0 && std::isfinite(light.mIntensity) && light.mIntensity > 0;
    }

    // The deferred pass's frame (its esp, from 0xAC1030's own and through 0x9268B0's ebp) holds the
    // light + 0x28 at +1C, the radius it tests at +14, the camera at +28 and the light's position at +40.
    static const rage::CLightSource* PassLight(uintptr_t frame) noexcept
    {
        const auto source = *reinterpret_cast<const uintptr_t*>(frame + 0x1C);
        if (source < 0x10028) return nullptr;
        const auto* light = reinterpret_cast<const rage::CLightSource*>(source - 0x28);
        return Qualifies(*light) ? light : nullptr;
    }

    // Distance over radius against the game's limit, as 0xAC144E computes them: past 1 the game drops the map.
    static float SizeRatio(uintptr_t frame) noexcept
    {
        const auto camera = *reinterpret_cast<const uintptr_t*>(frame + 0x28);
        if (camera < 0x10000) return -1.0f;
        const auto* eye = reinterpret_cast<const float*>(camera + 0x70);
        const auto* position = reinterpret_cast<const float*>(frame + 0x40);
        const float radius = *reinterpret_cast<const float*>(frame + 0x14);
        const float limit = *reinterpret_cast<const float*>(camera + 0x2CC) * *reinterpret_cast<const float*>(GameBase() + 0xC3EFDC);
        const float dx = eye[0] - position[0], dy = eye[1] - position[1], dz = eye[2] - position[2];
        const float scaled = std::sqrt(dx * dx + dy * dy + dz * dz) * limit / radius;
        return std::isfinite(scaled) && scaled > 0 ? scaled : -1.0f;
    }

    // Recently kept lamps by shadow key, position and range (ApplicationHistory198), render thread only.
    struct History
    {
        struct Entry { int32_t key = 0; float position[3]{}, radius = 0; uint32_t frame = 0, tick = 0; bool used = false; };
        std::array<Entry, 32> entries{};

        static unsigned Slot(int32_t key) noexcept { const auto k = static_cast<uint32_t>(key); return ((k * 0x9E3779B9u) ^ (k >> 16)) & 31u; }
        static bool Same(const Entry& e, const rage::CLightSource& l) noexcept
        {
            return e.used && e.key == l.mCastShadows && e.radius == l.mRadius && e.position[0] == l.mPosition.x &&
                e.position[1] == l.mPosition.y && e.position[2] == l.mPosition.z;
        }
        static bool Fresh(const Entry& e, uint32_t frame, uint32_t tick) noexcept
        {
            return tick - e.tick <= RetentionMs && frame - e.frame <= RetentionFrames;
        }
        bool Retained(const rage::CLightSource& l, uint32_t frame, uint32_t tick) const noexcept
        {
            for (unsigned n = 0; n < 4; ++n)
            {
                const auto& e = entries[(Slot(l.mCastShadows) + n) & 31u];
                if (Same(e, l)) return Fresh(e, frame, tick);
            }
            return false;
        }
        // Only inside the reach: the band past it never extends its own hold.
        void Kept(const rage::CLightSource& l, uint32_t frame, uint32_t tick) noexcept
        {
            Entry* vacant = nullptr;
            for (unsigned n = 0; n < 4; ++n)
            {
                auto& e = entries[(Slot(l.mCastShadows) + n) & 31u];
                if (Same(e, l)) { e.frame = frame; e.tick = tick; return; }
                if (!vacant && (!e.used || !Fresh(e, frame, tick))) vacant = &e;
            }
            if (vacant) *vacant = { l.mCastShadows, { l.mPosition.x, l.mPosition.y, l.mPosition.z }, l.mRadius, frame, tick, true };
        }
    };
    static History history;

    // The player's position on foot, read once a frame on the render thread; none in a car.
    static bool PlayerOnFoot(float (&position)[3]) noexcept
    {
        static uint32_t frame = UINT32_MAX;
        static bool onFoot = false;
        static float cached[3]{};
        if (frame != *CTimer::m_frameCount)
        {
            frame = *CTimer::m_frameCount;
            PlayerCar::Focus focus;
            onFoot = PlayerCar::ReadFocus(focus) && !focus.car;
            std::copy(std::begin(focus.position), std::end(focus.position), cached);
        }
        std::copy(std::begin(cached), std::end(cached), position);
        return onFoot;
    }

    // On foot, the lamps of the room around you within 30 ft (9.1 m) always keep their shadow, fading
    // out to 35 ft (10.7 m) (212): 1 down to 0, -1 outside the bubble.
    static float IndoorStrength(const rage::CLightSource& light) noexcept
    {
        if (!indoorBubble || light.mInteriorIndex <= 0 || light.mRoomIndex <= 0) return -1.0f;
        float player[3];
        if (!PlayerOnFoot(player)) return -1.0f;
        return fusionfix::shadows::IndoorBubbleStrength212({ player[0], player[1], player[2] },
            { light.mPosition.x, light.mPosition.y, light.mPosition.z });
    }

    static void Gate(SafetyHookContext& regs) noexcept
    {
        auto* decisions = reinterpret_cast<uint8_t*>(regs.esp + 0xD);
        // [esp+D] map found, [esp+E] the size test; xmm0 the limit, xmm1 radius / distance.
        if (decisions[0] != 1 || decisions[1] != 0) return;
        const float limit = regs.xmm0.f32[0], ratio = regs.xmm1.f32[0];
        if (!std::isfinite(limit) || !std::isfinite(ratio) || limit <= 0 || ratio <= 0) return;
        const auto* light = PassLight(regs.esp);
        if (!light) return;
        const uint32_t frame = *CTimer::m_frameCount, tick = static_cast<uint32_t>(*CTimer::m_snTimeInMilliseconds);
        if (ratio * scale >= limit)
        {
            if (retention) history.Kept(*light, frame, tick);
            kept.fetch_add(1, std::memory_order_relaxed);
        }
        else if (retention && ratio * scale * RetentionScale >= limit && history.Retained(*light, frame, tick))
            retained.fetch_add(1, std::memory_order_relaxed);
        else if (IndoorStrength(*light) > 0)
            indoor.fetch_add(1, std::memory_order_relaxed);
        else
            return;
        decisions[1] = 1;
    }

    static void Binder(SafetyHookContext& regs) noexcept
    {
        // Only the deferred pass's call (0xAC124A); [ebp+8] the map's index, its frame above that.
        if (*reinterpret_cast<const uintptr_t*>(regs.ebp + 4) != GameBase() + 0x6C124F) return;
        const uintptr_t frame = regs.ebp + 0xC;
        auto* parameter = *reinterpret_cast<float**>(regs.esp + 8);
        if (reinterpret_cast<uintptr_t>(parameter) != regs.esp + 0x28 || parameter[0] != 0.0f) return;
        const auto* light = PassLight(frame);
        if (!light) return;
        const float scaled = SizeRatio(frame);
        // Full shadow up to a third of the way from the game's limit to the reach.
        const float start = 1.0f + (scale - 1.0f) / 3.0f;
        if (scaled < start) return;
        const float t = std::clamp((scale - scaled) / (scale - start), 0.0f, 1.0f);
        const float strength = (std::max)(t * t * (3.0f - 2.0f * t), IndoorStrength(*light));
        if (!(strength >= 0.0f && strength <= 1.0f)) return;
        parameter[0] = -198.0f - strength;
        faded.fetch_add(1, std::memory_order_relaxed);
    }

    static void Install(float reach, bool keepShortly, bool fadeOut, bool roomBubble) noexcept
    {
        scale = std::isfinite(reach) ? std::clamp(reach, 1.0f, 4.0f) : 1.0f;
        if (scale == 1.0f) return;
        retention = keepShortly;
        indoorBubble = roomBubble;
        using HeadlightEnhancement::CodeCheck;
        // comiss xmm0, xmm1 / setbe [esp+E] / cmp byte ptr [0x154E162], 1 / lookup call and its key push.
        const auto check = CodeCheck().Bytes(0x6C14A4, {0x0F,0x2F,0xC1,0x0F,0x96,0x44,0x24,0x0E,0x80,0x3D})
            .Address(0x6C14AE, 0x114E162).Bytes(0x6C14B2, {0x01})
            .Bytes(0x6C1234, {0xFF,0x77,0x38,0xE8}).Branch(0x6C1238, 0x525DB0)
            // The size test's camera, position and radius: 0xAC144E..0xAC14A3.
            .Bytes(0x6C144E, {0x8B,0x44,0x24,0x28,0xF3,0x0F,0x10,0x48,0x70,0xF3,0x0F,0x5C,0x4C,0x24,0x40})
            .Bytes(0x6C1483, {0xF3,0x0F,0x10,0x4C,0x24,0x14}).Bytes(0x6C1494, {0xF3,0x0F,0x10,0x80,0xCC,0x02,0x00,0x00,0xF3,0x0F,0x59,0x05})
            .Address(0x6C14A0, 0xC3EFDC);
        if (!check) { status = check.Status(); return; }
        gateHook = safetyhook::create_mid(GameBase() + 0x6C14AC, Gate);
        status = gateHook ? "installed" : "hook failed";
        if (!gateHook || !fadeOut) return;
        // push eax (map index) / call 0x9268B0 at 0xAC1244; in it, x from the header's +28 into [esp+1C],
        // lea eax, [esp+1C] / push eax / push dShadowParam's handle, then call 0x924A40 at 0x926AD9.
        const auto binder = CodeCheck().Bytes(0x6C1244, {0x50,0xC6,0x44,0x24,0x11,0x01,0xE8}).Branch(0x6C124B, 0x5268B0)
            .Bytes(0x526A99, {0xF3,0x0F,0x10,0x46,0x28,0xF3,0x0F,0x11,0x44,0x24,0x1C})
            .Bytes(0x526AAC, {0x8D,0x44,0x24,0x1C,0x50,0xFF,0x35}).Address(0x526AB3, 0x114E258)
            .Bytes(0x526AD9, {0xE8}).Branch(0x526ADA, 0x524A40);
        if (!binder) { status += " | fade: " + binder.Status(); return; }
        binderHook = safetyhook::create_mid(GameBase() + 0x526AD9, Binder);
        fade = static_cast<bool>(binderHook);
        if (!fade) status += " | fade: hook failed";
    }
}
