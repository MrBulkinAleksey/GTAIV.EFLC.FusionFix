module;

#include <common.hxx>
#include "FusionLog.hpp"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <format>
#include <string>
#include <unordered_map>
#include <vector>

export module drawcapture;

import common;
import comvars;
import renderskip;

// What the frame captures (Water Reflection Capture, Ped Capture) share: the observer of every device draw RenderSkip
// lets through, a shader's FusionShaders number, a draw's states and a picture of a surface next to the logs.
export namespace DrawCapture
{
    using Observer = void (*)(IDirect3DDevice9* device, D3DPRIMITIVETYPE type, UINT primitives);

    // Called before each device draw that is not dropped, on the render thread; add at init
    void AddObserver(Observer observer);

    IDirect3DDevice9* Real();

    // A shader's FusionShaders signature (a def of "FusionShaders" and its number), its size with the top bit for one without
    uint32_t ShaderNumber(IUnknown* shader, bool pixel);

    // The draw's depth, blend, alpha test, colour write, cull, stencil, clip, scissor, viewport and shaders
    std::string States(IDirect3DDevice9* real);

    // The surface as a picture named fileName (half size, 24-bit BMP, x / (1 + x) and gamma 2.2 for HDR colour) next to
    // the logs; says its size and format, its brightest pixel, how many are over 1, its mean and the file
    std::string SaveSurface(IDirect3DDevice9* real, IDirect3DSurface9* surface, const std::string& fileName);
}

namespace DrawCapture
{
    static std::vector<Observer> observers;

    static void Dispatch(IDirect3DDevice9* device, D3DPRIMITIVETYPE type, UINT primitives)
    {
        for (auto observer : observers)
            observer(device, type, primitives);
    }

    void AddObserver(Observer observer)
    {
        observers.push_back(observer);
        RenderSkip::pDrawObserver = &Dispatch;
    }

    IDirect3DDevice9* Real()
    {
        auto device = rage::grcDevice::GetD3DDevice();
        return device ? RageDirect3DDevice9::RealDevice(device) : nullptr;
    }

    uint32_t ShaderNumber(IUnknown* shader, bool pixel)
    {
        static std::unordered_map<const void*, uint32_t> known;
        if (!shader)
            return 0;
        if (auto it = known.find(shader); it != known.end())
            return it->second;
        UINT size = 0;
        uint32_t number = 0;
        auto getFunction = [&](void* data, UINT* bytes)
        {
            return pixel ? static_cast<IDirect3DPixelShader9*>(shader)->GetFunction(data, bytes)
                         : static_cast<IDirect3DVertexShader9*>(shader)->GetFunction(data, bytes);
        };
        if (SUCCEEDED(getFunction(nullptr, &size)) && size >= 16)
        {
            std::vector<uint32_t> code(size / 4);
            if (SUCCEEDED(getFunction(code.data(), &size)))
                for (size_t i = 0; i + 3 < code.size(); ++i)
                    if (code[i] == 0x69737546 && code[i + 1] == 0x68536E6F && code[i + 2] == 0x72656461)
                    {
                        number = code[i + 3];
                        break;
                    }
            if (!number)
                number = 0x80000000u | size;
        }
        known.emplace(shader, number);
        return number;
    }

