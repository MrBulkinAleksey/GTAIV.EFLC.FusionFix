module;

#include <common.hxx>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <array>
#include <filesystem>
#include <vector>

#include "FusionLog.hpp"

export module framegeneration;

import common;
import comvars;
import consolegamma;
import hdr;
import settings;
import upscaler;

#ifndef SAFE_RELEASE
#define SAFE_RELEASE(p) { if (p) { (p)->Release(); (p)=NULL; } }
#endif

// AMD FSR frame generation, run by the helper next to DLSS and FSR (see upscaler.ixx).
//
// - The upscaler's Evaluate prepares the frame generation with the frame's depth and motion vectors.
// - The finished scene before the HUD is copied into HudLess, right after the post processing. The HUD is what
//   differs from it, and the frame generation keeps it from the current frame instead of interpolating it. What is
//   drawn over the whole frame after the HUD, the console gamma and the HDR output, is drawn over the copy too.
// - Once the frame is finished, HUD and HDR output included, the back buffer is copied into Present and the helper
//   generates the frame between it and the previous one into Generated.
// - Frame Generation in the graphics menu (Advanced, with DLAA or FSR and AMD's frame generation library), deferred
//   (FrameGenerationDeferred, the default): the helper is handed the frame at its end and generates while the next frame
//   renders. The game's own Present is left out; a draw call of the render thread FrameGenerationShowGeneratedAt into the
//   next frame takes the generated frame from the helper and presents it, and one FrameGenerationDelay later presents
//   the rendered frame, which waits in PresentRT, both inside the game's scene, the back buffer left as it was found.
//   Without it the game presents the generated frame in place of its own, which the GPU waits for before it goes on, and
//   the rendered frame goes from a draw call FrameGenerationDelay into the next frame. A frame that ends before its
//   waiting frames went presents them first.
// - Sharpening (CAS) is left out of the post processing and drawn over the frames as they are shown, the rendered and
//   the generated one, with the HUD left as it is (SharpenShownFrame in postfx.ixx). Sharpened before the generation,
//   the shadow under a moving car went along with the ground in the generated frames and was shown twice.
// - Not in the menus, where nothing moves, nor with DLSS-IV loaded, which replays the game's D3D9 calls on a thread of
//   its own and so runs ahead of what the helper is handed.
// - [TEMPORAL] in the ini: FrameGenerationDelay, FrameGenerationPacing, and FrameGenerationDebug: 1 marks which frames
//   reach the screen, 2 logs how far between its neighbours a generated frame is (reads frames back, slow), 4 shows
//   the generated frames in place of the rendered ones, 8 logs the pacing.

namespace
{
    enum class Mode : int32_t
    {
        Off = 0,
        On = 1,
        ShowGenerated = 2,  // the generated frame replaces the rendered one
    };

    Mode mode = Mode::Off;
    float fDelay = 0.5f;            // of the frame's own time, between the generated and the rendered frame
    // FrameGenerationDeferred: the helper generates while the next frame renders, and both frames are presented inside
    // it, the generated one FrameGenerationShowGeneratedAt into it; the game's own Present is left out. Without it the
    // game's Present shows the generated frame, which the GPU waits for before it goes on with the next frame.
    bool bDeferredSetting = true;
    float fShowGeneratedAt = 0.2f;
    bool bDeferredBroken = false;   // the game's Present did not come through the device: not deferred any more

    // FrameGenerationDebug in [TEMPORAL]
    namespace Debug
    {
        constexpr int32_t Marker = 1;       // a square in the corner: magenta on generated frames, green on rendered ones
        constexpr int32_t Similarity = 2;   // how much the generated frame differs from the rendered ones around it
        constexpr int32_t ShowGenerated = 4; // only the generated frames are shown, to check the generation itself
        constexpr int32_t PacingLog = 8;    // the pacing on the CPU and the GPU, every 300 frames
    }
    int32_t nDebug = 0;

    rage::grcRenderTargetPC* PresentRT = nullptr;
    rage::grcRenderTargetPC* GeneratedRT = nullptr;
    rage::grcRenderTargetPC* HudLessRT = nullptr;
    rage::grcRenderTargetPC* SavedRT = nullptr;     // the back buffer while the rendered frame is presented
    bool bHudLessCaptured = false;  // this frame

    // Sharpening: the post processing leaves it to the frames as they are shown (SharpenShownFrame in postfx.ixx), from
    // a frame into a target, with the finished frame and the one before the HUD to tell the HUD
    using Sharpener = bool (*)(IDirect3DDevice9* device, IDirect3DTexture9* frame, IDirect3DTexture9* present, IDirect3DTexture9* hudLess,
        IDirect3DSurface9* target);
    Sharpener Sharpen = nullptr;
    bool bSharpenDeferred = false;  // this frame
    // The frames waiting for their Present are sharpened as they go, straight into the back buffer, with the frame
    // before the HUD only while HudLessRT still holds theirs: a frame that ends first has put its own there
    bool bPendingRenderedSharpen = false;
    bool bPendingRenderedHudLess = false;
    uint32_t FrameNumber = 0;       // of the frame that ended last
    uint32_t HudLessFrame = 0;      // the frame HudLessRT holds
    uint32_t PendingFrame = 0;      // the frame the waiting frames came from
    uint32_t TargetWidth = 0;
    uint32_t TargetHeight = 0;
    bool bTargetsEightBit = false;

    // GTAIV.EFLC.FusionFix.FrameGeneration.log next to the plugin (FusionLog), each kind of failure once
    void Log(const char* format, ...)
    {
        va_list args;
        va_start(args, format);
        FusionLog::WriteV("FrameGeneration", "", format, args);
        va_end(args);
    }

    void LogOnce(int reason, const char* message)
    {
        static uint32_t logged = 0;
        if (logged & (1u << reason))
            return;
        logged |= 1u << reason;
        Log("%s", message);
    }

    // Pacing, render thread
    LARGE_INTEGER Frequency{};
    LARGE_INTEGER LastFrameEnd{};   // the previous frame's OnBeforePresent, 0 before the first
    double FrameMs = 0.0;           // smoothed time between two rendered frames
    DWORD RenderThread = 0;

    // The rendered frame waiting in PresentRT for its Present, after the generated one went
    bool bPending = false;
    // Deferred: the generated frame waiting for the helper and its Present, before the rendered one, and the game's
    // Present that is left out
    bool bPendingGenerated = false;
    bool bGeneratedTaken = false;           // copied out of the helper into GeneratedRT
    bool bPendingGeneratedSharpen = false;  // sharpened as it is shown
    bool bPendingGeneratedHudLess = false;
    bool bSkipPresent = false;
    bool bDeferredFlow = false;             // the waiting frames came from the deferred flow
    LARGE_INTEGER PendingGeneratedDue{};

    // Logs the order the frames go in whenever it changes
    void LogFlow(bool deferred)
    {
        static int logged = -1;
        if (logged == int(deferred))
            return;
        logged = int(deferred);
        Log(deferred ? "Generated frames: made while the next frame renders, both frames shown inside it"
                     : "Generated frames: shown by the game's Present at the frame's end, the rendered one inside the next frame");
    }
    bool bInPresent = false;
    bool bEndOfFrameOnly = false;   // the runtime refused a Present inside the game's scene
    bool bBackBufferDrawn = false;  // the post processing of this frame began: the back buffer has the frame from then on
    LARGE_INTEGER PendingDue{};
    LARGE_INTEGER GeneratedAt{};

