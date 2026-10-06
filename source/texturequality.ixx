module;

#include <common.hxx>
#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <functional>
#include <optional>
#include <vector>

export module texturequality;

import common;
import comvars;
import renderscale;
import settings;

// Texture quality options for the G-buffer pass, each turned on and off on its own in the
// graphics menu ([TEXTURES] in the ini has their tuning):
//  - Texture LOD Bias: a negative mip bias on the material textures, sharper at a distance.
//  - Anisotropic All Maps: the normal and specular maps that the game filters bilinearly get the
//    anisotropic filtering its own menu sets for the diffuse textures.
//  - Detail Textures: a fine grain on surfaces up close (b12, c190, c191, s12).
//  - Bicubic Filtering: magnified diffuse textures read with a Catmull-Rom filter (b13, c192).
//  - Specular Anti-Aliasing: highlights widened where the normal turns within a pixel (b14, c193).
// The shader side is shaders/patches/texture_quality.patch; Sharpening is a post fx pass.
class TextureQuality
{
    // s0-s5 hold the material textures in the G-buffer shaders; s10 is the stipple texture.
    static constexpr DWORD kMaterialStages = 6;
    static constexpr DWORD kDetailStage = 12;
    static constexpr int kDetailSize = 256;
    // IDirect3DDevice9 methods
    static constexpr int kSetTexture = 65;
    static constexpr int kSetSamplerState = 69;

    static inline float fDetailTiling = 8.0f;
    static inline float fDetailAlbedo = 0.15f;
    static inline float fDetailNormal = 0.5f;
    static inline float fSpecularAAStrength = 1.0f;
    static inline float fSpecularAAMax = 0.18f;

    // Made during the asynchronous init, which is over before the G-buffer hooks are added.
    static inline std::vector<std::vector<uint32_t>> detailLevels;
    static inline IDirect3DTexture9* pDetailTex = nullptr;
    static inline bool bDetailTexFailed = false;

    static inline SafetyHookInline shSetTexture{};
    static inline SafetyHookInline shSetSamplerState{};
    static inline bool bDeviceHooksTried = false;

    // Set from the start to the end of the G-buffer pass, on the render thread.
    static inline bool bInGBuffer = false;
    static inline bool bBicubic = false;
    static inline bool bAnisoAllMaps = false;
    static inline bool bLodBiasSet = false;
    static inline DWORD savedLodBias[kMaterialStages] = {};
    // Stages whose last MINFILTER from the game was linear and went to the device as anisotropic.
    static inline bool bRaisedMinFilter[kMaterialStages] = {};
    static inline float lastDiffuseSize[4] = {};

