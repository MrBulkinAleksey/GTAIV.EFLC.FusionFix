module;

#include <common.hxx>
#include "FusionLog.hpp"
#include <algorithm>
#include <cmath>

export module farlights;

import common;
import comvars;

// Map and traffic lights from farther away ([LIGHTING] FarLights).
//
// A map entity registers its 2dfx lights as it is readied for the frame (CE 0xC1DB60). One not seen by any phase
// ([esi+8] & [0x159B75C]) does so only within 35 m of the camera (comiss with [0xE8B800] = 35², CE 0xC1DBFA): a
// lamp just out of view no longer lit the road in it. Each light then fades from its authored distance (+0x60 light,
// +0x64 shaft, -1 = none) over its radius, so many street and shop lights went out a short way down the street.
// Traffic signals light from CE 0xD208F0 only while their object is seen (je at CE 0xD2090F), and beyond 80 m
// ([0xFE8CA4] = 80², CE 0xB272A9 / 0xB27559 / 0xB27655) a signal is a dummy with no lights at all.
//
// Radius, colour and intensity stay the game's; only where a light is admitted changes. The frame's light list keeps
// its limit (0x280), so the far lights compete for it with the near ones.
namespace FarLights
{
    static float offscreenSquared = 100.0f * 100.0f;   // FarLightsOffscreenDistance: unseen entities' lights
    static float fadeDistance = 350.0f;                 // FarLightsDistance: the least fade distance
    static float trafficObjectSquared = 200.0f * 200.0f; // FarLightsTrafficObjects: signals kept as objects
    static float stockObjectSquared = 80.0f * 80.0f;    // [0xFE8CA4], every other object
    static const float* camera = nullptr;               // CE 0x128E340, the position the game's own test uses
    static uintptr_t modelInfos = 0;                    // CE 0x1295CD8, model infos by index

    static float DistanceSquared(uintptr_t entity)
    {
        float position[3];
        if (!CEntity::GetPosition(entity, position)) return 1e30f;
        const float dx = position[0] - camera[0], dy = position[1] - camera[1], dz = position[2] - camera[2];
        return dx * dx + dy * dy + dz * dz;
    }

    // Model type 3, the one CE 0xA32838 sends to the traffic signal lights.
    static bool IsTrafficSignal(uintptr_t entity)
    {
        if (entity < 0x10000) return false;
        const auto index = *reinterpret_cast<const int16_t*>(entity + 0x2E);
        if (index < 0 || index >= 31000) return false;
        const auto info = *reinterpret_cast<const uintptr_t*>(modelInfos + index * 4);
        return info && (*reinterpret_cast<const uint32_t*>(info + 0x40) & 0xFF000000) == 0x03000000;
    }

    static float ObjectSquared(uintptr_t entity)
    {
        return IsTrafficSignal(entity) ? trafficObjectSquared : stockObjectSquared;
    }

    // ja: CF and ZF clear take it, ZF set falls through.
    static void SetAbove(SafetyHookContext& regs, bool above)
    {
        if (above) regs.eflags &= ~0x41u;
        else regs.eflags |= 0x40u;
    }

    static float LeastFade(float distance)
    {
        return distance >= 0.0f ? std::max(distance, fadeDistance) : distance;
    }

