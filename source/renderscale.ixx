module;

#include <common.hxx>
#include <algorithm>
#include <cmath>
#include <string_view>

export module renderscale;

import common;
import comvars;

// Render scale: the scene renders at a fraction of the screen size and DLSS or FSR upscales it.
//
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

namespace
{
    float fScale = 1.0f;
    bool bActive = false;            // the scene's targets were created at the render size
    bool bInScene = false;           // from the G-buffer pass of the main scene to the post processing
    bool bInPost = false;            // FullScreenCopy is the full size texture, until the frame ends
    uint32_t DisplayWidth = 0;
    uint32_t DisplayHeight = 0;
    uint32_t RenderWidth = 0;
    uint32_t RenderHeight = 0;

    // FullScreenCopy at the screen size, and the targets FullScreenCopy had at the render size
    IDirect3DTexture9* OutputTexture = nullptr;
    IDirect3DSurface9* OutputSurface = nullptr;
    rage::grcRenderTargetPC* SwappedRT = nullptr;
    IDirect3DTexture9* SceneTexture = nullptr;
    IDirect3DSurface9* SceneSurface = nullptr;

    // Depth buffer of the screen size for targets of the screen size, and the scene's one it stands in for
    IDirect3DSurface9* FullDepth = nullptr;
    IDirect3DSurface9* SceneDepth = nullptr;

    constexpr std::string_view ScaledTargets[] =
    {
        "_DEFERRED_GBUFFER_0_", "_DEFERRED_GBUFFER_1_", "_DEFERRED_GBUFFER_2_", "_DEFERRED_GBUFFER_3_", "_DEFERRED_GBUFFER_1_COPY",
        "_STENCIL_BUFFER_", "_BACK_ZBUFFER_", "FullScreenCopy",
    };

    uint32_t Scaled(uint32_t size)
    {
        return std::max(1u, static_cast<uint32_t>(std::lround(static_cast<double>(size) * fScale)));
    }

