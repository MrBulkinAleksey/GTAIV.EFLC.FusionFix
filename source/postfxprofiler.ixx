module;

#include <common.hxx>
#include <algorithm>
#include <string>
#include <unordered_map>
#include <vector>
#include "FusionLog.hpp"

export module postfxprofiler;

import common;
import comvars;

#ifndef SAFE_RELEASE
#define SAFE_RELEASE(p) { if (p) { (p)->Release(); (p)=NULL; } }
#endif

namespace PostFxProfiler
{
    // PostFxProfiler in the ini, live: Ctrl+Shift+F10 turns it on and off in game. Off, no query is made.
    export inline bool bEnabled = false;

    // PostFxProfiler: GPU time of FusionFix's passes, from timestamp queries. Each frame's queries
    // are read kProfilerFrames frames later, without waiting on the GPU, and every 120 frames the
    // averages in milliseconds per frame are written to GTAIV.EFLC.FusionFix.PostFx.log next to the plugin.
    // The sections form a tree: an effect and the passes it is made of, each pass inside its
    // effect's time. What of a section its passes leave is shown as "other": in the lighting phase
    // that is the game's lights with their local contact shadows, and the light shafts. A section
    // can be entered more than once a frame, as the water is, and its times add up. The GGX
    // highlights, the sky reflection's BRDF and the rain's rings on the water are a few
    // instructions more in the game's own lighting and water shaders: they show in the lighting
    // phase's lamps, sun and water, not as sections of their own. The lighting phase's game part is
    // split by where the game is (SetLightingStage): its sun and ambient once FusionFix's passes are
    // done, its lamps from the first light of the light loop, its shafts from the shaft loop.
    // A frame runs from one post processing to the next. Off, no query is made.
    export enum ProfilerSection
    {
        kProfLighting,
            kProfAO, kProfAODepth, kProfAOMain, kProfAOTemporal, kProfAOBlur, kProfAOApply,
            kProfSSR, kProfSSRTrace, kProfSSRFill, kProfSSRResolve, kProfSSRDenoise, kProfSSRTemporal, kProfSSRDebug,
            kProfContact, kProfContactMarch, kProfContactUpsample, kProfContactDenoise, kProfContactTemporal,
            kProfGI, kProfGIMarch, kProfGIDenoise, kProfGITemporal, kProfGIUpsample,
            kProfLightSun, kProfLightLocal, kProfLightShafts, kProfGlints,
        kProfDepthCopy,
        kProfHiZ,
        kProfMotion,
        kProfWetGround, kProfWetGroundCopies, kProfWetGroundPass,
        kProfWaterAll, kProfWaterGame, kProfWater,
        kProfCloudReflection, kProfCloudReflectionMap, kProfCloudReflectionWater,
        kProfFogPass,
            kProfClouds, kProfCloudsSkyRef, kProfCloudsMarch, kProfCloudsLight, kProfCloudsResolve, kProfCloudsComposite,
            kProfSkin, kProfSkinLight, kProfSkinScatter, kProfSkinFinal,
            kProfFog,
        kProfResolve,
            kProfResolveReactive, kProfResolveUpscale,
        kProfPost,
            kProfPostTAA, kProfPostStipple, kProfPostDOF, kProfPostSunShafts, kProfPostGame, kProfPostAA, kProfPostSharpen,
        kProfHDROutput,
        kProfFrameGeneration,
            kProfFrameGenerationCopies, kProfFrameGenerationGenerate, kProfFrameGenerationShow,
        kProfFrameGenerationPresent,
        kProfSections
    };
    struct ProfilerSectionInfo { const char* name; int parent; };
    static constexpr ProfilerSectionInfo kProfilerSectionInfo[] =
    {
        { "lighting phase", -1 },
            { "ambient occlusion", kProfLighting }, { "depth and its mips", kProfAO }, { "occlusion", kProfAO },
            { "accumulation", kProfAO }, { "blur", kProfAO }, { "into the G-buffer", kProfAO },
            { "SSR", kProfLighting }, { "march", kProfSSR }, { "fill of misses", kProfSSR }, { "resolve", kProfSSR },
            { "smoothing", kProfSSR }, { "accumulation", kProfSSR }, { "debug view", kProfSSR },
            { "contact shadows", kProfLighting }, { "march", kProfContact }, { "upsample", kProfContact },
            { "smoothing", kProfContact }, { "accumulation", kProfContact },
            { "indirect light", kProfLighting }, { "march", kProfGI }, { "smoothing", kProfGI },
            { "accumulation", kProfGI }, { "upsample", kProfGI },
            { "the game's sun and ambient", kProfLighting }, { "the game's lamps and headlights", kProfLighting },
            { "the game's light shafts", kProfLighting }, { "headlight glints", kProfLightShafts },
        { "depth copy at the G-buffer's end", -1 },
        { "Hi-Z for occlusion culling", -1 },
        { "motion vectors (temporal AA, upscaling)", -1 },
        { "wet ground", -1 }, { "copies of the G-buffer", kProfWetGround }, { "wet pass", kProfWetGround },
        { "water", -1 }, { "the game's water", kProfWaterAll }, { "SSR on the water", kProfWaterAll },
        { "clouds in reflections", -1 }, { "the reflection map", kProfCloudReflection }, { "the water's reflection", kProfCloudReflection },
        { "fog pass", -1 },
            { "volumetric clouds", kProfFogPass }, { "sky brightness", kProfClouds }, { "march", kProfClouds },
            { "light", kProfClouds }, { "accumulation", kProfClouds }, { "into the scene", kProfClouds },
            { "skin scattering", kProfFogPass }, { "light on skin", kProfSkin }, { "scatter", kProfSkin },
            { "final", kProfSkin },
            { "the game's fog", kProfFogPass },
        { "temporal AA / upscaling", -1 },
            { "reactive mask", kProfResolve }, { "DLSS / FSR, copies and wait included", kProfResolve },
        { "post processing", -1 },
            { "temporal AA", kProfPost }, { "stipple filter", kProfPost }, { "depth of field", kProfPost },
            { "sun shafts", kProfPost }, { "the game's post processing", kProfPost }, { "FXAA / SMAA", kProfPost },
            { "sharpening", kProfPost },
        { "HDR output", -1 },
        { "frame generation", -1 },
            { "frame made ready for it", kProfFrameGeneration }, { "generation (with its wait unless deferred)", kProfFrameGeneration },
            { "generated frame into the back buffer", kProfFrameGeneration },
        { "frames presented inside the next frame", -1 },
    };
    static_assert(std::size(kProfilerSectionInfo) == kProfSections);
    // The game's own passes, between FusionFix's top level sections, are timed by the render target the
    // game has locked (grcTextureFactoryPC::LockRenderTarget, index 0): shadow maps, the G-buffer, the
    // reflection maps and so on, each a section of its own after kProfSections, named by the target as
    // the game first locks it. kProfTargetScreen stands for what the game draws with no target locked.
    static constexpr int kProfilerTargets = 64;
    static constexpr int kProfTargetScreen = kProfSections;
    static constexpr int kProfAll = kProfSections + kProfilerTargets;
    static constexpr int kProfilerFrames = 4;
    static constexpr int kProfilerAverage = 120;
    static constexpr int kProfilerStamps = 512; // timestamps a frame, two per section entered
    struct ProfilerFrame
    {
        IDirect3DQuery9* disjoint = nullptr;
        IDirect3DQuery9* freq = nullptr;
        IDirect3DQuery9* start = nullptr;
        IDirect3DQuery9* stop = nullptr;
        // Made as they are first needed, and kept for the frames after
        IDirect3DQuery9* stamps[kProfilerStamps] = {};
        struct { int section; bool begin; } marks[kProfilerStamps] = {};
        int count = 0;
        bool issued = false;
    };
    static ProfilerFrame profilerFrames[kProfilerFrames];
    static int nProfilerFrame = -1;
    static double profilerSums[kProfAll] = {};
    static std::string profilerTargetNames[kProfilerTargets] = { "no target locked (the screen)" };
    static int nProfilerTargets = 1;
    static std::unordered_map<const void*, int> profilerTargetIds; // the game's targets by address
    static double profilerFrameSum = 0.0;
    static int nProfilerSamples = 0;

