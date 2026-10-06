module;

#include <common.hxx>
#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <string_view>

export module renderscale;

import common;
import comvars;
import hdr;
import settings;
import upscaler;

// Render scale: the scene renders at a fraction of the screen size and DLSS or FSR upscales it.
//
// - The fraction is the Upscaler Quality of the menu, with DLAA or FSR as the antialiasing and the upscaler
//   available. It's decided when the game creates its targets; a change resets the device in the current display
//   mode, which creates them again.
// - The scene's targets are created smaller: the G-buffer, the material ID target, the depth buffers and
//   FullScreenCopy, which the lighting, water, fog and transparent geometry draw into. Everything else, the
//   post processing and the interface included, keeps the screen size.
// - The game rarely sets viewports itself: SetRenderTarget sets one the size of the target. Where it sets one the
//   size of the screen on a smaller target, it's scaled to the target, and so are scissor rectangles.
// - Shaders that turn the pixel position into texture coordinates divide it by globalScreenSize or, for the
//   lights, deferredLightScreenSize. The game computes both from the screen size; from the G-buffer pass of the
//   main scene to the post processing they get the render size. Frames without a scene, like menus, keep the
//   screen size.
// - A target of the screen size with the smaller depth buffer (the interface draws into the back buffer with
//   _BACK_ZBUFFER_) gets a depth buffer of the screen size instead.
// - Post processing starts with the scene upscaled into a texture of the screen size, which then stands in for
//   FullScreenCopy until the frame ends (see BeginPost in postfx.ixx).

export namespace RenderScale
{
    // For a trace (PostFX's SSR trace sets it): every depth buffer the hooks put in the game's place and every clear
    // of a depth buffer, with the depth buffer, the bound target 0 and the clear flags (0 for a swap).
    inline void (*TraceDepth)(const char* what, IDirect3DSurface9* depth, IDirect3DSurface9* target, DWORD flags) = nullptr;
}

namespace
{
    float fScale = 1.0f;             // of the targets the game creates, decided at their first one
    bool bScaleDecided = false;
    float fRequestedScale = 0.0f;    // the scale a device reset was asked for, so it's asked once
    bool bActive = false;            // the scene's targets were created at the render size
    bool bInScene = false;           // from the G-buffer pass of the main scene to the post processing
    bool bInPost = false;            // FullScreenCopy is the full size texture, until the frame ends
    uint32_t DisplayWidth = 0;
    uint32_t DisplayHeight = 0;
    uint32_t RenderWidth = 0;
    uint32_t RenderHeight = 0;

    // FullScreenCopy at the screen size, and the targets FullScreenCopy had at the render size. The game
    // leaves mD3DSurface of its targets empty at times, the scene's surface is taken from its texture.
    IDirect3DTexture9* OutputTexture = nullptr;
    IDirect3DSurface9* OutputSurface = nullptr;
    rage::grcRenderTargetPC* SwappedRT = nullptr;
    IDirect3DTexture9* SceneTexture = nullptr;
    IDirect3DSurface9* SceneSurface = nullptr;      // mD3DSurface as the game had it
    IDirect3DSurface9* SceneLevel = nullptr;        // level 0 of SceneTexture, referenced