    std::string States(IDirect3DDevice9* real)
    {
        DWORD z = 0, zFunc = 0, zWrite = 0, blend = 0, source = 0, destination = 0, alphaTest = 0, colourWrite = 0, cull = 0, stencil = 0, clip = 0, scissor = 0;
        real->GetRenderState(D3DRS_ZENABLE, &z);
        real->GetRenderState(D3DRS_ZFUNC, &zFunc);
        real->GetRenderState(D3DRS_ZWRITEENABLE, &zWrite);
        real->GetRenderState(D3DRS_ALPHABLENDENABLE, &blend);
        real->GetRenderState(D3DRS_SRCBLEND, &source);
        real->GetRenderState(D3DRS_DESTBLEND, &destination);
        real->GetRenderState(D3DRS_ALPHATESTENABLE, &alphaTest);
        real->GetRenderState(D3DRS_COLORWRITEENABLE, &colourWrite);
        real->GetRenderState(D3DRS_CULLMODE, &cull);
        real->GetRenderState(D3DRS_STENCILENABLE, &stencil);
        real->GetRenderState(D3DRS_CLIPPLANEENABLE, &clip);
        real->GetRenderState(D3DRS_SCISSORTESTENABLE, &scissor);
        IDirect3DVertexShader9* vs = nullptr;
        IDirect3DPixelShader9* ps = nullptr;
        real->GetVertexShader(&vs);
        real->GetPixelShader(&ps);
        const uint32_t vsNumber = ShaderNumber(vs, false), psNumber = ShaderNumber(ps, true);
        if (vs) vs->Release();
        if (ps) ps->Release();
        D3DVIEWPORT9 viewport{};
        real->GetViewport(&viewport);
        return std::format("z {} func {} write {} | blend {} {}/{} | atest {} | cw {:X} cull {} stencil {} | clip {:X} scissor {} | viewport {},{} {}x{} | vs {:X} ps {:X}",
            z, zFunc, zWrite, blend, source, destination, alphaTest, colourWrite, cull, stencil, clip, scissor,
            viewport.X, viewport.Y, viewport.Width, viewport.Height, vsNumber, psNumber);
    }

    static float HalfToFloat(uint16_t h)
    {
        const uint32_t sign = uint32_t(h & 0x8000) << 16, exponent = (h >> 10) & 0x1F, mantissa = h & 0x3FF;
        uint32_t bits;
        if (exponent == 0)
        {
            float f = float(mantissa) / 1024.0f / 16384.0f;
            return sign ? -f : f;
        }
        if (exponent == 31)
            bits = sign | 0x7F800000 | (mantissa << 13);
        else
            bits = sign | ((exponent + 112) << 23) | (mantissa << 13);
        float f;
        std::memcpy(&f, &bits, sizeof(f));
        return f;
    }