    double Ms(LARGE_INTEGER from, LARGE_INTEGER to)
    {
        return static_cast<double>(to.QuadPart - from.QuadPart) * 1000.0 / static_cast<double>(Frequency.QuadPart);
    }

    LARGE_INTEGER Now()
    {
        LARGE_INTEGER now;
        QueryPerformanceCounter(&now);
        return now;
    }

    LARGE_INTEGER After(LARGE_INTEGER from, double ms)
    {
        from.QuadPart += static_cast<LONGLONG>(ms * static_cast<double>(Frequency.QuadPart) / 1000.0);
        return from;
    }

    // Statistics of the pacing, logged every few seconds
    struct PacingStats
    {
        uint32_t frames = 0, late = 0;
        double frameMs = 0.0, gapMs = 0.0, gapMin = 1e9, gapMax = 0.0, delayMs = 0.0;

        void Add(double gap, double delay, bool wasLate)
        {
            ++frames;
            late += wasLate;
            frameMs += FrameMs;
            gapMs += gap;
            delayMs += delay;
            gapMin = std::min(gapMin, gap);
            gapMax = std::max(gapMax, gap);
            if (frames < 300)
                return;
            if (nDebug & Debug::PacingLog)
                Log("Pacing over %u frames: frame %.2f ms, rendered frame %.2f ms after the generated one (%.2f..%.2f, aimed at %.2f), %u late",
                frames, frameMs / frames, gapMs / frames, gapMin, gapMax, delayMs / frames, late);
            *this = {};
        }
    } Stats;

    uint32_t DrawsThisFrame = 0;

    // ---------------------------------------------------------------------------------------------
    // Where in the next frame the rendered frame goes (FrameGenerationPacing)
    //
    // The frames reach the screen when the GPU gets to their Present in its commands, which it runs in order, and the
    // render thread usually runs ahead of it: the time on the CPU says little about the time on the GPU.
    // 0: the time on the CPU, FrameGenerationDelay of the smoothed frame time after the generated frame.
    // 1: the draw calls, FrameGenerationDelay of the previous frames' draw calls.
    // 2: the GPU: timestamps through the frame tell the draw call the GPU passes FrameGenerationDelay of its frame at,
    //    read back a few frames later without waiting; until there is one, as 1.
    //    Deferred, the generated frame goes FrameGenerationShowGeneratedAt into the frame and the rendered one
    //    FrameGenerationDelay after it, each by these rules.
    int32_t nPacing = 2;
    double DrawsEma = 0.0;          // draw calls of a frame, smoothed
    double TargetDraws[2]{};        // the draw calls for 2, the generated frame's and the rendered frame's, smoothed; 0 while unknown

    // Where in the frame a waiting frame goes: 0 the generated one (deferred), 1 the rendered one
    double Aim(int which)
    {
        if (which == 0)
            return fShowGeneratedAt;
        return bDeferredFlow ? std::min(fShowGeneratedAt + fDelay, 0.98f) : fDelay;
    }

    struct GpuFrame
    {
        IDirect3DQuery9* disjoint = nullptr;
        IDirect3DQuery9* frequency = nullptr;
        IDirect3DQuery9* presented = nullptr;       // right before the rendered frame's Present within the frame
        std::vector<IDirect3DQuery9*> queries;      // timestamps, reused
        std::vector<uint32_t> draws;                // the draw call of each one used, the first at 0, the last the frame's end
        bool hadPresent = false;
        bool issued = false;                        // the frame is over, its results are awaited
    };
    std::array<GpuFrame, 4> GpuFrames;
    uint32_t GpuSlot = 0;
    bool bGpuRecording = false;
    uint32_t CheckpointEvery = 64;

    struct GpuPacingStats
    {
        uint32_t frames = 0, presents = 0;
        double frameMs = 0.0, at = 0.0, atMin = 1e9, atMax = 0.0, target = 0.0;
    } GpuStats;

    void ReleaseGpuTiming()
    {
        for (auto& f : GpuFrames)
        {
            SAFE_RELEASE(f.disjoint);
            SAFE_RELEASE(f.frequency);
            SAFE_RELEASE(f.presented);
            for (auto& q : f.queries)
                SAFE_RELEASE(q);
            f = {};
        }
        bGpuRecording = false;
    }

    // A timestamp at this point of the frame's commands
    void Checkpoint(IDirect3DDevice9* device)
    {
        auto& f = GpuFrames[GpuSlot];
        if (f.draws.size() == f.queries.size())
        {
            IDirect3DQuery9* query = nullptr;
            if (FAILED(device->CreateQuery(D3DQUERYTYPE_TIMESTAMP, &query)) || !query)
                return;
            f.queries.push_back(query);
        }
        f.queries[f.draws.size()]->Issue(D3DISSUE_END);
        f.draws.push_back(DrawsThisFrame);
    }

    // The results of the frames that are over, once the GPU has them
    void ReadGpuFrames()
    {
        for (auto& f : GpuFrames)
        {
            if (!f.issued)
                continue;
            BOOL disjoint = TRUE;
            UINT64 frequency = 0;
            if (f.disjoint->GetData(&disjoint, sizeof(disjoint), 0) != S_OK || f.frequency->GetData(&frequency, sizeof(frequency), 0) != S_OK)
                continue;
            std::vector<UINT64> times(f.draws.size());
            bool ready = true;
            for (size_t i = 0; i < times.size() && ready; ++i)
                ready = f.queries[i]->GetData(&times[i], sizeof(UINT64), 0) == S_OK;
            UINT64 presented = 0;
            if (ready && f.hadPresent)
                ready = f.presented->GetData(&presented, sizeof(presented), 0) == S_OK;
            if (!ready)
                continue;
            f.issued = false;
            if (disjoint || !frequency || times.size() < 3 || times.back() <= times.front())
                continue;

            // The first draw call the GPU reaches each aim of the frame at, between two timestamps
            double total = static_cast<double>(times.back() - times.front());
            double draw = 0.0;
            for (int which = 0; which < 2; ++which)
            {
                double aim = Aim(which) * total;
                draw = f.draws.back();
                for (size_t i = 1; i < times.size(); ++i)
                {
                    double t = static_cast<double>(times[i] - times.front());
                    if (t < aim)
                        continue;
                    double t0 = static_cast<double>(times[i - 1] - times.front());
                    double part = t > t0 ? (aim - t0) / (t - t0) : 0.0;
                    draw = f.draws[i - 1] + part * (static_cast<double>(f.draws[i]) - f.draws[i - 1]);
                    break;
                }
                auto& target = TargetDraws[which];
                target = target > 0.0 ? target + (draw - target) * 0.25 : draw;
            }

            auto& g = GpuStats;
            ++g.frames;
            g.frameMs += total * 1000.0 / static_cast<double>(frequency);
            g.target += draw;
            if (f.hadPresent && presented >= times.front())
            {
                double at = static_cast<double>(presented - times.front()) / total;
                ++g.presents;
                g.at += at;
                g.atMin = std::min(g.atMin, at);
                g.atMax = std::max(g.atMax, at);
            }
            if (g.frames >= 300)
            {
                if (nDebug & Debug::PacingLog)
                    Log("GPU pacing (FrameGenerationPacing %d) over %u frames: %.2f ms of GPU work a frame, rendered frame presented at %.2f of it (%.2f..%.2f, aimed at %.2f, %u measured), draw call %.0f of %.0f",
                    nPacing, g.frames, g.frameMs / g.frames, g.presents ? g.at / g.presents : 0.0, g.presents ? g.atMin : 0.0, g.presents ? g.atMax : 0.0,
                    Aim(1), g.presents, g.target / g.frames, DrawsEma);
                g = {};
            }
        }
    }