    // Whether the profiler ran last frame: PostFxProfiler is a live setting, Ctrl+Shift+F10 turns it on
    // and off in game.
    static bool bProfilerRunning = false;

    // The game's target sections (see kProfilerTargets): the targets it has locked, innermost last; the
    // section open now, -1 for none; and how many of FusionFix's top level sections are open, which
    // pause the game's while they run.
    static std::vector<int> profilerTargetStack;
    static int nProfilerOpenTarget = -1;
    static int nProfilerOwnSections = 0;

    // Defined further down, in the order the frame uses them
    export void ResetProfilerSums();
    static void ProfilerStamp(IDirect3DDevice9* pDevice, int section, bool begin);
    static void SwitchProfilerTarget(IDirect3DDevice9* pDevice);
    static void InstallProfilerTargetHooks();

    static void ReleaseProfilerFrame(ProfilerFrame& f)
    {
        SAFE_RELEASE(f.disjoint);
        SAFE_RELEASE(f.freq);
        SAFE_RELEASE(f.start);
        SAFE_RELEASE(f.stop);
        for (auto& q : f.stamps)
            SAFE_RELEASE(q);
        f.count = 0;
        f.issued = false;
    }

    export void ReleaseProfiler()
    {
        for (auto& f : profilerFrames)
            ReleaseProfilerFrame(f);
        nProfilerFrame = -1;
    }

