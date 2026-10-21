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

export module watercoronacapture;

import common;
import comvars;
import coronas;
import renderskip;

// Water Corona Capture (hidden [PROJECT2DFX] WaterCoronaCapture, 1 for now): Ctrl+Shift+F5 takes the water reflection's
// next corona pass (the dim one, its eye under the water) to GTAIV.EFLC.FusionFix.WaterCoronaCapture.log: its target,
// depth, viewport and clip plane, every draw's states, pictures of the target before and after the coronas; then the
// first draw that reads that target as a texture (the water), its shaders, sampler and constants, pictures of what it
// reads and of its own target before and after it. The game draws the coronas in the water's reflection with the
// brightness of the main view's (CoronaPassLog), and none shows on the water.
namespace WaterCoronaCapture
{
    enum class Stage { Idle, Armed, InCoronas, WaitWater, WaterNext };
    static Stage stage = Stage::Idle;          // render thread
    static bool keyWasDown = false;
    static std::string text;
    static uint32_t captureNumber = 0, pictures = 0, coronaDraws = 0, waitedDraws = 0;
    static IDirect3DTexture9* reflection = nullptr; // the corona pass's target as a texture, held while waiting
    static const char* reflectionName = nullptr;

    // A shader's FusionShaders signature (a def of "FusionShaders" and its number), its size with the top bit for one without
    static uint32_t ShaderNumber(IUnknown* shader, bool pixel)
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