    // GTAIV.EFLC.FusionFix.RenderScale.log next to the plugin: the sizes, the hooks, and why the post
    // processing could not start at the screen size, each reason once
    void Log(const char* format, ...)
    {
        static bool started = false;
        FILE* f = nullptr;
        if (_wfopen_s(&f, (GetThisModulePath() / L"GTAIV.EFLC.FusionFix.RenderScale.log").c_str(), started ? L"a" : L"w") || !f)
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

    // Depth buffer of the screen size for targets of the screen size, and the scene's one it stands in for
    IDirect3DSurface9* FullDepth = nullptr;
    IDirect3DSurface9* SceneDepth = nullptr;

    constexpr std::string_view ScaledTargets[] =
    {
        "_DEFERRED_GBUFFER_0_", "_DEFERRED_GBUFFER_1_", "_DEFERRED_GBUFFER_2_", "_DEFERRED_GBUFFER_3_", "_DEFERRED_GBUFFER_1_COPY",
        "_STENCIL_BUFFER_", "_BACK_ZBUFFER_", "FullScreenCopy",
    };

    // Upscaler Quality: native (DLAA), quality, balanced, performance, ultra performance
    constexpr float QualityScales[] = { 1.0f, 0.667f, 0.58f, 0.5f, 0.333f };

    float DesiredScale()
    {
        static auto antialiasing = FusionFixSettings.GetRef("PREF_ANTIALIASING");
        static auto quality = FusionFixSettings.GetRef("PREF_UPSCALER_QUALITY");
        if (!antialiasing || !quality)
            return 1.0f;
        auto mode = antialiasing->get();
        if (mode != FusionFixSettings.AntialiasingText.eDLAA && mode != FusionFixSettings.AntialiasingText.eFSR)
            return 1.0f;
        // Until the helper has started the upscaler counts as there
        auto backend = mode == FusionFixSettings.AntialiasingText.eDLAA ? Upscaler::Backend::DLSS : Upscaler::Backend::FSR;
        if (Upscaler::IsSettled() && !Upscaler::IsAvailable(backend))
            return 1.0f;
        return QualityScales[std::clamp(quality->get(), 0, static_cast<int32_t>(std::size(QualityScales)) - 1)];
    }

    void DecideScale()
    {
        if (bScaleDecided)
            return;
        fScale = DesiredScale();
        bScaleDecided = true;
    }

    uint32_t Scaled(uint32_t size)
    {
        return std::max(1u, static_cast<uint32_t>(std::lround(static_cast<double>(size) * fScale)));
    }

    void CreateRTSize(const char* name, uint32_t& width, uint32_t& height)
    {
        DecideScale();
        if (fScale >= 1.0f || !rage::grcDevice::ms_nActiveWidth || !rage::grcDevice::ms_nActiveHeight)
            return;
        auto screenWidth = static_cast<uint32_t>(*rage::grcDevice::ms_nActiveWidth);
        auto screenHeight = static_cast<uint32_t>(*rage::grcDevice::ms_nActiveHeight);
        if (width != screenWidth || height != screenHeight)
            return;
        if (std::find(std::begin(ScaledTargets), std::end(ScaledTargets), std::string_view(name)) == std::end(ScaledTargets))
            return;

        DisplayWidth = screenWidth;
        DisplayHeight = screenHeight;
        RenderWidth = Scaled(screenWidth);
        RenderHeight = Scaled(screenHeight);
        width = RenderWidth;
        height = RenderHeight;
        if (!bActive || std::string_view(name) == "FullScreenCopy")
            Log("%s: %ux%u for a %ux%u screen", name, RenderWidth, RenderHeight, DisplayWidth, DisplayHeight);
        bActive = true;
    }

    bool SurfaceSize(IDirect3DSurface9* surface, uint32_t& width, uint32_t& height)
    {
        D3DSURFACE_DESC desc{};
        if (!surface || FAILED(surface->GetDesc(&desc)))
            return false;
        width = desc.Width;
        height = desc.Height;
        return true;
    }

    // The bound render target 0 and depth buffer with their sizes, kept by the device hooks so that the thousands of
    // calls a frame don't ask the device. Unknown after a reset, until the next call sets them.
    struct Bound
    {
        IDirect3DSurface9* surface = nullptr;
        uint32_t width = 0;
        uint32_t height = 0;
        bool known = false;

        void Set(IDirect3DSurface9* s)
        {
            surface = s;
            width = height = 0;
            SurfaceSize(s, width, height);
            known = true;
        }
    };
    Bound BoundTarget;
    Bound BoundDepth;

    // Size of render target 0
    bool TargetSize(IDirect3DDevice9* device, uint32_t& width, uint32_t& height)
    {
        if (!BoundTarget.known)
        {
            IDirect3DSurface9* rt = nullptr;
            if (FAILED(device->GetRenderTarget(0, &rt)))
                return false;
            BoundTarget.Set(rt);
            if (rt)
                rt->Release();
        }
        width = BoundTarget.width;
        height = BoundTarget.height;
        return BoundTarget.surface != nullptr;
    }

    Bound& DepthBound(IDirect3DDevice9* device)
    {
        if (!BoundDepth.known)
        {
            IDirect3DSurface9* ds = nullptr;
            device->GetDepthStencilSurface(&ds);
            BoundDepth.Set(ds);
            if (ds)
                ds->Release();
        }
        return BoundDepth;
    }

    bool IsRenderSize(uint32_t width, uint32_t height)
    {
        return width == RenderWidth && height == RenderHeight;
    }

    // ---------------------------------------------------------------------------------------------
    // Device hooks

    constexpr int SetRenderTargetIndex = 37;
    constexpr int SetDepthStencilSurfaceIndex = 39;
    constexpr int ClearIndex = 43;
    constexpr int SetViewportIndex = 47;
    constexpr int SetScissorRectIndex = 75;

    HRESULT(__stdcall* RealSetRenderTarget)(IDirect3DDevice9*, DWORD, IDirect3DSurface9*) = nullptr;
    HRESULT(__stdcall* RealSetDepthStencilSurface)(IDirect3DDevice9*, IDirect3DSurface9*) = nullptr;
    HRESULT(__stdcall* RealClear)(IDirect3DDevice9*, DWORD, const D3DRECT*, DWORD, D3DCOLOR, float, DWORD) = nullptr;
    HRESULT(__stdcall* RealSetViewport)(IDirect3DDevice9*, const D3DVIEWPORT9*) = nullptr;
    HRESULT(__stdcall* RealSetScissorRect)(IDirect3DDevice9*, const RECT*) = nullptr;
    void** HookedVTable = nullptr;

    // Binds a depth buffer past the hooks, and keeps track of it
    void BindDepth(IDirect3DDevice9* device, IDirect3DSurface9* surface)
    {
        if (SUCCEEDED(RealSetDepthStencilSurface(device, surface)))
            BoundDepth.Set(surface);
    }

    // Pairs the scene's depth buffer with targets of its size, and the full size one with the others
    void MatchDepth(IDirect3DDevice9* device)
    {
        uint32_t rtWidth = 0, rtHeight = 0;
        if (!TargetSize(device, rtWidth, rtHeight))
            return;
        auto& ds = DepthBound(device);
        if (!ds.surface)
            return;

        if (ds.surface != FullDepth && IsRenderSize(ds.width, ds.height) && (rtWidth > ds.width || rtHeight > ds.height))
        {
            // INTZ is a texture format, a plain depth surface takes D24S8
            if (!FullDepth)
                device->CreateDepthStencilSurface(DisplayWidth, DisplayHeight, D3DFMT_D24S8, D3DMULTISAMPLE_NONE, 0, FALSE, &FullDepth, nullptr);
            if (FullDepth && rtWidth <= DisplayWidth && rtHeight <= DisplayHeight)
            {
                SceneDepth = ds.surface;
                BindDepth(device, FullDepth);
                if (RenderScale::TraceDepth)
                    RenderScale::TraceDepth("full size depth in for the scene's", SceneDepth, BoundTarget.surface, 0);
            }
        }
        else if (ds.surface == FullDepth && IsRenderSize(rtWidth, rtHeight) && SceneDepth)
        {
            BindDepth(device, SceneDepth);
            if (RenderScale::TraceDepth)
                RenderScale::TraceDepth("scene's depth back in for the full size one", SceneDepth, BoundTarget.surface, 0);
        }
    }

    HRESULT __stdcall SetRenderTarget(IDirect3DDevice9* device, DWORD index, IDirect3DSurface9* surface)
    {
        auto hr = RealSetRenderTarget(device, index, surface);
        if (index == 0 && SUCCEEDED(hr))
        {
            BoundTarget.Set(surface);
            if (bActive && surface)
                MatchDepth(device);
        }
        return hr;
    }

    HRESULT __stdcall SetDepthStencilSurface(IDirect3DDevice9* device, IDirect3DSurface9* surface)
    {
        auto hr = RealSetDepthStencilSurface(device, surface);
        if (SUCCEEDED(hr))
        {
            BoundDepth.Set(surface);
            if (bActive && surface)
                MatchDepth(device);
        }
        return hr;
    }

    // The game clears _BACK_ZBUFFER_ with the back buffer bound, where the full size depth buffer stands in for it:
    // both are cleared
    HRESULT __stdcall Clear(IDirect3DDevice9* device, DWORD count, const D3DRECT* rects, DWORD flags, D3DCOLOR color, float z, DWORD stencil)
    {
        auto hr = RealClear(device, count, rects, flags, color, z, stencil);
        auto depthFlags = flags & (D3DCLEAR_ZBUFFER | D3DCLEAR_STENCIL);
        if (depthFlags && RenderScale::TraceDepth)
            RenderScale::TraceDepth(count ? "clear of part of" : "clear of", DepthBound(device).surface, BoundTarget.surface, flags);
        if (bActive && depthFlags && count == 0 && FullDepth && SceneDepth && DepthBound(device).surface == FullDepth)
        {
            RealSetDepthStencilSurface(device, SceneDepth);
            RealClear(device, 0, nullptr, depthFlags, 0, z, stencil);
            if (RenderScale::TraceDepth)
                RenderScale::TraceDepth("clear passed on to the scene's depth", SceneDepth, BoundTarget.surface, depthFlags);
            RealSetDepthStencilSurface(device, FullDepth);
        }
        return hr;
    }

    // A viewport or rectangle of the screen on a target of the render size is scaled to the target
    template <class T>
    void ScaleRect(T& x0, T& y0, T& x1, T& y1, uint32_t width, uint32_t height)
    {
        auto sx = static_cast<double>(RenderWidth) / static_cast<double>(DisplayWidth);
        auto sy = static_cast<double>(RenderHeight) / static_cast<double>(DisplayHeight);
        x0 = static_cast<T>(std::clamp<double>(std::floor(x0 * sx), 0.0, width));
        y0 = static_cast<T>(std::clamp<double>(std::floor(y0 * sy), 0.0, height));
        x1 = static_cast<T>(std::clamp<double>(std::ceil(x1 * sx), 0.0, width));
        y1 = static_cast<T>(std::clamp<double>(std::ceil(y1 * sy), 0.0, height));
    }

    HRESULT __stdcall SetViewport(IDirect3DDevice9* device, const D3DVIEWPORT9* vp)
    {
        uint32_t width = 0, height = 0;
        if (bActive && vp && TargetSize(device, width, height) && IsRenderSize(width, height) &&
            (vp->X + vp->Width > width || vp->Y + vp->Height > height))
        {
            D3DVIEWPORT9 scaled = *vp;
            uint32_t x0 = vp->X, y0 = vp->Y, x1 = vp->X + vp->Width, y1 = vp->Y + vp->Height;
            ScaleRect(x0, y0, x1, y1, width, height);
            scaled.X = x0;
            scaled.Y = y0;
            scaled.Width = std::max(1u, x1 - x0);
            scaled.Height = std::max(1u, y1 - y0);
            return RealSetViewport(device, &scaled);
        }
        return RealSetViewport(device, vp);
    }

    HRESULT __stdcall SetScissorRect(IDirect3DDevice9* device, const RECT* rect)
    {
        uint32_t width = 0, height = 0;
        if (bActive && rect && TargetSize(device, width, height) && IsRenderSize(width, height) &&
            (static_cast<uint32_t>(std::max(0L, rect->right)) > width || static_cast<uint32_t>(std::max(0L, rect->bottom)) > height))
        {
            RECT scaled = *rect;
            ScaleRect(scaled.left, scaled.top, scaled.right, scaled.bottom, width, height);
            return RealSetScissorRect(device, &scaled);
        }
        return RealSetScissorRect(device, rect);
    }

    template <class F>
    void Patch(void** vtable, int index, F& real, F hook)
    {
        if (vtable[index] == reinterpret_cast<void*>(hook))
            return;
        real = reinterpret_cast<F>(vtable[index]);
        injector::WriteMemory(&vtable[index], reinterpret_cast<void*>(hook), true);
    }

    void InstallHooks()
    {
        auto device = RageDirect3DDevice9::m_pRealDevice ? *RageDirect3DDevice9::m_pRealDevice : nullptr;
        if (!device)
            return;
        auto vtable = *reinterpret_cast<void***>(device);
        if (vtable == HookedVTable)
            return;
        Patch(vtable, SetRenderTargetIndex, RealSetRenderTarget, &SetRenderTarget);
        Patch(vtable, SetDepthStencilSurfaceIndex, RealSetDepthStencilSurface, &SetDepthStencilSurface);
        Patch(vtable, ClearIndex, RealClear, &Clear);
        Patch(vtable, SetViewportIndex, RealSetViewport, &SetViewport);
        Patch(vtable, SetScissorRectIndex, RealSetScissorRect, &SetScissorRect);
        HookedVTable = vtable;
    }

    // ---------------------------------------------------------------------------------------------
    // Screen size constants

    SafetyHookInline shSetGlobalScreenSize{};

    // globalScreenSize = (width, height, 1 / width, 1 / height)
    void __cdecl SetGlobalScreenSize(const float* size)
    {
        if (bActive && bInScene && size && size[0] == static_cast<float>(DisplayWidth) && size[1] == static_cast<float>(DisplayHeight))
        {
            float scaled[2] = { static_cast<float>(RenderWidth), static_cast<float>(RenderHeight) };
            shSetGlobalScreenSize.ccall(scaled);
            return;
        }
        shSetGlobalScreenSize.ccall(size);
    }

    void ApplyGlobalScreenSize(bool scene)
    {
        if (!shSetGlobalScreenSize)
            return;
        float size[2] = { static_cast<float>(scene ? RenderWidth : DisplayWidth), static_cast<float>(scene ? RenderHeight : DisplayHeight) };
        shSetGlobalScreenSize.ccall(size);
    }

    // ---------------------------------------------------------------------------------------------

    void RestoreScene()
    {
        if (SwappedRT)
        {
            SwappedRT->mD3DTexture = SceneTexture;
            SwappedRT->mD3DSurface = SceneSurface;
            SwappedRT->mWidth = static_cast<uint16_t>(RenderWidth);
            SwappedRT->mHeight = static_cast<uint16_t>(RenderHeight);
            SwappedRT = nullptr;
        }
        if (SceneLevel)
            SceneLevel->Release();
        SceneLevel = nullptr;
    }

    void ReleaseOutput()
    {
        RestoreScene();
        if (OutputSurface)
            OutputSurface->Release();
        if (OutputTexture)
            OutputTexture->Release();
        if (FullDepth)
            FullDepth->Release();
        OutputSurface = nullptr;
        OutputTexture = nullptr;
        FullDepth = nullptr;
        SceneDepth = nullptr;
        BoundTarget = {};
        BoundDepth = {};
        bInPost = false;
        bInScene = false;
        // The targets are created again, at the scale of the moment
        bActive = false;
        bScaleDecided = false;
        fRequestedScale = 0.0f;
    }

    // The frame is over: FullScreenCopy is the scene's target again
    void EndFrame()
    {
        if (bInScene)
        {
            // A scene without post processing
            bInScene = false;
            ApplyGlobalScreenSize(false);
        }
        if (!bInPost)
            return;
        RestoreScene();
        bInPost = false;
    }
}

export namespace RenderScale
{
    // The scene renders at the render size this session
    bool IsActive()
    {
        return bActive;
    }

