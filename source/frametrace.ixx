module;

#include <common.hxx>
#include <algorithm>
#include <atomic>
#include <cstdarg>
#include <string>
#include <unordered_map>
#include <vector>

export module frametrace;

import common;
import comvars;

// Temporary diagnostics for the render scale (branch upscaler-render-scale).
//
// Ctrl+Shift+F12 writes the next frame's render target, viewport and draw sequence to
// GTAIV.EFLC.FusionFix.frame.log next to the plugin: every render target and depth target change, viewport,
// clear and StretchRect, consecutive draws with the same target, viewport and shaders as one line with the
// textures they read, and for every shader the constants of its constant table, with the values of those
// that look screen sized at its first draw. Ends with the game's render targets and their sizes.

namespace
{
    constexpr int SetRenderTargetIndex = 37;
    constexpr int SetDepthStencilSurfaceIndex = 39;
    constexpr int ClearIndex = 43;
    constexpr int SetViewportIndex = 47;
    constexpr int StretchRectIndex = 34;
    constexpr int DrawPrimitiveIndex = 81;
    constexpr int DrawIndexedPrimitiveIndex = 82;
    constexpr int DrawPrimitiveUPIndex = 83;
    constexpr int DrawIndexedPrimitiveUPIndex = 84;

    HRESULT(__stdcall* RealSetRenderTarget)(IDirect3DDevice9*, DWORD, IDirect3DSurface9*) = nullptr;
    HRESULT(__stdcall* RealSetDepthStencilSurface)(IDirect3DDevice9*, IDirect3DSurface9*) = nullptr;
    HRESULT(__stdcall* RealClear)(IDirect3DDevice9*, DWORD, const D3DRECT*, DWORD, D3DCOLOR, float, DWORD) = nullptr;
    HRESULT(__stdcall* RealSetViewport)(IDirect3DDevice9*, const D3DVIEWPORT9*) = nullptr;
    HRESULT(__stdcall* RealStretchRect)(IDirect3DDevice9*, IDirect3DSurface9*, const RECT*, IDirect3DSurface9*, const RECT*, D3DTEXTUREFILTERTYPE) = nullptr;
    HRESULT(__stdcall* RealDrawPrimitive)(IDirect3DDevice9*, D3DPRIMITIVETYPE, UINT, UINT) = nullptr;
    HRESULT(__stdcall* RealDrawIndexedPrimitive)(IDirect3DDevice9*, D3DPRIMITIVETYPE, INT, UINT, UINT, UINT, UINT) = nullptr;
    HRESULT(__stdcall* RealDrawPrimitiveUP)(IDirect3DDevice9*, D3DPRIMITIVETYPE, UINT, const void*, UINT) = nullptr;
    HRESULT(__stdcall* RealDrawIndexedPrimitiveUP)(IDirect3DDevice9*, D3DPRIMITIVETYPE, UINT, UINT, UINT, const void*, D3DFORMAT, const void*, UINT) = nullptr;

    void** HookedVTable = nullptr;
    std::atomic<int> TraceState = 0;   // 0 off, 1 armed (starts at the next frame), 2 tracing
    std::vector<std::string> Lines;
    std::unordered_map<void*, uint32_t> ShaderIds;

    // The current run of draws
    struct Segment
    {
        IDirect3DSurface9* rt = nullptr;
        IDirect3DSurface9* ds = nullptr;
        D3DVIEWPORT9 vp{};
        IDirect3DVertexShader9* vs = nullptr;
        IDirect3DPixelShader9* ps = nullptr;
        uint32_t draws = 0;
        uint32_t primitives = 0;
        size_t line = SIZE_MAX;
    } Current;

    std::string Format(const char* format, ...)
    {
        char buffer[1024];
        va_list args;
        va_start(args, format);
        vsnprintf(buffer, sizeof(buffer), format, args);
        va_end(args);
        return buffer;
    }