    static bool CreateProfilerFrame(IDirect3DDevice9* pDevice, ProfilerFrame& f)
    {
        if (f.disjoint)
            return true;
        bool ok = SUCCEEDED(pDevice->CreateQuery(D3DQUERYTYPE_TIMESTAMPDISJOINT, &f.disjoint)) &&
                  SUCCEEDED(pDevice->CreateQuery(D3DQUERYTYPE_TIMESTAMPFREQ, &f.freq)) &&
                  SUCCEEDED(pDevice->CreateQuery(D3DQUERYTYPE_TIMESTAMP, &f.start)) &&
                  SUCCEEDED(pDevice->CreateQuery(D3DQUERYTYPE_TIMESTAMP, &f.stop));
        if (!ok)
            ReleaseProfilerFrame(f);
        return ok;
    }

    // A section, its passes below it, then what they leave of it.
    static void WriteProfilerSection(FusionLog::Block& log, int section, int depth, double n)
    {
        log.Printf("%*s%-*s %6.2f\n", 2 + depth * 2, "", 34 - depth * 2, kProfilerSectionInfo[section].name, profilerSums[section] / n);
        double children = 0.0;
        bool any = false;
        for (int i = 0; i < kProfSections; ++i)
        {
            if (kProfilerSectionInfo[i].parent != section || profilerSums[i] <= 0.0)
                continue;
            WriteProfilerSection(log, i, depth + 1, n);
            children += profilerSums[i];
            any = true;
        }
        if (any)
            log.Printf("%*s%-*s %6.2f\n", 4 + depth * 2, "", 32 - depth * 2, "other", (std::max)(profilerSums[section] - children, 0.0) / n);
    }

    static void WriteProfilerBlock()
    {
        {
            FusionLog::Block log("PostFx", "Profiler");
            const double n = double(nProfilerSamples);
            log.Printf("GPU milliseconds per frame, averaged over %d frames\n", nProfilerSamples);
            log.Printf("%-36s %6.2f\n", "frame", profilerFrameSum / n);
            double sections = 0.0;
            for (int i = 0; i < kProfSections; ++i)
            {
                if (kProfilerSectionInfo[i].parent >= 0 || profilerSums[i] <= 0.0)
                    continue;
                WriteProfilerSection(log, i, 0, n);
                sections += profilerSums[i];
            }
            const double game = (std::max)(profilerFrameSum - sections, 0.0);
            log.Printf("  %-34s %6.2f\n", "the game's other passes", game / n);
            // By the target the game drew into, the most expensive first; what is left over is time no target
            // accounts for, such as the GPU waiting on the game.
            int order[kProfilerTargets];
            for (int i = 0; i < nProfilerTargets; ++i)
                order[i] = i;
            std::sort(order, order + nProfilerTargets,
                      [](int a, int b) { return profilerSums[kProfSections + a] > profilerSums[kProfSections + b]; });
            double targets = 0.0;
            for (int i = 0; i < nProfilerTargets; ++i)
            {
                const double t = profilerSums[kProfSections + order[i]];
                if (t / n < 0.005)
                    continue;
                log.Printf("    %-32s %6.2f\n", profilerTargetNames[order[i]].c_str(), t / n);
                targets += t;
            }
            if (targets > 0.0)
                log.Printf("    %-32s %6.2f\n", "other", (std::max)(game - targets, 0.0) / n);
        }
        ResetProfilerSums();
    }

    // Adds a frame whose queries were issued kProfilerFrames frames ago, if the GPU has them.
    static void ReadProfilerFrame(ProfilerFrame& f)
    {
        BOOL disjoint = TRUE;
        UINT64 freq = 0, start = 0, stop = 0;
        if (f.disjoint->GetData(&disjoint, sizeof(disjoint), 0) != S_OK || disjoint ||
            f.freq->GetData(&freq, sizeof(freq), 0) != S_OK || !freq ||
            f.start->GetData(&start, sizeof(start), 0) != S_OK || f.stop->GetData(&stop, sizeof(stop), 0) != S_OK)
            return;
        double ms[kProfAll] = {};
        UINT64 open[kProfAll] = {};
        bool opened[kProfAll] = {};
        for (int i = 0; i < f.count; ++i)
        {
            UINT64 t = 0;
            if (f.stamps[i]->GetData(&t, sizeof(t), 0) != S_OK)
                return;
            const int section = f.marks[i].section;
            if (f.marks[i].begin)
            {
                open[section] = t;
                opened[section] = true;
            }
            else if (opened[section] && t >= open[section])
            {
                ms[section] += double(t - open[section]) * 1000.0 / double(freq);
                opened[section] = false;
            }
        }
        for (int i = 0; i < kProfAll; ++i)
            profilerSums[i] += ms[i];
        profilerFrameSum += double(stop - start) * 1000.0 / double(freq);
        if (++nProfilerSamples >= kProfilerAverage)
            WriteProfilerBlock();
    }

