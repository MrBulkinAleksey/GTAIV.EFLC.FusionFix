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

    // A point on a surface as a share of its width and height (0 to 1)
    struct Point { float x = 0.5f, y = 0.5f; };

    // The surface as a picture named fileName next to the logs (half size, 24-bit BMP; 8 and 10 bit colour as it is,
    // float formats through x / (1 + x) and gamma 2.2, one or two channels as grey or red-green); says its size and
    // format, its brightest pixel, how many are over 1, its mean, the file, and the raw values (all four channels, as
    // the format holds them) at each of the points. An empty fileName reads the points only.
    std::string SaveSurface(IDirect3DDevice9* real, IDirect3DSurface9* surface, const std::string& fileName, const std::vector<Point>& points = {});
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

    // One pixel's four channels as floats; false for a format not read
    static bool Decode(D3DFORMAT format, const uint8_t* p, float (&out)[4])
    {
        auto u32 = [&](size_t at) { uint32_t v; std::memcpy(&v, p + at, 4); return v; };
        auto f32 = [&](size_t at) { float v; std::memcpy(&v, p + at, 4); return v; };
        auto f16 = [&](size_t at) { uint16_t v; std::memcpy(&v, p + at, 2); return HalfToFloat(v); };
        auto u16 = [&](size_t at) { uint16_t v; std::memcpy(&v, p + at, 2); return v / 65535.0f; };
        out[0] = out[1] = out[2] = 0.0f, out[3] = 1.0f;
        switch (format)
        {
        case D3DFMT_A8R8G8B8: case D3DFMT_X8R8G8B8:
            out[0] = p[2] / 255.0f, out[1] = p[1] / 255.0f, out[2] = p[0] / 255.0f, out[3] = format == D3DFMT_A8R8G8B8 ? p[3] / 255.0f : 1.0f;
            return true;
        case D3DFMT_A2R10G10B10: case D3DFMT_A2B10G10R10:
        {
            const uint32_t v = u32(0);
            const float lo = (v & 0x3FF) / 1023.0f, mid = ((v >> 10) & 0x3FF) / 1023.0f, hi = ((v >> 20) & 0x3FF) / 1023.0f;
            out[1] = mid, out[3] = (v >> 30) / 3.0f;
            if (format == D3DFMT_A2R10G10B10)
                out[0] = hi, out[2] = lo;
            else
                out[0] = lo, out[2] = hi;
            return true;
        }
        case D3DFMT_A16B16G16R16F:
            out[0] = f16(0), out[1] = f16(2), out[2] = f16(4), out[3] = f16(6);
            return true;
        case D3DFMT_A16B16G16R16:
            out[0] = u16(0), out[1] = u16(2), out[2] = u16(4), out[3] = u16(6);
            return true;
        case D3DFMT_G16R16F:
            out[0] = f16(0), out[1] = f16(2);
            return true;
        case D3DFMT_G16R16:
            out[0] = u16(0), out[1] = u16(2);
            return true;
        case D3DFMT_R16F:
            out[0] = f16(0);
            return true;
        case D3DFMT_R32F:
            out[0] = f32(0);
            return true;
        case D3DFMT_G32R32F:
            out[0] = f32(0), out[1] = f32(4);
            return true;
        case D3DFMT_A32B32G32R32F:
            out[0] = f32(0), out[1] = f32(4), out[2] = f32(8), out[3] = f32(12);
            return true;
        case D3DFMT_L8:
            out[0] = out[1] = out[2] = p[0] / 255.0f;
            return true;
        default:
            return false;
        }
    }

    static UINT BytesPerPixel(D3DFORMAT format)
    {
        switch (format)
        {
        case D3DFMT_L8: return 1;
        case D3DFMT_R16F: return 2;
        case D3DFMT_A16B16G16R16F: case D3DFMT_A16B16G16R16: case D3DFMT_G32R32F: return 8;
        case D3DFMT_A32B32G32R32F: return 16;
        default: return 4;
        }
    }

    std::string SaveSurface(IDirect3DDevice9* real, IDirect3DSurface9* surface, const std::string& fileName, const std::vector<Point>& points)
    {
        D3DSURFACE_DESC desc{};
        surface->GetDesc(&desc);
        std::string where = std::format("{}x{} format {} samples {}", desc.Width, desc.Height, int(desc.Format), int(desc.MultiSampleType));
        const uint8_t zeros[16]{};
        float probe[4];
        if (!Decode(desc.Format, zeros, probe))
            return where + ", not read (format)";
        const bool byteColour = desc.Format == D3DFMT_A8R8G8B8 || desc.Format == D3DFMT_X8R8G8B8 || desc.Format == D3DFMT_A2R10G10B10 ||
                                desc.Format == D3DFMT_A2B10G10R10 || desc.Format == D3DFMT_A16B16G16R16 || desc.Format == D3DFMT_L8;
        const int channels = (desc.Format == D3DFMT_R16F || desc.Format == D3DFMT_R32F) ? 1 :
                             (desc.Format == D3DFMT_G16R16F || desc.Format == D3DFMT_G16R16 || desc.Format == D3DFMT_G32R32F) ? 2 : 3;
        const UINT bytesPerPixel = BytesPerPixel(desc.Format);

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
                return where + ", not read (resolve)";
            }
            source = resolved;
        }
        std::string result = where + ", not read";
        IDirect3DSurface9* copy = nullptr;
        if (SUCCEEDED(real->CreateOffscreenPlainSurface(desc.Width, desc.Height, desc.Format, D3DPOOL_SYSTEMMEM, &copy, nullptr)) && copy)
        {
            D3DLOCKED_RECT locked{};
            if (SUCCEEDED(real->GetRenderTargetData(source, copy)) && SUCCEEDED(copy->LockRect(&locked, nullptr, D3DLOCK_READONLY)))
            {
                auto pixelAt = [&](UINT x, UINT y, float (&v)[4])
                {
                    Decode(desc.Format, static_cast<const uint8_t*>(locked.pBits) + size_t(locked.Pitch) * y + size_t(x) * bytesPerPixel, v);
                };
                result = where;
                if (!fileName.empty())
                {
                    const UINT w = desc.Width / 2, h = desc.Height / 2, row = (w * 3 + 3) & ~3u;
                    std::vector<uint8_t> pixels(size_t(row) * h);
                    auto toByte = [&](float v)
                    {
                        if (!(v > 0.0f))
                            return uint8_t(0);
                        if (byteColour)
                            return uint8_t(std::min(255.0f, v * 255.0f + 0.5f));
                        v = std::pow(v / (1.0f + v), 1.0f / 2.2f);
                        return uint8_t(std::min(255.0f, v * 255.0f + 0.5f));
                    };
                    float brightest = -1e30f;
                    UINT brightestX = 0, brightestY = 0, overOne = 0;
                    double sum = 0.0;
                    for (UINT y = 0; y < desc.Height; ++y)
                        for (UINT x = 0; x < desc.Width; ++x)
                        {
                            float v[4];
                            pixelAt(x, y, v);
                            const float value = channels == 1 ? v[0] : channels == 2 ? std::max(v[0], v[1]) : std::max({ v[0], v[1], v[2] });
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
                                const float r = v[0], g = channels == 1 ? v[0] : v[1], b = channels == 1 ? v[0] : channels == 2 ? 0.0f : v[2];
                                out[0] = toByte(b), out[1] = toByte(g), out[2] = toByte(r);
                            }
                        }
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
                        result += std::format(", brightest {:.4g} at {},{}, {} pixels over 1, mean {:.4g}: {}", brightest, brightestX, brightestY, overOne,
                            sum / (double(desc.Width) * desc.Height), fileName);
                    }
                    else
                        result += ", picture not written";
                }
                for (const auto& point : points)
                {
                    const UINT x = std::min(desc.Width - 1, UINT(std::clamp(point.x, 0.0f, 1.0f) * desc.Width));
                    const UINT y = std::min(desc.Height - 1, UINT(std::clamp(point.y, 0.0f, 1.0f) * desc.Height));
                    float v[4];
                    pixelAt(x, y, v);
                    result += std::format(" | at {:.3f},{:.3f} ({},{}) {:.6g} {:.6g} {:.6g} {:.6g}", point.x, point.y, x, y, v[0], v[1], v[2], v[3]);
                }
                copy->UnlockRect();
            }
            copy->Release();
        }
        if (resolved)
            resolved->Release();
        return result;
    }
}