    float GetScale()
    {
        return bActive ? static_cast<float>(RenderWidth) / static_cast<float>(DisplayWidth) : 1.0f;
    }

    uint32_t GetRenderWidth() { return bActive ? RenderWidth : 0; }
    uint32_t GetRenderHeight() { return bActive ? RenderHeight : 0; }
    uint32_t GetDisplayWidth() { return bActive ? DisplayWidth : 0; }
    uint32_t GetDisplayHeight() { return bActive ? DisplayHeight : 0; }

    // Render thread, at the G-buffer pass of the main scene
    void BeginScene()
    {
        if (!bActive || bInScene)
            return;
        bInScene = true;
        ApplyGlobalScreenSize(true);
    }

    // A size of the screen as the scene renders it, also while the targets are being created again
    uint32_t ToRenderWidth(uint32_t width)
    {
        DecideScale();
        bool screen = rage::grcDevice::ms_nActiveWidth && width == static_cast<uint32_t>(*rage::grcDevice::ms_nActiveWidth);
        return fScale < 1.0f && screen ? Scaled(width) : width;
    }

    uint32_t ToRenderHeight(uint32_t height)
    {
        DecideScale();
        bool screen = rage::grcDevice::ms_nActiveHeight && height == static_cast<uint32_t>(*rage::grcDevice::ms_nActiveHeight);
        return fScale < 1.0f && screen ? Scaled(height) : height;
    }

