module;

#include <common.hxx>
#include <cstdarg>
#include <cstdio>

export module framegeneration;

import common;
import comvars;
import hdr;
import upscaler;

#ifndef SAFE_RELEASE
#define SAFE_RELEASE(p) { if (p) { (p)->Release(); (p)=NULL; } }
#endif

// AMD FSR frame generation, run by the helper next to DLSS and FSR (see upscaler.ixx).
//
// - The upscaler's Evaluate prepares the frame generation with the frame's depth and motion vectors.
// - The finished scene before the HUD is copied into HudLess, right after the post processing. The HUD is what
//   differs from it, and the frame generation keeps it from the current frame instead of interpolating it. Not
//   with HDR output yet: the HDR pass converts the whole frame only afterwards.
// - Once the frame is finished, HUD and HDR output included, the back buffer is copied into Present and the helper
//   generates the frame between it and the previous one into Generated.
// - FrameGeneration = 1 in [TEMPORAL]: the generated frame is presented first, from the render thread, and the
//   rendered one after a wait of half a frame. The wait is half the frame's own time, from the last presented
//   rendered frame to the generated one, so that it doesn't feed on itself; FrameGenerationDelay scales it. While
//   the GPU is the limit the render thread waits for it anyway, so the wait costs little; while the CPU is, it
//   lowers the rendered frame rate.
// - FrameGeneration = 2 shows the generated frames in place of the rendered ones, to check the generation itself.

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

    rage::grcRenderTargetPC* PresentRT = nullptr;
    rage::grcRenderTargetPC* GeneratedRT = nullptr;
    rage::grcRenderTargetPC* HudLessRT = nullptr;
    bool bHudLessCaptured = false;  // this frame
    uint32_t TargetWidth = 0;
    uint32_t TargetHeight = 0;

    // GTAIV.EFLC.FusionFix.FrameGeneration.log next to the plugin, each kind of failure once
    void Log(const char* format, ...)
    {
        static bool started = false;
        FILE* f = nullptr;
        if (_wfopen_s(&f, (GetThisModulePath() / L"GTAIV.EFLC.FusionFix.FrameGeneration.log").c_str(), started ? L"a" : L"w") || !f)
            return;
        started = true;
        va_list args;
        va_start(args, format);
        vfprintf(f, format, args);
        va_end(args);
        fputc('\n', f);
        fclose(f);
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
    LARGE_INTEGER RealPresented{};  // when the last rendered frame went to Present, 0 before the first
    double FrameMs = 0.0;           // smoothed time of a frame, without the wait

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

    // Until ms after from: sleeps while there is time, spins the last 2 ms
    void WaitUntil(LARGE_INTEGER from, double ms)
    {
        while (true)
        {
            auto left = ms - Ms(from, Now());
            if (left <= 0.0)
                return;
            if (left > 2.0)
                Sleep(1);
            else
                YieldProcessor();
        }
    }

    // Statistics of the pacing, logged every few seconds
    struct PacingStats
    {
        uint32_t frames = 0;
        double frameMs = 0.0, waitMs = 0.0, generatedPresentMs = 0.0, realGapMs = 0.0, realGapMin = 1e9, realGapMax = 0.0;

        void Add(double frame, double wait, double generatedPresent, double realGap)
        {
            ++frames;
            frameMs += frame;
            waitMs += wait;
            generatedPresentMs += generatedPresent;
            realGapMs += realGap;
            realGapMin = std::min(realGapMin, realGap);
            realGapMax = std::max(realGapMax, realGap);
            if (frames < 300)
                return;
            Log("Pacing over %u frames: frame %.2f ms, wait %.2f ms, generated Present call %.2f ms, rendered frames %.2f ms apart (%.2f..%.2f)",
                frames, frameMs / frames, waitMs / frames, generatedPresentMs / frames, realGapMs / frames, realGapMin, realGapMax);
            *this = {};
        }
    } Stats;

    void ReleaseTargets()
    {
        for (auto rt : { &PresentRT, &GeneratedRT, &HudLessRT })
        {
            if (*rt)
            {
                (*rt)->Destroy();
                *rt = nullptr;
            }
        }
        TargetWidth = TargetHeight = 0;
    }

    // Both at the back buffer's size, 16-bit float as the helper's textures
    bool CreateTargets(uint32_t width, uint32_t height)
    {
        if (PresentRT && GeneratedRT && HudLessRT && TargetWidth == width && TargetHeight == height)
            return true;
        ReleaseTargets();

        auto desc = rage::OwnRenderTargetDesc(rage::GRCFMT_A16B16G16R16F);
        PresentRT = rage::CreateEmptyRenderTarget("FrameGenerationPresent", width, height, 64, desc);
        GeneratedRT = rage::CreateEmptyRenderTarget("FrameGenerationGenerated", width, height, 64, desc);
        HudLessRT = rage::CreateEmptyRenderTarget("FrameGenerationHudLess", width, height, 64, desc);
        if (!PresentRT || !PresentRT->mD3DTexture || !GeneratedRT || !GeneratedRT->mD3DTexture || !HudLessRT || !HudLessRT->mD3DTexture)
        {
            ReleaseTargets();
            return false;
        }
        TargetWidth = width;
        TargetHeight = height;
        Log("Targets: %ux%u", width, height);
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

    // The generated frame goes to Present now, the rendered one, back in the back buffer, once the game presents it
    // after the wait
    void PresentGenerated(IDirect3DDevice9* device, IDirect3DSurface9* backBuffer, IDirect3DSurface9* generated, IDirect3DSurface9* rendered,
        LARGE_INTEGER lastRendered)
    {
        auto realDevice = RageDirect3DDevice9::m_pRealDevice ? *RageDirect3DDevice9::m_pRealDevice : nullptr;
        if (!realDevice || FAILED(device->StretchRect(generated, nullptr, backBuffer, nullptr, D3DTEXF_POINT)))
        {
            LogOnce(5, "The generated frame could not be copied into the back buffer");
            return;
        }

        // The game is inside its scene until it calls EndScene after this
        auto before = Now();
        realDevice->EndScene();
        auto hr = realDevice->Present(nullptr, nullptr, nullptr, nullptr);
        realDevice->BeginScene();
        auto generatedAt = Now();
        if (FAILED(hr))
            LogOnce(6, "Present of the generated frame failed");

        device->StretchRect(rendered, nullptr, backBuffer, nullptr, D3DTEXF_POINT);

        // The frame's own time: from the last rendered frame to this generated one, without the wait
        double frame = lastRendered.QuadPart ? Ms(lastRendered, generatedAt) : 0.0;
        bool steady = frame > 0.0 && frame < 250.0;
        if (steady)
            FrameMs = FrameMs > 0.0 ? FrameMs + (frame - FrameMs) * 0.1 : frame;

        double wait = steady ? std::clamp(FrameMs * fDelay, 0.0, 50.0) : 0.0;
        WaitUntil(generatedAt, wait);

        auto realAt = Now();
        if (steady)
            Stats.Add(frame, Ms(generatedAt, realAt), Ms(before, generatedAt), Ms(lastRendered, realAt));
        RealPresented = realAt;
    }

    // Render thread, after the frame is finished
    void OnBeforePresent()
    {
        bool hudLess = bHudLessCaptured;
        bHudLessCaptured = false;
        // Set again by a paced frame: after one that isn't, the time of a frame is not known
        auto lastRendered = RealPresented;
        RealPresented.QuadPart = 0;
        if (mode == Mode::Off || !Upscaler::IsFrameGenerationReady())
            return;

        auto device = rage::grcDevice::GetD3DDevice();
        if (!device)
            return;

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

        if (presentSurface && generatedSurface && SUCCEEDED(device->StretchRect(backBuffer, nullptr, presentSurface, nullptr, D3DTEXF_POINT)))
        {
            auto hdr = HDROutput::IsActive();
            if (Upscaler::Generate(PresentRT->mD3DTexture, hudLess ? HudLessRT->mD3DTexture : nullptr, GeneratedRT->mD3DTexture, hdr ? HDROutput::GetPeakNits() : 0.0f))
            {
                static bool first = true;
                if (first)
                    Log("First frame generated at %ux%u%s", desc.Width, desc.Height, hdr ? ", HDR" : "");
                first = false;

                if (mode == Mode::ShowGenerated)
                    device->StretchRect(generatedSurface, nullptr, backBuffer, nullptr, D3DTEXF_POINT);
                else if (!Upscaler::WasGenerateReset())
                    PresentGenerated(device, backBuffer, generatedSurface, presentSurface, lastRendered);
            }
            else
            {
                LogOnce(2, "Generate failed, see GTAIV.EFLC.FusionFix.UpscalerGame.log and GTAIV.EFLC.FusionFix.Upscaler.log");
            }
        }
        else
        {
            LogOnce(3, "The back buffer could not be copied");
        }

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
        return mode != Mode::Off && !HDROutput::IsActive();
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
        bHudLessCaptured = CopyInto(device, backBuffer, HudLessRT);
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
            CIniReader iniReader("");
            mode = static_cast<Mode>(std::clamp(iniReader.ReadInteger("TEMPORAL", "FrameGeneration", 0), 0, 2));
            fDelay = std::clamp(iniReader.ReadFloat("TEMPORAL", "FrameGenerationDelay", 0.5f), 0.0f, 1.0f);
            QueryPerformanceFrequency(&Frequency);
            if (mode != Mode::Off)
                Log("Frame generation: %s", mode == Mode::ShowGenerated ? "showing the generated frames" : "on");

            FusionFix::onBeforePresent() += []()
            {
                OnBeforePresent();
            };

            FusionFix::onBeforeReset() += []()
            {
                ReleaseTargets();
            };
        };
    }
} FrameGenerationModule;