    // Render thread, where one frame ends and the next begins: closes the frame's timestamps, opens the next one's
    void NextGpuFrame(IDirect3DDevice9* device)
    {
        if (bGpuRecording)
        {
            auto& f = GpuFrames[GpuSlot];
            Checkpoint(device);
            f.disjoint->Issue(D3DISSUE_END);
            f.frequency->Issue(D3DISSUE_END);
            f.issued = true;
            bGpuRecording = false;
        }
        // Recorded with every pacing, which the log compares; only 2 paces by it
        ReadGpuFrames();

        GpuSlot = (GpuSlot + 1) % GpuFrames.size();
        auto& f = GpuFrames[GpuSlot];
        // Results that never came in four frames are dropped
        f.issued = false;
        f.draws.clear();
        f.hadPresent = false;
        if ((!f.disjoint && FAILED(device->CreateQuery(D3DQUERYTYPE_TIMESTAMPDISJOINT, &f.disjoint))) ||
            (!f.frequency && FAILED(device->CreateQuery(D3DQUERYTYPE_TIMESTAMPFREQ, &f.frequency))) ||
            (!f.presented && FAILED(device->CreateQuery(D3DQUERYTYPE_TIMESTAMP, &f.presented))))
        {
            LogOnce(8, "GPU timestamps are not available: pacing by the draw calls");
            return;
        }
        f.disjoint->Issue(D3DISSUE_BEGIN);
        Checkpoint(device);
        CheckpointEvery = std::max(16u, static_cast<uint32_t>(DrawsEma / 32.0));
        bGpuRecording = true;
    }
    // Debug::Similarity: the generated frame and the rendered ones before and after it, read back small. A frame
    // between them differs from both; a copy of one of them doesn't differ from it.
    constexpr UINT SmallWidth = 64, SmallHeight = 40;
    IDirect3DSurface9* SmallRT = nullptr;
    IDirect3DSurface9* SmallMemory = nullptr;
    std::vector<uint8_t> PreviousSmall;     // the rendered frame before, 0 bytes while there is none
    std::vector<uint8_t> OlderSmall;        // the one before that
    std::vector<uint8_t> GeneratedSmall;    // the generated frame before

    struct SimilarityStats
    {
        uint32_t frames = 0, hudLessFrames = 0, olderFrames = 0;
        double toPrevious = 0.0, toCurrent = 0.0, toBlend = 0.0, between = 0.0, hudLessDiffers = 0.0;
        double toOlder = 0.0, olderToPrevious = 0.0, toGenerated = 0.0;
    } Similar;

    bool ReadSmall(IDirect3DDevice9* device, IDirect3DSurface9* source, std::vector<uint8_t>& pixels)
    {
        if (!SmallRT && FAILED(device->CreateRenderTarget(SmallWidth, SmallHeight, D3DFMT_A8R8G8B8, D3DMULTISAMPLE_NONE, 0, FALSE, &SmallRT, nullptr)))
            return false;
        if (!SmallMemory && FAILED(device->CreateOffscreenPlainSurface(SmallWidth, SmallHeight, D3DFMT_A8R8G8B8, D3DPOOL_SYSTEMMEM, &SmallMemory, nullptr)))
            return false;
        if (FAILED(device->StretchRect(source, nullptr, SmallRT, nullptr, D3DTEXF_LINEAR)) || FAILED(device->GetRenderTargetData(SmallRT, SmallMemory)))
            return false;
        D3DLOCKED_RECT locked{};
        if (FAILED(SmallMemory->LockRect(&locked, nullptr, D3DLOCK_READONLY)))
            return false;
        pixels.resize(SmallWidth * SmallHeight * 3);
        for (UINT y = 0; y < SmallHeight; ++y)
        {
            auto row = reinterpret_cast<const uint8_t*>(locked.pBits) + y * locked.Pitch;
            for (UINT x = 0; x < SmallWidth; ++x)
                for (int c = 0; c < 3; ++c)
                    pixels[(y * SmallWidth + x) * 3 + c] = row[x * 4 + c];
        }
        SmallMemory->UnlockRect();
        return true;
    }

    double Difference(const std::vector<uint8_t>& a, const std::vector<uint8_t>& b)
    {
        double sum = 0.0;
        for (size_t i = 0; i < a.size() && i < b.size(); ++i)
            sum += std::abs(static_cast<int>(a[i]) - static_cast<int>(b[i]));
        return a.empty() ? 0.0 : sum / static_cast<double>(a.size());
    }

    // After Generate: the generated frame, between the last rendered frame and this one. hudLess: the frame before the
    // HUD the frame generation got, which should differ from the finished frame only where the HUD is.
    void CompareGenerated(IDirect3DDevice9* device, IDirect3DSurface9* current, IDirect3DSurface9* generated, IDirect3DSurface9* hudLess)
    {
        std::vector<uint8_t> now, between, beforeHud;
        if (!ReadSmall(device, current, now))
            return;
        if (hudLess && ReadSmall(device, hudLess, beforeHud))
        {
            uint32_t differs = 0;
            for (size_t i = 0; i + 2 < now.size(); i += 3)
                differs += std::abs(now[i] - beforeHud[i]) + std::abs(now[i + 1] - beforeHud[i + 1]) + std::abs(now[i + 2] - beforeHud[i + 2]) > 12;
            ++Similar.hudLessFrames;
            Similar.hudLessDiffers += static_cast<double>(differs) / (now.size() / 3);
        }
        bool compared = !PreviousSmall.empty() && ReadSmall(device, generated, between);
        if (compared)
        {
            auto& m = Similar;
            auto frames = Difference(PreviousSmall, now);
            // Frames that hardly change tell nothing
            if (frames > 0.5)
            {
                ++m.frames;
                m.toPrevious += Difference(between, PreviousSmall) / frames;
                m.toCurrent += Difference(between, now) / frames;
                // Against the plain average of the two: a frame generation that can't follow the motion blends them
                std::vector<uint8_t> blend(now.size());
                for (size_t i = 0; i < blend.size(); ++i)
                    blend[i] = static_cast<uint8_t>((now[i] + PreviousSmall[i] + 1) / 2);
                m.toBlend += Difference(between, blend) / frames;
                // A generated frame a frame late lies between the two rendered frames before
                if (!OlderSmall.empty() && !GeneratedSmall.empty())
                {
                    ++m.olderFrames;
                    m.toOlder += Difference(between, OlderSmall) / frames;
                    m.olderToPrevious += Difference(OlderSmall, PreviousSmall) / frames;
                    m.toGenerated += Difference(between, GeneratedSmall) / frames;
                }
                m.between += frames;
                if (m.frames >= 100)
                {
                    Log("Generated frames over %u moving frames: they differ from the rendered frame before by %.2f, from the one after by %.2f and from the average of the two by %.2f of what those two differ by (%.1f on average); the frame before the HUD differs from the finished one over %.0f%% of the screen (%u frames)",
                        m.frames, m.toPrevious / m.frames, m.toCurrent / m.frames, m.toBlend / m.frames, m.between / m.frames,
                        m.hudLessFrames ? 100.0 * m.hudLessDiffers / m.hudLessFrames : 0.0, m.hudLessFrames);
                    if (m.olderFrames)
                        Log("  the same frames against the ones before: %.2f from the rendered frame two back, which is %.2f from the one before; %.2f from the generated frame before (%u frames)",
                            m.toOlder / m.olderFrames, m.olderToPrevious / m.olderFrames, m.toGenerated / m.olderFrames, m.olderFrames);
                    m = {};
                }
            }
        }
        GeneratedSmall = compared ? std::move(between) : std::vector<uint8_t>{};
        OlderSmall = std::move(PreviousSmall);
        PreviousSmall = std::move(now);
    }