    // Render thread, at the start of post processing: FullScreenCopy becomes a texture of the screen size
    // for the rest of the frame. Returns the scene at the render size; the caller fills the returned
    // full size surface from it.
    bool BeginPost(IDirect3DDevice9* device, IDirect3DTexture9*& scene, IDirect3DSurface9*& sceneSurface, IDirect3DSurface9*& output)
    {
        if (!bActive || bInPost)
            return false;
        auto rt = rage::grcTextureFactoryPC::GetRTByName("FullScreenCopy");
        if (!rt || !rt->mD3DTexture)
        {
            LogOnce(0, "post: FullScreenCopy has no texture");
            return false;
        }
        D3DSURFACE_DESC desc{};
        rt->mD3DTexture->GetLevelDesc(0, &desc);
        if (!IsRenderSize(desc.Width, desc.Height))
        {
            LogOnce(1, "post: FullScreenCopy is not at the render size");
            return false;
        }

        if (!OutputTexture)
        {
            if (FAILED(device->CreateTexture(DisplayWidth, DisplayHeight, 1, D3DUSAGE_RENDERTARGET, desc.Format, D3DPOOL_DEFAULT, &OutputTexture, nullptr)) || !OutputTexture)
            {
                LogOnce(2, "post: the full size texture could not be created");
                return false;
            }
            OutputTexture->GetSurfaceLevel(0, &OutputSurface);
            if (!OutputSurface)
                return false;
            Log("post: FullScreenCopy is %ux%u from the post processing on (mD3DSurface %s)", DisplayWidth, DisplayHeight, rt->mD3DSurface ? "set" : "empty");
        }

        if (FAILED(rt->mD3DTexture->GetSurfaceLevel(0, &SceneLevel)) || !SceneLevel)
            return false;
        SceneTexture = rt->mD3DTexture;
        SceneSurface = rt->mD3DSurface;
        rt->mD3DTexture = OutputTexture;
        if (rt->mD3DSurface)
            rt->mD3DSurface = OutputSurface;
        rt->mWidth = static_cast<uint16_t>(DisplayWidth);
        rt->mHeight = static_cast<uint16_t>(DisplayHeight);
        SwappedRT = rt;
        bInPost = true;
        bInScene = false;
        ApplyGlobalScreenSize(false);

        // The full size depth buffer only exists once the device hooks made it, and starts the post processing empty.
        // The hooked functions are the D3D9 runtime's own: they take the real device, not the game's wrapper that
        // grcDevice hands out.
        auto realDevice = RageDirect3DDevice9::m_pRealDevice ? *RageDirect3DDevice9::m_pRealDevice : nullptr;
        if (FullDepth && RealSetDepthStencilSurface && RealClear && realDevice)
        {
            IDirect3DSurface9* oldDepth = nullptr;
            realDevice->GetDepthStencilSurface(&oldDepth);
            RealSetDepthStencilSurface(realDevice, FullDepth);
            RealClear(realDevice, 0, nullptr, D3DCLEAR_ZBUFFER | D3DCLEAR_STENCIL, 0, 1.0f, 0);
            RealSetDepthStencilSurface(realDevice, oldDepth);
            if (oldDepth)
                oldDepth->Release();
        }

        scene = SceneTexture;
        sceneSurface = SceneLevel;
        output = OutputSurface;
        return true;
    }