    static int32_t Pref(std::optional<std::reference_wrapper<int32_t>>& ref)
    {
        return ref ? ref->get() : 0;
    }

public:
    // The detail texture's levels, largest first, as A8R8G8B8: white noise blurred at four sizes
    // (which, unlike value noise, shows no grid), its height in blue around a mean of 0.5 and its
    // slopes as a normal in red and green. Each mip is the 2x2 average of the one above, so the
    // grain fades to flat grey, and leaves surfaces alone, as they get further away.
    static std::vector<std::vector<uint32_t>> MakeDetailLevels()
    {
        constexpr int N = kDetailSize;
        std::vector<float> sum(N * N, 0.0f), noise(N * N), blurred(N * N);

        uint32_t state = 0x9E3779B9u;
        auto next = [&state]()
        {
            state ^= state << 13;
            state ^= state >> 17;
            state ^= state << 5;
            return static_cast<float>(state / 4294967296.0);
        };

        auto normalize = [](std::vector<float>& v)
        {
            double mean = 0.0, var = 0.0;
            for (auto x : v)
                mean += x;
            mean /= v.size();
            for (auto x : v)
                var += (x - mean) * (x - mean);
            const double deviation = std::sqrt(var / v.size());
            for (auto& x : v)
                x = static_cast<float>((x - mean) / deviation);
        };

        constexpr std::pair<float, float> octaves[] = { { 0.7f, 0.2f }, { 1.4f, 0.3f }, { 2.8f, 0.3f }, { 5.6f, 0.2f } };
        for (auto [sigma, weight] : octaves)
        {
            for (auto& x : noise)
                x = next();

            const int r = static_cast<int>(std::ceil(sigma * 3.0f));
            std::vector<float> kernel(2 * r + 1);
            float kernelSum = 0.0f;
            for (int i = -r; i <= r; ++i)
                kernelSum += kernel[i + r] = std::exp(-0.5f * (i / sigma) * (i / sigma));
            for (auto& k : kernel)
                k /= kernelSum;

            // Gaussian blur, rows then columns, wrapping around so that the texture tiles
            for (int y = 0; y < N; ++y)
                for (int x = 0; x < N; ++x)
                {
                    float acc = 0.0f;
                    for (int i = -r; i <= r; ++i)
                        acc += kernel[i + r] * noise[y * N + (x + i + N) % N];
                    blurred[y * N + x] = acc;
                }
            for (int y = 0; y < N; ++y)
                for (int x = 0; x < N; ++x)
                {
                    float acc = 0.0f;
                    for (int i = -r; i <= r; ++i)
                        acc += kernel[i + r] * blurred[((y + i + N) % N) * N + x];
                    noise[y * N + x] = acc;
                }

            normalize(noise);
            for (int i = 0; i < N * N; ++i)
                sum[i] += weight * noise[i];
        }

        normalize(sum);
        std::vector<float> height(N * N);
        for (int i = 0; i < N * N; ++i)
            height[i] = std::clamp(0.5f + sum[i] * 0.18f, 0.0f, 1.0f);

        // Red, green, blue as floats, averaged down for the mips
        std::vector<std::vector<float>> levels(1, std::vector<float>(N * N * 3));
        for (int y = 0; y < N; ++y)
            for (int x = 0; x < N; ++x)
            {
                const float dx = height[y * N + (x + 1) % N] - height[y * N + (x + N - 1) % N];
                const float dy = height[((y + 1) % N) * N + x] - height[((y + N - 1) % N) * N + x];
                auto* texel = &levels[0][(y * N + x) * 3];
                texel[0] = std::clamp(0.5f - dx, 0.0f, 1.0f);
                texel[1] = std::clamp(0.5f - dy, 0.0f, 1.0f);
                texel[2] = height[y * N + x];
            }
        for (int size = N / 2; size >= 1; size /= 2)
        {
            const auto& above = levels.back();
            std::vector<float> level(size * size * 3);
            for (int y = 0; y < size; ++y)
                for (int x = 0; x < size; ++x)
                    for (int c = 0; c < 3; ++c)
                    {
                        auto at = [&](int ax, int ay) { return above[(ay * size * 2 + ax) * 3 + c]; };
                        level[(y * size + x) * 3 + c] = 0.25f * (at(2 * x, 2 * y) + at(2 * x + 1, 2 * y) + at(2 * x, 2 * y + 1) + at(2 * x + 1, 2 * y + 1));
                    }
            levels.push_back(std::move(level));
        }

        std::vector<std::vector<uint32_t>> out;
        for (const auto& level : levels)
        {
            std::vector<uint32_t> texels(level.size() / 3);
            for (size_t i = 0; i < texels.size(); ++i)
            {
                auto byte = [&](int c) { return static_cast<uint32_t>(std::lround(std::clamp(level[i * 3 + c], 0.0f, 1.0f) * 255.0f)); };
                texels[i] = 0xFF000000u | (byte(0) << 16) | (byte(1) << 8) | byte(2);
            }
            out.push_back(std::move(texels));
        }
        return out;
    }

private:
    // Managed, so it survives a device reset.
    static IDirect3DTexture9* GetDetailTexture(IDirect3DDevice9* pDevice)
    {
        if (pDetailTex || bDetailTexFailed)
            return pDetailTex;

        const auto& levels = detailLevels;
        IDirect3DTexture9* tex = nullptr;
        if (levels.empty())
        {
            bDetailTexFailed = true;
            return nullptr;
        }
        if (FAILED(pDevice->CreateTexture(kDetailSize, kDetailSize, static_cast<UINT>(levels.size()), 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &tex, nullptr)) || !tex)
        {
            bDetailTexFailed = true;
            return nullptr;
        }
        for (UINT level = 0; level < levels.size(); ++level)
        {
            const int size = kDetailSize >> level;
            D3DLOCKED_RECT locked = {};
            if (FAILED(tex->LockRect(level, &locked, nullptr, 0)))
            {
                tex->Release();
                bDetailTexFailed = true;
                return nullptr;
            }
            for (int y = 0; y < size; ++y)
                memcpy(static_cast<uint8_t*>(locked.pBits) + y * locked.Pitch, &levels[level][y * size], size * sizeof(uint32_t));
            tex->UnlockRect(level);
        }
        pDetailTex = tex;
        return pDetailTex;
    }