    const char* FormatName(D3DFORMAT format)
    {
        switch (format)
        {
        case D3DFMT_A8R8G8B8: return "A8R8G8B8";
        case D3DFMT_X8R8G8B8: return "X8R8G8B8";
        case D3DFMT_A2R10G10B10: return "A2R10G10B10";
        case D3DFMT_A2B10G10R10: return "A2B10G10R10";
        case D3DFMT_A16B16G16R16F: return "A16B16G16R16F";
        case D3DFMT_A32B32G32R32F: return "A32B32G32R32F";
        case D3DFMT_G16R16F: return "G16R16F";
        case D3DFMT_G32R32F: return "G32R32F";
        case D3DFMT_R16F: return "R16F";
        case D3DFMT_R32F: return "R32F";
        case D3DFMT_D24S8: return "D24S8";
        case D3DFMT_D24X8: return "D24X8";
        case D3DFMT_D16: return "D16";
        case D3DFMT_D32: return "D32";
        case D3DFMT_D32F_LOCKABLE: return "D32F";
        case D3DFMT_L8: return "L8";
        case D3DFMT_A8: return "A8";
        case D3DFMT_R5G6B5: return "R5G6B5";
        default:
        {
            static thread_local char buffer[16];
            auto f = static_cast<uint32_t>(format);
            if (f > 0xFF)
                snprintf(buffer, sizeof(buffer), "%c%c%c%c", f & 0xFF, (f >> 8) & 0xFF, (f >> 16) & 0xFF, f >> 24);
            else
                snprintf(buffer, sizeof(buffer), "fmt%u", f);
            return buffer;
        }
        }
    }

    // Name of a render target of the game or of FusionFix, by its surface or texture
    std::string Describe(IDirect3DSurface9* surface)
    {
        if (!surface)
            return "null";

        std::string name;
        for (auto& [rtName, rt] : rage::grcTextureFactoryPC::RTCache)
        {
            if (!rt)
                continue;
            if (rt->mD3DSurface == surface)
            {
                name = rtName;
                break;
            }
            if (rt->mD3DTexture)
            {
                IDirect3DSurface9* level = nullptr;
                if (SUCCEEDED(rt->mD3DTexture->GetSurfaceLevel(0, &level)) && level)
                {
                    bool same = level == surface;
                    level->Release();
                    if (same)
                    {
                        name = rtName;
                        break;
                    }
                }
            }
        }

        if (name.empty())
        {
            auto device = *RageDirect3DDevice9::m_pRealDevice;
            IDirect3DSurface9* backBuffer = nullptr;
            if (SUCCEEDED(device->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &backBuffer)) && backBuffer)
            {
                if (backBuffer == surface)
                    name = "BACKBUFFER";
                backBuffer->Release();
            }
        }

