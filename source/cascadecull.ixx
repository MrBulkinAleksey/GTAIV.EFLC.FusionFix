module;

#include <common.hxx>
#include "FusionLog.hpp"
#include <atomic>
#include <format>
#include <string>

export module cascadecull;

import common;
import comvars;
import renderskip;
import shadows;

// Small things left out of the sun's two far cascades. The cascades' phase (CRenderPhaseCascadeShadows) has one render
// list for all four of them: the scan gives each entity it takes a byte of the cascades its bounds reach (entity +0x62,
// written at CE 0xae6a25 from CE 0xae56d0), and the phase records the list four times over, nearest cascade first, each
// time only what has that cascade's bit (CE 0xae4db0 with 1 << cascade at CE 0x11a1bd0, set at CE 0x928be7). A cascade
// covers about twice its far edge (CascadeRanges) in its side of texels, so in the fourth a texel is some 25 cm at 2048;
// a thing whose bounding sphere is a few texels across there shadows a speck, and still costs its draws.
//  - CascadeMinTexels = 4: the third and fourth cascades leave out what is narrower than this many of their texels
//    across. The near two keep everything, as do the far ones for anything larger. 0: nothing left out.
//  - CascadeCullLog = 1: every 3600 frames, how many of the far cascades' entities were left out.
export namespace CascadeCull
{
    float fMinTexels = 4.0f;
    bool bLog = false;
}

namespace CascadeCull
{
    constexpr uint32_t DefaultSize = 2048;  // until the game has drawn a cascade
    constexpr int FirstFar = 2;

    std::atomic<uint32_t> nIn[4]{}, nLeftOut[4]{};

    uint8_t Cull(uint8_t cascades, float radius)
    {
        if (!(cascades & ~((1 << FirstFar) - 1)))
            return cascades;
        uint32_t size = RenderSkip::nCascadeSize ? RenderSkip::nCascadeSize : DefaultSize;
        for (int i = FirstFar; i < 4; i++)
        {
            if (!(cascades & (1 << i)))
                continue;
            if (bLog)
                nIn[i].fetch_add(1, std::memory_order_relaxed);
            // Across in texels: twice the radius over a texel of 2 * range / size
            if (radius * float(size) / CascadeRanges[i] < fMinTexels)
            {
                cascades &= ~(1 << i);
                if (bLog)
                    nLeftOut[i].fetch_add(1, std::memory_order_relaxed);
            }
        }
        return cascades;
    }
}

class CascadeCullFeature
{
public:
    CascadeCullFeature()
    {
        using namespace CascadeCull;

        FusionFix::onInitEventAsync() += []()
        {
            CIniReader iniReader("");
            fMinTexels = std::max(iniReader.ReadFloat("SHADOWS", "CascadeMinTexels", 4.0f), 0.0f);
            bLog = iniReader.ReadInteger("SHADOWS", "CascadeCullLog", 0) != 0;
            if (fMinTexels <= 0.0f)
                return;

            // The scan's last step for an entity of the cascades' list: esi the entity, cl the cascades it reaches,
            // [esp+0xc] its bounding radius (from its vtable +0x5c, on both of the function's ways here)
            auto pattern = hook::pattern("80 3D ? ? ? ? 00 B8 0F 00 00 00 0F 45 C8 88 4E 62 32 C0");
            if (pattern.empty())
            {
                FusionLog::Write("CascadeCull", "Settings", "the cascades' scan not found, nothing is left out\n");
                return;
            }
            static auto CascadesHook = safetyhook::create_mid(pattern.get_first(15), [](SafetyHookContext& regs)
            {
                auto cascades = Cull(uint8_t(regs.ecx), *(float*)(regs.esp + 0xC));
                regs.ecx = (regs.ecx & ~0xFFu) | cascades;
            });
            FusionLog::Write("CascadeCull", "Settings", "the far cascades leave out what is under %.1f of their texels across\n", fMinTexels);
        };

        FusionFix::onGameProcessEvent() += []()
        {
            static uint32_t frames = 0;
            if (!bLog || ++frames % 3600)
                return;
            std::string text = std::format("cascade size {}: ", RenderSkip::nCascadeSize);
            for (int i = FirstFar; i < 4; i++)
            {
                uint32_t in = nIn[i].exchange(0), out = nLeftOut[i].exchange(0);
                text += std::format("cascade {}: {} a frame, {} of them left out ({:.0f}%); ", i + 1, in / 3600, out / 3600,
                    in ? 100.0 * out / in : 0.0);
            }
            FusionLog::WriteText("CascadeCull", "Stats", text);
        };
    }
} CascadeCullFeature;