    void ReleaseTargets()
    {
        bPending = false;
        bPendingGenerated = false;
        bSkipPresent = false;
        ReleaseGpuTiming();
        SAFE_RELEASE(SmallRT);
        SAFE_RELEASE(SmallMemory);
        PreviousSmall.clear();
        OlderSmall.clear();
        GeneratedSmall.clear();
        for (auto rt : { &PresentRT, &GeneratedRT, &HudLessRT, &SavedRT })
        {
            if (*rt)
            {
                (*rt)->Destroy();
                *rt = nullptr;
            }
        }
        TargetWidth = TargetHeight = 0;
    }

    // All at the back buffer's size, in the format of the helper's textures: A8R8G8B8 as the back buffer without HDR,
    // where the helper can write it, 16-bit float otherwise
    bool CreateTargets(uint32_t width, uint32_t height)
    {
        bool eightBit = Upscaler::IsFrameGenerationEightBit();
        if (PresentRT && GeneratedRT && HudLessRT && SavedRT && TargetWidth == width && TargetHeight == height && bTargetsEightBit == eightBit)
            return true;
        ReleaseTargets();

        auto desc = rage::OwnRenderTargetDesc(eightBit ? rage::GRCFMT_A8R8G8B8 : rage::GRCFMT_A16B16G16R16F);
        uint32_t bits = eightBit ? 32 : 64;
        PresentRT = rage::CreateEmptyRenderTarget("FrameGenerationPresent", width, height, bits, desc);
        GeneratedRT = rage::CreateEmptyRenderTarget("FrameGenerationGenerated", width, height, bits, desc);
        HudLessRT = rage::CreateEmptyRenderTarget("FrameGenerationHudLess", width, height, bits, desc);
        SavedRT = rage::CreateEmptyRenderTarget("FrameGenerationSaved", width, height, bits, desc);
        if (!PresentRT || !PresentRT->mD3DTexture || !GeneratedRT || !GeneratedRT->mD3DTexture || !HudLessRT || !HudLessRT->mD3DTexture ||
            !SavedRT || !SavedRT->mD3DTexture)
        {
            ReleaseTargets();
            return false;
        }
        TargetWidth = width;
        TargetHeight = height;
        bTargetsEightBit = eightBit;
        Log("Targets: %ux%u, %s", width, height, eightBit ? "A8R8G8B8" : "A16B16G16R16F");
        return true;
    }

    // Copies a surface into a target of the same size
    bool CopyInto(IDirect3DDevice9* device, IDirect3DSurface9* source, rage::grcRenderTargetPC* target)
    {
        IDirect3DSurface9* surface = nullptr;
        target->mD3DTexture->GetSurfaceLevel(0, &surface);
        bool ok = surface && SUCCEEDED(device->StretchRect(source, nullptr, surface, nullptr, D3DTEXF_POINT));
        SAFE_RELEASE(surface);
        return ok;
    }

    // Copies a target into a surface of the same size
    bool CopyFrom(IDirect3DDevice9* device, rage::grcRenderTargetPC* source, IDirect3DSurface9* target)
    {
        IDirect3DSurface9* surface = nullptr;
        source->mD3DTexture->GetSurfaceLevel(0, &surface);
        bool ok = surface && SUCCEEDED(device->StretchRect(surface, nullptr, target, nullptr, D3DTEXF_POINT));
        SAFE_RELEASE(surface);
        return ok;
    }

    // Debug::Marker: which of the frames reach the screen
    void Mark(IDirect3DDevice9* device, IDirect3DSurface9* backBuffer, bool generated)
    {
        if (!(nDebug & Debug::Marker))
            return;
        RECT square{ 0, 0, 64, 64 };
        device->ColorFill(backBuffer, &square, generated ? D3DCOLOR_XRGB(255, 0, 255) : D3DCOLOR_XRGB(0, 255, 0));
    }

    // A section of the PostFx profiler, when it runs
    void Profile(IDirect3DDevice9* device, Upscaler::ProfilePart part, bool begin)
    {
        if (Upscaler::Profile)
            Upscaler::Profile(device, part, begin);
    }

    bool SharpenFrame(IDirect3DDevice9* device, rage::grcRenderTargetPC* frame, IDirect3DSurface9* target, bool hudLess);
    void PresentFrame(IDirect3DDevice9* device, bool late, bool generated);

    // Presents a waiting frame, the rendered or the generated one, the game's back buffer kept as it was. late: the next
    // frame ended first.
    void PresentWaiting(IDirect3DDevice9* device, bool late, bool generated)
    {
        auto start = Upscaler::TimingStart();
        Profile(device, Upscaler::ProfilePart::FrameGenerationPresent, true);
        PresentFrame(device, late, generated);
        Profile(device, Upscaler::ProfilePart::FrameGenerationPresent, false);
        Upscaler::TimingAdd(Upscaler::TimingPart::FrameGenerationPresent, start);
    }

    // Deferred: the generated frame into the back buffer, sharpened as it is shown when the post processing left it
    bool FillGenerated(IDirect3DDevice9* device, IDirect3DSurface9* backBuffer)
    {
        if (bPendingGeneratedSharpen && SharpenFrame(device, GeneratedRT, backBuffer, bPendingGeneratedHudLess && HudLessFrame == PendingFrame))
            return true;
        return CopyFrom(device, GeneratedRT, backBuffer);
    }

    // The rendered frame into the back buffer, sharpened as it is shown when the post processing left it
    bool FillRendered(IDirect3DDevice9* device, IDirect3DSurface9* backBuffer)
    {
        if (bPendingRenderedSharpen && SharpenFrame(device, PresentRT, backBuffer, bPendingRenderedHudLess && HudLessFrame == PendingFrame))
            return true;
        return CopyFrom(device, PresentRT, backBuffer);
    }

    void PresentFrame(IDirect3DDevice9* device, bool late, bool generated)
    {
        if (generated)
        {
            bPendingGenerated = false;
            // Copied out of the helper once, here and not at the frame's end: the GPU went on with this frame meanwhile
            if (!bGeneratedTaken)
            {
                bGeneratedTaken = true;
                if (!Upscaler::TakeGenerated(GeneratedRT->mD3DTexture))
                {
                    LogOnce(9, "A generated frame could not be taken from the helper: the rendered frame goes alone");
                    return;
                }
            }
        }
        else
        {
            bPending = false;
        }
        bool retry = false;
        IDirect3DSurface9* backBuffer = nullptr;
        if (FAILED(device->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &backBuffer)) || !backBuffer)
            return;

        // Nothing of this frame is in the back buffer before its post processing, which clears it and draws the whole
        // frame: only a back buffer the game drew into, or is drawing into, is kept and given back after the Present
        IDirect3DSurface9* bound = nullptr;
        device->GetRenderTarget(0, &bound);
        bool keep = late || bBackBufferDrawn || bound == backBuffer;
        SAFE_RELEASE(bound);