    bool IsInPost()
    {
        return bInPost;
    }
}

class RenderScaleModule
{
public:
    RenderScaleModule()
    {
        rage::grcTextureFactoryPC::CreateRTSize = &CreateRTSize;

        FusionFix::onInitEventAsync() += []()
        {
            // globalScreenSize = (w, h, 1/w, 1/h) from the screen size, once a frame
            auto pattern = hook::pattern("8B 4C 24 04 F3 0F 10 05 ? ? ? ? F3 0F 10 09 0F 2E C1 9F F6 C4 44 7A 12 F3 0F 10 05 ? ? ? ? 0F 2E 41 04 9F F6 C4 44 7B ? F3 0F 11 0D");
            if (!pattern.empty())
                shSetGlobalScreenSize = safetyhook::create_inline(pattern.get_first(0), SetGlobalScreenSize);
            Log("globalScreenSize hook %s", shSetGlobalScreenSize ? "installed" : "NOT FOUND");

            // The lights' deferredLightScreenSize = (0, 0, 1/w, 1/h), set right before this call
            pattern = hook::pattern("8D 44 24 10 50 FF 35 ? ? ? ? C7 44 24 18 00 00 00 00 C7 44 24 1C 00 00 00 00 F3 0F 11 44 24 24 E8");
            if (!pattern.empty())
            {
                static auto LightScreenSizeHook = safetyhook::create_mid(pattern.get_first(33), [](SafetyHookContext& regs)
                {
                    if (!bActive || !bInScene)
                        return;
                    auto size = *reinterpret_cast<float**>(regs.esp + 4);
                    if (size[2] > 0.0f && std::abs(1.0f / size[2] - static_cast<float>(DisplayWidth)) < 0.5f)
                        size[2] = 1.0f / static_cast<float>(RenderWidth);
                    if (size[3] > 0.0f && std::abs(1.0f / size[3] - static_cast<float>(DisplayHeight)) < 0.5f)
                        size[3] = 1.0f / static_cast<float>(RenderHeight);
                });
            }
            Log("deferredLightScreenSize hook %s", pattern.empty() ? "NOT FOUND" : "installed");

            FusionFix::onAfterEndScene() += []()
            {
                InstallHooks();
                EndFrame();

                // The menu or the upscaler's availability changed the scale: once for each scale asked for
                if (bScaleDecided)
                {
                    auto desired = DesiredScale();
                    if (desired != fScale && desired != fRequestedScale)
                    {
                        Log("render scale %.3f -> %.3f: resetting the device", fScale, desired);
                        fRequestedScale = desired;
                        DisplayMode::RequestReset();
                    }
                }
            };

            FusionFix::onBeforeReset() += []()
            {
                ReleaseOutput();
            };
        };
    }
} RenderScaleModule;
