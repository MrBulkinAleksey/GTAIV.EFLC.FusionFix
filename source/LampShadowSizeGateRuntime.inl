// Liberty's Shadows 2.0.1 (OnyxOak, GPL-3.0): StaticConsumerReach136, the lamp shadow size gate.
//
// The deferred pass draws a light with its shadow map (CE 0xAC1030) only while the light is big on
// screen: radius / distance from the camera >= camera+2CC * [0x103EFDC], else without its shadow
// (setbe [esp+E] at 0xAC14A7). Far lamps lost their shadows while their light still lit the street.
// This lets a lamp that has its map (lookup [esp+D]) keep it until a light this many times smaller.
// Vehicle lights (flags 0x100/0x400) keep the game's limit: it draws no extra maps, only uses those
// it has.

namespace LampShadowSizeGate
{
    static SafetyHookMid gateHook;
    static float scale = 1.0f;
    static std::string status = "off in the ini";
    static std::atomic<uint32_t> kept{0};

    static void Gate(SafetyHookContext& regs) noexcept
    {
        auto* decisions = reinterpret_cast<uint8_t*>(regs.esp + 0xD);
        // [esp+D] map found, [esp+E] the size test; xmm0 the limit, xmm1 radius / distance.
        if (decisions[0] != 1 || decisions[1] != 0) return;
        const float limit = regs.xmm0.f32[0], ratio = regs.xmm1.f32[0];
        if (!std::isfinite(limit) || !std::isfinite(ratio) || limit <= 0 || ratio <= 0 || ratio * scale < limit) return;
        // edi of the lookup (light + 0x28) kept at [esp+1C].
        const auto source = *reinterpret_cast<const uintptr_t*>(regs.esp + 0x1C);
        if (source < 0x10028) return;
        const auto& light = *reinterpret_cast<const rage::CLightSource*>(source - 0x28);
        if ((light.mType != rage::LT_POINT && light.mType != rage::LT_SPOT) || (light.mFlags & 0x500) || !light.mCastShadows || light.mCastShadows == -1) return;
        if (!std::isfinite(light.mRadius) || light.mRadius <= 0 || !std::isfinite(light.mIntensity) || light.mIntensity <= 0) return;
        decisions[1] = 1;
        kept.fetch_add(1, std::memory_order_relaxed);
    }

    static void Install(float reach) noexcept
    {
        scale = std::isfinite(reach) ? std::clamp(reach, 1.0f, 4.0f) : 1.0f;
        if (scale == 1.0f) return;
        using HeadlightEnhancement::CodeCheck;
        // comiss xmm0, xmm1 / setbe [esp+E] / cmp byte ptr [0x154E162], 1 / lookup call and its key push.
        const auto check = CodeCheck().Bytes(0x6C14A4, {0x0F,0x2F,0xC1,0x0F,0x96,0x44,0x24,0x0E,0x80,0x3D})
            .Address(0x6C14AE, 0x114E162).Bytes(0x6C14B2, {0x01})
            .Bytes(0x6C1234, {0xFF,0x77,0x38,0xE8}).Branch(0x6C1238, 0x525DB0);
        if (!check) { status = check.Status(); return; }
        gateHook = safetyhook::create_mid(GameBase() + 0x6C14AC, Gate);
        status = gateHook ? "installed" : "hook failed";
    }
}