        bInPresent = true;
        if ((!keep || CopyInto(device, backBuffer, SavedRT)) &&
            (generated ? FillGenerated(device, backBuffer) : FillRendered(device, backBuffer)))
        {
            Mark(device, backBuffer, generated);
            // Present moves the images of the two back buffers around: what is bound to the device is bound again
            // afterwards, so that the rest of the frame draws into the back buffer it had
            IDirect3DSurface9* targets[4]{};
            IDirect3DSurface9* depth = nullptr;
            D3DVIEWPORT9 viewport{};
            RECT scissor{};
            for (DWORD i = 0; i < 4; ++i)
                device->GetRenderTarget(i, &targets[i]);
            device->GetDepthStencilSurface(&depth);
            device->GetViewport(&viewport);
            device->GetScissorRect(&scissor);

            // The game is inside its scene. Within a frame it stays so: D3D9, and DXVK as it, only lets go of the vertex
            // and index buffers the game unbound at EndScene, and the game goes on drawing with ones it unbound, which
            // came out black after an EndScene of ours. At the end of the frame nothing is drawn after it.
            auto presentedAt = Now();
            if (bGpuRecording && !late && !generated)
            {
                auto& f = GpuFrames[GpuSlot];
                f.presented->Issue(D3DISSUE_END);
                f.hadPresent = true;
            }
            if (late)
                device->EndScene();
            auto hr = device->Present(nullptr, nullptr, nullptr, nullptr);
            if (late)
                device->BeginScene();
            if (hr == D3DERR_INVALIDCALL && !late)
            {
                // Not inside a scene with this runtime: from now on when the next frame ends
                Log("Present inside the game's scene was refused: the rendered frames go when the next frame ends");
                bEndOfFrameOnly = true;
                retry = true;
            }
            else if (FAILED(hr))
                LogOnce(6, "Present of a waiting frame failed");

            // The game's back buffer as it was, in whichever surface is the back buffer now
            IDirect3DSurface9* current = nullptr;
            if (keep && SUCCEEDED(device->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &current)) && current)
            {
                CopyFrom(device, SavedRT, current);
                current->Release();
            }

            for (DWORD i = 0; i < 4; ++i)
                if (targets[i] || i > 0)
                    device->SetRenderTarget(i, targets[i]);
            device->SetDepthStencilSurface(depth);
            device->SetViewport(&viewport);
            device->SetScissorRect(&scissor);
            for (auto& target : targets)
                SAFE_RELEASE(target);
            SAFE_RELEASE(depth);

