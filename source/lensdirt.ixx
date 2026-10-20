module;

#include <common.hxx>
#include <algorithm>
#include <cmath>
#include <random>
#include <vector>

export module lensdirt;

import common;
import comvars;
import settings;
import inireload;

using RageDirect3DDevice9::RealDevice;
using RageDirect3DDevice9::SetTextureBoth;
using RageDirect3DDevice9::SetSamplerStateBoth;

// Lens Dirt (Graphics > Lighting): bright lights light up dirt on the lens, and draw a faint horizontal streak. The
// game's composite post fx pass (shaders/patches/world_lens_dirt.patch) adds the bloom times a dirt texture and a
// horizontal streak of the bloom, at the frame's exposure. The dirt is made here once: soft smudges of many sizes and
// a few sharp specks, faintly coloured, on s11. c180 = dirt strength, streak strength, the streak's step (texture
// units), -; set with the texture right before the pass.
// Tuning: hidden keys of [POSTFX] (LensDirt*), read again on Ctrl+Shift+F10.
class LensDirt
{
    static constexpr UINT kSize = 256;
    static constexpr DWORD kSampler = 11;

    static inline float fDirt = 0.6f;
    static inline float fStreak = 0.15f;
    static inline float fStreakStep = 0.012f;
    static inline IDirect3DTexture9* texture = nullptr;
    static inline bool bTried = false;

    static void ReadIni(CIniReader& iniReader)
    {
        fDirt = std::clamp(iniReader.ReadFloat("POSTFX", "LensDirtStrength", 0.6f), 0.0f, 10.0f);
        fStreak = std::clamp(iniReader.ReadFloat("POSTFX", "LensDirtStreak", 0.15f), 0.0f, 10.0f);
        fStreakStep = std::clamp(iniReader.ReadFloat("POSTFX", "LensDirtStreakStep", 0.012f), 0.0f, 0.2f);
    }

    static bool Enabled()
    {
        static auto pref = FusionFixSettings.GetRef("PREF_LENS_DIRT");
        return pref ? pref->get() != 0 : true;
    }

    static bool Debug()
    {
        static auto pref = FusionFixSettings.GetRef("PREF_LENS_DIRT_DEBUG");
        return pref && pref->get() != 0;
    }

    // Soft smudges (gaussian, 2 to 40 texels), a few sharp specks and a faint base, each faintly tinted; managed, so it
    // outlives a device reset.
    static bool MakeTexture(IDirect3DDevice9* pDevice)
    {
        if (bTried)
            return texture != nullptr;
        bTried = true;
        std::vector<float> rgb(kSize * kSize * 3, 0.08f);
        std::mt19937 rng(20261021u);
        std::uniform_real_distribution<float> u(0.0f, 1.0f);
        const auto splat = [&](float cx, float cy, float radius, float strength, float sharp)
        {
            const float tint[3] = { 0.9f + 0.2f * u(rng), 0.9f + 0.2f * u(rng), 0.9f + 0.2f * u(rng) };
            const int r = int(std::ceil(radius * 3.0f));
            for (int y = int(cy) - r; y <= int(cy) + r; ++y)
                for (int x = int(cx) - r; x <= int(cx) + r; ++x)
                {
                    const float dx = x - cx, dy = y - cy;
                    const float d = std::sqrt(dx * dx + dy * dy) / radius;
                    const float w = sharp > 0.0f ? std::clamp((1.0f - d) * sharp, 0.0f, 1.0f) : std::exp(-d * d);
                    if (w <= 0.001f)
                        continue;
                    const int px = (x % int(kSize) + kSize) % kSize, py = (y % int(kSize) + kSize) % kSize;
                    for (int c = 0; c < 3; ++c)
                        rgb[(py * kSize + px) * 3 + c] += w * strength * tint[c];
                }
        };
        for (int i = 0; i < 60; ++i)
            splat(u(rng) * kSize, u(rng) * kSize, 8.0f + 32.0f * u(rng) * u(rng), 0.15f + 0.25f * u(rng), 0.0f);
        for (int i = 0; i < 200; ++i)
            splat(u(rng) * kSize, u(rng) * kSize, 1.5f + 4.0f * u(rng), 0.2f + 0.5f * u(rng), 0.0f);
        for (int i = 0; i < 40; ++i)
            splat(u(rng) * kSize, u(rng) * kSize, 1.0f + 2.5f * u(rng), 0.4f + 0.6f * u(rng), 3.0f);

        if (FAILED(RealDevice(pDevice)->CreateTexture(kSize, kSize, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &texture, nullptr)))
        {
            texture = nullptr;
            return false;
        }
        D3DLOCKED_RECT locked{};
        if (FAILED(texture->LockRect(0, &locked, nullptr, 0)))
        {
            texture->Release();
            texture = nullptr;
            return false;
        }
        for (UINT y = 0; y < kSize; ++y)
        {
            auto row = reinterpret_cast<uint8_t*>(locked.pBits) + y * locked.Pitch;
            for (UINT x = 0; x < kSize; ++x)
            {
                const float* p = &rgb[(y * kSize + x) * 3];
                const auto unorm = [](float v) { return uint8_t(std::clamp(v, 0.0f, 1.0f) * 255.0f + 0.5f); };
                row[x * 4 + 0] = unorm(p[2]);
                row[x * 4 + 1] = unorm(p[1]);
                row[x * 4 + 2] = unorm(p[0]);
                row[x * 4 + 3] = 255;
            }
        }
        texture->UnlockRect(0);
        return true;
    }

public:
    static void Bind(IDirect3DDevice9* pDevice)
    {
        const bool on = (Enabled() || Debug()) && MakeTexture(pDevice);
        const float scale = Debug() ? 5.0f : 1.0f;
        const float params[4] = { on ? fDirt * scale : 0.0f, on ? fStreak * scale : 0.0f, fStreakStep, 0.0f };
        pDevice->SetPixelShaderConstantF(180, params, 1);
        if (auto real = RealDevice(pDevice); real != pDevice)
            real->SetPixelShaderConstantF(180, params, 1);
        if (!texture)
            return;
        SetTextureBoth(pDevice, kSampler, texture);
        for (auto [type, value] : { std::pair{ D3DSAMP_ADDRESSU, DWORD(D3DTADDRESS_CLAMP) }, std::pair{ D3DSAMP_ADDRESSV, DWORD(D3DTADDRESS_CLAMP) },
                 std::pair{ D3DSAMP_MINFILTER, DWORD(D3DTEXF_LINEAR) }, std::pair{ D3DSAMP_MAGFILTER, DWORD(D3DTEXF_LINEAR) },
                 std::pair{ D3DSAMP_MIPFILTER, DWORD(D3DTEXF_NONE) }, std::pair{ D3DSAMP_SRGBTEXTURE, DWORD(FALSE) } })
            SetSamplerStateBoth(pDevice, kSampler, type, value);
    }

    LensDirt()
    {
        FusionFix::onInitEventAsync() += []()
        {
            CIniReader iniReader("");
            ReadIni(iniReader);
            IniReload::Add(ReadIni);
        };
    }
} LensDirt;

// Right before the game's composite post fx pass (postfx.ixx DrawCallPostFX).
export void BindLensDirt(IDirect3DDevice9* pDevice)
{
    LensDirt::Bind(pDevice);
}
