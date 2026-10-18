module;

#include <common.hxx>
#include <atomic>
#include "FusionLog.hpp"
#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <format>
#include <functional>
#include <optional>
#include <unordered_map>
#include <vector>

export module texturequality;

import common;
import comvars;
import renderscale;
import settings;
import inireload;
import entityclass;

// Texture quality options for the G-buffer pass, each turned on and off on its own in the
// graphics menu ([TEXTURES] in the ini has their tuning):
//  - Texture LOD Bias: a negative mip bias on the material textures, sharper at a distance.
//  - Anisotropic All Maps: the normal and specular maps that the game filters bilinearly get the
//    anisotropic filtering its own menu sets for the diffuse textures.
//  - Detail Textures: a fine grain on surfaces up close (b12, c190, c191, s12).
//  - Bicubic Filtering: magnified diffuse textures read with a Catmull-Rom filter (b13, c192).
//  - Specular Anti-Aliasing: highlights widened where the normal turns within a pixel (b14, c193).
//  - Ground Surfaces: terrain layers blended by height, their repeat broken up (hexagonal tiling of the textures
//    that tile evenly, c179), wide colour patches and bumps from their height; parallax occlusion mapping on
//    gta_parallax* and natural ground (b15, c166-c174, s12).
// The shader side is shaders/patches/texture_quality.patch, world_terrain_and_parallax.patch and world_terrain_hex_tiling.patch;
// Sharpening is a post fx pass.
class TextureQuality
{
    // s0-s5 hold the material textures in the G-buffer shaders; s10 is the stipple texture.
    static constexpr DWORD kMaterialStages = 6;
    static constexpr DWORD kDetailStage = 12;
    static constexpr int kDetailSize = 256;
    // IDirect3DDevice9 methods
    static constexpr int kSetTexture = 65;
    static constexpr int kSetSamplerState = 69;
    static constexpr int kSetPixelShader = 107;

    static inline float fDetailTiling = 8.0f;
    static inline float fDetailAlbedo = 0.15f;
    static inline float fDetailNormal = 0.5f;
    static inline float fSpecularAAStrength = 1.0f;
    static inline float fSpecularAAMax = 0.18f;
    static inline float fGroundHeightBlend = 1.0f;
    static inline float fGroundHeightBlendDepth = 0.2f;
    static inline float fGroundAntiTiling = 1.0f;
    static inline float fGroundAntiTilingSize = 4.0f;
    static inline float fGroundAntiTilingSharpness = 16.0f;
    static inline float fGroundColorVariation = 0.25f;
    static inline float fGroundColorVariationSize = 15.0f;
    static inline float fGroundBumps = 0.08f;
    static inline float fGroundDetailBumps = 0.005f;
    static inline float fGroundBumpsCoarse = 0.25f;
    static inline float fGroundBumpsFadeStart = 15.0f;
    static inline float fGroundBumpsFadeEnd = 60.0f;
    static inline float fGroundCavity = 2.0f;
    static inline float fGroundCavityAlbedo = 0.3f;
    static inline float fGroundParallaxDepth = 0.05f;
    static inline float fGroundParallaxContrast = 3.0f;
    static inline float fGroundSelfShadow = 0.5f;
    static inline float fGroundSelfShadowSharpness = 4.0f;
    static inline float fGroundParallaxFadeStart = 10.0f;
    static inline float fGroundParallaxFadeEnd = 30.0f;
    // Towards the sun or the moon, from the game's gDirectionalLight, and whether it was ever read.
    static inline float sunDirection[3] = {};
    static inline bool bSunDirection = false;
    static inline float fParallaxMinSteps = 8.0f;
    static inline float fParallaxMaxSteps = 32.0f;
    static inline float fParallaxDepth = 1.0f;
    // GroundSurfacesDebug (graphics menu, Debug): the ground shaders in colours, and how often each is bound.
    static int GroundDebug()
    {
        static auto pref = FusionFixSettings.GetRef("PREF_GROUND_DEBUG");
        return Pref(pref);
    }

    // GroundSurfacesDebug: how often the G-buffer pass binds the ground's shaders, which
    // world_terrain_and_parallax.patch marks with a constant (c186: 0.37, 0.61, 1e-12, kind).
    static inline SafetyHookInline shSetPixelShader{};
    static inline bool bPixelShaderHookTried = false;
    struct SeenShader
    {
        int kind = 0;
        // The number every shader of the pack carries in def c219 (snippets/AddShadersSignature2.lua),
        // after which its .asm says "// <number>"; 0 for none.
        uint32_t signature = 0;
        uint64_t binds = 0;
    };
    static inline std::unordered_map<IDirect3DPixelShader9*, SeenShader> shaderKinds;
    static inline uint64_t groundBinds[4] = {};
    static inline uint32_t groundFrames = 0;
    static inline bool bLastGround = false;

