module;

#include <common.hxx>
#include <cstdarg>
#include <cstdio>
#include <filesystem>
#include <map>
#include <string>

export module framegeneration;

import common;
import comvars;
import hdr;
import renderscale;
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
// - FrameGeneration = 1 in [TEMPORAL]: the game presents the generated frame in place of its own, which waits in
//   PresentRT for half a frame. Nothing waits for it: the game goes on with the next frame, and the draw calls of the
//   render thread check the time. The first one after the half (FrameGenerationDelay of the smoothed frame time)
//   presents the rendered frame and leaves the back buffer as it found it. A frame that ends before that presents
//   the waiting one first.
// - FrameGeneration = 2 shows the generated frames in place of the rendered ones, to check the generation itself;
//   3 paces as 1 with the rendered frame in place of the generated one, to check the pacing alone.

namespace
{
    enum class Mode : int32_t
    {
        Off = 0,
        On = 1,
        ShowGenerated = 2,  // the generated frame replaces the rendered one
        PaceRendered = 3,   // as On, with the rendered frame where the generated one would go: tells the pacing apart
    };

    Mode mode = Mode::Off;
    float fDelay = 0.5f;            // of the frame's own time, between the generated and the rendered frame

    // FrameGenerationDebug in [TEMPORAL], to find what breaks the Present within a frame
    namespace Debug
    {
        constexpr int32_t NoPresent = 1;        // the copies around it, without the Present
        constexpr int32_t NoCopies = 2;         // the Present of whatever the back buffer holds, without the copies
        constexpr int32_t EndOfFrame = 4;       // never within a frame: the waiting frame goes when the next one ends
        constexpr int32_t Brightness = 8;       // reads every finished frame back and counts the dark ones
        // Where and how the rendered frame may be presented within a frame
        constexpr int32_t NotOnBackBuffer = 16; // not while render target 0 is the back buffer
        constexpr int32_t BeforePost = 32;      // only before the upscaler's Evaluate, which starts the post processing
        constexpr int32_t AfterPost = 64;       // only once the frame before the HUD was captured
        constexpr int32_t NoScene = 128;        // no EndScene and BeginScene around it
        constexpr int32_t NoRebind = 256;       // the targets are not set again after it
    }
    int32_t nDebug = 0;

    rage::grcRenderTargetPC* PresentRT = nullptr;
    rage::grcRenderTargetPC* GeneratedRT = nullptr;
    rage::grcRenderTargetPC* HudLessRT = nullptr;
    rage::grcRenderTargetPC* SavedRT = nullptr;     // the back buffer while the rendered frame is presented
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
    LARGE_INTEGER LastFrameEnd{};   // the previous frame's OnBeforePresent, 0 before the first
    double FrameMs = 0.0;           // smoothed time between two rendered frames
    DWORD RenderThread = 0;