    std::string SaveSurface(IDirect3DDevice9* real, IDirect3DSurface9* surface, const std::string& fileName)
    {
        D3DSURFACE_DESC desc{};
        surface->GetDesc(&desc);
        const bool half = desc.Format == D3DFMT_A16B16G16R16F;
        const bool bytes = desc.Format == D3DFMT_A8R8G8B8 || desc.Format == D3DFMT_X8R8G8B8;
        const bool tenARGB = desc.Format == D3DFMT_A2R10G10B10, tenABGR = desc.Format == D3DFMT_A2B10G10R10;
        std::string where = std::format("{}x{} format {} samples {}", desc.Width, desc.Height, int(desc.Format), int(desc.MultiSampleType));
        if (!(half || bytes || tenARGB || tenABGR))
            return where + ", not taken (format)";

        // A multisampled one resolved first
        IDirect3DSurface9* source = surface;
        IDirect3DSurface9* resolved = nullptr;
        if (desc.MultiSampleType != D3DMULTISAMPLE_NONE)
        {
            if (FAILED(real->CreateRenderTarget(desc.Width, desc.Height, desc.Format, D3DMULTISAMPLE_NONE, 0, FALSE, &resolved, nullptr)) || !resolved ||
                FAILED(real->StretchRect(surface, nullptr, resolved, nullptr, D3DTEXF_NONE)))
            {
                if (resolved)
                    resolved->Release();
                return where + ", not taken (resolve)";
            }
            source = resolved;
        }
        std::string result = where + ", not taken";
        IDirect3DSurface9* copy = nullptr;
        if (SUCCEEDED(real->CreateOffscreenPlainSurface(desc.Width, desc.Height, desc.Format, D3DPOOL_SYSTEMMEM, &copy, nullptr)) && copy)
        {
            D3DLOCKED_RECT locked{};
            if (SUCCEEDED(real->GetRenderTargetData(source, copy)) && SUCCEEDED(copy->LockRect(&locked, nullptr, D3DLOCK_READONLY)))
            {
                const UINT w = desc.Width / 2, h = desc.Height / 2, row = (w * 3 + 3) & ~3u;
                std::vector<uint8_t> pixels(size_t(row) * h);
                auto toByte = [](float v)
                {
                    if (!(v > 0.0f))
                        return uint8_t(0);
                    v = std::pow(v / (1.0f + v), 1.0f / 2.2f);
                    return uint8_t(std::min(255.0f, v * 255.0f + 0.5f));
                };
                float brightest = 0.0f;
                UINT brightestX = 0, brightestY = 0, overOne = 0;
                double sum = 0.0;
                for (UINT y = 0; y < desc.Height; ++y)
                {
                    const auto* line = static_cast<const uint8_t*>(locked.pBits) + size_t(locked.Pitch) * y;
                    for (UINT x = 0; x < desc.Width; ++x)
                    {
                        float r, g, b;
                        if (half)
                        {
                            const auto* p = reinterpret_cast<const uint16_t*>(line) + size_t(x) * 4;
                            r = HalfToFloat(p[0]), g = HalfToFloat(p[1]), b = HalfToFloat(p[2]);
                        }
                        else if (bytes)
                        {
                            const uint8_t* p = line + size_t(x) * 4;
                            b = p[0] / 255.0f, g = p[1] / 255.0f, r = p[2] / 255.0f;
                        }
                        else
                        {
                            uint32_t v;
                            std::memcpy(&v, line + size_t(x) * 4, 4);
                            float lo = (v & 0x3FF) / 1023.0f, mid = ((v >> 10) & 0x3FF) / 1023.0f, hi = ((v >> 20) & 0x3FF) / 1023.0f;
                            if (tenARGB)
                                b = lo, g = mid, r = hi;
                            else
                                r = lo, g = mid, b = hi;
                        }
                        const float value = std::max({ r, g, b });
                        if (value > brightest)
                        {
                            brightest = value;
                            brightestX = x, brightestY = y;
                        }
                        if (value > 1.0f)
                            ++overOne;
                        sum += value;
                        if ((x & 1) == 0 && (y & 1) == 0 && x / 2 < w && y / 2 < h)
                        {
                            uint8_t* out = pixels.data() + size_t(row) * (h - 1 - y / 2) + size_t(x / 2) * 3;
                            if (bytes || tenARGB || tenABGR)
                                out[0] = uint8_t(b * 255.0f + 0.5f), out[1] = uint8_t(g * 255.0f + 0.5f), out[2] = uint8_t(r * 255.0f + 0.5f);
                            else
                                out[0] = toByte(b), out[1] = toByte(g), out[2] = toByte(r);
                        }
                    }
                }
                copy->UnlockRect();
                const auto path = FusionLog::Directory() / std::wstring(fileName.begin(), fileName.end());
                if (FILE* file = _wfopen(path.c_str(), L"wb"))
                {
                    const uint32_t imageSize = uint32_t(pixels.size()), fileSize = 54 + imageSize;
                    uint8_t header[54]{ 'B', 'M' };
                    auto put32 = [&](size_t at, uint32_t v) { std::memcpy(header + at, &v, 4); };
                    put32(2, fileSize);
                    put32(10, 54);
                    put32(14, 40);
                    put32(18, w);
                    put32(22, h);
                    header[26] = 1;
                    header[28] = 24;
                    put32(34, imageSize);
                    fwrite(header, 1, sizeof(header), file);
                    fwrite(pixels.data(), 1, pixels.size(), file);
                    fclose(file);
                    result = where + std::format(", brightest {:.3f} at {},{}, {} pixels over 1, mean {:.4f}: {}", brightest, brightestX, brightestY, overOne,
                        sum / (double(desc.Width) * desc.Height), fileName);
                }
            }
            copy->Release();
        }
        if (resolved)
            resolved->Release();
        return result;
    }
}