            if (retry)
                (generated ? bPendingGenerated : bPending) = true;
            else if (generated)
            {
                // The rendered frame follows FrameGenerationDelay of a frame later
                GeneratedAt = presentedAt;
                PendingDue = After(presentedAt, std::clamp(FrameMs * fDelay, 0.0, 50.0));
            }
            else
                Stats.Add(Ms(GeneratedAt, presentedAt), Ms(GeneratedAt, PendingDue), late);
        }
        else
        {
            LogOnce(5, "A waiting frame could not be put into the back buffer");
        }
        bInPresent = false;
        backBuffer->Release();
    }

    // Whether a waiting frame's time has come: 0 the generated one, 1 the rendered one
    bool IsDue(int which, LARGE_INTEGER due)
    {
        if (nPacing == 2 && TargetDraws[which] > 0.0)
            return DrawsThisFrame >= TargetDraws[which];
        if (nPacing >= 1 && DrawsEma > 0.0)
            return DrawsThisFrame >= Aim(which) * DrawsEma;
        return Now().QuadPart >= due.QuadPart;
    }

    // Draw calls of the render thread: the waiting frames go once their time has come, the generated one first
    void CheckPending(IDirect3DDevice9* device)
    {
        if (GetCurrentThreadId() != RenderThread)
            return;
        ++DrawsThisFrame;
        if (bGpuRecording && !bInPresent && DrawsThisFrame % CheckpointEvery == 0)
            Checkpoint(device);
        if (bInPresent || bEndOfFrameOnly)
            return;

        if (bPendingGenerated)
        {
            if (IsDue(0, PendingGeneratedDue))
                PresentWaiting(device, false, true);
            return;
        }
        if (bPending && IsDue(1, PendingDue))
            PresentWaiting(device, false, false);
    }

    // The game's Present, left out after a frame whose frames are presented inside the next one. An inline hook on the
    // runtime's Present itself: a plugin between the game and the device (Script Hook's device proxy, an overlay) can
    // call it through a pointer it kept from before a hook in the vtable, which then never saw the game's Present.
    SafetyHookInline shPresent{};

    HRESULT __stdcall Present(IDirect3DDevice9* device, const RECT* source, const RECT* destination, HWND window, const RGNDATA* dirty)
    {
        if (bSkipPresent && !bInPresent && GetCurrentThreadId() == RenderThread)
        {
            bSkipPresent = false;
            return D3D_OK;
        }
        return shPresent.unsafe_stdcall<HRESULT>(device, source, destination, window, dirty);
    }

    // The same through the swap chain, which the game could present with as well
    SafetyHookInline shSwapChainPresent{};

    HRESULT __stdcall SwapChainPresent(IDirect3DSwapChain9* swapChain, const RECT* source, const RECT* destination, HWND window, const RGNDATA* dirty, DWORD flags)
    {
        if (bSkipPresent && !bInPresent && GetCurrentThreadId() == RenderThread)
        {
            bSkipPresent = false;
            return D3D_OK;
        }
        return shSwapChainPresent.unsafe_stdcall<HRESULT>(swapChain, source, destination, window, dirty, flags);
    }

    // The module some code is in, for the log
    std::string ModuleOf(const void* address)
    {
        HMODULE module = nullptr;
        char path[MAX_PATH]{};
        if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, static_cast<const char*>(address), &module) ||
            !GetModuleFileNameA(module, path, MAX_PATH))
            return "no module";
        return std::filesystem::path(path).filename().string();
    }

    HRESULT(__stdcall* RealDrawPrimitive)(IDirect3DDevice9*, D3DPRIMITIVETYPE, UINT, UINT) = nullptr;
    HRESULT(__stdcall* RealDrawIndexedPrimitive)(IDirect3DDevice9*, D3DPRIMITIVETYPE, INT, UINT, UINT, UINT, UINT) = nullptr;
    HRESULT(__stdcall* RealDrawPrimitiveUP)(IDirect3DDevice9*, D3DPRIMITIVETYPE, UINT, const void*, UINT) = nullptr;
    HRESULT(__stdcall* RealDrawIndexedPrimitiveUP)(IDirect3DDevice9*, D3DPRIMITIVETYPE, UINT, UINT, UINT, const void*, D3DFORMAT, const void*, UINT) = nullptr;

    HRESULT __stdcall DrawPrimitive(IDirect3DDevice9* device, D3DPRIMITIVETYPE type, UINT start, UINT count)
    {
        CheckPending(device);
        return RealDrawPrimitive(device, type, start, count);
    }

    HRESULT __stdcall DrawIndexedPrimitive(IDirect3DDevice9* device, D3DPRIMITIVETYPE type, INT base, UINT minIndex, UINT vertices, UINT start, UINT count)
    {
        CheckPending(device);
        return RealDrawIndexedPrimitive(device, type, base, minIndex, vertices, start, count);
    }

    HRESULT __stdcall DrawPrimitiveUP(IDirect3DDevice9* device, D3DPRIMITIVETYPE type, UINT count, const void* data, UINT stride)
    {
        CheckPending(device);
        return RealDrawPrimitiveUP(device, type, count, data, stride);
    }

    HRESULT __stdcall DrawIndexedPrimitiveUP(IDirect3DDevice9* device, D3DPRIMITIVETYPE type, UINT minIndex, UINT vertices, UINT count, const void* indices,
        D3DFORMAT format, const void* data, UINT stride)
    {
        CheckPending(device);
        return RealDrawIndexedPrimitiveUP(device, type, minIndex, vertices, count, indices, format, data, stride);
    }

    template <class F>
    void Patch(void** vtable, int index, F& real, F hook)
    {
        if (vtable[index] == reinterpret_cast<void*>(hook))
            return;
        real = reinterpret_cast<F>(vtable[index]);
        injector::WriteMemory(&vtable[index], reinterpret_cast<void*>(hook), true);
    }

    // On the D3D9 runtime's own device, as the render scale's hooks
    void InstallHooks(IDirect3DDevice9* device)
    {
        static void** hooked = nullptr;
        auto vtable = *reinterpret_cast<void***>(device);
        if (vtable == hooked)
            return;
        Patch(vtable, 81, RealDrawPrimitive, &DrawPrimitive);
        Patch(vtable, 82, RealDrawIndexedPrimitive, &DrawIndexedPrimitive);
        Patch(vtable, 83, RealDrawPrimitiveUP, &DrawPrimitiveUP);
        Patch(vtable, 84, RealDrawIndexedPrimitiveUP, &DrawIndexedPrimitiveUP);
        hooked = vtable;
        Log("Draw call hooks installed");

        if (!shPresent)
        {
            auto target = vtable[17];
            shPresent = safetyhook::create_inline(target, reinterpret_cast<void*>(&Present));
            Log("Present at %p in %s: %s", target, ModuleOf(target).c_str(), shPresent ? "hooked" : "could NOT be hooked, the generated frames go at the frame's end");
            if (!shPresent)
                bDeferredBroken = true;

            IDirect3DSwapChain9* swapChain = nullptr;
            if (SUCCEEDED(device->GetSwapChain(0, &swapChain)) && swapChain)
            {
                auto swapTarget = (*reinterpret_cast<void***>(swapChain))[3];
                shSwapChainPresent = safetyhook::create_inline(swapTarget, reinterpret_cast<void*>(&SwapChainPresent));
                Log("Swap chain Present at %p in %s: %s", swapTarget, ModuleOf(swapTarget).c_str(), shSwapChainPresent ? "hooked" : "not hooked");
                swapChain->Release();
            }
        }
    }

    // The settings of [TEMPORAL], read again whenever the ini changes while the game runs
    std::filesystem::path IniPath;
    std::filesystem::file_time_type IniTime{};

    void ReadSettings()
    {
        CIniReader iniReader("");
        IniPath = iniReader.GetIniPath();
        std::error_code error;
        IniTime = std::filesystem::last_write_time(IniPath, error);

        fDelay = std::clamp(iniReader.ReadFloat("TEMPORAL", "FrameGenerationDelay", 0.5f), 0.0f, 1.0f);
        Upscaler::SetTimingLog(iniReader.ReadInteger("TEMPORAL", "UpscalerTimingLog", 0) != 0);
        nDebug = iniReader.ReadInteger("TEMPORAL", "FrameGenerationDebug", 0);
        nPacing = std::clamp(iniReader.ReadInteger("TEMPORAL", "FrameGenerationPacing", 2), 0, 2);
        bDeferredSetting = iniReader.ReadInteger("TEMPORAL", "FrameGenerationDeferred", 1) != 0;
        fShowGeneratedAt = std::clamp(iniReader.ReadFloat("TEMPORAL", "FrameGenerationShowGeneratedAt", 0.2f), 0.0f, 0.9f);
        // The aims may have changed
        TargetDraws[0] = TargetDraws[1] = 0.0;
        Log("Frame generation settings: delay %.2f, pacing %d, deferred %d (generated frame at %.2f), debug %d", fDelay, nPacing,
            int(bDeferredSetting), fShowGeneratedAt, nDebug);

        // A fresh start for the statistics
        Stats = {};
        GpuStats = {};
    }

    void CheckSettings()
    {
        static ULONGLONG checked = 0;
        auto now = GetTickCount64();
        if (now - checked < 1000)
            return;
        checked = now;
        std::error_code error;
        auto time = std::filesystem::last_write_time(IniPath, error);
        if (!error && time != IniTime)
            ReadSettings();
    }

    // DLSS-IV, another DLSS and FSR for the game, records the D3D9 calls of the render thread and replays them on a thread
    // of its own 300 presents in: the copies for the helper then ran ahead of the frame they copy, and every generated
    // frame was a copy of the frame before
    bool IsDlssIvLoaded()
    {
        static bool loaded = false;
        static ULONGLONG checked = 0;
        if (!loaded && GetTickCount64() - checked > 2000)
        {
            checked = GetTickCount64();
            loaded = GetModuleHandleW(L"DLSS-IV.asi") != nullptr;
            if (loaded)
                Log("DLSS-IV.asi is loaded: no frame generation while it is (it replays the game's D3D9 calls on a thread of its own)");
        }
        return loaded;
    }

    // The menu's choice, while the frame generation can run
    void UpdateMode()
    {
        static auto pref = FusionFixSettings.GetRef("PREF_FRAME_GENERATION");
        auto previous = mode;
        bool on = pref && pref->get() != 0 && Upscaler::IsFrameGenerationAvailable() && !IsDlssIvLoaded();
        mode = !on ? Mode::Off : (nDebug & Debug::ShowGenerated) ? Mode::ShowGenerated : Mode::On;
        if (mode != previous)
        {
            Log("Frame generation: %s", mode == Mode::Off ? "off" : mode == Mode::ShowGenerated ? "showing the generated frames" : "on");
            // A fresh start for the pacing
            LastFrameEnd.QuadPart = 0;
            bPending = false;
            bPendingGenerated = false;
            bSkipPresent = false;
            Upscaler::DropGenerated();
        }
    }

    bool IsMenuActive()
    {
        return CMenuManager::m_MenuActive && *CMenuManager::m_MenuActive;
    }

    // Whatever is drawn over the whole frame after the HUD is part of the finished frame, and so has to be of the copy
    // before the HUD too: the frame generation takes what differs for the HUD, the whole frame otherwise. Drawn here,
    // where the console gamma itself is drawn and nothing of the game follows; through the game's device wrapper, which
    // its effect was made with.
    void ApplyFinishingPasses()
    {
        if ((!ConsoleGamma::IsActive() && !HDROutput::IsActive()) || !HudLessRT)
            return;
        auto device = rage::grcDevice::GetD3DDevice();
        if (!device)
            return;
        IDirect3DSurface9* oldTarget = nullptr;
        IDirect3DSurface9* hudLessSurface = nullptr;
        D3DVIEWPORT9 oldViewport{};
        device->GetRenderTarget(0, &oldTarget);
        device->GetViewport(&oldViewport);
        HudLessRT->mD3DTexture->GetSurfaceLevel(0, &hudLessSurface);
        // In their order at the end of the frame: the console gamma at EndScene, the HDR output after it
        if (hudLessSurface && SUCCEEDED(device->SetRenderTarget(0, hudLessSurface)))
        {
            if (ConsoleGamma::IsActive())
                ConsoleGamma::Apply(device);
            HDROutput::ApplyToRenderTarget();
        }
        if (oldTarget)
            device->SetRenderTarget(0, oldTarget);
        device->SetViewport(&oldViewport);
        SAFE_RELEASE(hudLessSurface);
        SAFE_RELEASE(oldTarget);
    }

    // The frame in a target sharpened into target. The HUD is where PresentRT differs from HudLessRT, which has had the
    // finishing passes; without the frame before the HUD all of it is sharpened.
    bool SharpenFrame(IDirect3DDevice9* device, rage::grcRenderTargetPC* frame, IDirect3DSurface9* target, bool hudLess)
    {
        auto beforeHud = hudLess ? HudLessRT : PresentRT;
        return Sharpen && frame && PresentRT && beforeHud && Sharpen(device, frame->mD3DTexture, PresentRT->mD3DTexture, beforeHud->mD3DTexture, target);
    }

    // A frame the post processing left unsharpened that goes out as it was rendered: sharpened in the back buffer
    void SharpenBackBuffer(IDirect3DDevice9* device, bool hudLess, bool presentCopied)
    {
        IDirect3DSurface9* backBuffer = nullptr;
        if (FAILED(device->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &backBuffer)) || !backBuffer)
            return;
        D3DSURFACE_DESC desc{};
        backBuffer->GetDesc(&desc);
        if (presentCopied || (CreateTargets(desc.Width, desc.Height) && CopyInto(device, backBuffer, PresentRT)))
        {
            if (hudLess && !presentCopied)
                ApplyFinishingPasses();
            if (!SharpenFrame(device, PresentRT, backBuffer, hudLess))
                LogOnce(7, "The frame could not be sharpened");
        }
        backBuffer->Release();
    }

    // Render thread, after the frame is finished
    void OnBeforePresent()
    {
        ++FrameNumber;
        CheckSettings();
        UpdateMode();
        bool hudLess = bHudLessCaptured;
        bHudLessCaptured = false;
        bool sharpen = bSharpenDeferred;
        bSharpenDeferred = false;
        FusionFix::bFrameGenerationPresenting = false;

        auto device = RageDirect3DDevice9::GetRuntimeDevice();
        if (!device)
            return;
        if (mode == Mode::Off)
        {
            if (sharpen)
                SharpenBackBuffer(device, hudLess, false);
            return;
        }

        RenderThread = GetCurrentThreadId();
        auto now = Now();
        if (LastFrameEnd.QuadPart)
        {
            auto frame = Ms(LastFrameEnd, now);
            if (frame > 0.0 && frame < 250.0)
                FrameMs = FrameMs > 0.0 ? FrameMs + (frame - FrameMs) * 0.1 : frame;
        }
        LastFrameEnd = now;

        if (DrawsThisFrame > 100)
            DrawsEma = DrawsEma > 0.0 ? DrawsEma + (DrawsThisFrame - DrawsEma) * 0.1 : DrawsThisFrame;
        NextGpuFrame(device);
        DrawsThisFrame = 0;

        // The game's Present after the last frame should have been left out: it went another way, and the frames would be
        // shown out of order
        if (bSkipPresent)
        {
            bSkipPresent = false;
            bDeferredBroken = true;
            auto vtable = *reinterpret_cast<void***>(device);
            Log("The game's Present did not come through the device's Present (the device's vtable has it at %p in %s now): the generated frames go at the frame's end again",
                vtable[17], ModuleOf(vtable[17]).c_str());
        }

        // This frame ended before the last one's frames went: they go first, in their order
        if (bPendingGenerated)
            PresentWaiting(device, true, true);
        if (bPending)
            PresentWaiting(device, true, false);
        bBackBufferDrawn = false;

        // Not in the menus: the generation starts over once they close
        if (!Upscaler::IsFrameGenerationReady() || IsMenuActive())
        {
            if (sharpen)
                SharpenBackBuffer(device, hudLess, false);
            return;
        }
        bool pacing = mode == Mode::On;
        if (pacing)
        {
            InstallHooks(device);

            static bool logged = false;
            IDirect3DSwapChain9* swapChain = nullptr;
            D3DPRESENT_PARAMETERS pp{};
            if (!logged && SUCCEEDED(device->GetSwapChain(0, &swapChain)) && swapChain && SUCCEEDED(swapChain->GetPresentParameters(&pp)))
            {
                Log("Swap chain: %ux%u format %d, %u back buffers, swap effect %d, flags %08x, interval %08x, windowed %d, auto depth %d (format %d)",
                    pp.BackBufferWidth, pp.BackBufferHeight, pp.BackBufferFormat, pp.BackBufferCount, pp.SwapEffect, pp.Flags,
                    pp.PresentationInterval, pp.Windowed, pp.EnableAutoDepthStencil, pp.AutoDepthStencilFormat);
                logged = true;
            }
            SAFE_RELEASE(swapChain);
        }

        IDirect3DSurface9* backBuffer = nullptr;
        if (FAILED(device->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &backBuffer)) || !backBuffer)
        {
            LogOnce(0, "The back buffer is not available");
            return;
        }

        D3DSURFACE_DESC desc{};
        backBuffer->GetDesc(&desc);
        if (!CreateTargets(desc.Width, desc.Height))
        {
            LogOnce(1, "The targets could not be created");
            backBuffer->Release();
            return;
        }

        // The output of the upscaler, and so of the frame generation, is the size of the screen
        IDirect3DSurface9* presentSurface = nullptr;
        IDirect3DSurface9* generatedSurface = nullptr;
        PresentRT->mD3DTexture->GetSurfaceLevel(0, &presentSurface);
        GeneratedRT->mD3DTexture->GetSurfaceLevel(0, &generatedSurface);

        Profile(device, Upscaler::ProfilePart::FrameGeneration, true);
        Profile(device, Upscaler::ProfilePart::FrameGenerationCopies, true);
        bool copied = presentSurface && generatedSurface && SUCCEEDED(device->StretchRect(backBuffer, nullptr, presentSurface, nullptr, D3DTEXF_POINT));
        auto hdr = HDROutput::IsActive();
        if (copied && hudLess)
            ApplyFinishingPasses();
        Profile(device, Upscaler::ProfilePart::FrameGenerationCopies, false);

        // Deferred: the helper generates while the next frame renders, both frames go inside it, the game's Present not
        bool deferred = pacing && bDeferredSetting && !bDeferredBroken && !bEndOfFrameOnly && !(nDebug & Debug::Similarity);
        if (copied && deferred)
        {
            Profile(device, Upscaler::ProfilePart::FrameGenerationGenerate, true);
            bool posted = Upscaler::PostGenerate(PresentRT->mD3DTexture, hudLess ? HudLessRT->mD3DTexture : nullptr, hdr ? HDROutput::GetPeakNits() : 0.0f);
            Profile(device, Upscaler::ProfilePart::FrameGenerationGenerate, false);
            // Not a frame generated without a previous one, nor before the frame time is known: the game's Present shows
            // this frame as it is
            bool paced = posted && !Upscaler::WasGenerateReset() && FrameMs > 0.0;
            Profile(device, Upscaler::ProfilePart::FrameGenerationShow, true);
            if (paced)
            {
                static bool first = true;
                if (first)
                    Log("First frame generated at %ux%u%s, shown inside the next frame", desc.Width, desc.Height, hdr ? ", HDR" : "");
                first = false;

                bPendingRenderedSharpen = sharpen;
                bPendingRenderedHudLess = hudLess;
                PendingFrame = FrameNumber;
                bPendingGeneratedSharpen = sharpen;
                bPendingGeneratedHudLess = hudLess;
                bGeneratedTaken = false;
                bPendingGenerated = true;
                bPending = true;
                bSkipPresent = true;
                bDeferredFlow = true;
                LogFlow(true);
                FusionFix::bFrameGenerationPresenting = true;
                PendingGeneratedDue = After(Now(), std::clamp(FrameMs * fShowGeneratedAt, 0.0, 50.0));
            }
            else
            {
                if (!posted)
                    LogOnce(2, "Generate failed, see GTAIV.EFLC.FusionFix.Upscaler.log and GTAIV.EFLC.FusionFix.UpscalerHelper.log");
                Upscaler::DropGenerated();
                if (sharpen)
                    SharpenBackBuffer(device, hudLess, true);
            }
            Profile(device, Upscaler::ProfilePart::FrameGenerationShow, false);
        }
        else if (copied)
        {
            Profile(device, Upscaler::ProfilePart::FrameGenerationGenerate, true);
            bool generated = Upscaler::Generate(PresentRT->mD3DTexture, hudLess ? HudLessRT->mD3DTexture : nullptr, GeneratedRT->mD3DTexture,
                hdr ? HDROutput::GetPeakNits() : 0.0f);
            Profile(device, Upscaler::ProfilePart::FrameGenerationGenerate, false);
            if (generated)
            {
                static bool first = true;
                if (first)
                    Log("First frame generated at %ux%u%s", desc.Width, desc.Height, hdr ? ", HDR" : "");
                first = false;

                if (nDebug & Debug::Similarity)
                {
                    if (Upscaler::WasGenerateReset())
                    {
                        PreviousSmall.clear();
                        OlderSmall.clear();
                        GeneratedSmall.clear();
                    }
                    IDirect3DSurface9* hudLessSurface = nullptr;
                    if (hudLess)
                        HudLessRT->mD3DTexture->GetSurfaceLevel(0, &hudLessSurface);
                    CompareGenerated(device, presentSurface, generatedSurface, hudLessSurface);
                    SAFE_RELEASE(hudLessSurface);
                }

                // The game presents the generated frame, the rendered one waits in PresentRT. Not a frame generated
                // without a previous one, nor before the frame time is known.
                bool paced = pacing && !Upscaler::WasGenerateReset() && FrameMs > 0.0;
                Profile(device, Upscaler::ProfilePart::FrameGenerationShow, true);
                // Sharpened as it is shown, when the post processing left it
                if (paced)
                {
                    bPendingRenderedSharpen = sharpen;
                    bPendingRenderedHudLess = hudLess;
                    PendingFrame = FrameNumber;
                }
                if (mode == Mode::ShowGenerated || paced)
                {
                    if (!sharpen || !SharpenFrame(device, GeneratedRT, backBuffer, hudLess))
                        device->StretchRect(generatedSurface, nullptr, backBuffer, nullptr, D3DTEXF_POINT);
                    Mark(device, backBuffer, true);
                }
                else if (sharpen)
                    SharpenBackBuffer(device, hudLess, true);
                Profile(device, Upscaler::ProfilePart::FrameGenerationShow, false);
                if (paced)
                {
                    bDeferredFlow = false;
                    LogFlow(false);
                    FusionFix::bFrameGenerationPresenting = true;
                    GeneratedAt = Now();
                    PendingDue = After(GeneratedAt, std::clamp(FrameMs * fDelay, 0.0, 50.0));
                    bPending = true;
                }
            }
            else
            {
                LogOnce(2, "Generate failed, see GTAIV.EFLC.FusionFix.Upscaler.log and GTAIV.EFLC.FusionFix.UpscalerHelper.log");
                if (sharpen)
                    SharpenBackBuffer(device, hudLess, true);
            }
        }
        else
        {
            LogOnce(3, "The back buffer could not be copied");
        }
        Profile(device, Upscaler::ProfilePart::FrameGeneration, false);

        SAFE_RELEASE(presentSurface);
        SAFE_RELEASE(generatedSurface);
        backBuffer->Release();
    }
}