    // The rendered frame waiting in PresentRT for its Present, after the generated one went
    bool bPending = false;
    bool bInPresent = false;
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
            Log("Pacing over %u frames: frame %.2f ms, rendered frame %.2f ms after the generated one (%.2f..%.2f, aimed at %.2f), %u late",
                frames, frameMs / frames, gapMs / frames, gapMin, gapMax, delayMs / frames, late);
            *this = {};
        }
    } Stats;

    // Debug::Brightness: each finished frame, as the game left it, is read back at 8x8 and compared with the frames
    // before it. Frames that had the rendered frame presented within them are counted apart.
    IDirect3DSurface9* ProbeRT = nullptr;
    IDirect3DSurface9* ProbeMemory = nullptr;
    bool bPresentedWithin = false;

    // What was bound when the rendered frame was presented within a frame, by kind, against the dark frames
    uint32_t DrawsThisFrame = 0;
    struct PresentPoint
    {
        uint32_t draws = 0;
        double ms = 0.0;        // since the previous frame ended
        std::string phase;      // the render phase of rage, by its class
        std::string target;     // "back buffer" or the size of render target 0
        std::string depth;      // the size of the depth buffer, or "none"
        bool afterEvaluate = false;
        bool inPost = false;
        bool afterHudLess = false;
    } LastPoint;

    // The class name of a polymorphic object of the game, from its RTTI
    const char* ClassName(const void* object)
    {
        __try
        {
            auto vtable = *reinterpret_cast<const uintptr_t* const*>(object);
            auto locator = reinterpret_cast<const uint32_t*>(vtable[-1]);
            auto name = reinterpret_cast<const char*>(locator[3]) + 8;
            return name[0] == '.' && name[1] == '?' ? name + 4 : "?";
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return "?";
        }
    }

    std::string CurrentPhase()
    {
        if (!CRenderPhase::sm_pCurrent || !*CRenderPhase::sm_pCurrent)
            return "none";
        std::string name = ClassName(reinterpret_cast<const void*>(*CRenderPhase::sm_pCurrent));
        if (auto at = name.find("@@"); at != std::string::npos)
            name.resize(at);
        return name;
    }
    std::map<std::string, std::pair<uint32_t, uint32_t>> PointsByKind;   // presents, dark frames
    uint32_t DarkLogged = 0;

    std::string SurfaceKind(IDirect3DSurface9* surface, IDirect3DSurface9* backBuffer)
    {
        if (!surface)
            return "none";
        if (surface == backBuffer)
            return "back buffer";
        D3DSURFACE_DESC desc{};
        surface->GetDesc(&desc);
        char text[48];
        snprintf(text, sizeof(text), "%ux%u fmt %d", desc.Width, desc.Height, desc.Format);
        return text;
    }

    struct BrightnessStats
    {
        uint32_t frames = 0, dark = 0, dim = 0, within = 0, darkWithin = 0;
        double average = 0.0;
    } Brightness;

    void ProbeBrightness(IDirect3DDevice9* device)
    {
        bool within = bPresentedWithin;
        bPresentedWithin = false;
        if (!ProbeRT && FAILED(device->CreateRenderTarget(8, 8, D3DFMT_A8R8G8B8, D3DMULTISAMPLE_NONE, 0, FALSE, &ProbeRT, nullptr)))
            return;
        if (!ProbeMemory && FAILED(device->CreateOffscreenPlainSurface(8, 8, D3DFMT_A8R8G8B8, D3DPOOL_SYSTEMMEM, &ProbeMemory, nullptr)))
            return;

        IDirect3DSurface9* backBuffer = nullptr;
        if (FAILED(device->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &backBuffer)) || !backBuffer)
            return;
        bool read = SUCCEEDED(device->StretchRect(backBuffer, nullptr, ProbeRT, nullptr, D3DTEXF_LINEAR)) &&
            SUCCEEDED(device->GetRenderTargetData(ProbeRT, ProbeMemory));
        backBuffer->Release();

        D3DLOCKED_RECT locked{};
        if (!read || FAILED(ProbeMemory->LockRect(&locked, nullptr, D3DLOCK_READONLY)))
            return;
        double sum = 0.0;
        for (int y = 0; y < 8; ++y)
        {
            auto row = reinterpret_cast<const uint8_t*>(locked.pBits) + y * locked.Pitch;
            for (int x = 0; x < 8; ++x)
                sum += row[x * 4] + row[x * 4 + 1] + row[x * 4 + 2];
        }
        ProbeMemory->UnlockRect();
        auto value = sum / (64.0 * 3.0);

        auto& b = Brightness;
        bool dark = b.average > 8.0 && value < b.average * 0.25;
        bool dim = b.average > 8.0 && value < b.average * 0.6;
        b.dim += dim;
        if (within)
        {
            auto& kind = PointsByKind[LastPoint.phase + ", target " + LastPoint.target];
            ++kind.first;
            kind.second += dim;
            if (dim && DarkLogged++ < 40)
                Log("%s frame %.1f against %.1f: presented %.1f ms into it after %u draw calls, phase %s, target %s, depth %s, %s, %s, %s",
                    dark ? "Dark" : "Dim", value, b.average, LastPoint.ms, LastPoint.draws, LastPoint.phase.c_str(), LastPoint.target.c_str(),
                    LastPoint.depth.c_str(), LastPoint.afterEvaluate ? "after Evaluate" : "before Evaluate",
                    LastPoint.inPost ? "in post" : "not in post", LastPoint.afterHudLess ? "after the HUD-less copy" : "before the HUD-less copy");
        }
        b.average = b.average > 0.0 ? b.average + (value - b.average) * 0.05 : value;
        ++b.frames;
        b.dark += dark;
        b.within += within;
        b.darkWithin += dark && within;
        if (b.frames < 300)
            return;
        Log("Brightness over %u frames: average %.1f, %u dark, %u dim; %u had the rendered frame presented within them, %u of those dark",
            b.frames, b.average, b.dark, b.dim, b.within, b.darkWithin);
        for (auto& [kind, counts] : PointsByKind)
            Log("  presented in %s: %u times, %u dim or dark", kind.c_str(), counts.first, counts.second);
        PointsByKind.clear();
        auto average = b.average;
        b = {};
        b.average = average;
    }

    void ReleaseTargets()
    {
        bPending = false;
        SAFE_RELEASE(ProbeRT);
        SAFE_RELEASE(ProbeMemory);
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

    // Both at the back buffer's size, 16-bit float as the helper's textures
    bool CreateTargets(uint32_t width, uint32_t height)
    {
        if (PresentRT && GeneratedRT && HudLessRT && SavedRT && TargetWidth == width && TargetHeight == height)
            return true;
        ReleaseTargets();

        auto desc = rage::OwnRenderTargetDesc(rage::GRCFMT_A16B16G16R16F);
        PresentRT = rage::CreateEmptyRenderTarget("FrameGenerationPresent", width, height, 64, desc);
        GeneratedRT = rage::CreateEmptyRenderTarget("FrameGenerationGenerated", width, height, 64, desc);
        HudLessRT = rage::CreateEmptyRenderTarget("FrameGenerationHudLess", width, height, 64, desc);
        SavedRT = rage::CreateEmptyRenderTarget("FrameGenerationSaved", width, height, 64, desc);
        if (!PresentRT || !PresentRT->mD3DTexture || !GeneratedRT || !GeneratedRT->mD3DTexture || !HudLessRT || !HudLessRT->mD3DTexture ||
            !SavedRT || !SavedRT->mD3DTexture)
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

    // Copies a target into a surface of the same size
    bool CopyFrom(IDirect3DDevice9* device, rage::grcRenderTargetPC* source, IDirect3DSurface9* target)
    {
        IDirect3DSurface9* surface = nullptr;
        source->mD3DTexture->GetSurfaceLevel(0, &surface);
        bool ok = surface && SUCCEEDED(device->StretchRect(surface, nullptr, target, nullptr, D3DTEXF_POINT));
        SAFE_RELEASE(surface);
        return ok;
    }

    // Presents the waiting rendered frame, the game's back buffer kept as it was. late: the next frame ended first.
    void PresentPending(IDirect3DDevice9* device, bool late)
    {
        bPending = false;
        IDirect3DSurface9* backBuffer = nullptr;
        if (FAILED(device->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &backBuffer)) || !backBuffer)
            return;

        bInPresent = true;
        bool copies = !(nDebug & Debug::NoCopies);
        if (!copies || (CopyInto(device, backBuffer, SavedRT) && CopyFrom(device, PresentRT, backBuffer)))
        {
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
            if (nDebug & Debug::Brightness)
                LastPoint = { DrawsThisFrame, Ms(LastFrameEnd, Now()), CurrentPhase(), SurfaceKind(targets[0], backBuffer), SurfaceKind(depth, nullptr),
                    Upscaler::IsFrameGenerationReady(), RenderScale::IsInPost(), bHudLessCaptured };

            // The game is inside its scene
            auto presentedAt = Now();
            if (!(nDebug & Debug::NoPresent))
            {
                if (!(nDebug & Debug::NoScene))
                    device->EndScene();
                if (FAILED(device->Present(nullptr, nullptr, nullptr, nullptr)))
                    LogOnce(6, "Present of the rendered frame failed");
                if (!(nDebug & Debug::NoScene))
                    device->BeginScene();
            }

            // The game's back buffer as it was, in whichever surface is the back buffer now
            IDirect3DSurface9* current = nullptr;
            if (SUCCEEDED(device->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &current)) && current)
            {
                if (current != backBuffer)
                    LogOnce(7, "Present changed the back buffer surface");
                if (copies)
                    CopyFrom(device, SavedRT, current);
                current->Release();
            }

            if (!(nDebug & Debug::NoRebind))
            {
                for (DWORD i = 0; i < 4; ++i)
                    if (targets[i] || i > 0)
                        device->SetRenderTarget(i, targets[i]);
                device->SetDepthStencilSurface(depth);
                device->SetViewport(&viewport);
                device->SetScissorRect(&scissor);
            }
            for (auto& target : targets)
                SAFE_RELEASE(target);
            SAFE_RELEASE(depth);

            Stats.Add(Ms(GeneratedAt, presentedAt), Ms(GeneratedAt, PendingDue), late);
            bPresentedWithin = !late;
        }
        else
        {
            LogOnce(5, "The rendered frame could not be put back into the back buffer");
        }
        bInPresent = false;
        backBuffer->Release();
    }

    // Draw calls of the render thread: the rendered frame goes once its time has come
    void CheckPending(IDirect3DDevice9* device)
    {
        if (GetCurrentThreadId() == RenderThread)
            ++DrawsThisFrame;
        if (!bPending || bInPresent || (nDebug & Debug::EndOfFrame) || GetCurrentThreadId() != RenderThread || Now().QuadPart < PendingDue.QuadPart)
            return;

        // Experiments: only in some parts of the frame
        if ((nDebug & Debug::BeforePost) && Upscaler::IsFrameGenerationReady())
            return;
        if ((nDebug & Debug::AfterPost) && !bHudLessCaptured)
            return;
        if (nDebug & Debug::NotOnBackBuffer)
        {
            IDirect3DSurface9* target = nullptr;
            IDirect3DSurface9* backBuffer = nullptr;
            device->GetRenderTarget(0, &target);
            device->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &backBuffer);
            bool onBackBuffer = target && target == backBuffer;
            SAFE_RELEASE(target);
            SAFE_RELEASE(backBuffer);
            if (onBackBuffer)
                return;
        }
        PresentPending(device, false);
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

        auto previous = mode;
        mode = static_cast<Mode>(std::clamp(iniReader.ReadInteger("TEMPORAL", "FrameGeneration", 0), 0, 3));
        fDelay = std::clamp(iniReader.ReadFloat("TEMPORAL", "FrameGenerationDelay", 0.5f), 0.0f, 1.0f);
        nDebug = iniReader.ReadInteger("TEMPORAL", "FrameGenerationDebug", 0);
        Log("Frame generation: %s, delay %.2f, debug %d", mode == Mode::Off ? "off" : mode == Mode::ShowGenerated ? "showing the generated frames" :
            mode == Mode::PaceRendered ? "pacing the rendered frames only" : "on", fDelay, nDebug);

        // A fresh start for the pacing and the statistics
        if (mode != previous)
        {
            LastFrameEnd.QuadPart = 0;
            bPending = false;
        }
        Stats = {};
        Brightness = {};
        PointsByKind.clear();
        DarkLogged = 0;
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

    // Render thread, after the frame is finished
    void OnBeforePresent()
    {
        CheckSettings();
        bool hudLess = bHudLessCaptured;
        bHudLessCaptured = false;
        if (mode == Mode::Off)
            return;

        auto device = RageDirect3DDevice9::m_pRealDevice ? *RageDirect3DDevice9::m_pRealDevice : nullptr;
        if (!device)
            return;

        RenderThread = GetCurrentThreadId();
        auto now = Now();
        if (LastFrameEnd.QuadPart)
        {
            auto frame = Ms(LastFrameEnd, now);
            if (frame > 0.0 && frame < 250.0)
                FrameMs = FrameMs > 0.0 ? FrameMs + (frame - FrameMs) * 0.1 : frame;
        }
        LastFrameEnd = now;

        if (nDebug & Debug::Brightness)
            ProbeBrightness(device);
        DrawsThisFrame = 0;

        // This frame ended before the last one went: it goes first
        if (bPending)
            PresentPending(device, true);

        if (!Upscaler::IsFrameGenerationReady())
            return;
        bool pacing = mode == Mode::On || mode == Mode::PaceRendered;
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

        if (presentSurface && generatedSurface && SUCCEEDED(device->StretchRect(backBuffer, nullptr, presentSurface, nullptr, D3DTEXF_POINT)))
        {
            auto hdr = HDROutput::IsActive();
            if (Upscaler::Generate(PresentRT->mD3DTexture, hudLess ? HudLessRT->mD3DTexture : nullptr, GeneratedRT->mD3DTexture, hdr ? HDROutput::GetPeakNits() : 0.0f))
            {
                static bool first = true;
                if (first)
                    Log("First frame generated at %ux%u%s", desc.Width, desc.Height, hdr ? ", HDR" : "");
                first = false;

                // The game presents the generated frame, the rendered one waits in PresentRT. Not a frame generated
                // without a previous one, nor before the frame time is known.
                bool paced = pacing && !Upscaler::WasGenerateReset() && FrameMs > 0.0;
                if (mode == Mode::ShowGenerated || (paced && mode == Mode::On))
                    device->StretchRect(generatedSurface, nullptr, backBuffer, nullptr, D3DTEXF_POINT);
                if (paced)
                {
                    GeneratedAt = Now();
                    PendingDue = After(GeneratedAt, std::clamp(FrameMs * fDelay, 0.0, 50.0));
                    bPending = true;
                }
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
            QueryPerformanceFrequency(&Frequency);
            ReadSettings();

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