    void CreateRTSize(const char* name, uint32_t& width, uint32_t& height)
    {
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

    // Size of render target 0
    bool TargetSize(IDirect3DDevice9* device, uint32_t& width, uint32_t& height)
    {
        IDirect3DSurface9* rt = nullptr;
        if (FAILED(device->GetRenderTarget(0, &rt)) || !rt)
            return false;
        bool ok = SurfaceSize(rt, width, height);
        rt->Release();
        return ok;
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

    // Pairs the scene's depth buffer with targets of its size, and the full size one with the others
    void MatchDepth(IDirect3DDevice9* device)
    {
        uint32_t rtWidth = 0, rtHeight = 0;
        if (!TargetSize(device, rtWidth, rtHeight))
            return;
        IDirect3DSurface9* ds = nullptr;
        if (FAILED(device->GetDepthStencilSurface(&ds)) || !ds)
            return;

        uint32_t dsWidth = 0, dsHeight = 0;
        SurfaceSize(ds, dsWidth, dsHeight);
        if (ds != FullDepth && IsRenderSize(dsWidth, dsHeight) && (rtWidth > dsWidth || rtHeight > dsHeight))
        {
            if (!FullDepth)
            {
                D3DSURFACE_DESC desc{};
                ds->GetDesc(&desc);
                // INTZ is a texture format, a plain depth surface takes D24S8
                device->CreateDepthStencilSurface(DisplayWidth, DisplayHeight, D3DFMT_D24S8, D3DMULTISAMPLE_NONE, 0, FALSE, &FullDepth, nullptr);
            }
            if (FullDepth && rtWidth <= DisplayWidth && rtHeight <= DisplayHeight)
            {
                SceneDepth = ds;
                RealSetDepthStencilSurface(device, FullDepth);
            }
        }
        else if (ds == FullDepth && IsRenderSize(rtWidth, rtHeight) && SceneDepth)
        {
            RealSetDepthStencilSurface(device, SceneDepth);
        }
        ds->Release();
    }

    HRESULT __stdcall SetRenderTarget(IDirect3DDevice9* device, DWORD index, IDirect3DSurface9* surface)
    {
        auto hr = RealSetRenderTarget(device, index, surface);
        if (bActive && index == 0 && surface)
            MatchDepth(device);
        return hr;
    }

    HRESULT __stdcall SetDepthStencilSurface(IDirect3DDevice9* device, IDirect3DSurface9* surface)
    {
        auto hr = RealSetDepthStencilSurface(device, surface);
        if (bActive && surface)
            MatchDepth(device);
        return hr;
    }

    // The game clears _BACK_ZBUFFER_ with the back buffer bound, where the full size depth buffer stands in for it:
    // both are cleared
    HRESULT __stdcall Clear(IDirect3DDevice9* device, DWORD count, const D3DRECT* rects, DWORD flags, D3DCOLOR color, float z, DWORD stencil)
    {
        auto hr = RealClear(device, count, rects, flags, color, z, stencil);
        auto depthFlags = flags & (D3DCLEAR_ZBUFFER | D3DCLEAR_STENCIL);
        if (bActive && depthFlags && count == 0 && FullDepth && SceneDepth)
        {
            IDirect3DSurface9* ds = nullptr;
            if (SUCCEEDED(device->GetDepthStencilSurface(&ds)) && ds)
            {
                if (ds == FullDepth)
                {
                    RealSetDepthStencilSurface(device, SceneDepth);
                    RealClear(device, 0, nullptr, depthFlags, 0, z, stencil);
                    RealSetDepthStencilSurface(device, FullDepth);
                }
                ds->Release();
            }
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

    void ReleaseOutput()
    {
        if (SwappedRT)
        {
            SwappedRT->mD3DTexture = SceneTexture;
            SwappedRT->mD3DSurface = SceneSurface;
            SwappedRT->mWidth = static_cast<uint16_t>(RenderWidth);
            SwappedRT->mHeight = static_cast<uint16_t>(RenderHeight);
            SwappedRT = nullptr;
        }
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
        bInPost = false;
        bInScene = false;
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
        if (SwappedRT)
        {
            SwappedRT->mD3DTexture = SceneTexture;
            SwappedRT->mD3DSurface = SceneSurface;
            SwappedRT->mWidth = static_cast<uint16_t>(RenderWidth);
            SwappedRT->mHeight = static_cast<uint16_t>(RenderHeight);
            SwappedRT = nullptr;
        }
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

    // A size of the screen as the scene renders it
    uint32_t ToRenderWidth(uint32_t width) { return bActive && width == DisplayWidth ? RenderWidth : width; }
    uint32_t ToRenderHeight(uint32_t height) { return bActive && height == DisplayHeight ? RenderHeight : height; }

    // Render thread, at the start of post processing: FullScreenCopy becomes a texture of the screen size
    // for the rest of the frame. Returns the scene at the render size; the caller fills the returned
    // full size surface from it.
    bool BeginPost(IDirect3DDevice9* device, IDirect3DTexture9*& scene, IDirect3DSurface9*& sceneSurface, IDirect3DSurface9*& output)
    {
        if (!bActive || bInPost)
            return false;
        auto rt = rage::grcTextureFactoryPC::GetRTByName("FullScreenCopy");
        if (!rt || !rt->mD3DTexture || !rt->mD3DSurface)
            return false;

        if (!OutputTexture)
        {
            D3DSURFACE_DESC desc{};
            rt->mD3DTexture->GetLevelDesc(0, &desc);
            if (FAILED(device->CreateTexture(DisplayWidth, DisplayHeight, 1, D3DUSAGE_RENDERTARGET, desc.Format, D3DPOOL_DEFAULT, &OutputTexture, nullptr)) || !OutputTexture)
                return false;
            OutputTexture->GetSurfaceLevel(0, &OutputSurface);
            if (!OutputSurface)
                return false;
        }

        SceneTexture = rt->mD3DTexture;
        SceneSurface = rt->mD3DSurface;
        rt->mD3DTexture = OutputTexture;
        rt->mD3DSurface = OutputSurface;
        rt->mWidth = static_cast<uint16_t>(DisplayWidth);
        rt->mHeight = static_cast<uint16_t>(DisplayHeight);
        SwappedRT = rt;
        bInPost = true;
        bInScene = false;
        ApplyGlobalScreenSize(false);

        // The full size depth buffer only exists once the device hooks made it, and starts the post processing empty
        if (FullDepth && RealSetDepthStencilSurface && RealClear)
        {
            IDirect3DSurface9* oldDepth = nullptr;
            device->GetDepthStencilSurface(&oldDepth);
            RealSetDepthStencilSurface(device, FullDepth);
            RealClear(device, 0, nullptr, D3DCLEAR_ZBUFFER | D3DCLEAR_STENCIL, 0, 1.0f, 0);
            RealSetDepthStencilSurface(device, oldDepth);
            if (oldDepth)
                oldDepth->Release();
        }

        scene = SceneTexture;
        sceneSurface = SceneSurface;
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
            CIniReader iniReader("");
            fScale = std::clamp(iniReader.ReadFloat("TEMPORAL", "RenderScale", 1.0f), 0.33f, 1.0f);
            if (fScale >= 0.999f)
            {
                fScale = 1.0f;
                return;
            }

            // globalScreenSize = (w, h, 1/w, 1/h) from the screen size, once a frame
            auto pattern = hook::pattern("8B 4C 24 04 F3 0F 10 05 ? ? ? ? F3 0F 10 09 0F 2E C1 9F F6 C4 44 7A 12 F3 0F 10 05 ? ? ? ? 0F 2E 41 04 9F F6 C4 44 7B ? F3 0F 11 0D");
            if (!pattern.empty())
                shSetGlobalScreenSize = safetyhook::create_inline(pattern.get_first(0), SetGlobalScreenSize);

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

            FusionFix::onAfterEndScene() += []()
            {
                InstallHooks();
                EndFrame();
            };

            FusionFix::onBeforeReset() += []()
            {
                ReleaseOutput();
            };
        };
    }
} RenderScaleModule;
