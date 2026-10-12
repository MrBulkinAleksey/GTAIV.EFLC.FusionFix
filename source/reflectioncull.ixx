module;

#include <common.hxx>
#include "FusionLog.hpp"
#include <atomic>
#include <cmath>
#include <format>
#include <string>

export module reflectioncull;

import common;
import comvars;
import renderskip;

// Small and far things left out of the reflections. Each render phase takes a bit of the render lists' phase masks
// anew every frame (its +0x938, CE 0xb05750), and says what it is when it is registered for the frame (CE 0xae4880,
// its vtable +0x24): 0x11 the water's reflection, 0x13 the cars' reflection map, 0x14 the one in interiors; mirrors
// (0x24) are seen close and keep everything. The render lists (CE 0xae85b4 and 0xae706f, hooked in procedural.ixx)
// then drop those bits for an entity whose bounding sphere looks smaller than ReflectionMinSize from the game camera:
// in the water's reflection, at a quarter of the frame or less and broken up by the waves, and in a reflection map a
// few hundred texels round, such a thing is a pixel or two.
//  - ReflectionMinSize = 0.01: its radius over its distance, about 1.1 degrees across; a bin at 30 m, a car at 250 m.
//    0: nothing left out.
//  - ReflectionCullLog = 1: every 3600 frames, how many entities a frame the reflections had and left out.
export namespace ReflectionCull
{
    float fMinSize = 0.01f;
    bool bLog = false;

    uint32_t Drop(uintptr_t entity, uint32_t phases);
}

namespace ReflectionCull
{
    void** ppCurrentPhase = nullptr;    // CE 0x12fb1b8, the phase being registered
    uint32_t reflectionBits = 0;
    std::atomic<uint32_t> nIn{}, nLeftOut{};

    bool IsReflection(int32_t type)
    {
        return type == 0x11 || type == 0x13 || type == 0x14;
    }

    void Register()
    {
        auto phase = ppCurrentPhase ? *ppCurrentPhase : nullptr;
        if (!phase)
            return;
        int32_t bit = *(int32_t*)((uint8_t*)phase + 0x938);
        if (bit < 0 || bit >= 32)
            return;
        auto type = reinterpret_cast<int32_t(__thiscall*)(void*)>((*(void***)phase)[9])(phase);
        if (IsReflection(type))
            reflectionBits |= 1u << bit;
        else
            reflectionBits &= ~(1u << bit);
    }

    uint32_t Drop(uintptr_t entity, uint32_t phases)
    {
        uint32_t reflections = phases & reflectionBits;
        if (!reflections || fMinSize <= 0.0f || !RenderSkip::pCamera)
            return phases;
        // The bounding sphere, from the entity's vtable +0x5c as the cascades' scan takes it (CE 0xae6965)
        alignas(16) float centre[4]{};
        float radius = reinterpret_cast<float(__thiscall*)(void*, float*)>((*(void***)entity)[0x5C / 4])((void*)entity, centre);
        const float* camera = RenderSkip::pCamera;
        float dx = centre[0] - camera[0], dy = centre[1] - camera[1], dz = centre[2] - camera[2];
        float distanceSquared = dx * dx + dy * dy + dz * dz;
        if (bLog)
            nIn.fetch_add(1, std::memory_order_relaxed);
        if (!(radius > 0.0f) || radius * radius >= fMinSize * fMinSize * distanceSquared)
            return phases;
        if (bLog)
            nLeftOut.fetch_add(1, std::memory_order_relaxed);
        return phases & ~reflections;
    }
}

class ReflectionCullFeature
{
public:
    ReflectionCullFeature()
    {
        using namespace ReflectionCull;

        FusionFix::onInitEventAsync() += []()
        {
            CIniReader iniReader("");
            fMinSize = std::max(iniReader.ReadFloat("MISC", "ReflectionMinSize", 0.01f), 0.0f);
            bLog = iniReader.ReadInteger("MISC", "ReflectionCullLog", 0) != 0;
            if (fMinSize <= 0.0f)
                return;

            // The start of the registration: the phase is at [0x12fb1b8] (mov esi, [0x12fb1b8])
            auto pattern = hook::pattern("55 8B EC 83 E4 F0 83 EC 38 56 8B 35 ? ? ? ? 57 C6 46 1B 01");
            if (pattern.empty())
            {
                FusionLog::Write("ReflectionCull", "Settings", "the phases' registration not found, nothing is left out\n");
                fMinSize = 0.0f;
                return;
            }
            ppCurrentPhase = *pattern.get_first<void**>(12);
            static auto RegisterHook = safetyhook::create_mid(pattern.get_first(0), [](SafetyHookContext&)
            {
                Register();
            });
            FusionLog::Write("ReflectionCull", "Settings", "the reflections leave out what is under %.3f of its distance in radius\n", fMinSize);
        };

        FusionFix::onGameProcessEvent() += []()
        {
            static uint32_t frames = 0;
            if (!bLog || ++frames % 3600)
                return;
            uint32_t in = nIn.exchange(0), out = nLeftOut.exchange(0);
            FusionLog::WriteText("ReflectionCull", "Stats", std::format("{} entities a frame in reflections, {} of them left out ({:.0f}%), phase bits {:#x}",
                in / 3600, out / 3600, in ? 100.0 * out / in : 0.0, reflectionBits));
        };
    }
} ReflectionCullFeature;