    // Props lying on the ground (boxes, bags, bins) are drawn with the same world shaders and
    // materials as the ground, so Ground Surfaces is kept to the map's own geometry: b15 is turned
    // off around the draws of props (entityclass.ixx). What b15 holds within the G-buffer pass,
    // and the draws it was turned off for.
    static inline bool bGroundBound = true;
    static inline uint64_t propDraws = 0;

    // Made during the asynchronous init, which is over before the G-buffer hooks are added.
    static inline std::vector<std::vector<uint32_t>> detailLevels;
    static inline IDirect3DTexture9* pDetailTex = nullptr;
    static inline bool bDetailTexFailed = false;

    static inline SafetyHookInline shSetTexture{};
    static inline SafetyHookInline shSetSamplerState{};
    static inline bool bDeviceHooksTried = false;
    static inline bool bTileHook = false;

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
        if (FAILED(RageDirect3DDevice9::RealDevice(pDevice)->CreateTexture(kDetailSize, kDetailSize, static_cast<UINT>(levels.size()), 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &tex, nullptr)) || !tex)
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

    // Ground Surfaces' hexagonal tiling (world_terrain_hex_tiling.patch) takes only diffuse textures that tile evenly:
    // c179.x, from a look at each texture's small mips the first time the G-buffer pass binds it. Textures are freed
    // and made as the world streams, so a texture's entry is checked against its size and format and all are
    // dropped now and then.
    struct TileInfo
    {
        UINT width = 0, height = 0, levels = 0;
        D3DFORMAT format = D3DFMT_UNKNOWN;
        float score = 1.0f;
    };
    static inline std::unordered_map<IDirect3DBaseTexture9*, TileInfo> tileInfos;
    static inline std::atomic<bool> bForgetAfterReset = false;
    static inline float lastTileScore = -1.0f;
    static inline uint32_t tileFrames = 0;
    static inline uint32_t tileLooksThisFrame = 0;
    static inline uint64_t tileLooked = 0, tileUnreadable = 0, tileEven = 0;
    // GroundAntiTilingTolerance: how much large scale variation a texture may have and still be tiled
    static inline float fGroundAntiTilingTolerance = 0.1f;
    static constexpr uint32_t kTileLooksPerFrame = 64;
    static constexpr uint32_t kTileFramesKept = 1200;

    // A level of a texture as RGB floats; only the formats world textures come in.
    static bool ReadLevel(IDirect3DTexture9* tex, UINT level, std::vector<float>& rgb, UINT& width, UINT& height)
    {
        D3DSURFACE_DESC desc = {};
        if (FAILED(tex->GetLevelDesc(level, &desc)) || !desc.Width || !desc.Height)
            return false;
        const bool dxt1 = desc.Format == D3DFMT_DXT1;
        const bool dxt = dxt1 || desc.Format == D3DFMT_DXT2 || desc.Format == D3DFMT_DXT3 || desc.Format == D3DFMT_DXT4 || desc.Format == D3DFMT_DXT5;
        const bool argb = desc.Format == D3DFMT_A8R8G8B8 || desc.Format == D3DFMT_X8R8G8B8;
        if (!dxt && !argb)
            return false;
        D3DLOCKED_RECT locked = {};
        if (FAILED(tex->LockRect(level, &locked, nullptr, D3DLOCK_READONLY)) || !locked.pBits)
            return false;
        width = desc.Width;
        height = desc.Height;
        rgb.assign(size_t(width) * height * 3, 0.0f);
        auto put = [&](UINT x, UINT y, float r, float g, float b)
        {
            if (x >= width || y >= height)
                return;
            auto* t = &rgb[(size_t(y) * width + x) * 3];
            t[0] = r; t[1] = g; t[2] = b;
        };
        const auto* bits = static_cast<const uint8_t*>(locked.pBits);
        if (argb)
        {
            for (UINT y = 0; y < height; ++y)
                for (UINT x = 0; x < width; ++x)
                {
                    const uint8_t* p = bits + y * locked.Pitch + x * 4;
                    put(x, y, p[2] / 255.0f, p[1] / 255.0f, p[0] / 255.0f);
                }
        }
        else
        {
            // Only the colour block: DXT1 has it alone, DXT2-5 after an 8 byte alpha block.
            const UINT blockSize = dxt1 ? 8 : 16;
            for (UINT by = 0; by < (height + 3) / 4; ++by)
                for (UINT bx = 0; bx < (width + 3) / 4; ++bx)
                {
                    const uint8_t* block = bits + by * locked.Pitch + bx * blockSize + (dxt1 ? 0 : 8);
                    const uint16_t c0 = uint16_t(block[0] | (block[1] << 8)), c1 = uint16_t(block[2] | (block[3] << 8));
                    float pal[4][3];
                    auto unpack = [](uint16_t c, float* out)
                    {
                        out[0] = ((c >> 11) & 31) / 31.0f;
                        out[1] = ((c >> 5) & 63) / 63.0f;
                        out[2] = (c & 31) / 31.0f;
                    };
                    unpack(c0, pal[0]);
                    unpack(c1, pal[1]);
                    for (int c = 0; c < 3; ++c)
                    {
                        if (!dxt1 || c0 > c1)
                        {
                            pal[2][c] = (2.0f * pal[0][c] + pal[1][c]) / 3.0f;
                            pal[3][c] = (pal[0][c] + 2.0f * pal[1][c]) / 3.0f;
                        }
                        else
                        {
                            pal[2][c] = 0.5f * (pal[0][c] + pal[1][c]);
                            pal[3][c] = 0.0f;
                        }
                    }
                    const uint32_t indices = block[4] | (block[5] << 8) | (block[6] << 16) | (uint32_t(block[7]) << 24);
                    for (UINT i = 0; i < 16; ++i)
                    {
                        const float* c = pal[(indices >> (2 * i)) & 3];
                        put(bx * 4 + (i & 3), by * 4 + (i >> 2), c[0], c[1], c[2]);
                    }
                }
        }
        tex->UnlockRect(level);
        return true;
    }