    static void Install(CIniReader& iniReader)
    {
        fadeDistance = std::clamp(iniReader.ReadFloat("LIGHTING", "FarLightsDistance", 350.0f), 35.0f, 1000.0f);
        const float offscreen = std::clamp(iniReader.ReadFloat("LIGHTING", "FarLightsOffscreenDistance", 100.0f), 35.0f, 350.0f);
        const float objects = iniReader.ReadFloat("LIGHTING", "FarLightsTrafficObjects", 200.0f);
        offscreenSquared = offscreen * offscreen;

        auto offscreenGate = hook::pattern("F3 0F 5C 0D ? ? ? ? F3 0F 10 40 08 F3 0F 5C 05 ? ? ? ? F3 0F 59 D2 F3 0F 59 C9 F3 0F 59 C0 F3 0F 58 D1 F3 0F 58 D0 0F 2F 15 ? ? ? ? 0F 87");
        auto lightFade = hook::pattern("F3 0F 10 41 60 F3 0F 10 15 ? ? ? ? 0F 2E C2");
        auto shaftFade = hook::pattern("F3 0F 10 71 64 F3 0F 10 69 38");
        auto signalGate = hook::pattern("85 46 08 0F 84 ? ? ? ? 39 7E 20");
        if (offscreenGate.empty() || lightFade.empty() || shaftFade.empty() || signalGate.empty())
        {
            FusionLog::Write("Lighting", "FarLights", "map light code not found (%d %d %d %d), not changed\n",
                !offscreenGate.empty(), !lightFade.empty(), !shaftFade.empty(), !signalGate.empty());
            return;
        }
        camera = *offscreenGate.get_first<const float*>(4); // subss xmm1, [camera x]

        static auto offscreenHook = safetyhook::create_mid(offscreenGate.get_first(0x30), [](SafetyHookContext& regs)
        {
            SetAbove(regs, regs.xmm2.f32[0] > offscreenSquared);
        });
        static auto lightFadeHook = safetyhook::create_mid(lightFade.get_first(5), [](SafetyHookContext& regs)
        {
            regs.xmm0.f32[0] = LeastFade(regs.xmm0.f32[0]);
        });
        static auto shaftFadeHook = safetyhook::create_mid(shaftFade.get_first(5), [](SafetyHookContext& regs)
        {
            regs.xmm6.f32[0] = LeastFade(regs.xmm6.f32[0]);
        });
        // je over the signal's lights while unseen: not taken within the offscreen distance.
        static auto signalHook = safetyhook::create_mid(signalGate.get_first(3), [](SafetyHookContext& regs)
        {
            if ((regs.eflags & 0x40u) && DistanceSquared(regs.esi) <= offscreenSquared)
                regs.eflags &= ~0x40u;
        });

        FusionLog::Write("Lighting", "FarLights", "map and traffic lights fade from %.0f m at least, unseen ones lit within %.0f m\n",
            fadeDistance, offscreen);

        if (objects <= 0.0f)
            return;
        auto classifier = hook::pattern("0F BF 46 2E 5F 8B 04 85 ? ? ? ? 8B 40 40 25 00 00 00 FF 3D 00 00 00 03");
        auto activation = hook::pattern("F3 0F 10 05 ? ? ? ? 0F 2F C2 76 07 51 E8");
        auto timer = hook::pattern("0F 2F 25 ? ? ? ? 0F 87 ? ? ? ? 8B 86");
        auto retirement = hook::pattern("F3 0F 10 05 ? ? ? ? 0F 2F E0 76 19 0F 2F D0");
        if (classifier.empty() || activation.empty() || timer.empty() || retirement.empty())
        {
            FusionLog::Write("Lighting", "FarLights", "object range code not found (%d %d %d %d), traffic signals stay objects to 80 m\n",
                !classifier.empty(), !activation.empty(), !timer.empty(), !retirement.empty());
            return;
        }
        modelInfos = *classifier.get_first<uintptr_t>(8);
        stockObjectSquared = **activation.get_first<const float*>(4);
        trafficObjectSquared = std::max(std::clamp(objects, 80.0f, 350.0f) * std::clamp(objects, 80.0f, 350.0f), stockObjectSquared);

        // A dummy within the distance becomes an object (ECX the dummy; xmm0 the limit, xmm2 its distance²).
        static auto activationHook = safetyhook::create_mid(activation.get_first(8), [](SafetyHookContext& regs)
        {
            regs.xmm0.f32[0] = ObjectSquared(regs.ecx);
        });
        // An object past the distance (xmm4) is passed over (ja; ESI the object).
        static auto timerHook = safetyhook::create_mid(timer.get_first(7), [](SafetyHookContext& regs)
        {
            SetAbove(regs, regs.xmm4.f32[0] > ObjectSquared(regs.esi));
        });
        // and one past it again goes back to a dummy (xmm0 the limit).
        static auto retirementHook = safetyhook::create_mid(retirement.get_first(8), [](SafetyHookContext& regs)
        {
            regs.xmm0.f32[0] = ObjectSquared(regs.esi);
        });

        FusionLog::Write("Lighting", "FarLights", "traffic signals stay objects to %.0f m, other objects %.0f m\n",
            std::sqrt(trafficObjectSquared), std::sqrt(stockObjectSquared));
    }
}

class FarLightsInit
{
public:
    FarLightsInit()
    {
        FusionFix::onInitEventAsync() += []()
        {
            CIniReader iniReader("");
            if (iniReader.ReadInteger("LIGHTING", "FarLights", 0) != 0)
                FarLights::Install(iniReader);
        };
    }
} FarLightsInit;