    // Starts the averages over, as the profiler is turned on and as the ini is read again in game, so a
    // block holds no frames from before.
    export void ResetProfilerSums()
    {
        std::fill(std::begin(profilerSums), std::end(profilerSums), 0.0);
        profilerFrameSum = 0.0;
        nProfilerSamples = 0;
    }

    // Once a frame, as post processing begins: closes this frame's queries and opens the next.
    export void ProfilerNextFrame(IDirect3DDevice9* pDevice)
    {
        if (!pDevice)
            return;
        if (!bEnabled)
        {
            if (bProfilerRunning)
            {
                ReleaseProfiler();
                ResetProfilerSums();
                nProfilerOpenTarget = -1;
                nProfilerOwnSections = 0;
                bProfilerRunning = false;
                FusionLog::Block log("PostFx", "Profiler");
                log.Printf("off\n");
            }
            return;
        }
        if (!bProfilerRunning)
        {
            ResetProfilerSums();
            bProfilerRunning = true;
            FusionLog::Block log("PostFx", "Profiler");
            log.Printf("on, a block every %d frames\n", kProfilerAverage);
        }
        InstallProfilerTargetHooks();
        // The target section open at the frame's end is closed in this frame and opened again in the next.
        const int target = nProfilerOpenTarget;
        if (target >= 0)
            ProfilerStamp(pDevice, target, false);
        nProfilerOpenTarget = -1;
        nProfilerOwnSections = 0;
        if (nProfilerFrame >= 0 && profilerFrames[nProfilerFrame].issued)
        {
            auto& cur = profilerFrames[nProfilerFrame];
            cur.stop->Issue(D3DISSUE_END);
            cur.freq->Issue(D3DISSUE_END);
            cur.disjoint->Issue(D3DISSUE_END);
        }
        nProfilerFrame = (nProfilerFrame + 1) % kProfilerFrames;
        auto& f = profilerFrames[nProfilerFrame];
        if (f.issued)
        {
            ReadProfilerFrame(f);
            f.issued = false;
        }
        if (!CreateProfilerFrame(pDevice, f))
        {
            bEnabled = false; // no timestamp queries on this device
            ReleaseProfiler();
            return;
        }
        f.count = 0;
        f.disjoint->Issue(D3DISSUE_BEGIN);
        f.start->Issue(D3DISSUE_END);
        f.issued = true;
        SwitchProfilerTarget(pDevice);
    }

    static void ProfilerStamp(IDirect3DDevice9* pDevice, int section, bool begin)
    {
        if (!bEnabled || !pDevice || nProfilerFrame < 0 || section < 0)
            return;
        auto& f = profilerFrames[nProfilerFrame];
        if (!f.issued || f.count >= kProfilerStamps)
            return;
        auto& q = f.stamps[f.count];
        if (!q && FAILED(pDevice->CreateQuery(D3DQUERYTYPE_TIMESTAMP, &q)))
            return;
        q->Issue(D3DISSUE_END);
        f.marks[f.count++] = { section, begin };
    }

    static void SwitchProfilerTarget(IDirect3DDevice9* pDevice)
    {
        if (!bEnabled)
            return;
        const int want = nProfilerOwnSections > 0 ? -1
                       : kProfTargetScreen + (profilerTargetStack.empty() ? 0 : profilerTargetStack.back());
        if (want == nProfilerOpenTarget)
            return;
        if (nProfilerOpenTarget >= 0)
            ProfilerStamp(pDevice, nProfilerOpenTarget, false);
        if (want >= 0)
            ProfilerStamp(pDevice, want, true);
        nProfilerOpenTarget = want;
    }