    static float Luma(const float* c)
    {
        return 0.2126f * c[0] + 0.7152f * c[1] + 0.0722f * c[2];
    }

    static float Smoothstep(float a, float b, float x)
    {
        const float t = std::clamp((x - a) / (b - a), 0.0f, 1.0f);
        return t * t * (3.0f - 2.0f * t);
    }

    // 1 for a texture that tiles evenly: even at large scale (its mip of about 4x4 texels), of a natural colour
    // (grass, earth, sand: saturated, green not far under red, blue not over green) and without stripes or tracks
    // (in its mip of about 16x16 the light changes as much along every direction); 0 otherwise. Unreadable
    // textures keep the old behaviour, 1, and are left to the shader's own test of the saturation.
    static float TileScore(IDirect3DBaseTexture9* base, bool& readable)
    {
        readable = false;
        if (!base || base->GetType() != D3DRTYPE_TEXTURE)
            return 0.0f;
        auto tex = static_cast<IDirect3DTexture9*>(base);
        D3DSURFACE_DESC desc = {};
        if (FAILED(tex->GetLevelDesc(0, &desc)))
            return 0.0f;
        const UINT levels = tex->GetLevelCount();
        const UINT shortSide = (std::min)(desc.Width, desc.Height);
        if (shortSide < 32 || (std::max)(desc.Width, desc.Height) > 2 * shortSide)
            return 0.0f;
        auto msb = [](UINT v) { UINT n = 0; while (v >>= 1) ++n; return n; };
        const UINT coarse = (std::min)(msb(shortSide) - 2, levels - 1);
        const UINT fine = (std::min)(msb(shortSide) - 4, levels - 1);
        if ((shortSide >> coarse) > 16)
            return 0.0f; // no mips to look at
        std::vector<float> rgb;
        UINT w = 0, h = 0;
        if (!ReadLevel(tex, coarse, rgb, w, h))
            return 1.0f;
        readable = true;
        const size_t n = size_t(w) * h;
        float mean[3] = {};
        for (size_t i = 0; i < n; ++i)
            for (int c = 0; c < 3; ++c)
                mean[c] += rgb[i * 3 + c] / n;
        float deviation = 0.0f;
        for (size_t i = 0; i < n; ++i)
        {
            const float d[3] = { rgb[i * 3] - mean[0], rgb[i * 3 + 1] - mean[1], rgb[i * 3 + 2] - mean[2] };
            deviation = (std::max)(deviation, std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]));
        }
        const float luma = Luma(mean) + 0.05f;
        const float even = 1.0f - Smoothstep(fGroundAntiTilingTolerance, fGroundAntiTilingTolerance * 1.5f, deviation / luma);
        const float hi = (std::max)({ mean[0], mean[1], mean[2] }), lo = (std::min)({ mean[0], mean[1], mean[2] });
        const float saturation = hi > 1e-4f ? (hi - lo) / hi : 0.0f;
        const float natural = Smoothstep(0.08f, 0.15f, saturation) * Smoothstep(0.45f, 0.6f, mean[1] / (std::max)(mean[0], 1e-4f)) *
                              (mean[2] <= mean[1] * 1.05f ? 1.0f : 0.0f);
        float score = even * natural;
        if (score > 0.0f && ReadLevel(tex, fine, rgb, w, h) && w >= 4 && h >= 4)
        {
            // How much the light changes along x, y and the two diagonals, wrapping around as the texture tiles
            float sums[4] = {};
            auto at = [&](UINT x, UINT y) { return Luma(&rgb[(size_t(y % h) * w + (x % w)) * 3]); };
            for (UINT y = 0; y < h; ++y)
                for (UINT x = 0; x < w; ++x)
                {
                    const float c = at(x, y);
                    sums[0] += std::abs(at(x + 1, y) - c);
                    sums[1] += std::abs(at(x, y + 1) - c);
                    sums[2] += std::abs(at(x + 1, y + 1) - c);
                    sums[3] += std::abs(at(x + 1, y + h - 1) - c);
                }
            const float most = (std::max)({ sums[0], sums[1], sums[2], sums[3] }), least = (std::min)({ sums[0], sums[1], sums[2], sums[3] });
            const float anisotropy = (most - least) / (most + 1e-4f);
            const float contrast = most / float(w * h) / luma;
            score *= 1.0f - Smoothstep(0.55f, 0.75f, anisotropy) * Smoothstep(0.03f, 0.06f, contrast);
        }
        return score;
    }

    // c179 for the hexagonal tiling: how evenly the diffuse texture the next draws read tiles.
    static void SetTileScore(IDirect3DDevice9* pDevice, IDirect3DBaseTexture9* pTexture)
    {
        float score = 0.0f;
        if (pTexture)
        {
            TileInfo info;
            D3DSURFACE_DESC desc = {};
            if (pTexture->GetType() == D3DRTYPE_TEXTURE && SUCCEEDED(static_cast<IDirect3DTexture9*>(pTexture)->GetLevelDesc(0, &desc)))
            {
                info.width = desc.Width;
                info.height = desc.Height;
                info.format = desc.Format;
                info.levels = pTexture->GetLevelCount();
            }
            auto it = tileInfos.find(pTexture);
            if (it != tileInfos.end() && it->second.width == info.width && it->second.height == info.height &&
                it->second.format == info.format && it->second.levels == info.levels)
                score = it->second.score;
            else if (tileLooksThisFrame < kTileLooksPerFrame)
            {
                ++tileLooksThisFrame;
                bool readable = false;
                info.score = TileScore(pTexture, readable);
                ++tileLooked;
                if (!readable)
                    ++tileUnreadable;
                else if (info.score > 0.5f)
                    ++tileEven;
                tileInfos[pTexture] = info;
                score = info.score;
            }
            else
                score = 1.0f; // looked at in a later frame
        }
        if (score == lastTileScore)
            return;
        lastTileScore = score;
        const float c179[4] = { score, 0.0f, 0.0f, 0.0f };
        pDevice->SetPixelShaderConstantF(179, c179, 1);
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
        if (bInGBuffer && bLastGround && bTileHook && stage == 0)
            SetTileScore(pDevice, pTexture);
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

    // UpscalerLodBias in [TEMPORAL]: with DLAA or FSR the textures' mips go this much further below the render scale's,
    // as AMD and NVIDIA advise: the upscaler resolves the detail over the jittered frames that one frame would alias
    static inline float fUpscalerLodBias = -1.0f;

    // At startup and on Ctrl+Shift+F10 (IniReload): all of these go to the shaders every frame.
    static void ReadIni(CIniReader& iniReader)
    {
        fDetailTiling = std::clamp(iniReader.ReadFloat("TEXTURES", "DetailTexturesTiling", 8.0f), 1.0f, 64.0f);
        fDetailAlbedo = std::clamp(iniReader.ReadFloat("TEXTURES", "DetailTexturesAlbedo", 0.15f), 0.0f, 1.0f);
        fDetailNormal = std::clamp(iniReader.ReadFloat("TEXTURES", "DetailTexturesNormal", 0.5f), 0.0f, 2.0f);
        fSpecularAAStrength = std::clamp(iniReader.ReadFloat("TEXTURES", "SpecularAntiAliasingStrength", 1.0f), 0.0f, 4.0f);
        fSpecularAAMax = std::clamp(iniReader.ReadFloat("TEXTURES", "SpecularAntiAliasingMax", 0.18f), 0.0f, 1.0f);
        fUpscalerLodBias = std::clamp(iniReader.ReadFloat("ANTIALIASING", "UpscalerLodBias", -1.0f), -2.0f, 0.0f);
        fGroundHeightBlend = std::clamp(iniReader.ReadFloat("TEXTURES", "GroundHeightBlend", 1.0f), 0.0f, 1.0f);
        fGroundHeightBlendDepth = std::clamp(iniReader.ReadFloat("TEXTURES", "GroundHeightBlendDepth", 0.2f), 0.01f, 1.0f);
        fGroundAntiTiling = std::clamp(iniReader.ReadFloat("TEXTURES", "GroundAntiTiling", 1.0f), 0.0f, 1.0f);
        fGroundAntiTilingSize = std::clamp(iniReader.ReadFloat("TEXTURES", "GroundAntiTilingSize", 4.0f), 0.5f, 100.0f);
        fGroundAntiTilingSharpness = std::clamp(iniReader.ReadFloat("TEXTURES", "GroundAntiTilingSharpness", 16.0f), 1.0f, 100.0f);
        fGroundAntiTilingTolerance = std::clamp(iniReader.ReadFloat("TEXTURES", "GroundAntiTilingTolerance", 0.1f), 0.01f, 1.0f);
        tileInfos.clear();
        fGroundColorVariation = std::clamp(iniReader.ReadFloat("TEXTURES", "GroundColorVariation", 0.25f), 0.0f, 1.0f);
        fGroundColorVariationSize = std::clamp(iniReader.ReadFloat("TEXTURES", "GroundColorVariationSize", 15.0f), 1.0f, 500.0f);
        fGroundBumps = std::clamp(iniReader.ReadFloat("TEXTURES", "GroundBumps", 0.08f), 0.0f, 0.5f);
        fGroundBumpsCoarse = std::clamp(iniReader.ReadFloat("TEXTURES", "GroundBumpsCoarse", 0.25f), 0.0f, 2.0f);
        fGroundDetailBumps = std::clamp(iniReader.ReadFloat("TEXTURES", "GroundDetailBumps", 0.005f), 0.0f, 0.1f);
        fGroundBumpsFadeStart = std::clamp(iniReader.ReadFloat("TEXTURES", "GroundBumpsFadeStart", 15.0f), 0.0f, 1000.0f);
        fGroundBumpsFadeEnd = std::clamp(iniReader.ReadFloat("TEXTURES", "GroundBumpsFadeEnd", 60.0f), fGroundBumpsFadeStart, 1000.0f);
        fGroundCavity = std::clamp(iniReader.ReadFloat("TEXTURES", "GroundCavity", 2.0f), 0.0f, 10.0f);
        fGroundCavityAlbedo = std::clamp(iniReader.ReadFloat("TEXTURES", "GroundCavityAlbedo", 0.3f), 0.0f, 1.0f);
        fGroundParallaxDepth = std::clamp(iniReader.ReadFloat("TEXTURES", "GroundParallaxDepth", 0.05f), 0.0f, 0.5f);
        fGroundParallaxContrast = std::clamp(iniReader.ReadFloat("TEXTURES", "GroundParallaxContrast", 3.0f), 0.1f, 20.0f);
        fGroundSelfShadow = std::clamp(iniReader.ReadFloat("TEXTURES", "GroundSelfShadow", 0.5f), 0.0f, 1.0f);
        fGroundSelfShadowSharpness = std::clamp(iniReader.ReadFloat("TEXTURES", "GroundSelfShadowSharpness", 4.0f), 0.5f, 50.0f);
        fGroundParallaxFadeStart = std::clamp(iniReader.ReadFloat("TEXTURES", "GroundParallaxFadeStart", 10.0f), 0.0f, 1000.0f);
        fGroundParallaxFadeEnd = std::clamp(iniReader.ReadFloat("TEXTURES", "GroundParallaxFadeEnd", 30.0f), fGroundParallaxFadeStart, 1000.0f);
        // The shader's loop stops at 32 steps.
        fParallaxMinSteps = std::clamp(iniReader.ReadFloat("TEXTURES", "ParallaxOcclusionMinSteps", 8.0f), 2.0f, 32.0f);
        fParallaxMaxSteps = std::clamp(iniReader.ReadFloat("TEXTURES", "ParallaxOcclusionMaxSteps", 32.0f), fParallaxMinSteps, 32.0f);
        fParallaxDepth = std::clamp(iniReader.ReadFloat("TEXTURES", "ParallaxOcclusionDepth", 1.0f), 0.0f, 4.0f);
    }

    // Kind 0 for any other shader, 1 for the terrain, 2 for gta_parallax*, 3 for the generic world
    // shaders that treat ground facing up (gta_default, rage_default, gta_spec, gta_normal, gta_normal_spec).
    static SeenShader& GroundShaderInfo(IDirect3DPixelShader9* shader)
    {
        if (auto it = shaderKinds.find(shader); it != shaderKinds.end())
            return it->second;
        SeenShader info;
        UINT size = 0;
        if (SUCCEEDED(shader->GetFunction(nullptr, &size)) && size >= 16 && size < (1u << 20))
        {
            std::vector<uint32_t> code(size / 4);
            if (SUCCEEDED(shader->GetFunction(code.data(), &size)))
            {
                const uint32_t mark[3] = { std::bit_cast<uint32_t>(0.37f), std::bit_cast<uint32_t>(0.61f), std::bit_cast<uint32_t>(1e-12f) };
                const uint32_t sign[3] = { std::bit_cast<uint32_t>(1.8395173895e+25f), std::bit_cast<uint32_t>(3.9938258725e+24f), std::bit_cast<uint32_t>(4.5435787456e+30f) };
                for (size_t i = 0; i + 3 < code.size(); ++i)
                {
                    if (code[i] == mark[0] && code[i + 1] == mark[1] && code[i + 2] == mark[2])
                        info.kind = std::clamp(static_cast<int>(std::bit_cast<float>(code[i + 3])), 0, 3);
                    if (code[i] == sign[0] && code[i + 1] == sign[1] && code[i + 2] == sign[2])
                        info.signature = code[i + 3];
                }
            }
        }
        return shaderKinds.emplace(shader, info).first->second;
    }

    static HRESULT WINAPI SetPixelShaderHook(IDirect3DDevice9* pDevice, IDirect3DPixelShader9* shader)
    {
        if (bInGBuffer && GroundDebug() && shader)
        {
            auto& info = GroundShaderInfo(shader);
            ++info.binds;
            ++groundBinds[info.kind];
        }
        return shSetPixelShader.unsafe_stdcall<HRESULT>(pDevice, shader);
    }

    static void BindGround(bool ground)
    {
        if (!bInGBuffer || !bLastGround || ground == bGroundBound)
            return;
        auto pDevice = rage::grcDevice::GetD3DDevice();
        if (!pDevice)
            return;
        const BOOL flag = ground;
        pDevice->SetPixelShaderConstantB(15, &flag, 1);
        bGroundBound = ground;
    }

    static void BeforeEntityDraw(EntityClass::Kind kind)
    {
        if (kind != EntityClass::Kind::Prop)
            return;
        BindGround(false);
        ++propDraws;
    }

    static void AfterEntityDraw(EntityClass::Kind kind)
    {
        if (kind == EntityClass::Kind::Prop)
            BindGround(true);
    }

    // Ctrl+Shift+F10 (IniReload): the settings read again, and with GroundSurfacesDebug on what the G-buffer pass drew
    // with written to GTAIV.EFLC.FusionFix.Ground.log.
    static void OnIniReload(CIniReader& iniReader)
    {
        {
            ReadIni(iniReader);
            const double frames = groundFrames ? double(groundFrames) : 1.0;
            FusionLog::Write("Ground", "Draws", "Ground Surfaces %s, detail texture %s, debug %d, set-pixel-shader hook %s, prop hooks %s; per frame over %u frames: "
                    "terrain shaders %.1f, parallax shaders %.1f, world shaders with ground %.1f, other shaders %.1f, prop draws without ground %.1f; %zu shaders seen\n",
                    bLastGround ? "on" : "off", pDetailTex ? "made" : (bDetailTexFailed ? "failed" : "not made"), GroundDebug(),
                    shSetPixelShader ? "on" : (bPixelShaderHookTried ? "failed" : "not tried"), EntityClass::Installed() ? "on" : "failed", groundFrames,
                    groundBinds[1] / frames, groundBinds[2] / frames, groundBinds[3] / frames, groundBinds[0] / frames, propDraws / frames, shaderKinds.size());
            // Which shaders those were, the most bound first, by their number in the pack
            std::vector<const SeenShader*> seen;
            for (const auto& [ptr, info] : shaderKinds)
                if (info.binds)
                    seen.push_back(&info);
            std::sort(seen.begin(), seen.end(), [](auto a, auto b) { return a->binds > b->binds; });
            std::string list;
            for (auto info : seen)
                list += std::format("{}{} {:.1f}", list.empty() ? "" : ", ", info->signature, info->binds / frames);
            if (!list.empty())
                FusionLog::Write("Ground", "Shaders", "G-buffer pixel shaders by their number in the pack, binds per frame: %s\n", list.c_str());
            FusionLog::Write("Ground", "Tiling", "Hexagonal tiling: texture hook %s, %llu textures looked at, %llu unreadable (tiled as before), %llu tile evenly; %zu kept\n",
                    bTileHook ? "on" : "off", static_cast<unsigned long long>(tileLooked), static_cast<unsigned long long>(tileUnreadable),
                    static_cast<unsigned long long>(tileEven), tileInfos.size());
            for (auto& [ptr, info] : shaderKinds)
                info.binds = 0;
            memset(groundBinds, 0, sizeof(groundBinds));
            propDraws = 0;
            groundFrames = 0;
        }
    }

    // Every G-buffer pass: what GroundSurfacesDebug counts.
    static void TickGroundDebug(IDirect3DDevice9* pDevice)
    {
        if (GroundDebug() && !bPixelShaderHookTried)
        {
            bPixelShaderHookTried = true;
            auto vtbl = *reinterpret_cast<void***>(pDevice);
            shSetPixelShader = safetyhook::create_inline(vtbl[kSetPixelShader], SetPixelShaderHook);
        }
        if (GroundDebug())
            ++groundFrames;
    }

    // First command of the G-buffer pass.
    static void BeginGBuffer()
    {
        if (bForgetAfterReset.exchange(false))
        {
            shaderKinds.clear();
            tileInfos.clear();
        }
        auto pDevice = rage::grcDevice::GetD3DDevice();
        if (!pDevice)
            return;
        TickGroundDebug(pDevice);

        static auto lodBiasPref = FusionFixSettings.GetRef("PREF_TEXTURE_LOD_BIAS");
        static auto anisoPref = FusionFixSettings.GetRef("PREF_ANISO_ALL_MAPS");
        static auto detailPref = FusionFixSettings.GetRef("PREF_DETAIL_TEXTURES");
        static auto bicubicPref = FusionFixSettings.GetRef("PREF_BICUBIC_TEXTURES");
        static auto specularAAPref = FusionFixSettings.GetRef("PREF_SPECULAR_AA");
        static auto groundPref = FusionFixSettings.GetRef("PREF_GROUND_SURFACES");

        bAnisoAllMaps = Pref(anisoPref) != 0;
        bBicubic = Pref(bicubicPref) != 0;
        const bool detail = Pref(detailPref) != 0 && GetDetailTexture(pDevice);
        const bool specularAA = Pref(specularAAPref) != 0;
        // The ground's noise is the detail texture read at the scale of metres.
        const bool ground = Pref(groundPref) != 0 && GetDetailTexture(pDevice);
        const bool hooks = (bAnisoAllMaps || bBicubic || ground) && InstallDeviceHooks(pDevice);
        if (!hooks)
            bAnisoAllMaps = bBicubic = false;
        bTileHook = ground && hooks;
        bLastGround = ground;
        bInGBuffer = true;
        bGroundBound = true;

        // Menu steps of -0.25, and the render scale's: textures as sharp as they'd be at the screen size, which DLSS
        // and FSR keep through the upscale, and UpscalerLodBias more with them
        const float scaleBias = std::log2(RenderScale::GetScale()) + (RenderScale::IsUpscalerAntialiasing() ? fUpscalerLodBias : 0.0f);
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

        if (detail || ground)
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
        }

        if (detail)
        {
            // c190: tiling, the albedo grain as 1 + (height - 0.5) * 2 * strength; c191: the bumps
            const float params[8] = { fDetailTiling, 2.0f * fDetailAlbedo, 1.0f - fDetailAlbedo, 0.0f,
                                      fDetailNormal, -0.5f * fDetailNormal, 0.0f, 0.0f };
            pDevice->SetPixelShaderConstantF(190, params, 2);
        }

        if (ground)
        {
            // Without the hook every natural texture is tiled, as before the look at their mips.
            tileLooksThisFrame = 0;
            if (++tileFrames % kTileFramesKept == 0)
                tileInfos.clear();
            lastTileScore = -1.0f;
            if (bTileHook)
            {
                // The game skips setting a texture that is already bound, so start from what is.
                IDirect3DBaseTexture9* tex = nullptr;
                pDevice->GetTexture(0, &tex);
                SetTileScore(pDevice, tex);
                if (tex)
                    tex->Release();
            }
            else
            {
                const float c179[4] = { 1.0f, 0.0f, 0.0f, 0.0f };
                pDevice->SetPixelShaderConstantF(179, c179, 1);
            }
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

        if (ground)
        {
            // The noise's blobs are about a twelfth of the texture's repeat across.
            constexpr float kBlobsPerRepeat = 12.0f;
            const float fadeRange = (std::max)(fGroundBumpsFadeEnd - fGroundBumpsFadeStart, 0.1f);
            const float parallaxFadeRange = (std::max)(fGroundParallaxFadeEnd - fGroundParallaxFadeStart, 0.1f);
            // The ground's parallax looks at it from the camera of the pass, the scene's at its start.
            float camera[3] = {};
            if (auto viewport = rage::GetCurrentViewport(); viewport && viewport->mIsPerspective)
                for (int i = 0; i < 3; ++i)
                    camera[i] = viewport->mViewInverseMatrix[3][i];
            // gDirectionalLight is a RAGE global, the same register in every shader, still the last frame's here:
            // the direction the sun's (or the moon's) light travels.
            float light[4] = {};
            if (SUCCEEDED(pDevice->GetPixelShaderConstantF(17, light, 1)))
            {
                const float len = std::sqrt(light[0] * light[0] + light[1] * light[1] + light[2] * light[2]);
                if (len > 0.9f && len < 1.1f)
                {
                    for (int i = 0; i < 3; ++i)
                        sunDirection[i] = -light[i] / len;
                    bSunDirection = true;
                }
            }
            const float params[36] = {
                // c166: height blend depth and strength, anti-tiling strength and the sharpness of its patches
                fGroundHeightBlendDepth, fGroundHeightBlend, fGroundAntiTiling, fGroundAntiTilingSharpness,
                // c167: per metre, the colour patches' and the anti-tiling's noise; a mip bias that leaves out its grain;
                // the colour noise's contrast: blurred, it strays only about 0.1 from its mean, so it is stretched to -1 to 1
                1.0f / (kBlobsPerRepeat * fGroundColorVariationSize), 1.0f / (kBlobsPerRepeat * fGroundAntiTilingSize), 1.0f, 3.0f,
                // c168: how much the colour patches lighten and darken each channel, a little warmer where lighter
                fGroundColorVariation, 0.9f * fGroundColorVariation, 0.75f * fGroundColorVariation, 0.0f,
                // c169: bump height in metres, its fade with the distance as a * depth + b, the detail grain's height
                fGroundBumps, -1.0f / fadeRange, fGroundBumpsFadeEnd / fadeRange, fGroundDetailBumps,
                // c170: parallax steps looking straight down, the more added flat along the surface, depth
                // and GroundSurfacesDebug: the terrain drawn half magenta, gta_parallax* half cyan, other ground orange
                fParallaxMinSteps, fParallaxMaxSteps - fParallaxMinSteps, fParallaxDepth, float(GroundDebug()),
                // c171: the hollows' occlusion per unit of brightness under the local average, the part of it the
                // colour takes, the height of the local average's lumps in metres, the mip bias that reads it
                fGroundCavity, fGroundCavityAlbedo, fGroundBumpsCoarse, 3.0f,
                // c172: the camera's position, the depth of the ground's parallax in metres
                camera[0], camera[1], camera[2], fGroundParallaxDepth,
                // c173: towards the sun, how dark the parallax's self-shadow is
                sunDirection[0], sunDirection[1], sunDirection[2], bSunDirection ? fGroundSelfShadow : 0.0f,
                // c174: how much the brightness, against the texture's average, raises the parallax's height; how
                // sharply the self-shadow comes in with the height standing above the ray to the sun; the parallax's
                // fade with the distance as a * depth + b
                fGroundParallaxContrast, fGroundSelfShadowSharpness, -1.0f / parallaxFadeRange, fGroundParallaxFadeEnd / parallaxFadeRange,
            };
            pDevice->SetPixelShaderConstantF(166, params, 9);
        }

        const BOOL flags[4] = { detail, bBicubic, specularAA, ground };
        pDevice->SetPixelShaderConstantB(12, flags, 4);
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
        const BOOL flags[4] = {};
        pDevice->SetPixelShaderConstantB(12, flags, 4);
    }

public:
    TextureQuality()
    {
        FusionFix::onInitEventAsync() += []()
        {
            CIniReader iniReader("");
            ReadIni(iniReader);
            IniReload::Add(OnIniReload);
            detailLevels = MakeDetailLevels();
        };

        EntityClass::AddDrawHooks(BeforeEntityDraw, AfterEntityDraw);

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
            // Shaders made after the reset may reuse the old ones' addresses. Emptied on the render thread, which
            // fills them while the main thread resets the device
            bForgetAfterReset = true;
        };
    }
} TextureQuality;