export namespace FrameGeneration
{
    // Evaluate prepares the frame generation
    bool IsEnabled()
    {
        return mode != Mode::Off;
    }

    // The frame before the HUD will be captured: tells Evaluate, which comes earlier in the frame
    bool UsesHudLess()
    {
        return mode != Mode::Off;
    }

    // The post processing's sharpening, for the frames as they are shown
    void SetSharpener(bool (*sharpener)(IDirect3DDevice9* device, IDirect3DTexture9* frame, IDirect3DTexture9* present, IDirect3DTexture9* hudLess,
        IDirect3DSurface9* target))
    {
        Sharpen = sharpener;
    }

    // Render thread, once a frame as the post processing would sharpen it: true when the frame generation takes the frame
    // unsharpened and sharpens the frames it shows, the rendered and the generated one. Sharpened before it, the frame
    // generation moves a sharpened shadow under a moving car with the ground, and the shadow is shown twice.
    bool DefersSharpening()
    {
        bSharpenDeferred = Sharpen && mode != Mode::Off && Upscaler::IsFrameGenerationReady() && !IsMenuActive();
        return bSharpenDeferred;
    }

    // Render thread, as the post processing begins to draw the frame into the back buffer
    void OnPostProcessing()
    {
        bBackBufferDrawn = true;
    }