    export void ProfilerMark(IDirect3DDevice9* pDevice, int section, bool begin)
    {
        if (!bEnabled || !pDevice || nProfilerFrame < 0 || section < 0)
            return;
        const bool topLevel = section < kProfSections && kProfilerSectionInfo[section].parent < 0;
        if (topLevel && begin)
        {
            ++nProfilerOwnSections;
            SwitchProfilerTarget(pDevice);
        }
        ProfilerStamp(pDevice, section, begin);
        if (topLevel && !begin && nProfilerOwnSections > 0)
        {
            --nProfilerOwnSections;
            SwitchProfilerTarget(pDevice);
        }
    }

    // The game's targets come and go through grcTextureFactoryPC's LockRenderTarget and
    // UnlockRenderTarget (vtable slots 15 and 16), which the game only calls through the vtable. The
    // two entries are replaced once the profiler runs, and only then.
    using LockRenderTargetFn = void(__thiscall*)(void*, uint32_t, rage::grcRenderTargetPC*, rage::grcRenderTargetPC*, uint32_t, bool, uint32_t);
    using UnlockRenderTargetFn = void(__thiscall*)(void*, uint32_t, void*, int32_t);
    static LockRenderTargetFn pfnLockRenderTarget = nullptr;
    static UnlockRenderTargetFn pfnUnlockRenderTarget = nullptr;

    static int ProfilerTargetId(rage::grcRenderTargetPC* rt)
    {
        if (!rt)
            return 0;
        std::string name = rt->mName ? rt->mName : "unnamed target";
        // A target the game made again, as on a resolution change, can take the address of another.
        if (auto it = profilerTargetIds.find(rt); it != profilerTargetIds.end() && (it->second == 0 || profilerTargetNames[it->second] == name))
            return it->second;
        int id = -1;
        for (int i = 1; i < nProfilerTargets; ++i)
            if (profilerTargetNames[i] == name)
                id = i;
        if (id < 0 && nProfilerTargets < kProfilerTargets)
        {
            id = nProfilerTargets++;
            profilerTargetNames[id] = std::move(name);
        }
        if (id < 0)
            id = 0; // out of sections: counted with the screen
        profilerTargetIds[rt] = id;
        return id;
    }

    static void __fastcall ProfilerLockRenderTarget(void* factory, void*, uint32_t index, rage::grcRenderTargetPC* color,
                                                    rage::grcRenderTargetPC* depth, uint32_t a5, bool a6, uint32_t mip)
    {
        pfnLockRenderTarget(factory, index, color, depth, a5, a6, mip);
        // Followed with the profiler off too, so the stack is right when it is turned on again in game.
        if (index != 0)
            return;
        // Shadow maps are drawn into a depth target alone. A lock the game never undid would grow the stack
        // for good; deeper than any nesting it uses, the stack starts over.
        if (profilerTargetStack.size() >= 16)
            profilerTargetStack.clear();
        profilerTargetStack.push_back(bEnabled ? ProfilerTargetId(color ? color : depth) : 0);
        SwitchProfilerTarget(rage::grcDevice::GetD3DDevice());
    }

    static void __fastcall ProfilerUnlockRenderTarget(void* factory, void*, uint32_t index, void* resolveFlags, int32_t unused)
    {
        pfnUnlockRenderTarget(factory, index, resolveFlags, unused);
        if (index != 0 || profilerTargetStack.empty())
            return;
        profilerTargetStack.pop_back();
        SwitchProfilerTarget(rage::grcDevice::GetD3DDevice());
    }

    static void InstallProfilerTargetHooks()
    {
        if (pfnLockRenderTarget || !rage::grcTextureFactory::g_pTextureFactory)
            return;
        auto* factory = rage::grcTextureFactoryPC::GetInstance();
        if (!factory)
            return;
        auto* vft = *reinterpret_cast<uintptr_t**>(factory);
        pfnLockRenderTarget = reinterpret_cast<LockRenderTargetFn>(vft[15]);
        pfnUnlockRenderTarget = reinterpret_cast<UnlockRenderTargetFn>(vft[16]);
        injector::WriteMemory(&vft[15], reinterpret_cast<uintptr_t>(&ProfilerLockRenderTarget), true);
        injector::WriteMemory(&vft[16], reinterpret_cast<uintptr_t>(&ProfilerUnlockRenderTarget), true);
    }

    // Times what runs from here to the end of the scope; a section below 0 times nothing.
    export struct ProfilerScope
    {
        IDirect3DDevice9* device;
        int section;
        ProfilerScope(IDirect3DDevice9* pDevice, int s) : device(pDevice), section(s) { ProfilerMark(device, section, true); }
        ~ProfilerScope() { ProfilerMark(device, section, false); }
        ProfilerScope(const ProfilerScope&) = delete;
        ProfilerScope& operator=(const ProfilerScope&) = delete;
    };
}
