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
// - Once the frame is finished, HUD and HDR output included, the back buffer is copied into Present and the helper
//   generates the frame between it and the previous one into Generated.
// - First stage: FrameGeneration = 2 in [TEMPORAL] shows the generated frames in place of the rendered ones, to
//   check the generation itself. The game presents them at its own pace, nothing is paced yet.

namespace
{
    enum class Mode : int32_t
    {
        Off = 0,
        On = 1,
        ShowGenerated = 2,  // the generated frame replaces the rendered one
    };

    Mode mode = Mode::Off;

    rage::grcRenderTargetPC* PresentRT = nullptr;
    rage::grcRenderTargetPC* GeneratedRT = nullptr;
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

    void ReleaseTargets()
    {
        for (auto rt : { &PresentRT, &GeneratedRT })
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
        if (PresentRT && GeneratedRT && TargetWidth == width && TargetHeight == height)
            return true;
        ReleaseTargets();

        auto desc = rage::OwnRenderTargetDesc(rage::GRCFMT_A16B16G16R16F);
        PresentRT = rage::CreateEmptyRenderTarget("FrameGenerationPresent", width, height, 64, desc);
        GeneratedRT = rage::CreateEmptyRenderTarget("FrameGenerationGenerated", width, height, 64, desc);
        if (!PresentRT || !PresentRT->mD3DTexture || !GeneratedRT || !GeneratedRT->mD3DTexture)
        {
            ReleaseTargets();
            return false;
        }
        TargetWidth = width;
        TargetHeight = height;
        Log("Targets: %ux%u", width, height);
        return true;
    }

    // Render thread, after the frame is finished
    void OnBeforePresent()
    {
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
            if (Upscaler::Generate(PresentRT->mD3DTexture, GeneratedRT->mD3DTexture, hdr ? HDROutput::GetPeakNits() : 0.0f))
            {
                static bool first = true;
                if (first)
                    Log("First frame generated at %ux%u%s", desc.Width, desc.Height, hdr ? ", HDR" : "");
                first = false;

                if (mode == Mode::ShowGenerated)
                    device->StretchRect(generatedSurface, nullptr, backBuffer, nullptr, D3DTEXF_POINT);
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