    // Render thread, right after the post processing: the back buffer holds the scene without the HUD
    void CaptureHudLess(IDirect3DDevice9* device, IDirect3DSurface9* backBuffer)
    {
        if (!UsesHudLess() || !Upscaler::IsFrameGenerationReady() || !device || !backBuffer)
            return;

        D3DSURFACE_DESC desc{};
        backBuffer->GetDesc(&desc);
        if (!CreateTargets(desc.Width, desc.Height))
            return;
        // The console gamma it needs is drawn at the end of the frame (ApplyFinishingPasses), not here in the middle of the
        // post processing, where its effect's state saving would go through the game's device wrapper
        bHudLessCaptured = CopyInto(device, backBuffer, HudLessRT);
        // Of the frame that ends next
        if (bHudLessCaptured)
            HudLessFrame = FrameNumber + 1;
        if (!bHudLessCaptured)
            LogOnce(4, "The frame before the HUD could not be copied");
    }
}

class FrameGenerationModule
{
public:
    FrameGenerationModule()
    {
        FusionFix::onInitEventAsync() += []()
        {
            QueryPerformanceFrequency(&Frequency);
            ReadSettings();

            // Offered once the helper found AMD's frame generation library, and not with DLSS-IV. The choice comes back
            // with them: the cfg keeps it.
            FusionFixSettings.SetAvailability("PREF_FRAME_GENERATION", [](int32_t value) -> bool
            {
                return value == 0 || (Upscaler::IsFrameGenerationAvailable() && !IsDlssIvLoaded());
            });
            static auto refreshAvailability = []()
            {
                static uint32_t generation = UINT32_MAX;
                if (generation != Upscaler::Generation())
                {
                    generation = Upscaler::Generation();
                    FusionFixSettings.RefreshAvailability("PREF_FRAME_GENERATION");
                }
            };
            FusionFix::onGameProcessEvent() += []() { refreshAvailability(); };
            FusionFix::onMenuDrawingEvent() += []() { refreshAvailability(); };

            FusionFix::onBeforePresent() += []()
            {
                auto start = Upscaler::TimingStart();
                OnBeforePresent();
                if (mode != Mode::Off)
                    Upscaler::TimingAdd(Upscaler::TimingPart::FrameGenerationEnd, start);
            };

            FusionFix::onBeforeReset() += []()
            {
                ReleaseTargets();
            };
        };
    }
} FrameGenerationModule;