        D3DSURFACE_DESC desc{};
        surface->GetDesc(&desc);
        if (name.empty())
            name = Format("surface %p", surface);
        return Format("%s %ux%u %s", name.c_str(), desc.Width, desc.Height, FormatName(desc.Format));
    }

    std::string DescribeTexture(IDirect3DBaseTexture9* texture)
    {
        if (!texture || texture->GetType() != D3DRTYPE_TEXTURE)
            return {};
        auto tex = static_cast<IDirect3DTexture9*>(texture);
        D3DSURFACE_DESC desc{};
        tex->GetLevelDesc(0, &desc);
        if (!(desc.Usage & (D3DUSAGE_RENDERTARGET | D3DUSAGE_DEPTHSTENCIL)))
            return {};
        IDirect3DSurface9* level = nullptr;
        if (FAILED(tex->GetSurfaceLevel(0, &level)) || !level)
            return {};
        auto name = Describe(level);
        level->Release();
        return name;
    }

    // Constant table of a shader: name, register set and index of every constant
    struct Constant
    {
        std::string name;
        uint16_t set = 0;
        uint16_t index = 0;
        uint16_t count = 0;
    };

    std::vector<Constant> ConstantTable(const std::vector<DWORD>& code)
    {
        std::vector<Constant> constants;
        for (size_t i = 1; i < code.size(); )
        {
            auto token = code[i];
            if ((token & 0xFFFF) != 0xFFFE)
                break;
            auto length = token >> 16;
            if (i + 1 + length > code.size())
                break;
            if (length >= 7 && code[i + 1] == MAKEFOURCC('C', 'T', 'A', 'B'))
            {
                auto base = reinterpret_cast<const uint8_t*>(&code[i + 2]);
                auto size = (length - 1) * sizeof(DWORD);
                auto header = reinterpret_cast<const DWORD*>(base);
                auto count = header[3];
                auto infoOffset = header[4];
                for (DWORD c = 0; c < count && infoOffset + (c + 1) * 20 <= size; ++c)
                {
                    auto info = base + infoOffset + c * 20;
                    auto nameOffset = *reinterpret_cast<const DWORD*>(info);
                    Constant constant;
                    constant.set = *reinterpret_cast<const uint16_t*>(info + 4);
                    constant.index = *reinterpret_cast<const uint16_t*>(info + 6);
                    constant.count = *reinterpret_cast<const uint16_t*>(info + 8);
                    if (nameOffset < size)
                        constant.name.assign(reinterpret_cast<const char*>(base + nameOffset), strnlen(reinterpret_cast<const char*>(base + nameOffset), size - nameOffset));
                    constants.push_back(constant);
                }
                break;
            }
            i += 1 + length;
        }
        return constants;
    }

    bool LooksScreenSized(const std::string& name)
    {
        static const char* keys[] = { "Screen", "Texel", "Size", "Viewport", "Proj", "Offset", "Scale", "Window", "Pixel", "Resolution", "Params" };
        for (auto key : keys)
            if (name.find(key) != std::string::npos)
                return true;
        return false;
    }

    template <class Shader>
    uint32_t DescribeShader(IDirect3DDevice9* device, Shader* shader, bool pixel)
    {
        if (!shader)
            return 0;
        auto [it, added] = ShaderIds.try_emplace(shader, static_cast<uint32_t>(ShaderIds.size() + 1));
        if (!added)
            return it->second;

        UINT size = 0;
        shader->GetFunction(nullptr, &size);
        std::vector<DWORD> code(size / sizeof(DWORD));
        if (size)
            shader->GetFunction(code.data(), &size);

        std::string line = Format("    %s%u (%u bytes):", pixel ? "ps" : "vs", it->second, size);
        for (auto& c : ConstantTable(code))
        {
            static const char* sets[] = { "b", "i", "c", "s" };
            line += Format(" %s=%s%u", c.name.c_str(), c.set < 4 ? sets[c.set] : "?", c.index);
            if (c.set == 2 && LooksScreenSized(c.name))
            {
                float values[4]{};
                if (pixel)
                    device->GetPixelShaderConstantF(c.index, values, 1);
                else
                    device->GetVertexShaderConstantF(c.index, values, 1);
                line += Format("(%g %g %g %g)", values[0], values[1], values[2], values[3]);
            }
        }
        Lines.push_back(line);
        return it->second;
    }

    void Release(IDirect3DSurface9*& surface)
    {
        if (surface)
            surface->Release();
        surface = nullptr;
    }

    void EndSegment()
    {
        if (Current.line != SIZE_MAX && Current.line < Lines.size())
            Lines[Current.line] += Format(" x%u draws, %u primitives", Current.draws, Current.primitives);
        Release(Current.rt);
        Release(Current.ds);
        Current = {};
    }

    void Draw(IDirect3DDevice9* device, UINT primitives)
    {
        if (TraceState != 2)
            return;

        Segment segment;
        device->GetRenderTarget(0, &segment.rt);
        device->GetDepthStencilSurface(&segment.ds);
        device->GetViewport(&segment.vp);
        device->GetVertexShader(&segment.vs);
        device->GetPixelShader(&segment.ps);
        // The shaders are kept alive by the device, only their addresses are compared
        if (segment.vs)
            segment.vs->Release();
        if (segment.ps)
            segment.ps->Release();

        bool same = Current.line != SIZE_MAX && segment.rt == Current.rt && segment.ds == Current.ds && segment.vs == Current.vs && segment.ps == Current.ps &&
            !memcmp(&segment.vp, &Current.vp, sizeof(D3DVIEWPORT9));
        if (same)
        {
            Release(segment.rt);
            Release(segment.ds);
            Current.draws++;
            Current.primitives += primitives;
            return;
        }

        EndSegment();
        auto vs = DescribeShader(device, segment.vs, false);
        auto ps = DescribeShader(device, segment.ps, true);

        std::string textures;
        for (DWORD s = 0; s < 16; ++s)
        {
            IDirect3DBaseTexture9* texture = nullptr;
            if (SUCCEEDED(device->GetTexture(s, &texture)) && texture)
            {
                auto name = DescribeTexture(texture);
                if (!name.empty())
                    textures += Format(" s%u=[%s]", s, name.c_str());
                texture->Release();
            }
        }

        Lines.push_back(Format("  draw vs%u ps%u rt=[%s] vp=%u,%u %ux%u%s", vs, ps, Describe(segment.rt).c_str(),
            segment.vp.X, segment.vp.Y, segment.vp.Width, segment.vp.Height, textures.c_str()));
        Current = segment;
        Current.draws = 1;
        Current.primitives = primitives;
        Current.line = Lines.size() - 1;
    }

    HRESULT __stdcall SetRenderTarget(IDirect3DDevice9* device, DWORD index, IDirect3DSurface9* surface)
    {
        if (TraceState == 2)
        {
            EndSegment();
            Lines.push_back(Format("SetRenderTarget %u [%s]", index, Describe(surface).c_str()));
        }
        return RealSetRenderTarget(device, index, surface);
    }

    HRESULT __stdcall SetDepthStencilSurface(IDirect3DDevice9* device, IDirect3DSurface9* surface)
    {
        if (TraceState == 2)
        {
            EndSegment();
            Lines.push_back(Format("SetDepthStencil [%s]", Describe(surface).c_str()));
        }
        return RealSetDepthStencilSurface(device, surface);
    }

    HRESULT __stdcall Clear(IDirect3DDevice9* device, DWORD count, const D3DRECT* rects, DWORD flags, D3DCOLOR color, float z, DWORD stencil)
    {
        if (TraceState == 2)
        {
            EndSegment();
            Lines.push_back(Format("Clear flags %x rects %u color %08x z %g", flags, count, color, z));
        }
        return RealClear(device, count, rects, flags, color, z, stencil);
    }

    HRESULT __stdcall SetViewport(IDirect3DDevice9* device, const D3DVIEWPORT9* vp)
    {
        if (TraceState == 2 && vp)
        {
            D3DVIEWPORT9 old{};
            device->GetViewport(&old);
            if (memcmp(&old, vp, sizeof(old)))
                Lines.push_back(Format("SetViewport %u,%u %ux%u z %g..%g", vp->X, vp->Y, vp->Width, vp->Height, vp->MinZ, vp->MaxZ));
        }
        return RealSetViewport(device, vp);
    }

    HRESULT __stdcall StretchRect(IDirect3DDevice9* device, IDirect3DSurface9* source, const RECT* sourceRect, IDirect3DSurface9* target, const RECT* targetRect, D3DTEXTUREFILTERTYPE filter)
    {
        if (TraceState == 2)
        {
            EndSegment();
            auto rect = [](const RECT* r) { return r ? Format("%d,%d-%d,%d", r->left, r->top, r->right, r->bottom) : std::string("all"); };
            Lines.push_back(Format("StretchRect [%s] %s -> [%s] %s filter %u", Describe(source).c_str(), rect(sourceRect).c_str(),
                Describe(target).c_str(), rect(targetRect).c_str(), filter));
        }
        return RealStretchRect(device, source, sourceRect, target, targetRect, filter);
    }

    HRESULT __stdcall DrawPrimitive(IDirect3DDevice9* device, D3DPRIMITIVETYPE type, UINT start, UINT count)
    {
        Draw(device, count);
        return RealDrawPrimitive(device, type, start, count);
    }

    HRESULT __stdcall DrawIndexedPrimitive(IDirect3DDevice9* device, D3DPRIMITIVETYPE type, INT baseVertex, UINT minIndex, UINT numVertices, UINT startIndex, UINT count)
    {
        Draw(device, count);
        return RealDrawIndexedPrimitive(device, type, baseVertex, minIndex, numVertices, startIndex, count);
    }

    HRESULT __stdcall DrawPrimitiveUP(IDirect3DDevice9* device, D3DPRIMITIVETYPE type, UINT count, const void* data, UINT stride)
    {
        Draw(device, count);
        return RealDrawPrimitiveUP(device, type, count, data, stride);
    }

    HRESULT __stdcall DrawIndexedPrimitiveUP(IDirect3DDevice9* device, D3DPRIMITIVETYPE type, UINT minIndex, UINT numVertices, UINT count, const void* indices, D3DFORMAT indexFormat, const void* data, UINT stride)
    {
        Draw(device, count);
        return RealDrawIndexedPrimitiveUP(device, type, minIndex, numVertices, count, indices, indexFormat, data, stride);
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
        Patch(vtable, StretchRectIndex, RealStretchRect, &StretchRect);
        Patch(vtable, DrawPrimitiveIndex, RealDrawPrimitive, &DrawPrimitive);
        Patch(vtable, DrawIndexedPrimitiveIndex, RealDrawIndexedPrimitive, &DrawIndexedPrimitive);
        Patch(vtable, DrawPrimitiveUPIndex, RealDrawPrimitiveUP, &DrawPrimitiveUP);
        Patch(vtable, DrawIndexedPrimitiveUPIndex, RealDrawIndexedPrimitiveUP, &DrawIndexedPrimitiveUP);
        HookedVTable = vtable;
    }

    void Write()
    {
        std::vector<std::pair<std::string, rage::grcRenderTargetPC*>> targets(rage::grcTextureFactoryPC::RTCache.begin(), rage::grcTextureFactoryPC::RTCache.end());
        std::sort(targets.begin(), targets.end(), [](auto& a, auto& b) { return a.first < b.first; });
        Lines.push_back("");
        Lines.push_back("Render targets:");
        for (auto& [name, rt] : targets)
        {
            if (!rt)
                continue;
            D3DSURFACE_DESC desc{};
            if (rt->mD3DTexture)
                rt->mD3DTexture->GetLevelDesc(0, &desc);
            else if (rt->mD3DSurface)
                rt->mD3DSurface->GetDesc(&desc);
            Lines.push_back(Format("  %s: %ux%u (game %ux%u) %s levels %u", name.c_str(), desc.Width, desc.Height, rt->mWidth, rt->mHeight,
                FormatName(desc.Format), rt->mLevels));
        }

        auto device = *RageDirect3DDevice9::m_pRealDevice;
        IDirect3DSurface9* backBuffer = nullptr;
        if (SUCCEEDED(device->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &backBuffer)) && backBuffer)
        {
            Lines.push_back("  back buffer: " + Describe(backBuffer));
            backBuffer->Release();
        }
        Lines.push_back(Format("  grcDevice active size %dx%d", *rage::grcDevice::ms_nActiveWidth, *rage::grcDevice::ms_nActiveHeight));

        FILE* f = nullptr;
        if (!_wfopen_s(&f, (GetThisModulePath() / L"GTAIV.EFLC.FusionFix.frame.log").c_str(), L"w") && f)
        {
            for (auto& line : Lines)
                fprintf(f, "%s\n", line.c_str());
            fclose(f);
        }
        Lines.clear();
        ShaderIds.clear();
    }
}

class FrameTrace
{
public:
    FrameTrace()
    {
        FusionFix::onInitEventAsync() += []()
        {
            // Frame boundary: the game's EndScene, on the render thread
            FusionFix::onAfterEndScene() += []()
            {
                InstallHooks();

                if (TraceState == 2)
                {
                    EndSegment();
                    TraceState = 0;
                    Write();
                }
                else if (TraceState == 1)
                {
                    Lines.clear();
                    Lines.push_back("GTAIV.EFLC.FusionFix frame trace");
                    TraceState = 2;
                }

                static bool down = false;
                bool pressed = (GetAsyncKeyState(VK_CONTROL) & 0x8000) && (GetAsyncKeyState(VK_SHIFT) & 0x8000) && (GetAsyncKeyState(VK_F12) & 0x8000);
                if (pressed && !down && TraceState == 0)
                    TraceState = 1;
                down = pressed;
            };
        };
    }
} FrameTrace;