    // The surface as a picture (half size, 24-bit BMP, x / (1 + x) and gamma 2.2 for HDR colour) next to the log, with
    // its brightest pixel, how many are over 1 and its mean
    static std::string SaveSurface(IDirect3DDevice9* real, IDirect3DSurface9* surface, const char* pointName)
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
                            if (bytes)
                                out[0] = uint8_t(b * 255.0f + 0.5f), out[1] = uint8_t(g * 255.0f + 0.5f), out[2] = uint8_t(r * 255.0f + 0.5f);
                            else
                                out[0] = toByte(b), out[1] = toByte(g), out[2] = toByte(r);
                        }
                    }
                }
                copy->UnlockRect();
                const auto name = std::format("GTAIV.EFLC.FusionFix.WaterCoronaCapture.{}.{}-{}.bmp", captureNumber, pictures++, pointName);
                const auto path = FusionLog::Directory() / std::wstring(name.begin(), name.end());
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
                        sum / (double(desc.Width) * desc.Height), name);
                }
            }
            copy->Release();
        }
        if (resolved)
            resolved->Release();
        return result;
    }

    static IDirect3DDevice9* Real()
    {
        auto device = rage::grcDevice::GetD3DDevice();
        return device ? RageDirect3DDevice9::RealDevice(device) : nullptr;
    }

    static void Picture(IDirect3DDevice9* real, IDirect3DSurface9* surface, const char* pointName)
    {
        text += std::format("----- picture {} {}: {}\n", pictures, pointName, SaveSurface(real, surface, pointName));
    }

    static void PictureOfTarget(IDirect3DDevice9* real, const char* pointName)
    {
        IDirect3DSurface9* target = nullptr;
        if (SUCCEEDED(real->GetRenderTarget(0, &target)) && target)
        {
            Picture(real, target, pointName);
            target->Release();
        }
    }

    static std::string States(IDirect3DDevice9* real)
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

    static void Finish()
    {
        if (reflection)
        {
            reflection->Release();
            reflection = nullptr;
        }
        FusionLog::WriteText("WaterCoronaCapture", "Pass", text);
        text.clear();
        text.shrink_to_fit();
        stage = Stage::Idle;
    }

    static void PassHook(bool isReflection, bool dim, bool after)
    {
        if (!dim || isReflection)
            return;
        auto real = Real();
        if (!real)
            return;
        if (!after)
        {
            const bool down = (GetAsyncKeyState(VK_CONTROL) & 0x8000) && (GetAsyncKeyState(VK_SHIFT) & 0x8000) && (GetAsyncKeyState(VK_F5) & 0x8000);
            if (down && !keyWasDown && stage == Stage::Idle)
                stage = Stage::Armed;
            keyWasDown = down;
            if (stage != Stage::Armed)
                return;
            ++captureNumber;
            pictures = coronaDraws = waitedDraws = 0;
            reflectionName = RenderSkip::CurrentTarget();
            text = std::format("capture {}: the dim corona pass, target {}\n", captureNumber, reflectionName ? reflectionName : "?");
            IDirect3DSurface9* depth = nullptr;
            if (SUCCEEDED(real->GetDepthStencilSurface(&depth)) && depth)
            {
                D3DSURFACE_DESC d{};
                depth->GetDesc(&d);
                text += std::format("      depth {}x{} format {} samples {}\n", d.Width, d.Height, int(d.Format), int(d.MultiSampleType));
                depth->Release();
            }
            else
                text += "      no depth surface\n";
            float plane[4]{};
            real->GetClipPlane(0, plane);
            text += std::format("      clip plane 0 {:.3f} {:.3f} {:.3f} {:.3f}\n", plane[0], plane[1], plane[2], plane[3]);
            for (UINT reg : { 15u, 235u, 236u })
            {
                float c[4]{};
                real->GetVertexShaderConstantF(reg, c, 1);
                text += std::format("      vs c{} {:.4f} {:.4f} {:.4f} {:.4f}\n", reg, c[0], c[1], c[2], c[3]);
            }
            PictureOfTarget(real, "before-coronas");
            stage = Stage::InCoronas;
            return;
        }
        if (stage != Stage::InCoronas)
            return;
        PictureOfTarget(real, "after-coronas");
        IDirect3DSurface9* target = nullptr;
        if (SUCCEEDED(real->GetRenderTarget(0, &target)) && target)
        {
            if (FAILED(target->GetContainer(__uuidof(IDirect3DTexture9), reinterpret_cast<void**>(&reflection))))
                reflection = nullptr;
            target->Release();
        }
        if (!reflection)
        {
            text += "the target is no texture's: nothing reads it directly (resolved elsewhere?)\n";
            Finish();
            return;
        }
        text += std::format("the target is texture {}, waiting for a draw that reads it\n", static_cast<const void*>(reflection));
        stage = Stage::WaitWater;
    }

    static void DrawObserver(IDirect3DDevice9* device, D3DPRIMITIVETYPE type, UINT primitives)
    {
        if (stage == Stage::Idle || stage == Stage::Armed)
            return;
        auto real = RageDirect3DDevice9::RealDevice(device);
        if (stage == Stage::InCoronas)
        {
            if (coronaDraws++ < 8)
                text += std::format("corona draw {}: prim {} x{} | {}\n", coronaDraws, int(type), primitives, States(real));
            return;
        }
        if (stage == Stage::WaterNext)
        {
            PictureOfTarget(real, "after-water");
            Finish();
            return;
        }
        // Stage::WaitWater
        if (++waitedDraws > 30000)
        {
            text += "no draw read the reflection in 30000 draws\n";
            Finish();
            return;
        }
        for (DWORD s = 0; s < 16; ++s)
        {
            IDirect3DBaseTexture9* texture = nullptr;
            real->GetTexture(s, &texture);
            const bool match = texture && texture == reflection;
            if (texture)
                texture->Release();
            if (!match)
                continue;
            const char* target = RenderSkip::CurrentTarget();
            text += std::format("draw {} after the coronas reads it on s{}: target {}, prim {} x{} | {}\n", waitedDraws, s, target ? target : "?",
                int(type), primitives, States(real));
            DWORD filter = 0, address = 0, srgb = 0;
            real->GetSamplerState(s, D3DSAMP_MAGFILTER, &filter);
            real->GetSamplerState(s, D3DSAMP_ADDRESSU, &address);
            real->GetSamplerState(s, D3DSAMP_SRGBTEXTURE, &srgb);
            text += std::format("      sampler filter {} address {} sRGB {}\n", filter, address, srgb);
            float c[96 * 4]{};
            real->GetPixelShaderConstantF(0, c, 96);
            for (UINT reg = 0; reg < 96; ++reg)
            {
                const float* v = c + reg * 4;
                if (v[0] != 0.0f || v[1] != 0.0f || v[2] != 0.0f || v[3] != 0.0f)
                    text += std::format("      ps c{} {:.4f} {:.4f} {:.4f} {:.4f}\n", reg, v[0], v[1], v[2], v[3]);
            }
            IDirect3DSurface9* level = nullptr;
            if (SUCCEEDED(reflection->GetSurfaceLevel(0, &level)) && level)
            {
                Picture(real, level, "read-by-water");
                level->Release();
            }
            PictureOfTarget(real, "before-water");
            stage = Stage::WaterNext;
            return;
        }
    }
}

class WaterCoronaCaptureInit
{
public:
    WaterCoronaCaptureInit()
    {
        FusionFix::onInitEvent() += []()
        {
            CIniReader iniReader("");
            if (iniReader.ReadInteger("PROJECT2DFX", "WaterCoronaCapture", 1) == 0)
                return;
            CoronaRender::pPassHook = &WaterCoronaCapture::PassHook;
            RenderSkip::pDrawObserver = &WaterCoronaCapture::DrawObserver;
        };
    }
} WaterCoronaCaptureInit;