    // c192 for the bicubic filter: the size of the diffuse texture the next draws read.
    static void SetDiffuseSize(IDirect3DDevice9* pDevice, IDirect3DBaseTexture9* pTexture)
    {
        float size[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
        D3DSURFACE_DESC desc = {};
        if (pTexture && pTexture->GetType() == D3DRTYPE_TEXTURE &&
            SUCCEEDED(static_cast<IDirect3DTexture9*>(pTexture)->GetLevelDesc(0, &desc)) && desc.Width && desc.Height)
        {
            size[0] = static_cast<float>(desc.Width);
            size[1] = static_cast<float>(desc.Height);
            size[2] = 1.0f / size[0];
            size[3] = 1.0f / size[1];
        }
        if (memcmp(size, lastDiffuseSize, sizeof(size)) == 0)
            return;
        memcpy(lastDiffuseSize, size, sizeof(size));
        pDevice->SetPixelShaderConstantF(192, size, 1);
    }

    static HRESULT WINAPI SetTextureHook(IDirect3DDevice9* pDevice, DWORD stage, IDirect3DBaseTexture9* pTexture)
    {
        if (bInGBuffer && bBicubic && stage == 0)
            SetDiffuseSize(pDevice, pTexture);
        return shSetTexture.unsafe_stdcall<HRESULT>(pDevice, stage, pTexture);
    }

    static HRESULT WINAPI SetSamplerStateHook(IDirect3DDevice9* pDevice, DWORD sampler, D3DSAMPLERSTATETYPE type, DWORD value)
    {
        if (bInGBuffer && type == D3DSAMP_MINFILTER && sampler < kMaterialStages)
        {
            // With the game's anisotropy at 1x, anisotropic filtering is the same as linear.
            bRaisedMinFilter[sampler] = bAnisoAllMaps && value == D3DTEXF_LINEAR;
            if (bRaisedMinFilter[sampler])
                value = D3DTEXF_ANISOTROPIC;
        }
        return shSetSamplerState.unsafe_stdcall<HRESULT>(pDevice, sampler, type, value);
    }

    // Only once one of the two options that need them is on.
    static bool InstallDeviceHooks(IDirect3DDevice9* pDevice)
    {
        if (!bDeviceHooksTried)
        {
            bDeviceHooksTried = true;
            auto vtbl = *reinterpret_cast<void***>(pDevice);
            shSetTexture = safetyhook::create_inline(vtbl[kSetTexture], SetTextureHook);
            shSetSamplerState = safetyhook::create_inline(vtbl[kSetSamplerState], SetSamplerStateHook);
            if (!shSetTexture || !shSetSamplerState)
            {
                shSetTexture = {};
                shSetSamplerState = {};
            }
        }
        return shSetTexture && shSetSamplerState;
    }

    // First command of the G-buffer pass.
    static void BeginGBuffer()
    {
        auto pDevice = rage::grcDevice::GetD3DDevice();
        if (!pDevice)
            return;

        static auto lodBiasPref = FusionFixSettings.GetRef("PREF_TEXTURE_LOD_BIAS");
        static auto anisoPref = FusionFixSettings.GetRef("PREF_ANISO_ALL_MAPS");
        static auto detailPref = FusionFixSettings.GetRef("PREF_DETAIL_TEXTURES");
        static auto bicubicPref = FusionFixSettings.GetRef("PREF_BICUBIC_TEXTURES");
        static auto specularAAPref = FusionFixSettings.GetRef("PREF_SPECULAR_AA");

        bAnisoAllMaps = Pref(anisoPref) != 0;
        bBicubic = Pref(bicubicPref) != 0;
        if ((bAnisoAllMaps || bBicubic) && !InstallDeviceHooks(pDevice))
            bAnisoAllMaps = bBicubic = false;
        const bool detail = Pref(detailPref) != 0 && GetDetailTexture(pDevice);
        const bool specularAA = Pref(specularAAPref) != 0;
        bInGBuffer = true;

        // Menu steps of -0.25, and the render scale's: textures as sharp as they'd be at the screen size, which DLSS
        // and FSR keep through the upscale
        const float scaleBias = std::log2(RenderScale::GetScale());
        const float bias = -0.25f * Pref(lodBiasPref) + scaleBias;
        if (bias < 0.0f)
        {
            for (DWORD i = 0; i < kMaterialStages; ++i)
            {
                savedLodBias[i] = 0;
                pDevice->GetSamplerState(i, D3DSAMP_MIPMAPLODBIAS, &savedLodBias[i]);
                pDevice->SetSamplerState(i, D3DSAMP_MIPMAPLODBIAS, std::bit_cast<DWORD>(bias));
            }
            bLodBiasSet = true;
        }

        if (detail)
        {
            RageDirect3DDevice9::SetTextureBoth(pDevice, kDetailStage, pDetailTex);
            pDevice->SetSamplerState(kDetailStage, D3DSAMP_ADDRESSU, D3DTADDRESS_WRAP);
            pDevice->SetSamplerState(kDetailStage, D3DSAMP_ADDRESSV, D3DTADDRESS_WRAP);
            pDevice->SetSamplerState(kDetailStage, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
            pDevice->SetSamplerState(kDetailStage, D3DSAMP_MINFILTER, D3DTEXF_ANISOTROPIC);
            pDevice->SetSamplerState(kDetailStage, D3DSAMP_MIPFILTER, D3DTEXF_LINEAR);
            pDevice->SetSamplerState(kDetailStage, D3DSAMP_MAXANISOTROPY, 4);
            // Its mips fade to grey, so below the screen size it would fade out nearer without the render scale's bias
            pDevice->SetSamplerState(kDetailStage, D3DSAMP_MIPMAPLODBIAS, std::bit_cast<DWORD>(scaleBias));
            // c190: tiling, the albedo grain as 1 + (height - 0.5) * 2 * strength; c191: the bumps
            const float params[8] = { fDetailTiling, 2.0f * fDetailAlbedo, 1.0f - fDetailAlbedo, 0.0f,
                                      fDetailNormal, -0.5f * fDetailNormal, 0.0f, 0.0f };
            pDevice->SetPixelShaderConstantF(190, params, 2);
        }

        if (bBicubic)
        {
            // The game skips setting a texture that is already bound, so start from what is.
            memset(lastDiffuseSize, 0, sizeof(lastDiffuseSize));
            IDirect3DBaseTexture9* tex = nullptr;
            pDevice->GetTexture(0, &tex);
            SetDiffuseSize(pDevice, tex);
            if (tex)
                tex->Release();
        }

        if (specularAA)
        {
            // Kaplanyan and Hoffman: roughness squared grows by twice the normal's variance,
            // half the sum of its squared screen space derivatives, up to a cap.
            const float params[4] = { 0.5f * fSpecularAAStrength, fSpecularAAMax, 0.0f, 0.0f };
            pDevice->SetPixelShaderConstantF(193, params, 1);
        }

        const BOOL flags[3] = { detail, bBicubic, specularAA };
        pDevice->SetPixelShaderConstantB(12, flags, 3);
    }

    // Last command of the G-buffer pass: lighting, post fx and the other render phases see the
    // game's own states again.
    static void EndGBuffer()
    {
        auto pDevice = rage::grcDevice::GetD3DDevice();
        if (!pDevice || !bInGBuffer)
            return;
        bInGBuffer = false;

        if (bLodBiasSet)
        {
            for (DWORD i = 0; i < kMaterialStages; ++i)
                pDevice->SetSamplerState(i, D3DSAMP_MIPMAPLODBIAS, savedLodBias[i]);
            bLodBiasSet = false;
        }
        for (DWORD i = 0; i < kMaterialStages; ++i)
        {
            if (bRaisedMinFilter[i])
            {
                pDevice->SetSamplerState(i, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
                bRaisedMinFilter[i] = false;
            }
        }
        RageDirect3DDevice9::SetTextureBoth(pDevice, kDetailStage, nullptr);
        const BOOL flags[3] = {};
        pDevice->SetPixelShaderConstantB(12, flags, 3);
    }

public:
    TextureQuality()
    {
        FusionFix::onInitEventAsync() += []()
        {
            CIniReader iniReader("");
            fDetailTiling = std::clamp(iniReader.ReadFloat("TEXTURES", "DetailTexturesTiling", 8.0f), 1.0f, 64.0f);
            fDetailAlbedo = std::clamp(iniReader.ReadFloat("TEXTURES", "DetailTexturesAlbedo", 0.15f), 0.0f, 1.0f);
            fDetailNormal = std::clamp(iniReader.ReadFloat("TEXTURES", "DetailTexturesNormal", 0.5f), 0.0f, 2.0f);
            fSpecularAAStrength = std::clamp(iniReader.ReadFloat("TEXTURES", "SpecularAntiAliasingStrength", 1.0f), 0.0f, 4.0f);
            fSpecularAAMax = std::clamp(iniReader.ReadFloat("TEXTURES", "SpecularAntiAliasingMax", 0.18f), 0.0f, 1.0f);
            detailLevels = MakeDetailLevels();
        };

        // Added once all the other modules have hooked the G-buffer pass, so that what they draw
        // at its start (the reflection mips) runs before the bias and the flags are set.
        FusionFix::onGameInitEvent() += []()
        {
            CRenderPhaseDeferredLighting_SceneToGBuffer::OnBuildRenderList() += []()
            {
                auto cb = new T_CB_Generic_NoArgs(BeginGBuffer);
                if (cb)
                    cb->Append();
            };
            CRenderPhaseDeferredLighting_SceneToGBuffer::OnAfterBuildRenderList() += []()
            {
                auto cb = new T_CB_Generic_NoArgs(EndGBuffer);
                if (cb)
                    cb->Append();
            };
        };

        FusionFix::onBeforeReset() += []()
        {
            // The textures and states are the device's own again after a reset.
            bInGBuffer = false;
            bLodBiasSet = false;
            memset(bRaisedMinFilter, 0, sizeof(bRaisedMinFilter));
            memset(lastDiffuseSize, 0, sizeof(lastDiffuseSize));
        };
    }
} TextureQuality;
