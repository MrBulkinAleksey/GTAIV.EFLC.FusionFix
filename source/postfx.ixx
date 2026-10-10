module;

#include <common.hxx>
#include "ShadowLookupLayout.hpp"
#include <d3dx9tex.h>
#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <random>
#include <regex>
#include <array>
#include <atomic>
#include <unordered_map>
#include "FusionLog.hpp"

export module postfx;

import common;
import comvars;
import d3dx9_43;
import framegeneration;
import framehistory;
import hdr;
import natives;
import settings;
import shaders;
import renderscale;
import temporal;
import upscaler;

#define IDR_FXAA                                 101
#define IDR_SMAA                                 102
#define IDR_AreaTex                              103
#define IDR_SearchTex                            104
#define IDR_bluenoisevolume                      105

#define IDR_dof_blur_ps                          110
#define IDR_dof_coc_ps                           111
#define IDR_depth_of_field_tent_ps               113
#define IDR_stipple_filter_ps                    116
#define IDR_DeferredShadowGen_ps                 117
#define IDR_deferred_lighting_PS1                118
#define IDR_deferred_lighting_PS2                119
#define IDR_SSAO_gen_ps                          120
#define IDR_SSAO_blend_ps                        121

#define IDR_DeferredShadowBlur                   126
#define IDR_SunShafts_PS                         127
#define IDR_CascadeAtlasGen                      128

#define IDR_Blit_PS                              129

#define IDR_AO_FX                                133
#define IDR_SSR_FX                               136
#define IDR_CAS                                  137
#define IDR_CLOUDS_FX                            138
#define IDR_WETGROUND_FX                         139
#define IDR_HEADLIGHTGLINTS_FX                   140

#define IDR_SSDraw_PS_compiled                   2127
#define IDR_SSPrepass_PS_compiled                2128
#define IDR_SSAdd_PS_compiled                    2129
#define IDR_FxaaPS_compiled                      2101
#define IDR_SMAA_EdgeDetection_compiled          2102
#define IDR_SMAA_BlendingWeightsCalculation_compiled 2103
#define IDR_SMAA_NeighborhoodBlending_compiled   2104
#define IDR_SMAA_EdgeDetectionVS_compiled        2105
#define IDR_SMAA_BlendingWeightsCalculationVS_compiled 2106
#define IDR_SMAA_NeighborhoodBlendingVS_compiled 2107
#define IDR_CAS_PS_compiled                      2137
#define IDR_CASMasked_PS_compiled                2138

#ifndef SAFE_RELEASE
#define SAFE_RELEASE(p) { if (p) { (p)->Release(); (p)=NULL; } }
#endif

std::optional<std::reference_wrapper<int32_t>> UsePostFxAA;

// SMAA hangs the game on D3D9on12, it counts as off there
bool IsSMAASupported()
{
    static bool d3d9on12 = false;
    d3d9on12 = d3d9on12 || GetModuleHandleW(L"d3d9on12.dll");
    return !d3d9on12;
}

bool IsPostFxAA()
{
    if (!UsePostFxAA)
        return false;
    auto aa = UsePostFxAA->get();
    return aa == FusionFixSettings.AntialiasingText.eFXAA || (aa == FusionFixSettings.AntialiasingText.eSMAA && IsSMAASupported());
}

// Binding through both the game's device wrapper and the real device: see RageDirect3DDevice9::SetTextureBoth. SSGI's
// accumulation read the G-buffer normals as its depth while the right depth was "bound" through the wrapper alone.
using RageDirect3DDevice9::RealDevice;
using RageDirect3DDevice9::SetTextureBoth;
using RageDirect3DDevice9::SetSamplerStateBoth;

// The sampler states SSR.fx declares (MinFilter, MagFilter, MipFilter, AddressU, AddressV), read from its source once
// it is created: D3DX put them on its own registers too, and the trace showed samplers declared without a filter
// (SpecularTex, NormalTex) filtered linearly in some passes. Undeclared states are point and clamp, as
// kSSRSamplerStates leaves them.
using SamplerStates = std::array<DWORD, 5>;
static std::unordered_map<std::string, SamplerStates> SSRSamplerStates;

static void ReadSamplerStates(HMODULE hm, int resource, std::unordered_map<std::string, SamplerStates>& out)
{
    out.clear();
    HRSRC info = FindResourceW(hm, MAKEINTRESOURCEW(resource), RT_RCDATA);
    HGLOBAL data = info ? LoadResource(hm, info) : nullptr;
    const char* text = data ? static_cast<const char*>(LockResource(data)) : nullptr;
    if (!text)
        return;
    const std::string source(text, SizeofResource(hm, info));
    auto lower = [](std::string v) { for (auto& ch : v) ch = char(std::tolower(static_cast<unsigned char>(ch))); return v; };
    static const std::regex block(R"(sampler2D\s+(\w+)\s*\{([^}]*)\})");
    static const std::regex assign(R"((\w+)\s*=\s*(\w+)\s*;)");
    for (std::sregex_iterator it(source.begin(), source.end(), block), end; it != end; ++it)
    {
        SamplerStates st = { D3DTEXF_POINT, D3DTEXF_POINT, D3DTEXF_NONE, D3DTADDRESS_CLAMP, D3DTADDRESS_CLAMP };
        const std::string body = (*it)[2];
        for (std::sregex_iterator a(body.begin(), body.end(), assign); a != end; ++a)
        {
            const std::string key = lower((*a)[1]), value = lower((*a)[2]);
            const DWORD filter = value == "linear" ? D3DTEXF_LINEAR : value == "none" ? D3DTEXF_NONE : D3DTEXF_POINT;
            const DWORD address = value == "wrap" ? D3DTADDRESS_WRAP : value == "mirror" ? D3DTADDRESS_MIRROR : value == "border" ? D3DTADDRESS_BORDER : D3DTADDRESS_CLAMP;
            if (key == "minfilter") st[0] = filter;
            else if (key == "magfilter") st[1] = filter;
            else if (key == "mipfilter") st[2] = filter;
            else if (key == "addressu") st[3] = address;
            else if (key == "addressv") st[4] = address;
        }
        out[(*it)[1]] = st;
    }
}


class PostFxResource
{
public:

    ID3DXEffect* AOEffect = nullptr;

    // --------- load --------- 
    IDirect3DTexture9* SMAA_areaTex = nullptr; // loaded from file
    IDirect3DTexture9* SMAA_searchTex = nullptr; // loaded from file

    // --------- game textures --------- 
    IDirect3DTexture9* FullScreenTex = nullptr; // game hdr texture
    IDirect3DTexture9* HalfScreenTex = nullptr; // game half res screen texture
    // IDirect3DTexture9* pQuarterHDRTex = nullptr; //  game 1/4 res screen texture
    // IDirect3DTexture9* CascadeAtlasTex = nullptr;
    // IDirect3DSurface9* CascadeAtlasSurf = nullptr;

    // IDirect3DTexture9* NormalTex = nullptr;
    IDirect3DTexture9* DiffuseTex = nullptr;
    IDirect3DTexture9* SpecularTex = nullptr;
    IDirect3DTexture9* DepthTex = nullptr;
    // IDirect3DTexture9* StencilTex = nullptr;
    // IDirect3DTexture9* BloomTex = nullptr;
    // IDirect3DTexture9* CurrentLumTex = nullptr;


    //------- full screen ---------
    // postfx textures created
    rage::grcRenderTargetPC* FullScreenTex_temp1 = nullptr; // main temp texture
    rage::grcRenderTargetPC* FullScreenTex_temp2 = nullptr; // main temp texture

    //rage::grcRenderTargetPC* pShadowBlurTex1 = nullptr; // main shadow temp texture
    //rage::grcRenderTargetPC* pShadowBlurTex2 = nullptr; // main shadow temp texture

    // smaa textures
    rage::grcRenderTargetPC* edgesTex = nullptr; // smaa gen
    rage::grcRenderTargetPC* blendTex = nullptr; // smaa gen

    // temp set and used in postfx
    IDirect3DTexture9* renderTargetTex = nullptr;
    IDirect3DTexture9* textureRead = nullptr;
    IDirect3DSurface9* renderTargetSurf = nullptr;
    IDirect3DSurface9* surfaceRead = nullptr;

    rage::grcRenderTargetPC* AOCamDepthTex = nullptr;
    rage::grcRenderTargetPC* AOTex = nullptr;
    rage::grcRenderTargetPC* AOBlurTex = nullptr;
    std::vector<IDirect3DSurface9*> AOCamDepthSurf = {};
    IDirect3DSurface9* AOSurf = nullptr;
    IDirect3DSurface9* AOBlurSurf = nullptr;
    // GTAO's accumulation over frames (TemporalAO_PS in AO.fx): each frame blends this frame's
    // GTAO with the previous accumulation into the other target of the pair, before the blur.
    rage::grcRenderTargetPC* AOAccumTex[2] = {};
    IDirect3DSurface9* AOAccumSurf[2] = {};
    int nAOAccumIndex = 0;
    uint32_t nAOAccumFrame = 0; // FrameHistory::Frame() of AOAccumTex[nAOAccumIndex], 0 if none
    bool AOEnabled = true;

    // Pre alpha pass depth texture copy
    rage::grcRenderTargetPC* PreAlphaDepthCopyRT = nullptr;
    // This frame's depth at the render size, copied at the end of the G-buffer pass (CopySceneDepth) for the passes
    // of the lighting phase, see LightingDepth; the frame it was copied in.
    rage::grcRenderTargetPC* SceneDepthTex = nullptr;
    IDirect3DSurface9* SceneDepthSurf = nullptr;
    uint32_t nSceneDepthFrame = 0;

    //-------- half resolution screen --------------
    rage::grcRenderTargetPC* FullScreenDownsampleTex = nullptr; // main downsampled texture
    rage::grcRenderTargetPC* FullScreenDownsampleTex2 = nullptr; // main downsampled texture


    // game render targets
    // rage::grcRenderTargetPC* mSpecularAoRT = nullptr;
    rage::grcRenderTargetPC* mNormalRT = nullptr;
    rage::grcRenderTargetPC* mDiffuseRT = nullptr;
    rage::grcRenderTargetPC* mSpecularRT = nullptr;
    rage::grcRenderTargetPC* mDepthRT = nullptr;
    // rage::grcRenderTargetPC* mStencilRT = nullptr;
    rage::grcRenderTargetPC* mFullScreenRT = nullptr;
    // rage::grcRenderTargetPC* mFullScreenRT2 = nullptr;
    rage::grcRenderTargetPC* mHalfScreenRT = nullptr;
    // rage::grcRenderTargetPC* mCascadeAtlasRT = nullptr;
    // rage::grcRenderTargetPC* mQuarterScreenRT = nullptr;
    // rage::grcRenderTargetPC* mBloomRT = nullptr;
    // rage::grcRenderTargetPC* mCurrentLum = nullptr;


    // surfaces
    IDirect3DSurface9* FullScreenSurface = nullptr;
    IDirect3DSurface9* FullScreenSurface_temp1 = nullptr;
    IDirect3DSurface9* FullScreenSurface_temp2 = nullptr;
    IDirect3DSurface9* FullScreenDownsampleSurf = nullptr;
    IDirect3DSurface9* FullScreenDownsampleSurf2 = nullptr;
    IDirect3DSurface9* backBuffer = nullptr;
    IDirect3DSurface9* edgesSurf = nullptr;
    IDirect3DSurface9* blendSurf = nullptr;
    //IDirect3DSurface9* pShadowBlurSurf1 = nullptr;
    //IDirect3DSurface9* pShadowBlurSurf2 = nullptr;

    IDirect3DSurface9* PreAlphaDepthSurface = nullptr;
    IDirect3DSurface9* HDRFullScreenSurface = nullptr;

    // shaders
    IDirect3DPixelShader9* FxaaPS = nullptr;
    IDirect3DPixelShader9* CAS_PS = nullptr; // Sharpening
    IDirect3DPixelShader9* CASMasked_PS = nullptr; // Sharpening of the shown frames with frame generation

    IDirect3DPixelShader9* SSDraw_PS = nullptr;
    IDirect3DPixelShader9* SSAdd_PS = nullptr;
    IDirect3DPixelShader9* SSPrepass_PS = nullptr;
    // IDirect3DPixelShader9* SSAO_gen_ps = nullptr;
    // IDirect3DPixelShader9* SSAO_blend_ps = nullptr;


    IDirect3DPixelShader9* dof_blur_ps = nullptr;
    IDirect3DPixelShader9* dof_coc_ps = nullptr;

    IDirect3DPixelShader9* depth_of_field_tent_ps = nullptr;

    IDirect3DPixelShader9* stipple_filter_ps = nullptr;

    IDirect3DPixelShader9* SMAA_EdgeDetection = nullptr;
    IDirect3DPixelShader9* SMAA_BlendingWeightsCalculation = nullptr;
    IDirect3DPixelShader9* SMAA_NeighborhoodBlending = nullptr;
    IDirect3DVertexShader9* SMAA_EdgeDetectionVS = nullptr;
    IDirect3DVertexShader9* SMAA_BlendingWeightsCalculationVS = nullptr;
    IDirect3DVertexShader9* SMAA_NeighborhoodBlendingVS = nullptr;

    IDirect3DPixelShader9* Blit_PS = nullptr;

    //IDirect3DPixelShader9* DeferredShadowGen_ps = nullptr;
    //IDirect3DPixelShader9* DeferredShadowBlurH_ps = nullptr;
    //IDirect3DPixelShader9* DeferredShadowBlurV_ps = nullptr;
    //IDirect3DPixelShader9* DeferredShadowBlurCircle_ps = nullptr;
    // IDirect3DPixelShader9* deferred_lighting_PS1 = nullptr;
    // IDirect3DPixelShader9* deferred_lighting_PS2 = nullptr;

    // IDirect3DPixelShader9* CascadeAtlasGen = nullptr;

    //std::unordered_map<IDirect3DPixelShader9*, int> ShaderListPS;
    //std::unordered_map<IDirect3DVertexShader9*, int> ShaderListVS;

    bool EnablePostfx = false;

    // 0 off, 1 horizontal, 2 vertical, 3 horizontal e vertical.
    //int useScreenSpaceShadowsBlur = 3;
    bool useStippleFilter = true;

    bool shadersLoaded = false;

    bool bEnablePreAlphaDepth = false;

    ID3DXEffect* SSREffect = nullptr;
    rage::grcRenderTargetPC* SSRTex = nullptr;
    IDirect3DSurface9* SSRSurf = nullptr;
    rage::grcRenderTargetPC* SSRHistoryTex = nullptr;
    IDirect3DSurface9* SSRHistorySurf = nullptr;
    bool bSSRValidThisFrame = false;
    D3DXVECTOR4 SSRReprojRows[4] = {};
    bool bSSRReprojValid = false;
    // Set once the fog pass has copied this frame's scene into SSRHistoryTex; SSR runs before
    // that and sees last frame's, water may run after it.
    bool bSSRHistoryThisFrame = false;
    // FrameHistory::Frame() of the scene in SSRHistoryTex, 0 if none; indirect light reprojects it.
    uint32_t nSSRHistoryFrame = 0;

    // Copies of the scene right before and right after CWater::Render. They differ only
    // where water was drawn, which limits the water reflection pass to real water.
    // D3DPOOL_DEFAULT textures in the render target's own format, so released on device loss.
    IDirect3DTexture9* WaterMaskTex[2] = {};
    D3DSURFACE_DESC WaterMaskDesc = {};
    bool bWaterMaskCaptured = false;
    bool bWaterDoneThisFrame = false; // reset by the SSR pass, which runs before any water
    void ReleaseWaterMask()
    {
        SAFE_RELEASE(WaterMaskTex[0]);
        SAFE_RELEASE(WaterMaskTex[1]);
        WaterMaskDesc = {};
        bWaterMaskCaptured = false;
    }
    struct
    {
        D3DXHANDLE DepthTex2D, HistoryTex2D, SpecularTex2D, SurfaceTex2D;
        D3DXHANDLE NormalTex2D, SSRResultTex2D, DebugTex2D, fDebugMode, techSSRDebug, techSSRDebugCopy;
        D3DXHANDLE fUseGBufferNormals;
        D3DXHANDLE PreWaterTex2D, PostWaterTex2D, fUseWaterMask, PrevDepthTex2D, fUsePrevDepth, vec2PrevDepthRange;
        D3DXHANDLE fDenoiseRadius, fDenoiseSSROnly, techSSRDenoise, fPassThinObjects, fStepJitter, fTowardCamera, fReflectionBlur, fDistanceFade, fFallback, fSpreadRadius;
        D3DXHANDLE vec4SunView, fCSLength, fCSThickness, fCSMaxViewDistance, fCSIntensity, techContactShadows;
        D3DXHANDLE techContactTemporal, vec2NoiseOffset, techContactUpsample;
        D3DXHANDLE vec2InvViewportSize, fNearPlane, fFarDivNear, vec4ProjInfo;
        D3DXHANDLE fMaxDistance, fThickness, fEdgeFade, fIntensity;
        D3DXHANDLE vec4ViewToPrevClip, fGlossBoost, fGlossCutoff, fWetness, fWetGroundBoost;
        D3DXHANDLE vec4WaterPlane, fWaterIntensity, fWaterBlur;
        D3DXHANDLE fWaterNormalStrength, vec4WaterToView, vec4WaterWorldX, vec4WaterWorldY, vec4WaterRings;
        D3DXHANDLE techSSR, techSSRWater;
        D3DXHANDLE SSRAccumTex2D, fTemporalBlend, techSSRTemporal, SSRFallbackTex2D, SSRHitDistTex2D;
        D3DXHANDLE MarchDepthTex2D, fStepsPerPixel, fMarchFullDepth;
        D3DXHANDLE MotionTex2D, fUseMotion, vec2MotionJitter;
        D3DXHANDLE fTemporalAnySurface, fGIRayLength, fGIThickness, fGIMaxViewDistance, fGIIntensity, techSSGI;
        D3DXHANDLE fGIMaxBrightness, techGIUpsample, AlbedoTex2D, AlbedoLinearTex2D, GIPrevTex2D, fGIFeedback, fGIOcclusion, fGIRespectAO;
        D3DXHANDLE SceneTex2D, SkinIDTex2D, SkinLightTex2D, vec4SkinStep, fSkinStrength;
        D3DXHANDLE techSkinLight, techSkinScatter, techSkinScatterFinal, techSkinDebug;
    } SSREffectHandles = {};

    // PREF_SSR: 0 off, 1 half resolution, 2 full resolution.
    static bool SSREnabled() { static auto p = FusionFixSettings.GetRef("PREF_SSR"); return p && p->get() != 0; }
    static bool SSRHalfRes() { static auto p = FusionFixSettings.GetRef("PREF_SSR"); return p && p->get() == 1; }
    // SSR debug view from the graphics menu (PREF_SSR_DEBUG), see SSRDebug_PS in SSR.fx.
    // Built after the SSR pass and shown over the finished frame.
    // Contact shadows (ContactShadows_PS in SSR.fx), bound to s9 for deferred_lighting
    // (shaders/patches/deferred_lighting_contact_shadows.patch) and unbound right after it.
    static bool ContactShadowsEnabled() { static auto p = FusionFixSettings.GetRef("PREF_CONTACTSHADOWS"); return p && p->get() != 0; }
    // Extra Night Shadows at its last setting: lampposts, headlights and vehicle night shadows.
    static bool AllNightShadowsEnabled()
    {
        static auto p = FusionFixSettings.GetRef("PREF_EXTRANIGHTSHADOWS");
        return p && p->get() == FusionFixSettings.ExtraNightShadowsText.eLampHeadlVNS;
    }
    float fContactShadowLength = 0.3f;
    float fContactShadowThickness = 0.15f;
    // Screen space indirect light (SSGI_PS in SSR.fx), at half resolution: marched into
    // GIRawTex, smoothed into GIDenoisedTex, accumulated over frames into one of GIAccumTex, and
    // bound to s8 for deferred_lighting (shaders/patches/deferred_lighting_ssgi.patch), which
    // adds it to its ambient term. While it is off s8 gets a black texture, so the sampler never
    // shows the lighting whatever another shader left there.
    static bool SSGIEnabled() { static auto p = FusionFixSettings.GetRef("PREF_SSGI"); return p && p->get() != 0; }
    static constexpr int kGIDebugMode = 8;
    // What the accumulation of indirect light does with each pixel, see GIHistoryDebug in SSR.fx.
    static constexpr int kGIHistoryDebugMode = 10;
    float fGIIntensity = 1.5f;
    float fGIMaxBrightness = 4.0f;
    // How much of the ambient the indirect light takes the place of where its rays hit (alpha of
    // GIResult, see SSGI_PS); 0 adds it on top of the full ambient.
    float fGIOcclusion = 1.0f;
    float fGIRayLength = 4.0f;
    float fGIThickness = 0.5f;
    float fGIMaxDistance = 60.0f;
    float fGITemporalBlend = 0.9f;
    int nGIRays = 4;
    int nGISteps = 8;
    rage::grcRenderTargetPC* GIRawTex = nullptr;
    IDirect3DSurface9* GIRawSurf = nullptr;
    rage::grcRenderTargetPC* GIDenoisedTex = nullptr;
    IDirect3DSurface9* GIDenoisedSurf = nullptr;
    rage::grcRenderTargetPC* GIAccumTex[2] = {};
    IDirect3DSurface9* GIAccumSurf[2] = {};
    // The accumulation brought to full resolution with the depth in mind (GIUpsample_PS).
    rage::grcRenderTargetPC* GIFullTex = nullptr;
    IDirect3DSurface9* GIFullSurf = nullptr;
    int nGIAccumIndex = 0;
    uint32_t nGIAccumFrame = 0;     // FrameHistory::Frame() of GIAccumTex[nGIAccumIndex], 0 if none
    // What deferred_lighting gets this frame, null while there is none.
    IDirect3DTexture9* GIResult = nullptr;
    bool bGIBound = false;
    // mMaterialIdRT on s11 during lighting, for skin in the light volume shaders.
    bool bMaterialIdBound = false;
    bool bSpecularBound = false;

    // Light scattering under the skin (SkinScatter_PS in SSR.fx), as the fog pass begins: the
    // light on skin with its view depth into SkinLightTex[0], blurred along x into [1], and along
    // y, with the rest of the scene, back into [0], which the fog pass reads instead of the scene.
    // Skin is where shaders/patches/ped_skin_scattering_mask.patch puts a quarter step on the
    // material ID the G-buffer pass writes to _STENCIL_BUFFER_: the skin shaders always, gta_ped
    // for the HEAD and HAND components (InstallPedSkinHooks).
    static bool SkinScatteringEnabled() { static auto p = FusionFixSettings.GetRef("PREF_SKIN_SSS"); return p && p->get() != 0; }
    float fSkinScatteringWidth = 0.03f;
    float fSkinScatteringStrength = 1.0f;
    // The light on skin (c201 and c205; deferred_lighting_sun_on_skin.patch for the sun,
    // local_light_on_skin.patch for lamps and headlights): wraps past the terminator by
    // SkinLighting times 0.35 in red, 0.25 in green and 0.2 in blue, and in the sun's penumbra red
    // goes SkinLighting times a fifth of the way to the square root of the shadow. The channels
    // stay close: past where green and blue end only red is lit, and with 0.5, 0.2 and 0.1 that
    // band reached a fifth of full light and turned the dark side of faces red.
    float fSkinLighting = 1.0f;
    // Materials with no specular map (c197.x; deferred_lighting_sun_sheen.patch) write no specular
    // intensity, so buildings and LOD roads got neither the sun's highlight nor the sky's
    // reflection. The sun pass gives them half this much of one, as if the G-buffer held it,
    // times the square of one less their colour's saturation and faded out on dark colours, and so
    // do lamps and headlights (local_light_specular_sheen_and_fade.patch), for their highlights.
    // They are told apart by the gloss 258 / 1023 they write (world_no_specular_mark.patch).
    float fSpecularSheen = 0.1f;
    // Highlights of lamps, headlights and the sun (c165, c200, c206; local_light_specular_ggx.patch):
    // the game's are pow(R.L, n), the same peak at every gloss and nothing past the lobe. LightsGGX
    // swaps in a GGX lobe of the same width with a height correlated Smith term, so glossy surfaces
    // get a bright core with a long soft tail, times that strength; 0 keeps the game's everywhere.
    // - LightsGGXFresnel: how far the reflectance rises from 0.125 head on towards 1 at grazing angles.
    // - LightsGGXSize: the lights' radius in metres, which widens the lobe by its angle, so small
    //   lamps leave no tiny specks.
    // - LightsGGXStretch: the lobe is that much wider along the light projected onto the surface,
    //   so wet roads streak towards lamps.
    // - LightsGGXHeadlights: half the spacing of a car's lamps. The game lights both with one light
    //   between them; its highlight is taken as two, that far either way along the car's right, so
    //   a wet road shows a streak from each lamp.
    // - LightsGGXFillLights: lights the game draws with no highlight at all (fillerVolumePoint) get
    //   this much of one, from the G-buffer's specular on s13.
    // - LightsGGXSun: the sun's highlight the same way, 0 keeps the game's.
    // - LightsGGXEnvironment: on rough surfaces (gloss under about 0.25: concrete, plaster) the sky's
    //   reflection takes its Fresnel from the split sum environment BRDF, so they stop shining at
    //   grazing angles; asphalt and anything glossier, and the reflection's blur, stay the game's.
    //   0 keeps the game's, values between blend.
    float fLightsGGX = 1.0f;
    // GGX Lighting in the graphics menu (PREF_GGX_LIGHTING, [POSTFX] GGXLighting): off keeps the game's
    // highlights everywhere, the lamps', the sun's and the sky reflection's alike.
    bool GGXLightingEnabled() const
    {
        static auto p = FusionFixSettings.GetRef("PREF_GGX_LIGHTING");
        return !p || p->get() != 0;
    }
    float fLightsGGXFresnel = 0.5f;
    float fLightsGGXSize = 0.05f;
    float fLightsGGXStretch = 0.5f;
    float fLightsGGXHeadlights = 0.65f;
    float fLightsGGXFillLights = 0.5f;
    float fLightsGGXSun = 1.0f;
    float fLightsGGXEnvironment = 1.0f;
    // - LightsGGXMax: the most a GGX highlight may reach, in the game's own peak (its pow(R.L, n) at 1).
    //   GGX keeps the energy, so a narrow lobe peaks far above it: at 8, normal mapped clothes, skin and
    //   wood burnt into white patches and grazing edges into white strips.
    //   The highlight also fades out as the surface turns away from the camera (N.V under 0.08,
    //   local_light_specular_sheen_and_fade.patch), where normal mapped pixels sparkled.
    float fLightsGGXMax = 2.0f;
    // - LightsGGXSoft (c183, c184.x; local_light_specular_soft_lobe.patch): 0 the lobe above, 1 a softer
    //   look: as wide as a Blinn lobe of the same exponent 1.3 times as rough, widened by 0.2 more at any
    //   distance, and its peak held at the game's whatever the gloss, so rough surfaces get a broad
    //   glow and glossy ones no brighter core. Values between blend.
    // - LightsGGXStretchView (c184.y): 0 stretches the lobe along the light projected onto the surface,
    //   1 along the view, so streaks on wet roads run straight towards the camera. Both agree at the
    //   highlight's centre; only its tail turns.
    float fLightsGGXSoft = 0.0f;
    float fLightsGGXStretchView = 1.0f;
    // - LightsGGXGlints (HeadlightGlints.fx, RenderHeadlightGlints): the lamps of headlights seen in
    //   glossy surfaces from any side but behind, not only inside their beam; 0 none. LightsGGXGlintsSize
    //   the lamps' radius in metres, LightsGGXGlintsGloss the gloss from which surfaces glint,
    //   LightsGGXGlintsMax the ceiling of the GGX lobe before Fresnel.
    float fLightsGGXGlints = 1.0f;
    float fLightsGGXGlintsSize = 0.1f;
    float fLightsGGXGlintsGloss = 0.5f;
    float fLightsGGXGlintsMax = 64.0f;
    // The headlights the light loop met this lighting pass (InstallLocalContactLightHook), of which the
    // kGlintLights nearest the camera glint; what the last pass did, for the Ctrl+Shift+F10 log.
    static constexpr uint32_t kGlintCandidates = 64;
    static constexpr uint32_t kGlintLights = 16;
    struct GlintLight
    {
        float position[3];
        float direction[3];
        float colour[3];
        float radius;
        float distance;
        bool twin;
    };
    GlintLight GlintCandidates[kGlintCandidates] = {};
    uint32_t nGlintCandidates = 0;
    bool bGlintsDone = false;
    const char* szGlintsStatus = "not drawn yet";
    uint32_t nGlintsLastLights = 0;
    float GlintsLastIntensity[2] = {};
    float GlintsLastCone[2] = {};
    // Wet ground (WetGround.fx): WetGround the strength, 0 off. WetGroundPuddles the share of flat
    // ground under water at full wetness, WetGroundPuddleSize the metres one tile of the puddle map
    // takes, WetGroundRipples the rain's rings in them, WetGroundDarkening how much darker wet
    // surfaces turn. WetGroundMaterials a bit per material category (the material ID less its 128 and
    // 8 bits) that gets wet; WetGroundDebug 1 shows the categories, 2 wetness, puddles and rings.
    float fWetGround = 1.0f;
    // Wet Weather in the graphics menu (PREF_WET_WEATHER, [POSTFX] WetWeather): off leaves the ground
    // dry and the water without the rain's rings.
    bool WetWeatherEnabled() const
    {
        static auto p = FusionFixSettings.GetRef("PREF_WET_WEATHER");
        return !p || p->get() != 0;
    }
    float fWetGroundPuddles = 0.35f;
    float fWetGroundPuddleSize = 24.0f;
    float fWetGroundRipples = 1.0f;
    float fWetGroundDarkening = 1.0f;
    float fWetGroundWetting = 30.0f;
    float fWetGroundDrying = 240.0f;
    int nWetGroundMaterials = 1;
    int nWetGroundDebug = 0;
    // c206 as last set for a light, so lights of the same shape set nothing; the headlights found
    // since the last Ctrl+Shift+F10 log, and the lights looked at.
    float LightGGXShape[4] = {};
    uint32_t nLightGGXHeadlights = 0;
    uint32_t nLightGGXLights = 0;
    // Cloud shadows on the ground (c197.y-w, c198, c199, s12; deferred_lighting_sun_under_clouds.patch):
    // the ray from a surface towards the sun meets a cloud deck CloudShadowsHeight up, and the sun is
    // dimmed by up to CloudShadows where the clouds cover it there. The sky's clouds are on a dome
    // at infinity and cannot cast a real shadow, so the deck has its own noise, CloudShadowsScale
    // metres a tile, drifting CloudShadowsWind metres a second; its coverage is the game's own
    // cloud threshold, bias and thickness, so it follows the weather and the timecycle.
    float fCloudShadows = 0.6f;
    float fCloudShadowsHeight = 1200.0f;
    float fCloudShadowsScale = 16000.0f;
    float fCloudShadowsWind = 6.0f;
    float fCloudShadowsSoftness = 3.0f;
    // Added to the deck's coverage before the game's thickness curve: above 0 more of the sky
    // casts a shadow, below 0 less.
    float fCloudShadowsCoverage = 0.0f;
    // CloudShadowsDebug: 1 the whole ground in cloud shadow, to see whether the shadows reach the
    // sun light at all; 2 the raw noise as the coverage, to see whether the sun reads the noise.
    int nCloudShadowsDebug = 0;
    float CloudShadowConsts[12] = {};
    // The game's cloud values the last lighting pass used, for the log Ctrl+Shift+F10 writes.
    float fCloudLastThreshold = 0.0f, fCloudLastBias = 0.0f, fCloudLastThickness = 0.0f;
    bool bCloudLastFromGame = false;
    // The cloud deck's noise offset the lighting pass drifted it to this frame (c198.xy), which the
    // volumetric clouds take too, so each shadow lies under its cloud.
    float fCloudWindX = 0.0f, fCloudWindY = 0.0f;
    double fCloudSeconds = 0.0;

    // Volumetric clouds (Clouds.fx, RenderVolumetricClouds): one layer, VolumetricCloudsBase metres
    // up and VolumetricCloudsThickness deep, marched before the fog pass. Its coverage is the cloud
    // shadows' noise at their scale and wind, so while they are on the shadows follow these clouds
    // instead of the game's threshold and bias. Colours come from the game's own clouds.
    bool bVolumetricClouds = true;
    float fVolumetricCloudsCoverage = 0.4f;
    float fVolumetricCloudsBase = 800.0f;
    float fVolumetricCloudsThickness = 600.0f;
    float fVolumetricCloudsDensity = 0.03f;
    float fVolumetricCloudsDetail = 0.6f;
    float fVolumetricCloudsDetailScale = 1300.0f;
    float fVolumetricCloudsHaze = 25000.0f;
    float fVolumetricCloudsMaxDistance = 40000.0f;
    float fVolumetricCloudsBrightness = 1.0f;
    float fVolumetricCloudsSunTint = 0.6f;
    // The moon's light on the clouds once the sun is down, against the sun's; and how much the
    // clouds' shaded side takes the hue of the sky above it.
    float fVolumetricCloudsMoonlight = 0.2f;
    float fVolumetricCloudsSkyLight = 0.5f;
    // The least of the sun's light any part of a cloud keeps, however deep in its shadow: lighter,
    // airier bases than the light's march alone gives.
    float fVolumetricCloudsMinLight = 0.25f;
    // The clouds' sunlit side against the sky behind them, in times its brightness.
    float fVolumetricCloudsSkyMatch = 2.0f;
    bool bVolumetricCloudsWeather = true;
    float fVolumetricCloudsVanilla = 0.0f;
    float fVolumetricCloudsTranslucency = 0.25f;
    float fVolumetricCloudsEvolution = 1.0f;
    float fVolumetricCloudsSaturation = 1.0f;
    float fVolumetricCloudsMottle = 0.4f;
    // The shaded side and the bases against the game's cloud colour, and how much of the view's
    // extinction the sun's light takes inside a cloud: the clouds' contrast.
    float fVolumetricCloudsShade = 0.65f;
    float fVolumetricCloudsAbsorption = 0.35f;
    int nVolumetricCloudsDebug = 0;
    // The clouds in the reflection map (water, mirrors), at this brightness against the clouds.
    bool bVolumetricCloudsReflections = true;
    float fVolumetricCloudsReflectionBrightness = 1.0f;

    // The cloud layer this frame (UpdateCloudLayer): from the weather's preset, blended through the
    // game's weather change, or from the VolumetricClouds* settings with VolumetricCloudsWeather 0.
    struct CloudLayer
    {
        float coverage, base, thickness, density, stratus, wind;
        // Against VolumetricCloudsAbsorption, VolumetricCloudsTranslucency and VolumetricCloudsDetail;
        // and the glow's strength around the sun.
        float absorption, translucency, detail, glow;
        // How much brighter the cloud near the sun in the sky is, all of it, the thick middle too.
        float sunPower;
        // How fast the clouds reshape, against VolumetricCloudsEvolution, and how round their bases'
        // edges are.
        float evolution, baseRound;
        // The clouds' brightness against VolumetricCloudsSkyMatch: overcast clouds are grey, not
        // brighter than the sky.
        float skyMatch;
        // How softly the density rises inside a cloud, 0 to 1: at 1 the edges thin out into smoke
        // over a deeper band (the density's compressor from 1 to 3 instead of 3 to 12).
        float softness;
    };
    CloudLayer Cloud = { 0.4f, 800.0f, 600.0f, 1.0f, 0.0f, 1.0f, 1.0f, 1.0f, 1.0f, 6.0f, 0.5f, 1.0f, 0.5f, 1.0f, 0.0f };
    // How far the wind has carried the coverage and the detail, in tiles, and how far the detail
    // has drifted up through itself; summed frame by frame, as the wind changes with the weather.
    double fCloudDrift = 0.0, fCloudDetailDrift = 0.0, fCloudEvolution = 0.0, fCloudLastSeconds = -1.0;
    // The coverage map's slow morph (Clouds.fx's Morph): its phase in radians, which moves on by
    // VolumetricCloudsEvolution at 0.03 a second, kept within 10 pi, where both its waves repeat.
    double fCloudMorph = 0.0;
    bool bCloudDriftSeeded = false;
    float CloudMorphPhase() const { return static_cast<float>(fCloudMorph); }
    // The morph swings the map by up to this many metres; the weather map, read 12.5 times larger
    // than the coverage map, moves the cover by up to this share either way and the heaps' height
    // by a quarter.
    static constexpr float kCloudMorphReach = 250.0f;
    // The volumetric clouds' sunlit side against the game's CloudColor, which is the middle of its
    // own clouds' range: the shaded side is VolumetricCloudsShade of it, and most of what the eye
    // sees of a cloud lies in between.
    static constexpr float kCloudLitGain = 1.6f;
    static constexpr float kCloudWeatherReach = 0.45f;
    static constexpr float kCloudWeatherScale = 0.08f;
    void UpdateCloudLayer(double seconds);
    ID3DXEffect* CloudsEffect = nullptr;
    // Wet ground in the rain (WetGround.fx, RenderWetGround): copies of _DEFERRED_GBUFFER_0_ to _2_,
    // made afresh at the G-buffer's size and format whenever those change, which the pass reads
    // while it writes the G-buffer.
    ID3DXEffect* WetGroundEffect = nullptr;
    HRESULT hrWetGroundEffect = S_OK;
    ID3DXEffect* HeadlightGlintsEffect = nullptr;
    HRESULT hrHeadlightGlintsEffect = S_OK;
    IDirect3DTexture9* WetCopyTex[3] = {};
    IDirect3DSurface9* WetCopySurf[3] = {};
    // How wet the world is, 0..1: rises with the rain over WetGroundWetting seconds and dries over
    // WetGroundDrying once it stops; the time it was last brought up to date, in game seconds.
    float fWetness = 0.0f;
    double fWetnessTime = -1.0;
    // Whether the camera is in an interior, taken on the main thread as the frame's draw list is built:
    // there the G-buffer's vertex colour says nothing of the sky, and tunnels and rooms got puddles.
    bool bInteriorScene = false;
    const char* szWetGroundStatus = "not run yet";
    void ReleaseWetCopies()
    {
        for (int i = 0; i < 3; ++i)
        {
            SAFE_RELEASE(WetCopySurf[i]);
            SAFE_RELEASE(WetCopyTex[i]);
        }
    }
    // The clouds at half the render size: [0] this frame's march, [1] and [2] the accumulation,
    // which swap every frame; nCloudAccumIndex picks last frame's ([1 + index]).
    rage::grcRenderTargetPC* CloudTex[3] = {};
    IDirect3DSurface9* CloudSurf[3] = {};
    // The sky's brightness this frame, 1 x 1, which the clouds are matched to (CloudsSkyRef).
    rage::grcRenderTargetPC* CloudSkyRefTex = nullptr;
    IDirect3DSurface9* CloudSkyRefSurf = nullptr;
    // The frame it was last drawn in, 0 never: the reflections take the sky's hue from it.
    uint32_t nCloudSkyRefFrame = 0;
    // The march's sums at half size, which CloudsLight lights: (transmittance, sun, shade, silver)
    // and (glow, first hit).
    rage::grcRenderTargetPC* CloudMarchTex[2] = {};
    IDirect3DSurface9* CloudMarchSurf[2] = {};
    int nCloudAccumIndex = 0;
    uint32_t nCloudAccumFrame = 0; // FrameHistory::Frame() of the accumulation, 0 if none
    IDirect3DVolumeTexture9* CloudDetailTexture = nullptr;
    IDirect3DVolumeTexture9* CloudDetailTex();
    // Volumetric Clouds in the graphics menu (PREF_VOLUMETRIC_CLOUDS, the same VolumetricClouds key)
    // turns them on and off at once; the game's flat clouds and the cloud shadows follow.
    bool VolumetricCloudsEnabled() const
    {
        static auto p = FusionFixSettings.GetRef("PREF_VOLUMETRIC_CLOUDS");
        return p ? p->get() != 0 : bVolumetricClouds;
    }
    bool VolumetricCloudsOn() const { return VolumetricCloudsEnabled() && CloudsEffect != nullptr; }
    // Why the last frame drew no volumetric clouds, or that it did, for the Ctrl+Shift+F10 log.
    const char* szCloudsStatus = "not run yet";
    // The same for the reflection map (DrawSkyReflection): what the last call did, how many calls
    // since the last log, and the viewport and target it drew into.
    const char* szCloudsReflectionStatus = "never called";
    uint32_t nCloudReflectionCalls = 0;
    uint32_t nCloudWaterReflectionCalls = 0;
    // The water's rings, for the log: c178 as last set, the water draws since the last log, and
    // whether SSR's water pass last had the game's wave texture (none: a flat mirror, no rings).
    float WaterRingsLast[4] = {};
    uint32_t nWaterRingDraws = 0;
    int nWaterSsrSurface = -1;
    D3DVIEWPORT9 CloudReflectionViewport = {};
    UINT CloudReflectionTarget[2] = {};
    HRESULT hrCloudsEffect = S_OK;
    IDirect3DTexture9* CloudNoiseTexture = nullptr;
    IDirect3DTexture9* CloudNoiseTex();
    bool bCloudNoiseBound = false;
    bool bCloudNoiseSurvived = false;
    // s12's SRGBTEXTURE, MAXMIPLEVEL, MINFILTER and MIPMAPLODBIAS before the shadows set theirs, for the log.
    DWORD CloudSamplerBefore[4] = {};
    // The game's cloud parameters the cloud shadows take, registered at start (RegisterCloudParams):
    // registering while drawing would grow the list the shader parameter hook may be reading.
    size_t CloudThresholdIdx = 0, CloudBiasIdx = 0, CloudThicknessIdx = 0;
    size_t CloudColorIdx = 0, CloudExposureIdx = 0, CloudSunDirectionIdx = 0;
    size_t SunsetColorIdx = 0, CloudInscatteringIdx = 0, CloudSunColorIdx = 0, CloudExposureClampIdx = 0;
    size_t CloudMoonPositionIdx = 0, CloudSkyColorIdx = 0;
    // The lit and shaded colours the clouds were last drawn with, and the sky's clamp, for the log.
    float CloudLastLit[3] = {}, CloudLastShade[3] = {}, CloudLastClamp[3] = {}, CloudLastCeiling = 0.0f, CloudLastLightStrength = 0.0f;
    // The direction towards the game's own directional light (-c17, gDirectionalLight), taken in
    // the lighting phase, and the frame it was taken; the sky's SunDirection the clouds used to be
    // lit by came out mirrored across the sky. And the directions the clouds were last lit from.
    float CloudLightDir[3] = {};
    uint32_t nCloudLightFrame = 0;
    float CloudLastSkySun[3] = {}, CloudLastUsedSun[3] = {};
    // Which horizontal axes of the sky's directions run opposite to the world's, learnt by day from
    // the sun against the game's light, and put on the moon at night: the sky's MoonPosition goes
    // through the same remap as its SunDirection, which came out mirrored.
    float CloudSkyAxisSign[2] = { 1.0f, 1.0f };
    bool bCloudLastMoonlit = false;
    bool bCloudParamsRegistered = false;
    void RegisterCloudParams()
    {
        CloudThresholdIdx = rage::grmShaderInfo::registerShaderParam("gta_atmoscatt_clouds.fxc", "CloudThreshold");
        CloudBiasIdx = rage::grmShaderInfo::registerShaderParam("gta_atmoscatt_clouds.fxc", "CloudBias");
        CloudThicknessIdx = rage::grmShaderInfo::registerShaderParam("gta_atmoscatt_clouds.fxc", "CloudThicknessEdgeSmoothDetailScaleStrength");
        CloudColorIdx = rage::grmShaderInfo::registerShaderParam("gta_atmoscatt_clouds.fxc", "CloudColor");
        SunsetColorIdx = rage::grmShaderInfo::registerShaderParam("gta_atmoscatt_clouds.fxc", "SunsetColor");
        CloudInscatteringIdx = rage::grmShaderInfo::registerShaderParam("gta_atmoscatt_clouds.fxc", "CloudInscatteringRange");
        CloudSunColorIdx = rage::grmShaderInfo::registerShaderParam("gta_atmoscatt_clouds.fxc", "SunColor");
        CloudExposureClampIdx = rage::grmShaderInfo::registerShaderParam("gta_atmoscatt_clouds.fxc", "HDRExposureClamp");
        CloudExposureIdx = rage::grmShaderInfo::registerShaderParam("gta_atmoscatt_clouds.fxc", "HDRExposure");
        CloudSunDirectionIdx = rage::grmShaderInfo::registerShaderParam("gta_atmoscatt_clouds.fxc", "SunDirection");
        CloudMoonPositionIdx = rage::grmShaderInfo::registerShaderParam("gta_atmoscatt_clouds.fxc", "MoonPosition");
        CloudSkyColorIdx = rage::grmShaderInfo::registerShaderParam("gta_atmoscatt_clouds.fxc", "SkyColor");
        bCloudParamsRegistered = true;
    }
    static constexpr int kSkinDebugMode = 9;
    rage::grcRenderTargetPC* mMaterialIdRT = nullptr;
    rage::grcRenderTargetPC* SkinLightTex[2] = {};
    IDirect3DSurface9* SkinLightSurf[2] = {};
    // The lighting phase's camera, which the fog pass has not: |_11| and |_22| of the projection,
    // near and far clip; all 0 until the lighting phase has run.
    float SkinCamera[4] = {};
    float fContactShadowMaxDistance = 60.0f;
    float fContactShadowIntensity = 1.0f;
    bool bContactShadowStepJitter = true;
    // ContactShadowsHalfResolution: the march runs at half size into ContactRawHalfTex and
    // ContactUpsample_PS brings it to full size, weighing by depth, before the smoothing.
    bool bContactShadowsHalfRes = true;
    // Contact shadows from street lights and headlights, marched in the light shaders
    // themselves (shaders/patches/local_light_contact_shadows.patch); they follow the Contact
    // Shadows menu toggle.
    bool bLocalContactShadows = true;
    // From binding the lighting inputs to the end of deferred lighting, the light loop may turn
    // c203 off for a light and back on (see InstallLocalContactLightHook).
    bool bLocalContactPass = false;
    bool bLocalContactLightOff = false;
    // Lights without a shadow map this frame (most headlights, lamps left without a slot) light a car's surroundings all the same, so
    // their contact shadow is only a dark frame around it on a lit floor (tunnels): this much of it is kept
    float fLocalContactShadowUnshadowed = 0.0f;
    float fLocalContactLightIntensity = -1.0f;  // what c202.w holds for the light being drawn
    float fLocalContactShadowLength = 0.5f;
    float fLocalContactShadowThickness = 0.2f;
    float fLocalContactShadowMaxDistance = 40.0f;
    float fLocalContactShadowIntensity = 1.0f;
    // Vehicle Box Shadows in the graphics menu (PREF_VEHICLE_BOX_SHADOWS, [POSTFX] VehicleBoxShadows): cars shadowed as boxes
    // by the lights whose shadow maps hold no cars (see VehicleBoxShadows). The light's size widens the penumbra with
    // the way from the car to the ground; the rounding takes the box's edges off.
    bool VehicleBoxShadowsEnabled() const
    {
        static auto p = FusionFixSettings.GetRef("PREF_VEHICLE_BOX_SHADOWS");
        return !p || p->get() != 0;
    }
    float fVehicleBoxShadowLightSize = 0.5f;
    float fVehicleBoxShadowRounding = 0.3f;
    float fVehicleBoxShadowScale[3] = { 1.0f, 0.95f, 0.9f }; // length, width, height, of the model's bounds
    float fVehicleBoxShadowSelfMargin = 0.6f;
    float fVehicleBoxShadowLowLightNarrow = 1.0f;
    // Car lights (headlights, tail lights) shine low and close, so a box's shadow from them spreads over the whole road
    // ahead and shows its shape; off by default, lamps only.
    bool bVehicleBoxShadowsFromCarLights = false;
    // A lamp that takes a shadow slot, whose map then shows the cars, fades its boxes out over SlotFade ms, so the
    // shadow under a car does not jump between the two; WithSlots keeps them under its real shadow for good.
    bool bVehicleBoxShadowsWithSlots = false;
    uint32_t nVehicleBoxShadowSlotFadeMs = 600;
    // c202 ray length, thickness, max view distance and strength; c203 the main camera's _34 and
    // 12345 in w while they are on; c204 its _11, _22, _31, _32. Set right before lighting, as
    // the viewport hook runs for every view and the last before lighting is not the camera's.
    float LocalContactShadowConsts[12] = {};
    // The engine's own light shafts on street lights, as the snow season turns them on (see
    // OnAfterCopyLight in seasonal/snow.ixx), at all times: spot lights of 8 to 20 m that are
    // no vehicle light, traffic light or fire (0x398, see OnAfterCopyLight) get the shaft flag, VolumetricLightIntensity
    // and VolumetricLightScale, fading out towards VolumetricLightMaxDistance. When the snow
    // season gives the weather shafts of its own, its handler, which runs after this one, wins.
    // Volumetric Light in the graphics menu (PREF_VOLUMETRIC_LIGHT) turns them on and off.
    static bool VolumetricLight() { static auto p = FusionFixSettings.GetRef("PREF_VOLUMETRIC_LIGHT"); return p ? p->get() != 0 : true; }
    float fVolumetricLightIntensity = 4.0f;
    float fVolumetricLightScale = 0.25f;
    float fVolumetricLightMaxDistance = 100.0f;
    // Headlights too: spot lights carrying this flag, whatever their radius and other flags.
    // 0x100 is the vehicle beam bit the night shadow code goes by (ShadowAllocationRuntime);
    // 0 leaves headlights out.
    uint32_t nVolumetricLightHeadlightFlag = rage::LF_VEHICLE;
    float fVolumetricLightHeadlightIntensity = 2.0f;
    // Headlight shaft size in metres, radius times scale for the shaft mesh.
    float fVolumetricLightHeadlightLength = 25.0f;
    // Flags headlights get besides the shaft bit, none by default: the shaft draw loop (CE
    // 0xAC2A09) looks at no flag but the shaft bit and 0x10, so 0x1 made no difference.
    uint32_t nVolumetricLightHeadlightAddFlags = 0;
    // Headlight shafts with the headlight's shadow map, which leaves them unseen (see
    // InstallShaftHooks).
    bool bVolumetricLightHeadlightShadow = false;
    // Degrees headlight shafts are tilted down, the shaft alone (see InstallShaftHooks).
    float fVolumetricLightHeadlightPitch = 6.0f;
    // FillLights in the INI, off by default: off darkens the large exterior map lights, reaching
    // at least FillLightsMinRadius, that flood whole squares and building fronts. They made scenes
    // look washed out, and at some cell edges (a garage at x -900 in Algonquin) the game stops
    // sending them with a step of the camera.
    bool bFillLights = false;
    float fFillLightsMinRadius = 30.0f;
    rage::grcRenderTargetPC* ContactRawTex = nullptr;
    IDirect3DSurface9* ContactRawSurf = nullptr;
    rage::grcRenderTargetPC* ContactRawHalfTex = nullptr;
    IDirect3DSurface9* ContactRawHalfSurf = nullptr;
    rage::grcRenderTargetPC* ContactTex = nullptr;
    IDirect3DSurface9* ContactSurf = nullptr;
    // Accumulation over frames (ContactTemporal_PS in SSR.fx): each frame blends the smoothed
    // contact shadows with the previous accumulation into the other target of the pair, and
    // moves every pixel's step offset on. ContactShadowsTemporal is the share of the history
    // kept, 0 turns it off.
    rage::grcRenderTargetPC* ContactAccumTex[2] = {};
    IDirect3DSurface9* ContactAccumSurf[2] = {};
    int nContactAccumIndex = 0;
    uint32_t nContactAccumFrame = 0;    // FrameHistory::Frame() of ContactAccumTex[nContactAccumIndex], 0 if none
    float fContactTemporalBlend = 0.8f;
    // What deferred_lighting gets on s9: ContactTex, or the accumulation.
    IDirect3DTexture9* ContactResult = nullptr;
    bool bContactValid = false;
    bool bContactBound = false;
    static constexpr int kContactDebugMode = 7;
    static int SSRDebugMode() { static auto p = FusionFixSettings.GetRef("PREF_SSR_DEBUG"); return p ? p->get() : 0; }
    // The smoothed SSR result (SSRDenoise_PS) that deferred_lighting reads, when enabled.
    float fSSRDenoiseRadius = 2.0f;
    bool bSSRPassThinObjects = true;
    bool bSSRStepJitter = true;
    // ScreenSpaceReflectionsTemporalJitter: while SSR accumulates, the step offsets move on every
    // frame (vec2NoiseOffset in SSR.fx), so the accumulation averages them.
    bool bSSRTemporalJitter = true;
    // PostFxProfiler: GPU time of FusionFix's passes to GTAIV.EFLC.FusionFix.PostFx.log, see ProfilerNextFrame.
    bool bPostFxProfiler = false;
    float fSSRTowardCamera = 0.0f;
    float fSSRReflectionBlur = 0.0f;
    // ScreenSpaceReflectionsStepPixels: pixels of a ray on the march's target per step, 2 by default.
    float fSSRStepPixels = 2.0f;
    // ScreenSpaceReflectionsMarchFullDepth (temporary, live): the march steps through the full size
    // depth instead of SSRMarchDepthTex, to compare the two.
    bool bSSRMarchFullDepth = false;
    float fSSRDistanceFade = 0.0f;
    float fSSRFallback = 0.8f;
    rage::grcRenderTargetPC* SSRDenoisedTex = nullptr;
    IDirect3DSurface9* SSRDenoisedSurf = nullptr;
    bool bSSRDenoised = false;
    // The same pair at half the resolution, for the Half quality setting; deferred_lighting
    // reads it with bilinear filtering. Both pairs are kept, so switching needs no reset.
    rage::grcRenderTargetPC* SSRHalfTex = nullptr;
    IDirect3DSurface9* SSRHalfSurf = nullptr;
    rage::grcRenderTargetPC* SSRHalfDenoisedTex = nullptr;
    IDirect3DSurface9* SSRHalfDenoisedSurf = nullptr;
    // The passes of SSR before its result (technique SSR in SSR.fx), at full [0] and half [1]
    // resolution: the reflections the march found, colour and confidence in half floats as
    // SSRTex holds them, and what fills in the misses.
    rage::grcRenderTargetPC* SSRTraceTex[2] = {};
    IDirect3DSurface9* SSRTraceSurf[2] = {};
    rage::grcRenderTargetPC* SSRFallbackTex[2] = {};
    IDirect3DSurface9* SSRFallbackSurf[2] = {};
    // The other target of the fill's cascade (SSRSpread_PS), which takes turns with SSRFallbackTex.
    rage::grcRenderTargetPC* SSRSpreadTex[2] = {};
    IDirect3DSurface9* SSRSpreadSurf[2] = {};
    // How far the march's rays went (SSRTrace_PS's second target), for the accumulation to take
    // the history where the reflected image was; only while accumulating. 64 bits like the trace
    // target it is drawn with.
    rage::grcRenderTargetPC* SSRHitDistTex[2] = {};
    IDirect3DSurface9* SSRHitDistSurf[2] = {};
    // View depth at half the full size, the nearest of each 2x2 pixels (MarchDepth_PS), which the
    // march steps through at either SSR size; R32F.
    rage::grcRenderTargetPC* SSRMarchDepthTex = nullptr;
    IDirect3DSurface9* SSRMarchDepthSurf = nullptr;
    // Accumulation over frames (SSRTemporal_PS in SSR.fx): each frame blends the smoothed
    // result with the previous accumulation into the other target of a pair, one pair per
    // resolution. ScreenSpaceReflectionsTemporal is the share of the history kept, 0 turns
    // it off. The history is taken where the reflected image was last frame.
    // The pair is why lighting must get the result only once SSR is done, see BindSSRTexture.
    rage::grcRenderTargetPC* SSRAccumTex[2][2] = {}; // [half][ping-pong]
    IDirect3DSurface9* SSRAccumSurf[2][2] = {};
    int nSSRAccumIndex = 0;
    uint32_t nSSRAccumFrame = 0;    // FrameHistory::Frame() of SSRAccumTex[..][nSSRAccumIndex], 0 if none
    bool bSSRAccumHalf = false;
    float fSSRTemporalBlend = 0.85f;
    // What deferred_lighting gets this frame: one of the textures above.
    IDirect3DTexture9* SSRResult = nullptr;
    rage::grcRenderTargetPC* SSRDebugTex = nullptr;
    IDirect3DSurface9* SSRDebugSurf = nullptr;
    bool bSSRDebugValid = false;

    // Car glass reflections (shaders/patches/vehicle_glass_reflections.patch). Glass is drawn
    // after lighting and is not in the depth buffer, so the SSR buffer cannot reach it; the
    // patched gta_vehicle_vehglass shaders march their own reflected ray through the lit
    // opaque scene. They read, from samplers no game shader uses:
    //   s9  a 5x1 float texture: texel 0 projection _11, _22, _31, _32; texel 1 _34,
    //       thickness, ray length and a magic value, so a foreign texture is never used;
    //       texel 2 the camera's right axis and the debug flag; texel 3 its up axis (the axes
    //       come from here because those shaders overwrite gViewInverse's first two rows) and
    //       half the screen height, to count the steps a ray needs;
    //       texel 4 near and log2(far / near), to make the depth in s11 linear, the step
    //       jitter flag (ScreenSpaceReflectionsGlassStepJitter) and how far reflections
    //       pointing back at the camera reach (ScreenSpaceReflectionsTowardCamera, as in SSR)
    //   s11 PreAlphaDepthCopyRT and s13 SSRHistoryTex, the log depth (_DEFERRED_GBUFFER_3_)
    //       and the lit opaque scene, which the fog pass copies with draws before the alpha
    //       passes. The glass takes no copies of its own: a StretchRect of the scene target
    //       after lighting made foliage and glass tremble, and GBUFFER_3 itself is written
    //       by the passes the glass is drawn in.
    bool bGlassReflections = true;
    float fGlassReflectionsLength = 15.0f;
    float fGlassReflectionsThickness = 0.5f;
    // On by default: without it the edges of what the glass march caught show as shifted
    // slices; with it, as fine noise, since glass has no smoothing pass. Averaging pixel quads
    // in the shader with dsx and dsy left bright dots where the derivatives are per quad.
    bool bGlassStepJitter = true;
    // Dynamic and locked with D3DLOCK_DISCARD, so the upload each frame never waits for the GPU
    // to finish with last frame's; D3DPOOL_DEFAULT, so released on device loss.
    IDirect3DTexture9* GlassParamsTex = nullptr;
    float GlassParams[20] = {};
    bool bGlassFrameValid = false;
    bool bGlassBound = false;
    static constexpr int kGlassDebugMode = 6;
    bool bSSRGBufferNormals = true;

    // 1x1 transparent black for s3 when there is no SSR target. An empty sampler reads alpha
    // 1, which deferred_lighting would take as a full strength black reflection with its
    // horizon fade lifted. Managed, so it survives device resets.
    IDirect3DTexture9* TransparentTexture = nullptr;
    IDirect3DTexture9* TransparentTex()
    {
        if (!TransparentTexture)
        {
            auto pDevice = rage::grcDevice::GetD3DDevice();
            if (pDevice && SUCCEEDED(pDevice->CreateTexture(1, 1, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &TransparentTexture, nullptr)))
            {
                D3DLOCKED_RECT locked = {};
                if (SUCCEEDED(TransparentTexture->LockRect(0, &locked, nullptr, 0)))
                {
                    *static_cast<uint32_t*>(locked.pBits) = 0;
                    TransparentTexture->UnlockRect(0);
                }
            }
        }
        return TransparentTexture;
    }
    int nSSRSteps = 32;
    int nSSRRefineSteps = 8;
    float fSSRMaxDistance = 24.0f;
    float fSSRThickness = 0.3f;
    float fSSREdgeFade = 0.1f;
    float fSSRIntensity = 1.0f;
    float fSSRGlossBoost = 2.0f;
    float fSSRGlossCutoff = 0.5f;
    // ScreenSpaceReflectionsWetGround: while it rains, SSR on ground facing up that is under the
    // gloss cutoff, drawn this many times brighter (fWetGroundBoost in SSR.fx); 0 turns it off. The
    // wet ground pass gives only puddles water's specular: its film keeps about dry asphalt's (0.12),
    // so at 1 cars on wet roads reflected half as bright as at 2, and 2 was brighter than the scene.
    float fSSRWetGround = 1.5f;
    float fSSRWaterIntensity = 1.0f;
    // CWater::Render loads this as the Z of every flat water vertex, so it is the real
    // surface height rather than an assumed sea level.
    const float* pWaterLevel = nullptr;
    float fSSRWaterLevelOffset = 0.0f;
    float fSSRWaterBlur = 3.0f;
    float fSSRWaterNormalStrength = 1.0f;
    int nAmbientOcclusionSamples = 9;
    int nAmbientOcclusionBlurPasses = 1;
    int nAmbientOcclusionLogMaxOffset = 3;
    int nAmbientOcclusionMaxMipLevel = 5;
    float fAmbientOcclusionFarClip = 150.0f;

    float fAmbientOcclusionRadius = 1.125f;
    float fAmbientOcclusionBias = 0.03f;
    float fAmbientOcclusionIntensity = 0.4f;
    float fAmbientOcclusionBlurRadius = 2.0f;
    // GTAO (AO.fx, Ambient Occlusion: GTAO in the graphics menu): slices through the view
    // direction and steps per side.
    int nAmbientOcclusionGTAOSlices = 3;
    int nAmbientOcclusionGTAOSteps = 4;
    float fAmbientOcclusionGTAOStrength = 1.0f;
    float fAmbientOcclusionGTAOThinOccluders = 0.5f;
    float fAmbientOcclusionTemporal = 0.9f;  // share of last frames' GTAO kept, 0 turns accumulation off
    bool bAmbientOcclusionMultiBounce = true;

    struct
    {
        D3DXHANDLE AOTexture2D, AOCamDepthTexture2D, DepthTex2D, NormalTex2D;
        D3DXHANDLE vec4WorldToView, fUseNormals, fGTAOStrength, fThinOccluders, vec2NoiseOffset, fMultiBounce, AlbedoTex2D;
        D3DXHANDLE AOHistoryTex2D, PrevDepthTex2D, MotionTex2D, vec4ViewToPrevClip, fUseMotion, vec2MotionJitter, fUsePrevDepth, vec2PrevDepthRange, fTemporalBlend;

        D3DXHANDLE vec2InvViewportSize;
        D3DXHANDLE fNearPlane;
        D3DXHANDLE fFarPlane;
        D3DXHANDLE fFarDivNear;
        D3DXHANDLE fRadius;
        D3DXHANDLE fBias;
        D3DXHANDLE fIntensity;
        D3DXHANDLE fProjScale;
        D3DXHANDLE vec4ProjInfo;
        D3DXHANDLE vec2BlurDirection;
        D3DXHANDLE vec2PrevMipSize;
        D3DXHANDLE vec2PrevMipTexel;
        D3DXHANDLE iPreviousMip;
    } AOEffectHandles = {};

    bool loadShaders(LPDIRECT3DDEVICE9 pDevice, HMODULE hm)
    {
        ID3DXBuffer* bf1 = nullptr;
        ID3DXBuffer* bf2 = nullptr;
        ID3DXConstantTable* ppConstantTable = nullptr;

        // Lambda helper to load compiled shader bytecode from resource
        auto loadCompiledShader = [&](int resourceID, auto& shader) -> bool
        {
            HRSRC hRes = FindResourceW(hm, MAKEINTRESOURCEW(resourceID), RT_RCDATA);
            if (!hRes) return false;
            HGLOBAL hGlob = LoadResource(hm, hRes);
            if (!hGlob) return false;
            void* buffer = LockResource(hGlob);
            if (!buffer) return false;
            using ShaderType = std::remove_reference_t<decltype(shader)>;
            if constexpr (std::is_same_v<ShaderType, IDirect3DPixelShader9*>)
            {
                return pDevice->CreatePixelShader((DWORD*)buffer, &shader) == S_OK && shader;
            }
            else if constexpr (std::is_same_v<ShaderType, IDirect3DVertexShader9*>)
            {
                return pDevice->CreateVertexShader((DWORD*)buffer, &shader) == S_OK && shader;
            }
            return false;
        };

        //asm
        if (!dof_blur_ps)
        {
            if (D3DXAssembleShaderFromResourceW(hm, MAKEINTRESOURCEW(IDR_dof_blur_ps), NULL, NULL, 0, &bf1, &bf2) == S_OK)
            {
                if (pDevice->CreatePixelShader((DWORD*)bf1->GetBufferPointer(), &dof_blur_ps) != S_OK || !dof_blur_ps)
                    SAFE_RELEASE(dof_blur_ps);
                SAFE_RELEASE(bf1);
                SAFE_RELEASE(bf2);
            }
        }

        if (!dof_coc_ps)
        {
            if (D3DXAssembleShaderFromResourceW(hm, MAKEINTRESOURCEW(IDR_dof_coc_ps), NULL, NULL, 0, &bf1, &bf2) == S_OK)
            {
                if (pDevice->CreatePixelShader((DWORD*)bf1->GetBufferPointer(), &dof_coc_ps) != S_OK || !dof_coc_ps)
                    SAFE_RELEASE(dof_coc_ps);
                SAFE_RELEASE(bf1);
                SAFE_RELEASE(bf2);
            }
        }

        if (!depth_of_field_tent_ps)
        {
            if (D3DXAssembleShaderFromResourceW(hm, MAKEINTRESOURCEW(IDR_depth_of_field_tent_ps), NULL, NULL, 0, &bf1, &bf2) == S_OK)
            {
                if (pDevice->CreatePixelShader((DWORD*)bf1->GetBufferPointer(), &depth_of_field_tent_ps) != S_OK || !depth_of_field_tent_ps)
                    SAFE_RELEASE(depth_of_field_tent_ps);
                SAFE_RELEASE(bf1);
                SAFE_RELEASE(bf2);
            }
        }

        if (!stipple_filter_ps)
        {
            if (D3DXAssembleShaderFromResourceW(hm, MAKEINTRESOURCEW(IDR_stipple_filter_ps), NULL, NULL, 0, &bf1, &bf2) == S_OK)
            {
                if (pDevice->CreatePixelShader((DWORD*)bf1->GetBufferPointer(), &stipple_filter_ps) != S_OK || !stipple_filter_ps)
                    SAFE_RELEASE(stipple_filter_ps);
                SAFE_RELEASE(bf1);
                SAFE_RELEASE(bf2);
            }
        }

        //hlsl
        if (!SSDraw_PS)
        {
            if (D3DXCompileShaderFromResourceW(hm, MAKEINTRESOURCEW(IDR_SunShafts_PS), NULL, NULL, "SSDraw", "ps_3_0", 0, &bf1, &bf2, &ppConstantTable) == S_OK)
            {
                if (pDevice->CreatePixelShader((DWORD*)bf1->GetBufferPointer(), &SSDraw_PS) != S_OK || !SSDraw_PS)
                    SAFE_RELEASE(SSDraw_PS);
            }
            else
            {
                loadCompiledShader(IDR_SSDraw_PS_compiled, SSDraw_PS);
            }
            SAFE_RELEASE(bf1);
            SAFE_RELEASE(bf2);
            SAFE_RELEASE(ppConstantTable);
        }

        if (!SSPrepass_PS)
        {
            if (D3DXCompileShaderFromResourceW(hm, MAKEINTRESOURCEW(IDR_SunShafts_PS), NULL, NULL, "SSPrepass", "ps_3_0", 0, &bf1, &bf2, &ppConstantTable) == S_OK)
            {
                if (pDevice->CreatePixelShader((DWORD*)bf1->GetBufferPointer(), &SSPrepass_PS) != S_OK || !SSPrepass_PS)
                    SAFE_RELEASE(SSPrepass_PS);
            }
            else
            {
                loadCompiledShader(IDR_SSPrepass_PS_compiled, SSPrepass_PS);
            }
            SAFE_RELEASE(bf1);
            SAFE_RELEASE(bf2);
            SAFE_RELEASE(ppConstantTable);
        }

        if (!SSAdd_PS)
        {
            if (D3DXCompileShaderFromResourceW(hm, MAKEINTRESOURCEW(IDR_SunShafts_PS), NULL, NULL, "SSAdd", "ps_3_0", 0, &bf1, &bf2, &ppConstantTable) == S_OK)
            {
                if (pDevice->CreatePixelShader((DWORD*)bf1->GetBufferPointer(), &SSAdd_PS) != S_OK || !SSAdd_PS)
                    SAFE_RELEASE(SSAdd_PS);
            }
            else
            {
                loadCompiledShader(IDR_SSAdd_PS_compiled, SSAdd_PS);
            }
            SAFE_RELEASE(bf1);
            SAFE_RELEASE(bf2);
            SAFE_RELEASE(ppConstantTable);
        }

        if (!FxaaPS)
        {
            if (D3DXCompileShaderFromResourceW(hm, MAKEINTRESOURCEW(IDR_FXAA), NULL, NULL, "ApplyFXAA", "ps_3_0", 0, &bf1, &bf2, &ppConstantTable) == S_OK)
            {
                if (pDevice->CreatePixelShader((DWORD*)bf1->GetBufferPointer(), &FxaaPS) != S_OK || !FxaaPS)
                    SAFE_RELEASE(FxaaPS);
            }
            else
            {
                loadCompiledShader(IDR_FxaaPS_compiled, FxaaPS);
            }
            SAFE_RELEASE(bf1);
            SAFE_RELEASE(bf2);
            SAFE_RELEASE(ppConstantTable);
        }

        if (!CAS_PS)
        {
            if (D3DXCompileShaderFromResourceW(hm, MAKEINTRESOURCEW(IDR_CAS), NULL, NULL, "ApplyCAS", "ps_3_0", 0, &bf1, &bf2, &ppConstantTable) == S_OK)
            {
                if (pDevice->CreatePixelShader((DWORD*)bf1->GetBufferPointer(), &CAS_PS) != S_OK || !CAS_PS)
                    SAFE_RELEASE(CAS_PS);
            }
            else
            {
                loadCompiledShader(IDR_CAS_PS_compiled, CAS_PS);
            }
            SAFE_RELEASE(bf1);
            SAFE_RELEASE(bf2);
            SAFE_RELEASE(ppConstantTable);
        }

        if (!CASMasked_PS)
        {
            if (D3DXCompileShaderFromResourceW(hm, MAKEINTRESOURCEW(IDR_CAS), NULL, NULL, "ApplyCASMasked", "ps_3_0", 0, &bf1, &bf2, &ppConstantTable) == S_OK)
            {
                if (pDevice->CreatePixelShader((DWORD*)bf1->GetBufferPointer(), &CASMasked_PS) != S_OK || !CASMasked_PS)
                    SAFE_RELEASE(CASMasked_PS);
            }
            else
            {
                loadCompiledShader(IDR_CASMasked_PS_compiled, CASMasked_PS);
            }
            SAFE_RELEASE(bf1);
            SAFE_RELEASE(bf2);
            SAFE_RELEASE(ppConstantTable);
        }

        if (!SMAA_EdgeDetection)
        {
            if (D3DXCompileShaderFromResourceW(hm, MAKEINTRESOURCEW(IDR_SMAA), NULL, NULL, "DX9_SMAALumaEdgeDetectionPS", "ps_3_0", 0, &bf1, &bf2, &ppConstantTable) == S_OK)
            {
                if (pDevice->CreatePixelShader((DWORD*)bf1->GetBufferPointer(), &SMAA_EdgeDetection) != S_OK || !SMAA_EdgeDetection)
                    SAFE_RELEASE(SMAA_EdgeDetection);
            }
            else
            {
                loadCompiledShader(IDR_SMAA_EdgeDetection_compiled, SMAA_EdgeDetection);
            }
            SAFE_RELEASE(bf1);
            SAFE_RELEASE(bf2);
            SAFE_RELEASE(ppConstantTable);
        }

        if (!SMAA_BlendingWeightsCalculation)
        {
            if (D3DXCompileShaderFromResourceW(hm, MAKEINTRESOURCEW(IDR_SMAA), NULL, NULL, "DX9_SMAABlendingWeightCalculationPS", "ps_3_0", 0, &bf1, &bf2, &ppConstantTable) == S_OK)
            {
                if (pDevice->CreatePixelShader((DWORD*)bf1->GetBufferPointer(), &SMAA_BlendingWeightsCalculation) != S_OK || !SMAA_BlendingWeightsCalculation)
                    SAFE_RELEASE(SMAA_BlendingWeightsCalculation);
            }
            else
            {
                loadCompiledShader(IDR_SMAA_BlendingWeightsCalculation_compiled, SMAA_BlendingWeightsCalculation);
            }
            SAFE_RELEASE(bf1);
            SAFE_RELEASE(bf2);
            SAFE_RELEASE(ppConstantTable);
        }

        if (!SMAA_NeighborhoodBlending)
        {
            if (D3DXCompileShaderFromResourceW(hm, MAKEINTRESOURCEW(IDR_SMAA), NULL, NULL, "DX9_SMAANeighborhoodBlendingPS", "ps_3_0", 0, &bf1, &bf2, &ppConstantTable) == S_OK)
            {
                if (pDevice->CreatePixelShader((DWORD*)bf1->GetBufferPointer(), &SMAA_NeighborhoodBlending) != S_OK || !SMAA_NeighborhoodBlending)
                    SAFE_RELEASE(SMAA_NeighborhoodBlending);
            }
            else
            {
                loadCompiledShader(IDR_SMAA_NeighborhoodBlending_compiled, SMAA_NeighborhoodBlending);
            }
            SAFE_RELEASE(bf1);
            SAFE_RELEASE(bf2);
            SAFE_RELEASE(ppConstantTable);
        }

        if (!SMAA_EdgeDetectionVS)
        {
            if (D3DXCompileShaderFromResourceW(hm, MAKEINTRESOURCEW(IDR_SMAA), NULL, NULL, "DX9_SMAAEdgeDetectionVS", "vs_3_0", 0, &bf1, &bf2, &ppConstantTable) == S_OK)
            {
                if (pDevice->CreateVertexShader((DWORD*)bf1->GetBufferPointer(), &SMAA_EdgeDetectionVS) != S_OK || !SMAA_EdgeDetectionVS)
                    SAFE_RELEASE(SMAA_EdgeDetectionVS);
            }
            else
            {
                loadCompiledShader(IDR_SMAA_EdgeDetectionVS_compiled, SMAA_EdgeDetectionVS);
            }
            SAFE_RELEASE(bf1);
            SAFE_RELEASE(bf2);
            SAFE_RELEASE(ppConstantTable);
        }

        if (!SMAA_BlendingWeightsCalculationVS)
        {
            if (D3DXCompileShaderFromResourceW(hm, MAKEINTRESOURCEW(IDR_SMAA), NULL, NULL, "DX9_SMAABlendingWeightCalculationVS", "vs_3_0", 0, &bf1, &bf2, &ppConstantTable) == S_OK)
            {
                if (pDevice->CreateVertexShader((DWORD*)bf1->GetBufferPointer(), &SMAA_BlendingWeightsCalculationVS) != S_OK || !SMAA_BlendingWeightsCalculationVS)
                    SAFE_RELEASE(SMAA_BlendingWeightsCalculationVS);
            }
            else
            {
                loadCompiledShader(IDR_SMAA_BlendingWeightsCalculationVS_compiled, SMAA_BlendingWeightsCalculationVS);
            }
            SAFE_RELEASE(bf1);
            SAFE_RELEASE(bf2);
            SAFE_RELEASE(ppConstantTable);
        }

        if (!SMAA_NeighborhoodBlendingVS)
        {
            if (D3DXCompileShaderFromResourceW(hm, MAKEINTRESOURCEW(IDR_SMAA), NULL, NULL, "DX9_SMAANeighborhoodBlendingVS", "vs_3_0", 0, &bf1, &bf2, &ppConstantTable) == S_OK)
            {
                if (pDevice->CreateVertexShader((DWORD*)bf1->GetBufferPointer(), &SMAA_NeighborhoodBlendingVS) != S_OK || !SMAA_NeighborhoodBlendingVS)
                    SAFE_RELEASE(SMAA_NeighborhoodBlendingVS);
            }
            else
            {
                loadCompiledShader(IDR_SMAA_NeighborhoodBlendingVS_compiled, SMAA_NeighborhoodBlendingVS);
            }
            SAFE_RELEASE(bf1);
            SAFE_RELEASE(bf2);
            SAFE_RELEASE(ppConstantTable);
        }

        if (!Blit_PS)
        {
            if (D3DXAssembleShaderFromResourceW(hm, MAKEINTRESOURCEW(IDR_Blit_PS), NULL, NULL, 0, &bf1, &bf2) == S_OK)
            {
                if (pDevice->CreatePixelShader((DWORD*)bf1->GetBufferPointer(), &Blit_PS) != S_OK || !Blit_PS)
                    SAFE_RELEASE(Blit_PS);
                SAFE_RELEASE(bf1);
                SAFE_RELEASE(bf2);
            }
        }

        if (!AOEffect)
        {
            ID3DXBuffer* errors = nullptr;
            static std::string sampleCount = std::to_string(nAmbientOcclusionSamples);
            static std::string logMaxOffset = std::to_string(nAmbientOcclusionLogMaxOffset);
            static std::string maxMipLevel = std::to_string(nAmbientOcclusionMaxMipLevel);
            static std::string farClip = std::to_string(fAmbientOcclusionFarClip);
            static std::string gtaoSlices = std::to_string(nAmbientOcclusionGTAOSlices);
            static std::string gtaoSteps = std::to_string(nAmbientOcclusionGTAOSteps);
            D3DXMACRO defines[] = {
                {"GTAO_SLICES", gtaoSlices.c_str()},
                {"GTAO_STEPS", gtaoSteps.c_str()},
                {"NUM_SAMPLES", sampleCount.c_str()},
                {"LOG_MAX_OFFSET", logMaxOffset.c_str()},
                {"MAX_MIP_LEVEL", maxMipLevel.c_str()},
                {"FAR_CLIP", farClip.c_str()},
                {} // last must be empty
            };
            if (D3DXCreateEffectFromResourceW(rage::grcDevice::GetD3DDevice(),
                hm, MAKEINTRESOURCEW(IDR_AO_FX), defines, nullptr, 0, nullptr, &AOEffect, &errors) != S_OK)
            {
                if (errors)
                    MessageBoxA(nullptr, (LPCSTR)errors->GetBufferPointer(), "Error building shader!", MB_OK);
            }
            else
            {
                AOEffectHandles.AOTexture2D = AOEffect->GetParameterByName(nullptr, "AOTexture2D");
                AOEffectHandles.AOCamDepthTexture2D = AOEffect->GetParameterByName(nullptr, "AOCamDepthTexture2D");
                AOEffectHandles.DepthTex2D = AOEffect->GetParameterByName(nullptr, "DepthTex2D");
                AOEffectHandles.NormalTex2D = AOEffect->GetParameterByName(nullptr, "NormalTex2D");
                AOEffectHandles.vec4WorldToView = AOEffect->GetParameterByName(nullptr, "vec4WorldToView");
                AOEffectHandles.fUseNormals = AOEffect->GetParameterByName(nullptr, "fUseNormals");
                AOEffectHandles.fGTAOStrength = AOEffect->GetParameterByName(nullptr, "fGTAOStrength");
                for (auto [handle, name] : std::initializer_list<std::pair<D3DXHANDLE*, const char*>>{
                         { &AOEffectHandles.fThinOccluders, "fThinOccluders" }, { &AOEffectHandles.vec2NoiseOffset, "vec2NoiseOffset" },
                         { &AOEffectHandles.fMultiBounce, "fMultiBounce" }, { &AOEffectHandles.AlbedoTex2D, "AlbedoTex2D" },
                         { &AOEffectHandles.AOHistoryTex2D, "AOHistoryTex2D" }, { &AOEffectHandles.PrevDepthTex2D, "PrevDepthTex2D" },
                         { &AOEffectHandles.MotionTex2D, "MotionTex2D" }, { &AOEffectHandles.vec4ViewToPrevClip, "vec4ViewToPrevClip" },
                         { &AOEffectHandles.fUseMotion, "fUseMotion" }, { &AOEffectHandles.vec2MotionJitter, "vec2MotionJitter" },
                         { &AOEffectHandles.fUsePrevDepth, "fUsePrevDepth" }, { &AOEffectHandles.vec2PrevDepthRange, "vec2PrevDepthRange" },
                         { &AOEffectHandles.fTemporalBlend, "fTemporalBlend" } })
                    *handle = AOEffect->GetParameterByName(nullptr, name);
                AOEffectHandles.vec2InvViewportSize = AOEffect->GetParameterByName(nullptr, "vec2InvViewportSize");
                AOEffectHandles.fNearPlane = AOEffect->GetParameterByName(nullptr, "fNearPlane");
                AOEffectHandles.fFarPlane = AOEffect->GetParameterByName(nullptr, "fFarPlane");
                AOEffectHandles.fFarDivNear = AOEffect->GetParameterByName(nullptr, "fFarDivNear");
                AOEffectHandles.fRadius = AOEffect->GetParameterByName(nullptr, "fRadius");
                AOEffectHandles.fBias = AOEffect->GetParameterByName(nullptr, "fBias");
                AOEffectHandles.fIntensity = AOEffect->GetParameterByName(nullptr, "fIntensity");
                AOEffectHandles.fProjScale = AOEffect->GetParameterByName(nullptr, "fProjScale");
                AOEffectHandles.vec4ProjInfo = AOEffect->GetParameterByName(nullptr, "vec4ProjInfo");
                AOEffectHandles.vec2BlurDirection = AOEffect->GetParameterByName(nullptr, "vec2BlurDirection");
                AOEffectHandles.vec2PrevMipSize = AOEffect->GetParameterByName(nullptr, "vec2PrevMipSize");
                AOEffectHandles.vec2PrevMipTexel = AOEffect->GetParameterByName(nullptr, "vec2PrevMipTexel");
                AOEffectHandles.iPreviousMip = AOEffect->GetParameterByName(nullptr, "iPreviousMip");
            }
        }

        if (!SSREffect)
        {
            ID3DXBuffer* errors = nullptr;
            static std::string steps = std::to_string(nSSRSteps);
            static std::string refineSteps = std::to_string(nSSRRefineSteps);
            static std::string giRays = std::to_string(nGIRays);
            static std::string giSteps = std::to_string(nGISteps);
            D3DXMACRO defines[] = {
                {"NUM_STEPS", steps.c_str()},
                {"NUM_REFINE_STEPS", refineSteps.c_str()},
                {"GI_RAYS", giRays.c_str()},
                {"GI_STEPS", giSteps.c_str()},
                {} // last must be empty
            };
            if (D3DXCreateEffectFromResourceW(rage::grcDevice::GetD3DDevice(),
                hm, MAKEINTRESOURCEW(IDR_SSR_FX), defines, nullptr, 0, nullptr, &SSREffect, &errors) != S_OK)
            {
                if (errors)
                    MessageBoxA(nullptr, (LPCSTR)errors->GetBufferPointer(), "Error building shader!", MB_OK);
            }
            else
            {
                ReadSamplerStates(hm, IDR_SSR_FX, SSRSamplerStates);
                auto& h = SSREffectHandles;
                h.DepthTex2D = SSREffect->GetParameterByName(nullptr, "DepthTex2D");
                h.HistoryTex2D = SSREffect->GetParameterByName(nullptr, "HistoryTex2D");
                h.PrevDepthTex2D = SSREffect->GetParameterByName(nullptr, "PrevDepthTex2D");
                h.fUsePrevDepth = SSREffect->GetParameterByName(nullptr, "fUsePrevDepth");
                h.vec2PrevDepthRange = SSREffect->GetParameterByName(nullptr, "vec2PrevDepthRange");
                h.SpecularTex2D = SSREffect->GetParameterByName(nullptr, "SpecularTex2D");
                h.SurfaceTex2D = SSREffect->GetParameterByName(nullptr, "SurfaceTex2D");
                h.vec2InvViewportSize = SSREffect->GetParameterByName(nullptr, "vec2InvViewportSize");
                h.fNearPlane = SSREffect->GetParameterByName(nullptr, "fNearPlane");
                h.fFarDivNear = SSREffect->GetParameterByName(nullptr, "fFarDivNear");
                h.vec4ProjInfo = SSREffect->GetParameterByName(nullptr, "vec4ProjInfo");
                h.fMaxDistance = SSREffect->GetParameterByName(nullptr, "fMaxDistance");
                h.fThickness = SSREffect->GetParameterByName(nullptr, "fThickness");
                h.fEdgeFade = SSREffect->GetParameterByName(nullptr, "fEdgeFade");
                h.fIntensity = SSREffect->GetParameterByName(nullptr, "fIntensity");
                h.vec4ViewToPrevClip = SSREffect->GetParameterByName(nullptr, "vec4ViewToPrevClip");
                h.fGlossBoost = SSREffect->GetParameterByName(nullptr, "fGlossBoost");
                h.fGlossCutoff = SSREffect->GetParameterByName(nullptr, "fGlossCutoff");
                h.fWetness = SSREffect->GetParameterByName(nullptr, "fWetness");
                h.fWetGroundBoost = SSREffect->GetParameterByName(nullptr, "fWetGroundBoost");
                h.vec4WaterPlane = SSREffect->GetParameterByName(nullptr, "vec4WaterPlane");
                h.fWaterIntensity = SSREffect->GetParameterByName(nullptr, "fWaterIntensity");
                h.fWaterBlur = SSREffect->GetParameterByName(nullptr, "fWaterBlur");
                h.fWaterNormalStrength = SSREffect->GetParameterByName(nullptr, "fWaterNormalStrength");
                h.vec4WaterRings = SSREffect->GetParameterByName(nullptr, "vec4WaterRings");
                h.vec4WaterToView = SSREffect->GetParameterByName(nullptr, "vec4WaterToView");
                h.vec4WaterWorldX = SSREffect->GetParameterByName(nullptr, "vec4WaterWorldX");
                h.vec4WaterWorldY = SSREffect->GetParameterByName(nullptr, "vec4WaterWorldY");
                h.techSSR = SSREffect->GetTechniqueByName("SSR");
                h.techSSRWater = SSREffect->GetTechniqueByName("SSRWater");
                h.NormalTex2D = SSREffect->GetParameterByName(nullptr, "NormalTex2D");
                h.SSRResultTex2D = SSREffect->GetParameterByName(nullptr, "SSRResultTex2D");
                h.DebugTex2D = SSREffect->GetParameterByName(nullptr, "DebugTex2D");
                h.fDebugMode = SSREffect->GetParameterByName(nullptr, "fDebugMode");
                h.fUseGBufferNormals = SSREffect->GetParameterByName(nullptr, "fUseGBufferNormals");
                h.PreWaterTex2D = SSREffect->GetParameterByName(nullptr, "PreWaterTex2D");
                h.PostWaterTex2D = SSREffect->GetParameterByName(nullptr, "PostWaterTex2D");
                h.fUseWaterMask = SSREffect->GetParameterByName(nullptr, "fUseWaterMask");
                h.fDenoiseRadius = SSREffect->GetParameterByName(nullptr, "fDenoiseRadius");
                h.fDenoiseSSROnly = SSREffect->GetParameterByName(nullptr, "fDenoiseSSROnly");
                h.fPassThinObjects = SSREffect->GetParameterByName(nullptr, "fPassThinObjects");
                h.fStepJitter = SSREffect->GetParameterByName(nullptr, "fStepJitter");
                h.fTowardCamera = SSREffect->GetParameterByName(nullptr, "fTowardCamera");
                h.fReflectionBlur = SSREffect->GetParameterByName(nullptr, "fReflectionBlur");
                h.fDistanceFade = SSREffect->GetParameterByName(nullptr, "fDistanceFade");
                h.fFallback = SSREffect->GetParameterByName(nullptr, "fFallback");
                h.fSpreadRadius = SSREffect->GetParameterByName(nullptr, "fSpreadRadius");
                h.vec4SunView = SSREffect->GetParameterByName(nullptr, "vec4SunView");
                h.fCSLength = SSREffect->GetParameterByName(nullptr, "fCSLength");
                h.fCSThickness = SSREffect->GetParameterByName(nullptr, "fCSThickness");
                h.fCSMaxViewDistance = SSREffect->GetParameterByName(nullptr, "fCSMaxViewDistance");
                h.fCSIntensity = SSREffect->GetParameterByName(nullptr, "fCSIntensity");
                h.techContactShadows = SSREffect->GetTechniqueByName("ContactShadows");
                h.techContactTemporal = SSREffect->GetTechniqueByName("ContactTemporal");
                h.techContactUpsample = SSREffect->GetTechniqueByName("ContactUpsample");
                h.vec2NoiseOffset = SSREffect->GetParameterByName(nullptr, "vec2NoiseOffset");
                h.techSSRDenoise = SSREffect->GetTechniqueByName("SSRDenoise");
                h.techSSRDebug = SSREffect->GetTechniqueByName("SSRDebug");
                h.techSSRDebugCopy = SSREffect->GetTechniqueByName("SSRDebugCopy");
                h.SSRAccumTex2D = SSREffect->GetParameterByName(nullptr, "SSRAccumTex2D");
                h.SSRFallbackTex2D = SSREffect->GetParameterByName(nullptr, "SSRFallbackTex2D");
                h.SSRHitDistTex2D = SSREffect->GetParameterByName(nullptr, "SSRHitDistTex2D");
                h.MarchDepthTex2D = SSREffect->GetParameterByName(nullptr, "MarchDepthTex2D");
                h.fStepsPerPixel = SSREffect->GetParameterByName(nullptr, "fStepsPerPixel");
                h.fMarchFullDepth = SSREffect->GetParameterByName(nullptr, "fMarchFullDepth");
                h.fTemporalBlend = SSREffect->GetParameterByName(nullptr, "fTemporalBlend");
                h.techSSRTemporal = SSREffect->GetTechniqueByName("SSRTemporal");
                h.MotionTex2D = SSREffect->GetParameterByName(nullptr, "MotionTex2D");
                h.fUseMotion = SSREffect->GetParameterByName(nullptr, "fUseMotion");
                h.vec2MotionJitter = SSREffect->GetParameterByName(nullptr, "vec2MotionJitter");
                h.fTemporalAnySurface = SSREffect->GetParameterByName(nullptr, "fTemporalAnySurface");
                h.fGIRayLength = SSREffect->GetParameterByName(nullptr, "fGIRayLength");
                h.fGIThickness = SSREffect->GetParameterByName(nullptr, "fGIThickness");
                h.fGIMaxViewDistance = SSREffect->GetParameterByName(nullptr, "fGIMaxViewDistance");
                h.fGIIntensity = SSREffect->GetParameterByName(nullptr, "fGIIntensity");
                h.fGIRespectAO = SSREffect->GetParameterByName(nullptr, "fGIRespectAO");
                h.techSSGI = SSREffect->GetTechniqueByName("SSGI");
                h.fGIMaxBrightness = SSREffect->GetParameterByName(nullptr, "fGIMaxBrightness");
                h.techGIUpsample = SSREffect->GetTechniqueByName("GIUpsample");
                h.AlbedoTex2D = SSREffect->GetParameterByName(nullptr, "AlbedoTex2D");
                h.AlbedoLinearTex2D = SSREffect->GetParameterByName(nullptr, "AlbedoLinearTex2D");
                h.GIPrevTex2D = SSREffect->GetParameterByName(nullptr, "GIPrevTex2D");
                h.fGIFeedback = SSREffect->GetParameterByName(nullptr, "fGIFeedback");
                h.fGIOcclusion = SSREffect->GetParameterByName(nullptr, "fGIOcclusion");
                h.SceneTex2D = SSREffect->GetParameterByName(nullptr, "SceneTex2D");
                h.SkinIDTex2D = SSREffect->GetParameterByName(nullptr, "SkinIDTex2D");
                h.SkinLightTex2D = SSREffect->GetParameterByName(nullptr, "SkinLightTex2D");
                h.vec4SkinStep = SSREffect->GetParameterByName(nullptr, "vec4SkinStep");
                h.fSkinStrength = SSREffect->GetParameterByName(nullptr, "fSkinStrength");
                h.techSkinLight = SSREffect->GetTechniqueByName("SkinLight");
                h.techSkinScatter = SSREffect->GetTechniqueByName("SkinScatter");
                h.techSkinScatterFinal = SSREffect->GetTechniqueByName("SkinScatterFinal");
                h.techSkinDebug = SSREffect->GetTechniqueByName("SkinDebug");
            }
        }

        // Like the clouds: without it the ground stays dry.
        static bool wetGroundEffectTried = false;
        if (!WetGroundEffect && !wetGroundEffectTried)
        {
            wetGroundEffectTried = true;
            ID3DXBuffer* errors = nullptr;
            hrWetGroundEffect = D3DXCreateEffectFromResourceW(rage::grcDevice::GetD3DDevice(),
                hm, MAKEINTRESOURCEW(IDR_WETGROUND_FX), nullptr, nullptr, 0, nullptr, &WetGroundEffect, &errors);
            if (hrWetGroundEffect != S_OK)
            {
                WetGroundEffect = nullptr;
                if (errors)
                    MessageBoxA(nullptr, (LPCSTR)errors->GetBufferPointer(), "Error building shader!", MB_OK);
            }
            SAFE_RELEASE(errors);
        }

        // Without it headlights glint only inside their beams, as the game has them.
        static bool headlightGlintsEffectTried = false;
        if (!HeadlightGlintsEffect && !headlightGlintsEffectTried)
        {
            headlightGlintsEffectTried = true;
            ID3DXBuffer* errors = nullptr;
            hrHeadlightGlintsEffect = D3DXCreateEffectFromResourceW(rage::grcDevice::GetD3DDevice(),
                hm, MAKEINTRESOURCEW(IDR_HEADLIGHTGLINTS_FX), nullptr, nullptr, 0, nullptr, &HeadlightGlintsEffect, &errors);
            if (hrHeadlightGlintsEffect != S_OK)
            {
                HeadlightGlintsEffect = nullptr;
                if (errors)
                    MessageBoxA(nullptr, (LPCSTR)errors->GetBufferPointer(), "Error building shader!", MB_OK);
            }
            SAFE_RELEASE(errors);
        }

        // Not in ShadersFinishedLoading: without it the sky keeps only the game's clouds. Tried once,
        // so a build error shows one message, not one a frame.
        static bool cloudsEffectTried = false;
        if (!CloudsEffect && !cloudsEffectTried)
        {
            cloudsEffectTried = true;
            ID3DXBuffer* errors = nullptr;
            hrCloudsEffect = D3DXCreateEffectFromResourceW(rage::grcDevice::GetD3DDevice(),
                hm, MAKEINTRESOURCEW(IDR_CLOUDS_FX), nullptr, nullptr, 0, nullptr, &CloudsEffect, &errors);
            if (hrCloudsEffect != S_OK)
            {
                CloudsEffect = nullptr;
                if (errors)
                    MessageBoxA(nullptr, (LPCSTR)errors->GetBufferPointer(), "Error building shader!", MB_OK);
            }
            SAFE_RELEASE(errors);
        }

        TemporalAA::LoadShaders(pDevice);

        return ShadersFinishedLoading();
    }

    #define PostfxTextureCount 15
    IDirect3DBaseTexture9* prePostFx[PostfxTextureCount] = { 0 };
    DWORD Samplers[PostfxTextureCount] = { D3DTEXF_LINEAR };

    bool resourcesFinishedLoading()
    {
        return SMAA_EdgeDetection && SMAA_BlendingWeightsCalculation && SMAA_NeighborhoodBlending &&
            SMAA_EdgeDetectionVS && SMAA_BlendingWeightsCalculationVS && SMAA_NeighborhoodBlendingVS &&
            SMAA_areaTex && SMAA_searchTex && edgesTex && blendTex;
    }

    void swapbuffers()
    {
        auto temptex = renderTargetTex;
        renderTargetTex = textureRead;
        textureRead = temptex;

        auto tempsurf = renderTargetSurf;
        renderTargetSurf = surfaceRead;
        surfaceRead = tempsurf;
    };

    void ReleaseTextures()
    {
        // NormalTex = nullptr;
        DiffuseTex = nullptr;
        // SpecularTex = nullptr;
        // StencilTex = nullptr;
        // BloomTex = nullptr;
        // CurrentLumTex = nullptr;
        // DepthTex = nullptr;
        // CascadeAtlasTex = nullptr;

        FullScreenTex = nullptr;
        HalfScreenTex = nullptr;
        // pQuarterHDRTex = nullptr;

        if (FullScreenTex_temp1)
        {
            FullScreenTex_temp1->Destroy();
            FullScreenTex_temp1 = nullptr;
        }

        if (FullScreenTex_temp2)
        {
            FullScreenTex_temp2->Destroy();
            FullScreenTex_temp2 = nullptr;
        }

        if (edgesTex)
        {
            edgesTex->Destroy();
            edgesTex = nullptr;
        }

        if (blendTex)
        {
            blendTex->Destroy();
            blendTex = nullptr;
        }

        if (FullScreenDownsampleTex)
        {
            FullScreenDownsampleTex->Destroy();
            FullScreenDownsampleTex = nullptr;
        }

        if (FullScreenDownsampleTex2)
        {
            FullScreenDownsampleTex2->Destroy();
            FullScreenDownsampleTex2 = nullptr;
        }

        if (PreAlphaDepthCopyRT)
        {
            PreAlphaDepthCopyRT->Destroy();
            PreAlphaDepthCopyRT = nullptr;
        }
        //if (pShadowBlurTex1)
        //{
        //    pShadowBlurTex1->Destroy();
        //    pShadowBlurTex1 = nullptr;
        //}
        //
        //if (pShadowBlurTex2)
        //{
        //    pShadowBlurTex2->Destroy();
        //    pShadowBlurTex2 = nullptr;
        //}
    }

    bool ShadersFinishedLoading()
    {
        if (FxaaPS && dof_blur_ps && dof_coc_ps && depth_of_field_tent_ps && stipple_filter_ps
           && SSDraw_PS && SSPrepass_PS && SSAdd_PS
           && SMAA_EdgeDetection && SMAA_BlendingWeightsCalculation && SMAA_NeighborhoodBlending
           && SMAA_EdgeDetectionVS && SMAA_BlendingWeightsCalculationVS && SMAA_NeighborhoodBlendingVS
           && Blit_PS && AOEffect)
            // DeferredShadowGen_ps && deferred_lighting_PS1 && deferred_lighting_PS2 && SSAO_gen_ps && SSAO_blend_ps && DeferredShadowBlurH_ps && DeferredShadowBlurV_ps && DeferredShadowBlurCircle_ps
            return true;

        return false;
    }

    // The settings used afresh every frame, which Ctrl+Shift+F10 reads again from the ini while the
    // game runs (TickIniReload). Anything that sizes targets or builds shaders stays in Readini.
    void ReadLiveIni(CIniReader& iniReader)
    {
        fContactShadowIntensity = std::clamp(iniReader.ReadFloat("POSTFX", "ContactShadowsIntensity", 1.0f), 0.0f, 1.0f);
        fGIOcclusion = std::clamp(iniReader.ReadFloat("POSTFX", "ScreenSpaceIndirectLightOcclusion", 1.0f), 0.0f, 1.0f);
        fLocalContactShadowIntensity = std::clamp(iniReader.ReadFloat("POSTFX", "LocalContactShadowsIntensity", 1.0f), 0.0f, 1.0f);
        fLocalContactShadowUnshadowed = std::clamp(iniReader.ReadFloat("POSTFX", "LocalContactShadowsWithoutShadowMap", 0.0f), 0.0f, 1.0f);
        fVehicleBoxShadowLightSize = std::clamp(iniReader.ReadFloat("POSTFX", "VehicleBoxShadowsLightSize", 0.5f), 0.0f, 4.0f);
        fVehicleBoxShadowRounding = std::clamp(iniReader.ReadFloat("POSTFX", "VehicleBoxShadowsRounding", 0.3f), 0.0f, 1.0f);
        bVehicleBoxShadowsFromCarLights = iniReader.ReadInteger("POSTFX", "VehicleBoxShadowsFromCarLights", 0) != 0;
        bVehicleBoxShadowsWithSlots = iniReader.ReadInteger("POSTFX", "VehicleBoxShadowsWithSlots", 0) != 0;
        nVehicleBoxShadowSlotFadeMs = static_cast<uint32_t>(std::clamp(iniReader.ReadInteger("POSTFX", "VehicleBoxShadowsSlotFade", 600), 0, 5000));
        fVehicleBoxShadowScale[0] = std::clamp(iniReader.ReadFloat("POSTFX", "VehicleBoxShadowsLength", 1.0f), 0.3f, 1.2f);
        fVehicleBoxShadowScale[1] = std::clamp(iniReader.ReadFloat("POSTFX", "VehicleBoxShadowsWidth", 0.95f), 0.3f, 1.2f);
        fVehicleBoxShadowScale[2] = std::clamp(iniReader.ReadFloat("POSTFX", "VehicleBoxShadowsHeight", 0.9f), 0.3f, 1.2f);
        fVehicleBoxShadowSelfMargin = std::clamp(iniReader.ReadFloat("POSTFX", "VehicleBoxShadowsSelfMargin", 0.6f), 0.0f, 2.0f);
        fVehicleBoxShadowLowLightNarrow = std::clamp(iniReader.ReadFloat("POSTFX", "VehicleBoxShadowsLowLightNarrow", 1.0f), 0.2f, 1.0f);
        fSkinLighting = std::clamp(iniReader.ReadFloat("POSTFX", "SkinLighting", 1.0f), 0.0f, 2.0f);
        fSpecularSheen = std::clamp(iniReader.ReadFloat("POSTFX", "SpecularSheen", 0.1f), 0.0f, 50.0f);
        fLightsGGX = std::clamp(iniReader.ReadFloat("POSTFX", "LightsGGX", 1.0f), 0.0f, 4.0f);
        fLightsGGXFresnel = std::clamp(iniReader.ReadFloat("POSTFX", "LightsGGXFresnel", 0.5f), 0.0f, 1.0f);
        fLightsGGXSize = std::clamp(iniReader.ReadFloat("POSTFX", "LightsGGXSize", 0.05f), 0.0f, 2.0f);
        fLightsGGXStretch = std::clamp(iniReader.ReadFloat("POSTFX", "LightsGGXStretch", 0.5f), 0.0f, 4.0f);
        fLightsGGXHeadlights = std::clamp(iniReader.ReadFloat("POSTFX", "LightsGGXHeadlights", 0.65f), 0.0f, 2.0f);
        fLightsGGXFillLights = std::clamp(iniReader.ReadFloat("POSTFX", "LightsGGXFillLights", 0.5f), 0.0f, 2.0f);
        fLightsGGXSun = std::clamp(iniReader.ReadFloat("POSTFX", "LightsGGXSun", 1.0f), 0.0f, 4.0f);
        fLightsGGXEnvironment = std::clamp(iniReader.ReadFloat("POSTFX", "LightsGGXEnvironment", 1.0f), 0.0f, 1.0f);
        fLightsGGXMax = std::clamp(iniReader.ReadFloat("POSTFX", "LightsGGXMax", 2.0f), 0.1f, 16.0f);
        fSSRIntensity = std::clamp(iniReader.ReadFloat("POSTFX", "ScreenSpaceReflectionsIntensity", 1.0f), 0.0f, 1.0f);
        fSSRWetGround = std::clamp(iniReader.ReadFloat("POSTFX", "ScreenSpaceReflectionsWetGround", 1.5f), 0.0f, 8.0f);
        fSSRStepPixels = std::clamp(iniReader.ReadFloat("POSTFX", "ScreenSpaceReflectionsStepPixels", 2.0f), 1.0f, 8.0f);
        fSSRDenoiseRadius = std::clamp(iniReader.ReadFloat("POSTFX", "ScreenSpaceReflectionsSmoothing", 2.0f), 0.0f, 8.0f);
        bSSRPassThinObjects = iniReader.ReadInteger("POSTFX", "ScreenSpaceReflectionsPastThinObjects", 1) != 0;
        bSSRStepJitter = iniReader.ReadInteger("POSTFX", "ScreenSpaceReflectionsStepJitter", 1) != 0;
        bSSRTemporalJitter = iniReader.ReadInteger("POSTFX", "ScreenSpaceReflectionsTemporalJitter", 1) != 0;
        fSSRTemporalBlend = std::clamp(iniReader.ReadFloat("POSTFX", "ScreenSpaceReflectionsTemporal", 0.85f), 0.0f, 0.97f);
        fSSRFallback = std::clamp(iniReader.ReadFloat("POSTFX", "ScreenSpaceReflectionsFallback", 0.8f), 0.0f, 1.0f);
        bSSRMarchFullDepth = iniReader.ReadInteger("POSTFX", "ScreenSpaceReflectionsMarchFullDepth", 0) != 0;
        bPostFxProfiler = iniReader.ReadInteger("POSTFX", "PostFxProfiler", 0) != 0;
        fLightsGGXSoft = std::clamp(iniReader.ReadFloat("POSTFX", "LightsGGXSoft", 0.0f), 0.0f, 1.0f);
        fLightsGGXStretchView = std::clamp(iniReader.ReadFloat("POSTFX", "LightsGGXStretchView", 1.0f), 0.0f, 1.0f);
        fLightsGGXGlints = std::clamp(iniReader.ReadFloat("POSTFX", "LightsGGXGlints", 1.0f), 0.0f, 8.0f);
        fLightsGGXGlintsSize = std::clamp(iniReader.ReadFloat("POSTFX", "LightsGGXGlintsSize", 0.1f), 0.01f, 1.0f);
        fLightsGGXGlintsGloss = std::clamp(iniReader.ReadFloat("POSTFX", "LightsGGXGlintsGloss", 0.5f), 0.0f, 1.0f);
        fLightsGGXGlintsMax = std::clamp(iniReader.ReadFloat("POSTFX", "LightsGGXGlintsMax", 64.0f), 1.0f, 1000.0f);
        fWetGround = std::clamp(iniReader.ReadFloat("POSTFX", "WetGround", 1.0f), 0.0f, 1.0f);
        fWetGroundPuddles = std::clamp(iniReader.ReadFloat("POSTFX", "WetGroundPuddles", 0.35f), 0.0f, 1.0f);
        fWetGroundPuddleSize = std::clamp(iniReader.ReadFloat("POSTFX", "WetGroundPuddleSize", 24.0f), 2.0f, 500.0f);
        fWetGroundRipples = std::clamp(iniReader.ReadFloat("POSTFX", "WetGroundRipples", 1.0f), 0.0f, 3.0f);
        fWetGroundDarkening = std::clamp(iniReader.ReadFloat("POSTFX", "WetGroundDarkening", 1.0f), 0.0f, 2.0f);
        fWetGroundWetting = std::clamp(iniReader.ReadFloat("POSTFX", "WetGroundWetting", 30.0f), 0.0f, 3600.0f);
        fWetGroundDrying = std::clamp(iniReader.ReadFloat("POSTFX", "WetGroundDrying", 240.0f), 0.0f, 3600.0f);
        nWetGroundMaterials = iniReader.ReadInteger("POSTFX", "WetGroundMaterials", 1) & 0xFF;
        nWetGroundDebug = std::clamp(iniReader.ReadInteger("POSTFX", "WetGroundDebug", 0), 0, 2);
        fCloudShadows = std::clamp(iniReader.ReadFloat("POSTFX", "CloudShadows", 0.6f), 0.0f, 1.0f);
        fCloudShadowsHeight = std::clamp(iniReader.ReadFloat("POSTFX", "CloudShadowsHeight", 1200.0f), 100.0f, 10000.0f);
        fCloudShadowsScale = std::clamp(iniReader.ReadFloat("POSTFX", "CloudShadowsScale", 16000.0f), 100.0f, 50000.0f);
        fCloudShadowsWind = std::clamp(iniReader.ReadFloat("POSTFX", "CloudShadowsWind", 6.0f), 0.0f, 100.0f);
        fCloudShadowsSoftness = std::clamp(iniReader.ReadFloat("POSTFX", "CloudShadowsSoftness", 3.0f), 0.0f, 8.0f);
        fCloudShadowsCoverage = std::clamp(iniReader.ReadFloat("POSTFX", "CloudShadowsCoverage", 0.0f), -1.0f, 1.0f);
        nCloudShadowsDebug = std::clamp(iniReader.ReadInteger("POSTFX", "CloudShadowsDebug", 0), 0, 2);
        bVolumetricClouds = iniReader.ReadInteger("POSTFX", "VolumetricClouds", 1) != 0;
        fVolumetricCloudsCoverage = std::clamp(iniReader.ReadFloat("POSTFX", "VolumetricCloudsCoverage", 0.4f), 0.0f, 1.0f);
        fVolumetricCloudsBase = std::clamp(iniReader.ReadFloat("POSTFX", "VolumetricCloudsBase", 800.0f), 50.0f, 10000.0f);
        fVolumetricCloudsThickness = std::clamp(iniReader.ReadFloat("POSTFX", "VolumetricCloudsThickness", 600.0f), 50.0f, 5000.0f);
        fVolumetricCloudsDensity = std::clamp(iniReader.ReadFloat("POSTFX", "VolumetricCloudsDensity", 0.03f), 0.0005f, 1.0f);
        fVolumetricCloudsDetail = std::clamp(iniReader.ReadFloat("POSTFX", "VolumetricCloudsDetail", 0.6f), 0.0f, 1.0f);
        fVolumetricCloudsDetailScale = std::clamp(iniReader.ReadFloat("POSTFX", "VolumetricCloudsDetailScale", 1300.0f), 20.0f, 10000.0f);
        fVolumetricCloudsHaze = std::clamp(iniReader.ReadFloat("POSTFX", "VolumetricCloudsHaze", 25000.0f), 1000.0f, 200000.0f);
        fVolumetricCloudsMaxDistance = std::clamp(iniReader.ReadFloat("POSTFX", "VolumetricCloudsMaxDistance", 40000.0f), 1000.0f, 200000.0f);
        fVolumetricCloudsBrightness = std::clamp(iniReader.ReadFloat("POSTFX", "VolumetricCloudsBrightness", 1.0f), 0.0f, 4.0f);
        fVolumetricCloudsSunTint = std::clamp(iniReader.ReadFloat("POSTFX", "VolumetricCloudsSunTint", 0.6f), 0.0f, 1.0f);
        fVolumetricCloudsMoonlight = std::clamp(iniReader.ReadFloat("POSTFX", "VolumetricCloudsMoonlight", 0.2f), 0.0f, 2.0f);
        fVolumetricCloudsSkyLight = std::clamp(iniReader.ReadFloat("POSTFX", "VolumetricCloudsSkyLight", 0.5f), 0.0f, 1.0f);
        fVolumetricCloudsMinLight = std::clamp(iniReader.ReadFloat("POSTFX", "VolumetricCloudsMinLight", 0.25f), 0.0f, 0.9f);
        fVolumetricCloudsSkyMatch = std::clamp(iniReader.ReadFloat("POSTFX", "VolumetricCloudsSkyMatch", 2.0f), 0.0f, 20.0f);
        bVolumetricCloudsWeather = iniReader.ReadInteger("POSTFX", "VolumetricCloudsWeather", 1) != 0;
        fVolumetricCloudsVanilla = std::clamp(iniReader.ReadFloat("POSTFX", "VolumetricCloudsVanilla", 0.0f), 0.0f, 1.0f);
        fVolumetricCloudsTranslucency = std::clamp(iniReader.ReadFloat("POSTFX", "VolumetricCloudsTranslucency", 0.25f), 0.0f, 0.9f);
        fVolumetricCloudsEvolution = std::clamp(iniReader.ReadFloat("POSTFX", "VolumetricCloudsEvolution", 1.0f), 0.0f, 10.0f);
        fVolumetricCloudsSaturation = std::clamp(iniReader.ReadFloat("POSTFX", "VolumetricCloudsSaturation", 1.0f), 0.0f, 2.0f);
        fVolumetricCloudsMottle = std::clamp(iniReader.ReadFloat("POSTFX", "VolumetricCloudsMottle", 0.4f), 0.0f, 1.0f);
        fVolumetricCloudsShade = std::clamp(iniReader.ReadFloat("POSTFX", "VolumetricCloudsShade", 0.65f), 0.0f, 2.0f);
        fVolumetricCloudsAbsorption = std::clamp(iniReader.ReadFloat("POSTFX", "VolumetricCloudsAbsorption", 0.35f), 0.05f, 3.0f);
        nVolumetricCloudsDebug = std::clamp(iniReader.ReadInteger("POSTFX", "VolumetricCloudsDebug", 0), 0, 15);
        bVolumetricCloudsReflections = iniReader.ReadInteger("POSTFX", "VolumetricCloudsReflections", 1) != 0;
        fVolumetricCloudsReflectionBrightness = std::clamp(iniReader.ReadFloat("POSTFX", "VolumetricCloudsReflectionBrightness", 1.0f), 0.0f, 10.0f);
    }

    void Readini()
    {
        CIniReader iniReader("");
        EnablePostfx = iniReader.ReadInteger("SRF", "EnablePostfx", 1);

        useStippleFilter = iniReader.ReadInteger("SRF", "StippleFilter", 1) != 0;

        bEnablePreAlphaDepth = iniReader.ReadInteger("POSTFX", "EnablePreAlphaDepth", 1) != 0;

        nSSRSteps = std::clamp(iniReader.ReadInteger("POSTFX", "ScreenSpaceReflectionsSteps", 32), 4, 128);
        nSSRRefineSteps = std::clamp(iniReader.ReadInteger("POSTFX", "ScreenSpaceReflectionsRefineSteps", 8), 0, 16);
        fSSRMaxDistance = std::max(1.0f, iniReader.ReadFloat("POSTFX", "ScreenSpaceReflectionsMaxDistance", 24.0f));
        fSSRThickness = std::max(0.0f, iniReader.ReadFloat("POSTFX", "ScreenSpaceReflectionsThickness", 0.3f));
        fSSREdgeFade = std::clamp(iniReader.ReadFloat("POSTFX", "ScreenSpaceReflectionsEdgeFade", 0.1f), 0.001f, 0.5f);
        fSSRGlossBoost = std::clamp(iniReader.ReadFloat("POSTFX", "ScreenSpaceReflectionsGlossBoost", 2.0f), 0.0f, 8.0f);
        fSSRGlossCutoff = std::clamp(iniReader.ReadFloat("POSTFX", "ScreenSpaceReflectionsGlossCutoff", 0.5f), 0.0f, 1.0f);
        fSSRWaterIntensity = std::clamp(iniReader.ReadFloat("POSTFX", "ScreenSpaceReflectionsWaterIntensity", 1.0f), 0.0f, 1.0f);
        fSSRWaterLevelOffset = iniReader.ReadFloat("POSTFX", "ScreenSpaceReflectionsWaterLevelOffset", 0.0f);
        fSSRWaterBlur = std::clamp(iniReader.ReadFloat("POSTFX", "ScreenSpaceReflectionsWaterBlur", 3.0f), 0.0f, 32.0f);
        fSSRWaterNormalStrength = std::clamp(iniReader.ReadFloat("POSTFX", "ScreenSpaceReflectionsWaterRipple", 1.0f), 0.0f, 4.0f);
        bSSRGBufferNormals = iniReader.ReadInteger("POSTFX", "ScreenSpaceReflectionsGBufferNormals", 1) != 0;
        fSSRTowardCamera = std::clamp(iniReader.ReadFloat("POSTFX", "ScreenSpaceReflectionsTowardCamera", 0.0f), 0.0f, 1.0f);
        fSSRReflectionBlur = std::clamp(iniReader.ReadFloat("POSTFX", "ScreenSpaceReflectionsBlur", 0.0f), 0.0f, 32.0f);
        fSSRDistanceFade = std::clamp(iniReader.ReadFloat("POSTFX", "ScreenSpaceReflectionsDistanceFade", 0.0f), 0.0f, 100.0f);
        fContactShadowLength = std::clamp(iniReader.ReadFloat("POSTFX", "ContactShadowsLength", 0.3f), 0.05f, 10.0f);
        fContactShadowThickness = std::clamp(iniReader.ReadFloat("POSTFX", "ContactShadowsThickness", 0.15f), 0.01f, 10.0f);
        fContactShadowMaxDistance = std::clamp(iniReader.ReadFloat("POSTFX", "ContactShadowsMaxDistance", 60.0f), 1.0f, 1000.0f);
        bContactShadowStepJitter = iniReader.ReadInteger("POSTFX", "ContactShadowsStepJitter", 1) != 0;
        bContactShadowsHalfRes = iniReader.ReadInteger("POSTFX", "ContactShadowsHalfResolution", 1) != 0;
        fContactTemporalBlend = std::clamp(iniReader.ReadFloat("POSTFX", "ContactShadowsTemporal", 0.8f), 0.0f, 0.95f);
        fGIIntensity = std::clamp(iniReader.ReadFloat("POSTFX", "ScreenSpaceIndirectLightIntensity", 1.5f), 0.0f, 8.0f);
        fGIMaxBrightness = std::clamp(iniReader.ReadFloat("POSTFX", "ScreenSpaceIndirectLightMaxBrightness", 4.0f), 0.05f, 8.0f);
        fSkinScatteringWidth = std::clamp(iniReader.ReadFloat("POSTFX", "SkinScatteringWidth", 0.03f), 0.001f, 0.1f);
        fSkinScatteringStrength = std::clamp(iniReader.ReadFloat("POSTFX", "SkinScatteringStrength", 1.0f), 0.0f, 2.0f);
        ReadLiveIni(iniReader);
        fGIRayLength = std::clamp(iniReader.ReadFloat("POSTFX", "ScreenSpaceIndirectLightRayLength", 4.0f), 0.1f, 20.0f);
        fGIThickness = std::clamp(iniReader.ReadFloat("POSTFX", "ScreenSpaceIndirectLightThickness", 0.5f), 0.01f, 10.0f);
        fGIMaxDistance = std::clamp(iniReader.ReadFloat("POSTFX", "ScreenSpaceIndirectLightMaxDistance", 60.0f), 1.0f, 1000.0f);
        fGITemporalBlend = std::clamp(iniReader.ReadFloat("POSTFX", "ScreenSpaceIndirectLightTemporal", 0.9f), 0.0f, 0.97f);
        nGIRays = std::clamp(iniReader.ReadInteger("POSTFX", "ScreenSpaceIndirectLightRays", 4), 1, 16);
        nGISteps = std::clamp(iniReader.ReadInteger("POSTFX", "ScreenSpaceIndirectLightSteps", 8), 2, 32);
        bLocalContactShadows = iniReader.ReadInteger("POSTFX", "LocalContactShadows", 1) != 0;
        fLocalContactShadowLength = std::clamp(iniReader.ReadFloat("POSTFX", "LocalContactShadowsLength", 0.5f), 0.05f, 10.0f);
        fLocalContactShadowThickness = std::clamp(iniReader.ReadFloat("POSTFX", "LocalContactShadowsThickness", 0.2f), 0.01f, 5.0f);
        fLocalContactShadowMaxDistance = std::clamp(iniReader.ReadFloat("POSTFX", "LocalContactShadowsMaxDistance", 40.0f), 1.0f, 1000.0f);
        fVolumetricLightIntensity = std::clamp(iniReader.ReadFloat("POSTFX", "VolumetricLightIntensity", 4.0f), 0.0f, 20.0f);
        fVolumetricLightScale = std::clamp(iniReader.ReadFloat("POSTFX", "VolumetricLightScale", 0.25f), 0.0f, 2.0f);
        fVolumetricLightMaxDistance = std::clamp(iniReader.ReadFloat("POSTFX", "VolumetricLightMaxDistance", 100.0f), 10.0f, 1000.0f);
        nVolumetricLightHeadlightFlag = uint32_t(iniReader.ReadInteger("POSTFX", "VolumetricLightHeadlightFlag", rage::LF_VEHICLE));
        fVolumetricLightHeadlightIntensity = std::clamp(iniReader.ReadFloat("POSTFX", "VolumetricLightHeadlightIntensity", 2.0f), 0.0f, 20.0f);
        fVolumetricLightHeadlightLength = std::clamp(iniReader.ReadFloat("POSTFX", "VolumetricLightHeadlightLength", 25.0f), 1.0f, 200.0f);
        nVolumetricLightHeadlightAddFlags = uint32_t(iniReader.ReadInteger("POSTFX", "VolumetricLightHeadlightAddFlags", 0));
        bFillLights = iniReader.ReadInteger("POSTFX", "FillLights", 0) != 0;
        fFillLightsMinRadius = std::clamp(iniReader.ReadFloat("POSTFX", "FillLightsMinRadius", 30.0f), 0.0f, 1000.0f);
        bVolumetricLightHeadlightShadow = iniReader.ReadInteger("POSTFX", "VolumetricLightHeadlightShadow", 0) != 0;
        fVolumetricLightHeadlightPitch = std::clamp(iniReader.ReadFloat("POSTFX", "VolumetricLightHeadlightPitch", 6.0f), -45.0f, 45.0f);
        bGlassReflections = iniReader.ReadInteger("POSTFX", "ScreenSpaceReflectionsGlass", 1) != 0;
        fGlassReflectionsLength = std::clamp(iniReader.ReadFloat("POSTFX", "ScreenSpaceReflectionsGlassLength", 15.0f), 1.0f, 100.0f);
        fGlassReflectionsThickness = std::clamp(iniReader.ReadFloat("POSTFX", "ScreenSpaceReflectionsGlassThickness", 0.5f), 0.05f, 10.0f);
        bGlassStepJitter = iniReader.ReadInteger("POSTFX", "ScreenSpaceReflectionsGlassStepJitter", 1) != 0;

        nAmbientOcclusionBlurPasses = iniReader.ReadInteger("POSTFX", "AmbientOcclusionBlurPasses", 1);
        nAmbientOcclusionSamples = iniReader.ReadInteger("POSTFX", "AmbientOcclusionSamples", 9);
        nAmbientOcclusionLogMaxOffset = iniReader.ReadInteger("POSTFX", "AmbientOcclusionLogMaxOffset", 3);
        nAmbientOcclusionMaxMipLevel = iniReader.ReadInteger("POSTFX", "AmbientOcclusionMaxMipLevel", 5);
        fAmbientOcclusionFarClip = iniReader.ReadFloat("POSTFX", "AmbientOcclusionFarClip", 150.0f);
        fAmbientOcclusionBlurRadius = iniReader.ReadFloat("POSTFX", "AmbientOcclusionBlurRadius", 2.0f);
        nAmbientOcclusionGTAOSlices = std::clamp(iniReader.ReadInteger("POSTFX", "AmbientOcclusionGTAOSlices", 3), 1, 8);
        nAmbientOcclusionGTAOSteps = std::clamp(iniReader.ReadInteger("POSTFX", "AmbientOcclusionGTAOSteps", 4), 1, 16);
        fAmbientOcclusionGTAOStrength = std::clamp(iniReader.ReadFloat("POSTFX", "AmbientOcclusionGTAOStrength", 1.0f), 0.0f, 4.0f);
        fAmbientOcclusionGTAOThinOccluders = std::clamp(iniReader.ReadFloat("POSTFX", "AmbientOcclusionGTAOThinOccluders", 0.5f), 0.0f, 1.0f);
        fAmbientOcclusionTemporal = std::clamp(iniReader.ReadFloat("POSTFX", "AmbientOcclusionGTAOTemporal", 0.9f), 0.0f, 0.98f);
        bAmbientOcclusionMultiBounce = iniReader.ReadInteger("POSTFX", "AmbientOcclusionGTAOMultiBounce", 1) != 0;

        nAmbientOcclusionBlurPasses = std::max(0, nAmbientOcclusionBlurPasses);
        nAmbientOcclusionSamples = std::clamp(nAmbientOcclusionSamples, 0, 128);
        nAmbientOcclusionLogMaxOffset = std::max(0, nAmbientOcclusionLogMaxOffset);
        nAmbientOcclusionMaxMipLevel = std::max(1, nAmbientOcclusionMaxMipLevel);
        fAmbientOcclusionFarClip = std::max(1.0f, fAmbientOcclusionFarClip);
        fAmbientOcclusionBlurRadius = std::max(0.0f, fAmbientOcclusionBlurRadius);

        AOCamDepthSurf.resize(nAmbientOcclusionMaxMipLevel);

        fAmbientOcclusionRadius = iniReader.ReadFloat("POSTFX", "AmbientOcclusionRadius", 1.125f);
        fAmbientOcclusionBias = iniReader.ReadFloat("POSTFX", "AmbientOcclusionBias", 0.03f);
        fAmbientOcclusionIntensity = iniReader.ReadFloat("POSTFX", "AmbientOcclusionIntensity", 0.4f);

        fAmbientOcclusionRadius = std::max(fAmbientOcclusionRadius, 0.0f);
        fAmbientOcclusionBias = std::max(fAmbientOcclusionBias, 0.0f);
        fAmbientOcclusionIntensity = std::max(fAmbientOcclusionIntensity, 0.0f);
        // 0 off, 1 horizontal, 2 vertical, 3 horizontal e vertical.
        //useScreenSpaceShadowsBlur = iniReader.ReadInteger("SRF", "ScreenSpaceShadowsBlur", 0);
        //useHardwareBilinearSampling = iniReader.ReadInteger("SRF", "NewShadowAtlas", 0) != 0;
    }

    void createTextures(UINT Width, UINT Height, HMODULE hm)
    {
        IDirect3DDevice9* pDevice = rage::grcDevice::GetD3DDevice();

        auto desc = rage::OwnRenderTargetDesc(rage::GRCFMT_A16B16G16R16F);
        FullScreenTex_temp1 = rage::CreateEmptyRenderTarget("FullScreenTex_temp1", Width, Height, 64, desc);

        // Composited into the back buffer by FXAA and SMAA, it must not clip the highlights of an HDR back buffer
        desc.mFormat = HDROutput::IsBackBufferFloat() ? rage::GRCFMT_A16B16G16R16F : rage::GRCFMT_A8R8G8B8;
        FullScreenTex_temp2 = rage::CreateEmptyRenderTarget("FullScreenTex_temp2", Width, Height, HDROutput::IsBackBufferFloat() ? 64 : 32, desc);

        //desc.mFormat = rage::GRCFMT_G16R16F;
        // 
        //pShadowBlurTex1 = rage::CreateEmptyRenderTarget("pShadowBlurTex1", Width, Height, 32, desc);
        //pShadowBlurTex2 = rage::CreateEmptyRenderTarget("pShadowBlurTex2", Width, Height, 32, desc);

        desc.mFormat = rage::GRCFMT_X8R8G8B8;

        edgesTex = rage::CreateEmptyRenderTarget("edgesTex", Width, Height, 32, desc);

        desc.mFormat = rage::GRCFMT_A8R8G8B8;

        blendTex = rage::CreateEmptyRenderTarget("blendTex", Width, Height, 32, desc);

        desc.mFormat = rage::GRCFMT_A16B16G16R16F;

        FullScreenDownsampleTex = rage::CreateEmptyRenderTarget("FullScreenDownsampleTex", Width / 2, Height / 2, 64, desc);
        FullScreenDownsampleTex2 = rage::CreateEmptyRenderTarget("FullScreenDownsampleTex2", Width / 2, Height / 2, 64, desc);

        // Always taken: SSR, its history check and the car glass read it. EnablePreAlphaDepth
        // decides only whether depth of field and sun shafts use it.
        desc.mFormat = rage::GRCFMT_R32F;
        PreAlphaDepthCopyRT = rage::CreateEmptyRenderTarget("PreAlphaDepthCopy", Width, Height, 32, desc);


        if (!SMAA_areaTex)
            D3DXCreateTextureFromResourceExW(pDevice, hm, MAKEINTRESOURCEW(IDR_AreaTex), 160, 560, 1, 0, D3DFMT_UNKNOWN, D3DPOOL_MANAGED, D3DX_FILTER_LINEAR, D3DX_FILTER_LINEAR, 0, NULL, NULL, &SMAA_areaTex);
        if (!SMAA_searchTex)
            D3DXCreateTextureFromResourceExW(pDevice, hm, MAKEINTRESOURCEW(IDR_SearchTex), 64, 16, 1, 0, D3DFMT_UNKNOWN, D3DPOOL_MANAGED, D3DX_FILTER_LINEAR, D3DX_FILTER_LINEAR, 0, NULL, NULL, &SMAA_searchTex);
    }
};

// The cloud deck's noise for the cloud shadows and the volumetric clouds: 1024 x 1024, tiling.
// - Heaps: each of 6 x 6 cells a tile holds one heap at a random point, of a random radius, falling
//   off from its middle; smaller heaps from 12 x 12 cells add to them. On a 16 km tile a heap is one
//   to two kilometres across, about one every two and a half kilometres: at 8 x 8 the heaps, under a
//   kilometre, came out small, and at 4 x 4, one every four kilometres, the sky stayed mostly empty
//   however much each heap grew with the cover. Perlin-Worley noise in its place
//   joined the clouds into one network over half the sky, where real fair weather cumulus stand
//   apart, spread evenly.
// - The heaps are read through a warp of value noise, so their outlines wander, and a fine value
//   noise roughens their edges.
// - Equalised: each texel is its value's rank, so a share c of the map lies above 1 - c, and the
//   clouds' and the shadows' cover is the share of the sky they take.
// At 256 x 256 a texel of an 8 km tile spanned 31 m, and the quintic filtering's flat texel middles
// showed as steps along the clouds' edges. Managed, so it survives device resets.
IDirect3DTexture9* PostFxResource::CloudNoiseTex()
{
    if (CloudNoiseTexture)
        return CloudNoiseTexture;
    auto pDevice = rage::grcDevice::GetD3DDevice();
    if (!pDevice)
        return nullptr;

    constexpr int size = 1024;
    auto lattice = [](int x, int y, int seed) {
        uint32_t h = static_cast<uint32_t>(x) * 374761393u + static_cast<uint32_t>(y) * 668265263u + static_cast<uint32_t>(seed) * 2246822519u;
        h = (h ^ (h >> 13)) * 1274126177u;
        return static_cast<float>((h ^ (h >> 16)) & 0xffff) / 65535.0f;
    };
    auto wrap = [](int i, int n) { return ((i % n) + n) % n; };
    // Value noise of the given octaves from cells0 cells a tile, each half the one before, 0 to 1.
    auto valueNoise = [&](int x, int y, int cells0, int octaves, int seed0) {
        float sum = 0.0f, total = 0.0f, amplitude = 1.0f;
        for (int octave = 0, cells = cells0; octave < octaves; ++octave, cells *= 2, amplitude *= 0.5f)
        {
            const float cell = static_cast<float>(size) / cells;
            const float fx = x / cell, fy = y / cell;
            const int x0 = static_cast<int>(fx), y0 = static_cast<int>(fy);
            float tx = fx - x0, ty = fy - y0;
            tx = tx * tx * (3.0f - 2.0f * tx);
            ty = ty * ty * (3.0f - 2.0f * ty);
            const int x1 = (x0 + 1) % cells, y1 = (y0 + 1) % cells;
            const int seed = seed0 + octave;
            const float a = lattice(x0, y0, seed), b = lattice(x1, y0, seed);
            const float c = lattice(x0, y1, seed), d = lattice(x1, y1, seed);
            const float ab = a + (b - a) * tx, cd = c + (d - c) * tx;
            sum += amplitude * (ab + (cd - ab) * ty);
            total += amplitude;
        }
        return sum / total;
    };
    // The highest heap over the cells around (x, y): 1 at a heap's middle, 0 at its radius. A heap
    // reaches up to 1.6 cells from its cell's corner (its middle within 0.15 to 0.85, its radius up
    // to 0.75), so the search takes two cells either way; one either way cut heaps off along the
    // cells' straight edges.
    auto heaps = [&](float x, float y, int cells, int seed) {
        const float cell = static_cast<float>(size) / cells;
        const float fx = x / cell, fy = y / cell;
        const int cx = static_cast<int>(std::floor(fx)), cy = static_cast<int>(std::floor(fy));
        float best = 0.0f;
        for (int dy = -2; dy <= 2; ++dy)
            for (int dx = -2; dx <= 2; ++dx)
            {
                const int nx = cx + dx, ny = cy + dy;
                const int wx = wrap(nx, cells), wy = wrap(ny, cells);
                const float px = nx + 0.15f + 0.7f * lattice(wx, wy, seed), py = ny + 0.15f + 0.7f * lattice(wy, wx, seed + 1);
                const float radius = 0.35f + 0.4f * lattice(wx, wy, seed + 2);
                const float distance = std::sqrt((px - fx) * (px - fx) + (py - fy) * (py - fy));
                best = (std::max)(best, std::clamp(1.0f - distance / radius, 0.0f, 1.0f));
            }
        return best;
    };

    constexpr int heapCells = 6;
    const float warpReach = static_cast<float>(size) / heapCells * 0.6f;
    std::vector<float> value(size * size, 0.0f);
    for (int y = 0; y < size; ++y)
        for (int x = 0; x < size; ++x)
        {
            const float wx = x + (valueNoise(x, y, 16, 3, 300) - 0.5f) * warpReach;
            const float wy = y + (valueNoise(x, y, 16, 3, 310) - 0.5f) * warpReach;
            value[y * size + x] = heaps(wx, wy, heapCells, 50) + 0.35f * heaps(wx, wy, heapCells * 2, 60) + 0.3f * valueNoise(x, y, 64, 4, 400);
        }
    {
        std::vector<int> order(value.size());
        for (size_t i = 0; i < order.size(); ++i)
            order[i] = static_cast<int>(i);
        std::stable_sort(order.begin(), order.end(), [&](int a, int b) { return value[a] < value[b]; });
        const float last = static_cast<float>(order.size() - 1);
        for (size_t rank = 0; rank < order.size(); ++rank)
            value[order[rank]] = rank / last;
    }

    // 16 bits: the volumetric clouds stretch the coverage several times over near their edges, and
    // the 8 bit steps showed as terraces and streaks down their sides.
    bool wide = SUCCEEDED(pDevice->CreateTexture(size, size, 0, 0, D3DFMT_L16, D3DPOOL_MANAGED, &CloudNoiseTexture, nullptr));
    if (!wide && FAILED(pDevice->CreateTexture(size, size, 0, 0, D3DFMT_L8, D3DPOOL_MANAGED, &CloudNoiseTexture, nullptr)))
    {
        CloudNoiseTexture = nullptr;
        return nullptr;
    }
    // Each mip a box filter of the one above, so a wider mip gives the shadow a softer edge.
    for (DWORD level = 0, n = size; level < CloudNoiseTexture->GetLevelCount(); ++level, n /= 2)
    {
        D3DLOCKED_RECT locked = {};
        if (FAILED(CloudNoiseTexture->LockRect(level, &locked, nullptr, 0)))
            continue;
        for (DWORD y = 0; y < n; ++y)
        {
            auto row = static_cast<uint8_t*>(locked.pBits) + y * locked.Pitch;
            for (DWORD x = 0; x < n; ++x)
            {
                const float v = std::clamp(value[y * n + x], 0.0f, 1.0f);
                if (wide)
                    reinterpret_cast<uint16_t*>(row)[x] = static_cast<uint16_t>(v * 65535.0f + 0.5f);
                else
                    row[x] = static_cast<uint8_t>(v * 255.0f + 0.5f);
            }
        }
        CloudNoiseTexture->UnlockRect(level);
        if (n > 1)
        {
            const DWORD half = n / 2;
            for (DWORD y = 0; y < half; ++y)
                for (DWORD x = 0; x < half; ++x)
                    value[y * half + x] = 0.25f * (value[2 * y * n + 2 * x] + value[2 * y * n + 2 * x + 1] +
                                                   value[(2 * y + 1) * n + 2 * x] + value[(2 * y + 1) * n + 2 * x + 1]);
        }
    }
    return CloudNoiseTexture;
}

// The volumetric clouds' detail: 64 x 64 x 64, tiling, three octaves of inverted Worley noise
// (4, 8 and 16 cells a tile), which reads as round billows when it erodes a cloud's edge, in two
// channels from different points. Managed, so it survives device resets.
IDirect3DVolumeTexture9* PostFxResource::CloudDetailTex()
{
    if (CloudDetailTexture)
        return CloudDetailTexture;
    auto pDevice = rage::grcDevice::GetD3DDevice();
    if (!pDevice)
        return nullptr;

    constexpr int size = 64;
    auto hash = [](int x, int y, int z, int seed) {
        uint32_t h = static_cast<uint32_t>(x) * 73856093u ^ static_cast<uint32_t>(y) * 19349663u ^ static_cast<uint32_t>(z) * 83492791u ^
                     static_cast<uint32_t>(seed) * 2654435761u;
        h = (h ^ (h >> 15)) * 2246822519u;
        h = (h ^ (h >> 13)) * 3266489917u;
        return h ^ (h >> 16);
    };
    // Two channels of the same Worley noise from different points: the outline's wander reads both
    // at once where it took two reads of one, and the fine billows swirl by both.
    std::vector<float> value[2] = { std::vector<float>(size * size * size, 0.0f), std::vector<float>(size * size * size, 0.0f) };
    const int seeds[2] = { 17, 29 };
    const int cellCounts[3] = { 4, 8, 16 };
    const float weights[3] = { 0.625f, 0.25f, 0.125f };
    for (int channel = 0; channel < 2; ++channel)
    {
        for (int octave = 0; octave < 3; ++octave)
        {
            const int cells = cellCounts[octave];
            const float cell = static_cast<float>(size) / cells;
            // One feature point per cell, at a random place inside it.
            std::vector<float> points(cells * cells * cells * 3);
            for (int i = 0; i < cells * cells * cells; ++i)
                for (int k = 0; k < 3; ++k)
                    points[i * 3 + k] = static_cast<float>(hash(i, k, octave, seeds[channel]) & 0xffff) / 65535.0f;
            for (int z = 0; z < size; ++z)
                for (int y = 0; y < size; ++y)
                    for (int x = 0; x < size; ++x)
                    {
                        const float fx = (x + 0.5f) / cell, fy = (y + 0.5f) / cell, fz = (z + 0.5f) / cell;
                        const int cx = static_cast<int>(fx), cy = static_cast<int>(fy), cz = static_cast<int>(fz);
                        float nearest = 3.0f;
                        for (int dz = -1; dz <= 1; ++dz)
                            for (int dy = -1; dy <= 1; ++dy)
                                for (int dx = -1; dx <= 1; ++dx)
                                {
                                    const int nx = cx + dx, ny = cy + dy, nz = cz + dz;
                                    const int wx = (nx + cells) % cells, wy = (ny + cells) % cells, wz = (nz + cells) % cells;
                                    const float* pt = &points[((wz * cells + wy) * cells + wx) * 3];
                                    const float ex = nx + pt[0] - fx, ey = ny + pt[1] - fy, ez = nz + pt[2] - fz;
                                    nearest = (std::min)(nearest, ex * ex + ey * ey + ez * ez);
                                }
                        value[channel][(z * size + y) * size + x] += weights[octave] * (1.0f - std::clamp(std::sqrt(nearest), 0.0f, 1.0f));
                    }
        }
        const auto [lo, hi] = std::minmax_element(value[channel].begin(), value[channel].end());
        const float minValue = *lo, range = (std::max)(*hi - *lo, 1e-5f);
        for (auto& v : value[channel])
            v = (v - minValue) / range;
    }

    // 16 bits a channel for the same reason as the coverage; 8 where G16R16 volumes are missing.
    bool wide = SUCCEEDED(pDevice->CreateVolumeTexture(size, size, size, 1, 0, D3DFMT_G16R16, D3DPOOL_MANAGED, &CloudDetailTexture, nullptr));
    if (!wide && FAILED(pDevice->CreateVolumeTexture(size, size, size, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &CloudDetailTexture, nullptr)))
    {
        CloudDetailTexture = nullptr;
        return nullptr;
    }
    D3DLOCKED_BOX locked = {};
    if (SUCCEEDED(CloudDetailTexture->LockBox(0, &locked, nullptr, 0)))
    {
        for (int z = 0; z < size; ++z)
            for (int y = 0; y < size; ++y)
            {
                auto row = static_cast<uint8_t*>(locked.pBits) + z * locked.SlicePitch + y * locked.RowPitch;
                for (int x = 0; x < size; ++x)
                {
                    const size_t i = (z * size + y) * size + x;
                    const float r = value[0][i], g = value[1][i];
                    if (wide)
                    {
                        // G16R16: red in the low word.
                        reinterpret_cast<uint16_t*>(row)[x * 2] = static_cast<uint16_t>(r * 65535.0f + 0.5f);
                        reinterpret_cast<uint16_t*>(row)[x * 2 + 1] = static_cast<uint16_t>(g * 65535.0f + 0.5f);
                    }
                    else
                    {
                        // A8R8G8B8: blue, green, red, alpha in memory.
                        row[x * 4 + 0] = 0;
                        row[x * 4 + 1] = static_cast<uint8_t>(g * 255.0f + 0.5f);
                        row[x * 4 + 2] = static_cast<uint8_t>(r * 255.0f + 0.5f);
                        row[x * 4 + 3] = 255;
                    }
                }
            }
        CloudDetailTexture->UnlockBox(0);
    }
    return CloudDetailTexture;
}

// The weathers' cloud layers, in CWeather::eWeatherType order:
// - coverage, the share of the sky the clouds take (the middle of the weather's range, which the
//   weather map spreads either way), base and thickness in metres, density against
//   VolumetricCloudsDensity, the share of overcast sheet, and the wind against CloudShadowsWind;
// - the light's absorption inside them, how much more their thin parts let through and how much the
//   billows eat their edges, each against its VolumetricClouds* setting, and the glow around the sun;
// - how much brighter the cloud near the sun is, how fast they reshape against
//   VolumetricCloudsEvolution, how round their bases' edges are (a threshold on the density, so a
//   few tenths at most: at a half they took a fifth of the clouds away), and their brightness
//   against VolumetricCloudsSkyMatch (in CLOUDY at 1 the clouds came out white on a dark sky), and
//   how softly their density rises from the edges in (smoky edges in cloudy and windy weather).
// The billows eat through the whole cloud, not only its edges, so it breaks into ragged pieces with
// gaps; the wet and cloudy weathers' detail is half what it was before that, or their decks lost up
// to a third of their cover and the rain's opened up.
// Fair weather: heaps from 600 to 700 m up, ragged and see-through at the edges, with bright rims,
// barely reshaping; their layer 900 to 1200 m thick, so they build up in towers of rounded lobes
// (at 550 to 700 m they came out as broad flat loaves). Rain and storms: a low, thick, closed deck, an
// overcast sheet over most of it, smooth, dense, with dark bases and little glow, churning (held
// to 3). Fog has no clouds.
static constexpr PostFxResource::CloudLayer kWeatherClouds[8] =
{
    //  cover   base   thick   dens  strat  wind   abs   transl detail glow  sun   evol  round  match  soft
    { 0.25f,  700.0f,  900.0f, 1.67f, 0.0f, 2.00f, 0.8f, 1.3f, 1.2f,  7.0f, 0.70f, 0.4f, 0.10f, 1.00f, 0.0f }, // EXTRASUNNY
    { 0.40f,  600.0f, 1200.0f, 1.33f, 0.0f, 1.00f, 0.9f, 1.2f, 1.1f,  6.0f, 0.55f, 0.7f, 0.15f, 1.00f, 0.0f }, // SUNNY
    { 0.45f,  700.0f, 1100.0f, 0.67f, 0.0f, 1.67f, 1.0f, 1.2f, 0.87f, 6.0f, 0.50f, 0.9f, 0.12f, 1.00f, 1.0f }, // SUNNY_WINDY
    { 0.70f,  500.0f,  900.0f, 1.00f, 0.2f, 0.67f, 1.3f, 1.0f, 0.5f,  4.0f, 0.20f, 1.3f, 0.15f, 0.65f, 1.0f }, // CLOUDY
    { 0.95f,  300.0f, 1000.0f, 0.83f, 0.6f, 0.33f, 2.0f, 0.5f, 0.3f,  2.0f, 0.25f, 3.0f, 0.15f, 1.00f, 0.0f }, // RAIN
    { 0.85f,  400.0f,  900.0f, 0.67f, 0.4f, 1.67f, 1.6f, 0.7f, 0.4f,  3.0f, 0.40f, 0.8f, 0.15f, 0.80f, 1.0f }, // DRIZZLE
    { 0.00f,  600.0f,  600.0f, 0.20f, 0.3f, 1.67f, 1.0f, 1.0f, 0.8f,  3.0f, 0.30f, 0.4f, 0.15f, 1.00f, 0.0f }, // FOGGY
    { 0.95f,  300.0f, 1200.0f, 0.80f, 0.5f, 1.67f, 2.2f, 0.5f, 0.35f, 2.0f, 0.25f, 1.4f, 0.08f, 1.00f, 0.0f }, // LIGHTNING
};

void PostFxResource::UpdateCloudLayer(double seconds)
{
    // Each game starts the clouds somewhere else: how far the wind has carried them, how far the
    // map has morphed and the billows have turned over. From zero, every session opened on the same
    // sky.
    if (!bCloudDriftSeeded)
    {
        std::mt19937_64 random(std::random_device{}());
        std::uniform_real_distribution<double> unit(0.0, 1.0);
        fCloudDrift = unit(random) * 1000.0;
        fCloudDetailDrift = unit(random) * 1000.0;
        fCloudEvolution = unit(random) * 1000.0;
        fCloudMorph = unit(random) * 31.415926535897932;
        bCloudDriftSeeded = true;
    }
    if (bVolumetricCloudsWeather && CWeather::OldWeatherType && CWeather::NewWeatherType && CWeather::InterpolationValue)
    {
        const auto from = static_cast<uint32_t>(*CWeather::OldWeatherType);
        const auto to = static_cast<uint32_t>(*CWeather::NewWeatherType);
        const float k = std::clamp(*CWeather::InterpolationValue, 0.0f, 1.0f);
        const auto& a = kWeatherClouds[from < 8 ? from : 1];
        const auto& b = kWeatherClouds[to < 8 ? to : 1];
        auto mix = [k](float x, float y) { return x + (y - x) * k; };
        Cloud = { mix(a.coverage, b.coverage), mix(a.base, b.base), mix(a.thickness, b.thickness),
                  mix(a.density, b.density), mix(a.stratus, b.stratus), mix(a.wind, b.wind),
                  mix(a.absorption, b.absorption), mix(a.translucency, b.translucency), mix(a.detail, b.detail), mix(a.glow, b.glow),
                  mix(a.sunPower, b.sunPower), mix(a.evolution, b.evolution), mix(a.baseRound, b.baseRound), mix(a.skyMatch, b.skyMatch),
                  mix(a.softness, b.softness) };
    }
    else
        Cloud = { fVolumetricCloudsCoverage, fVolumetricCloudsBase, fVolumetricCloudsThickness, 1.0f, 0.0f, 1.0f, 1.0f, 1.0f, 1.0f, 6.0f, 0.5f, 1.0f, 0.5f, 1.0f, 0.0f };

    // The drift moves on by this frame's time at this frame's wind; across a jump of the clock (a
    // load, a cutscene) it stays where it was.
    double dt = fCloudLastSeconds >= 0.0 ? seconds - fCloudLastSeconds : 0.0;
    if (dt < 0.0 || dt > 0.25)
        dt = 0.0;
    fCloudLastSeconds = seconds;
    const double wind = fCloudShadowsWind * Cloud.wind;
    fCloudDrift = std::fmod(fCloudDrift + dt * wind / fCloudShadowsScale, 1000.0);
    fCloudDetailDrift = std::fmod(fCloudDetailDrift + dt * wind * 0.5 / fVolumetricCloudsDetailScale, 1000.0);
    // About a metre a second up through the detail at 1: the billows turn over in a few minutes.
    fCloudEvolution = std::fmod(fCloudEvolution + dt * fVolumetricCloudsEvolution * Cloud.evolution / fVolumetricCloudsDetailScale, 1000.0);
    fCloudMorph = std::fmod(fCloudMorph + dt * 0.03 * fVolumetricCloudsEvolution * Cloud.evolution, 31.415926535897932);
}

PostFxResource PostFxResources;

// For now: a trace of SSR's frames, to find why reflections show in the pause menu and not in
// play. Ctrl+Shift+F11 writes the settings and the next kFrames frames into
// GTAIV.EFLC.FusionFix.PostFx.log (PostFx.SSRTrace) next to the plugin: every call of the passes around SSR with its viewport, where it left and what
// D3D returned, what lighting gets on s3, and for the first frames what the SSR targets hold,
// read back from the card. The post fx pass arms and flushes it once a frame; the rest only adds
// lines while it is armed.
namespace SSRTrace
{
    static std::mutex mutex;
    static std::string text;
    static std::atomic<int> framesLeft{0};
    static std::atomic<int> readbacksLeft{0};
    static constexpr int kFrames = 8;
    static bool keyWasDown = false;

    static bool Active() { return framesLeft.load(std::memory_order_relaxed) > 0; }

    static void Line(const char* format, ...)
    {
        if (!Active())
            return;
        char line[1024];
        va_list args;
        va_start(args, format);
        vsnprintf(line, sizeof(line), format, args);
        va_end(args);
        std::lock_guard lock(mutex);
        text += std::to_string(GetTickCount64()) + " scene " + std::to_string(FrameHistory::Frame()) + " left " +
            std::to_string(framesLeft.load(std::memory_order_relaxed)) + " " + line + "\n";
    }

    static float Half(uint16_t h)
    {
        const uint32_t sign = (h & 0x8000u) << 16, exponent = (h >> 10) & 0x1F, mantissa = h & 0x3FF;
        uint32_t bits;
        if (exponent == 0)
        {
            if (!mantissa) bits = sign;
            else
            {
                float f = std::ldexp(float(mantissa), -24);
                return (h & 0x8000u) ? -f : f;
            }
        }
        else if (exponent == 31)
            bits = sign | 0x7F800000u | (mantissa << 13);
        else
            bits = sign | ((exponent + 112) << 23) | (mantissa << 13);
        float f;
        std::memcpy(&f, &bits, 4);
        return f;
    }

    // What a render target holds, read back from the card: its size and format, the pixels with
    // alpha over 0.01, those not finite, the largest colour and alpha and the mean alpha.
    static void Contents(IDirect3DDevice9* pDevice, const char* name, IDirect3DTexture9* texture)
    {
        if (!Active() || readbacksLeft.load(std::memory_order_relaxed) <= 0)
            return;
        if (!texture)
        {
            Line("  %s: none", name);
            return;
        }
        IDirect3DSurface9* surface = nullptr;
        if (FAILED(texture->GetSurfaceLevel(0, &surface)) || !surface)
        {
            Line("  %s: no surface", name);
            return;
        }
        D3DSURFACE_DESC desc = {};
        surface->GetDesc(&desc);
        IDirect3DSurface9* copy = nullptr;
        HRESULT hr = pDevice->CreateOffscreenPlainSurface(desc.Width, desc.Height, desc.Format, D3DPOOL_SYSTEMMEM, &copy, nullptr);
        if (SUCCEEDED(hr))
            hr = pDevice->GetRenderTargetData(surface, copy);
        D3DLOCKED_RECT locked = {};
        if (SUCCEEDED(hr))
            hr = copy->LockRect(&locked, nullptr, D3DLOCK_READONLY);
        if (FAILED(hr))
        {
            Line("  %s: %ux%u format %u, read back failed %08x", name, desc.Width, desc.Height, unsigned(desc.Format), unsigned(hr));
            SAFE_RELEASE(copy);
            surface->Release();
            return;
        }
        uint32_t lit = 0, bad = 0;
        float maxColour = 0.0f, maxAlpha = 0.0f;
        double sumAlpha = 0.0;
        const bool isHalf = desc.Format == D3DFMT_A16B16G16R16F, isFixed = desc.Format == D3DFMT_A16B16G16R16;
        for (UINT y = 0; y < desc.Height && (isHalf || isFixed); ++y)
        {
            auto row = reinterpret_cast<const uint16_t*>(static_cast<const uint8_t*>(locked.pBits) + y * locked.Pitch);
            for (UINT x = 0; x < desc.Width; ++x)
            {
                float v[4];
                for (int c = 0; c < 4; ++c)
                    v[c] = isHalf ? Half(row[x * 4 + c]) : row[x * 4 + c] / 65535.0f;
                if (!std::isfinite(v[0]) || !std::isfinite(v[1]) || !std::isfinite(v[2]) || !std::isfinite(v[3]))
                {
                    ++bad;
                    continue;
                }
                maxColour = (std::max)({ maxColour, v[0], v[1], v[2] });
                maxAlpha = (std::max)(maxAlpha, v[3]);
                sumAlpha += v[3];
                if (v[3] > 0.01f)
                    ++lit;
            }
        }
        copy->UnlockRect();
        copy->Release();
        surface->Release();
        Line("  %s: %ux%u format %u, alpha>0.01 %u, not finite %u, max colour %.3f, max alpha %.3f, mean alpha %.4f%s", name,
             desc.Width, desc.Height, unsigned(desc.Format), lit, bad, maxColour, maxAlpha,
             desc.Width * desc.Height ? sumAlpha / (double(desc.Width) * desc.Height) : 0.0, isHalf || isFixed ? "" : " (format not read)");
    }

    // The first channel of a texture at five points (the centre and halfway to each corner), as stored and as metres
    // through near and far the way SSR.fx decodes the log depth, to compare this frame's depth with last frame's copy.
    static void DepthProbe(IDirect3DDevice9* pDevice, const char* name, IDirect3DTexture9* texture, float nearClip, float farClip)
    {
        if (!Active())
            return;
        IDirect3DSurface9* surface = nullptr;
        if (!texture || FAILED(texture->GetSurfaceLevel(0, &surface)) || !surface)
        {
            Line("  %s probe: no surface", name);
            return;
        }
        D3DSURFACE_DESC desc = {};
        surface->GetDesc(&desc);
        IDirect3DSurface9* copy = nullptr;
        HRESULT hr = pDevice->CreateOffscreenPlainSurface(desc.Width, desc.Height, desc.Format, D3DPOOL_SYSTEMMEM, &copy, nullptr);
        if (SUCCEEDED(hr))
            hr = pDevice->GetRenderTargetData(surface, copy);
        D3DLOCKED_RECT locked = {};
        if (SUCCEEDED(hr))
            hr = copy->LockRect(&locked, nullptr, D3DLOCK_READONLY);
        if (FAILED(hr))
        {
            Line("  %s probe: %ux%u format %u, read back failed %08x", name, desc.Width, desc.Height, unsigned(desc.Format), unsigned(hr));
            SAFE_RELEASE(copy);
            surface->Release();
            return;
        }
        auto read = [&](UINT x, UINT y) -> float
        {
            auto row = static_cast<const uint8_t*>(locked.pBits) + y * locked.Pitch;
            switch (desc.Format)
            {
            case D3DFMT_R32F: return reinterpret_cast<const float*>(row)[x];
            case D3DFMT_G32R32F: return reinterpret_cast<const float*>(row)[x * 2];
            case D3DFMT_A32B32G32R32F: return reinterpret_cast<const float*>(row)[x * 4];
            case D3DFMT_R16F: return Half(reinterpret_cast<const uint16_t*>(row)[x]);
            case D3DFMT_G16R16F: return Half(reinterpret_cast<const uint16_t*>(row)[x * 2]);
            case D3DFMT_A16B16G16R16F: return Half(reinterpret_cast<const uint16_t*>(row)[x * 4]);
            case D3DFMT_A8R8G8B8: case D3DFMT_X8R8G8B8: return row[x * 4 + 2] / 255.0f;
            default: return -1.0f;
            }
        };
        std::string text;
        const float at[5][2] = { { 0.5f, 0.5f }, { 0.25f, 0.25f }, { 0.75f, 0.25f }, { 0.25f, 0.75f }, { 0.75f, 0.75f } };
        for (auto& p : at)
        {
            const float raw = read(UINT(p[0] * desc.Width), UINT(p[1] * desc.Height));
            const float metres = nearClip > 0.0f ? nearClip * std::pow(farClip / nearClip, raw) : 0.0f;
            char item[64];
            snprintf(item, sizeof(item), " (%.2f,%.2f) %.6f = %.2f m", p[0], p[1], raw, metres);
            text += item;
        }
        copy->UnlockRect();
        copy->Release();
        surface->Release();
        Line("  %s probe: %ux%u format %u:%s", name, desc.Width, desc.Height, unsigned(desc.Format), text.c_str());
    }

    // All four channels of a half float target at five points (the centre and halfway to each corner).
    static void PixelProbe(IDirect3DDevice9* pDevice, const char* name, IDirect3DTexture9* texture)
    {
        if (!Active())
            return;
        IDirect3DSurface9* surface = nullptr;
        if (!texture || FAILED(texture->GetSurfaceLevel(0, &surface)) || !surface)
            return;
        D3DSURFACE_DESC desc = {};
        surface->GetDesc(&desc);
        IDirect3DSurface9* copy = nullptr;
        HRESULT hr = desc.Format == D3DFMT_A16B16G16R16F
            ? pDevice->CreateOffscreenPlainSurface(desc.Width, desc.Height, desc.Format, D3DPOOL_SYSTEMMEM, &copy, nullptr) : E_FAIL;
        if (SUCCEEDED(hr))
            hr = pDevice->GetRenderTargetData(surface, copy);
        D3DLOCKED_RECT locked = {};
        if (SUCCEEDED(hr))
            hr = copy->LockRect(&locked, nullptr, D3DLOCK_READONLY);
        if (FAILED(hr))
        {
            Line("  %s probe: %ux%u format %u, read back failed %08x", name, desc.Width, desc.Height, unsigned(desc.Format), unsigned(hr));
            SAFE_RELEASE(copy);
            surface->Release();
            return;
        }
        std::string text;
        const float at[5][2] = { { 0.5f, 0.5f }, { 0.25f, 0.25f }, { 0.75f, 0.25f }, { 0.25f, 0.75f }, { 0.75f, 0.75f } };
        for (auto& p : at)
        {
            auto row = reinterpret_cast<const uint16_t*>(static_cast<const uint8_t*>(locked.pBits) + UINT(p[1] * desc.Height) * locked.Pitch);
            auto px = row + UINT(p[0] * desc.Width) * 4;
            char item[96];
            snprintf(item, sizeof(item), " (%.2f,%.2f) %.3f %.3f %.3f %.0f", p[0], p[1], Half(px[0]), Half(px[1]), Half(px[2]), Half(px[3]));
            text += item;
        }
        copy->UnlockRect();
        copy->Release();
        surface->Release();
        Line("  %s probe: %ux%u:%s", name, desc.Width, desc.Height, text.c_str());
    }

    // A surface by the render target it belongs to, FusionFix's or the game's, or the full size depth buffer RenderScale
    // stands in with.
    static std::string SurfaceName(IDirect3DSurface9* surface)
    {
        if (!surface)
            return "none";
        for (auto& [name, rt] : rage::grcTextureFactoryPC::RTCache)
        {
            if (!rt)
                continue;
            if (rt->mD3DSurface == surface)
                return name;
            IDirect3DSurface9* level = nullptr;
            if (rt->mD3DTexture && SUCCEEDED(rt->mD3DTexture->GetSurfaceLevel(0, &level)) && level)
            {
                const bool same = level == surface;
                level->Release();
                if (same)
                    return name;
            }
        }
        D3DSURFACE_DESC desc = {};
        surface->GetDesc(&desc);
        char buffer[64];
        snprintf(buffer, sizeof(buffer), "%p %ux%u format %u", static_cast<void*>(surface), desc.Width, desc.Height, unsigned(desc.Format));
        return buffer;
    }

    static std::string TextureName(IDirect3DBaseTexture9* texture)
    {
        auto& R = PostFxResources;
        if (!texture) return "none";
        const std::pair<rage::grcRenderTargetPC*, const char*> known[] = {
            { R.SSRTex, "SSRTex" }, { R.SSRHalfTex, "SSRHalfTex" }, { R.SSRDenoisedTex, "SSRDenoisedTex" },
            { R.SSRHalfDenoisedTex, "SSRHalfDenoisedTex" }, { R.SSRAccumTex[0][0], "SSRAccumTex0" }, { R.SSRAccumTex[0][1], "SSRAccumTex1" },
            { R.SSRAccumTex[1][0], "SSRHalfAccumTex0" }, { R.SSRAccumTex[1][1], "SSRHalfAccumTex1" }, { R.SSRTraceTex[0], "SSRTraceTex" },
            { R.SSRTraceTex[1], "SSRHalfTraceTex" }, { R.SSRHistoryTex, "SSRHistoryTex" }, { R.SSRFallbackTex[0], "SSRFallbackTex" },
            { R.SSRFallbackTex[1], "SSRHalfFallbackTex" }, { R.SSRSpreadTex[0], "SSRSpreadTex" }, { R.SSRSpreadTex[1], "SSRHalfSpreadTex" },
            { R.SSRHitDistTex[0], "SSRHitDistTex" }, { R.SSRHitDistTex[1], "SSRHalfHitDistTex" },
            { R.mDepthRT, "depth" }, { R.mSpecularRT, "specular" }, { R.mNormalRT, "normal" }, { R.PreAlphaDepthCopyRT, "prevDepth" } };
        for (auto [rt, name] : known)
            if (rt && rt->mD3DTexture == texture)
                return name;
        // The game's own targets by the name they were created with
        for (auto& [name, rt] : rage::grcTextureFactoryPC::RTCache)
            if (rt && rt->mD3DTexture == texture)
                return "game:" + name;
        char buffer[32];
        snprintf(buffer, sizeof(buffer), "%p", static_cast<void*>(texture));
        return buffer;
    }

    // The device state a draw runs with: render target, viewport, the states that could keep it
    // from writing, and what every sampler holds.
    static void State(IDirect3DDevice9* pDevice, const char* what)
    {
        if (!Active())
            return;
        IDirect3DSurface9* rt = nullptr;
        pDevice->GetRenderTarget(0, &rt);
        // The texture whose top level the target is, among FusionFix's own; none for another.
        IDirect3DBaseTexture9* rtTexture = nullptr;
        auto& R = PostFxResources;
        for (auto* known : { R.SSRTex, R.SSRHalfTex, R.SSRDenoisedTex, R.SSRHalfDenoisedTex, R.SSRAccumTex[0][0], R.SSRAccumTex[0][1],
                             R.SSRAccumTex[1][0], R.SSRAccumTex[1][1], R.SSRTraceTex[0], R.SSRTraceTex[1], R.SSRFallbackTex[0],
                             R.SSRFallbackTex[1], R.SSRSpreadTex[0], R.SSRSpreadTex[1] })
        {
            IDirect3DSurface9* level = nullptr;
            if (rt && known && known->mD3DTexture && SUCCEEDED(known->mD3DTexture->GetSurfaceLevel(0, &level)) && level)
            {
                if (level == rt)
                    rtTexture = known->mD3DTexture;
                level->Release();
            }
        }
        D3DVIEWPORT9 view = {};
        pDevice->GetViewport(&view);
        DWORD v[10] = {};
        const D3DRENDERSTATETYPE states[10] = { D3DRS_COLORWRITEENABLE, D3DRS_ALPHABLENDENABLE, D3DRS_SRCBLEND, D3DRS_DESTBLEND,
            D3DRS_ALPHATESTENABLE, D3DRS_SCISSORTESTENABLE, D3DRS_STENCILENABLE, D3DRS_ZENABLE, D3DRS_SRGBWRITEENABLE, D3DRS_SEPARATEALPHABLENDENABLE };
        for (int i = 0; i < 10; ++i)
            pDevice->GetRenderState(states[i], &v[i]);
        RECT scissor = {};
        pDevice->GetScissorRect(&scissor);
        std::string samplers;
        for (DWORD slot = 0; slot < 16; ++slot)
        {
            IDirect3DBaseTexture9* t = nullptr;
            pDevice->GetTexture(slot, &t);
            if (t)
            {
                samplers += " s" + std::to_string(slot) + "=" + TextureName(t) + (t == rtTexture ? "(TARGET)" : "");
                t->Release();
            }
        }
        // Whether a texture set now takes: one set on s15 and read back, then put back.
        const char* takes = "?";
        if (rtTexture || R.SSRHistoryTex)
        {
            IDirect3DBaseTexture9* old15 = nullptr;
            pDevice->GetTexture(15, &old15);
            IDirect3DBaseTexture9* probe = R.SSRHistoryTex ? R.SSRHistoryTex->mD3DTexture : rtTexture;
            if (probe == old15)
                probe = nullptr;
            SetTextureBoth(pDevice, 15, probe);
            IDirect3DBaseTexture9* now15 = nullptr;
            pDevice->GetTexture(15, &now15);
            takes = now15 == probe ? "yes" : "NO";
            SAFE_RELEASE(now15);
            SetTextureBoth(pDevice, 15, old15);
            SAFE_RELEASE(old15);
        }
        IDirect3DPixelShader9* ps = nullptr;
        pDevice->GetPixelShader(&ps);
        Line("  set texture takes: %s", takes);
        Line("  state %s: target %s view %ux%u+%u+%u write %x blend %u (%u,%u) test %u scissor %u (%d,%d,%d,%d) stencil %u z %u srgb %u sepalpha %u ps %p;%s",
            what, TextureName(rtTexture).c_str(), unsigned(view.Width), unsigned(view.Height), unsigned(view.X), unsigned(view.Y),
            unsigned(v[0]), unsigned(v[1]), unsigned(v[2]), unsigned(v[3]), unsigned(v[4]), unsigned(v[5]), int(scissor.left), int(scissor.top),
            int(scissor.right), int(scissor.bottom), unsigned(v[6]), unsigned(v[7]), unsigned(v[8]), unsigned(v[9]), static_cast<void*>(ps), samplers.c_str());
        SAFE_RELEASE(ps);
        SAFE_RELEASE(rt);
    }

    // Once a frame, from the post fx pass, which runs in the pause menu too, where the game does
    // not tick: Ctrl+Shift+F11 arms the trace with the settings; it is written once the frames
    // are done.
    static void Tick()
    {
        const bool down = (GetAsyncKeyState(VK_CONTROL) & 0x8000) && (GetAsyncKeyState(VK_SHIFT) & 0x8000) &&
            (GetAsyncKeyState(VK_F11) & 0x8000);
        if (down && !keyWasDown && !Active())
        {
            auto& R = PostFxResources;
            auto pref = [](const char* name) { auto p = FusionFixSettings.GetRef(name); return p ? p->get() : -1; };
            {
                std::lock_guard lock(mutex);
                text.clear();
            }
            framesLeft = kFrames;
            readbacksLeft = 2;
            Line("=== trace: menu SSR %d debug %d AO %d SSGI %d contact %d skin %d; ini intensity %.2f max distance %.1f fallback %.2f "
                 "temporal %.2f denoise %.1f; active %ux%u",
                 pref("PREF_SSR"), pref("PREF_SSR_DEBUG"), pref("PREF_SAO"), pref("PREF_SSGI"), pref("PREF_CONTACTSHADOWS"),
                 pref("PREF_SKIN_SSS"), R.fSSRIntensity, R.fSSRMaxDistance, R.fSSRFallback, R.fSSRTemporalBlend, R.fSSRDenoiseRadius,
                 unsigned(rage::grcDevice::ms_nActiveWidth ? *rage::grcDevice::ms_nActiveWidth : 0),
                 unsigned(rage::grcDevice::ms_nActiveHeight ? *rage::grcDevice::ms_nActiveHeight : 0));
            Line("targets: trace %p/%p fallback %p/%p spread %p/%p ssr %p half %p effect %p history %p depth %p",
                 static_cast<void*>(R.SSRTraceSurf[0]), static_cast<void*>(R.SSRTraceSurf[1]), static_cast<void*>(R.SSRFallbackSurf[0]),
                 static_cast<void*>(R.SSRFallbackSurf[1]), static_cast<void*>(R.SSRSpreadSurf[0]), static_cast<void*>(R.SSRSpreadSurf[1]),
                 static_cast<void*>(R.SSRSurf), static_cast<void*>(R.SSRHalfSurf), static_cast<void*>(R.SSREffect),
                 static_cast<void*>(R.SSRHistoryTex), static_cast<void*>(R.mDepthRT));
        }
        keyWasDown = down;
        if (!Active())
            return;
        if (framesLeft.fetch_sub(1) == 1)
        {
            std::lock_guard lock(mutex);
            // Written at once when the trace ends; each line keeps the tick it was taken at.
            FusionLog::WriteText("PostFx", "SSRTrace", text);
            text.clear();
        }
    }
}

class PostFX
{
private:
    struct VertexFormat
    {
        float Pos[3];
        float TexCoord[2];
    };

    static inline IDirect3DVertexBuffer9* mQuadVertexBuffer;
    static inline IDirect3DVertexDeclaration9* mQuadVertexDecl;

    static inline UINT lastoffset = 0;
    static inline UINT laststride = 0;

    static inline IDirect3DVertexBuffer9* last_VertexBuffer = 0;
    static inline IDirect3DVertexDeclaration9* last_VertexDecl = 0;

    static inline IDirect3DBaseTexture9* prePostFx[PostfxTextureCount] = { 0 };
    static inline DWORD Samplers[PostfxTextureCount] = { D3DTEXF_LINEAR };
    static inline HMODULE hm = NULL;

    static void saveRenderState()
    {
        IDirect3DDevice9* pDevice = rage::grcDevice::GetD3DDevice();
        for (int i = 0; i < PostfxTextureCount; i++)
        {
            pDevice->GetTexture(i, &prePostFx[i]);
            pDevice->GetSamplerState(i, D3DSAMP_MAGFILTER, &Samplers[i]);
        }
    }

    static void restoreRenderState()
    {
        IDirect3DDevice9* pDevice = rage::grcDevice::GetD3DDevice();
        for (int i = 0; i < PostfxTextureCount; i++)
        {
            SetTextureBoth(pDevice, i, prePostFx[i]);
            pDevice->SetSamplerState(i, D3DSAMP_MAGFILTER, Samplers[i]);
            SAFE_RELEASE(prePostFx[i]);
        }
    }

    // SSR, contact shadow and debug targets are D3DPOOL_DEFAULT, and the SSR effect keeps a
    // reference to every texture set on it. Reset fails while any of that is still held, so
    // this runs on device loss, before Reset, and again ahead of recreating them.
    static void ReleaseScreenSpaceTargets()
    {
        if (auto effect = PostFxResources.SSREffect)
        {
            D3DXEFFECT_DESC effectDesc = {};
            if (SUCCEEDED(effect->GetDesc(&effectDesc)))
            {
                for (UINT i = 0; i < effectDesc.Parameters; ++i)
                {
                    D3DXHANDLE param = effect->GetParameter(nullptr, i);
                    D3DXPARAMETER_DESC paramDesc = {};
                    if (param && SUCCEEDED(effect->GetParameterDesc(param, &paramDesc)) &&
                        paramDesc.Type >= D3DXPT_TEXTURE && paramDesc.Type <= D3DXPT_TEXTURECUBE)
                        effect->SetTexture(param, nullptr);
                }
            }
        }
        if (auto pDevice = rage::grcDevice::GetD3DDevice())
        {
            SetTextureBoth(pDevice, 3, nullptr);
            SetTextureBoth(pDevice, 8, nullptr);
            SetTextureBoth(pDevice, 9, nullptr);
            SetTextureBoth(pDevice, 11, nullptr);
        }
        PostFxResources.bGIBound = false;
        PostFxResources.bMaterialIdBound = false;
        SAFE_RELEASE(PostFxResources.SSRSurf);
        if (PostFxResources.SSRTex)
        {
            PostFxResources.SSRTex->Destroy();
            PostFxResources.SSRTex = nullptr;
        }
        SAFE_RELEASE(PostFxResources.SSRHistorySurf);
        if (PostFxResources.SSRHistoryTex)
        {
            PostFxResources.SSRHistoryTex->Destroy();
            PostFxResources.SSRHistoryTex = nullptr;
        }
        SAFE_RELEASE(PostFxResources.SSRDenoisedSurf);
        if (PostFxResources.SSRDenoisedTex)
        {
            PostFxResources.SSRDenoisedTex->Destroy();
            PostFxResources.SSRDenoisedTex = nullptr;
        }
        PostFxResources.bSSRDenoised = false;
        SAFE_RELEASE(PostFxResources.SSRHalfSurf);
        SAFE_RELEASE(PostFxResources.SSRHalfDenoisedSurf);
        SAFE_RELEASE(PostFxResources.SSRMarchDepthSurf);
        for (int i = 0; i < 2; ++i)
        {
            SAFE_RELEASE(PostFxResources.SSRTraceSurf[i]);
            SAFE_RELEASE(PostFxResources.SSRFallbackSurf[i]);
            SAFE_RELEASE(PostFxResources.SSRSpreadSurf[i]);
            SAFE_RELEASE(PostFxResources.SSRHitDistSurf[i]);
        }
        for (auto* rt : { &PostFxResources.SSRHalfTex, &PostFxResources.SSRHalfDenoisedTex, &PostFxResources.SSRTraceTex[0],
                          &PostFxResources.SSRTraceTex[1], &PostFxResources.SSRFallbackTex[0], &PostFxResources.SSRFallbackTex[1],
                          &PostFxResources.SSRSpreadTex[0], &PostFxResources.SSRSpreadTex[1], &PostFxResources.SSRHitDistTex[0],
                          &PostFxResources.SSRHitDistTex[1], &PostFxResources.SSRMarchDepthTex })
        {
            if (*rt)
            {
                (*rt)->Destroy();
                *rt = nullptr;
            }
        }
        PostFxResources.SSRResult = nullptr;
        for (int i = 0; i < 2; ++i)
        {
            SAFE_RELEASE(PostFxResources.AOAccumSurf[i]);
            if (PostFxResources.AOAccumTex[i])
            {
                PostFxResources.AOAccumTex[i]->Destroy();
                PostFxResources.AOAccumTex[i] = nullptr;
            }
        }
        PostFxResources.nAOAccumFrame = 0;
        for (auto* rt : { &PostFxResources.ContactRawTex, &PostFxResources.ContactRawHalfTex, &PostFxResources.ContactTex,
                          &PostFxResources.ContactAccumTex[0], &PostFxResources.ContactAccumTex[1] })
        {
            if (*rt)
            {
                (*rt)->Destroy();
                *rt = nullptr;
            }
        }
        SAFE_RELEASE(PostFxResources.ContactRawSurf);
        SAFE_RELEASE(PostFxResources.ContactRawHalfSurf);
        SAFE_RELEASE(PostFxResources.ContactSurf);
        SAFE_RELEASE(PostFxResources.ContactAccumSurf[0]);
        SAFE_RELEASE(PostFxResources.ContactAccumSurf[1]);
        PostFxResources.ContactResult = nullptr;
        PostFxResources.nContactAccumFrame = 0;
        PostFxResources.bContactValid = false;
        PostFxResources.bGlassFrameValid = false;
        SAFE_RELEASE(PostFxResources.SSRDebugSurf);
        if (PostFxResources.SSRDebugTex)
        {
            PostFxResources.SSRDebugTex->Destroy();
            PostFxResources.SSRDebugTex = nullptr;
        }
        for (int half = 0; half < 2; ++half)
            for (int i = 0; i < 2; ++i)
            {
                SAFE_RELEASE(PostFxResources.SSRAccumSurf[half][i]);
                if (PostFxResources.SSRAccumTex[half][i])
                {
                    PostFxResources.SSRAccumTex[half][i]->Destroy();
                    PostFxResources.SSRAccumTex[half][i] = nullptr;
                }
            }
        PostFxResources.nSSRAccumFrame = 0;
        for (auto* rt : { &PostFxResources.GIRawTex, &PostFxResources.GIDenoisedTex, &PostFxResources.GIAccumTex[0], &PostFxResources.GIAccumTex[1], &PostFxResources.GIFullTex })
        {
            if (*rt)
            {
                (*rt)->Destroy();
                *rt = nullptr;
            }
        }
        SAFE_RELEASE(PostFxResources.GIRawSurf);
        SAFE_RELEASE(PostFxResources.GIDenoisedSurf);
        SAFE_RELEASE(PostFxResources.GIAccumSurf[0]);
        SAFE_RELEASE(PostFxResources.GIAccumSurf[1]);
        SAFE_RELEASE(PostFxResources.GIFullSurf);
        PostFxResources.GIResult = nullptr;
        SAFE_RELEASE(PostFxResources.SceneDepthSurf);
        if (PostFxResources.SceneDepthTex)
        {
            PostFxResources.SceneDepthTex->Destroy();
            PostFxResources.SceneDepthTex = nullptr;
        }
        PostFxResources.nSceneDepthFrame = 0;
        for (int i = 0; i < 3; ++i)
        {
            SAFE_RELEASE(PostFxResources.CloudSurf[i]);
            if (PostFxResources.CloudTex[i])
            {
                PostFxResources.CloudTex[i]->Destroy();
                PostFxResources.CloudTex[i] = nullptr;
            }
        }
        PostFxResources.nCloudAccumFrame = 0;
        SAFE_RELEASE(PostFxResources.CloudSkyRefSurf);
        if (PostFxResources.CloudSkyRefTex)
        {
            PostFxResources.CloudSkyRefTex->Destroy();
            PostFxResources.CloudSkyRefTex = nullptr;
        }
        for (int i = 0; i < 2; ++i)
        {
            SAFE_RELEASE(PostFxResources.CloudMarchSurf[i]);
            if (PostFxResources.CloudMarchTex[i])
            {
                PostFxResources.CloudMarchTex[i]->Destroy();
                PostFxResources.CloudMarchTex[i] = nullptr;
            }
        }
        for (int i = 0; i < 2; ++i)
        {
            SAFE_RELEASE(PostFxResources.SkinLightSurf[i]);
            if (PostFxResources.SkinLightTex[i])
            {
                PostFxResources.SkinLightTex[i]->Destroy();
                PostFxResources.SkinLightTex[i] = nullptr;
            }
        }
        PostFxResources.nGIAccumFrame = 0;
        PostFxResources.bSSRDebugValid = false;
        PostFxResources.bSSRValidThisFrame = false;
        PostFxResources.nSSRHistoryFrame = 0;
        PostFxResources.bSSRReprojValid = false;
    }

    static void __fastcall OnDeviceLost()
    {
        PostFxResources.ReleaseTextures();
        PostFxResources.ReleaseWaterMask();
        UnbindGlassReflections();
        SAFE_RELEASE(PostFxResources.GlassParamsTex);
        ReleaseScreenSpaceTargets();
        TemporalAA::ReleaseResources();
        // PostFxResources.mSpecularAoRT    =nullptr;
        PostFxResources.mNormalRT = nullptr;
        PostFxResources.mDiffuseRT = nullptr;
        PostFxResources.mMaterialIdRT = nullptr;
        // PostFxResources.mSpecularRT      =nullptr;
        // PostFxResources.mDepthRT         =nullptr;
        // PostFxResources.mStencilRT       =nullptr;
        PostFxResources.mFullScreenRT = nullptr;
        // PostFxResources.mFullScreenRT2   =nullptr;
        PostFxResources.mHalfScreenRT = nullptr;
        // PostFxResources.mCascadeAtlasRT  =nullptr;
        // PostFxResources.mQuarterScreenRT =nullptr;
        // PostFxResources.mBloomRT         =nullptr;
        // PostFxResources.mCurrentLum      =nullptr;

        if (mQuadVertexBuffer)
        {
            mQuadVertexBuffer->Release();
            mQuadVertexBuffer = nullptr;
        }

        if (mQuadVertexDecl)
        {
            mQuadVertexDecl->Release();
            mQuadVertexDecl = nullptr;
        }
        if (PostFxResources.AOEffect)
            PostFxResources.AOEffect->OnLostDevice();
        if (PostFxResources.SSREffect)
            PostFxResources.SSREffect->OnLostDevice();
        if (PostFxResources.CloudsEffect)
            PostFxResources.CloudsEffect->OnLostDevice();
        EffectBindings().clear();
        if (PostFxResources.WetGroundEffect)
            PostFxResources.WetGroundEffect->OnLostDevice();
        if (PostFxResources.HeadlightGlintsEffect)
            PostFxResources.HeadlightGlintsEffect->OnLostDevice();
        PostFxResources.ReleaseWetCopies();
        ReleaseProfiler();

        for (auto i = 0; i < PostFxResources.nAmbientOcclusionMaxMipLevel; ++i)
            SAFE_RELEASE(PostFxResources.AOCamDepthSurf[i]);
        SAFE_RELEASE(PostFxResources.AOSurf);
        SAFE_RELEASE(PostFxResources.AOBlurSurf);

        if (PostFxResources.AOCamDepthTex)
        {
            PostFxResources.AOCamDepthTex->Destroy();
            PostFxResources.AOCamDepthTex = nullptr;
        }
        if (PostFxResources.AOTex)
        {
            PostFxResources.AOTex->Destroy();
            PostFxResources.AOTex = nullptr;
        }
        if (PostFxResources.AOBlurTex)
        {
            PostFxResources.AOBlurTex->Destroy();
            PostFxResources.AOBlurTex = nullptr;
        }
    }

    // The G-buffer targets, looked up by name once a frame (lighting phase, fog pass) and not only at a
    // device reset: the game creates them anew when the render scale changes, as turning FSR on or off
    // does, and the ones kept from before were another texture by then. SSR and SSGI read a depth that
    // was not this frame's, and the accumulations, testing it against last frame's copy, dropped their
    // history on every pixel.
    static void RefreshGBufferTargets()
    {
        PostFxResources.mNormalRT = rage::grcTextureFactoryPC::GetRTByName("_DEFERRED_GBUFFER_1_");
        PostFxResources.mDiffuseRT = rage::grcTextureFactoryPC::GetRTByName("_DEFERRED_GBUFFER_0_");
        PostFxResources.mSpecularRT = rage::grcTextureFactoryPC::GetRTByName("_DEFERRED_GBUFFER_2_");
        PostFxResources.mDepthRT = rage::grcTextureFactoryPC::GetRTByName("_DEFERRED_GBUFFER_3_");
        // Not the stencil buffer: the G-buffer pass writes each material's ID (whole steps of 1/255)
        // to it, as R32F or R16F, for the lighting and fog shaders.
        PostFxResources.mMaterialIdRT = rage::grcTextureFactoryPC::GetRTByName("_STENCIL_BUFFER_");
    }

    static void __fastcall OnDeviceReset()
    {
        RefreshGBufferTargets();
        // PostFxResources.mCascadeAtlasRT = rage::grcTextureFactoryPC::GetRTByName( "CASCADE_ATLAS"         );
        PostFxResources.mFullScreenRT = rage::grcTextureFactoryPC::GetRTByName("FullScreenCopy");
        // PostFxResources.mFullScreenRT2  = rage::grcTextureFactoryPC::GetRTByName( "FullScreenCopy2"       );
        PostFxResources.mHalfScreenRT = rage::grcTextureFactoryPC::GetRTByName("Quarter Screen 0");
        // PostFxResources.mQuarterScreenRT= rage::grcTextureFactoryPC::GetRTByName( "Blur Screen 0"         );
        // PostFxResources.mBloomRT        = rage::grcTextureFactoryPC::GetRTByName( "Blur Screen 2 Copy"    );
        // PostFxResources.mCurrentLum     = rage::grcTextureFactoryPC::GetRTByName( "Current Lum"           );

        auto width = *rage::grcDevice::ms_nActiveWidth;
        auto height = *rage::grcDevice::ms_nActiveHeight;

        PostFxResources.createTextures(width, height, hm);
        // Motion vectors, depth and the reactive mask at the size the scene renders at
        TemporalAA::CreateResources(RenderScale::ToRenderWidth(width), RenderScale::ToRenderHeight(height));

        D3DVERTEXELEMENT9 vertexDeclElements[] =
        {
            {0, 0, D3DDECLTYPE_FLOAT3, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0},
            {0, 12, D3DDECLTYPE_FLOAT2, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 0},
            D3DDECL_END()
        };

        rage::grcDevice::GetD3DDevice()->CreateVertexDeclaration(vertexDeclElements, &mQuadVertexDecl);

        rage::grcDevice::GetD3DDevice()->CreateVertexBuffer(6 * sizeof(VertexFormat), 0, 0, D3DPOOL_DEFAULT, &mQuadVertexBuffer, NULL);

        VertexFormat* vertexData = 0;
        D3DXVECTOR2 pixelSize = D3DXVECTOR2(1.0f / width, 1.0f / height);

        mQuadVertexBuffer->Lock(0, 0, (void**)&vertexData, 0);

        vertexData[0] = { -1.0f - pixelSize.x, -1.0f + pixelSize.y, 0.0f, 0.0f, 1.0f };
        vertexData[1] = { -1.0f - pixelSize.x,  1.0f + pixelSize.y, 0.0f, 0.0f, 0.0f };
        vertexData[2] = { 1.0f - pixelSize.x, -1.0f + pixelSize.y, 0.0f, 1.0f, 1.0f };
        vertexData[3] = { -1.0f - pixelSize.x,  1.0f + pixelSize.y, 0.0f, 0.0f, 0.0f };
        vertexData[4] = { 1.0f - pixelSize.x,  1.0f + pixelSize.y, 0.0f, 1.0f, 0.0f };
        vertexData[5] = { 1.0f - pixelSize.x, -1.0f + pixelSize.y, 0.0f, 1.0f, 1.0f };

        mQuadVertexBuffer->Unlock();

        // The screen space effects work on the scene, at the size it renders at
        width = static_cast<int32_t>(RenderScale::ToRenderWidth(static_cast<uint32_t>(width)));
        height = static_cast<int32_t>(RenderScale::ToRenderHeight(static_cast<uint32_t>(height)));

        if (PostFxResources.AOEffect)
            PostFxResources.AOEffect->OnResetDevice();
        if (PostFxResources.SSREffect)
            PostFxResources.SSREffect->OnResetDevice();
        if (PostFxResources.CloudsEffect)
            PostFxResources.CloudsEffect->OnResetDevice();
        if (PostFxResources.WetGroundEffect)
            PostFxResources.WetGroundEffect->OnResetDevice();
        if (PostFxResources.HeadlightGlintsEffect)
            PostFxResources.HeadlightGlintsEffect->OnResetDevice();

        for (auto i = 0; i < PostFxResources.nAmbientOcclusionMaxMipLevel; ++i)
            SAFE_RELEASE(PostFxResources.AOCamDepthSurf[i]);
        SAFE_RELEASE(PostFxResources.AOSurf);
        SAFE_RELEASE(PostFxResources.AOBlurSurf);

        if (PostFxResources.AOCamDepthTex)
        {
            PostFxResources.AOCamDepthTex->Destroy();
            PostFxResources.AOCamDepthTex = nullptr;
        }
        if (PostFxResources.AOTex)
        {
            PostFxResources.AOTex->Destroy();
            PostFxResources.AOTex = nullptr;
        }
        if (PostFxResources.AOBlurTex)
        {
            PostFxResources.AOBlurTex->Destroy();
            PostFxResources.AOBlurTex = nullptr;
        }
        ReleaseScreenSpaceTargets();

        auto pDevice = rage::grcDevice::GetD3DDevice();

        auto aoDesc = rage::OwnRenderTargetDesc(rage::GRCFMT_UNKNOWN);

        aoDesc.mFormat = rage::GRCFMT_R32F;
        aoDesc.mLevels = PostFxResources.nAmbientOcclusionMaxMipLevel;
        PostFxResources.AOCamDepthTex = rage::CreateEmptyRenderTarget("AOCamDepthTex", width, height, 32, aoDesc);

        aoDesc.mLevels = 1;
        PostFxResources.SceneDepthTex = rage::CreateEmptyRenderTarget("SceneDepthCopy", width, height, 32, aoDesc, PostFxResources.SceneDepthSurf);
        PostFxResources.nSceneDepthFrame = 0;

        aoDesc.mFormat = rage::GRCFMT_L8;
        aoDesc.mLevels = 1;
        PostFxResources.AOTex = rage::CreateEmptyRenderTarget("AOTex", width, height, 8, aoDesc);
        PostFxResources.AOBlurTex = rage::CreateEmptyRenderTarget("AOBlurTex", width, height, 8, aoDesc);
        if (PostFxResources.fAmbientOcclusionTemporal > 0.0f)
        {
            // 16-bit: blending a small share of each frame into 8 bits gets stuck on its steps.
            aoDesc.mFormat = rage::GRCFMT_R16F;
            static const char* names[2] = { "AOAccumTex0", "AOAccumTex1" };
            for (int i = 0; i < 2; ++i)
                PostFxResources.AOAccumTex[i] = rage::CreateEmptyRenderTarget(names[i], width, height, 16, aoDesc, PostFxResources.AOAccumSurf[i]);
        }

        {
            aoDesc.mFormat = rage::GRCFMT_A16B16G16R16F;
            aoDesc.mLevels = 1;
            PostFxResources.SSRTex = rage::CreateEmptyRenderTarget("SSRTex", width, height, 64, aoDesc, PostFxResources.SSRSurf);

            PostFxResources.SSRHistoryTex = rage::CreateEmptyRenderTarget("SSRHistoryTex", width, height, 64, aoDesc, PostFxResources.SSRHistorySurf);

            PostFxResources.SSRDenoisedTex = rage::CreateEmptyRenderTarget("SSRDenoisedTex", width, height, 64, aoDesc, PostFxResources.SSRDenoisedSurf);

            PostFxResources.SSRHalfTex = rage::CreateEmptyRenderTarget("SSRHalfTex", width / 2, height / 2, 64, aoDesc, PostFxResources.SSRHalfSurf);
            {
                static const char* cloudNames[3] = { "CloudTex", "CloudAccumTex0", "CloudAccumTex1" };
                for (int i = 0; i < 3; ++i)
                    PostFxResources.CloudTex[i] = rage::CreateEmptyRenderTarget(cloudNames[i], width / 2, height / 2, 64, aoDesc, PostFxResources.CloudSurf[i]);
                PostFxResources.nCloudAccumFrame = 0;
                PostFxResources.CloudSkyRefTex = rage::CreateEmptyRenderTarget("CloudSkyRefTex", 1, 1, 64, aoDesc, PostFxResources.CloudSkyRefSurf);
                static const char* marchNames[2] = { "CloudMarchTex0", "CloudMarchTex1" };
                for (int i = 0; i < 2; ++i)
                    PostFxResources.CloudMarchTex[i] = rage::CreateEmptyRenderTarget(marchNames[i], width / 2, height / 2, 64, aoDesc, PostFxResources.CloudMarchSurf[i]);
                // Cleared: it is only ever blended into, and a NaN left in it would stay for good.
                if (PostFxResources.CloudSkyRefSurf && pDevice)
                    pDevice->ColorFill(PostFxResources.CloudSkyRefSurf, nullptr, D3DCOLOR_ARGB(0, 0, 0, 0));
                PostFxResources.nCloudSkyRefFrame = 0;
            }
            PostFxResources.SSRHalfDenoisedTex = rage::CreateEmptyRenderTarget("SSRHalfDenoisedTex", width / 2, height / 2, 64, aoDesc, PostFxResources.SSRHalfDenoisedSurf);
            {
                static const char* fallbackNames[2] = { "SSRFallbackTex", "SSRHalfFallbackTex" };
                static const char* traceNames[2] = { "SSRTraceTex", "SSRHalfTraceTex" };
                for (int half = 0; half < 2; ++half)
                {
                    const auto w = half ? width / 2 : width, hgt = half ? height / 2 : height;
                    PostFxResources.SSRFallbackTex[half] = rage::CreateEmptyRenderTarget(fallbackNames[half], w, hgt, 64, aoDesc,
                        PostFxResources.SSRFallbackSurf[half]);
                    PostFxResources.SSRSpreadTex[half] = rage::CreateEmptyRenderTarget(half ? "SSRHalfSpreadTex" : "SSRSpreadTex", w, hgt,
                        64, aoDesc, PostFxResources.SSRSpreadSurf[half]);
                    PostFxResources.SSRTraceTex[half] = rage::CreateEmptyRenderTarget(traceNames[half], w, hgt, 64, aoDesc,
                        PostFxResources.SSRTraceSurf[half]);
                    if (PostFxResources.fSSRTemporalBlend > 0.0f)
                        PostFxResources.SSRHitDistTex[half] = rage::CreateEmptyRenderTarget(half ? "SSRHalfHitDistTex" : "SSRHitDistTex",
                            w, hgt, 64, aoDesc, PostFxResources.SSRHitDistSurf[half]);
                }
                auto marchDesc = rage::OwnRenderTargetDesc(rage::GRCFMT_R32F);
                PostFxResources.SSRMarchDepthTex = rage::CreateEmptyRenderTarget("SSRMarchDepthTex", width / 2, height / 2, 32, marchDesc,
                    PostFxResources.SSRMarchDepthSurf);
            }

            PostFxResources.ContactRawTex = rage::CreateEmptyRenderTarget("ContactShadowRawTex", width, height, 64, aoDesc, PostFxResources.ContactRawSurf);
            if (PostFxResources.bContactShadowsHalfRes)
            {
                PostFxResources.ContactRawHalfTex = rage::CreateEmptyRenderTarget("ContactShadowRawHalfTex", width / 2, height / 2, 64, aoDesc, PostFxResources.ContactRawHalfSurf);
            }
            // Smoothed and accumulated at the size they are marched at, brought up to full size last
            const int contactWidth = PostFxResources.bContactShadowsHalfRes ? width / 2 : width;
            const int contactHeight = PostFxResources.bContactShadowsHalfRes ? height / 2 : height;
            PostFxResources.ContactTex = rage::CreateEmptyRenderTarget("ContactShadowTex", contactWidth, contactHeight, 64, aoDesc, PostFxResources.ContactSurf);
            if (PostFxResources.fContactTemporalBlend > 0.0f)
            {
                static const char* names[2] = { "ContactShadowAccumTex0", "ContactShadowAccumTex1" };
                for (int i = 0; i < 2; ++i)
                    PostFxResources.ContactAccumTex[i] = rage::CreateEmptyRenderTarget(names[i], contactWidth, contactHeight, 64, aoDesc, PostFxResources.ContactAccumSurf[i]);
            }

            if (PostFxResources.fSSRTemporalBlend > 0.0f)
            {
                static const char* names[2][2] = { { "SSRAccumTex0", "SSRAccumTex1" }, { "SSRHalfAccumTex0", "SSRHalfAccumTex1" } };
                for (int half = 0; half < 2; ++half)
                    for (int i = 0; i < 2; ++i)
                        PostFxResources.SSRAccumTex[half][i] = rage::CreateEmptyRenderTarget(names[half][i], half ? width / 2 : width,
                            half ? height / 2 : height, 64, aoDesc, PostFxResources.SSRAccumSurf[half][i]);
            }

            {
                auto create = [&](rage::grcRenderTargetPC*& rt, IDirect3DSurface9*& surf, const char* name)
                {
                    rt = rage::CreateEmptyRenderTarget(name, width / 2, height / 2, 64, aoDesc, surf);
                };
                create(PostFxResources.GIRawTex, PostFxResources.GIRawSurf, "GIRawTex");
                create(PostFxResources.GIDenoisedTex, PostFxResources.GIDenoisedSurf, "GIDenoisedTex");
                create(PostFxResources.GIAccumTex[0], PostFxResources.GIAccumSurf[0], "GIAccumTex0");
                create(PostFxResources.GIAccumTex[1], PostFxResources.GIAccumSurf[1], "GIAccumTex1");
                PostFxResources.GIFullTex = rage::CreateEmptyRenderTarget("GIFullTex", width, height, 64, aoDesc, PostFxResources.GIFullSurf);
            }

            for (int i = 0; i < 2; ++i)
            {
                PostFxResources.SkinLightTex[i] = rage::CreateEmptyRenderTarget(i ? "SkinLightTex1" : "SkinLightTex0", width, height, 64, aoDesc, PostFxResources.SkinLightSurf[i]);
            }

            PostFxResources.SSRDebugTex = rage::CreateEmptyRenderTarget("SSRDebugTex", width, height, 64, aoDesc, PostFxResources.SSRDebugSurf);

            IDirect3DSurface9* oldRT = nullptr;
            pDevice->GetRenderTarget(0, &oldRT);
            for (auto* surf : { PostFxResources.SSRSurf, PostFxResources.SSRHistorySurf })
            {
                if (!surf)
                    continue;
                pDevice->SetRenderTarget(0, surf);
                pDevice->Clear(0, nullptr, D3DCLEAR_TARGET, D3DCOLOR_ARGB(0, 0, 0, 0), 1.0f, 0);
            }
            if (oldRT)
            {
                pDevice->SetRenderTarget(0, oldRT);
                oldRT->Release();
            }
        }

        for (auto i = 0; i < PostFxResources.nAmbientOcclusionMaxMipLevel; ++i)
            PostFxResources.AOCamDepthTex->mD3DTexture->GetSurfaceLevel(i, &PostFxResources.AOCamDepthSurf[i]);

        PostFxResources.AOTex->mD3DTexture->GetSurfaceLevel(0, &PostFxResources.AOSurf);
        PostFxResources.AOBlurTex->mD3DTexture->GetSurfaceLevel(0, &PostFxResources.AOBlurSurf);
    }

    static void Init()
    {
        static bool initialized = false;
        if (initialized)
            return;

        // none, fxaa, smaa, blend, edge
        UsePostFxAA = FusionFixSettings.GetRef("PREF_ANTIALIASING");

        auto onLostCB = rage::grcDevice::Functor0(NULL, OnDeviceLost, NULL, 0);
        auto onResetCB = rage::grcDevice::Functor0(NULL, OnDeviceReset, NULL, 0);
        rage::grcDevice::RegisterDeviceCallbacks(onLostCB, onResetCB);

        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCWSTR)&Init, &hm);

        PostFxResources.loadShaders(rage::grcDevice::GetD3DDevice(), hm);

        OnDeviceReset();

        TemporalAA::ProfileMotion = [](IDirect3DDevice9* device, bool begin) { ProfilerMark(device, kProfMotion, begin); };
        HDROutput::ProfileOutput = [](IDirect3DDevice9* device, bool begin) { ProfilerMark(device, kProfHDROutput, begin); };
        Upscaler::Profile = [](IDirect3DDevice9* device, Upscaler::ProfilePart part, bool begin)
        {
            static constexpr int kSections[] =
            {
                kProfResolveReactive, kProfResolveUpscale, kProfFrameGeneration, kProfFrameGenerationCopies,
                kProfFrameGenerationGenerate, kProfFrameGenerationShow, kProfFrameGenerationPresent,
            };
            const auto i = static_cast<size_t>(part);
            if (i < std::size(kSections))
                ProfilerMark(device, kSections[i], begin);
        };
        TemporalAA::OnGBufferEnd = [](IDirect3DDevice9* device)
        {
            CopySceneDepth(device);
            RenderWetGround(device);
        };
        RenderScale::TraceDepth = [](const char* what, IDirect3DSurface9* depth, IDirect3DSurface9* target, DWORD flags)
        {
            if (SSRTrace::Active())
                SSRTrace::Line("render scale: %s %s, target %s%s", what, SSRTrace::SurfaceName(depth).c_str(), SSRTrace::SurfaceName(target).c_str(),
                               flags ? (std::string(", flags ") + std::to_string(flags)).c_str() : "");
        };

        initialized = true;
    }

    // Wet ground (WetGround.fx), right after CopySceneDepth at the end of the G-buffer pass: copies
    // _DEFERRED_GBUFFER_0_ to _2_ and draws them back wet, darker and glossier, with puddles and the
    // rain's rings, before any light reads them. Only while anything is wet; the wetness follows
    // CWeather::Rain (0.3 drizzle, 0.7 rain, 1.0 a storm) up over WetGroundWetting seconds and down
    // over WetGroundDrying. Leaves the device as it found it.
    static void RenderWetGround(IDirect3DDevice9* pDevice)
    {
        auto& R = PostFxResources;
        auto skip = [&](const char* why) { R.szWetGroundStatus = why; };

        // The wetness, in game time, so it stands still while the game is paused.
        const double seconds = CTimer::m_snTimeInMilliseconds ? *CTimer::m_snTimeInMilliseconds * 0.001 : 0.0;
        const float rain = CWeather::Rain ? std::clamp(*CWeather::Rain / 0.7f, 0.0f, 1.0f) : 0.0f;
        {
            const bool first = R.fWetnessTime < 0.0;
            const double dt = first ? 0.0 : std::clamp(seconds - R.fWetnessTime, 0.0, 1.0);
            R.fWetnessTime = seconds;
            if (first)
                R.fWetness = rain; // a game started or loaded in the rain starts wet
            else if (rain > R.fWetness)
                R.fWetness = R.fWetGroundWetting > 0.0f ? (std::min)(rain, R.fWetness + float(dt) / R.fWetGroundWetting) : rain;
            else
                R.fWetness = R.fWetGroundDrying > 0.0f ? (std::max)(rain, R.fWetness - float(dt) / R.fWetGroundDrying) : rain;
        }

        if (R.fWetGround <= 0.0f)
            return skip("off in the ini");
        if (!R.WetWeatherEnabled())
            return skip("off in the menu");
        if (!R.WetGroundEffect)
            return skip("no effect");
        if (R.fWetness <= 0.0f && R.nWetGroundDebug != 1)
            return skip("dry");
        if (R.bInteriorScene && R.nWetGroundDebug != 1)
            return skip("interior");
        if (!pDevice || !R.mDiffuseRT || !R.mNormalRT || !R.mSpecularRT || !R.mMaterialIdRT || !R.mDiffuseRT->mD3DTexture ||
            !R.mNormalRT->mD3DTexture || !R.mSpecularRT->mD3DTexture || !R.mMaterialIdRT->mD3DTexture)
            return skip("no G-buffer");
        auto noise = R.CloudNoiseTex();
        if (!noise)
            return skip("no noise texture");
        rage::grcViewport* vp = rage::GetCurrentViewport();
        if (!vp)
            return skip("no viewport");

        ProfilerScope timed(pDevice, kProfWetGround);
        // The G-buffer's surfaces, and copies of them at their size and format.
        ProfilerMark(pDevice, kProfWetGroundCopies, true);
        IDirect3DTexture9* gbuffer[3] = { R.mDiffuseRT->mD3DTexture, R.mNormalRT->mD3DTexture, R.mSpecularRT->mD3DTexture };
        IDirect3DSurface9* gbufferSurf[3] = {};
        D3DSURFACE_DESC desc[3] = {};
        bool ok = true;
        for (int i = 0; i < 3 && ok; ++i)
            ok = SUCCEEDED(gbuffer[i]->GetSurfaceLevel(0, &gbufferSurf[i])) && SUCCEEDED(gbufferSurf[i]->GetDesc(&desc[i]));
        for (int i = 0; i < 3 && ok; ++i)
        {
            D3DSURFACE_DESC copyDesc = {};
            if (R.WetCopySurf[i] && SUCCEEDED(R.WetCopySurf[i]->GetDesc(&copyDesc)) &&
                (copyDesc.Width != desc[i].Width || copyDesc.Height != desc[i].Height || copyDesc.Format != desc[i].Format))
            {
                SAFE_RELEASE(R.WetCopySurf[i]);
                SAFE_RELEASE(R.WetCopyTex[i]);
            }
            if (!R.WetCopyTex[i])
                ok = SUCCEEDED(pDevice->CreateTexture(desc[i].Width, desc[i].Height, 1, D3DUSAGE_RENDERTARGET, desc[i].Format,
                                                      D3DPOOL_DEFAULT, &R.WetCopyTex[i], nullptr)) &&
                     SUCCEEDED(R.WetCopyTex[i]->GetSurfaceLevel(0, &R.WetCopySurf[i]));
            if (ok)
                ok = SUCCEEDED(pDevice->StretchRect(gbufferSurf[i], nullptr, R.WetCopySurf[i], nullptr, D3DTEXF_NONE));
        }
        ProfilerMark(pDevice, kProfWetGroundCopies, false);
        if (!ok)
        {
            for (auto& surf : gbufferSurf)
                SAFE_RELEASE(surf);
            return skip("could not copy the G-buffer");
        }
        R.szWetGroundStatus = "drawn";

        ID3DXEffect* effect = R.WetGroundEffect;
        const float width = float(desc[0].Width), height = float(desc[0].Height);
        const D3DMATRIX& proj = *(const D3DMATRIX*)vp->mProjectionMatrix;
        const D3DXVECTOR4 projInfo = ProjInfo(proj, width, height);
        effect->SetVector("vec4ProjInfo", &projInfo);
        effect->SetFloat("fNearPlane", vp->mNearClip);
        effect->SetFloat("fFarDivNear", vp->mFarClip / vp->mNearClip);
        {
            const D3DXMATRIX& viewInv = *(const D3DXMATRIX*)vp->mViewInverseMatrix;
            D3DXVECTOR4 toView[3];
            WorldToViewRows(vp, toView);
            const D3DXVECTOR4 worldX(toView[0].x, toView[1].x, toView[2].x, viewInv.m[3][0]);
            const D3DXVECTOR4 worldY(toView[0].y, toView[1].y, toView[2].y, viewInv.m[3][1]);
            const D3DXVECTOR4 worldZ(toView[0].z, toView[1].z, toView[2].z, viewInv.m[3][2]);
            effect->SetVector("vec4WorldX", &worldX);
            effect->SetVector("vec4WorldY", &worldY);
            effect->SetVector("vec4WorldZ", &worldZ);
        }
        // The rings' clock wraps every 1000 s, where a frame's jump goes unseen among the rings.
        const D3DXVECTOR4 wet(R.fWetness * R.fWetGround, R.fWetGroundPuddles, rain, float(std::fmod(seconds, 1000.0)));
        effect->SetVector("vec4Wet", &wet);
        const D3DXVECTOR4 shape(1.0f / R.fWetGroundPuddleSize, R.fWetGroundRipples, 15.0f, R.fWetGroundDarkening);
        effect->SetVector("vec4Shape", &shape);
        const int mask = R.nWetGroundMaterials;
        const D3DXVECTOR4 allow0(float(mask & 1), float((mask >> 1) & 1), float((mask >> 2) & 1), float((mask >> 3) & 1));
        const D3DXVECTOR4 allow1(float((mask >> 4) & 1), float((mask >> 5) & 1), float((mask >> 6) & 1), float((mask >> 7) & 1));
        effect->SetVector("vec4Allow0", &allow0);
        effect->SetVector("vec4Allow1", &allow1);
        effect->SetFloat("fDebug", float(R.nWetGroundDebug));

        // Saved besides what StateBackup keeps around OnGBufferEnd: the textures, sampler states and
        // constants the pass sets.
        static constexpr DWORD kSlots = 6;
        static constexpr D3DSAMPLERSTATETYPE kSamplerStates[] = { D3DSAMP_ADDRESSU, D3DSAMP_ADDRESSV, D3DSAMP_MAGFILTER,
                                                                   D3DSAMP_MINFILTER, D3DSAMP_MIPFILTER, D3DSAMP_SRGBTEXTURE };
        IDirect3DBaseTexture9* oldTextures[kSlots] = {};
        DWORD savedSamplerStates[kSlots][std::size(kSamplerStates)] = {};
        for (DWORD slot = 0; slot < kSlots; ++slot)
        {
            pDevice->GetTexture(slot, &oldTextures[slot]);
            for (size_t i = 0; i < std::size(kSamplerStates); ++i)
                pDevice->GetSamplerState(slot, kSamplerStates[i], &savedSamplerStates[slot][i]);
        }
        pDevice->GetPixelShaderConstantF(0, savedPSConsts, kPSConstCount);
        DWORD colorWrite[3] = {};
        pDevice->GetRenderState(D3DRS_COLORWRITEENABLE1, &colorWrite[1]);
        pDevice->GetRenderState(D3DRS_COLORWRITEENABLE2, &colorWrite[2]);

        static constexpr struct { D3DRENDERSTATETYPE state; DWORD value; } kStates[] =
        {
            { D3DRS_ZENABLE, FALSE }, { D3DRS_ZWRITEENABLE, FALSE }, { D3DRS_ALPHABLENDENABLE, FALSE }, { D3DRS_ALPHATESTENABLE, FALSE },
            { D3DRS_STENCILENABLE, FALSE }, { D3DRS_CULLMODE, D3DCULL_NONE }, { D3DRS_COLORWRITEENABLE, 0x0F },
            { D3DRS_SCISSORTESTENABLE, FALSE }, { D3DRS_SRGBWRITEENABLE, FALSE }, { D3DRS_FILLMODE, D3DFILL_SOLID },
            { D3DRS_CLIPPLANEENABLE, 0 }, { D3DRS_FOGENABLE, FALSE },
        };
        for (auto [state, value] : kStates)
            pDevice->SetRenderState(state, value);
        pDevice->SetRenderState(D3DRS_COLORWRITEENABLE1, 0x0F);
        pDevice->SetRenderState(D3DRS_COLORWRITEENABLE2, 0x0F);
        for (DWORD i = 0; i < 3; ++i)
            pDevice->SetRenderTarget(i, gbufferSurf[i]);
        pDevice->SetRenderTarget(3, nullptr);
        pDevice->SetDepthStencilSurface(nullptr);
        D3DVIEWPORT9 viewport = { 0, 0, desc[0].Width, desc[0].Height, 0.0f, 1.0f };
        pDevice->SetViewport(&viewport);
        pDevice->SetVertexShader(nullptr);
        pDevice->SetFVF(D3DFVF_XYZRHW | D3DFVF_TEX1);

        ProfilerScope timedPass(pDevice, kProfWetGroundPass);
        UINT passes = 0;
        effect->SetTechnique("Wet");
        effect->Begin(&passes, D3DXFX_DONOTSAVESTATE);
        effect->BeginPass(0);
        effect->CommitChanges();
        IDirect3DBaseTexture9* textures[kSlots] = { LightingDepth(), R.WetCopyTex[0], R.WetCopyTex[1], R.WetCopyTex[2],
                                                    R.mMaterialIdRT->mD3DTexture, noise };
        for (DWORD slot = 0; slot < kSlots; ++slot)
        {
            const bool wrap = slot == 5;
            SetTextureBoth(pDevice, slot, textures[slot]);
            SetSamplerStateBoth(pDevice, slot, D3DSAMP_ADDRESSU, wrap ? D3DTADDRESS_WRAP : D3DTADDRESS_CLAMP);
            SetSamplerStateBoth(pDevice, slot, D3DSAMP_ADDRESSV, wrap ? D3DTADDRESS_WRAP : D3DTADDRESS_CLAMP);
            SetSamplerStateBoth(pDevice, slot, D3DSAMP_MAGFILTER, wrap ? D3DTEXF_LINEAR : D3DTEXF_POINT);
            SetSamplerStateBoth(pDevice, slot, D3DSAMP_MINFILTER, wrap ? D3DTEXF_LINEAR : D3DTEXF_POINT);
            SetSamplerStateBoth(pDevice, slot, D3DSAMP_MIPFILTER, wrap ? D3DTEXF_LINEAR : D3DTEXF_NONE);
            SetSamplerStateBoth(pDevice, slot, D3DSAMP_SRGBTEXTURE, FALSE);
        }
        BindEffectConstantsOnly(pDevice, effect);
        struct ScreenVertex { float x, y, z, rhw; float u, v; };
        const ScreenVertex quad[4] =
        {
            { -0.5f,         -0.5f,          0.0f, 1.0f, 0.0f, 0.0f },
            { -0.5f,         height - 0.5f,  0.0f, 1.0f, 0.0f, 1.0f },
            { width - 0.5f,  -0.5f,          0.0f, 1.0f, 1.0f, 0.0f },
            { width - 0.5f,  height - 0.5f,  0.0f, 1.0f, 1.0f, 1.0f },
        };
        pDevice->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, quad, sizeof(ScreenVertex));
        effect->EndPass();
        effect->End();

        for (DWORD slot = 0; slot < kSlots; ++slot)
        {
            SetTextureBoth(pDevice, slot, oldTextures[slot]);
            SAFE_RELEASE(oldTextures[slot]);
            for (size_t i = 0; i < std::size(kSamplerStates); ++i)
                SetSamplerStateBoth(pDevice, slot, kSamplerStates[i], savedSamplerStates[slot][i]);
        }
        pDevice->SetPixelShaderConstantF(0, savedPSConsts, kPSConstCount);
        pDevice->SetRenderState(D3DRS_COLORWRITEENABLE1, colorWrite[1]);
        pDevice->SetRenderState(D3DRS_COLORWRITEENABLE2, colorWrite[2]);
        for (auto& surf : gbufferSurf)
            SAFE_RELEASE(surf);
    }

    // Headlight glints (HeadlightGlints.fx): at the start of the game's light shaft loop, once every light
    // of the main view is drawn, added to the lit scene they were drawn into. Whatever the pass changes is
    // put back for the shafts.
    static void RenderHeadlightGlints(IDirect3DDevice9* pDevice)
    {
        auto& R = PostFxResources;
        R.nGlintsLastLights = 0;
        auto skip = [&](const char* why) { R.szGlintsStatus = why; };
        if (!R.GGXLightingEnabled() || R.fLightsGGX <= 0.0f)
            return skip("GGX Lighting off");
        if (R.fLightsGGXGlints <= 0.0f)
            return skip("off in the ini");
        if (!R.HeadlightGlintsEffect)
            return skip("no effect");
        if (!R.nGlintCandidates)
            return skip("no headlights");
        if (!pDevice || !R.mNormalRT || !R.mSpecularRT || !R.mNormalRT->mD3DTexture || !R.mSpecularRT->mD3DTexture)
            return skip("no G-buffer");
        IDirect3DTexture9* depth = LightingDepth();
        if (!depth)
            return skip("no depth");
        rage::grcViewport* vp = rage::GetCurrentViewport();
        if (!vp)
            return skip("no viewport");
        D3DSURFACE_DESC desc = {};
        if (FAILED(R.mNormalRT->mD3DTexture->GetLevelDesc(0, &desc)))
            return skip("no G-buffer");

        ProfilerScope timed(pDevice, kProfGlints);
        // The nearest headlights.
        const D3DXMATRIX& viewInv = *(const D3DXMATRIX*)vp->mViewInverseMatrix;
        auto* candidates = R.GlintCandidates;
        const uint32_t found = R.nGlintCandidates;
        for (uint32_t i = 0; i < found; ++i)
        {
            const float dx = candidates[i].position[0] - viewInv.m[3][0];
            const float dy = candidates[i].position[1] - viewInv.m[3][1];
            const float dz = candidates[i].position[2] - viewInv.m[3][2];
            candidates[i].distance = dx * dx + dy * dy + dz * dz;
        }
        const uint32_t count = (std::min)(found, PostFxResource::kGlintLights);
        std::partial_sort(candidates, candidates + count, candidates + found,
                          [](const auto& a, const auto& b) { return a.distance < b.distance; });

        constexpr uint32_t kLights = PostFxResource::kGlintLights;
        D3DXVECTOR4 position[kLights] = {}, direction[kLights] = {}, right[kLights] = {}, colour[kLights] = {};
        for (uint32_t i = 0; i < count; ++i)
        {
            const auto& g = candidates[i];
            float d[3] = { g.direction[0], g.direction[1], g.direction[2] };
            const float len = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
            if (len > 1e-3f)
                for (auto& c : d)
                    c /= len;
            // The car's level right, dir x up, as SetLightGGXShape takes it; a light aimed straight up
            // or down, or a lamp alone, keeps one lamp.
            const float kx = d[1], ky = -d[0];
            const float rlen = std::sqrt(kx * kx + ky * ky);
            const bool level = rlen > 1e-3f;
            // The game's own highlight ends at about two thirds of the light's radius.
            position[i] = D3DXVECTOR4(g.position[0], g.position[1], g.position[2], level && g.twin ? R.fLightsGGXHeadlights : 0.0f);
            direction[i] = D3DXVECTOR4(d[0], d[1], d[2], 1.0f / (0.66f * g.radius));
            right[i] = D3DXVECTOR4(level ? kx / rlen : 0.0f, level ? ky / rlen : 0.0f, 0.0f, 0.0f);
            colour[i] = D3DXVECTOR4(g.colour[0], g.colour[1], g.colour[2], 0.0f);
        }

        ID3DXEffect* effect = R.HeadlightGlintsEffect;
        const float width = float(desc.Width), height = float(desc.Height);
        const D3DMATRIX& proj = *(const D3DMATRIX*)vp->mProjectionMatrix;
        const D3DXVECTOR4 projInfo = ProjInfo(proj, width, height);
        effect->SetVector("vec4ProjInfo", &projInfo);
        effect->SetFloat("fNearPlane", vp->mNearClip);
        effect->SetFloat("fFarDivNear", vp->mFarClip / vp->mNearClip);
        {
            D3DXVECTOR4 toView[3];
            WorldToViewRows(vp, toView);
            const D3DXVECTOR4 worldX(toView[0].x, toView[1].x, toView[2].x, viewInv.m[3][0]);
            const D3DXVECTOR4 worldY(toView[0].y, toView[1].y, toView[2].y, viewInv.m[3][1]);
            const D3DXVECTOR4 worldZ(toView[0].z, toView[1].z, toView[2].z, viewInv.m[3][2]);
            effect->SetVector("vec4WorldX", &worldX);
            effect->SetVector("vec4WorldY", &worldY);
            effect->SetVector("vec4WorldZ", &worldZ);
        }
        const float invSize[2] = { 1.0f / width, 1.0f / height };
        effect->SetFloatArray("vec2InvSize", invSize, 2);
        const D3DXVECTOR4 glint(R.fLightsGGXGlints, R.fLightsGGXGlintsGloss, R.fLightsGGXGlintsSize, R.fLightsGGXGlintsMax);
        effect->SetVector("vec4Glint", &glint);
        effect->SetFloat("fLightCount", float(count));
        effect->SetVectorArray("vec4LightPos", position, kLights);
        effect->SetVectorArray("vec4LightDir", direction, kLights);
        effect->SetVectorArray("vec4LightRight", right, kLights);
        effect->SetVectorArray("vec4LightColour", colour, kLights);

        // Saved: the textures, sampler states, constants, shaders, render states and geometry the pass sets.
        static constexpr DWORD kSlots = 3;
        static constexpr D3DSAMPLERSTATETYPE kSamplerStates[] = { D3DSAMP_ADDRESSU, D3DSAMP_ADDRESSV, D3DSAMP_MAGFILTER,
                                                                   D3DSAMP_MINFILTER, D3DSAMP_MIPFILTER, D3DSAMP_SRGBTEXTURE };
        IDirect3DBaseTexture9* oldTextures[kSlots] = {};
        DWORD savedSamplerStates[kSlots][std::size(kSamplerStates)] = {};
        for (DWORD slot = 0; slot < kSlots; ++slot)
        {
            pDevice->GetTexture(slot, &oldTextures[slot]);
            for (size_t i = 0; i < std::size(kSamplerStates); ++i)
                pDevice->GetSamplerState(slot, kSamplerStates[i], &savedSamplerStates[slot][i]);
        }
        pDevice->GetPixelShaderConstantF(0, savedPSConsts, kPSConstCount);
        IDirect3DPixelShader9* oldPS = nullptr;
        IDirect3DVertexShader9* oldVS = nullptr;
        IDirect3DVertexDeclaration9* oldDecl = nullptr;
        IDirect3DVertexBuffer9* oldVB = nullptr;
        UINT oldOffset = 0, oldStride = 0;
        DWORD oldFVF = 0;
        D3DVIEWPORT9 oldViewport = {};
        pDevice->GetPixelShader(&oldPS);
        pDevice->GetVertexShader(&oldVS);
        pDevice->GetVertexDeclaration(&oldDecl);
        pDevice->GetStreamSource(0, &oldVB, &oldOffset, &oldStride);
        pDevice->GetFVF(&oldFVF);
        pDevice->GetViewport(&oldViewport);

        static constexpr struct { D3DRENDERSTATETYPE state; DWORD value; } kStates[] =
        {
            { D3DRS_ZENABLE, FALSE }, { D3DRS_ZWRITEENABLE, FALSE }, { D3DRS_ALPHATESTENABLE, FALSE }, { D3DRS_STENCILENABLE, FALSE },
            { D3DRS_CULLMODE, D3DCULL_NONE }, { D3DRS_COLORWRITEENABLE, 0x07 }, { D3DRS_SCISSORTESTENABLE, FALSE },
            { D3DRS_SRGBWRITEENABLE, FALSE }, { D3DRS_FILLMODE, D3DFILL_SOLID }, { D3DRS_CLIPPLANEENABLE, 0 }, { D3DRS_FOGENABLE, FALSE },
            { D3DRS_ALPHABLENDENABLE, TRUE }, { D3DRS_SEPARATEALPHABLENDENABLE, FALSE }, { D3DRS_BLENDOP, D3DBLENDOP_ADD },
            { D3DRS_SRCBLEND, D3DBLEND_ONE }, { D3DRS_DESTBLEND, D3DBLEND_ONE },
        };
        DWORD savedStates[std::size(kStates)] = {};
        for (size_t i = 0; i < std::size(kStates); ++i)
        {
            pDevice->GetRenderState(kStates[i].state, &savedStates[i]);
            pDevice->SetRenderState(kStates[i].state, kStates[i].value);
        }
        D3DVIEWPORT9 viewport = { 0, 0, desc.Width, desc.Height, 0.0f, 1.0f };
        pDevice->SetViewport(&viewport);
        pDevice->SetVertexShader(nullptr);
        pDevice->SetFVF(D3DFVF_XYZRHW | D3DFVF_TEX1);

        UINT passes = 0;
        effect->SetTechnique("Glints");
        effect->Begin(&passes, D3DXFX_DONOTSAVESTATE);
        effect->BeginPass(0);
        effect->CommitChanges();
        IDirect3DBaseTexture9* textures[kSlots] = { depth, R.mNormalRT->mD3DTexture, R.mSpecularRT->mD3DTexture };
        for (DWORD slot = 0; slot < kSlots; ++slot)
        {
            SetTextureBoth(pDevice, slot, textures[slot]);
            SetSamplerStateBoth(pDevice, slot, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
            SetSamplerStateBoth(pDevice, slot, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
            SetSamplerStateBoth(pDevice, slot, D3DSAMP_MAGFILTER, D3DTEXF_POINT);
            SetSamplerStateBoth(pDevice, slot, D3DSAMP_MINFILTER, D3DTEXF_POINT);
            SetSamplerStateBoth(pDevice, slot, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
            SetSamplerStateBoth(pDevice, slot, D3DSAMP_SRGBTEXTURE, FALSE);
        }
        BindEffectConstantsOnly(pDevice, effect);
        struct ScreenVertex { float x, y, z, rhw; float u, v; };
        const ScreenVertex quad[4] =
        {
            { -0.5f,         -0.5f,          0.0f, 1.0f, 0.0f, 0.0f },
            { -0.5f,         height - 0.5f,  0.0f, 1.0f, 0.0f, 1.0f },
            { width - 0.5f,  -0.5f,          0.0f, 1.0f, 1.0f, 0.0f },
            { width - 0.5f,  height - 0.5f,  0.0f, 1.0f, 1.0f, 1.0f },
        };
        pDevice->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, quad, sizeof(ScreenVertex));
        effect->EndPass();
        effect->End();
        R.szGlintsStatus = "drawn";
        R.nGlintsLastLights = count;

        for (DWORD slot = 0; slot < kSlots; ++slot)
        {
            SetTextureBoth(pDevice, slot, oldTextures[slot]);
            SAFE_RELEASE(oldTextures[slot]);
            for (size_t i = 0; i < std::size(kSamplerStates); ++i)
                SetSamplerStateBoth(pDevice, slot, kSamplerStates[i], savedSamplerStates[slot][i]);
        }
        pDevice->SetPixelShaderConstantF(0, savedPSConsts, kPSConstCount);
        for (size_t i = 0; i < std::size(kStates); ++i)
            pDevice->SetRenderState(kStates[i].state, savedStates[i]);
        pDevice->SetViewport(&oldViewport);
        pDevice->SetPixelShader(oldPS);
        pDevice->SetVertexShader(oldVS);
        pDevice->SetFVF(oldFVF);
        pDevice->SetVertexDeclaration(oldDecl);
        pDevice->SetStreamSource(0, oldVB, oldOffset, oldStride);
        SAFE_RELEASE(oldPS);
        SAFE_RELEASE(oldVS);
        SAFE_RELEASE(oldDecl);
        SAFE_RELEASE(oldVB);
    }

    // At the end of the G-buffer pass (TemporalAA::OnGBufferEnd, the device state saved around it): this frame's
    // depth into SceneDepthTex. With FSR's render scale, _DEFERRED_GBUFFER_3_ read in the lighting phase, where the
    // game keeps it bound as its depth buffer, came out in whole steps of 1/255 or as another depth altogether (15 m,
    // 983 m and 0.35 m in a room 3 to 6 m deep), while temporal AA's motion vectors, read from it here, and the fog
    // pass's copy, made after lighting, held the right depths. SSR, SSGI, contact shadows and SSAO marched and tested
    // their histories against that.
    static void CopySceneDepth(IDirect3DDevice9* pDevice)
    {
        auto& R = PostFxResources;
        RefreshGBufferTargets();
        if (!pDevice || !R.SceneDepthSurf || !R.Blit_PS || !R.mDepthRT || !R.mDepthRT->mD3DTexture)
            return;
        ProfilerScope timed(pDevice, kProfDepthCopy);
        pDevice->SetRenderTarget(0, R.SceneDepthSurf);
        for (DWORD i = 1; i < 4; ++i)
            pDevice->SetRenderTarget(i, nullptr);
        pDevice->SetDepthStencilSurface(nullptr);
        static constexpr struct { D3DRENDERSTATETYPE state; DWORD value; } kStates[] =
        {
            { D3DRS_ZENABLE, FALSE }, { D3DRS_ZWRITEENABLE, FALSE }, { D3DRS_ALPHABLENDENABLE, FALSE }, { D3DRS_ALPHATESTENABLE, FALSE },
            { D3DRS_STENCILENABLE, FALSE }, { D3DRS_CULLMODE, D3DCULL_NONE }, { D3DRS_COLORWRITEENABLE, 0x0F },
            { D3DRS_SCISSORTESTENABLE, FALSE }, { D3DRS_SRGBWRITEENABLE, FALSE }, { D3DRS_FILLMODE, D3DFILL_SOLID },
            { D3DRS_CLIPPLANEENABLE, 0 }, { D3DRS_FOGENABLE, FALSE },
        };
        for (auto [state, value] : kStates)
            pDevice->SetRenderState(state, value);
        SetTextureBoth(pDevice, 0, R.mDepthRT->mD3DTexture);
        pDevice->SetSamplerState(0, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
        pDevice->SetSamplerState(0, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
        pDevice->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_POINT);
        pDevice->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_POINT);
        pDevice->SetSamplerState(0, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
        pDevice->SetSamplerState(0, D3DSAMP_SRGBTEXTURE, FALSE);
        BlitToTarget(pDevice, R.SceneDepthSurf);
        R.nSceneDepthFrame = FrameHistory::Frame();
        if (SSRTrace::Active())
        {
            const auto& camera = FrameHistory::Current();
            SSRTrace::DepthProbe(pDevice, "G-buffer end depth copy", R.SceneDepthTex->mD3DTexture, camera.Near, camera.Far);
        }
    }

    // The depth the passes of the lighting phase read: the copy from the end of this frame's G-buffer pass, or
    // _DEFERRED_GBUFFER_3_ where there is none.
    static IDirect3DTexture9* LightingDepth()
    {
        auto& R = PostFxResources;
        if (R.SceneDepthTex && R.SceneDepthTex->mD3DTexture && R.nSceneDepthFrame && R.nSceneDepthFrame == FrameHistory::Frame())
            return R.SceneDepthTex->mD3DTexture;
        return R.mDepthRT ? R.mDepthRT->mD3DTexture : nullptr;
    }

    // For the SSR trace, at the start of the lighting phase, before any FusionFix pass: what the game has on each
    // sampler and as its depth buffer.
    static void TraceLightingInputs(IDirect3DDevice9* pDevice)
    {
        auto& R = PostFxResources;
        if (!SSRTrace::Active() || !pDevice)
            return;
        std::string bound;
        for (DWORD slot = 0; slot < 16; ++slot)
        {
            IDirect3DBaseTexture9* tex = nullptr;
            pDevice->GetTexture(slot, &tex);
            if (tex)
                bound += " s" + std::to_string(slot) + "=" + SSRTrace::TextureName(tex);
            SAFE_RELEASE(tex);
        }
        IDirect3DSurface9* ds = nullptr;
        IDirect3DSurface9* rt0 = nullptr;
        pDevice->GetDepthStencilSurface(&ds);
        pDevice->GetRenderTarget(0, &rt0);
        IDirect3DSurface9* depthSurface = nullptr;
        if (R.mDepthRT && R.mDepthRT->mD3DTexture)
            R.mDepthRT->mD3DTexture->GetSurfaceLevel(0, &depthSurface);
        D3DSURFACE_DESC dsDesc = {}, rtDesc = {};
        if (ds)
            ds->GetDesc(&dsDesc);
        if (rt0)
            rt0->GetDesc(&rtDesc);
        SSRTrace::Line("lighting inputs: depth buffer %p %ux%u format %u%s, target %ux%u format %u;%s", static_cast<void*>(ds),
            dsDesc.Width, dsDesc.Height, unsigned(dsDesc.Format), ds && ds == depthSurface ? " (is _DEFERRED_GBUFFER_3_)" : "",
            rtDesc.Width, rtDesc.Height, unsigned(rtDesc.Format), bound.c_str());

        SAFE_RELEASE(depthSurface);
        SAFE_RELEASE(ds);
        SAFE_RELEASE(rt0);
    }

    // Copies the texture on s0 over all of the bound target 0 through Blit_PS, with a quad of its own. The fog pass's
    // own quad was drawn by the game's vertex shader, which RenderScale feeds the render size (globalScreenSize): on a
    // target of the screen size it covered the copy otherwise than the texture, and with FSR's render scale last
    // frame's depth sat elsewhere than this frame's, so every accumulation dropped its history on every pixel.
    static void BlitToTarget(IDirect3DDevice9* pDevice, IDirect3DSurface9* target)
    {
        D3DSURFACE_DESC desc = {};
        if (!target || FAILED(target->GetDesc(&desc)))
            return;
        // DrawPrimitiveUP leaves stream 0 unbound, and the fog pass draws from it after the copies.
        IDirect3DVertexShader9* oldVS = nullptr;
        IDirect3DVertexDeclaration9* oldDecl = nullptr;
        IDirect3DVertexBuffer9* oldVB = nullptr;
        UINT oldOffset = 0, oldStride = 0;
        DWORD oldFVF = 0;
        pDevice->GetVertexShader(&oldVS);
        pDevice->GetVertexDeclaration(&oldDecl);
        pDevice->GetStreamSource(0, &oldVB, &oldOffset, &oldStride);
        pDevice->GetFVF(&oldFVF);

        D3DVIEWPORT9 vp = { 0, 0, desc.Width, desc.Height, 0.0f, 1.0f };
        pDevice->SetViewport(&vp);
        pDevice->SetVertexShader(nullptr);
        pDevice->SetFVF(D3DFVF_XYZRHW | D3DFVF_TEX1);
        const float w = float(desc.Width), h = float(desc.Height);
        struct Vertex { float x, y, z, rhw, u, v; };
        const Vertex quad[4] =
        {
            { -0.5f,     -0.5f,     0.0f, 1.0f, 0.0f, 0.0f },
            { -0.5f,     h - 0.5f,  0.0f, 1.0f, 0.0f, 1.0f },
            { w - 0.5f,  -0.5f,     0.0f, 1.0f, 1.0f, 0.0f },
            { w - 0.5f,  h - 0.5f,  0.0f, 1.0f, 1.0f, 1.0f },
        };
        pDevice->SetPixelShader(PostFxResources.Blit_PS);
        pDevice->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, quad, sizeof(Vertex));

        pDevice->SetVertexShader(oldVS);
        pDevice->SetFVF(oldFVF);
        pDevice->SetVertexDeclaration(oldDecl);
        pDevice->SetStreamSource(0, oldVB, oldOffset, oldStride);
        SAFE_RELEASE(oldVS);
        SAFE_RELEASE(oldDecl);
        SAFE_RELEASE(oldVB);
    }

    static void NewFog()
    {
        RefreshGBufferTargets();
        IDirect3DDevice9* pDevice = rage::grcDevice::GetD3DDevice();
        ProfilerScope timed(pDevice, kProfFogPass);

        IDirect3DSurface9* prevSurface = nullptr;
        IDirect3DSurface9* prevDepthStencilSurface = nullptr;
        IDirect3DPixelShader9* prevPS = nullptr;

        IDirect3DBaseTexture9* prevTex[2] = { nullptr };

        // Previous sampler/render states
        DWORD prevMinFilter[2] = { D3DTEXF_NONE };
        DWORD prevMagFilter[2] = { D3DTEXF_NONE };
        DWORD prevMipFilter[2] = { D3DTEXF_NONE };
        DWORD prevAddressU[2] = { D3DTADDRESS_WRAP };
        DWORD prevAddressV[2] = { D3DTADDRESS_WRAP };

        // Store previous sampler states, renderstates, surfaces, textures, shaders
        pDevice->GetSamplerState(0, D3DSAMP_MINFILTER, &prevMinFilter[0]);
        pDevice->GetSamplerState(0, D3DSAMP_MAGFILTER, &prevMagFilter[0]);
        pDevice->GetSamplerState(0, D3DSAMP_MIPFILTER, &prevMipFilter[0]);
        pDevice->GetSamplerState(0, D3DSAMP_ADDRESSU, &prevAddressU[0]);
        pDevice->GetSamplerState(0, D3DSAMP_ADDRESSV, &prevAddressV[0]);

        pDevice->GetSamplerState(1, D3DSAMP_MINFILTER, &prevMinFilter[1]);
        pDevice->GetSamplerState(1, D3DSAMP_MAGFILTER, &prevMagFilter[1]);
        pDevice->GetSamplerState(1, D3DSAMP_MIPFILTER, &prevMipFilter[1]);
        pDevice->GetSamplerState(1, D3DSAMP_ADDRESSU, &prevAddressU[1]);
        pDevice->GetSamplerState(1, D3DSAMP_ADDRESSV, &prevAddressV[1]);

        pDevice->GetRenderTarget(0, &prevSurface);
        pDevice->GetDepthStencilSurface(&prevDepthStencilSurface);

        pDevice->GetTexture(0, &prevTex[0]);
        pDevice->GetTexture(1, &prevTex[1]);

        pDevice->GetPixelShader(&prevPS);

        {
            if (PostFxResources.PreAlphaDepthCopyRT)
            {
                PostFxResources.PreAlphaDepthCopyRT->mD3DTexture->GetSurfaceLevel(0, &PostFxResources.PreAlphaDepthSurface);

                // Blit depth to texture 
                if (PostFxResources.Blit_PS)
                {
                    if (PostFxResources.PreAlphaDepthSurface)
                    {
                        pDevice->SetRenderTarget(0, PostFxResources.PreAlphaDepthSurface);
                        pDevice->SetDepthStencilSurface(nullptr);

                        // _DEFERRED_GBUFFER_3_, bound here and not taken from what the game left on s0: whatever that
                        // was, last frame's depth disagreed with this one's everywhere and every accumulation (SSR, SSGI,
                        // contact shadows, GTAO) dropped its history on every pixel.
                        if (SSRTrace::Active())
                        {
                            IDirect3DBaseTexture9* bound = nullptr;
                            pDevice->GetTexture(0, &bound);
                            SSRTrace::Line("fog pass: depth copy, s0 held %s", SSRTrace::TextureName(bound).c_str());
                            SAFE_RELEASE(bound);
                        }
                        if (PostFxResources.mDepthRT && PostFxResources.mDepthRT->mD3DTexture)
                        {
                            SetTextureBoth(pDevice, 0, PostFxResources.mDepthRT->mD3DTexture);
                            SetSamplerStateBoth(pDevice, 0, D3DSAMP_MINFILTER, D3DTEXF_POINT);
                            SetSamplerStateBoth(pDevice, 0, D3DSAMP_MAGFILTER, D3DTEXF_POINT);
                            SetSamplerStateBoth(pDevice, 0, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
                        }

                        BlitToTarget(pDevice, PostFxResources.PreAlphaDepthSurface);

                        if (SSRTrace::Active())
                        {
                            const auto& camera = FrameHistory::Current();
                            SSRTrace::DepthProbe(pDevice, "fog depth", PostFxResources.mDepthRT ? PostFxResources.mDepthRT->mD3DTexture : nullptr,
                                                 camera.Near, camera.Far);
                            SSRTrace::DepthProbe(pDevice, "fog depth copy", PostFxResources.PreAlphaDepthCopyRT->mD3DTexture, camera.Near, camera.Far);
                        }
                        SetTextureBoth(pDevice, 0, prevTex[0]);
                        SetSamplerStateBoth(pDevice, 0, D3DSAMP_MINFILTER, prevMinFilter[0]);
                        SetSamplerStateBoth(pDevice, 0, D3DSAMP_MAGFILTER, prevMagFilter[0]);
                        SetSamplerStateBoth(pDevice, 0, D3DSAMP_MIPFILTER, prevMipFilter[0]);
                    }
                }
            }

            // The lit scene, sampler 1 of the fog pass: the volumetric clouds drawn into the game's own scene first, then
            // the light scattered under the skin if that runs, from a copy that has the clouds; the fog, the copy below
            // and SSR's history all take it. The other way round the clouds drew into the skin passes' own target, and
            // without temporal anti-aliasing the whole picture shook.
            RenderVolumetricClouds(pDevice, prevTex[1]);
            IDirect3DBaseTexture9* scene = prevTex[1];
            ProfilerMark(pDevice, kProfSkin, true);
            if (auto skin = RenderSkinScattering(pDevice, prevTex[1]))
                scene = skin;
            ProfilerMark(pDevice, kProfSkin, false);

            if (PostFxResources.FullScreenTex_temp1)
            {
                // Get custom rendertarget D3D surfaces
                PostFxResources.FullScreenTex_temp1->mD3DTexture->GetSurfaceLevel(0, &PostFxResources.HDRFullScreenSurface);

                // Copy HDR fullscreen buffer at this stage to use it later in raindrop refraction shader
                if (PostFxResources.HDRFullScreenSurface && PostFxResources.Blit_PS)
                {
                    pDevice->SetSamplerState(0, D3DSAMP_MINFILTER, prevMinFilter[1]);
                    pDevice->SetSamplerState(0, D3DSAMP_MAGFILTER, prevMagFilter[1]);
                    pDevice->SetSamplerState(0, D3DSAMP_MIPFILTER, prevMipFilter[1]);
                    pDevice->SetSamplerState(0, D3DSAMP_ADDRESSU, prevAddressU[1]);
                    pDevice->SetSamplerState(0, D3DSAMP_ADDRESSV, prevAddressV[1]);

                    pDevice->SetRenderTarget(0, PostFxResources.HDRFullScreenSurface);
                    pDevice->SetDepthStencilSurface(nullptr);

                    SetTextureBoth(pDevice, 0, scene);

                    // Its own quad, as for the depth copy above: SSR's and SSGI's history is taken from this copy and
                    // read where last frame's depth copy is.
                    BlitToTarget(pDevice, PostFxResources.HDRFullScreenSurface);

                    pDevice->SetSamplerState(0, D3DSAMP_MINFILTER, prevMinFilter[0]);
                    pDevice->SetSamplerState(0, D3DSAMP_MAGFILTER, prevMagFilter[0]);
                    pDevice->SetSamplerState(0, D3DSAMP_MIPFILTER, prevMipFilter[0]);
                    pDevice->SetSamplerState(0, D3DSAMP_ADDRESSU, prevAddressU[0]);
                    pDevice->SetSamplerState(0, D3DSAMP_ADDRESSV, prevAddressV[0]);
                    SetTextureBoth(pDevice, 0, prevTex[0]);

                    if ((PostFxResources.SSREnabled() || PostFxResources.SSGIEnabled()) && PostFxResources.SSRHistorySurf && PostFxResources.SSRSurf)
                    {
                        D3DVIEWPORT9 vpBeforeCapture;
                        pDevice->GetViewport(&vpBeforeCapture);

                        pDevice->SetRenderTarget(0, PostFxResources.SSRSurf);
                        pDevice->StretchRect(PostFxResources.HDRFullScreenSurface, nullptr, PostFxResources.SSRHistorySurf, nullptr, D3DTEXF_NONE);
                        pDevice->SetRenderTarget(0, PostFxResources.HDRFullScreenSurface);
                        PostFxResources.bSSRHistoryThisFrame = true;
                        SSRTrace::Line("fog pass: history copied");
                        PostFxResources.nSSRHistoryFrame = FrameHistory::Frame();

                        pDevice->SetViewport(&vpBeforeCapture);
                    }
                }
            }

            // Restore fog pass
            if (prevPS)
            {
                if (prevSurface)
                {
                    pDevice->SetRenderTarget(0, prevSurface);
                    pDevice->SetDepthStencilSurface(prevDepthStencilSurface);

                    pDevice->SetPixelShader(prevPS);

                    SetTextureBoth(pDevice, 1, scene);
                    ProfilerMark(pDevice, kProfFog, true);
                    hbDrawPrimitivePostFX.fun();
                    ProfilerMark(pDevice, kProfFog, false);
                    SetTextureBoth(pDevice, 1, prevTex[1]);
                }
            }
        }

        SAFE_RELEASE(PostFxResources.PreAlphaDepthSurface);
        SAFE_RELEASE(PostFxResources.HDRFullScreenSurface);

        SAFE_RELEASE(prevSurface);
        SAFE_RELEASE(prevDepthStencilSurface);

        SAFE_RELEASE(prevTex[0]);
        SAFE_RELEASE(prevTex[1]);

        SAFE_RELEASE(prevPS);
    }

    static void NewPostFX()
    {
        ProfilerNextFrame(rage::grcDevice::GetD3DDevice());

        // The game's own post processing without the scene, e.g. while the render targets are recreated
        if (!PostFxResources.mFullScreenRT || !PostFxResources.mFullScreenRT->mD3DTexture)
        {
            hbDrawPrimitivePostFX.fun();
            return;
        }

        IDirect3DPixelShader9* oldps = 0;
        IDirect3DVertexShader9* oldvs = 0;
        IDirect3DDevice9* pDevice = rage::grcDevice::GetD3DDevice();

        // get textures
        // if(PostFxResources.mNormalRT->mD3DTexture == nullptr)
        //     return;
        // PostFxResources.NormalTex = PostFxResources.mNormalRT->mD3DTexture;
        PostFxResources.DiffuseTex = PostFxResources.mDiffuseRT->mD3DTexture;
        PostFxResources.SpecularTex = PostFxResources.mSpecularRT->mD3DTexture;
        // PostFxResources.StencilTex = PostFxResources.mStencilRT->mD3DTexture;
        // PostFxResources.BloomTex = PostFxResources.mBloomRT->mD3DTexture;
        // PostFxResources.CurrentLumTex = PostFxResources.mCurrentLum->mD3DTexture;
        PostFxResources.DepthTex = PostFxResources.mDepthRT->mD3DTexture;
        // if (PostFxResources.mCascadeAtlasRT)
        //     PostFxResources.CascadeAtlasTex = PostFxResources.mCascadeAtlasRT->mD3DTexture;

        PostFxResources.FullScreenTex = PostFxResources.mFullScreenRT->mD3DTexture;
        PostFxResources.HalfScreenTex = PostFxResources.mHalfScreenRT->mD3DTexture;
        // PostFxResources.pQuarterHDRTex = PostFxResources.mQuarterScreenRT->mD3DTexture;

        // get surfaces
        PostFxResources.FullScreenTex->GetSurfaceLevel(0, &PostFxResources.FullScreenSurface);

        PostFxResources.FullScreenTex_temp1->mD3DTexture->GetSurfaceLevel(0, &PostFxResources.FullScreenSurface_temp1);
        PostFxResources.FullScreenTex_temp2->mD3DTexture->GetSurfaceLevel(0, &PostFxResources.FullScreenSurface_temp2);
        PostFxResources.edgesTex->mD3DTexture->GetSurfaceLevel(0, &PostFxResources.edgesSurf);
        PostFxResources.blendTex->mD3DTexture->GetSurfaceLevel(0, &PostFxResources.blendSurf);

        PostFxResources.FullScreenDownsampleTex->mD3DTexture->GetSurfaceLevel(0, &PostFxResources.FullScreenDownsampleSurf);
        PostFxResources.FullScreenDownsampleTex2->mD3DTexture->GetSurfaceLevel(0, &PostFxResources.FullScreenDownsampleSurf2);

        // if (PostFxResources.CascadeAtlasTex)
        //     PostFxResources.CascadeAtlasTex->GetSurfaceLevel(0, &PostFxResources.CascadeAtlasSurf);
        //PostFxResources.pShadowBlurTex1->mD3DTexture->GetSurfaceLevel(0, &PostFxResources.pShadowBlurSurf1);
        //PostFxResources.pShadowBlurTex2->mD3DTexture->GetSurfaceLevel(0, &PostFxResources.pShadowBlurSurf2);


        pDevice->GetRenderTarget(0, &PostFxResources.backBuffer);
        FrameGeneration::OnPostProcessing();
        pDevice->GetPixelShader(&oldps);
        pDevice->GetVertexShader(&oldvs);

        saveRenderState();

        pDevice->SetSamplerState(2, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);

        // new 
        {
            ProfilerScope timed(pDevice, kProfPost);
            PostFx3(pDevice, oldps, oldvs);
        }

        restoreRenderState();

        // restore
        pDevice->SetPixelShader(oldps);
        pDevice->SetVertexShader(oldvs);
        pDevice->SetRenderTarget(0, PostFxResources.backBuffer);

        // release
        SAFE_RELEASE(oldps);
        SAFE_RELEASE(oldvs);

        SAFE_RELEASE(last_VertexDecl);
        SAFE_RELEASE(last_VertexBuffer);

        SAFE_RELEASE(PostFxResources.backBuffer);
        SAFE_RELEASE(PostFxResources.FullScreenSurface_temp1);
        SAFE_RELEASE(PostFxResources.FullScreenSurface_temp2);
        SAFE_RELEASE(PostFxResources.FullScreenDownsampleSurf);
        SAFE_RELEASE(PostFxResources.FullScreenDownsampleSurf2);
        SAFE_RELEASE(PostFxResources.edgesSurf);
        SAFE_RELEASE(PostFxResources.blendSurf);
        // SAFE_RELEASE(PostFxResources.CascadeAtlasSurf);

        SAFE_RELEASE(PostFxResources.FullScreenSurface);

        //SAFE_RELEASE(PostFxResources.pShadowBlurSurf1);
        //SAFE_RELEASE(PostFxResources.pShadowBlurSurf2);

        PostFxResources.renderTargetTex = nullptr;
        PostFxResources.textureRead = nullptr;
        PostFxResources.renderTargetSurf = nullptr;
        PostFxResources.surfaceRead = nullptr;
    }

    static bool IsSharpeningActive()
    {
        static auto sharpening = FusionFixSettings.GetRef("PREF_SHARPENING");
        auto& R = PostFxResources;
        return sharpening && sharpening->get() > 0 && R.CAS_PS && R.backBuffer && R.FullScreenTex_temp2 && R.FullScreenSurface_temp2;
    }

    // Low, medium and high; the peak CAS weighs the neighbours with is -1 / lerp(8, 5, sharpness).
    static float SharpeningPeak()
    {
        static auto sharpening = FusionFixSettings.GetRef("PREF_SHARPENING");
        static constexpr float kSharpness[] = { 0.3f, 0.6f, 1.0f };
        const float sharpness = kSharpness[std::clamp(sharpening->get(), 1, 3) - 1];
        return -1.0f / (8.0f - 3.0f * sharpness);
    }

    // With frame generation: sharpening of a frame as it is shown (ApplyCASMasked in CAS.hlsl), from frame into target.
    // Sharpened before it, the frame generation took the sharpened shadow under a moving car along with the ground and
    // showed it twice. Present and hudLess tell the HUD, which is left as it is. On the D3D9 runtime's own device, at
    // the end of the frame or, deferred, inside the next one; what it changes is put back. Not with a state block of all
    // the state: made and applied twice a frame, that cost DXVK the capture and the binding of everything again.
    static bool SharpenShownFrame(IDirect3DDevice9* device, IDirect3DTexture9* frame, IDirect3DTexture9* present, IDirect3DTexture9* hudLess,
        IDirect3DSurface9* target)
    {
        static auto sharpening = FusionFixSettings.GetRef("PREF_SHARPENING");
        auto& R = PostFxResources;
        D3DSURFACE_DESC desc{};
        if (!R.CASMasked_PS || !sharpening || sharpening->get() <= 0 || !device || !frame || !present || !hudLess || !target ||
            FAILED(target->GetDesc(&desc)))
            return false;

        static constexpr D3DRENDERSTATETYPE kStates[] =
        {
            D3DRS_ZENABLE, D3DRS_ZWRITEENABLE, D3DRS_STENCILENABLE, D3DRS_ALPHABLENDENABLE, D3DRS_ALPHATESTENABLE,
            D3DRS_SCISSORTESTENABLE, D3DRS_CULLMODE, D3DRS_SRGBWRITEENABLE, D3DRS_COLORWRITEENABLE,
        };
        static constexpr D3DSAMPLERSTATETYPE kSamplerStates[] =
        {
            D3DSAMP_MINFILTER, D3DSAMP_MAGFILTER, D3DSAMP_MIPFILTER, D3DSAMP_ADDRESSU, D3DSAMP_ADDRESSV, D3DSAMP_SRGBTEXTURE,
        };
        IDirect3DSurface9* oldTargets[4]{};
        D3DVIEWPORT9 oldViewport{};
        DWORD oldStates[std::size(kStates)]{};
        IDirect3DBaseTexture9* oldTextures[3]{};
        DWORD oldSamplerStates[3][std::size(kSamplerStates)]{};
        float oldConstants[2 * 4]{};
        IDirect3DPixelShader9* oldPixelShader = nullptr;
        IDirect3DVertexShader9* oldVertexShader = nullptr;
        IDirect3DVertexDeclaration9* oldDeclaration = nullptr;
        DWORD oldFvf = 0;
        IDirect3DVertexBuffer9* oldStream = nullptr;   // DrawPrimitiveUP unbinds stream 0
        UINT oldOffset = 0, oldStride = 0;
        for (DWORD i = 0; i < 4; ++i)
            device->GetRenderTarget(i, &oldTargets[i]);
        device->GetViewport(&oldViewport);
        for (size_t i = 0; i < std::size(kStates); ++i)
            device->GetRenderState(kStates[i], &oldStates[i]);
        for (DWORD i = 0; i < 3; ++i)
        {
            device->GetTexture(2 + i, &oldTextures[i]);
            for (size_t j = 0; j < std::size(kSamplerStates); ++j)
                device->GetSamplerState(2 + i, kSamplerStates[j], &oldSamplerStates[i][j]);
        }
        device->GetPixelShaderConstantF(200, oldConstants, 2);
        device->GetPixelShader(&oldPixelShader);
        device->GetVertexShader(&oldVertexShader);
        device->GetVertexDeclaration(&oldDeclaration);
        device->GetFVF(&oldFvf);
        device->GetStreamSource(0, &oldStream, &oldOffset, &oldStride);

        const float w = float(desc.Width), h = float(desc.Height);
        const float peak[4] = { SharpeningPeak(), 0.0f, 0.0f, 0.0f };
        // scRGB: 1 is 80 nits
        const float paperWhite = HDROutput::IsActive() ? HDROutput::GetPaperWhiteNits() : 0.0f;
        const float masked[4] = { paperWhite > 0.0f ? 80.0f / paperWhite : 0.0f, 0.0f, 1.0f / w, 1.0f / h };

        device->SetRenderTarget(0, target);
        for (DWORD i = 1; i < 4; ++i)
            device->SetRenderTarget(i, nullptr);
        D3DVIEWPORT9 vp = { 0, 0, desc.Width, desc.Height, 0.0f, 1.0f };
        device->SetViewport(&vp);
        device->SetRenderState(D3DRS_ZENABLE, FALSE);
        device->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
        device->SetRenderState(D3DRS_STENCILENABLE, FALSE);
        device->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
        device->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
        device->SetRenderState(D3DRS_SCISSORTESTENABLE, FALSE);
        device->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
        device->SetRenderState(D3DRS_SRGBWRITEENABLE, FALSE);
        device->SetRenderState(D3DRS_COLORWRITEENABLE, 0xF);
        IDirect3DTexture9* textures[] = { frame, present, hudLess };
        for (DWORD i = 0; i < 3; ++i)
        {
            device->SetTexture(2 + i, textures[i]);
            device->SetSamplerState(2 + i, D3DSAMP_MINFILTER, D3DTEXF_POINT);
            device->SetSamplerState(2 + i, D3DSAMP_MAGFILTER, D3DTEXF_POINT);
            device->SetSamplerState(2 + i, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
            device->SetSamplerState(2 + i, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
            device->SetSamplerState(2 + i, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
            device->SetSamplerState(2 + i, D3DSAMP_SRGBTEXTURE, FALSE);
        }
        device->SetPixelShaderConstantF(200, peak, 1);
        device->SetPixelShaderConstantF(201, masked, 1);
        device->SetPixelShader(R.CASMasked_PS);
        device->SetVertexShader(nullptr);
        device->SetFVF(D3DFVF_XYZRHW | D3DFVF_TEX1);
        struct Vertex { float x, y, z, rhw, u, v; };
        const Vertex quad[4] =
        {
            { -0.5f,     -0.5f,     0.0f, 1.0f, 0.0f, 0.0f },
            { -0.5f,     h - 0.5f,  0.0f, 1.0f, 0.0f, 1.0f },
            { w - 0.5f,  -0.5f,     0.0f, 1.0f, 1.0f, 0.0f },
            { w - 0.5f,  h - 0.5f,  0.0f, 1.0f, 1.0f, 1.0f },
        };
        bool drawn = SUCCEEDED(device->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, quad, sizeof(Vertex)));

        for (DWORD i = 0; i < 4; ++i)
            if (oldTargets[i] || i > 0)
                device->SetRenderTarget(i, oldTargets[i]);
        device->SetViewport(&oldViewport);
        for (size_t i = 0; i < std::size(kStates); ++i)
            device->SetRenderState(kStates[i], oldStates[i]);
        for (DWORD i = 0; i < 3; ++i)
        {
            device->SetTexture(2 + i, oldTextures[i]);
            for (size_t j = 0; j < std::size(kSamplerStates); ++j)
                device->SetSamplerState(2 + i, kSamplerStates[j], oldSamplerStates[i][j]);
        }
        device->SetPixelShaderConstantF(200, oldConstants, 2);
        device->SetPixelShader(oldPixelShader);
        device->SetVertexShader(oldVertexShader);
        if (oldDeclaration)
            device->SetVertexDeclaration(oldDeclaration);
        else
            device->SetFVF(oldFvf);
        device->SetStreamSource(0, oldStream, oldOffset, oldStride);
        for (auto& t : oldTargets)
            SAFE_RELEASE(t);
        for (auto& t : oldTextures)
            SAFE_RELEASE(t);
        SAFE_RELEASE(oldPixelShader);
        SAFE_RELEASE(oldVertexShader);
        SAFE_RELEASE(oldDeclaration);
        SAFE_RELEASE(oldStream);
        return drawn;
    }

    // Sharpening (CAS.hlsl) of the finished frame, after anti-aliasing and before the HUD, from
    // FullScreenTex_temp2 into the back buffer. Without anti-aliasing of its own the game's post
    // processing drew the frame there already (inTemp2); after FXAA or SMAA, which read it from
    // there, the frame is copied into it.
    static void ApplySharpening(IDirect3DDevice9* pDevice, IDirect3DPixelShader9* pShader, IDirect3DVertexShader9* vShader, bool inTemp2)
    {
        auto& R = PostFxResources;
        ProfilerScope timed(pDevice, kProfPostSharpen);
        if (!inTemp2 && FAILED(pDevice->StretchRect(R.backBuffer, nullptr, R.FullScreenSurface_temp2, nullptr, D3DTEXF_NONE)))
            return;

        const float params[4] = { SharpeningPeak(), 0.0f, 0.0f, 0.0f };

        static constexpr D3DSAMPLERSTATETYPE kStates[] = { D3DSAMP_MINFILTER, D3DSAMP_MAGFILTER, D3DSAMP_MIPFILTER, D3DSAMP_ADDRESSU, D3DSAMP_ADDRESSV, D3DSAMP_SRGBTEXTURE };
        static constexpr DWORD kValues[] = { D3DTEXF_POINT, D3DTEXF_POINT, D3DTEXF_NONE, D3DTADDRESS_CLAMP, D3DTADDRESS_CLAMP, FALSE };
        DWORD saved[std::size(kStates)] = {};
        for (size_t i = 0; i < std::size(kStates); ++i)
        {
            pDevice->GetSamplerState(2, kStates[i], &saved[i]);
            pDevice->SetSamplerState(2, kStates[i], kValues[i]);
        }
        DWORD srgbWrite = FALSE;
        pDevice->GetRenderState(D3DRS_SRGBWRITEENABLE, &srgbWrite);
        pDevice->SetRenderState(D3DRS_SRGBWRITEENABLE, FALSE);

        // c200 is also the GGX lights' (BindLightingInputs): lights drawn before it is set again would read the peak
        float savedParams[4] = {};
        pDevice->GetPixelShaderConstantF(200, savedParams, 1);

        pDevice->SetRenderTarget(0, R.backBuffer);
        SetTextureBoth(pDevice, 2, R.FullScreenTex_temp2->mD3DTexture);
        pDevice->SetPixelShaderConstantF(200, params, 1);
        pDevice->SetPixelShader(R.CAS_PS);
        pDevice->SetVertexShader(vShader);
        pDevice->DrawPrimitive(D3DPT_TRIANGLELIST, 0, 2);

        pDevice->SetPixelShaderConstantF(200, savedParams, 1);
        pDevice->SetRenderState(D3DRS_SRGBWRITEENABLE, srgbWrite);
        for (size_t i = 0; i < std::size(kStates); ++i)
            pDevice->SetSamplerState(2, kStates[i], saved[i]);
        pDevice->SetPixelShader(pShader);
    }

    static HRESULT PostFx3(LPDIRECT3DDEVICE9 pDevice, IDirect3DPixelShader9* pShader, IDirect3DVertexShader9* vShader)
    {
        auto currGrcViewport = rage::GetCurrentViewport();

        HRESULT hr = S_FALSE;

        DWORD OldSRGB = 0;
        DWORD OldSampler = 0;

        // save render state between post processing steps, 
        // in general each step expects the environment as the game 
        // leaves it at the time post processing is used, 
        // each step works individually from each other
        for (int i = 0; i < PostfxTextureCount; i++)
        {
            pDevice->GetTexture(i, &PostFxResources.prePostFx[i]);
            pDevice->GetSamplerState(i, D3DSAMP_MAGFILTER, &PostFxResources.Samplers[i]);
        }

        // main postfx passes
        {
            if (PostFxResources.FullScreenTex_temp1 && PostFxResources.FullScreenTex /*&& PostFxResources.aoTex*/)
            {
                PostFxResources.renderTargetTex = PostFxResources.FullScreenTex_temp1->mD3DTexture;

                PostFxResources.textureRead = PostFxResources.FullScreenTex;
                PostFxResources.surfaceRead = PostFxResources.FullScreenSurface;

                PostFxResources.renderTargetTex = PostFxResources.FullScreenTex_temp1->mD3DTexture;
                PostFxResources.renderTargetSurf = PostFxResources.FullScreenSurface_temp1;

                // ready for new post processing?
                if (PostFxResources.backBuffer && PostFxResources.renderTargetSurf && PostFxResources.surfaceRead)
                {
                    //if(PostFxResources.UseSSAO && PostFxResources.SSAO_gen_ps && PostFxResources.SSAO_blend_ps && PostFxResources.pShadowBlurSurf1 && PostFxResources.pShadowBlurSurf2) {
                    //
                    //    pDevice->SetPixelShader(PostFxResources.SSAO_gen_ps);
                    //    vec4[1] = PostFxResources.AoDistance;
                    //
                    //    //SetTextureBoth(pDevice, 2, 0);
                    //    pDevice->SetRenderTarget(0, PostFxResources.pShadowBlurSurf1);
                    //    //SetTextureBoth(pDevice, 2, PostFxResources.textureRead);
                    //    pDevice->DrawPrimitive(D3DPT_TRIANGLELIST, 0, 2);
                    //    SetTextureBoth(pDevice, 3, 0);
                    //
                    //    pDevice->SetPixelShader(PostFxResources.DeferredShadowBlurCircle_ps);
                    //    pDevice->SetRenderTarget(0, PostFxResources.pShadowBlurSurf2);
                    //    SetTextureBoth(pDevice, 11, PostFxResources.pShadowBlurTex1->mD3DTexture);
                    //    pDevice->DrawPrimitive(D3DPT_TRIANGLELIST, 0, 2);
                    //
                    //    SetTextureBoth(pDevice, 11, 0);
                    //
                    //    pDevice->SetPixelShader(PostFxResources.SSAO_blend_ps);
                    //    pDevice->SetRenderTarget(0, PostFxResources.renderTargetSurf);
                    //    SetTextureBoth(pDevice, 2, PostFxResources.textureRead);
                    //    SetTextureBoth(pDevice, 3, PostFxResources.pShadowBlurTex2->mD3DTexture);
                    //    pDevice->DrawPrimitive(D3DPT_TRIANGLELIST, 0, 2);
                    //    PostFxResources.swapbuffers();
                    //    SetTextureBoth(pDevice, 2, PostFxResources.textureRead);
                    //    SetTextureBoth(pDevice, 3, PostFxResources.prePostFx[3]);
                    //    pDevice->SetPixelShader(pShader);
                    //}

                    // Temporal anti-aliasing resolves the HDR scene before everything else. Normally ResolveScene did
                    // it before the game computed bloom and exposure.
                    if (TemporalAA::GetMode() != TemporalAA::Mode::Off && !TemporalAA::IsSceneResolved() && !RenderScale::IsActive())
                    {
                        ProfilerScope timed(pDevice, kProfPostTAA);
                        auto source = FilterStippleBeforeResolve(PostFxResources.textureRead);
                        if (TemporalAA::Resolve(pDevice, source, PostFxResources.renderTargetTex, PostFxResources.renderTargetSurf))
                        {
                            PostFxResources.swapbuffers();
                            pDevice->SetPixelShader(pShader);
                        }
                        else if (source != PostFxResources.textureRead)
                            TemporalAA::KeepStipple(PostFxResources.textureRead);
                    }

                    if (PostFxResources.useStippleFilter && PostFxResources.stipple_filter_ps && !TemporalAA::IsStippleFiltered())
                    {
                        ProfilerScope timed(pDevice, kProfPostStipple);
                        pDevice->SetPixelShader(PostFxResources.stipple_filter_ps);
                        pDevice->SetRenderTarget(0, PostFxResources.renderTargetSurf);
                        SetTextureBoth(pDevice, 2, PostFxResources.textureRead);
                        pDevice->DrawPrimitive(D3DPT_TRIANGLELIST, 0, 2);
                        PostFxResources.swapbuffers();
                        pDevice->SetPixelShader(pShader);
                    }

                    static auto dof = FusionFixSettings.GetRef("PREF_TCYC_DOF");
                    if (dof->get() > FusionFixSettings.DofText.eCutscenesOnly || (dof->get() == FusionFixSettings.DofText.eCutscenesOnly && CCutsceneManager::IsRunning()) || shouldModifyMapMenuBackground())
                    {
                        if (PostFxResources.dof_blur_ps && PostFxResources.depth_of_field_tent_ps && PostFxResources.dof_coc_ps)
                        {
                            if (PostFxResources.FullScreenDownsampleSurf && PostFxResources.FullScreenDownsampleSurf2)
                            {
                                ProfilerScope timed(pDevice, kProfPostDOF);
                                pDevice->SetSamplerState(8, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
                                pDevice->SetSamplerState(8, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
                                pDevice->SetSamplerState(8, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
                                pDevice->SetSamplerState(2, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);

                                pDevice->SetPixelShader(PostFxResources.dof_blur_ps);
                                pDevice->SetRenderTarget(0, PostFxResources.FullScreenDownsampleSurf);
                                SetTextureBoth(pDevice, 8, PostFxResources.HalfScreenTex);
                                pDevice->DrawPrimitive(D3DPT_TRIANGLELIST, 0, 2);

                                pDevice->SetPixelShader(PostFxResources.depth_of_field_tent_ps);
                                pDevice->SetRenderTarget(0, PostFxResources.FullScreenDownsampleSurf2);
                                SetTextureBoth(pDevice, 8, PostFxResources.FullScreenDownsampleTex->mD3DTexture);
                                pDevice->DrawPrimitive(D3DPT_TRIANGLELIST, 0, 2);

                                pDevice->SetPixelShader(PostFxResources.dof_coc_ps);
                                pDevice->SetRenderTarget(0, PostFxResources.renderTargetSurf);
                                if (PostFxResources.bEnablePreAlphaDepth)
                                    SetTextureBoth(pDevice, 1, PostDepth());
                                SetTextureBoth(pDevice, 2, PostFxResources.textureRead);
                                SetTextureBoth(pDevice, 8, PostFxResources.FullScreenDownsampleTex2->mD3DTexture);
                                pDevice->DrawPrimitive(D3DPT_TRIANGLELIST, 0, 2);
                                PostFxResources.swapbuffers();

                                pDevice->SetPixelShader(pShader);
                            }
                        }
                    }

                    static auto refSunShafts = FusionFixSettings.GetRef("PREF_SUNSHAFTS");
                    if (refSunShafts->get())
                    {
                        // 2 passes at half res
                        if (PostFxResources.SSPrepass_PS && PostFxResources.SSDraw_PS)
                        {
                            if (PostFxResources.FullScreenDownsampleSurf && PostFxResources.FullScreenDownsampleSurf2)
                            {
                                ProfilerScope timed(pDevice, kProfPostSunShafts);
                                pDevice->SetSamplerState(2, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
                                pDevice->SetSamplerState(8, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
                                pDevice->SetSamplerState(8, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
                                pDevice->SetSamplerState(8, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
                                pDevice->SetSamplerState(11, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
                                pDevice->SetSamplerState(11, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
                                pDevice->SetSamplerState(11, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
                                pDevice->SetSamplerState(13, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
                                pDevice->SetSamplerState(13, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
                                pDevice->SetSamplerState(13, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);

                                // crop around sun position
                                pDevice->SetPixelShader(PostFxResources.SSPrepass_PS);
                                pDevice->SetRenderTarget(0, PostFxResources.FullScreenDownsampleSurf);
                                if (PostFxResources.bEnablePreAlphaDepth)
                                    SetTextureBoth(pDevice, 1, PostDepth());
                                SetTextureBoth(pDevice, 2, PostFxResources.textureRead);
                                SetTextureBoth(pDevice, 13, PostFxResources.DiffuseTex);
                                pDevice->DrawPrimitive(D3DPT_TRIANGLELIST, 0, 2);

                                // sample sunshafts from a cropped texture
                                pDevice->SetPixelShader(PostFxResources.SSDraw_PS);
                                pDevice->SetRenderTarget(0, PostFxResources.FullScreenDownsampleSurf2);
                                SetTextureBoth(pDevice, 11, PostFxResources.FullScreenDownsampleTex->mD3DTexture);
                                pDevice->DrawPrimitive(D3DPT_TRIANGLELIST, 0, 2);

                                // second sunshafts pass
                                pDevice->SetPixelShader(PostFxResources.SSDraw_PS);
                                pDevice->SetRenderTarget(0, PostFxResources.FullScreenDownsampleSurf);
                                SetTextureBoth(pDevice, 11, PostFxResources.FullScreenDownsampleTex2->mD3DTexture);
                                pDevice->DrawPrimitive(D3DPT_TRIANGLELIST, 0, 2);

                                // add sunshafts to screen
                                pDevice->SetPixelShader(PostFxResources.SSAdd_PS);
                                pDevice->SetRenderTarget(0, PostFxResources.renderTargetSurf);
                                SetTextureBoth(pDevice, 2, PostFxResources.textureRead);
                                SetTextureBoth(pDevice, 11, PostFxResources.FullScreenDownsampleTex->mD3DTexture);

                                pDevice->DrawPrimitive(D3DPT_TRIANGLELIST, 0, 2);
                                PostFxResources.swapbuffers();

                                pDevice->SetPixelShader(pShader);
                            }
                        }
                    }

                    // The sharpening reads the frame from FullScreenTex_temp2: without anti-aliasing of
                    // its own the game's post processing draws it there
                    // With frame generation the frames are sharpened as they are shown instead (SharpenShownFrame)
                    const bool sharpenNow = IsSharpeningActive() && !(PostFxResources.CASMasked_PS && FrameGeneration::DefersSharpening());
                    const bool sharpenFromTemp2 = !IsPostFxAA() && sharpenNow;

                    // game postfx
                    {
                        ProfilerScope timed(pDevice, kProfPostGame);
                        for (int i = 0; i < 4; i++)
                        {
                            SetTextureBoth(pDevice, i, PostFxResources.prePostFx[i]);
                            pDevice->SetSamplerState(i, D3DSAMP_MAGFILTER, PostFxResources.Samplers[i]);
                        }

                        if (IsPostFxAA() || sharpenFromTemp2)
                            pDevice->SetRenderTarget(0, PostFxResources.FullScreenSurface_temp2);
                        else
                            pDevice->SetRenderTarget(0, PostFxResources.backBuffer);

                        if (PostFxResources.bEnablePreAlphaDepth)
                            SetTextureBoth(pDevice, 1, PostDepth());
                        SetTextureBoth(pDevice, 2, PostFxResources.textureRead);
                        pDevice->Clear(0, 0, D3DCLEAR_TARGET, 0, 0, 0);

                        pDevice->SetPixelShader(pShader);
                        pDevice->SetVertexShader(vShader);
                        //hr = pDevice->DrawPrimitive(D3DPT_TRIANGLELIST, 0, 2);
                        hbDrawPrimitivePostFX.fun();
                        // if(UsePostFxAA->get() > FusionFixSettings.AntialiasingText.eMO_OFF)
                        //     PostFxResources.swapbuffers();
                    }

                    // Anti aliasing
                    if (IsPostFxAA())
                    {
                        ProfilerScope timed(pDevice, kProfPostAA);
                        // FXAA
                        if ((UsePostFxAA->get() == FusionFixSettings.AntialiasingText.eFXAA) && PostFxResources.FxaaPS)
                        {
                            pDevice->SetPixelShader(PostFxResources.FxaaPS);

                            // pDevice->SetRenderTarget(0, PostFxResources.renderTargetSurf);
                            pDevice->SetRenderTarget(0, PostFxResources.backBuffer);

                            SetTextureBoth(pDevice, 2, PostFxResources.FullScreenTex_temp2->mD3DTexture);
                            // SetTextureBoth(pDevice, 2, PostFxResources.textureRead);

                            hr = pDevice->DrawPrimitive(D3DPT_TRIANGLELIST, 0, 2);
                            pDevice->SetPixelShader(pShader);
                        }

                        // SMAA
                        if (UsePostFxAA->get() == FusionFixSettings.AntialiasingText.eSMAA &&
                           PostFxResources.SMAA_EdgeDetection && PostFxResources.SMAA_BlendingWeightsCalculation && PostFxResources.SMAA_NeighborhoodBlending &&
                           PostFxResources.SMAA_EdgeDetectionVS && PostFxResources.SMAA_BlendingWeightsCalculationVS && PostFxResources.SMAA_NeighborhoodBlendingVS &&
                           PostFxResources.SMAA_areaTex && PostFxResources.SMAA_searchTex && PostFxResources.edgesTex && PostFxResources.blendTex
                           )
                        {
                            DWORD oldSample = 0;

                            for (int i = 0; i <= 4; ++i)
                            {
                                pDevice->GetSamplerState(i, D3DSAMP_MINFILTER, &oldSample);
                                pDevice->GetSamplerState(i, D3DSAMP_MAGFILTER, &oldSample);
                                pDevice->GetSamplerState(i, D3DSAMP_MIPFILTER, &oldSample);
                                pDevice->GetSamplerState(i, D3DSAMP_ADDRESSU, &oldSample);
                                pDevice->GetSamplerState(i, D3DSAMP_ADDRESSV, &oldSample);
                                pDevice->GetSamplerState(i, D3DSAMP_ADDRESSW, &oldSample);
                            }

                            // colorTex / colorGammaTex
                            pDevice->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
                            pDevice->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
                            pDevice->SetSamplerState(0, D3DSAMP_MIPFILTER, D3DTEXF_POINT);
                            pDevice->SetSamplerState(0, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
                            pDevice->SetSamplerState(0, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
                            pDevice->SetSamplerState(0, D3DSAMP_ADDRESSW, D3DTADDRESS_WRAP);

                            // edgesTex
                            pDevice->SetSamplerState(1, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
                            pDevice->SetSamplerState(1, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
                            pDevice->SetSamplerState(1, D3DSAMP_MIPFILTER, D3DTEXF_LINEAR);
                            pDevice->SetSamplerState(1, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
                            pDevice->SetSamplerState(1, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
                            pDevice->SetSamplerState(1, D3DSAMP_ADDRESSW, D3DTADDRESS_WRAP);

                            // blendTex
                            pDevice->SetSamplerState(4, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
                            pDevice->SetSamplerState(4, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
                            pDevice->SetSamplerState(4, D3DSAMP_MIPFILTER, D3DTEXF_LINEAR);
                            pDevice->SetSamplerState(4, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
                            pDevice->SetSamplerState(4, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
                            pDevice->SetSamplerState(4, D3DSAMP_ADDRESSW, D3DTADDRESS_WRAP);

                            // areaTex
                            pDevice->SetSamplerState(2, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
                            pDevice->SetSamplerState(2, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
                            pDevice->SetSamplerState(2, D3DSAMP_MIPFILTER, D3DTEXF_LINEAR);
                            pDevice->SetSamplerState(2, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
                            pDevice->SetSamplerState(2, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
                            pDevice->SetSamplerState(2, D3DSAMP_ADDRESSW, D3DTADDRESS_CLAMP);

                            // searchTex
                            pDevice->SetSamplerState(3, D3DSAMP_MINFILTER, D3DTEXF_POINT);
                            pDevice->SetSamplerState(3, D3DSAMP_MAGFILTER, D3DTEXF_POINT);
                            pDevice->SetSamplerState(3, D3DSAMP_MIPFILTER, D3DTEXF_POINT);
                            pDevice->SetSamplerState(3, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
                            pDevice->SetSamplerState(3, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
                            pDevice->SetSamplerState(3, D3DSAMP_ADDRESSW, D3DTADDRESS_CLAMP);

                            // SMAA_EdgeDetection
                            pDevice->SetPixelShader(PostFxResources.SMAA_EdgeDetection);
                            pDevice->SetVertexShader(PostFxResources.SMAA_EdgeDetectionVS);
                            pDevice->SetRenderTarget(0, PostFxResources.edgesSurf);
                            SetTextureBoth(pDevice, 0, PostFxResources.FullScreenTex_temp2->mD3DTexture);
                            pDevice->Clear(0, 0, D3DCLEAR_TARGET, 0, 0, 0);
                            pDevice->DrawPrimitive(D3DPT_TRIANGLELIST, 0, 2);

                            // SMAA_BlendingWeightsCalculation
                            pDevice->SetPixelShader(PostFxResources.SMAA_BlendingWeightsCalculation);
                            pDevice->SetVertexShader(PostFxResources.SMAA_BlendingWeightsCalculationVS);
                            pDevice->SetRenderTarget(0, PostFxResources.blendSurf);
                            SetTextureBoth(pDevice, 1, PostFxResources.edgesTex->mD3DTexture);
                            SetTextureBoth(pDevice, 2, PostFxResources.SMAA_areaTex);
                            SetTextureBoth(pDevice, 3, PostFxResources.SMAA_searchTex);
                            pDevice->Clear(0, 0, D3DCLEAR_TARGET, 0, 0, 0);
                            pDevice->DrawPrimitive(D3DPT_TRIANGLELIST, 0, 2);

                            // SMAA_NeighborhoodBlending
                            pDevice->SetPixelShader(PostFxResources.SMAA_NeighborhoodBlending);
                            pDevice->SetVertexShader(PostFxResources.SMAA_NeighborhoodBlendingVS);

                            // pDevice->SetRenderTarget(0, PostFxResources.renderTargetSurf);
                            pDevice->SetRenderTarget(0, PostFxResources.backBuffer);

                            SetTextureBoth(pDevice, 0, PostFxResources.FullScreenTex_temp2->mD3DTexture);
                            SetTextureBoth(pDevice, 4, PostFxResources.blendTex->mD3DTexture);

                            pDevice->GetSamplerState(0, D3DSAMP_SRGBTEXTURE, &oldSample);
                            pDevice->GetRenderState(D3DRS_SRGBWRITEENABLE, &OldSRGB); // save srgb state
                            pDevice->SetSamplerState(0, D3DSAMP_SRGBTEXTURE, 1);
                            pDevice->SetRenderState(D3DRS_SRGBWRITEENABLE, 1);

                            pDevice->DrawPrimitive(D3DPT_TRIANGLELIST, 0, 2);

                            pDevice->SetSamplerState(0, D3DSAMP_SRGBTEXTURE, oldSample);
                            pDevice->SetRenderState(D3DRS_SRGBWRITEENABLE, OldSRGB); // restore srgb state

                            for (int i = 0; i <= 4; ++i)
                            {
                                pDevice->SetSamplerState(i, D3DSAMP_MINFILTER, oldSample);
                                pDevice->SetSamplerState(i, D3DSAMP_MAGFILTER, oldSample);
                                pDevice->SetSamplerState(i, D3DSAMP_MIPFILTER, oldSample);
                                pDevice->SetSamplerState(i, D3DSAMP_ADDRESSU, oldSample);
                                pDevice->SetSamplerState(i, D3DSAMP_ADDRESSV, oldSample);
                                pDevice->SetSamplerState(i, D3DSAMP_ADDRESSW, oldSample);
                            }

                            SetTextureBoth(pDevice, 0, PostFxResources.prePostFx[0]);
                            SetTextureBoth(pDevice, 1, PostFxResources.prePostFx[1]);
                            SetTextureBoth(pDevice, 2, PostFxResources.FullScreenTex_temp2->mD3DTexture);
                            SetTextureBoth(pDevice, 3, PostFxResources.prePostFx[3]);
                            SetTextureBoth(pDevice, 4, PostFxResources.prePostFx[4]);
                            pDevice->SetPixelShader(pShader);
                            pDevice->SetVertexShader(vShader);
                        }
                    }

                    if (sharpenNow)
                        ApplySharpening(pDevice, pShader, vShader, sharpenFromTemp2);
                    FrameGeneration::CaptureHudLess(pDevice, PostFxResources.backBuffer);

                    for (int i = 0; i < PostfxTextureCount; i++)
                    {
                        SetTextureBoth(pDevice, i, PostFxResources.prePostFx[i]);
                        pDevice->SetSamplerState(i, D3DSAMP_MAGFILTER, PostFxResources.Samplers[i]);
                        SAFE_RELEASE(PostFxResources.prePostFx[i]);
                    }
                    return S_OK;
                }

                for (int i = 0; i < PostfxTextureCount; i++)
                {
                    SetTextureBoth(pDevice, i, PostFxResources.prePostFx[i]);
                    pDevice->SetSamplerState(i, D3DSAMP_MAGFILTER, PostFxResources.Samplers[i]);
                    SAFE_RELEASE(PostFxResources.prePostFx[i]);
                }
                return S_FALSE;
            }
        }

        for (int i = 0; i < PostfxTextureCount; i++)
        {
            SetTextureBoth(pDevice, i, PostFxResources.prePostFx[i]);
            pDevice->SetSamplerState(i, D3DSAMP_MAGFILTER, PostFxResources.Samplers[i]);
            SAFE_RELEASE(PostFxResources.prePostFx[i]);
        }
        return S_FALSE;
    }

    static constexpr struct { D3DRENDERSTATETYPE state; DWORD value; } kSSRRenderStates[] =
    {
        { D3DRS_ZENABLE,          FALSE },
        { D3DRS_ZWRITEENABLE,     FALSE },
        { D3DRS_ALPHABLENDENABLE, FALSE },
        { D3DRS_ALPHATESTENABLE,  FALSE },
        { D3DRS_STENCILENABLE,    FALSE },
        { D3DRS_FOGENABLE,        FALSE },
        { D3DRS_CLIPPING,         FALSE },
        { D3DRS_CULLMODE,         D3DCULL_NONE },
        { D3DRS_COLORWRITEENABLE, D3DCOLORWRITEENABLE_RED | D3DCOLORWRITEENABLE_GREEN | D3DCOLORWRITEENABLE_BLUE | D3DCOLORWRITEENABLE_ALPHA },
    };

    static constexpr struct { D3DSAMPLERSTATETYPE state; DWORD value; } kSSRSamplerStates[] =
    {
        { D3DSAMP_ADDRESSU,  D3DTADDRESS_CLAMP },
        { D3DSAMP_ADDRESSV,  D3DTADDRESS_CLAMP },
        { D3DSAMP_MAGFILTER, D3DTEXF_POINT },
        { D3DSAMP_MINFILTER, D3DTEXF_POINT },
        { D3DSAMP_MIPFILTER, D3DTEXF_NONE },
        // Depths, normals and light: never sRGB, whatever the game's last draw left on the register
        { D3DSAMP_SRGBTEXTURE, FALSE },
    };
    static constexpr DWORD kSSRSamplerSlots = 8;
    static constexpr DWORD kSSRTextureSlots = 8;

    // The textures and the sampler states of the first kSSRTextureSlots samplers, saved on
    // construction and put back on destruction: deferred_lighting draws right after FusionFix's
    // passes and reads what the game bound there before them (the G-buffer normals on s1, and
    // more on s4). The AO pass left its own textures on s1 to s4 once GTAO used them.
    struct SavedSamplerSlots
    {
        IDirect3DDevice9* device;
        IDirect3DBaseTexture9* textures[kSSRTextureSlots] = {};
        DWORD states[kSSRSamplerSlots][std::size(kSSRSamplerStates)] = {};

        explicit SavedSamplerSlots(IDirect3DDevice9* pDevice) : device(pDevice)
        {
            for (DWORD slot = 0; slot < kSSRTextureSlots; ++slot)
                device->GetTexture(slot, &textures[slot]);
            for (DWORD slot = 0; slot < kSSRSamplerSlots; ++slot)
                for (size_t i = 0; i < std::size(kSSRSamplerStates); ++i)
                    device->GetSamplerState(slot, kSSRSamplerStates[i].state, &states[slot][i]);
        }

        ~SavedSamplerSlots()
        {
            for (DWORD slot = 0; slot < kSSRSamplerSlots; ++slot)
                for (size_t i = 0; i < std::size(kSSRSamplerStates); ++i)
                    device->SetSamplerState(slot, kSSRSamplerStates[i].state, states[slot][i]);
            for (DWORD slot = 0; slot < kSSRTextureSlots; ++slot)
            {
                SetTextureBoth(device, slot, textures[slot]);
                SAFE_RELEASE(textures[slot]);
            }
        }

        SavedSamplerSlots(const SavedSamplerSlots&) = delete;
        SavedSamplerSlots& operator=(const SavedSamplerSlots&) = delete;
    };

    // For effects begun with D3DXFX_DONOTSAVESTATE. D3DX's own state saving goes through state blocks on the game's
    // device wrapper, which drops a restore it takes for no change while the device behind it holds another value (see
    // SetTextureBoth): textures and shaders an effect had set stayed on the device. These take what the device itself
    // holds and put it back through the wrapper and on the device.
    struct SavedShaders
    {
        IDirect3DDevice9* device;
        IDirect3DDevice9* real;
        IDirect3DPixelShader9* ps = nullptr;
        IDirect3DVertexShader9* vs = nullptr;

        explicit SavedShaders(IDirect3DDevice9* pDevice) : device(pDevice), real(RealDevice(pDevice))
        {
            real->GetPixelShader(&ps);
            real->GetVertexShader(&vs);
        }

        ~SavedShaders()
        {
            device->SetPixelShader(ps);
            device->SetVertexShader(vs);
            if (real != device)
            {
                real->SetPixelShader(ps);
                real->SetVertexShader(vs);
            }
            SAFE_RELEASE(ps);
            SAFE_RELEASE(vs);
        }

        SavedShaders(const SavedShaders&) = delete;
        SavedShaders& operator=(const SavedShaders&) = delete;
    };

    // The render states the passes of FusionFix's effects set (AO.fx's are the most)
    struct SavedEffectPassStates
    {
        static constexpr D3DRENDERSTATETYPE kStates[] =
        {
            D3DRS_ZENABLE, D3DRS_ZWRITEENABLE, D3DRS_ALPHABLENDENABLE, D3DRS_SRCBLEND, D3DRS_DESTBLEND, D3DRS_ALPHATESTENABLE,
            D3DRS_STENCILENABLE, D3DRS_CULLMODE, D3DRS_FOGENABLE, D3DRS_CLIPPING, D3DRS_COLORWRITEENABLE,
        };
        IDirect3DDevice9* device;
        IDirect3DDevice9* real;
        DWORD values[std::size(kStates)] = {};

        explicit SavedEffectPassStates(IDirect3DDevice9* pDevice) : device(pDevice), real(RealDevice(pDevice))
        {
            for (size_t i = 0; i < std::size(kStates); ++i)
                real->GetRenderState(kStates[i], &values[i]);
        }

        ~SavedEffectPassStates()
        {
            for (size_t i = 0; i < std::size(kStates); ++i)
            {
                device->SetRenderState(kStates[i], values[i]);
                if (real != device)
                    real->SetRenderState(kStates[i], values[i]);
            }
        }

        SavedEffectPassStates(const SavedEffectPassStates&) = delete;
        SavedEffectPassStates& operator=(const SavedEffectPassStates&) = delete;
    };
    static constexpr UINT kPSConstCount = 224;
    static constexpr UINT kVSConstCount = 256;
    static inline float savedPSConsts[kPSConstCount * 4];
    static inline float savedVSConsts[kVSConstCount * 4];

    // The float shader constants, saved on construction and put back on destruction. An
    // effect writes its parameters into them, over what the game set for deferred_lighting,
    // which draws right after FusionFix's passes: once GTAO's accumulation took a dozen more,
    // the sun's went and it stopped lighting anything.
    struct SavedShaderConstants
    {
        IDirect3DDevice9* device;
        std::vector<float> ps = std::vector<float>(kPSConstCount * 4);
        std::vector<float> vs = std::vector<float>(kVSConstCount * 4);

        explicit SavedShaderConstants(IDirect3DDevice9* pDevice) : device(pDevice)
        {
            device->GetPixelShaderConstantF(0, ps.data(), kPSConstCount);
            device->GetVertexShaderConstantF(0, vs.data(), kVSConstCount);
        }

        ~SavedShaderConstants()
        {
            device->SetPixelShaderConstantF(0, ps.data(), kPSConstCount);
            device->SetVertexShaderConstantF(0, vs.data(), kVSConstCount);
        }

        SavedShaderConstants(const SavedShaderConstants&) = delete;
        SavedShaderConstants& operator=(const SavedShaderConstants&) = delete;
    };

    static void MatrixMultiply(D3DXMATRIX& out, const D3DXMATRIX& a, const D3DXMATRIX& b)
    {
        D3DXMATRIX r;
        for (int i = 0; i < 4; ++i)
            for (int j = 0; j < 4; ++j)
                r.m[i][j] = a.m[i][0] * b.m[0][j] + a.m[i][1] * b.m[1][j]
                          + a.m[i][2] * b.m[2][j] + a.m[i][3] * b.m[3][j];
        out = r;
    }

    // PostFxProfiler: GPU time of FusionFix's passes, from timestamp queries. Each frame's queries
    // are read kProfilerFrames frames later, without waiting on the GPU, and every 120 frames the
    // averages in milliseconds per frame are written to GTAIV.EFLC.FusionFix.PostFx.log next to the plugin.
    // The sections form a tree: an effect and the passes it is made of, each pass inside its
    // effect's time. What of a section its passes leave is shown as "other": in the lighting phase
    // that is the game's lights with their local contact shadows, and the light shafts. A section
    // can be entered more than once a frame, as the water is, and its times add up. The GGX
    // highlights, the sky reflection's BRDF and the rain's rings on the water are a few
    // instructions more in the game's own lighting and water shaders: they show in the lighting
    // phase's lamps, sun and water, not as sections of their own. The lighting phase's game part is
    // split by where the game is (SetLightingStage): its sun and ambient once FusionFix's passes are
    // done, its lamps from the first light of the light loop, its shafts from the shaft loop.
    // A frame runs from one post processing to the next. Off, no query is made.
    enum ProfilerSection
    {
        kProfLighting,
            kProfAO, kProfAODepth, kProfAOMain, kProfAOTemporal, kProfAOBlur, kProfAOApply,
            kProfSSR, kProfSSRTrace, kProfSSRFill, kProfSSRResolve, kProfSSRDenoise, kProfSSRTemporal, kProfSSRDebug,
            kProfContact, kProfContactMarch, kProfContactUpsample, kProfContactDenoise, kProfContactTemporal,
            kProfGI, kProfGIMarch, kProfGIDenoise, kProfGITemporal, kProfGIUpsample,
            kProfLightSun, kProfLightLocal, kProfLightShafts, kProfGlints,
        kProfDepthCopy,
        kProfMotion,
        kProfWetGround, kProfWetGroundCopies, kProfWetGroundPass,
        kProfWaterAll, kProfWaterGame, kProfWater,
        kProfCloudReflection, kProfCloudReflectionMap, kProfCloudReflectionWater,
        kProfFogPass,
            kProfClouds, kProfCloudsSkyRef, kProfCloudsMarch, kProfCloudsLight, kProfCloudsResolve, kProfCloudsComposite,
            kProfSkin, kProfSkinLight, kProfSkinScatter, kProfSkinFinal,
            kProfFog,
        kProfResolve,
            kProfResolveReactive, kProfResolveUpscale,
        kProfPost,
            kProfPostTAA, kProfPostStipple, kProfPostDOF, kProfPostSunShafts, kProfPostGame, kProfPostAA, kProfPostSharpen,
        kProfHDROutput,
        kProfFrameGeneration,
            kProfFrameGenerationCopies, kProfFrameGenerationGenerate, kProfFrameGenerationShow,
        kProfFrameGenerationPresent,
        kProfSections
    };
    struct ProfilerSectionInfo { const char* name; int parent; };
    static constexpr ProfilerSectionInfo kProfilerSectionInfo[] =
    {
        { "lighting phase", -1 },
            { "ambient occlusion", kProfLighting }, { "depth and its mips", kProfAO }, { "occlusion", kProfAO },
            { "accumulation", kProfAO }, { "blur", kProfAO }, { "into the G-buffer", kProfAO },
            { "SSR", kProfLighting }, { "march", kProfSSR }, { "fill of misses", kProfSSR }, { "resolve", kProfSSR },
            { "smoothing", kProfSSR }, { "accumulation", kProfSSR }, { "debug view", kProfSSR },
            { "contact shadows", kProfLighting }, { "march", kProfContact }, { "upsample", kProfContact },
            { "smoothing", kProfContact }, { "accumulation", kProfContact },
            { "indirect light", kProfLighting }, { "march", kProfGI }, { "smoothing", kProfGI },
            { "accumulation", kProfGI }, { "upsample", kProfGI },
            { "the game's sun and ambient", kProfLighting }, { "the game's lamps and headlights", kProfLighting },
            { "the game's light shafts", kProfLighting }, { "headlight glints", kProfLightShafts },
        { "depth copy at the G-buffer's end", -1 },
        { "motion vectors (temporal AA, upscaling)", -1 },
        { "wet ground", -1 }, { "copies of the G-buffer", kProfWetGround }, { "wet pass", kProfWetGround },
        { "water", -1 }, { "the game's water", kProfWaterAll }, { "SSR on the water", kProfWaterAll },
        { "clouds in reflections", -1 }, { "the reflection map", kProfCloudReflection }, { "the water's reflection", kProfCloudReflection },
        { "fog pass", -1 },
            { "volumetric clouds", kProfFogPass }, { "sky brightness", kProfClouds }, { "march", kProfClouds },
            { "light", kProfClouds }, { "accumulation", kProfClouds }, { "into the scene", kProfClouds },
            { "skin scattering", kProfFogPass }, { "light on skin", kProfSkin }, { "scatter", kProfSkin },
            { "final", kProfSkin },
            { "the game's fog", kProfFogPass },
        { "temporal AA / upscaling", -1 },
            { "reactive mask", kProfResolve }, { "DLSS / FSR, copies and wait included", kProfResolve },
        { "post processing", -1 },
            { "temporal AA", kProfPost }, { "stipple filter", kProfPost }, { "depth of field", kProfPost },
            { "sun shafts", kProfPost }, { "the game's post processing", kProfPost }, { "FXAA / SMAA", kProfPost },
            { "sharpening", kProfPost },
        { "HDR output", -1 },
        { "frame generation", -1 },
            { "frame made ready for it", kProfFrameGeneration }, { "generation (with its wait unless deferred)", kProfFrameGeneration },
            { "generated frame into the back buffer", kProfFrameGeneration },
        { "frames presented inside the next frame", -1 },
    };
    static_assert(std::size(kProfilerSectionInfo) == kProfSections);
    // The game's own passes, between FusionFix's top level sections, are timed by the render target the
    // game has locked (grcTextureFactoryPC::LockRenderTarget, index 0): shadow maps, the G-buffer, the
    // reflection maps and so on, each a section of its own after kProfSections, named by the target as
    // the game first locks it. kProfTargetScreen stands for what the game draws with no target locked.
    static constexpr int kProfilerTargets = 64;
    static constexpr int kProfTargetScreen = kProfSections;
    static constexpr int kProfAll = kProfSections + kProfilerTargets;
    static constexpr int kProfilerFrames = 4;
    static constexpr int kProfilerAverage = 120;
    static constexpr int kProfilerStamps = 512; // timestamps a frame, two per section entered
    struct ProfilerFrame
    {
        IDirect3DQuery9* disjoint = nullptr;
        IDirect3DQuery9* freq = nullptr;
        IDirect3DQuery9* start = nullptr;
        IDirect3DQuery9* stop = nullptr;
        // Made as they are first needed, and kept for the frames after
        IDirect3DQuery9* stamps[kProfilerStamps] = {};
        struct { int section; bool begin; } marks[kProfilerStamps] = {};
        int count = 0;
        bool issued = false;
    };
    static inline ProfilerFrame profilerFrames[kProfilerFrames];
    static inline int nProfilerFrame = -1;
    static inline double profilerSums[kProfAll] = {};
    static inline std::string profilerTargetNames[kProfilerTargets] = { "no target locked (the screen)" };
    static inline int nProfilerTargets = 1;
    static inline std::unordered_map<const void*, int> profilerTargetIds; // the game's targets by address
    static inline double profilerFrameSum = 0.0;
    static inline int nProfilerSamples = 0;

    static void ReleaseProfilerFrame(ProfilerFrame& f)
    {
        SAFE_RELEASE(f.disjoint);
        SAFE_RELEASE(f.freq);
        SAFE_RELEASE(f.start);
        SAFE_RELEASE(f.stop);
        for (auto& q : f.stamps)
            SAFE_RELEASE(q);
        f.count = 0;
        f.issued = false;
    }

    static void ReleaseProfiler()
    {
        for (auto& f : profilerFrames)
            ReleaseProfilerFrame(f);
        nProfilerFrame = -1;
    }

    static bool CreateProfilerFrame(IDirect3DDevice9* pDevice, ProfilerFrame& f)
    {
        if (f.disjoint)
            return true;
        bool ok = SUCCEEDED(pDevice->CreateQuery(D3DQUERYTYPE_TIMESTAMPDISJOINT, &f.disjoint)) &&
                  SUCCEEDED(pDevice->CreateQuery(D3DQUERYTYPE_TIMESTAMPFREQ, &f.freq)) &&
                  SUCCEEDED(pDevice->CreateQuery(D3DQUERYTYPE_TIMESTAMP, &f.start)) &&
                  SUCCEEDED(pDevice->CreateQuery(D3DQUERYTYPE_TIMESTAMP, &f.stop));
        if (!ok)
            ReleaseProfilerFrame(f);
        return ok;
    }

    // A section, its passes below it, then what they leave of it.
    static void WriteProfilerSection(FusionLog::Block& log, int section, int depth, double n)
    {
        log.Printf("%*s%-*s %6.2f\n", 2 + depth * 2, "", 34 - depth * 2, kProfilerSectionInfo[section].name, profilerSums[section] / n);
        double children = 0.0;
        bool any = false;
        for (int i = 0; i < kProfSections; ++i)
        {
            if (kProfilerSectionInfo[i].parent != section || profilerSums[i] <= 0.0)
                continue;
            WriteProfilerSection(log, i, depth + 1, n);
            children += profilerSums[i];
            any = true;
        }
        if (any)
            log.Printf("%*s%-*s %6.2f\n", 4 + depth * 2, "", 32 - depth * 2, "other", (std::max)(profilerSums[section] - children, 0.0) / n);
    }

    static void WriteProfilerBlock()
    {
        {
            FusionLog::Block log("PostFx", "Profiler");
            const double n = double(nProfilerSamples);
            log.Printf("GPU milliseconds per frame, averaged over %d frames\n", nProfilerSamples);
            log.Printf("%-36s %6.2f\n", "frame", profilerFrameSum / n);
            double sections = 0.0;
            for (int i = 0; i < kProfSections; ++i)
            {
                if (kProfilerSectionInfo[i].parent >= 0 || profilerSums[i] <= 0.0)
                    continue;
                WriteProfilerSection(log, i, 0, n);
                sections += profilerSums[i];
            }
            const double game = (std::max)(profilerFrameSum - sections, 0.0);
            log.Printf("  %-34s %6.2f\n", "the game's other passes", game / n);
            // By the target the game drew into, the most expensive first; what is left over is time no target
            // accounts for, such as the GPU waiting on the game.
            int order[kProfilerTargets];
            for (int i = 0; i < nProfilerTargets; ++i)
                order[i] = i;
            std::sort(order, order + nProfilerTargets,
                      [](int a, int b) { return profilerSums[kProfSections + a] > profilerSums[kProfSections + b]; });
            double targets = 0.0;
            for (int i = 0; i < nProfilerTargets; ++i)
            {
                const double t = profilerSums[kProfSections + order[i]];
                if (t / n < 0.005)
                    continue;
                log.Printf("    %-32s %6.2f\n", profilerTargetNames[order[i]].c_str(), t / n);
                targets += t;
            }
            if (targets > 0.0)
                log.Printf("    %-32s %6.2f\n", "other", (std::max)(game - targets, 0.0) / n);
        }
        ResetProfilerSums();
    }

    // Adds a frame whose queries were issued kProfilerFrames frames ago, if the GPU has them.
    static void ReadProfilerFrame(ProfilerFrame& f)
    {
        BOOL disjoint = TRUE;
        UINT64 freq = 0, start = 0, stop = 0;
        if (f.disjoint->GetData(&disjoint, sizeof(disjoint), 0) != S_OK || disjoint ||
            f.freq->GetData(&freq, sizeof(freq), 0) != S_OK || !freq ||
            f.start->GetData(&start, sizeof(start), 0) != S_OK || f.stop->GetData(&stop, sizeof(stop), 0) != S_OK)
            return;
        double ms[kProfAll] = {};
        UINT64 open[kProfAll] = {};
        bool opened[kProfAll] = {};
        for (int i = 0; i < f.count; ++i)
        {
            UINT64 t = 0;
            if (f.stamps[i]->GetData(&t, sizeof(t), 0) != S_OK)
                return;
            const int section = f.marks[i].section;
            if (f.marks[i].begin)
            {
                open[section] = t;
                opened[section] = true;
            }
            else if (opened[section] && t >= open[section])
            {
                ms[section] += double(t - open[section]) * 1000.0 / double(freq);
                opened[section] = false;
            }
        }
        for (int i = 0; i < kProfAll; ++i)
            profilerSums[i] += ms[i];
        profilerFrameSum += double(stop - start) * 1000.0 / double(freq);
        if (++nProfilerSamples >= kProfilerAverage)
            WriteProfilerBlock();
    }

    // Starts the averages over, as the profiler is turned on and as the ini is read again in game, so a
    // block holds no frames from before.
    static void ResetProfilerSums()
    {
        std::fill(std::begin(profilerSums), std::end(profilerSums), 0.0);
        profilerFrameSum = 0.0;
        nProfilerSamples = 0;
    }

    // Whether the profiler ran last frame: PostFxProfiler is a live setting, Ctrl+Shift+F10 turns it on
    // and off in game.
    static inline bool bProfilerRunning = false;

    // Once a frame, as post processing begins: closes this frame's queries and opens the next.
    static void ProfilerNextFrame(IDirect3DDevice9* pDevice)
    {
        auto& R = PostFxResources;
        if (!pDevice)
            return;
        if (!R.bPostFxProfiler)
        {
            if (bProfilerRunning)
            {
                ReleaseProfiler();
                ResetProfilerSums();
                nProfilerOpenTarget = -1;
                nProfilerOwnSections = 0;
                bProfilerRunning = false;
                FusionLog::Block log("PostFx", "Profiler");
                log.Printf("off\n");
            }
            return;
        }
        if (!bProfilerRunning)
        {
            ResetProfilerSums();
            bProfilerRunning = true;
            FusionLog::Block log("PostFx", "Profiler");
            log.Printf("on, a block every %d frames\n", kProfilerAverage);
        }
        InstallProfilerTargetHooks();
        // The target section open at the frame's end is closed in this frame and opened again in the next.
        const int target = nProfilerOpenTarget;
        if (target >= 0)
            ProfilerStamp(pDevice, target, false);
        nProfilerOpenTarget = -1;
        nProfilerOwnSections = 0;
        if (nProfilerFrame >= 0 && profilerFrames[nProfilerFrame].issued)
        {
            auto& cur = profilerFrames[nProfilerFrame];
            cur.stop->Issue(D3DISSUE_END);
            cur.freq->Issue(D3DISSUE_END);
            cur.disjoint->Issue(D3DISSUE_END);
        }
        nProfilerFrame = (nProfilerFrame + 1) % kProfilerFrames;
        auto& f = profilerFrames[nProfilerFrame];
        if (f.issued)
        {
            ReadProfilerFrame(f);
            f.issued = false;
        }
        if (!CreateProfilerFrame(pDevice, f))
        {
            R.bPostFxProfiler = false; // no timestamp queries on this device
            ReleaseProfiler();
            return;
        }
        f.count = 0;
        f.disjoint->Issue(D3DISSUE_BEGIN);
        f.start->Issue(D3DISSUE_END);
        f.issued = true;
        SwitchProfilerTarget(pDevice);
    }

    static void ProfilerStamp(IDirect3DDevice9* pDevice, int section, bool begin)
    {
        if (!PostFxResources.bPostFxProfiler || !pDevice || nProfilerFrame < 0 || section < 0)
            return;
        auto& f = profilerFrames[nProfilerFrame];
        if (!f.issued || f.count >= kProfilerStamps)
            return;
        auto& q = f.stamps[f.count];
        if (!q && FAILED(pDevice->CreateQuery(D3DQUERYTYPE_TIMESTAMP, &q)))
            return;
        q->Issue(D3DISSUE_END);
        f.marks[f.count++] = { section, begin };
    }

    // The game's target sections (see kProfilerTargets): the targets it has locked, innermost last; the
    // section open now, -1 for none; and how many of FusionFix's top level sections are open, which
    // pause the game's while they run.
    static inline std::vector<int> profilerTargetStack;
    static inline int nProfilerOpenTarget = -1;
    static inline int nProfilerOwnSections = 0;

    static void SwitchProfilerTarget(IDirect3DDevice9* pDevice)
    {
        if (!PostFxResources.bPostFxProfiler)
            return;
        const int want = nProfilerOwnSections > 0 ? -1
                       : kProfTargetScreen + (profilerTargetStack.empty() ? 0 : profilerTargetStack.back());
        if (want == nProfilerOpenTarget)
            return;
        if (nProfilerOpenTarget >= 0)
            ProfilerStamp(pDevice, nProfilerOpenTarget, false);
        if (want >= 0)
            ProfilerStamp(pDevice, want, true);
        nProfilerOpenTarget = want;
    }

    static void ProfilerMark(IDirect3DDevice9* pDevice, int section, bool begin)
    {
        if (!PostFxResources.bPostFxProfiler || !pDevice || nProfilerFrame < 0 || section < 0)
            return;
        const bool topLevel = section < kProfSections && kProfilerSectionInfo[section].parent < 0;
        if (topLevel && begin)
        {
            ++nProfilerOwnSections;
            SwitchProfilerTarget(pDevice);
        }
        ProfilerStamp(pDevice, section, begin);
        if (topLevel && !begin && nProfilerOwnSections > 0)
        {
            --nProfilerOwnSections;
            SwitchProfilerTarget(pDevice);
        }
    }

    // The game's targets come and go through grcTextureFactoryPC's LockRenderTarget and
    // UnlockRenderTarget (vtable slots 15 and 16), which the game only calls through the vtable. The
    // two entries are replaced once the profiler runs, and only then.
    using LockRenderTargetFn = void(__thiscall*)(void*, uint32_t, rage::grcRenderTargetPC*, rage::grcRenderTargetPC*, uint32_t, bool, uint32_t);
    using UnlockRenderTargetFn = void(__thiscall*)(void*, uint32_t, void*, int32_t);
    static inline LockRenderTargetFn pfnLockRenderTarget = nullptr;
    static inline UnlockRenderTargetFn pfnUnlockRenderTarget = nullptr;

    static int ProfilerTargetId(rage::grcRenderTargetPC* rt)
    {
        if (!rt)
            return 0;
        std::string name = rt->mName ? rt->mName : "unnamed target";
        // A target the game made again, as on a resolution change, can take the address of another.
        if (auto it = profilerTargetIds.find(rt); it != profilerTargetIds.end() && (it->second == 0 || profilerTargetNames[it->second] == name))
            return it->second;
        int id = -1;
        for (int i = 1; i < nProfilerTargets; ++i)
            if (profilerTargetNames[i] == name)
                id = i;
        if (id < 0 && nProfilerTargets < kProfilerTargets)
        {
            id = nProfilerTargets++;
            profilerTargetNames[id] = std::move(name);
        }
        if (id < 0)
            id = 0; // out of sections: counted with the screen
        profilerTargetIds[rt] = id;
        return id;
    }

    static void __fastcall ProfilerLockRenderTarget(void* factory, void*, uint32_t index, rage::grcRenderTargetPC* color,
                                                    rage::grcRenderTargetPC* depth, uint32_t a5, bool a6, uint32_t mip)
    {
        pfnLockRenderTarget(factory, index, color, depth, a5, a6, mip);
        // Followed with the profiler off too, so the stack is right when it is turned on again in game.
        if (index != 0)
            return;
        // Shadow maps are drawn into a depth target alone. A lock the game never undid would grow the stack
        // for good; deeper than any nesting it uses, the stack starts over.
        if (profilerTargetStack.size() >= 16)
            profilerTargetStack.clear();
        profilerTargetStack.push_back(PostFxResources.bPostFxProfiler ? ProfilerTargetId(color ? color : depth) : 0);
        SwitchProfilerTarget(rage::grcDevice::GetD3DDevice());
    }

    static void __fastcall ProfilerUnlockRenderTarget(void* factory, void*, uint32_t index, void* resolveFlags, int32_t unused)
    {
        pfnUnlockRenderTarget(factory, index, resolveFlags, unused);
        if (index != 0 || profilerTargetStack.empty())
            return;
        profilerTargetStack.pop_back();
        SwitchProfilerTarget(rage::grcDevice::GetD3DDevice());
    }

    static void InstallProfilerTargetHooks()
    {
        if (pfnLockRenderTarget || !rage::grcTextureFactory::g_pTextureFactory)
            return;
        auto* factory = rage::grcTextureFactoryPC::GetInstance();
        if (!factory)
            return;
        auto* vft = *reinterpret_cast<uintptr_t**>(factory);
        pfnLockRenderTarget = reinterpret_cast<LockRenderTargetFn>(vft[15]);
        pfnUnlockRenderTarget = reinterpret_cast<UnlockRenderTargetFn>(vft[16]);
        injector::WriteMemory(&vft[15], reinterpret_cast<uintptr_t>(&ProfilerLockRenderTarget), true);
        injector::WriteMemory(&vft[16], reinterpret_cast<uintptr_t>(&ProfilerUnlockRenderTarget), true);
    }

    // Which part of the game's lighting is being drawn, for the profiler: 0 none, 1 the sun and
    // ambient, 2 the lamps, 3 the light shafts. It only moves on, so a part the game skips leaves
    // the one before running, and 0 closes whatever is open.
    static inline int nLightingStage = 0;
    static void SetLightingStage(IDirect3DDevice9* pDevice, int stage)
    {
        static constexpr int kSections[] = { -1, kProfLightSun, kProfLightLocal, kProfLightShafts };
        if (stage != 0 && stage <= nLightingStage)
            return;
        if (nLightingStage > 0)
            ProfilerMark(pDevice, kSections[nLightingStage], false);
        nLightingStage = stage;
        if (stage > 0)
            ProfilerMark(pDevice, kSections[stage], true);
    }

    // Times what runs from here to the end of the scope; a section below 0 times nothing.
    struct ProfilerScope
    {
        IDirect3DDevice9* device;
        int section;
        ProfilerScope(IDirect3DDevice9* pDevice, int s) : device(pDevice), section(s) { ProfilerMark(device, section, true); }
        ~ProfilerScope() { ProfilerMark(device, section, false); }
        ProfilerScope(const ProfilerScope&) = delete;
        ProfilerScope& operator=(const ProfilerScope&) = delete;
    };

    // Binds every sampler of the pixel shader of the pass just begun to the texture its effect
    // parameter holds, found through the shader's constant table (sampler X reads X2D in SSR.fx
    // and AO.fx). D3DX left some holding what the game had bound, about four a frame in the SSR
    // passes: a diagnostic pass read a G-buffer texture where it sampled the depth.
    // Which register takes which parameter is read once per effect and shader from the shader's
    // constant table and kept (EffectBindings), as the effects keep their shaders for the whole
    // game; cleared on a lost device. What the registers get is written for every draw, through the
    // game's device wrapper and the device both: SSR coming out empty in play, while it worked in the
    // pause menu, was the wrapper dropping textures it had on record (SetTextureBoth), not this map.

    // The same holds for the float constants: with the ones D3DX left, SSGI's accumulation decoded the right depth
    // (6.1 m) as 15.3 m through fNearPlane and fFarDivNear while vec2PrevDepthRange, in another register, came through.
    // Each float constant of the shader is written from its parameter, a register per vector or array element.
    struct EffectConstant
    {
        UINT reg; UINT count; D3DXHANDLE param; std::string name; D3DXREGISTER_SET set;
        // From the parameter's description: a scalar or vector of floats, integers or booleans, and its elements
        bool usable = false;
        D3DXPARAMETER_TYPE type = D3DXPT_FLOAT;
        UINT columns = 0;
        std::vector<D3DXHANDLE> elements;
    };

    struct EffectSampler
    {
        UINT reg; D3DXHANDLE param; std::string name;
        bool setStates = false;     // SSR.fx's samplers below kSSRSamplerSlots, whose states its callers save
        SamplerStates states{};
    };

    struct EffectBinding
    {
        std::vector<EffectSampler> samplers;
        std::vector<EffectConstant> constants;
    };

    static std::map<std::pair<ID3DXEffect*, IDirect3DPixelShader9*>, EffectBinding>& EffectBindings()
    {
        static std::map<std::pair<ID3DXEffect*, IDirect3DPixelShader9*>, EffectBinding> bindings;
        return bindings;
    }

    static std::vector<EffectSampler> FindEffectSamplers(ID3DXEffect* effect, IDirect3DPixelShader9* ps,
                                                         std::vector<EffectConstant>* constants = nullptr)
    {
        std::vector<EffectSampler> samplers;
        std::vector<DWORD> function;
        UINT size = 0;
        if (SUCCEEDED(ps->GetFunction(nullptr, &size)) && size)
        {
            function.resize((size + 3) / 4);
            if (FAILED(ps->GetFunction(function.data(), &size)))
                function.clear();
        }
        ID3DXConstantTable* table = nullptr;
        if (function.empty() || FAILED(D3DXGetShaderConstantTable(function.data(), &table)) || !table)
            return samplers;

        D3DXCONSTANTTABLE_DESC tableDesc = {};
        table->GetDesc(&tableDesc);
        for (UINT i = 0; i < tableDesc.Constants; ++i)
        {
            D3DXCONSTANT_DESC desc = {};
            UINT count = 1;
            if (FAILED(table->GetConstantDesc(table->GetConstant(nullptr, i), &desc, &count)) || !desc.Name)
                continue;
            if (desc.RegisterSet != D3DXRS_SAMPLER && constants)
            {
                if (D3DXHANDLE param = effect->GetParameterByName(nullptr, desc.Name))
                    constants->push_back({ desc.RegisterIndex, desc.RegisterCount, param, desc.Name, desc.RegisterSet });
                continue;
            }
            if (desc.RegisterSet != D3DXRS_SAMPLER)
                continue;
            if (D3DXHANDLE param = effect->GetParameterByName(nullptr, (std::string(desc.Name) + "2D").c_str()))
                samplers.push_back({ desc.RegisterIndex, param, desc.Name });
        }
        table->Release();
        return samplers;
    }

    static void BindEffectConstants(IDirect3DDevice9* pDevice, ID3DXEffect* effect, const std::vector<EffectConstant>& constants)
    {
        std::string traced;
        for (const auto& c : constants)
        {
            if (!c.usable)
                continue;
            const UINT elements = UINT(c.elements.size());
            const UINT columns = c.columns;
            // Integers and booleans in their own register sets (loop counters, static branches)
            if (c.set == D3DXRS_INT4 || c.set == D3DXRS_BOOL)
            {
                int values[16 * 4] = {};
                for (UINT e = 0; e < elements; ++e)
                {
                    D3DXHANDLE h = c.elements[e];
                    if (c.type == D3DXPT_FLOAT)
                    {
                        float f[4] = {};
                        effect->GetFloatArray(h, f, columns);
                        for (UINT k = 0; k < columns; ++k)
                            values[e * 4 + k] = int(f[k]);
                    }
                    else
                        effect->GetIntArray(h, &values[e * 4], columns);
                }
                if (c.set == D3DXRS_INT4)
                {
                    pDevice->SetPixelShaderConstantI(c.reg, values, c.count);
                    if (auto real = RealDevice(pDevice); real != pDevice)
                        real->SetPixelShaderConstantI(c.reg, values, c.count);
                }
                else
                {
                    BOOL b[16] = {};
                    for (UINT e = 0; e < (std::min)(c.count, 16u); ++e)
                        b[e] = values[e * 4] != 0;
                    pDevice->SetPixelShaderConstantB(c.reg, b, c.count);
                    if (auto real = RealDevice(pDevice); real != pDevice)
                        real->SetPixelShaderConstantB(c.reg, b, c.count);
                }
                continue;
            }
            if (c.set != D3DXRS_FLOAT4)
                continue;
            float want[16 * 4] = {};
            for (UINT e = 0; e < elements; ++e)
            {
                D3DXHANDLE h = c.elements[e];
                if (c.type == D3DXPT_FLOAT)
                    effect->GetFloatArray(h, &want[e * 4], columns);
                else
                {
                    int v[4] = {};
                    effect->GetIntArray(h, v, columns);
                    for (UINT k = 0; k < columns; ++k)
                        want[e * 4 + k] = float(v[k]);
                }
            }
            float have[16 * 4] = {};
            RealDevice(pDevice)->GetPixelShaderConstantF(c.reg, have, c.count);
            bool differs = false;
            for (UINT e = 0; e < elements; ++e)
                for (UINT k = 0; k < columns; ++k)
                    differs |= want[e * 4 + k] != have[e * 4 + k];
            if (differs)
            {
                // The components the shader does not read keep what the register held
                for (UINT e = 0; e < c.count; ++e)
                    for (UINT k = (e < elements ? columns : 0u); k < 4; ++k)
                        want[e * 4 + k] = have[e * 4 + k];
                pDevice->SetPixelShaderConstantF(c.reg, want, c.count);
                if (auto real = RealDevice(pDevice); real != pDevice)
                    real->SetPixelShaderConstantF(c.reg, want, c.count);
                if (SSRTrace::Active())
                {
                    char item[160];
                    snprintf(item, sizeof(item), " c%u %s=%g (was %g)", c.reg, c.name.c_str(), want[0], have[0]);
                    traced += item;
                }
            }
        }
        if (SSRTrace::Active() && !traced.empty())
            SSRTrace::Line("  constants set:%s", traced.c_str());
    }

    // The registers of a shader of an effect and the parameters they take, read once
    static const EffectBinding& GetEffectBinding(ID3DXEffect* effect, IDirect3DPixelShader9* ps)
    {
        auto& bindings = EffectBindings();
        const auto key = std::make_pair(effect, ps);
        if (auto it = bindings.find(key); it != bindings.end())
            return it->second;

        EffectBinding binding;
        binding.samplers = FindEffectSamplers(effect, ps, &binding.constants);
        for (auto& c : binding.constants)
        {
            D3DXPARAMETER_DESC pd = {};
            c.usable = SUCCEEDED(effect->GetParameterDesc(c.param, &pd)) &&
                (pd.Type == D3DXPT_FLOAT || pd.Type == D3DXPT_INT || pd.Type == D3DXPT_BOOL) &&
                (pd.Class == D3DXPC_SCALAR || pd.Class == D3DXPC_VECTOR) && c.count > 0 && c.count <= 16;
            if (!c.usable)
                continue;
            c.type = pd.Type;
            c.columns = (std::min)(pd.Columns, 4u);
            const UINT elements = pd.Elements ? (std::min)(pd.Elements, c.count) : 1;
            for (UINT e = 0; e < elements; ++e)
                c.elements.push_back(pd.Elements ? effect->GetParameterElement(c.param, e) : c.param);
        }
        if (effect == PostFxResources.SSREffect)
        {
            for (auto& sampler : binding.samplers)
            {
                if (sampler.reg >= kSSRSamplerSlots)
                    continue;
                auto found = SSRSamplerStates.find(sampler.name);
                sampler.setStates = true;
                sampler.states = found != SSRSamplerStates.end() ? found->second
                    : SamplerStates{ D3DTEXF_POINT, D3DTEXF_POINT, D3DTEXF_NONE, D3DTADDRESS_CLAMP, D3DTADDRESS_CLAMP };
            }
        }
        // A shader whose table could not be read is tried again next time
        if (binding.samplers.empty() && binding.constants.empty())
        {
            static EffectBinding none;
            return none;
        }
        return bindings.emplace(key, std::move(binding)).first->second;
    }

    // Writes the textures (and the float, int and bool constants) of the bound pixel shader from the effect's parameters,
    // and for SSR.fx, whose callers save samplers 0 to kSSRSamplerSlots - 1 around their passes, the sampler states.
    static void BindEffectConstantsOnly(IDirect3DDevice9* pDevice, ID3DXEffect* effect)
    {
        IDirect3DPixelShader9* ps = nullptr;
        if (FAILED(pDevice->GetPixelShader(&ps)) || !ps)
            return;
        const auto& binding = GetEffectBinding(effect, ps);
        ps->Release();
        BindEffectConstants(pDevice, effect, binding.constants);
    }

    static void BindEffectSamplers(IDirect3DDevice9* pDevice, ID3DXEffect* effect)
    {
        IDirect3DPixelShader9* ps = nullptr;
        if (FAILED(pDevice->GetPixelShader(&ps)) || !ps)
            return;
        const auto& binding = GetEffectBinding(effect, ps);
        const auto& samplers = binding.samplers;
        BindEffectConstants(pDevice, effect, binding.constants);
        const void* shader = ps;
        ps->Release();

        std::string traced;
        for (const auto& sampler : samplers)
        {
            const UINT reg = sampler.reg;
            const D3DXHANDLE param = sampler.param;
            if (sampler.setStates)
            {
                const SamplerStates& st = sampler.states;
                SetSamplerStateBoth(pDevice, reg, D3DSAMP_MINFILTER, st[0]);
                SetSamplerStateBoth(pDevice, reg, D3DSAMP_MAGFILTER, st[1]);
                SetSamplerStateBoth(pDevice, reg, D3DSAMP_MIPFILTER, st[2]);
                SetSamplerStateBoth(pDevice, reg, D3DSAMP_ADDRESSU, st[3]);
                SetSamplerStateBoth(pDevice, reg, D3DSAMP_ADDRESSV, st[4]);
                SetSamplerStateBoth(pDevice, reg, D3DSAMP_SRGBTEXTURE, FALSE);
            }
            IDirect3DBaseTexture9* want = nullptr;
            IDirect3DBaseTexture9* have = nullptr;
            effect->GetTexture(param, &want);
            if (SSRTrace::Active())
                RealDevice(pDevice)->GetTexture(reg, &have);
            // Always, through both: the wrapper may hold want on record while the device has another
            SetTextureBoth(pDevice, reg, want);
            if (SSRTrace::Active())
            {
                D3DXPARAMETER_DESC desc = {};
                effect->GetParameterDesc(param, &desc);
                DWORD minFilter = 0, srgb = 0;
                RealDevice(pDevice)->GetSamplerState(reg, D3DSAMP_MINFILTER, &minFilter);
                RealDevice(pDevice)->GetSamplerState(reg, D3DSAMP_SRGBTEXTURE, &srgb);
                traced += " s" + std::to_string(reg) + "=" + (desc.Name ? desc.Name : "?") + ":" + SSRTrace::TextureName(want) +
                    (want != have ? "(was " + SSRTrace::TextureName(have) + ")" : "") + (minFilter == D3DTEXF_LINEAR ? "/lin" : "/pt") + (srgb ? "/SRGB" : "");
            }
            SAFE_RELEASE(want);
            SAFE_RELEASE(have);
        }
        if (SSRTrace::Active())
            SSRTrace::Line("  bind ps %p, %u samplers:%s", shader, unsigned(samplers.size()), traced.c_str());
    }

    // Where the passes of SSR.fx read their noise this frame: moved on every frame while they accumulate, so the
    // accumulation averages it out, else the same every frame.
    static void SetNoiseOffset(ID3DXEffect* effect, bool accumulating)
    {
        auto offset = accumulating ? FrameHistory::NoiseOffset() : std::array<float, 2>{};
        effect->SetFloatArray(PostFxResources.SSREffectHandles.vec2NoiseOffset, offset.data(), 2);
    }

    // The accumulation passes of SSR.fx follow temporal AA's motion vectors, when it drew them this frame and there is
    // a history to take from them (see TemporalHistoryUV), else the camera alone.
    static void BindMotionVectors(ID3DXEffect* effect, bool history)
    {
        auto& h = PostFxResources.SSREffectHandles;
        auto motion = history ? FrameHistory::MotionVectors() : nullptr;
        auto jitter = FrameHistory::JitterDeltaUV();
        effect->SetTexture(h.MotionTex2D, motion);
        effect->SetFloat(h.fUseMotion, motion ? 1.0f : 0.0f);
        effect->SetFloatArray(h.vec2MotionJitter, jitter.data(), 2);
    }

    // The camera parameters the screen space effects share, under the same names in each effect.

    // View space from depth for a width x height target: the scale and offset that turn a pixel
    // into its view ray.
    static D3DXVECTOR4 ProjInfo(const D3DMATRIX& proj, float width, float height)
    {
        return D3DXVECTOR4(-2.0f / (width * proj._11), -2.0f / (height * proj._22),
                           (1.0f - proj._31) / proj._11, (1.0f + proj._32) / proj._22);
    }

    // vec2InvViewportSize and vec4ProjInfo: pixel size and reconstruction basis for a width x
    // height target.
    template <typename Handles>
    static void SetTargetSize(ID3DXEffect* effect, const Handles& h, const D3DMATRIX& proj, float width, float height)
    {
        const float invViewportSize[] = { 1.0f / width, 1.0f / height };
        effect->SetFloatArray(h.vec2InvViewportSize, invViewportSize, 2);
        const auto projInfo = ProjInfo(proj, width, height);
        effect->SetVector(h.vec4ProjInfo, &projInfo);
    }

    // fNearPlane and fFarDivNear, for linear depth from the depth buffer.
    template <typename Handles>
    static void SetDepthRange(ID3DXEffect* effect, const Handles& h, float nearClip, float farClip)
    {
        effect->SetFloat(h.fNearPlane, nearClip);
        effect->SetFloat(h.fFarDivNear, farClip / nearClip);
    }

    // vec2PrevDepthRange: fNearPlane and fFarDivNear of the scene PrevDepthTex was drawn in, the one
    // before this. The game moves the near plane as the camera closes in on a wall, and last frame's
    // depth decoded with this frame's planes failed every history's depth test.
    template <typename Handles>
    static void SetPrevDepthRange(ID3DXEffect* effect, const Handles& h)
    {
        const auto& prev = FrameHistory::Previous();
        const auto& camera = prev.Valid ? prev : FrameHistory::Current();
        const float range[] = { camera.Near, camera.Near > 0.0f ? camera.Far / camera.Near : 1.0f };
        effect->SetFloatArray(h.vec2PrevDepthRange, range, 2);
    }

    // World to reconstruction space rotation rows: the view's axes with x (and z, when _34 is
    // negative) flipped, as ViewToClipRows below.
    static void WorldToViewRows(const rage::grcViewport* vp, D3DXVECTOR4 rows[3])
    {
        const auto& viewInv = *(const D3DXMATRIX*)vp->mViewInverseMatrix;
        const float axisSign[3] = { -1.0f, 1.0f, (((const D3DMATRIX*)vp->mProjectionMatrix)->_34 < 0.0f) ? -1.0f : 1.0f };
        for (int row = 0; row < 3; ++row)
            rows[row] = D3DXVECTOR4(viewInv.m[row][0] * axisSign[row], viewInv.m[row][1] * axisSign[row],
                                    viewInv.m[row][2] * axisSign[row], 0.0f);
    }

    // vec4ViewToPrevClip: from SSR.fx's reconstruction space, whose x (and z, when _34 is
    // negative) run opposite to the game's view space, to viewProj's clip space.
    static void ViewToClipRows(const rage::grcViewport* vp, const D3DXMATRIX& viewProj, D3DXVECTOR4 rows[4])
    {
        D3DXMATRIX m;
        MatrixMultiply(m, *(const D3DXMATRIX*)vp->mViewInverseMatrix, viewProj);
        const float axisSign[4] = { -1.0f, 1.0f, (((const D3DMATRIX*)vp->mProjectionMatrix)->_34 < 0.0f) ? -1.0f : 1.0f, 1.0f };
        for (int row = 0; row < 4; ++row)
        {
            float s = axisSign[row];
            rows[row] = D3DXVECTOR4(m.m[row][0] * s, m.m[row][1] * s, m.m[row][2] * s, m.m[row][3] * s);
        }
    }

    static void RenderScreenSpaceReflections()
    {
        auto& R = PostFxResources;
        R.bSSRHistoryThisFrame = false;
        R.bWaterDoneThisFrame = false;
        R.SSRResult = nullptr;
        R.bSSRValidThisFrame = false;
        R.bSSRDebugValid = false;
        R.bGlassFrameValid = false;
        R.bSSRDenoised = false;

        if (!R.SSRSurf)
        {
            SSRTrace::Line("ssr: left, no SSRSurf");
            return;
        }

        IDirect3DDevice9* pDevice = rage::grcDevice::GetD3DDevice();
        if (!pDevice)
            return;

        auto clearSSR = [&]()
        {
            IDirect3DSurface9* oldRT = nullptr;
            pDevice->GetRenderTarget(0, &oldRT);
            pDevice->SetRenderTarget(0, R.SSRSurf);
            pDevice->Clear(0, nullptr, D3DCLEAR_TARGET, D3DCOLOR_ARGB(0, 0, 0, 0), 1.0f, 0);
            if (oldRT)
            {
                pDevice->SetRenderTarget(0, oldRT);
                oldRT->Release();
            }
        };

        if (!R.SSREnabled())
        {
            SSRTrace::Line("ssr: left, off in the menu");
            clearSSR();
            R.nSSRAccumFrame = 0;
            return;
        }

        rage::grcViewport* vp = rage::GetCurrentViewport();
        if (vp)
            SSRTrace::Line("ssr: vp %p %dx%d near %.3f far %.1f", static_cast<void*>(vp), int(vp->mWidth), int(vp->mHeight), vp->mNearClip, vp->mFarClip);
        if (!R.SSREffect || !R.mDepthRT || !R.SSRHistoryTex || !vp || R.fSSRIntensity <= 0.0f ||
            !R.SSRTraceSurf[0] || !R.SSRFallbackSurf[0] || !R.SSRMarchDepthSurf)
        {
            SSRTrace::Line("ssr: left, effect %p depth %p history %p vp %p intensity %.2f trace %p fallback %p march depth %p",
                static_cast<void*>(R.SSREffect), static_cast<void*>(R.mDepthRT), static_cast<void*>(R.SSRHistoryTex),
                static_cast<void*>(vp), R.fSSRIntensity, static_cast<void*>(R.SSRTraceSurf[0]), static_cast<void*>(R.SSRFallbackSurf[0]),
                static_cast<void*>(R.SSRMarchDepthSurf));
            clearSSR();
            R.nSSRAccumFrame = 0;
            return;
        }

        IDirect3DSurface9* rt0 = nullptr;
        IDirect3DSurface9* ds = nullptr;
        IDirect3DVertexDeclaration9* oldDecl = nullptr;
        IDirect3DVertexBuffer9* oldVB = nullptr;
        UINT oldOffset = 0, oldStride = 0;
        DWORD oldFVF = 0;
        D3DVIEWPORT9 oldViewport;

        pDevice->GetFVF(&oldFVF);
        pDevice->GetVertexDeclaration(&oldDecl);
        pDevice->GetStreamSource(0, &oldVB, &oldOffset, &oldStride);
        pDevice->GetRenderTarget(0, &rt0);
        pDevice->GetDepthStencilSurface(&ds);
        pDevice->GetViewport(&oldViewport);

        pDevice->SetDepthStencilSurface(nullptr);
        pDevice->SetStreamSource(0, nullptr, 0, 0);
        pDevice->SetVertexDeclaration(nullptr);
        pDevice->SetFVF(D3DFVF_XYZRHW | D3DFVF_TEX1);

        float width = float(RenderScale::ToRenderWidth(uint32_t(vp->mWidth)));
        float height = float(RenderScale::ToRenderHeight(uint32_t(vp->mHeight)));

        // Half: the march and the smoothing run on the half size targets, created as the full
        // size halved; the debug view stays full size.
        const bool half = R.SSRHalfRes() && R.SSRHalfSurf && R.SSRHalfDenoisedSurf && R.SSRTraceSurf[1] && R.SSRFallbackSurf[1];
        IDirect3DSurface9* ssrSurf = half ? R.SSRHalfSurf : R.SSRSurf;
        IDirect3DTexture9* ssrTex = half ? R.SSRHalfTex->mD3DTexture : R.SSRTex->mD3DTexture;
        IDirect3DSurface9* denoisedSurf = half ? R.SSRHalfDenoisedSurf : R.SSRDenoisedSurf;
        IDirect3DTexture9* denoisedTex = half ? R.SSRHalfDenoisedTex->mD3DTexture
                                              : (R.SSRDenoisedTex ? R.SSRDenoisedTex->mD3DTexture : nullptr);

        auto& h = R.SSREffectHandles;
        ID3DXEffect* effect = R.SSREffect;
        D3DMATRIX proj = *(D3DMATRIX*)vp->mProjectionMatrix;

        struct ScreenVertex { float x, y, z, rhw; float u, v; };
        ScreenVertex screenVertices[4] = {};
        // Viewport, quad and the effect's pixel size and reconstruction basis for a w x h target.
        auto setPassSize = [&](float w, float hgt)
        {
            D3DVIEWPORT9 vpDesc = {};
            vpDesc.MaxZ = 1.0f;
            vpDesc.Width = DWORD(w);
            vpDesc.Height = DWORD(hgt);
            pDevice->SetViewport(&vpDesc);

            const ScreenVertex quad[4] =
            {
                { -0.5f,      -0.5f,       0.0f, 1.0f, 0.0f, 0.0f },
                { -0.5f,       hgt - 0.5f, 0.0f, 1.0f, 0.0f, 1.0f },
                { w - 0.5f,   -0.5f,       0.0f, 1.0f, 1.0f, 0.0f },
                { w - 0.5f,    hgt - 0.5f, 0.0f, 1.0f, 1.0f, 1.0f }
            };
            memcpy(screenVertices, quad, sizeof(quad));
            SetTargetSize(effect, h, proj, w, hgt);
        };
        setPassSize(half ? float(DWORD(width) / 2) : width, half ? float(DWORD(height) / 2) : height);

        effect->SetTexture(h.DepthTex2D, LightingDepth());
        effect->SetTexture(h.HistoryTex2D, R.SSRHistoryTex->mD3DTexture);
        // Last frame's fog pass copied this depth along with the history; this frame's has not
        // run yet.
        const bool prevDepth = R.PreAlphaDepthCopyRT && R.PreAlphaDepthCopyRT->mD3DTexture;
        effect->SetTexture(h.PrevDepthTex2D, prevDepth ? R.PreAlphaDepthCopyRT->mD3DTexture : nullptr);
        effect->SetFloat(h.fUsePrevDepth, prevDepth ? 1.0f : 0.0f);
        SetPrevDepthRange(effect, h);

        // _DEFERRED_GBUFFER_2_ is (specular intensity, gloss, AO). Gloss decides what reflects:
        // car paint sits around 0.8, roads and walls around 0.2-0.35. Car paint stores almost no
        // specular intensity here, so that channel is not used.
        bool hasSpecular = R.mSpecularRT && R.mSpecularRT->mD3DTexture;
        if (hasSpecular)
            effect->SetTexture(h.SpecularTex2D, R.mSpecularRT->mD3DTexture);
        effect->SetFloat(h.fGlossBoost, hasSpecular ? R.fSSRGlossBoost : 0.0f);
        effect->SetFloat(h.fGlossCutoff, hasSpecular ? R.fSSRGlossCutoff : -1.0f);

        SetDepthRange(effect, h, vp->mNearClip, vp->mFarClip);

        // Last frame's camera, which the scene history and the accumulation were rendered with
        D3DXMATRIX prevViewProj;
        if (FrameHistory::Previous().Valid)
            prevViewProj = FrameHistory::Previous().ViewProjection;
        else
            MatrixMultiply(prevViewProj, *(const D3DXMATRIX*)vp->mViewMatrix, *(const D3DXMATRIX*)vp->mProjectionMatrix);

        D3DXVECTOR4 reprojRows[4];
        ViewToClipRows(vp, prevViewProj, reprojRows);
        effect->SetVectorArray(h.vec4ViewToPrevClip, reprojRows, 4);
        memcpy(R.SSRReprojRows, reprojRows, sizeof(reprojRows));
        R.bSSRReprojValid = true;

        effect->SetFloat(h.fMaxDistance, R.fSSRMaxDistance);
        effect->SetFloat(h.fThickness, R.fSSRThickness);
        effect->SetFloat(h.fEdgeFade, R.fSSREdgeFade);
        effect->SetFloat(h.fIntensity, R.fSSRIntensity);
        effect->SetFloat(h.fPassThinObjects, R.bSSRPassThinObjects ? 1.0f : 0.0f);
        effect->SetFloat(h.fStepJitter, R.bSSRStepJitter ? 1.0f : 0.0f);
        effect->SetFloat(h.fStepsPerPixel, 1.0f / R.fSSRStepPixels);
        effect->SetFloat(h.fMarchFullDepth, R.bSSRMarchFullDepth ? 1.0f : 0.0f);
        const bool temporal = R.fSSRTemporalBlend > 0.0f && h.techSSRTemporal && R.SSRAccumSurf[half][0] && R.SSRAccumSurf[half][1] &&
                              R.SSRHitDistSurf[half];
        SetNoiseOffset(effect, temporal && R.bSSRTemporalJitter);
        effect->SetFloat(h.fTowardCamera, R.fSSRTowardCamera);
        effect->SetFloat(h.fReflectionBlur, R.fSSRReflectionBlur);
        effect->SetFloat(h.fDistanceFade, R.fSSRDistanceFade);
        effect->SetFloat(h.fFallback, R.fSSRFallback);
        // Debug view 3 has SSRTrace_PS write where its rays hit in place of the hit, and SSR_PS pass it on.
        effect->SetFloat(h.fDebugMode, float(R.SSRDebugMode()));

        // World to reconstruction space rotation, for the G-buffer normals and the debug view.
        const D3DXMATRIX& viewInv = *(const D3DXMATRIX*)vp->mViewInverseMatrix;
        {
            D3DXVECTOR4 toView[3];
            WorldToViewRows(vp, toView);
            effect->SetVectorArray(h.vec4WaterToView, toView, 3);
        }
        const bool hasNormals = R.mNormalRT && R.mNormalRT->mD3DTexture;
        if (hasNormals)
            effect->SetTexture(h.NormalTex2D, R.mNormalRT->mD3DTexture);
        effect->SetFloat(h.fUseGBufferNormals, (hasNormals && R.bSSRGBufferNormals) ? 1.0f : 0.0f);
        // How wet the ground is, as the wet ground pass has it (RenderWetGround, which runs at the end
        // of the G-buffer pass every frame and keeps it up to date even while it draws nothing): it
        // follows CWeather::Rain over WetGroundWetting and WetGroundDrying seconds. Taken from the rain
        // itself, a shower turned on had SSR mirror the whole road at once, half a minute before the
        // ground pass made it wet.
        // Telling ground from walls takes the G-buffer normals, and the gloss the specular one.
        const bool wetGround = R.fSSRWetGround > 0.0f && hasNormals && hasSpecular;
        effect->SetFloat(h.fWetness, wetGround ? R.fWetness : 0.0f);
        effect->SetFloat(h.fWetGroundBoost, R.fSSRWetGround);

        UINT passes = 0;
        IDirect3DBaseTexture9* oldTextures[kSSRTextureSlots] = {};
        DWORD savedRenderStates[std::size(kSSRRenderStates)] = {};
        DWORD savedSamplerStates[kSSRSamplerSlots][std::size(kSSRSamplerStates)] = {};
        {
            for (DWORD slot = 0; slot < kSSRTextureSlots; ++slot)
                pDevice->GetTexture(slot, &oldTextures[slot]);
            SetTextureBoth(pDevice, 3, nullptr);

            pDevice->GetPixelShaderConstantF(0, savedPSConsts, kPSConstCount);
            pDevice->GetVertexShaderConstantF(0, savedVSConsts, kVSConstCount);

            for (size_t i = 0; i < std::size(kSSRRenderStates); ++i)
            {
                pDevice->GetRenderState(kSSRRenderStates[i].state, &savedRenderStates[i]);
                pDevice->SetRenderState(kSSRRenderStates[i].state, kSSRRenderStates[i].value);
            }

            for (DWORD slot = 0; slot < kSSRSamplerSlots; ++slot)
                for (size_t i = 0; i < std::size(kSSRSamplerStates); ++i)
                {
                    pDevice->GetSamplerState(slot, kSSRSamplerStates[i].state, &savedSamplerStates[slot][i]);
                    pDevice->SetSamplerState(slot, kSSRSamplerStates[i].state, kSSRSamplerStates[i].value);
                }

            // D3DX saving the state it changes, through state blocks, left the smoothing and the
            // accumulation in play drawing with what the game had bound: the textures they set
            // did not take. This function saves and puts back what the passes touch itself.
            effect->SetTechnique(h.techSSR);
            effect->Begin(&passes, D3DXFX_DONOTSAVESTATE);
        }
        {
            // The march, the guess for its misses (skipped while ScreenSpaceReflectionsFallback is
            // 0, SSR_PS then does not read it), and the two together. Each draws every pixel of
            // the viewport, no blending, nothing discarded: nothing is cleared first.
            auto draw = [&](UINT pass, IDirect3DSurface9* target)
            {
                const HRESULT rtHr = pDevice->SetRenderTarget(0, target);
                const HRESULT passHr = effect->BeginPass(pass);
                effect->CommitChanges();
                BindEffectSamplers(pDevice, effect);
                SSRTrace::State(pDevice, "ssr pass");
                const HRESULT drawHr = pDevice->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, screenVertices, sizeof(ScreenVertex));
                effect->EndPass();
                if (SSRTrace::Active())
                {
                    D3DVIEWPORT9 view = {};
                    pDevice->GetViewport(&view);
                    SSRTrace::Line("ssr: pass %u of %u into %p (%ux%u view) rt %08x pass %08x draw %08x",
                        pass, passes, static_cast<void*>(target), unsigned(view.Width), unsigned(view.Height),
                        unsigned(rtHr), unsigned(passHr), unsigned(drawHr));
                }
            };
            // The depth the march steps through, at half the full size whatever size SSR runs at;
            // timed with the march.
            ProfilerMark(pDevice, kProfSSRTrace, true);
            setPassSize(float(DWORD(width) / 2), float(DWORD(height) / 2));
            draw(4, R.SSRMarchDepthSurf);
            setPassSize(half ? float(DWORD(width) / 2) : width, half ? float(DWORD(height) / 2) : height);
            effect->SetTexture(h.MarchDepthTex2D, R.SSRMarchDepthTex->mD3DTexture);

            // The march also writes how far its rays went, for the accumulation.
            IDirect3DSurface9* oldTarget1 = nullptr;
            if (temporal)
            {
                pDevice->GetRenderTarget(1, &oldTarget1);
                pDevice->SetRenderTarget(1, R.SSRHitDistSurf[half]);
            }
            draw(0, R.SSRTraceSurf[half]);
            ProfilerMark(pDevice, kProfSSRTrace, false);
            if (temporal)
            {
                pDevice->SetRenderTarget(1, oldTarget1);
                SAFE_RELEASE(oldTarget1);
            }
            effect->SetTexture(h.SSRResultTex2D, R.SSRTraceTex[half]->mD3DTexture);
            IDirect3DTexture9* fill = R.SSRFallbackTex[half]->mD3DTexture;
            ProfilerMark(pDevice, kProfSSRFill, true);
            if (R.fSSRFallback > 0.0f)
            {
                draw(1, R.SSRFallbackSurf[half]);
                // The fill's cascade, each step twice as far out as the last (the first reached nine
                // pixels), taking turns between the two targets; in pixels of this size.
                if (R.SSRSpreadSurf[half])
                {
                    IDirect3DSurface9* surfs[2] = { R.SSRSpreadSurf[half], R.SSRFallbackSurf[half] };
                    IDirect3DTexture9* texs[2] = { R.SSRSpreadTex[half]->mD3DTexture, R.SSRFallbackTex[half]->mD3DTexture };
                    for (int step = 0; step < 2; ++step)
                    {
                        effect->SetTexture(h.SSRFallbackTex2D, fill);
                        effect->SetFloat(h.fSpreadRadius, step ? 36.0f : 18.0f);
                        draw(2, surfs[step]);
                        fill = texs[step];
                    }
                }
            }
            ProfilerMark(pDevice, kProfSSRFill, false);
            effect->SetTexture(h.SSRFallbackTex2D, fill);
            ProfilerMark(pDevice, kProfSSRResolve, true);
            draw(3, ssrSurf);
            ProfilerMark(pDevice, kProfSSRResolve, false);
        }
        effect->End();

        // Debug view, while the G-buffer still holds this frame. Changes nothing the game sees.
        // Smooth the result into the texture deferred_lighting reads.
        R.bSSRDenoised = false;
        if (R.fSSRDenoiseRadius > 0.0f && denoisedSurf && denoisedTex && h.techSSRDenoise)
        {
            effect->SetTexture(h.SSRResultTex2D, ssrTex);
            // The radius is in full size pixels, so the blur covers the same part of the screen.
            effect->SetFloat(h.fDenoiseRadius, half ? R.fSSRDenoiseRadius * 0.5f : R.fSSRDenoiseRadius);
            effect->SetFloat(h.fDenoiseSSROnly, 1.0f);
            pDevice->SetRenderTarget(0, denoisedSurf);
            effect->SetTechnique(h.techSSRDenoise);
            const HRESULT beginHr = effect->Begin(&passes, D3DXFX_DONOTSAVESTATE);
            const HRESULT passHr = effect->BeginPass(0);
            effect->CommitChanges();
            BindEffectSamplers(pDevice, effect);
            SSRTrace::State(pDevice, "ssr denoise");
            SSRTrace::Contents(pDevice, "denoise input", ssrTex);
            ProfilerMark(pDevice, kProfSSRDenoise, true);
            const HRESULT drawHr = pDevice->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, screenVertices, sizeof(ScreenVertex));
            ProfilerMark(pDevice, kProfSSRDenoise, false);
            effect->EndPass();
            effect->End();
            SSRTrace::Line("ssr denoise: begin %08x pass %08x draw %08x passes %u", unsigned(beginHr), unsigned(passHr), unsigned(drawHr), passes);
            SSRTrace::Contents(pDevice, "denoise output", denoisedTex);
            R.bSSRDenoised = true;
        }
        IDirect3DTexture9* ssrResult = R.bSSRDenoised ? denoisedTex : ssrTex;

        if (temporal)
        {
            const int sizeIndex = half ? 1 : 0;
            if (R.bSSRAccumHalf != half)
                R.nSSRAccumFrame = 0;
            R.bSSRAccumHalf = half;
            const int prev = R.nSSRAccumIndex, next = prev ^ 1;
            effect->SetTexture(h.SSRResultTex2D, ssrResult);
            effect->SetTexture(h.SSRAccumTex2D, R.SSRAccumTex[sizeIndex][prev]->mD3DTexture);
            effect->SetTexture(h.SSRHitDistTex2D, R.SSRHitDistTex[sizeIndex]->mD3DTexture);
            const bool history = FrameHistory::CanReproject(R.nSSRAccumFrame);
            BindMotionVectors(effect, history);
            effect->SetFloat(h.fTemporalBlend, history ? R.fSSRTemporalBlend : 0.0f);
            effect->SetFloat(h.fTemporalAnySurface, 0.0f);
            if (SSRTrace::Active())
            {
                SSRTrace::Line("ssr temporal: input %s, history %s of scene %u, reprojects %d, motion %p, into %s",
                    SSRTrace::TextureName(ssrResult).c_str(), SSRTrace::TextureName(R.SSRAccumTex[sizeIndex][prev]->mD3DTexture).c_str(),
                    unsigned(R.nSSRAccumFrame), int(history), static_cast<void*>(history ? FrameHistory::MotionVectors() : nullptr),
                    SSRTrace::TextureName(R.SSRAccumTex[sizeIndex][next]->mD3DTexture).c_str());
                SSRTrace::Contents(pDevice, "temporal input", ssrResult);
                SSRTrace::Contents(pDevice, "temporal history", R.SSRAccumTex[sizeIndex][prev]->mD3DTexture);
            }
            pDevice->SetRenderTarget(0, R.SSRAccumSurf[sizeIndex][next]);
            effect->SetTechnique(h.techSSRTemporal);
            effect->Begin(&passes, D3DXFX_DONOTSAVESTATE);
            effect->BeginPass(0);
            effect->CommitChanges();
            BindEffectSamplers(pDevice, effect);
            SSRTrace::State(pDevice, "ssr temporal");
            ProfilerMark(pDevice, kProfSSRTemporal, true);
            pDevice->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, screenVertices, sizeof(ScreenVertex));
            ProfilerMark(pDevice, kProfSSRTemporal, false);
            effect->EndPass();
            effect->End();
            ssrResult = R.SSRAccumTex[sizeIndex][next]->mD3DTexture;
            R.nSSRAccumIndex = next;
            R.nSSRAccumFrame = FrameHistory::Frame();
        }
        else
            R.nSSRAccumFrame = 0;
        R.SSRResult = ssrResult;
        if (SSRTrace::Active())
        {
            SSRTrace::Line("ssr: done, half %d denoised %d temporal %d result %s, history this frame %d",
                int(half), int(R.bSSRDenoised), int(temporal), SSRTrace::TextureName(ssrResult).c_str(), int(R.bSSRHistoryThisFrame));
            SSRTrace::Contents(pDevice, "trace", R.SSRTraceTex[half] ? R.SSRTraceTex[half]->mD3DTexture : nullptr);
            SSRTrace::Contents(pDevice, "resolve", ssrTex);
            SSRTrace::Contents(pDevice, "result", ssrResult);
            SSRTrace::readbacksLeft.fetch_sub(1);
        }

        const int debugMode = R.SSRDebugMode();
        if (debugMode && debugMode < R.kGlassDebugMode && R.SSRDebugSurf && h.techSSRDebug && hasNormals)
        {
            if (half)
                setPassSize(width, height);
            // View 3 reads where the rays hit straight from the march: smoothing and accumulation
            // would average the positions of neighbouring hits into places no ray went.
            effect->SetTexture(h.SSRResultTex2D, debugMode == 3 ? ssrTex : ssrResult);
            effect->SetFloat(h.fDebugMode, float(debugMode));

            pDevice->SetRenderTarget(0, R.SSRDebugSurf);
            effect->SetTechnique(h.techSSRDebug);
            effect->Begin(&passes, D3DXFX_DONOTSAVESTATE);
            effect->BeginPass(0);
            effect->CommitChanges();
            BindEffectSamplers(pDevice, effect);
            ProfilerScope timed(pDevice, kProfSSRDebug);
            pDevice->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, screenVertices, sizeof(ScreenVertex));
            effect->EndPass();
            effect->End();
            R.bSSRDebugValid = true;
        }

        // Camera for the car glass shaders, which draw after lighting.
        if (R.bGlassReflections)
        {
            const float params[20] =
            {
                proj._11, proj._22, proj._31, proj._32,
                proj._34, R.fGlassReflectionsThickness, R.fGlassReflectionsLength, 12345.0f,
                viewInv.m[0][0], viewInv.m[0][1], viewInv.m[0][2], debugMode == R.kGlassDebugMode ? 1.0f : 0.0f,
                viewInv.m[1][0], viewInv.m[1][1], viewInv.m[1][2], height * 0.5f,
                vp->mNearClip, log2f(vp->mFarClip / vp->mNearClip), R.bGlassStepJitter ? 1.0f : 0.0f, R.fSSRTowardCamera,
            };
            memcpy(R.GlassParams, params, sizeof(params));
            R.bGlassFrameValid = true;
        }
        {

            for (size_t i = 0; i < std::size(kSSRRenderStates); ++i)
                pDevice->SetRenderState(kSSRRenderStates[i].state, savedRenderStates[i]);

            for (DWORD slot = 0; slot < kSSRSamplerSlots; ++slot)
                for (size_t i = 0; i < std::size(kSSRSamplerStates); ++i)
                    pDevice->SetSamplerState(slot, kSSRSamplerStates[i].state, savedSamplerStates[slot][i]);

            pDevice->SetPixelShaderConstantF(0, savedPSConsts, kPSConstCount);
            pDevice->SetVertexShaderConstantF(0, savedVSConsts, kVSConstCount);

            for (DWORD slot = 0; slot < kSSRTextureSlots; ++slot)
            {
                SetTextureBoth(pDevice, slot, oldTextures[slot]);
                SAFE_RELEASE(oldTextures[slot]);
            }
        }

        R.bSSRValidThisFrame = true;

        pDevice->SetRenderTarget(0, rt0);
        pDevice->SetDepthStencilSurface(ds);
        pDevice->SetViewport(&oldViewport);
        pDevice->SetFVF(oldFVF);
        pDevice->SetVertexDeclaration(oldDecl);
        pDevice->SetStreamSource(0, oldVB, oldOffset, oldStride);

        SAFE_RELEASE(rt0);
        SAFE_RELEASE(ds);
        SAFE_RELEASE(oldDecl);
        SAFE_RELEASE(oldVB);
    }

    static constexpr struct { D3DRENDERSTATETYPE state; DWORD value; } kWaterSSRRenderStates[] =
    {
        { D3DRS_ZENABLE,          FALSE },
        { D3DRS_ZWRITEENABLE,     FALSE },
        { D3DRS_ALPHATESTENABLE,  FALSE },
        { D3DRS_STENCILENABLE,    FALSE },
        { D3DRS_FOGENABLE,        FALSE },
        { D3DRS_CLIPPING,         FALSE },
        { D3DRS_CULLMODE,         D3DCULL_NONE },
        { D3DRS_ALPHABLENDENABLE, TRUE },
        { D3DRS_SEPARATEALPHABLENDENABLE, FALSE },
        { D3DRS_BLENDOP,          D3DBLENDOP_ADD },
        { D3DRS_SRCBLEND,         D3DBLEND_SRCALPHA },
        { D3DRS_DESTBLEND,        D3DBLEND_ONE },
        // Additive and RGB only, so a miss adds nothing and the scene alpha is untouched.
        { D3DRS_COLORWRITEENABLE, D3DCOLORWRITEENABLE_RED | D3DCOLORWRITEENABLE_GREEN | D3DCOLORWRITEENABLE_BLUE },
    };

    static constexpr struct { D3DRENDERSTATETYPE state; DWORD value; } kCloudRenderStates[] =
    {
        { D3DRS_ZENABLE,          FALSE },
        { D3DRS_ZWRITEENABLE,     FALSE },
        // For the reflections, which test against the sky's depth (far) instead.
        { D3DRS_ZFUNC,            D3DCMP_LESSEQUAL },
        { D3DRS_ALPHATESTENABLE,  FALSE },
        { D3DRS_STENCILENABLE,    FALSE },
        { D3DRS_FOGENABLE,        FALSE },
        { D3DRS_CLIPPING,         FALSE },
        { D3DRS_SCISSORTESTENABLE, FALSE },
        { D3DRS_CULLMODE,         D3DCULL_NONE },
        { D3DRS_SRGBWRITEENABLE,  FALSE },
        { D3DRS_ALPHABLENDENABLE, TRUE },
        { D3DRS_SEPARATEALPHABLENDENABLE, FALSE },
        { D3DRS_BLENDOP,          D3DBLENDOP_ADD },
        // The cloud's light plus what shows through it; RGB only, the scene alpha stays.
        { D3DRS_SRCBLEND,         D3DBLEND_ONE },
        { D3DRS_DESTBLEND,        D3DBLEND_SRCALPHA },
        { D3DRS_COLORWRITEENABLE, D3DCOLORWRITEENABLE_RED | D3DCOLORWRITEENABLE_GREEN | D3DCOLORWRITEENABLE_BLUE },
        // The march's second target.
        { D3DRS_COLORWRITEENABLE1, 0xF },
    };

    // Blends the volumetric clouds (Clouds.fx) into the lit scene, from the fog pass before it reads
    // the scene; with reflection, into the reflection map's target right after its sky
    // (DrawSkyReflection), at full size, on the sky only by the depth test, at the brightness of
    // that simpler sky. Leaves the device as it found it.
    static void RenderVolumetricClouds(IDirect3DDevice9* pDevice, IDirect3DBaseTexture9* sceneBase, bool reflection = false,
                                       int reflectionSection = kProfCloudReflectionMap)
    {
        ProfilerScope timedAll(pDevice, reflection ? kProfCloudReflection : -1);
        ProfilerScope timed(pDevice, reflection ? reflectionSection : kProfClouds);
        auto& R = PostFxResources;
        auto skip = [&](const char* why) { (reflection ? R.szCloudsReflectionStatus : R.szCloudsStatus) = why; };
        if (!R.VolumetricCloudsEnabled())
            return skip("off in the ini");
        if (!R.CloudsEffect)
            return skip("no effect");
        if (!R.bCloudParamsRegistered)
            return skip("cloud parameters not registered");
        if (!sceneBase && !reflection)
            return skip("no scene texture");
        if (reflection && !R.bVolumetricCloudsReflections)
            return skip("VolumetricCloudsReflections 0");
        if (!R.mDepthRT || !R.mDepthRT->mD3DTexture)
            return skip("no depth texture");
        rage::grcViewport* vp = rage::GetCurrentViewport();
        if (!vp)
            return skip("no viewport");

        // The game's clouds: until the sky has been drawn once these read zero.
        // gta_atmoscatt_clouds colours its clouds CloudColor, brightened by CloudInscatteringRange
        // towards the sun, less the cloud's own shadow, plus SunsetColor where the sun lights them.
        // TopCloudColor belongs to its separate high layer and tinted these clouds cyan.
        const auto& cloudColour = rage::grmShaderInfo::getShaderParamData(R.CloudColorIdx);
        const auto& sunsetColour = rage::grmShaderInfo::getShaderParamData(R.SunsetColorIdx);
        const float inscattering = rage::grmShaderInfo::getShaderParamData(R.CloudInscatteringIdx)[0];
        const auto& sunDirection = rage::grmShaderInfo::getShaderParamData(R.CloudSunDirectionIdx);
        float exposure = rage::grmShaderInfo::getShaderParamData(R.CloudExposureIdx)[0] * R.fVolumetricCloudsBrightness;
        if (exposure <= 0.0f)
            return skip("HDRExposure of the sky reads zero");
        // The reflection map's sky (the sky's 0x40000 branch) takes the timecycle's colours without the
        // HDR exposure. The water's reflection draws its sky through the main branch, exposed as the
        // scene's: with the reflection map's exposure its clouds came out some 30 times too dark, black.
        if (reflection && reflectionSection != kProfCloudReflectionWater)
            exposure = R.fVolumetricCloudsBrightness * R.fVolumetricCloudsReflectionBrightness;
        else if (reflection)
            exposure *= R.fVolumetricCloudsReflectionBrightness;
        // The sky's SunDirection is y up; the world is z up.
        D3DXVECTOR4 sun(sunDirection[0], -sunDirection[2], sunDirection[1], 0.0f);
        const float sunLength = std::sqrt(sun.x * sun.x + sun.y * sun.y + sun.z * sun.z);
        if (sunLength <= 0.0f)
            return skip("SunDirection of the sky reads zero");
        sun /= sunLength;
        // Below the horizon the sun hands the clouds over to the moon. The sun still lights their
        // bellies a little way below it, at dusk; then its light fades out, and the moon's fades in
        // from the same zero, so the light never jumps from one to the other at full strength.
        // MoonPosition is the direction to the moon, y up like SunDirection.
        float lightStrength = 1.0f;
        bool moonlit = false;
        {
            auto smoothstep = [](float a, float b, float x) { const float t = std::clamp((x - a) / (b - a), 0.0f, 1.0f); return t * t * (3.0f - 2.0f * t); };
            const float day = smoothstep(-0.15f, -0.05f, sun.z);
            const auto& moonPosition = rage::grmShaderInfo::getShaderParamData(R.CloudMoonPositionIdx);
            D3DXVECTOR4 moon(moonPosition[0], -moonPosition[2], moonPosition[1], 0.0f);
            const float moonLength = std::sqrt(moon.x * moon.x + moon.y * moon.y + moon.z * moon.z);
            moon.x *= R.CloudSkyAxisSign[0];
            moon.y *= R.CloudSkyAxisSign[1];
            if (day >= 0.5f || moonLength <= 0.0f)
                lightStrength = day >= 0.5f ? day * 2.0f - 1.0f : 0.0f;
            else
            {
                moon /= moonLength;
                sun = moon;
                moonlit = true;
                lightStrength = (1.0f - day * 2.0f) * smoothstep(0.0f, 0.1f, moon.z) * R.fVolumetricCloudsMoonlight;
            }
        }

        auto coverage = R.CloudNoiseTex();
        auto detail = R.CloudDetailTex();
        if (!coverage)
            return skip("no coverage texture");
        if (!detail)
            return skip("no detail texture");

        IDirect3DSurface9* sceneSurface = nullptr;
        if (reflection)
            pDevice->GetRenderTarget(0, &sceneSurface);
        else
        {
            IDirect3DTexture9* scene = nullptr;
            if (FAILED(sceneBase->QueryInterface(__uuidof(IDirect3DTexture9), reinterpret_cast<void**>(&scene))) || !scene)
                return skip("the scene is no 2D texture");
            scene->GetSurfaceLevel(0, &sceneSurface);
            scene->Release();
        }
        if (!sceneSurface)
            return skip("no scene surface");
        D3DSURFACE_DESC desc = {};
        sceneSurface->GetDesc(&desc);
        float width = float(desc.Width), height = float(desc.Height);
        // A reflection is drawn through its own viewport, which may be a part of its target (a map
        // drawn in parts, as two halves of one texture): the clouds go into that part only, the
        // rays from its own corner. Insisting on the whole target left the water with no clouds.
        float originX = 0.0f, originY = 0.0f;
        if (reflection)
        {
            D3DVIEWPORT9 current = {};
            pDevice->GetViewport(&current);
            R.CloudReflectionViewport = current;
            R.CloudReflectionTarget[0] = desc.Width;
            R.CloudReflectionTarget[1] = desc.Height;
            if (current.Width == 0 || current.Height == 0 || current.X + current.Width > desc.Width || current.Y + current.Height > desc.Height)
            {
                sceneSurface->Release();
                return skip("viewport outside its target");
            }
            originX = float(current.X);
            originY = float(current.Y);
            width = float(current.Width);
            height = float(current.Height);
        }
        skip("drawn");

        ID3DXEffect* effect = R.CloudsEffect;
        const D3DMATRIX& proj = *(const D3DMATRIX*)vp->mProjectionMatrix;
        const D3DXVECTOR4 projInfo = ProjInfo(proj, width, height);
        const float invViewportSize[] = { 1.0f / width, 1.0f / height };
        effect->SetFloatArray("vec2InvViewportSize", invViewportSize, 2);
        effect->SetVector("vec4ProjInfo", &projInfo);
        effect->SetFloat("fNearPlane", vp->mNearClip);
        effect->SetFloat("fFarDivNear", vp->mFarClip / vp->mNearClip);

        const D3DXMATRIX& viewInv = *(const D3DXMATRIX*)vp->mViewInverseMatrix;
        D3DXVECTOR4 toView[3];
        WorldToViewRows(vp, toView);
        const D3DXVECTOR4 worldX(toView[0].x, toView[1].x, toView[2].x, viewInv.m[3][0]);
        const D3DXVECTOR4 worldY(toView[0].y, toView[1].y, toView[2].y, viewInv.m[3][1]);
        const D3DXVECTOR4 worldZ(toView[0].z, toView[1].z, toView[2].z, viewInv.m[3][2]);
        effect->SetVector("vec4WorldX", &worldX);
        effect->SetVector("vec4WorldY", &worldY);
        effect->SetVector("vec4WorldZ", &worldZ);

        if (!reflection)
            std::memcpy(R.CloudLastSkySun, &sun.x, sizeof(R.CloudLastSkySun));
        // By day the game's own directional light, taken in the lighting phase of this frame or the
        // last: the sky's SunDirection, remapped from its y up space, lit the clouds from the
        // mirror image of the sun across the sky, their far sides lit and the near ones dark.
        if (!moonlit && R.nCloudLightFrame && FrameHistory::Frame() - R.nCloudLightFrame <= 2 && R.CloudLightDir[2] > 0.0f)
        {
            // Learn the sky's axis signs while both directions are clear of the zenith.
            for (int axis = 0; axis < 2; ++axis)
            {
                const float skyAxis = (&sun.x)[axis], gameAxis = R.CloudLightDir[axis];
                if (std::fabs(skyAxis) > 0.2f && std::fabs(gameAxis) > 0.2f)
                    R.CloudSkyAxisSign[axis] = (skyAxis > 0.0f) == (gameAxis > 0.0f) ? 1.0f : -1.0f;
            }
            sun = D3DXVECTOR4(R.CloudLightDir[0], R.CloudLightDir[1], R.CloudLightDir[2], 0.0f);
        }
        if (!reflection)
            std::memcpy(R.CloudLastUsedSun, &sun.x, sizeof(R.CloudLastUsedSun));
        // VolumetricCloudsDebug 15: the sun straight overhead, to test the sun's direction.
        if (R.nVolumetricCloudsDebug == 15)
            sun = D3DXVECTOR4(0.0f, 0.0f, 1.0f, 0.0f);
        effect->SetFloatArray("vec3SunDir", &sun.x, 3);
        float litColour[3], shadeColour[3], sunsetLit[3];
        for (int i = 0; i < 3; ++i)
        {
            litColour[i] = cloudColour[i] * R.kCloudLitGain * exposure;
            shadeColour[i] = cloudColour[i] * R.fVolumetricCloudsShade * exposure;
            sunsetLit[i] = sunsetColour[i] * exposure;
        }
        // Matched to the sky in the scene behind them, while the march has targets of its own to draw
        // into and can read the scene; the reflections keep the game's cloud colour.
        const bool canReadScene = !reflection && R.CloudSurf[0] && R.CloudSurf[1] && R.CloudSurf[2] && R.CloudSkyRefSurf &&
                                  R.CloudMarchSurf[0] && R.CloudMarchSurf[1];
        // The shaded side is lit by the sky above it rather than the sun: it takes the sky's hue at its
        // own brightness, by VolumetricCloudsSkyLight, and the sunlit side some of it too. The hue of
        // the sky on screen (CloudsSkyRef, in Clouds.fx), the reflections that of the frame before:
        // the game's SkyColor stays bluish under a pink evening sky, and the clouds came out white
        // against it. SkyColor's hue only while there is no sky colour of a recent frame.
        constexpr float kCloudLitSkyHue = 0.4f;
        const bool skyRefRecent = R.CloudSkyRefTex && R.nCloudSkyRefFrame && FrameHistory::Frame() - R.nCloudSkyRefFrame <= 2;
        {
            const float skyHue[3] = { skyRefRecent ? R.fVolumetricCloudsSkyLight : 0.0f, skyRefRecent ? kCloudLitSkyHue : 0.0f,
                                      R.fVolumetricCloudsSaturation };
            effect->SetFloatArray("vec3SkyHue", skyHue, 3);
        }
        if (!skyRefRecent)
        {
            const auto& skyColour = rage::grmShaderInfo::getShaderParamData(R.CloudSkyColorIdx);
            const float skyLuma = 0.2126f * skyColour[0] + 0.7152f * skyColour[1] + 0.0722f * skyColour[2];
            const float shadeLuma = 0.2126f * shadeColour[0] + 0.7152f * shadeColour[1] + 0.0722f * shadeColour[2];
            if (skyLuma > 1e-4f)
                for (int i = 0; i < 3; ++i)
                    shadeColour[i] += ((std::max)(skyColour[i], 0.0f) / skyLuma * shadeLuma - shadeColour[i]) * R.fVolumetricCloudsSkyLight;
        }
        if (!skyRefRecent)
        {
            const auto& skyColour = rage::grmShaderInfo::getShaderParamData(R.CloudSkyColorIdx);
            const float skyLuma = 0.2126f * skyColour[0] + 0.7152f * skyColour[1] + 0.0722f * skyColour[2];
            const float litLuma = 0.2126f * litColour[0] + 0.7152f * litColour[1] + 0.0722f * litColour[2];
            if (skyLuma > 1e-4f)
                for (int i = 0; i < 3; ++i)
                    litColour[i] += ((std::max)(skyColour[i], 0.0f) / skyLuma * litLuma - litColour[i]) * kCloudLitSkyHue;
        }
        // VolumetricCloudsSaturation, about each colour's luma.
        for (float* colour : { litColour, shadeColour, sunsetLit })
        {
            const float luma = 0.2126f * colour[0] + 0.7152f * colour[1] + 0.0722f * colour[2];
            for (int i = 0; i < 3; ++i)
                colour[i] = (std::max)(luma + (colour[i] - luma) * R.fVolumetricCloudsSaturation, 0.0f);
        }
        // The ceiling the brightest cloud rolls off towards: room above the silver lining's peak for
        // the glow next to the sun. gta_atmoscatt_clouds clamps the sky and its clouds to
        // HDRExposureClamp unless FusionFix's volumetric fog or SkyHDR is on, and past that clamp our
        // clouds turned white while the sky around them stayed at it, so then the ceiling is the
        // clamp.
        float ceiling = 0.0f;
        {
            static auto volumetricFog = FusionFixSettings.GetRef("PREF_VOLUMETRICFOG");
            const auto& clamp = rage::grmShaderInfo::getShaderParamData(R.CloudExposureClampIdx);
            const float brightest = (std::max)({ litColour[0], litColour[1], litColour[2] });
            // Matched to the sky the lit side is VolumetricCloudsSkyMatch times its brightness, and the
            // rims and the glow next to the sun roll off below three times that.
            ceiling = brightest * (R.fVolumetricCloudsSkyMatch > 0.0f ? 3.0f : (1.0f + inscattering) * 1.5f);
            const float clampMin = (std::min)({ clamp[0], clamp[1], clamp[2] });
            if (!reflection && !(volumetricFog && volumetricFog->get()) && !bSkyHDR && clampMin > 0.0f)
                ceiling = (std::min)(ceiling, clampMin);
            ceiling = (std::max)(ceiling, 1e-3f);
            if (!reflection)
            {
                std::memcpy(R.CloudLastLit, litColour, sizeof(litColour));
                std::memcpy(R.CloudLastShade, shadeColour, sizeof(shadeColour));
                std::memcpy(R.CloudLastClamp, clamp.data(), sizeof(R.CloudLastClamp));
                R.CloudLastCeiling = ceiling;
                R.CloudLastLightStrength = lightStrength;
                R.bCloudLastMoonlit = moonlit;
            }
        }
        effect->SetFloatArray("vec3LitColour", litColour, 3);
        effect->SetFloatArray("vec3ShadeColour", shadeColour, 3);
        effect->SetFloatArray("vec3SunsetColour", sunsetLit, 3);
        effect->SetFloat("fSilver", inscattering);
        effect->SetFloat("fCeiling", ceiling);
        {
            const float litLuma = 0.2126f * litColour[0] + 0.7152f * litColour[1] + 0.0722f * litColour[2];
            effect->SetFloat("fSkyMatch", canReadScene && litLuma > 1e-4f ? R.fVolumetricCloudsSkyMatch * R.Cloud.skyMatch / litLuma : 0.0f);
        }
        effect->SetFloat("fLightStrength", lightStrength);
        // How high the sun (or the moon) stands: 0 up to 5 degrees, 1 from 35 up. With it low, the clouds
        // towards it are lit from behind: their bodies darker than the sky, down to 0.3 of their light
        // straight towards it, and the glow of their edges three times as strong. Through the day
        // nothing changes; nor across the sky from a low sun, where the clouds face its light. The
        // least of the light inside them stays: lowered with the sun, it darkened the sides the sun
        // lights too, grey where they should take its colour.
        effect->SetFloat("fMinLight", R.fVolumetricCloudsMinLight);
        {
            const float lowSun = std::clamp((sun.z - 0.0872f) / (0.5736f - 0.0872f), 0.0f, 1.0f);
            const float day = lowSun * lowSun * (3.0f - 2.0f * lowSun);
            effect->SetFloat("fBacklight", 0.3f + 0.7f * day);
            effect->SetFloat("fGlowBoost", 1.0f + 2.0f * (1.0f - day));
        }
        effect->SetFloat("fMottle", R.fVolumetricCloudsMottle);
        // The sun's hue at brightness 1 (Rec. 709 luma), each channel kept within 0 to 2, mixed
        // towards white by VolumetricCloudsSunTint. The moon's is a cool white.
        {
            static const float moonColour[3] = { 0.85f, 0.95f, 1.2f };
            const float* sunColour = moonlit ? moonColour : rage::grmShaderInfo::getShaderParamData(R.CloudSunColorIdx).data();
            const float luma = 0.2126f * sunColour[0] + 0.7152f * sunColour[1] + 0.0722f * sunColour[2];
            float tint[3] = { 1.0f, 1.0f, 1.0f };
            if (luma > 1e-4f)
                for (int i = 0; i < 3; ++i)
                    tint[i] = 1.0f + (std::clamp(sunColour[i] / luma, 0.0f, 2.0f) - 1.0f) * R.fVolumetricCloudsSunTint;
            effect->SetFloatArray("vec3SunTint", tint, 3);
            // The glow is the sunlight straight through the cloud's edges: it takes the sun's hue
            // whole, gold in the evening, whatever VolumetricCloudsSunTint does to the lit sides.
            float glow[3];
            for (int i = 0; i < 3; ++i)
                glow[i] = litColour[i] * (luma > 1e-4f ? std::clamp(sunColour[i] / luma, 0.0f, 2.0f) : 1.0f);
            effect->SetFloatArray("vec3GlowColour", glow, 3);
        }
        const D3DXVECTOR4 layer(R.Cloud.base, R.Cloud.thickness, 1.0f / R.fCloudShadowsScale, R.Cloud.coverage);
        effect->SetVector("vec4Layer", &layer);
        // The detail drifts with the wind too, half as fast, so the billows change as they go.
        const D3DXVECTOR4 wind(R.fCloudWindX, R.fCloudWindY, float(std::fmod(R.fCloudDetailDrift * 0.93, 1.0)),
                               float(std::fmod(R.fCloudDetailDrift * 0.37, 1.0)));
        effect->SetVector("vec4Wind", &wind);
        const D3DXVECTOR4 shape(R.fVolumetricCloudsDensity * R.Cloud.density, 1.0f / R.fVolumetricCloudsDetailScale,
                                (std::min)(R.fVolumetricCloudsDetail * R.Cloud.detail, 1.5f),
                                R.fVolumetricCloudsHaze * (1.0f - 0.6f * R.Cloud.stratus));
        effect->SetVector("vec4Shape", &shape);
        effect->SetFloat("fStratus", R.Cloud.stratus);
        // The density's soft compressor, d (1 + k) / (1 + k d), its k from the base to the top: 3 to 12,
        // or 1 to 3 where the weather wants smoky edges.
        {
            const float compress[2] = { 3.0f - 2.0f * R.Cloud.softness, 12.0f - 9.0f * R.Cloud.softness };
            effect->SetFloatArray("vec2Compress", compress, 2);
        }
        effect->SetFloat("fEvolution", float(std::fmod(R.fCloudEvolution, 1.0)));
        effect->SetFloat("fTranslucency", (std::min)(R.fVolumetricCloudsTranslucency * R.Cloud.translucency, 0.9f));
        effect->SetFloat("fLightAbsorption", R.fVolumetricCloudsAbsorption * R.Cloud.absorption);
        effect->SetFloat("fGlow", R.Cloud.glow);
        effect->SetFloat("fSunPower", R.Cloud.sunPower);
        effect->SetFloat("fBaseRound", R.Cloud.baseRound);
        // How far the billows are swept along the coarse noise at the tops, in their own size: more
        // in the wind. Up to 3 in a strong wind they drew out into parallel brush strokes.
        effect->SetFloat("fCurl", std::clamp(0.8f + 0.4f * R.Cloud.wind, 1.0f, 1.8f));
        // The wind's shear: a heap's top lies up to 120 m downwind of its base at the weather's
        // wind, at most 200 m; at 200 m, up to 300, the tops trailed off in streaks. The wind carries the map along (0.93, 0.37), so the clouds drift the
        // other way and their tops lean that way.
        {
            const float lean = (std::min)(120.0f * R.Cloud.wind, 200.0f) / R.fCloudShadowsScale;
            const float shear[2] = { 0.9293f * lean, 0.3697f * lean };
            effect->SetFloatArray("vec2Shear", shear, 2);
        }
        effect->SetFloat("fDebug", float(R.nVolumetricCloudsDebug));
        // The golden ratio's fraction per frame: each frame's march noise falls between the last
        // ones', and temporal anti-aliasing averages it away.
        effect->SetFloat("fFrameJitter", static_cast<float>(std::fmod(FrameHistory::Frame() * 0.6180339887, 1.0)));
        // The outline wanders by up to 150 metres.
        effect->SetFloat("fWarp", 150.0f / R.fCloudShadowsScale);
        const D3DXVECTOR4 morph(R.CloudMorphPhase(), R.kCloudMorphReach / R.fCloudShadowsScale, R.kCloudWeatherScale, R.kCloudWeatherReach);
        effect->SetVector("vec4Morph", &morph);
        effect->SetFloat("fMaxDistance", R.fVolumetricCloudsMaxDistance);

        IDirect3DSurface9* oldTarget = nullptr;
        IDirect3DSurface9* oldTarget1 = nullptr;
        IDirect3DSurface9* oldDepth = nullptr;
        IDirect3DVertexDeclaration9* oldDecl = nullptr;
        IDirect3DVertexBuffer9* oldVB = nullptr;
        IDirect3DPixelShader9* oldPS = nullptr;
        IDirect3DVertexShader9* oldVS = nullptr;
        IDirect3DBaseTexture9* oldTextures[10] = {};
        UINT oldOffset = 0, oldStride = 0;
        DWORD oldFVF = 0;
        D3DVIEWPORT9 oldViewport = {};
        DWORD savedRenderStates[std::size(kCloudRenderStates)] = {};
        static constexpr D3DSAMPLERSTATETYPE kSamplerStates[] = { D3DSAMP_ADDRESSU, D3DSAMP_ADDRESSV, D3DSAMP_ADDRESSW,
                                                                   D3DSAMP_MAGFILTER, D3DSAMP_MINFILTER, D3DSAMP_MIPFILTER };
        DWORD savedSamplerStates[10][std::size(kSamplerStates)] = {};

        pDevice->GetRenderTarget(0, &oldTarget);
        pDevice->GetRenderTarget(1, &oldTarget1);
        pDevice->GetDepthStencilSurface(&oldDepth);
        pDevice->GetViewport(&oldViewport);
        pDevice->GetFVF(&oldFVF);
        pDevice->GetVertexDeclaration(&oldDecl);
        pDevice->GetStreamSource(0, &oldVB, &oldOffset, &oldStride);
        pDevice->GetPixelShader(&oldPS);
        pDevice->GetVertexShader(&oldVS);
        pDevice->GetPixelShaderConstantF(0, savedPSConsts, kPSConstCount);
        pDevice->GetVertexShaderConstantF(0, savedVSConsts, kVSConstCount);
        for (DWORD slot = 0; slot < 10; ++slot)
        {
            pDevice->GetTexture(slot, &oldTextures[slot]);
            for (size_t i = 0; i < std::size(kSamplerStates); ++i)
                pDevice->GetSamplerState(slot, kSamplerStates[i], &savedSamplerStates[slot][i]);
        }
        for (size_t i = 0; i < std::size(kCloudRenderStates); ++i)
        {
            pDevice->GetRenderState(kCloudRenderStates[i].state, &savedRenderStates[i]);
            pDevice->SetRenderState(kCloudRenderStates[i].state, kCloudRenderStates[i].value);
        }

        // The reflections keep their depth buffer: the quad at the far plane passes the depth test
        // only where nothing but sky was drawn, whether before or after the sky.
        if (reflection)
            pDevice->SetRenderState(D3DRS_ZENABLE, D3DZB_TRUE);
        else
            pDevice->SetDepthStencilSurface(nullptr);
        pDevice->SetStreamSource(0, nullptr, 0, 0);
        pDevice->SetVertexDeclaration(nullptr);
        pDevice->SetFVF(D3DFVF_XYZRHW | D3DFVF_TEX1);
        const float quadZ = reflection ? 1.0f : 0.0f;

        // Half size, accumulated, laid over the scene at full size; one full size pass straight into
        // the scene while the half size targets are missing, and for the reflections.
        const bool halfSize = !reflection && R.CloudSurf[0] && R.CloudSurf[1] && R.CloudSurf[2] && R.CloudMarchSurf[0] && R.CloudMarchSurf[1];
        const int prevAccum = 1 + R.nCloudAccumIndex;
        const int nextAccum = 1 + (R.nCloudAccumIndex ^ 1);
        D3DSURFACE_DESC halfDesc = {};
        if (halfSize)
            R.CloudSurf[0]->GetDesc(&halfDesc);
        const float halfWidth = halfSize ? float(halfDesc.Width) : width, halfHeight = halfSize ? float(halfDesc.Height) : height;
        const float depthTexel[2] = { 1.0f / width, 1.0f / height };
        effect->SetFloatArray("vec2DepthTexel", depthTexel, 2);

        // Last frame's view projection by columns: a direction (w 0) leaves its translation out.
        const bool history = halfSize && FrameHistory::CanReproject(R.nCloudAccumFrame);
        {
            const D3DXMATRIX& m = FrameHistory::Previous().ViewProjectionNoJitter;
            const D3DXVECTOR4 prevX(m._11, m._21, m._31, 0.0f), prevY(m._12, m._22, m._32, 0.0f), prevW(m._14, m._24, m._34, 0.0f);
            effect->SetVector("vec4PrevX", &prevX);
            effect->SetVector("vec4PrevY", &prevY);
            effect->SetVector("vec4PrevW", &prevW);
            // A sixth of each frame: with the march's offset moving on every frame, its noise
            // averages out over about a dozen frames.
            const D3DXVECTOR4 historyInfo(history ? 1.0f : 0.0f, 0.15f, 1.0f / halfWidth, 1.0f / halfHeight);
            effect->SetVector("vec4History", &historyInfo);
        }

        struct ScreenVertex { float x, y, z, rhw; float u, v; };
        auto bindTextures = [&](bool readScene, bool readSkyRef, bool readMarch)
        {
            // The samplers have fixed registers: s0 depth, s1 coverage, s2 detail, s3 this frame's
            // half size clouds, s4 the history, s5 the accumulated clouds, s6 the scene, for the sky's
            // brightness, never while drawing into it, s7 that brightness, s8 and s9 the march's sums.
            // The reflections have no depth texture of their own: none reads as sky everywhere.
            SetTextureBoth(pDevice, 0, reflection ? nullptr : R.mDepthRT->mD3DTexture);
            SetTextureBoth(pDevice, 1, coverage);
            SetTextureBoth(pDevice, 2, detail);
            SetTextureBoth(pDevice, 3, halfSize && !readMarch ? R.CloudTex[0]->mD3DTexture : nullptr);
            SetTextureBoth(pDevice, 4, halfSize ? R.CloudTex[prevAccum]->mD3DTexture : nullptr);
            SetTextureBoth(pDevice, 5, halfSize ? R.CloudTex[nextAccum]->mD3DTexture : nullptr);
            SetTextureBoth(pDevice, 6, readScene ? sceneBase : nullptr);
            SetTextureBoth(pDevice, 7, readSkyRef && R.CloudSkyRefTex ? R.CloudSkyRefTex->mD3DTexture : nullptr);
            SetTextureBoth(pDevice, 8, readMarch ? R.CloudMarchTex[0]->mD3DTexture : nullptr);
            SetTextureBoth(pDevice, 9, readMarch ? R.CloudMarchTex[1]->mD3DTexture : nullptr);
            for (DWORD slot = 0; slot < 10; ++slot)
            {
                const bool wrap = slot == 1 || slot == 2;
                const bool linear = slot != 0 && slot != 3 && slot != 8 && slot != 9;
                SetSamplerStateBoth(pDevice, slot, D3DSAMP_ADDRESSU, wrap ? D3DTADDRESS_WRAP : D3DTADDRESS_CLAMP);
                SetSamplerStateBoth(pDevice, slot, D3DSAMP_ADDRESSV, wrap ? D3DTADDRESS_WRAP : D3DTADDRESS_CLAMP);
                SetSamplerStateBoth(pDevice, slot, D3DSAMP_ADDRESSW, wrap ? D3DTADDRESS_WRAP : D3DTADDRESS_CLAMP);
                SetSamplerStateBoth(pDevice, slot, D3DSAMP_MAGFILTER, linear ? D3DTEXF_LINEAR : D3DTEXF_POINT);
                SetSamplerStateBoth(pDevice, slot, D3DSAMP_MINFILTER, linear ? D3DTEXF_LINEAR : D3DTEXF_POINT);
                SetSamplerStateBoth(pDevice, slot, D3DSAMP_MIPFILTER, slot == 1 ? D3DTEXF_LINEAR : D3DTEXF_NONE);
            }
        };
        // The passes are timed apart only in the scene's clouds, not in the reflection map's.
        auto drawPass = [&](const char* technique, IDirect3DSurface9* target, float w, float h, bool blend, int profile)
        {
            // Only the reflection's own viewport starts away from the target's corner.
            const float x0 = target == sceneSurface ? originX : 0.0f, y0 = target == sceneSurface ? originY : 0.0f;
            pDevice->SetRenderTarget(0, target);
            D3DVIEWPORT9 viewport = { DWORD(x0), DWORD(y0), DWORD(w), DWORD(h), 0.0f, 1.0f };
            pDevice->SetViewport(&viewport);
            pDevice->SetRenderState(D3DRS_ALPHABLENDENABLE, blend ? TRUE : FALSE);
            pDevice->SetRenderState(D3DRS_COLORWRITEENABLE, blend ? (D3DCOLORWRITEENABLE_RED | D3DCOLORWRITEENABLE_GREEN | D3DCOLORWRITEENABLE_BLUE) : 0xF);
            // VPOS counts from the target's corner: the rays start from the viewport's.
            D3DXVECTOR4 info = ProjInfo(proj, w, h);
            info.z -= x0 * info.x;
            info.w -= y0 * info.y;
            effect->SetVector("vec4ProjInfo", &info);
            const ScreenVertex screenVertices[4] =
            {
                { x0 - 0.5f,      y0 - 0.5f,     quadZ, 1.0f, 0.0f, 0.0f },
                { x0 - 0.5f,      y0 + h - 0.5f, quadZ, 1.0f, 0.0f, 1.0f },
                { x0 + w - 0.5f,  y0 - 0.5f,     quadZ, 1.0f, 1.0f, 0.0f },
                { x0 + w - 0.5f,  y0 + h - 0.5f, quadZ, 1.0f, 1.0f, 1.0f }
            };
            UINT passes = 0;
            effect->SetTechnique(technique);
            effect->Begin(&passes, D3DXFX_DONOTSAVESTATE);
            effect->BeginPass(0);
            effect->CommitChanges();
            // Never the scene while drawing into it.
            bindTextures(halfSize && target != sceneSurface, target != R.CloudSkyRefSurf, halfSize && target == R.CloudSurf[0]);
            BindEffectConstantsOnly(pDevice, effect);
            ProfilerScope timedPass(pDevice, reflection ? -1 : profile);
            pDevice->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, screenVertices, sizeof(ScreenVertex));
            effect->EndPass();
            effect->End();
        };

        if (halfSize)
        {
            // The sky's brightness first, blended into its 1 x 1 target; the march reads it.
            if (R.CloudSkyRefSurf)
            {
                const D3DXVECTOR4 skyRefProj = ProjInfo(proj, 1.0f, 1.0f);
                effect->SetVector("vec4SkyRefProj", &skyRefProj);
                drawPass("CloudsSkyRef", R.CloudSkyRefSurf, 1.0f, 1.0f, true, kProfCloudsSkyRef);
                R.nCloudSkyRefFrame = FrameHistory::Frame();
            }
            // The march into its two targets, then its light into the half size clouds.
            pDevice->SetRenderTarget(1, R.CloudMarchSurf[1]);
            drawPass("CloudsMarch", R.CloudMarchSurf[0], halfWidth, halfHeight, false, kProfCloudsMarch);
            pDevice->SetRenderTarget(1, nullptr);
            drawPass("CloudsLight", R.CloudSurf[0], halfWidth, halfHeight, false, kProfCloudsLight);
            drawPass("CloudsResolve", R.CloudSurf[nextAccum], halfWidth, halfHeight, false, kProfCloudsResolve);
            drawPass("CloudsComposite", sceneSurface, width, height, true, kProfCloudsComposite);
            R.nCloudAccumIndex ^= 1;
            R.nCloudAccumFrame = FrameHistory::Frame();
        }
        else
            drawPass("Clouds", sceneSurface, width, height, true, kProfCloudsMarch);

        for (size_t i = 0; i < std::size(kCloudRenderStates); ++i)
            pDevice->SetRenderState(kCloudRenderStates[i].state, savedRenderStates[i]);
        for (DWORD slot = 0; slot < 10; ++slot)
        {
            SetTextureBoth(pDevice, slot, oldTextures[slot]);
            for (size_t i = 0; i < std::size(kSamplerStates); ++i)
                pDevice->SetSamplerState(slot, kSamplerStates[i], savedSamplerStates[slot][i]);
            SAFE_RELEASE(oldTextures[slot]);
        }
        pDevice->SetPixelShaderConstantF(0, savedPSConsts, kPSConstCount);
        pDevice->SetVertexShaderConstantF(0, savedVSConsts, kVSConstCount);
        pDevice->SetPixelShader(oldPS);
        pDevice->SetVertexShader(oldVS);
        pDevice->SetRenderTarget(0, oldTarget);
        pDevice->SetRenderTarget(1, oldTarget1);
        pDevice->SetDepthStencilSurface(oldDepth);
        pDevice->SetViewport(&oldViewport);
        pDevice->SetFVF(oldFVF);
        pDevice->SetVertexDeclaration(oldDecl);
        pDevice->SetStreamSource(0, oldVB, oldOffset, oldStride);

        SAFE_RELEASE(oldPS);
        SAFE_RELEASE(oldVS);
        SAFE_RELEASE(oldDecl);
        SAFE_RELEASE(oldVB);
        SAFE_RELEASE(oldDepth);
        SAFE_RELEASE(oldTarget);
        SAFE_RELEASE(oldTarget1);
        SAFE_RELEASE(sceneSurface);
    }

    static void RenderWaterReflections()
    {
        auto& R = PostFxResources;

        if (!R.SSREnabled() || !R.SSREffect || !R.mDepthRT || !R.SSRHistoryTex)
            return;
        if (!R.bSSRReprojValid || R.fSSRWaterIntensity <= 0.0f)
            return;

        IDirect3DDevice9* pDevice = rage::grcDevice::GetD3DDevice();
        rage::grcViewport* vp = rage::GetCurrentViewport();
        if (!pDevice || !vp)
            return;

        const D3DXMATRIX& viewInv = *(const D3DXMATRIX*)vp->mViewInverseMatrix;

        float waterLevel = (R.pWaterLevel ? *R.pWaterLevel : 0.0f) + R.fSSRWaterLevelOffset;

        if (viewInv.m[3][2] <= waterLevel)
            return;

        D3DMATRIX proj = *(D3DMATRIX*)vp->mProjectionMatrix;
        const float axisSign[3] = { -1.0f, 1.0f, (proj._34 < 0.0f) ? -1.0f : 1.0f };

        D3DXVECTOR4 plane(viewInv.m[0][2] * axisSign[0],
                          viewInv.m[1][2] * axisSign[1],
                          viewInv.m[2][2] * axisSign[2],
                          viewInv.m[3][2] - waterLevel);

        float width = float(RenderScale::ToRenderWidth(uint32_t(vp->mWidth)));
        float height = float(RenderScale::ToRenderHeight(uint32_t(vp->mHeight)));

        auto& h = R.SSREffectHandles;
        ID3DXEffect* effect = R.SSREffect;

        effect->SetTexture(h.DepthTex2D, LightingDepth());
        effect->SetTexture(h.HistoryTex2D, R.SSRHistoryTex->mD3DTexture);

        SetTargetSize(effect, h, proj, width, height);
        SetDepthRange(effect, h, vp->mNearClip, vp->mFarClip);

        // The history and its depth come from one fog pass, this frame's if it already ran.
        if (R.bSSRHistoryThisFrame)
        {
            D3DXMATRIX viewProj;
            MatrixMultiply(viewProj, *(const D3DXMATRIX*)vp->mViewMatrix, *(const D3DXMATRIX*)vp->mProjectionMatrix);
            D3DXVECTOR4 rows[4];
            ViewToClipRows(vp, viewProj, rows);
            effect->SetVectorArray(h.vec4ViewToPrevClip, rows, 4);
        }
        else
            effect->SetVectorArray(h.vec4ViewToPrevClip, R.SSRReprojRows, 4);
        effect->SetVector(h.vec4WaterPlane, &plane);
        effect->SetTexture(h.PrevDepthTex2D, R.PreAlphaDepthCopyRT ? R.PreAlphaDepthCopyRT->mD3DTexture : nullptr);
        effect->SetFloat(h.fUsePrevDepth, R.PreAlphaDepthCopyRT && R.PreAlphaDepthCopyRT->mD3DTexture ? 1.0f : 0.0f);
        SetPrevDepthRange(effect, h);

        // Without both copies the pass falls back to the whole water plane.
        const bool waterMask = R.bWaterMaskCaptured && CopyRenderTargetToWaterMask(1);
        effect->SetTexture(h.PreWaterTex2D, waterMask ? R.WaterMaskTex[0] : nullptr);
        effect->SetTexture(h.PostWaterTex2D, waterMask ? R.WaterMaskTex[1] : nullptr);
        effect->SetFloat(h.fUseWaterMask, waterMask ? 1.0f : 0.0f);

        D3DXVECTOR4 toView[3];
        WorldToViewRows(vp, toView);
        effect->SetVectorArray(h.vec4WaterToView, toView, 3);

        D3DXVECTOR4 worldX(toView[0].x, toView[1].x, toView[2].x, viewInv.m[3][0]);
        D3DXVECTOR4 worldY(toView[0].y, toView[1].y, toView[2].y, viewInv.m[3][1]);
        effect->SetVector(h.vec4WaterWorldX, &worldX);
        effect->SetVector(h.vec4WaterWorldY, &worldY);

        effect->SetFloat(h.fMaxDistance, R.fSSRMaxDistance);
        effect->SetFloat(h.fThickness, R.fSSRThickness);
        effect->SetFloat(h.fEdgeFade, R.fSSREdgeFade);
        effect->SetFloat(h.fWaterIntensity, R.fSSRWaterIntensity);
        effect->SetFloat(h.fWaterBlur, R.fSSRWaterBlur);

        IDirect3DVertexDeclaration9* oldDecl = nullptr;
        IDirect3DVertexBuffer9* oldVB = nullptr;
        IDirect3DSurface9* ds = nullptr;
        UINT oldOffset = 0, oldStride = 0;
        DWORD oldFVF = 0;

        pDevice->GetFVF(&oldFVF);
        pDevice->GetVertexDeclaration(&oldDecl);
        pDevice->GetStreamSource(0, &oldVB, &oldOffset, &oldStride);
        pDevice->GetDepthStencilSurface(&ds);

        pDevice->SetDepthStencilSurface(nullptr);
        pDevice->SetStreamSource(0, nullptr, 0, 0);
        pDevice->SetVertexDeclaration(nullptr);
        pDevice->SetFVF(D3DFVF_XYZRHW | D3DFVF_TEX1);

        struct ScreenVertex { float x, y, z, rhw; float u, v; };
        ScreenVertex screenVertices[4] =
        {
            { -0.5f,         -0.5f,          0.0f, 1.0f, 0.0f, 0.0f },
            { -0.5f,          height - 0.5f, 0.0f, 1.0f, 0.0f, 1.0f },
            { width - 0.5f,  -0.5f,          0.0f, 1.0f, 1.0f, 0.0f },
            { width - 0.5f,   height - 0.5f, 0.0f, 1.0f, 1.0f, 1.0f }
        };

        UINT passes = 0;
        IDirect3DBaseTexture9* oldTextures[kSSRTextureSlots] = {};
        DWORD savedRenderStates[std::size(kWaterSSRRenderStates)] = {};
        DWORD savedSamplerStates[kSSRSamplerSlots][std::size(kSSRSamplerStates)] = {};

        for (DWORD slot = 0; slot < kSSRTextureSlots; ++slot)
            pDevice->GetTexture(slot, &oldTextures[slot]);
        SetTextureBoth(pDevice, 3, nullptr);

        effect->SetTexture(h.SurfaceTex2D, oldTextures[0]);
        effect->SetFloat(h.fWaterNormalStrength, oldTextures[0] ? R.fSSRWaterNormalStrength : 0.0f);
        // The rain's rings, as the game's water shader takes them (WaterRainRings): its reflection is
        // drawn over by this one, which hid them.
        {
            const auto rings = WaterRainRings();
            R.nWaterSsrSurface = oldTextures[0] ? 1 : 0;
            const D3DXVECTOR4 v(oldTextures[0] ? rings[0] : 0.0f, rings[1], rings[2], rings[3]);
            effect->SetVector(h.vec4WaterRings, &v);
        }

        pDevice->GetPixelShaderConstantF(0, savedPSConsts, kPSConstCount);
        pDevice->GetVertexShaderConstantF(0, savedVSConsts, kVSConstCount);

        for (size_t i = 0; i < std::size(kWaterSSRRenderStates); ++i)
        {
            pDevice->GetRenderState(kWaterSSRRenderStates[i].state, &savedRenderStates[i]);
            pDevice->SetRenderState(kWaterSSRRenderStates[i].state, kWaterSSRRenderStates[i].value);
        }

        for (DWORD slot = 0; slot < kSSRSamplerSlots; ++slot)
            for (size_t i = 0; i < std::size(kSSRSamplerStates); ++i)
            {
                pDevice->GetSamplerState(slot, kSSRSamplerStates[i].state, &savedSamplerStates[slot][i]);
                pDevice->SetSamplerState(slot, kSSRSamplerStates[i].state, kSSRSamplerStates[i].value);
            }

        // The shaders back on both devices, D3DX saves nothing (see SavedShaders); the rest is saved above
        SavedShaders shaders(pDevice);
        effect->SetTechnique(h.techSSRWater);
        effect->Begin(&passes, D3DXFX_DONOTSAVESTATE);
        effect->BeginPass(0);
        effect->CommitChanges();
        BindEffectSamplers(pDevice, effect);
        pDevice->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, screenVertices, sizeof(ScreenVertex));
        effect->EndPass();
        effect->End();

        for (size_t i = 0; i < std::size(kWaterSSRRenderStates); ++i)
            pDevice->SetRenderState(kWaterSSRRenderStates[i].state, savedRenderStates[i]);

        for (DWORD slot = 0; slot < kSSRSamplerSlots; ++slot)
            for (size_t i = 0; i < std::size(kSSRSamplerStates); ++i)
                pDevice->SetSamplerState(slot, kSSRSamplerStates[i].state, savedSamplerStates[slot][i]);

        pDevice->SetPixelShaderConstantF(0, savedPSConsts, kPSConstCount);
        pDevice->SetVertexShaderConstantF(0, savedVSConsts, kVSConstCount);

        for (DWORD slot = 0; slot < kSSRTextureSlots; ++slot)
        {
            SetTextureBoth(pDevice, slot, oldTextures[slot]);
            SAFE_RELEASE(oldTextures[slot]);
        }

        pDevice->SetDepthStencilSurface(ds);
        pDevice->SetFVF(oldFVF);
        pDevice->SetVertexDeclaration(oldDecl);
        pDevice->SetStreamSource(0, oldVB, oldOffset, oldStride);

        SAFE_RELEASE(ds);
        SAFE_RELEASE(oldDecl);
        SAFE_RELEASE(oldVB);
    }

    static inline SafetyHookInline shWaterRender{};
    // Copies the bound render target into WaterMaskTex[index], (re)creating both copies in
    // its size and format when needed.
    static bool CopyRenderTargetToWaterMask(int index)
    {
        auto& R = PostFxResources;
        IDirect3DDevice9* pDevice = rage::grcDevice::GetD3DDevice();
        IDirect3DSurface9* rt = nullptr;
        if (!pDevice || FAILED(pDevice->GetRenderTarget(0, &rt)) || !rt)
            return false;

        D3DSURFACE_DESC desc = {};
        rt->GetDesc(&desc);
        if (R.WaterMaskTex[0] && (desc.Width != R.WaterMaskDesc.Width || desc.Height != R.WaterMaskDesc.Height ||
            desc.Format != R.WaterMaskDesc.Format))
            R.ReleaseWaterMask();

        bool ok = true;
        for (int i = 0; i < 2 && ok; ++i)
        {
            if (!R.WaterMaskTex[i])
                ok = SUCCEEDED(pDevice->CreateTexture(desc.Width, desc.Height, 1, D3DUSAGE_RENDERTARGET, desc.Format,
                                                      D3DPOOL_DEFAULT, &R.WaterMaskTex[i], nullptr));
        }
        if (ok)
        {
            R.WaterMaskDesc = desc;
            IDirect3DSurface9* dst = nullptr;
            ok = SUCCEEDED(R.WaterMaskTex[index]->GetSurfaceLevel(0, &dst)) &&
                 SUCCEEDED(pDevice->StretchRect(rt, nullptr, dst, nullptr, D3DTEXF_NONE));
            SAFE_RELEASE(dst);
        }
        else
        {
            R.ReleaseWaterMask();
        }

        SAFE_RELEASE(rt);
        return ok;
    }

    // Whether the bound render target is the size of the screen, as the main scene's is.
    static bool RenderTargetIsScreenSized()
    {
        auto& R = PostFxResources;
        IDirect3DDevice9* pDevice = rage::grcDevice::GetD3DDevice();
        IDirect3DSurface9* rt = nullptr;
        if (!pDevice || !R.SSRSurf || FAILED(pDevice->GetRenderTarget(0, &rt)) || !rt)
            return false;
        D3DSURFACE_DESC desc = {}, screen = {};
        rt->GetDesc(&desc);
        R.SSRSurf->GetDesc(&screen);
        SAFE_RELEASE(rt);
        return desc.Width == RenderScale::ToRenderWidth(screen.Width) && desc.Height == RenderScale::ToRenderHeight(screen.Height);
    }

    // The rain's rings on open water (water_rain_rings.patch, c178), set right before the water is
    // drawn, through the game's device wrapper and the real device alike: set on the real device
    // alone at the end of the G-buffer pass, the water never saw them. x their strength while it
    // rains (WetGroundDebug 2: four times full, rain or not), y the clock, zw their fade from 25 to 40 m: the
    // water is mostly seen from a quay, further off than puddles.
    static std::array<float, 4> WaterRainRings()
    {
        auto& R = PostFxResources;
        const double seconds = CTimer::m_snTimeInMilliseconds ? *CTimer::m_snTimeInMilliseconds * 0.001 : 0.0;
        const float rain = CWeather::Rain ? std::clamp(*CWeather::Rain / 0.7f, 0.0f, 1.0f) : 0.0f;
        float rings = R.fWetGround > 0.0f && R.WetWeatherEnabled() ? rain * R.fWetGroundRipples : 0.0f;
        if (R.nWetGroundDebug == 2)
            rings = 4.0f; // unmissable, in any weather
        return { rings, float(std::fmod(seconds, 1000.0)), -1.0f / 15.0f, 40.0f / 15.0f };
    }

    static void SetWaterRainRings()
    {
        auto pDevice = rage::grcDevice::GetD3DDevice();
        if (!pDevice)
            return;
        const auto c178 = WaterRainRings();
        pDevice->SetPixelShaderConstantF(178, c178.data(), 1);
        std::memcpy(PostFxResources.WaterRingsLast, c178.data(), sizeof(PostFxResources.WaterRingsLast));
        ++PostFxResources.nWaterRingDraws;
        if (auto real = RealDevice(pDevice); real != pDevice)
            real->SetPixelShaderConstantF(178, c178.data(), 1);
    }

    static void __cdecl WaterRenderHook(int a1)
    {
        auto& R = PostFxResources;
        SetWaterRainRings();
        auto pProfileDevice = rage::grcDevice::GetD3DDevice();
        ProfilerScope timedWater(pProfileDevice, kProfWaterAll);
        // The game renders water for other views too, into targets of other sizes: each call
        // took two full screen copies and a full screen pass, and a target of another size
        // had both mask copies released and created again, even with no water on screen.
        // Only the main scene's water gets reflections, once a frame.
        const bool mainScene = R.SSREnabled() && R.SSREffect && R.fSSRWaterIntensity > 0.0f &&
                               !R.bWaterDoneThisFrame && RenderTargetIsScreenSized();
        R.bWaterMaskCaptured = mainScene && CopyRenderTargetToWaterMask(0);
        ProfilerMark(pProfileDevice, kProfWaterGame, true);
        shWaterRender.unsafe_ccall<void>(a1);
        ProfilerMark(pProfileDevice, kProfWaterGame, false);
        if (mainScene)
        {
            auto pDevice = rage::grcDevice::GetD3DDevice();
            ProfilerMark(pDevice, kProfWater, true);
            RenderWaterReflections();
            ProfilerMark(pDevice, kProfWater, false);
            R.bWaterDoneThisFrame = true;
        }
        R.bWaterMaskCaptured = false;
    }

    static void RenderAmbientOcclusion()
    {
        static auto AO = FusionFixSettings.GetRef("PREF_SAO");
        if (PostFxResources.AOEffect && PostFxResources.AOEnabled && AO->get())
        { // AO
            IDirect3DDevice9* pDevice = rage::grcDevice::GetD3DDevice();
            // Nothing saved by D3DX (see SavedShaders): textures, samplers, constants, the passes' render states and the
            // shaders are saved here
            SavedSamplerSlots savedSamplers(pDevice);
            SavedShaderConstants savedConstants(pDevice);
            SavedEffectPassStates savedStates(pDevice);
            SavedShaders savedShaders(pDevice);

            IDirect3DSurface9* rt0 = nullptr;
            IDirect3DSurface9* ds = nullptr;
            IDirect3DVertexDeclaration9* oldDecl = nullptr;
            IDirect3DVertexBuffer9* oldVB = nullptr;
            UINT oldOffset = 0;
            UINT oldStride = 0;
            DWORD oldFVF = 0;
            D3DVIEWPORT9 oldViewport;

            pDevice->GetFVF(&oldFVF);
            pDevice->GetVertexDeclaration(&oldDecl);
            pDevice->GetStreamSource(0, &oldVB, &oldOffset, &oldStride);
            pDevice->GetRenderTarget(0, &rt0);
            pDevice->GetDepthStencilSurface(&ds);
            pDevice->GetViewport(&oldViewport);

            pDevice->SetDepthStencilSurface(nullptr);
            pDevice->SetStreamSource(0, nullptr, 0, 0);
            pDevice->SetVertexDeclaration(nullptr);
            pDevice->SetFVF(D3DFVF_XYZRHW | D3DFVF_TEX1); // fullscreen fvf

            PostFxResources.SpecularTex = PostFxResources.mSpecularRT->mD3DTexture;
            PostFxResources.DepthTex = LightingDepth();
            IDirect3DSurface9* SpecularRT;
            PostFxResources.SpecularTex->GetSurfaceLevel(0, &SpecularRT);

            UINT passes = 0;
            ID3DXEffect* effect = PostFxResources.AOEffect;
            effect->Begin(&passes, D3DXFX_DONOTSAVESTATE); assert(passes == 7);
            {
                rage::grcViewport* currGrcViewport = rage::GetCurrentViewport();

                IDirect3DTexture9* camDepthTex = PostFxResources.AOCamDepthTex->mD3DTexture;
                IDirect3DTexture9* aoTex = PostFxResources.AOTex->mD3DTexture;
                IDirect3DTexture9* aoBlurTex = PostFxResources.AOBlurTex->mD3DTexture;
                auto& camDepthSurf = PostFxResources.AOCamDepthSurf;
                IDirect3DSurface9* aoSurf = PostFxResources.AOSurf;
                IDirect3DSurface9* aoBlurSurf = PostFxResources.AOBlurSurf;

                float width = float(RenderScale::ToRenderWidth(uint32_t(currGrcViewport->mWidth)));
                float height = float(RenderScale::ToRenderHeight(uint32_t(currGrcViewport->mHeight)));

                D3DVIEWPORT9 vp = {};
                vp.MaxZ = 1.0;
                vp.Height = DWORD(height);
                vp.Width = DWORD(width);
                pDevice->SetViewport(&vp);

                auto& h = PostFxResources.AOEffectHandles;

                struct ScreenVertex { float x, y, z, rhw; float u, v; };
                ScreenVertex screenVertices[4] =
                {
                    { -0.5f,         -0.5f,          0.0f, 1.0f, 0.0f, 0.0f },
                    { -0.5f,          height - 0.5f, 0.0f, 1.0f, 0.0f, 1.0f },
                    { width - 0.5f, -0.5f,          0.0f, 1.0f, 1.0f, 0.0f },
                    { width - 0.5f, height - 0.5f, 0.0f, 1.0f, 1.0f, 1.0f }
                };

                float invViewportSize[] = { 1.0f / width, 1.0f / height };

                effect->SetTexture(h.DepthTex2D, PostFxResources.DepthTex);
                SetTargetSize(effect, h, *(const D3DMATRIX*)currGrcViewport->mProjectionMatrix, width, height);
                SetDepthRange(effect, h, currGrcViewport->mNearClip, currGrcViewport->mFarClip);
                effect->SetFloat(h.fFarPlane, currGrcViewport->mFarClip);

                ProfilerMark(pDevice, kProfAODepth, true);
                pDevice->SetRenderTarget(0, camDepthSurf[0]);
                effect->BeginPass(0);
                BindEffectSamplers(pDevice, effect);
                pDevice->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, screenVertices, sizeof(ScreenVertex));
                effect->EndPass();

                effect->SetTexture(h.AOCamDepthTexture2D, camDepthTex);
                effect->BeginPass(1);
                for (auto i = 1; i < PostFxResources.nAmbientOcclusionMaxMipLevel; ++i)
                {
                    float prevMipDimensions[] = { float((int)width >> (i - 1)), float((int)height >> (i - 1)) };
                    float prevMipTexel[] = { float(1.0f / prevMipDimensions[0]), float(1.0f / prevMipDimensions[1]) };
                    float curMipWidth = float(int(width) >> i);
                    float curMipHeight = float(int(height) >> i);

                    effect->SetFloatArray(h.vec2PrevMipSize, prevMipDimensions, 2);
                    effect->SetFloatArray(h.vec2PrevMipTexel, prevMipTexel, 2);
                    effect->SetInt(h.iPreviousMip, i - 1);

                    ScreenVertex mipVertices[4] =
                    {
                        { -0.5f,         -0.5f,          0.0f, 1.0f, 0.0f, 0.0f },
                        { -0.5f,          curMipHeight - 0.5f, 0.0f, 1.0f, 0.0f, 1.0f },
                        { curMipWidth - 0.5f, -0.5f,          0.0f, 1.0f, 1.0f, 0.0f },
                        { curMipWidth - 0.5f, curMipHeight - 0.5f, 0.0f, 1.0f, 1.0f, 1.0f }
                    };

                    effect->CommitChanges();

                    pDevice->SetRenderTarget(0, PostFxResources.AOCamDepthSurf[i]);
                    BindEffectSamplers(pDevice, effect);
                    pDevice->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, mipVertices, sizeof(ScreenVertex));
                }
                effect->EndPass();
                ProfilerMark(pDevice, kProfAODepth, false);

                pDevice->SetRenderTarget(0, aoSurf);
                pDevice->Clear(0, nullptr, D3DCLEAR_TARGET, D3DCOLOR_COLORVALUE(1.0, 0.0, 0, 1.0), 1.0f, 0);

                D3DMATRIX proj = *(D3DMATRIX*)currGrcViewport->mProjectionMatrix;

                effect->SetFloat(h.fRadius, PostFxResources.fAmbientOcclusionRadius);
                // GTAO's normals, from the G-buffer when there is one
                {
                    const bool normals = PostFxResources.mNormalRT && PostFxResources.mNormalRT->mD3DTexture;
                    effect->SetTexture(h.NormalTex2D, normals ? PostFxResources.mNormalRT->mD3DTexture : nullptr);
                    effect->SetFloat(h.fUseNormals, normals ? 1.0f : 0.0f);
                    D3DXVECTOR4 toView[3];
                    WorldToViewRows(currGrcViewport, toView);
                    effect->SetVectorArray(h.vec4WorldToView, toView, 3);
                    effect->SetFloat(h.fGTAOStrength, PostFxResources.fAmbientOcclusionGTAOStrength);
                    effect->SetFloat(h.fThinOccluders, PostFxResources.fAmbientOcclusionGTAOThinOccluders);
                }
                auto& R = PostFxResources;
                const bool gtao = AO->get() == 2;
                const bool temporal = gtao && R.fAmbientOcclusionTemporal > 0.0f && R.AOAccumSurf[0] && R.AOAccumSurf[1];
                {
                    // While GTAO accumulates, its noise is read elsewhere every frame (FrameHistory::NoiseOffset).
                    auto offset = temporal ? FrameHistory::NoiseOffset() : std::array<float, 2>{};
                    effect->SetFloatArray(h.vec2NoiseOffset, offset.data(), 2);
                    const bool albedo = gtao && R.bAmbientOcclusionMultiBounce && R.mDiffuseRT && R.mDiffuseRT->mD3DTexture;
                    effect->SetTexture(h.AlbedoTex2D, albedo ? R.mDiffuseRT->mD3DTexture : nullptr);
                    effect->SetFloat(h.fMultiBounce, albedo ? 1.0f : 0.0f);
                }
                effect->SetFloat(h.fBias, PostFxResources.fAmbientOcclusionBias);
                effect->SetFloat(h.fIntensity, PostFxResources.fAmbientOcclusionIntensity);
                effect->SetFloat(h.fProjScale, proj._22 * 0.5f * height);

                effect->CommitChanges();

                ProfilerMark(pDevice, kProfAOMain, true);
                effect->BeginPass(gtao ? 5 : 2); // 1 SAO, 2 GTAO
                BindEffectSamplers(pDevice, effect);
                pDevice->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, screenVertices, sizeof(ScreenVertex));
                effect->EndPass();
                ProfilerMark(pDevice, kProfAOMain, false);

                // GTAO's accumulation over frames, before the blur: the history is this frame's
                // raw GTAO blended with last frame's, followed to where each pixel was.
                IDirect3DTexture9* blurSource = aoTex;
                if (temporal)
                {
                    const int prev = R.nAOAccumIndex, next = prev ^ 1;
                    const bool history = FrameHistory::CanReproject(R.nAOAccumFrame);
                    D3DXMATRIX prevViewProj;
                    if (FrameHistory::Previous().Valid)
                        prevViewProj = FrameHistory::Previous().ViewProjection;
                    else
                        MatrixMultiply(prevViewProj, *(const D3DXMATRIX*)currGrcViewport->mViewMatrix, *(const D3DXMATRIX*)currGrcViewport->mProjectionMatrix);
                    D3DXVECTOR4 reprojRows[4];
                    ViewToClipRows(currGrcViewport, prevViewProj, reprojRows);
                    effect->SetVectorArray(h.vec4ViewToPrevClip, reprojRows, 4);
                    const bool prevDepth = R.PreAlphaDepthCopyRT && R.PreAlphaDepthCopyRT->mD3DTexture;
                    effect->SetTexture(h.PrevDepthTex2D, prevDepth ? R.PreAlphaDepthCopyRT->mD3DTexture : nullptr);
                    effect->SetFloat(h.fUsePrevDepth, prevDepth ? 1.0f : 0.0f);
                    SetPrevDepthRange(effect, h);
                    auto motion = history ? FrameHistory::MotionVectors() : nullptr;
                    auto jitter = FrameHistory::JitterDeltaUV();
                    effect->SetTexture(h.MotionTex2D, motion);
                    effect->SetFloat(h.fUseMotion, motion ? 1.0f : 0.0f);
                    effect->SetFloatArray(h.vec2MotionJitter, jitter.data(), 2);
                    effect->SetFloat(h.fTemporalBlend, history ? R.fAmbientOcclusionTemporal : 0.0f);
                    effect->SetTexture(h.AOTexture2D, aoTex);
                    effect->SetTexture(h.AOHistoryTex2D, R.AOAccumTex[prev]->mD3DTexture);
                    effect->CommitChanges();

                    pDevice->SetRenderTarget(0, R.AOAccumSurf[next]);
                    ProfilerMark(pDevice, kProfAOTemporal, true);
                    effect->BeginPass(6);
                    BindEffectSamplers(pDevice, effect);
                    pDevice->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, screenVertices, sizeof(ScreenVertex));
                    effect->EndPass();
                    ProfilerMark(pDevice, kProfAOTemporal, false);

                    R.nAOAccumIndex = next;
                    R.nAOAccumFrame = FrameHistory::Frame();
                    blurSource = R.AOAccumTex[next]->mD3DTexture;
                }
                else
                    R.nAOAccumFrame = 0;

                float hor[2] = { invViewportSize[0] * PostFxResources.fAmbientOcclusionBlurRadius, 0.0 };
                float ver[2] = { 0.0, invViewportSize[1] * PostFxResources.fAmbientOcclusionBlurRadius };
                ProfilerMark(pDevice, kProfAOBlur, true);
                effect->BeginPass(3);
                for (auto i = 0; i < PostFxResources.nAmbientOcclusionBlurPasses; ++i)
                {
                    pDevice->SetRenderTarget(0, aoBlurSurf);
                    effect->SetTexture(h.AOTexture2D, i == 0 ? blurSource : aoTex); // blur pass 1
                    effect->SetFloatArray(h.vec2BlurDirection, hor, 2);
                    effect->CommitChanges();

                    BindEffectSamplers(pDevice, effect);
                    pDevice->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, screenVertices, sizeof(ScreenVertex));

                    pDevice->SetRenderTarget(0, aoSurf);
                    effect->SetTexture(h.AOTexture2D, aoBlurTex); // blur pass 2
                    effect->SetFloatArray(h.vec2BlurDirection, ver, 2);
                    effect->CommitChanges();

                    BindEffectSamplers(pDevice, effect);
                    pDevice->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, screenVertices, sizeof(ScreenVertex));
                }
                effect->EndPass();
                ProfilerMark(pDevice, kProfAOBlur, false);

                // final output
                pDevice->SetRenderTarget(0, SpecularRT);
                effect->SetTexture(h.AOTexture2D, PostFxResources.nAmbientOcclusionBlurPasses > 0 ? aoTex : blurSource);

                ProfilerMark(pDevice, kProfAOApply, true);
                effect->BeginPass(4);
                BindEffectSamplers(pDevice, effect);
                pDevice->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, screenVertices, sizeof(ScreenVertex));
                effect->EndPass();
                ProfilerMark(pDevice, kProfAOApply, false);
            }
            effect->End();

            pDevice->SetRenderTarget(0, rt0);
            pDevice->SetDepthStencilSurface(ds);
            pDevice->SetFVF(oldFVF);
            pDevice->SetVertexDeclaration(oldDecl);
            pDevice->SetStreamSource(0, oldVB, oldOffset, oldStride);
            pDevice->SetViewport(&oldViewport);

            SAFE_RELEASE(SpecularRT);
            SAFE_RELEASE(rt0);
            SAFE_RELEASE(ds);
            SAFE_RELEASE(oldDecl);
            SAFE_RELEASE(oldVB);
        }
    }

    static inline thread_local bool bInsteadDrawPrimitiveFog = false;
    static inline injector::hook_back<void(__fastcall*)(void*, void*, int, int, int)> hbDrawCallFog;
    static void __fastcall DrawCallFog(void* _this, void* edx, int a2, int a3, int a4)
    {
        bInsteadDrawPrimitiveFog = true;
        hbDrawCallFog.fun(_this, edx, a2, a3, a4);
        bInsteadDrawPrimitiveFog = false;
    }

    static inline thread_local bool bInsteadDrawPrimitivePostFX = false;
    static inline injector::hook_back<void(__fastcall*)(void*, void*, int, int, int)> hbDrawCallPostFX;
    static void __fastcall DrawCallPostFX(void* _this, void* edx, int a2, int a3, int a4)
    {
        UnbindGlassReflections();
        bInsteadDrawPrimitivePostFX = true;
        hbDrawCallPostFX.fun(_this, edx, a2, a3, a4);
        bInsteadDrawPrimitivePostFX = false;
        DrawSSRDebugOverlay();
        SSRTrace::Tick();
        TickIniReload();
    }

    // Once a frame, from the post fx pass, which runs in the pause menu too: Ctrl+Shift+F10 reads
    // the live settings (ReadLiveIni) again from the ini, with a beep to say it did, and adds the
    // cloud values the shadows use to GTAIV.EFLC.FusionFix.PostFx.log next to the plugin; Ctrl+Shift+F9
    // moves the clouds somewhere else.
    static void TickIniReload()
    {
        static bool keyWasDown = false;
        const bool down = (GetAsyncKeyState(VK_CONTROL) & 0x8000) && (GetAsyncKeyState(VK_SHIFT) & 0x8000) &&
            (GetAsyncKeyState(VK_F10) & 0x8000);
        if (down && !keyWasDown)
        {
            CIniReader iniReader("");
            auto& R = PostFxResources;
            const bool profiling = R.bPostFxProfiler;
            R.ReadLiveIni(iniReader);
            // A mark between the profiler's blocks, the next of which holds only frames with what was read.
            if (profiling && R.bPostFxProfiler)
            {
                ResetProfilerSums();
                FusionLog::Block log("PostFx", "Profiler");
                log.Printf("Ctrl+Shift+F10: the ini read again, the next block starts afresh\n");
            }
            // The cloud values the shadows were cast with, to tune CloudShadowsCoverage by.
            if (FusionLog::Block log("PostFx", "CloudShadows"); true)
            {
                log.Printf("threshold %.4f  bias %.4f  thickness %.4f  (%s)  coverage %+.3f  strength %.2f\n", R.fCloudLastThreshold,
                        R.fCloudLastBias, R.fCloudLastThickness, R.bCloudLastFromGame ? "the game's" : "fallback, the sky not drawn yet",
                        R.fCloudShadowsCoverage, R.fCloudShadows);
                const auto& top = rage::grmShaderInfo::getShaderParamData(R.CloudColorIdx);
                const auto& sunset = rage::grmShaderInfo::getShaderParamData(R.SunsetColorIdx);
                const auto& sunDir = rage::grmShaderInfo::getShaderParamData(R.CloudSunDirectionIdx);
                const auto& sky = rage::grmShaderInfo::getShaderParamData(R.CloudSkyColorIdx);
                const auto& moon = rage::grmShaderInfo::getShaderParamData(R.CloudMoonPositionIdx);
                const float* k = R.CloudShadowConsts;
                log.Component("Clouds");
                log.Printf("clouds lit from %.3f %.3f %.3f; the sky's sun %.3f %.3f %.3f; the game's light %.3f %.3f %.3f (frame %u, now %u); sky axis signs %+.0f %+.0f\n",
                        R.CloudLastUsedSun[0], R.CloudLastUsedSun[1], R.CloudLastUsedSun[2], R.CloudLastSkySun[0], R.CloudLastSkySun[1],
                        R.CloudLastSkySun[2], R.CloudLightDir[0], R.CloudLightDir[1], R.CloudLightDir[2], R.nCloudLightFrame, FrameHistory::Frame(),
                        R.CloudSkyAxisSign[0], R.CloudSkyAxisSign[1]);
                log.Printf("clouds: density %.4f, absorption %.2f x %.2f, translucency %.2f, detail %.2f, shade %.2f, sky match %.2f x %.2f\n",
                        R.fVolumetricCloudsDensity * R.Cloud.density, R.fVolumetricCloudsAbsorption, R.Cloud.absorption,
                        R.fVolumetricCloudsTranslucency * R.Cloud.translucency, R.fVolumetricCloudsDetail * R.Cloud.detail,
                        R.fVolumetricCloudsShade, R.fVolumetricCloudsSkyMatch, R.Cloud.skyMatch);
                log.Printf("clouds drawn with lit %.2f %.2f %.2f, shade %.2f %.2f %.2f, ceiling %.2f; lit by the %s at %.2f; sky clamp %.2f %.2f %.2f; volumetric fog %d; sky HDR %d; sky match %.2f; debug %d\n",
                        R.CloudLastLit[0], R.CloudLastLit[1], R.CloudLastLit[2], R.CloudLastShade[0], R.CloudLastShade[1], R.CloudLastShade[2],
                        R.CloudLastCeiling, R.bCloudLastMoonlit ? "moon" : "sun", R.CloudLastLightStrength, R.CloudLastClamp[0], R.CloudLastClamp[1], R.CloudLastClamp[2],
                        [] { static auto fog = FusionFixSettings.GetRef("PREF_VOLUMETRICFOG"); return fog ? fog->get() : -1; }(), int(bSkyHDR), R.fVolumetricCloudsSkyMatch, R.nVolumetricCloudsDebug);
                log.Component("CloudShadows");
                log.Printf("shadow constants: c197 %.3f %.3f %.1f %.6f  c198 %.3f %.3f %.3f %.3f  c199 %.3f %.3f  noise %s, %s after the lights  debug %d\n",
                        k[0], k[1], k[2], k[3], k[4], k[5], k[6], k[7], k[8], k[9], R.CloudNoiseTexture ? "made" : "missing",
                        R.bCloudNoiseSurvived ? "still bound" : "gone", R.nCloudShadowsDebug);
                log.Printf("s12 before the shadows: srgb %lu  max mip %lu  min filter %lu  lod bias %.3f\n",
                        static_cast<unsigned long>(R.CloudSamplerBefore[0]), static_cast<unsigned long>(R.CloudSamplerBefore[1]),
                        static_cast<unsigned long>(R.CloudSamplerBefore[2]), std::bit_cast<float>(R.CloudSamplerBefore[3]));
                log.Component("GGX");
                log.Printf("GGX lights: %u headlights among %u lights drawn since the last log\n", R.nLightGGXHeadlights, R.nLightGGXLights);
                R.nLightGGXHeadlights = 0;
                R.nLightGGXLights = 0;
                log.Printf("headlight glints: %s; effect %s (hr 0x%08lX); %u headlights drawn; their intensity %.3f to %.3f, outer cone %.3f to %.3f\n",
                        R.szGlintsStatus, R.HeadlightGlintsEffect ? "built" : "missing", static_cast<unsigned long>(R.hrHeadlightGlintsEffect),
                        R.nGlintsLastLights, R.GlintsLastIntensity[0], R.GlintsLastIntensity[1], R.GlintsLastCone[0], R.GlintsLastCone[1]);
                log.Component("VehicleBoxShadows");
                log.Printf("vehicle box shadows: %s, hook %s; since the last log %u frames drawn with their boxes, %u without, %u lights shaded with boxes; light size %.2f, rounding %.2f, from car lights %d, with slots %d, size %.2f %.2f %.2f of the bounds, self margin %.2f, low light narrow %.2f\n",
                        R.VehicleBoxShadowsEnabled() ? "on" : "off", VehicleBoxShadows::lightListBuilt ? "installed" : "missing",
                        VehicleBoxShadows::framesMatched.exchange(0), VehicleBoxShadows::framesUnmatched.exchange(0), VehicleBoxShadows::lightsWithBoxes.exchange(0),
                        R.fVehicleBoxShadowLightSize, R.fVehicleBoxShadowRounding, int(R.bVehicleBoxShadowsFromCarLights), int(R.bVehicleBoxShadowsWithSlots),
                        R.fVehicleBoxShadowScale[0], R.fVehicleBoxShadowScale[1], R.fVehicleBoxShadowScale[2], R.fVehicleBoxShadowSelfMargin,
                        R.fVehicleBoxShadowLowLightNarrow);
                {
                    const auto& l = VehicleBoxShadows::lastLowLight;
                    log.Printf("%u times a lamp no higher than a car's roof was left without its box; the last at %.1f %.1f %.1f, %.2f m above the box's bottom: type %d, flags 0x%X, radius %.1f, intensity %.2f, shadow key 0x%08X, cache %d\n",
                            VehicleBoxShadows::lowLights.exchange(0), l.mPosition.x, l.mPosition.y, l.mPosition.z, VehicleBoxShadows::lastLowHeight,
                            int(l.mType), l.mFlags, l.mRadius, l.mIntensity, uint32_t(l.mCastShadows), l.mShadowCacheIndex);
                }
                {
                    const auto& b = VehicleBoxShadows::nearestBounds;
                    const auto& x = VehicleBoxShadows::nearestBox;
                    log.Printf("nearest car %.1f m away: model bounds %.2f %.2f %.2f to %.2f %.2f %.2f (x right, y forward, z up); box half length %.2f, width %.2f, height %.2f, middle %.2f above its position\n",
                            VehicleBoxShadows::nearestDistance, b[0], b[1], b[2], b[3], b[4], b[5], x[0], x[1], x[2], x[3]);
                }
                log.Component("CloudReflections");
                log.Printf("clouds in reflections: %s; %u reflection map and %u water reflection skies since the last log; viewport %lu,%lu %lux%lu of a %ux%u target\n",
                        R.szCloudsReflectionStatus, R.nCloudReflectionCalls, R.nCloudWaterReflectionCalls, R.CloudReflectionViewport.X, R.CloudReflectionViewport.Y,
                        R.CloudReflectionViewport.Width, R.CloudReflectionViewport.Height, R.CloudReflectionTarget[0], R.CloudReflectionTarget[1]);
                R.nCloudReflectionCalls = 0;
                R.nCloudWaterReflectionCalls = 0;
                log.Component("WaterRings");
                log.Printf("water rings: c178 %.2f %.1f %.3f %.3f; %u water draws since the last log; SSR on the water %s\n",
                        R.WaterRingsLast[0], R.WaterRingsLast[1], R.WaterRingsLast[2], R.WaterRingsLast[3], R.nWaterRingDraws,
                        R.nWaterSsrSurface < 0 ? "not drawn" : R.nWaterSsrSurface ? "had the wave texture" : "had no wave texture (flat, no rings)");
                R.nWaterRingDraws = 0;
                log.Component("WetGround");
                log.Printf("wet ground: %s; effect %s (hr 0x%08lX); wetness %.3f, rain %.3f, materials 0x%02X, debug %d\n",
                        R.szWetGroundStatus, R.WetGroundEffect ? "built" : "missing", static_cast<unsigned long>(R.hrWetGroundEffect), R.fWetness,
                        CWeather::Rain ? *CWeather::Rain : -1.0f, unsigned(R.nWetGroundMaterials), R.nWetGroundDebug);
                log.Component("Clouds");
                log.Printf("volumetric clouds: %s; effect %s (hr 0x%08lX); shadows follow them %d; HDRExposure %.3f; CloudColor %.3f %.3f %.3f; "
                             "SunsetColor %.3f %.3f %.3f; CloudInscatteringRange %.3f; SunDirection %.3f %.3f %.3f; SkyColor %.3f %.3f %.3f; MoonPosition %.3f %.3f %.3f\n",
                        R.szCloudsStatus, R.CloudsEffect ? "built" : "missing", static_cast<unsigned long>(R.hrCloudsEffect), int(R.VolumetricCloudsOn()),
                        rage::grmShaderInfo::getShaderParamData(R.CloudExposureIdx)[0], top[0], top[1], top[2], sunset[0], sunset[1], sunset[2],
                        rage::grmShaderInfo::getShaderParamData(R.CloudInscatteringIdx)[0], sunDir[0], sunDir[1], sunDir[2],
                        sky[0], sky[1], sky[2], moon[0], moon[1], moon[2]);
                // The game's weather by name, and the cloud layer it gives, so a screenshot can be
                // matched to its weather.
                {
                    static const char* kWeatherNames[8] = { "EXTRASUNNY", "SUNNY", "SUNNY_WINDY", "CLOUDY", "RAIN", "DRIZZLE", "FOGGY", "LIGHTNING" };
                    auto name = [](const auto* type) {
                        const auto value = type ? static_cast<uint32_t>(*type) : 99u;
                        return value < 8 ? kWeatherNames[value] : "?";
                    };
                    const float k = CWeather::InterpolationValue ? *CWeather::InterpolationValue : 0.0f;
                    const auto& c = R.Cloud;
                    log.Printf("weather %s -> %s at %.2f; cloud layer: cover %.2f, base %.0f m, thickness %.0f m, sheet %.2f, wind %.2f\n",
                            name(CWeather::OldWeatherType), name(CWeather::NewWeatherType), k, c.coverage, c.base, c.thickness, c.stratus, c.wind);
                }
            }
            MessageBeep(MB_OK);
        }
        keyWasDown = down;

        // Ctrl+Shift+F9: the clouds start somewhere else, as at a new game (a new place on the
        // map, a new phase of its morph and of the billows), to look at other skies without
        // restarting. The accumulation starts over, so the old clouds do not linger.
        static bool reseedWasDown = false;
        const bool reseed = (GetAsyncKeyState(VK_CONTROL) & 0x8000) && (GetAsyncKeyState(VK_SHIFT) & 0x8000) &&
            (GetAsyncKeyState(VK_F9) & 0x8000);
        if (reseed && !reseedWasDown)
        {
            auto& R = PostFxResources;
            R.bCloudDriftSeeded = false;
            R.nCloudAccumFrame = 0;
            MessageBeep(MB_OK);
        }
        reseedWasDown = reseed;
    }

    // Replaces the finished frame with the SSR debug view chosen in the graphics menu.
    static void DrawSSRDebugOverlay()
    {
        auto& R = PostFxResources;
        SSRTrace::Line("debug overlay: valid %d", int(R.bSSRDebugValid));
        if (!R.bSSRDebugValid || !R.SSRDebugTex || !R.SSREffect || !R.SSREffectHandles.techSSRDebugCopy)
            return;
        R.bSSRDebugValid = false;

        IDirect3DDevice9* pDevice = rage::grcDevice::GetD3DDevice();
        if (!pDevice)
            return;

        IDirect3DSurface9* rt0 = nullptr;
        if (FAILED(pDevice->GetRenderTarget(0, &rt0)) || !rt0)
            return;
        D3DSURFACE_DESC desc = {};
        rt0->GetDesc(&desc);
        const float width = float(desc.Width);
        const float height = float(desc.Height);

        IDirect3DSurface9* ds = nullptr;
        IDirect3DVertexDeclaration9* oldDecl = nullptr;
        IDirect3DVertexBuffer9* oldVB = nullptr;
        UINT oldOffset = 0, oldStride = 0;
        DWORD oldFVF = 0;
        D3DVIEWPORT9 oldViewport;
        pDevice->GetFVF(&oldFVF);
        pDevice->GetVertexDeclaration(&oldDecl);
        pDevice->GetStreamSource(0, &oldVB, &oldOffset, &oldStride);
        pDevice->GetDepthStencilSurface(&ds);
        pDevice->GetViewport(&oldViewport);

        IDirect3DBaseTexture9* oldTextures[kSSRTextureSlots] = {};
        DWORD savedRenderStates[std::size(kSSRRenderStates)] = {};
        DWORD savedSamplerStates[kSSRSamplerSlots][std::size(kSSRSamplerStates)] = {};
        for (DWORD slot = 0; slot < kSSRTextureSlots; ++slot)
            pDevice->GetTexture(slot, &oldTextures[slot]);
        pDevice->GetPixelShaderConstantF(0, savedPSConsts, kPSConstCount);
        pDevice->GetVertexShaderConstantF(0, savedVSConsts, kVSConstCount);
        for (size_t i = 0; i < std::size(kSSRRenderStates); ++i)
        {
            pDevice->GetRenderState(kSSRRenderStates[i].state, &savedRenderStates[i]);
            pDevice->SetRenderState(kSSRRenderStates[i].state, kSSRRenderStates[i].value);
        }
        for (DWORD slot = 0; slot < kSSRSamplerSlots; ++slot)
            for (size_t i = 0; i < std::size(kSSRSamplerStates); ++i)
            {
                pDevice->GetSamplerState(slot, kSSRSamplerStates[i].state, &savedSamplerStates[slot][i]);
                pDevice->SetSamplerState(slot, kSSRSamplerStates[i].state, kSSRSamplerStates[i].value);
            }

        pDevice->SetDepthStencilSurface(nullptr);
        pDevice->SetStreamSource(0, nullptr, 0, 0);
        pDevice->SetVertexDeclaration(nullptr);
        pDevice->SetFVF(D3DFVF_XYZRHW | D3DFVF_TEX1);
        D3DVIEWPORT9 vpDesc = {};
        vpDesc.Width = desc.Width;
        vpDesc.Height = desc.Height;
        vpDesc.MaxZ = 1.0f;
        pDevice->SetViewport(&vpDesc);

        struct ScreenVertex { float x, y, z, rhw; float u, v; };
        ScreenVertex screenVertices[4] =
        {
            { -0.5f,         -0.5f,          0.0f, 1.0f, 0.0f, 0.0f },
            { -0.5f,          height - 0.5f, 0.0f, 1.0f, 0.0f, 1.0f },
            { width - 0.5f,  -0.5f,          0.0f, 1.0f, 1.0f, 0.0f },
            { width - 0.5f,   height - 0.5f, 0.0f, 1.0f, 1.0f, 1.0f }
        };

        auto& h = R.SSREffectHandles;
        ID3DXEffect* effect = R.SSREffect;
        effect->SetTexture(h.DebugTex2D, R.SSRDebugTex->mD3DTexture);
        UINT passes = 0;
        effect->SetTechnique(h.techSSRDebugCopy);
        effect->Begin(&passes, 0);
        effect->BeginPass(0);
        effect->CommitChanges();
        BindEffectSamplers(pDevice, effect);
        pDevice->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, screenVertices, sizeof(ScreenVertex));
        effect->EndPass();
        effect->End();

        for (size_t i = 0; i < std::size(kSSRRenderStates); ++i)
            pDevice->SetRenderState(kSSRRenderStates[i].state, savedRenderStates[i]);
        for (DWORD slot = 0; slot < kSSRSamplerSlots; ++slot)
            for (size_t i = 0; i < std::size(kSSRSamplerStates); ++i)
                pDevice->SetSamplerState(slot, kSSRSamplerStates[i].state, savedSamplerStates[slot][i]);
        pDevice->SetPixelShaderConstantF(0, savedPSConsts, kPSConstCount);
        pDevice->SetVertexShaderConstantF(0, savedVSConsts, kVSConstCount);
        for (DWORD slot = 0; slot < kSSRTextureSlots; ++slot)
        {
            SetTextureBoth(pDevice, slot, oldTextures[slot]);
            SAFE_RELEASE(oldTextures[slot]);
        }

        pDevice->SetDepthStencilSurface(ds);
        pDevice->SetViewport(&oldViewport);
        pDevice->SetFVF(oldFVF);
        pDevice->SetVertexDeclaration(oldDecl);
        pDevice->SetStreamSource(0, oldVB, oldOffset, oldStride);
        SAFE_RELEASE(rt0);
        SAFE_RELEASE(ds);
        SAFE_RELEASE(oldDecl);
        SAFE_RELEASE(oldVB);
    }

    // The first pass of the game's post processing downsamples the scene for bloom and exposure
    static inline thread_local bool bInsteadDrawPrimitiveDownsample = false;
    static inline injector::hook_back<void(__fastcall*)(void*, void*, int, int, int)> hbDrawCallDownsample;
    static void __fastcall DrawCallDownsample(void* _this, void* edx, int a2, int a3, int a4)
    {
        // Before the game binds FullScreenCopy and computes its texel size for the downsample
        UpscaleScene();
        bInsteadDrawPrimitiveDownsample = true;
        hbDrawCallDownsample.fun(_this, edx, a2, a3, a4);
        bInsteadDrawPrimitiveDownsample = false;
    }

    // Depth of field, sun shafts and the game's post processing read the depth after the resolve: averaged over the
    // jitter when temporal anti-aliasing has it, which keeps their edges still
    static IDirect3DTexture9* PostDepth()
    {
        if (auto steady = TemporalAA::GetSteadyDepth())
            return steady;
        return PostFxResources.PreAlphaDepthCopyRT->mD3DTexture;
    }

    // With temporal anti-aliasing, DLAA or FSR the stipple filter runs before them, on the scene at the render size,
    // instead of in the post processing after them. Returns what the resolve reads: the filtered copy, or the scene
    // when the filter did not run. A scene nothing resolved takes the copy with TemporalAA::KeepStipple.
    static IDirect3DTexture9* FilterStippleBeforeResolve(IDirect3DTexture9* scene)
    {
        if (TemporalAA::GetMode() == TemporalAA::Mode::Off || !PostFxResources.useStippleFilter || !PostFxResources.stipple_filter_ps)
            return scene;
        auto filtered = TemporalAA::FilterStipple(scene, PostFxResources.stipple_filter_ps);
        return filtered ? filtered : scene;
    }

    // Render scale: from here on FullScreenCopy is a texture of the screen size, with the scene upscaled by DLSS
    // or FSR, or stretched when neither runs
    static void UpscaleScene()
    {
        if (!RenderScale::IsActive() || !PostFxResources.FullScreenTex_temp1 || !PostFxResources.FullScreenTex_temp1->mD3DTexture)
            return;

        auto pDevice = rage::grcDevice::GetD3DDevice();
        ProfilerScope timed(pDevice, kProfResolve);
        IDirect3DTexture9* scene = nullptr;
        IDirect3DSurface9* sceneSurface = nullptr;
        IDirect3DSurface9* output = nullptr;
        if (!RenderScale::BeginPost(pDevice, scene, sceneSurface, output))
            return;
        // The scene at the render size is not read past here: the filtered copy stands in for it
        auto source = FilterStippleBeforeResolve(scene);

        // DLSS and FSR write straight into the full size texture
        IDirect3DTexture9* outputTexture = nullptr;
        output->GetContainer(__uuidof(IDirect3DTexture9), reinterpret_cast<void**>(&outputTexture));

        auto mode = TemporalAA::GetMode();
        if (!outputTexture || (mode != TemporalAA::Mode::DLAA && mode != TemporalAA::Mode::FSR) ||
            !TemporalAA::Resolve(pDevice, source, outputTexture, output))
        {
            IDirect3DSurface9* sourceSurface = nullptr;
            if (SUCCEEDED(source->GetSurfaceLevel(0, &sourceSurface)) && sourceSurface)
                pDevice->StretchRect(sourceSurface, nullptr, output, nullptr, D3DTEXF_LINEAR);
            SAFE_RELEASE(sourceSurface);
        }
        SAFE_RELEASE(outputTexture);
    }

    // Temporal anti-aliasing resolves the scene before the game computes bloom from it, which would otherwise
    // follow the jitter. The result goes back into the scene copy that the game and PostFx3 read.
    static void ResolveScene()
    {
        // With the render scale UpscaleScene did it, on the scene at the render size
        if (TemporalAA::GetMode() == TemporalAA::Mode::Off || TemporalAA::IsSceneResolved() || RenderScale::IsActive())
            return;
        if (!PostFxResources.mFullScreenRT || !PostFxResources.mFullScreenRT->mD3DTexture ||
            !PostFxResources.FullScreenTex_temp1 || !PostFxResources.FullScreenTex_temp1->mD3DTexture)
            return;

        auto pDevice = rage::grcDevice::GetD3DDevice();
        ProfilerScope timed(pDevice, kProfResolve);
        auto scene = PostFxResources.mFullScreenRT->mD3DTexture;
        auto resolved = PostFxResources.FullScreenTex_temp1->mD3DTexture;
        IDirect3DSurface9* sceneSurface = nullptr;
        IDirect3DSurface9* resolvedSurface = nullptr;
        scene->GetSurfaceLevel(0, &sceneSurface);
        resolved->GetSurfaceLevel(0, &resolvedSurface);
        auto source = FilterStippleBeforeResolve(scene);
        // Straight into the scene when the resolve reads the stipple filter's copy, or when DLAA or FSR, which take
        // the scene in before they write, resolve it; temporal AA reading the scene itself writes into the copy
        auto mode = TemporalAA::GetMode();
        bool upscaler = mode == TemporalAA::Mode::DLAA || mode == TemporalAA::Mode::FSR;
        bool direct = sceneSurface && (source != scene || upscaler) && TemporalAA::Resolve(pDevice, source, scene, sceneSurface);
        if (!direct && source != scene)
            TemporalAA::KeepStipple(scene);
        else if (!direct && sceneSurface && resolvedSurface && TemporalAA::Resolve(pDevice, scene, resolved, resolvedSurface))
            pDevice->StretchRect(resolvedSurface, nullptr, sceneSurface, nullptr, D3DTEXF_POINT);
        SAFE_RELEASE(resolvedSurface);
        SAFE_RELEASE(sceneSurface);
    }

    static inline injector::hook_back<void(__stdcall*)()> hbDrawPrimitivePostFX;
    static void __stdcall DrawPrimitivePostFX()
    {
        if (bInsteadDrawPrimitiveFog)
        {
            bInsteadDrawPrimitiveFog = false;
            // Do not initialize shaders, RTs etc. here or we get a device reset error for some reason
            NewFog();
            // Transparent geometry and visual effects come next: the reactive mask of temporal AA starts here
            TemporalAA::OnFogDrawn();
        }
        else if (bInsteadDrawPrimitivePostFX)
        {
            bInsteadDrawPrimitivePostFX = false;
            Init();
            NewPostFX();
        }
        else if (bInsteadDrawPrimitiveDownsample)
        {
            bInsteadDrawPrimitiveDownsample = false;
            ResolveScene();
            hbDrawPrimitivePostFX.fun();
        }
        else
        {
            hbDrawPrimitivePostFX.fun();
        }
    }

    // Whether the bound target is the water's reflection (WATER_REFLECTION_COLOUR). Its sky is drawn
    // through the sky's main branch, not the reflection map's (REFLECTION_MAP_COLOUR, the paraboloids
    // the surfaces' sky reflection comes from, phases with flag 0x40000).
    static bool IsWaterReflectionTarget(IDirect3DDevice9* pDevice)
    {
        auto rt = rage::grcTextureFactoryPC::GetRTByName("WATER_REFLECTION_COLOUR");
        if (!pDevice || !rt || !rt->mD3DTexture)
            return false;
        IDirect3DSurface9* water = nullptr;
        IDirect3DSurface9* bound = nullptr;
        rt->mD3DTexture->GetSurfaceLevel(0, &water);
        pDevice->GetRenderTarget(0, &bound);
        const bool same = water && water == bound;
        SAFE_RELEASE(water);
        SAFE_RELEASE(bound);
        return same;
    }

    static inline injector::hook_back<int(__fastcall*)(int, void*, int, int, char, char, int, char)> hbDrawSkyHook;
    // The sky's main branch, which also draws the water reflection's sky: the volumetric clouds go in
    // right after it there, as into the reflection map (DrawSkyReflection).
    static int __fastcall DrawSky(int _this, void* edx, int a2, int a3, char a4, char a5, int a6, char a7)
    {
        auto pDevice = rage::grcDevice::GetD3DDevice();
        const int result = DrawSkyMain(_this, edx, a2, a3, a4, a5, a6, a7);
        if (pDevice && IsWaterReflectionTarget(pDevice))
        {
            ++PostFxResources.nCloudWaterReflectionCalls;
            RenderVolumetricClouds(pDevice, nullptr, true, kProfCloudReflectionWater);
        }
        return result;
    }

    // The scene's sky also writes the clouds' mask for the sun shafts (gta_atmoscatt_clouds, oC1) into
    // _DEFERRED_GBUFFER_0_, bound as the second target. The scene is 64 bits a pixel, the G-buffer 32:
    // a device that takes targets of different bit depths together, blending, draws the sky once, into both;
    // any other draws it twice, with the G-buffer for the mask, then without for the scene, as before.
    // The water's reflection, another size than the G-buffer, draws its sky once without it.
    static int DrawSkyMain(int _this, void* edx, int a2, int a3, char a4, char a5, int a6, char a7)
    {
        auto pDevice = rage::grcDevice::GetD3DDevice();
        if (!PostFxResources.DiffuseTex)
            return hbDrawSkyHook.fun(_this, edx, a2, a3, a4, a5, a6, a7);

        static const bool independentBitDepths = [&]()
        {
            D3DCAPS9 caps = {};
            // and blends with both bound, as the sky's layers may
            constexpr DWORD needed = D3DPMISCCAPS_MRTINDEPENDENTBITDEPTHS | D3DPMISCCAPS_MRTPOSTPIXELSHADERBLENDING;
            return SUCCEEDED(pDevice->GetDeviceCaps(&caps)) && (caps.PrimitiveMiscCaps & needed) == needed;
        }();

        IDirect3DSurface9* DiffuseSurf = nullptr;
        IDirect3DSurface9* target = nullptr;
        PostFxResources.DiffuseTex->GetSurfaceLevel(0, &DiffuseSurf);
        pDevice->GetRenderTarget(0, &target);
        D3DSURFACE_DESC diffuseDesc = {}, targetDesc = {};
        const bool scene = DiffuseSurf && target && SUCCEEDED(DiffuseSurf->GetDesc(&diffuseDesc)) && SUCCEEDED(target->GetDesc(&targetDesc)) &&
                           diffuseDesc.Width == targetDesc.Width && diffuseDesc.Height == targetDesc.Height;
        SAFE_RELEASE(target);
        if (!scene)
        {
            SAFE_RELEASE(DiffuseSurf);
            return hbDrawSkyHook.fun(_this, edx, a2, a3, a4, a5, a6, a7);
        }

        IDirect3DPixelShader9* pShader = nullptr;
        pDevice->GetPixelShader(&pShader);
        IDirect3DSurface9* oldRenderTarget1 = nullptr;
        pDevice->GetRenderTarget(1, &oldRenderTarget1);
        pDevice->SetRenderTarget(1, DiffuseSurf);
        int hr = hbDrawSkyHook.fun(_this, edx, a2, a3, a4, a5, a6, a7);
        pDevice->SetRenderTarget(1, oldRenderTarget1);
        if (!independentBitDepths)
        {
            pDevice->SetPixelShader(pShader);
            hr = hbDrawSkyHook.fun(_this, edx, a2, a3, a4, a5, a6, a7);
        }
        SAFE_RELEASE(oldRenderTarget1);
        SAFE_RELEASE(DiffuseSurf);
        SAFE_RELEASE(pShader);
        return hr;
    }

    // The reflection map's sky (CE 0xdbc2ab, the sky draw's branch for render phases with flag
    // 0x40000): the volumetric clouds go in right after it, through the reflection's viewport.
    static inline injector::hook_back<int(__fastcall*)(int, void*, int, int, char, char, int, char)> hbDrawSkyReflection;
    static int __fastcall DrawSkyReflection(int _this, void* edx, int a2, int a3, char a4, char a5, int a6, char a7)
    {
        const int result = hbDrawSkyReflection.fun(_this, edx, a2, a3, a4, a5, a6, a7);
        ++PostFxResources.nCloudReflectionCalls;
        if (auto pDevice = rage::grcDevice::GetD3DDevice())
            RenderVolumetricClouds(pDevice, nullptr, true);
        return result;
    }

    static inline SafetyHookInline RenderPedAndVehicleFakeShadowsInlineHook;
    static DWORD __cdecl RenderPedAndVehicleFakeShadows(DWORD a1)
    {
        DWORD result = RenderPedAndVehicleFakeShadowsInlineHook.unsafe_ccall<DWORD>(a1);
        RefreshGBufferTargets();
        TraceLightingInputs(rage::grcDevice::GetD3DDevice());

        auto pDevice = rage::grcDevice::GetD3DDevice();
        if (auto vp = rage::GetCurrentViewport())
        {
            const D3DMATRIX& proj = *(const D3DMATRIX*)vp->mProjectionMatrix;
            const float camera[4] = { fabsf(proj._11), fabsf(proj._22), vp->mNearClip, vp->mFarClip };
            memcpy(PostFxResources.SkinCamera, camera, sizeof(camera));
        }
        if (auto vp = rage::GetCurrentViewport())
            SSRTrace::Line("lighting phase: vp %p %dx%d", static_cast<void*>(vp), int(vp->mWidth), int(vp->mHeight));
        ProfilerMark(pDevice, kProfAO, true);
        RenderAmbientOcclusion();
        ProfilerMark(pDevice, kProfAO, false);
        ProfilerMark(pDevice, kProfSSR, true);
        RenderScreenSpaceReflections();
        ProfilerMark(pDevice, kProfSSR, false);
        ProfilerMark(pDevice, kProfContact, true);
        RenderContactShadows();
        ProfilerMark(pDevice, kProfContact, false);
        ProfilerMark(pDevice, kProfGI, true);
        RenderIndirectLight();
        ProfilerMark(pDevice, kProfGI, false);
        // deferred_lighting draws after this; BindSSRTexture bound last frame's results.
        if (pDevice)
        {
            BindLightingInputs(pDevice);
            SetLightingStage(pDevice, 1);
        }

        return result;
    }

    // Draws one full screen quad of the SSR effect's technique into target, then restores the
    // device state it touched. The draw is timed as profile, see ProfilerSection.
    static void DrawEffectPass(IDirect3DDevice9* pDevice, ID3DXEffect* effect, D3DXHANDLE technique, IDirect3DSurface9* target, float width, float height,
                               int profile = -1)
    {
        struct ScreenVertex { float x, y, z, rhw; float u, v; };
        const ScreenVertex screenVertices[4] =
        {
            { -0.5f,         -0.5f,          0.0f, 1.0f, 0.0f, 0.0f },
            { -0.5f,          height - 0.5f, 0.0f, 1.0f, 0.0f, 1.0f },
            { width - 0.5f,  -0.5f,          0.0f, 1.0f, 1.0f, 0.0f },
            { width - 0.5f,   height - 0.5f, 0.0f, 1.0f, 1.0f, 1.0f }
        };
        pDevice->SetRenderTarget(0, target);

        // No state saving by D3DX (see SavedShaders): the callers save and restore textures, samplers, render states
        // and constants themselves, the shaders, which the effect sets, are put back here.
        SavedShaders shaders(pDevice);

        UINT passes = 0;
        effect->SetTechnique(technique);
        effect->Begin(&passes, D3DXFX_DONOTSAVESTATE);
        effect->BeginPass(0);
        effect->CommitChanges();
        BindEffectSamplers(pDevice, effect);
        if (SSRTrace::Active())
        {
            // The viewport the device holds against the target's size and the quad's: a quad or viewport of another
            // size than the target draws part of it, and the passes rebuild positions on the wrong grid.
            D3DXTECHNIQUE_DESC tech = {};
            effect->GetTechniqueDesc(technique, &tech);
            D3DSURFACE_DESC desc = {};
            if (target)
                target->GetDesc(&desc);
            D3DVIEWPORT9 view = {};
            pDevice->GetViewport(&view);
            SSRTrace::Line("  draw %s: target %ux%u, viewport %ux%u at %u,%u, quad %.0fx%.0f", tech.Name ? tech.Name : "?",
                desc.Width, desc.Height, unsigned(view.Width), unsigned(view.Height), unsigned(view.X), unsigned(view.Y), width, height);
        }
        ProfilerScope timed(pDevice, profile);
        pDevice->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, screenVertices, sizeof(ScreenVertex));
        effect->EndPass();
        effect->End();
    }

    // Before deferred lighting, next to SSR: contact shadows towards the sun, smoothed into
    // ContactTex and accumulated over frames into ContactAccumTex, for deferred_lighting (see
    // BindLightingInputs). Independent of SSR being on.
    static void RenderContactShadows()
    {
        auto& R = PostFxResources;
        R.bContactValid = false;
        R.ContactResult = nullptr;
        // The history stays usable only if this frame accumulates too; any return below drops it.
        const bool accumWasValid = FrameHistory::CanReproject(R.nContactAccumFrame);
        R.nContactAccumFrame = 0;

        // For the light shaders, whatever becomes of the sun's pass below.
        {
            rage::grcViewport* camera = rage::GetCurrentViewport();
            // Only with every night shadow on in the menu (Extra Night Shadows with vehicle night
            // shadows): with fewer, cars have no night shadow for them to meet, only a dark halo.
            const bool local = camera && R.bLocalContactShadows && R.ContactShadowsEnabled() && R.AllNightShadowsEnabled() &&
                               R.fLocalContactShadowIntensity > 0.0f;
            const D3DMATRIX proj = camera ? *(const D3DMATRIX*)camera->mProjectionMatrix : D3DMATRIX{};
            const float consts[12] =
            {
                R.fLocalContactShadowLength, R.fLocalContactShadowThickness, R.fLocalContactShadowMaxDistance, R.fLocalContactShadowIntensity,
                proj._34, 0.0f, 0.0f, local ? 12345.0f : 0.0f,
                proj._11, proj._22, proj._31, proj._32,
            };
            memcpy(R.LocalContactShadowConsts, consts, sizeof(consts));

        }

        IDirect3DDevice9* pDevice = rage::grcDevice::GetD3DDevice();
        rage::grcViewport* vp = rage::GetCurrentViewport();
        auto& h = R.SSREffectHandles;
        ID3DXEffect* effect = R.SSREffect;
        if (!R.ContactShadowsEnabled() || R.fContactShadowIntensity <= 0.0f || !pDevice || !vp || !effect ||
            !h.techContactShadows || !R.mDepthRT || !R.ContactRawSurf || !R.ContactSurf)
            return;

        // gDirectionalLight is a RAGE global, and globals keep the same register in every
        // shader, so c17 holds the directional light the last lit draw used: the direction
        // the light travels, the reverse of the direction towards the sun.
        float light[4] = {};
        if (FAILED(pDevice->GetPixelShaderConstantF(17, light, 1)))
            return;
        const float lightLen = sqrtf(light[0] * light[0] + light[1] * light[1] + light[2] * light[2]);
        if (!(lightLen > 0.9f && lightLen < 1.1f))
            return;
        // With the sun below the horizon deferred_lighting, which lights by dot(n, -c17), shows
        // nothing, and the whole screen pass cost a frame or so for no shadow. The colour in c18
        // is no test: here it can still hold another shader's constant, and skipped the pass by day.
        if (-light[2] / lightLen <= 0.0f)
            return;

        const float width = float(RenderScale::ToRenderWidth(uint32_t(vp->mWidth)));
        const float height = float(RenderScale::ToRenderHeight(uint32_t(vp->mHeight)));
        const D3DMATRIX proj = *(D3DMATRIX*)vp->mProjectionMatrix;
        D3DXVECTOR4 toView[3];
        WorldToViewRows(vp, toView);
        D3DXVECTOR4 sun(0.0f, 0.0f, 0.0f, 1.0f);
        for (int row = 0; row < 3; ++row)
            (&sun.x)[row] = -(toView[row].x * light[0] + toView[row].y * light[1] + toView[row].z * light[2]) / lightLen;

        const bool hasNormals = R.mNormalRT && R.mNormalRT->mD3DTexture;
        effect->SetTexture(h.DepthTex2D, LightingDepth());
        if (hasNormals)
            effect->SetTexture(h.NormalTex2D, R.mNormalRT->mD3DTexture);
        effect->SetFloat(h.fUseGBufferNormals, (hasNormals && R.bSSRGBufferNormals) ? 1.0f : 0.0f);
        effect->SetVectorArray(h.vec4WaterToView, toView, 3);
        SetTargetSize(effect, h, proj, width, height);
        SetDepthRange(effect, h, vp->mNearClip, vp->mFarClip);
        effect->SetVector(h.vec4SunView, &sun);
        effect->SetFloat(h.fStepJitter, R.bContactShadowStepJitter ? 1.0f : 0.0f);
        const bool temporal = R.fContactTemporalBlend > 0.0f && h.techContactTemporal && R.ContactAccumSurf[0] &&
                              R.ContactAccumSurf[1];
        SetNoiseOffset(effect, temporal);
        effect->SetFloat(h.fCSLength, R.fContactShadowLength);
        effect->SetFloat(h.fCSThickness, R.fContactShadowThickness);
        effect->SetFloat(h.fCSMaxViewDistance, R.fContactShadowMaxDistance);
        effect->SetFloat(h.fCSIntensity, R.fContactShadowIntensity);

        IDirect3DSurface9* rt0 = nullptr;
        IDirect3DSurface9* ds = nullptr;
        IDirect3DVertexDeclaration9* oldDecl = nullptr;
        IDirect3DVertexBuffer9* oldVB = nullptr;
        UINT oldOffset = 0, oldStride = 0;
        DWORD oldFVF = 0;
        D3DVIEWPORT9 oldViewport;
        pDevice->GetFVF(&oldFVF);
        pDevice->GetVertexDeclaration(&oldDecl);
        pDevice->GetStreamSource(0, &oldVB, &oldOffset, &oldStride);
        pDevice->GetRenderTarget(0, &rt0);
        pDevice->GetDepthStencilSurface(&ds);
        pDevice->GetViewport(&oldViewport);

        IDirect3DBaseTexture9* oldTextures[kSSRTextureSlots] = {};
        DWORD savedRenderStates[std::size(kSSRRenderStates)] = {};
        DWORD savedSamplerStates[kSSRSamplerSlots][std::size(kSSRSamplerStates)] = {};
        for (DWORD slot = 0; slot < kSSRTextureSlots; ++slot)
            pDevice->GetTexture(slot, &oldTextures[slot]);
        pDevice->GetPixelShaderConstantF(0, savedPSConsts, kPSConstCount);
        pDevice->GetVertexShaderConstantF(0, savedVSConsts, kVSConstCount);
        for (size_t i = 0; i < std::size(kSSRRenderStates); ++i)
        {
            pDevice->GetRenderState(kSSRRenderStates[i].state, &savedRenderStates[i]);
            pDevice->SetRenderState(kSSRRenderStates[i].state, kSSRRenderStates[i].value);
        }
        for (DWORD slot = 0; slot < kSSRSamplerSlots; ++slot)
            for (size_t i = 0; i < std::size(kSSRSamplerStates); ++i)
            {
                pDevice->GetSamplerState(slot, kSSRSamplerStates[i].state, &savedSamplerStates[slot][i]);
                pDevice->SetSamplerState(slot, kSSRSamplerStates[i].state, kSSRSamplerStates[i].value);
            }

        pDevice->SetDepthStencilSurface(nullptr);
        pDevice->SetStreamSource(0, nullptr, 0, 0);
        pDevice->SetVertexDeclaration(nullptr);
        pDevice->SetFVF(D3DFVF_XYZRHW | D3DFVF_TEX1);
        D3DVIEWPORT9 vpDesc = {};
        vpDesc.Width = DWORD(width);
        vpDesc.Height = DWORD(height);
        vpDesc.MaxZ = 1.0f;
        pDevice->SetViewport(&vpDesc);

        // At half size the march, the smoothing and the accumulation run on the half size targets, with the pixel
        // size and reconstruction basis of that size, and the result is brought up to full size last, as for the
        // indirect light. Should the half size march target be missing, the full size march goes on its own: the
        // smoothing and accumulation targets are of the half size.
        const bool halfTargets = R.bContactShadowsHalfRes;
        const bool half = halfTargets && R.ContactRawHalfSurf && h.techContactUpsample;
        const bool smooth = half || !halfTargets;
        const float passWidth = half ? float(DWORD(width) / 2) : width;
        const float passHeight = half ? float(DWORD(height) / 2) : height;
        if (half)
            SetTargetSize(effect, h, proj, passWidth, passHeight);
        DrawEffectPass(pDevice, effect, h.techContactShadows, half ? R.ContactRawHalfSurf : R.ContactRawSurf, passWidth, passHeight, kProfContactMarch);

        // The same depth aware smoothing SSR uses; the raw result has alpha 1 everywhere, so
        // it is a plain weighted blur. The radius is in full size pixels.
        IDirect3DTexture9* result = half ? R.ContactRawHalfTex->mD3DTexture : R.ContactRawTex->mD3DTexture;
        if (smooth && R.fSSRDenoiseRadius > 0.0f && h.techSSRDenoise)
        {
            effect->SetTexture(h.SSRResultTex2D, result);
            effect->SetFloat(h.fDenoiseRadius, half ? R.fSSRDenoiseRadius * 0.5f : R.fSSRDenoiseRadius);
            effect->SetFloat(h.fDenoiseSSROnly, 0.0f);
            DrawEffectPass(pDevice, effect, h.techSSRDenoise, R.ContactSurf, passWidth, passHeight, kProfContactDenoise);
            result = R.ContactTex->mD3DTexture;
        }

        if (temporal && smooth)
        {
            // Without a history to reproject the blend is 0, any camera does
            D3DXMATRIX prevViewProj;
            if (accumWasValid)
                prevViewProj = FrameHistory::Previous().ViewProjection;
            else
                MatrixMultiply(prevViewProj, *(const D3DXMATRIX*)vp->mViewMatrix, *(const D3DXMATRIX*)vp->mProjectionMatrix);
            D3DXVECTOR4 reprojRows[4];
            ViewToClipRows(vp, prevViewProj, reprojRows);
            // Last frame's fog pass copied its depth; this frame's has not run yet.
            const bool prevDepth = R.PreAlphaDepthCopyRT && R.PreAlphaDepthCopyRT->mD3DTexture;

            const int prev = R.nContactAccumIndex, next = prev ^ 1;
            effect->SetTexture(h.SSRResultTex2D, result);
            effect->SetTexture(h.SSRAccumTex2D, R.ContactAccumTex[prev]->mD3DTexture);
            effect->SetTexture(h.PrevDepthTex2D, prevDepth ? R.PreAlphaDepthCopyRT->mD3DTexture : nullptr);
            effect->SetFloat(h.fUsePrevDepth, prevDepth ? 1.0f : 0.0f);
            SetPrevDepthRange(effect, h);
            effect->SetVectorArray(h.vec4ViewToPrevClip, reprojRows, 4);
            BindMotionVectors(effect, accumWasValid);
            effect->SetFloat(h.fTemporalBlend, accumWasValid ? R.fContactTemporalBlend : 0.0f);
            DrawEffectPass(pDevice, effect, h.techContactTemporal, R.ContactAccumSurf[next], passWidth, passHeight, kProfContactTemporal);

            result = R.ContactAccumTex[next]->mD3DTexture;
            R.nContactAccumIndex = next;
            R.nContactAccumFrame = FrameHistory::Frame();
        }

        if (half)
        {
            SetTargetSize(effect, h, proj, width, height);
            effect->SetTexture(h.SSRResultTex2D, result);
            DrawEffectPass(pDevice, effect, h.techContactUpsample, R.ContactRawSurf, width, height, kProfContactUpsample);
            result = R.ContactRawTex->mD3DTexture;
        }
        R.ContactResult = result;

        if (R.SSRDebugMode() == R.kContactDebugMode && R.SSRDebugSurf && h.techSSRDebug)
        {
            effect->SetTexture(h.SSRResultTex2D, result);
            effect->SetFloat(h.fDebugMode, float(R.kContactDebugMode));
            DrawEffectPass(pDevice, effect, h.techSSRDebug, R.SSRDebugSurf, width, height);
            R.bSSRDebugValid = true;
        }

        for (size_t i = 0; i < std::size(kSSRRenderStates); ++i)
            pDevice->SetRenderState(kSSRRenderStates[i].state, savedRenderStates[i]);
        for (DWORD slot = 0; slot < kSSRSamplerSlots; ++slot)
            for (size_t i = 0; i < std::size(kSSRSamplerStates); ++i)
                pDevice->SetSamplerState(slot, kSSRSamplerStates[i].state, savedSamplerStates[slot][i]);
        pDevice->SetPixelShaderConstantF(0, savedPSConsts, kPSConstCount);
        pDevice->SetVertexShaderConstantF(0, savedVSConsts, kVSConstCount);
        for (DWORD slot = 0; slot < kSSRTextureSlots; ++slot)
        {
            SetTextureBoth(pDevice, slot, oldTextures[slot]);
            SAFE_RELEASE(oldTextures[slot]);
        }
        pDevice->SetRenderTarget(0, rt0);
        pDevice->SetDepthStencilSurface(ds);
        pDevice->SetViewport(&oldViewport);
        pDevice->SetFVF(oldFVF);
        pDevice->SetVertexDeclaration(oldDecl);
        pDevice->SetStreamSource(0, oldVB, oldOffset, oldStride);
        SAFE_RELEASE(rt0);
        SAFE_RELEASE(ds);
        SAFE_RELEASE(oldDecl);
        SAFE_RELEASE(oldVB);

        R.bContactValid = true;
    }

    // Before deferred lighting, after contact shadows: indirect light at half resolution,
    // smoothed and accumulated, into GIResult for deferred_lighting. It reads the scene the fog
    // pass copied last frame, with the camera of last frame (FrameHistory).
    static void RenderIndirectLight()
    {
        auto& R = PostFxResources;
        // What deferred_lighting added last frame, still in its target, null if nothing.
        IDirect3DTexture9* prevGI = R.GIResult;
        R.GIResult = nullptr;

        IDirect3DDevice9* pDevice = rage::grcDevice::GetD3DDevice();
        rage::grcViewport* vp = rage::GetCurrentViewport();
        auto& h = R.SSREffectHandles;
        ID3DXEffect* effect = R.SSREffect;
        if (!R.SSGIEnabled() || R.fGIIntensity <= 0.0f || !pDevice || !vp || !effect || !h.techSSGI || !h.techSSRTemporal ||
            !R.mDepthRT || !R.SSRHistoryTex || !R.GIRawSurf || !R.GIDenoisedSurf || !R.GIAccumSurf[0] || !R.GIAccumSurf[1])
        {
            R.nGIAccumFrame = 0;
            return;
        }

        if (SSRTrace::Active())
        {
            const auto& cur = FrameHistory::Current();
            const auto& prv = FrameHistory::Previous();
            SSRTrace::Line("gi: vp near %.4f far %.1f; camera frame %u valid %d near %.4f far %.1f; previous frame %u valid %d near %.4f far %.1f; "
                           "cut %d; scene history of %u reprojects %d; accumulation of %u reprojects %d",
                           vp->mNearClip, vp->mFarClip, cur.Frame, int(cur.Valid), cur.Near, cur.Far, prv.Frame, int(prv.Valid), prv.Near, prv.Far,
                           int(FrameHistory::IsCameraCut()), R.nSSRHistoryFrame, int(FrameHistory::CanReproject(R.nSSRHistoryFrame)),
                           R.nGIAccumFrame, int(FrameHistory::CanReproject(R.nGIAccumFrame)));
            auto size = [](rage::grcRenderTargetPC* rt) -> std::string
            {
                D3DSURFACE_DESC d = {};
                if (!rt || !rt->mD3DTexture || FAILED(rt->mD3DTexture->GetLevelDesc(0, &d)))
                    return "none";
                return std::to_string(d.Width) + "x" + std::to_string(d.Height);
            };
            SSRTrace::Line("gi sizes: render scale active %d scale %.3f; game viewport %dx%d, screen %dx%d, ToRender %ux%u; depth %s, "
                           "depth copy %s, scene copy %s, history %s; GI raw %s denoised %s accum %s/%s full %s; SSR %s",
                           int(RenderScale::IsActive()), RenderScale::GetScale(), int(vp->mWidth), int(vp->mHeight),
                           rage::grcDevice::ms_nActiveWidth ? int(*rage::grcDevice::ms_nActiveWidth) : 0,
                           rage::grcDevice::ms_nActiveHeight ? int(*rage::grcDevice::ms_nActiveHeight) : 0,
                           RenderScale::ToRenderWidth(uint32_t(vp->mWidth)), RenderScale::ToRenderHeight(uint32_t(vp->mHeight)),
                           size(R.mDepthRT).c_str(), size(R.PreAlphaDepthCopyRT).c_str(), size(R.FullScreenTex_temp1).c_str(),
                           size(R.SSRHistoryTex).c_str(), size(R.GIRawTex).c_str(), size(R.GIDenoisedTex).c_str(),
                           size(R.GIAccumTex[0]).c_str(), size(R.GIAccumTex[1]).c_str(), size(R.GIFullTex).c_str(), size(R.SSRTex).c_str());
            SSRTrace::DepthProbe(pDevice, "gi depth", LightingDepth(), vp->mNearClip, vp->mFarClip);
            SSRTrace::DepthProbe(pDevice, "gi previous depth", R.PreAlphaDepthCopyRT ? R.PreAlphaDepthCopyRT->mD3DTexture : nullptr,
                                 prv.Near, prv.Far);
        }

        // The rays read last frame's scene: none on the first frame on, after a cut of the camera it shows another shot
        if (!FrameHistory::CanReproject(R.nSSRHistoryFrame))
        {
            R.nGIAccumFrame = 0;
            return;
        }
        D3DXVECTOR4 reprojRows[4];
        ViewToClipRows(vp, FrameHistory::Previous().ViewProjection, reprojRows);

        const float fullWidth = float(RenderScale::ToRenderWidth(uint32_t(vp->mWidth)));
        const float fullHeight = float(RenderScale::ToRenderHeight(uint32_t(vp->mHeight)));
        const float width = float(DWORD(fullWidth) / 2);
        const float height = float(DWORD(fullHeight) / 2);
        const D3DMATRIX proj = *(D3DMATRIX*)vp->mProjectionMatrix;
        D3DXVECTOR4 toView[3];
        WorldToViewRows(vp, toView);
        SetTargetSize(effect, h, proj, width, height);

        const bool hasNormals = R.mNormalRT && R.mNormalRT->mD3DTexture;
        const bool prevDepth = R.PreAlphaDepthCopyRT && R.PreAlphaDepthCopyRT->mD3DTexture;
        effect->SetTexture(h.DepthTex2D, LightingDepth());
        effect->SetTexture(h.HistoryTex2D, R.SSRHistoryTex->mD3DTexture);
        effect->SetTexture(h.PrevDepthTex2D, prevDepth ? R.PreAlphaDepthCopyRT->mD3DTexture : nullptr);
        effect->SetFloat(h.fUsePrevDepth, prevDepth ? 1.0f : 0.0f);
        SetPrevDepthRange(effect, h);
        if (hasNormals)
            effect->SetTexture(h.NormalTex2D, R.mNormalRT->mD3DTexture);
        effect->SetFloat(h.fUseGBufferNormals, (hasNormals && R.bSSRGBufferNormals) ? 1.0f : 0.0f);
        effect->SetVectorArray(h.vec4WaterToView, toView, 3);
        effect->SetVectorArray(h.vec4ViewToPrevClip, reprojRows, 4);
        SetDepthRange(effect, h, vp->mNearClip, vp->mFarClip);
        SetNoiseOffset(effect, R.fGITemporalBlend > 0.0f);
        effect->SetFloat(h.fGIRayLength, R.fGIRayLength);
        effect->SetFloat(h.fGIThickness, R.fGIThickness);
        effect->SetFloat(h.fGIMaxViewDistance, R.fGIMaxDistance);
        effect->SetFloat(h.fGIIntensity, R.fGIIntensity);
        effect->SetFloat(h.fGIMaxBrightness, R.fGIMaxBrightness);
        effect->SetFloat(h.fGIOcclusion, R.fGIOcclusion);
        // Last frame's scene holds the surfaces' colour times the indirect light at fGIIntensity;
        // the rays take all but an intensity 1 share of it back out, see SSGI_PS.
        const bool albedo = R.mDiffuseRT && R.mDiffuseRT->mD3DTexture;
        effect->SetTexture(h.AlbedoTex2D, albedo ? R.mDiffuseRT->mD3DTexture : nullptr);
        effect->SetTexture(h.AlbedoLinearTex2D, albedo ? R.mDiffuseRT->mD3DTexture : nullptr);
        effect->SetTexture(h.GIPrevTex2D, prevGI);
        // The occlusion deferred_lighting already takes off the ambient, so SSGI_PS does not take it off again.
        const bool specular = R.mSpecularRT && R.mSpecularRT->mD3DTexture;
        effect->SetTexture(h.SpecularTex2D, specular ? R.mSpecularRT->mD3DTexture : nullptr);
        effect->SetFloat(h.fGIRespectAO, specular ? 1.0f : 0.0f);
        effect->SetFloat(h.fGIFeedback, (albedo && prevGI) ? (std::max)(1.0f - 1.0f / R.fGIIntensity, 0.0f) : 0.0f);

        IDirect3DSurface9* rt0 = nullptr;
        IDirect3DSurface9* ds = nullptr;
        IDirect3DVertexDeclaration9* oldDecl = nullptr;
        IDirect3DVertexBuffer9* oldVB = nullptr;
        UINT oldOffset = 0, oldStride = 0;
        DWORD oldFVF = 0;
        D3DVIEWPORT9 oldViewport;
        pDevice->GetFVF(&oldFVF);
        pDevice->GetVertexDeclaration(&oldDecl);
        pDevice->GetStreamSource(0, &oldVB, &oldOffset, &oldStride);
        pDevice->GetRenderTarget(0, &rt0);
        pDevice->GetDepthStencilSurface(&ds);
        pDevice->GetViewport(&oldViewport);

        IDirect3DBaseTexture9* oldTextures[kSSRTextureSlots] = {};
        DWORD savedRenderStates[std::size(kSSRRenderStates)] = {};
        DWORD savedSamplerStates[kSSRSamplerSlots][std::size(kSSRSamplerStates)] = {};
        for (DWORD slot = 0; slot < kSSRTextureSlots; ++slot)
            pDevice->GetTexture(slot, &oldTextures[slot]);
        pDevice->GetPixelShaderConstantF(0, savedPSConsts, kPSConstCount);
        pDevice->GetVertexShaderConstantF(0, savedVSConsts, kVSConstCount);
        for (size_t i = 0; i < std::size(kSSRRenderStates); ++i)
        {
            pDevice->GetRenderState(kSSRRenderStates[i].state, &savedRenderStates[i]);
            pDevice->SetRenderState(kSSRRenderStates[i].state, kSSRRenderStates[i].value);
        }
        for (DWORD slot = 0; slot < kSSRSamplerSlots; ++slot)
            for (size_t i = 0; i < std::size(kSSRSamplerStates); ++i)
            {
                pDevice->GetSamplerState(slot, kSSRSamplerStates[i].state, &savedSamplerStates[slot][i]);
                pDevice->SetSamplerState(slot, kSSRSamplerStates[i].state, kSSRSamplerStates[i].value);
            }

        pDevice->SetDepthStencilSurface(nullptr);
        pDevice->SetStreamSource(0, nullptr, 0, 0);
        pDevice->SetVertexDeclaration(nullptr);
        pDevice->SetFVF(D3DFVF_XYZRHW | D3DFVF_TEX1);
        D3DVIEWPORT9 vpDesc = {};
        vpDesc.Width = DWORD(width);
        vpDesc.Height = DWORD(height);
        vpDesc.MaxZ = 1.0f;
        pDevice->SetViewport(&vpDesc);

        DrawEffectPass(pDevice, effect, h.techSSGI, R.GIRawSurf, width, height, kProfGIMarch);

        // The same depth aware smoothing SSR uses; the gather has alpha 1 everywhere, so it is a
        // plain weighted blur. The radius is in full size pixels.
        IDirect3DTexture9* gathered = R.GIRawTex->mD3DTexture;
        if (R.fSSRDenoiseRadius > 0.0f && h.techSSRDenoise)
        {
            effect->SetTexture(h.SSRResultTex2D, gathered);
            effect->SetFloat(h.fDenoiseRadius, R.fSSRDenoiseRadius * 0.5f);
            effect->SetFloat(h.fDenoiseSSROnly, 0.0f);
            DrawEffectPass(pDevice, effect, h.techSSRDenoise, R.GIDenoisedSurf, width, height, kProfGIDenoise);
            gathered = R.GIDenoisedTex->mD3DTexture;
        }

        const int prev = R.nGIAccumIndex, next = prev ^ 1;
        effect->SetTexture(h.SSRResultTex2D, gathered);
        effect->SetTexture(h.SSRAccumTex2D, R.GIAccumTex[prev]->mD3DTexture);
        const bool history = FrameHistory::CanReproject(R.nGIAccumFrame);
        BindMotionVectors(effect, history);
        effect->SetFloat(h.fTemporalBlend, history ? R.fGITemporalBlend : 0.0f);
        effect->SetFloat(h.fTemporalAnySurface, 1.0f);
        // The SSR pass left the menu's debug view in fDebugMode; view 10 would turn this pass into its own.
        effect->SetFloat(h.fDebugMode, 0.0f);
        DrawEffectPass(pDevice, effect, h.techSSRTemporal, R.GIAccumSurf[next], width, height, kProfGITemporal);
        // View 10 runs the accumulation again into whichever half size target it does not read.
        IDirect3DTexture9* historyDebug = nullptr;
        if (R.SSRDebugMode() == R.kGIHistoryDebugMode && R.SSRDebugSurf && h.techSSRDebug)
        {
            const bool rawFree = gathered != R.GIRawTex->mD3DTexture;
            // While the trace runs, the numbers behind the colours (see GIHistoryDebug in SSR.fx), read back here.
            effect->SetFloat(h.fDebugMode, SSRTrace::Active() ? float(R.kGIHistoryDebugMode) + 0.75f : float(R.kGIHistoryDebugMode));
            DrawEffectPass(pDevice, effect, h.techSSRTemporal, rawFree ? R.GIRawSurf : R.GIDenoisedSurf, width, height);
            effect->SetFloat(h.fDebugMode, 0.0f);
            historyDebug = rawFree ? R.GIRawTex->mD3DTexture : R.GIDenoisedTex->mD3DTexture;
            SSRTrace::Line("gi history debug: %.0fx%.0f, z now / last frame's copy / expected through last frame's camera / flags", width, height);
            SSRTrace::PixelProbe(pDevice, "gi history", historyDebug);
        }
        effect->SetFloat(h.fTemporalAnySurface, 0.0f);
        R.nGIAccumIndex = next;
        R.nGIAccumFrame = FrameHistory::Frame();
        R.GIResult = R.GIAccumTex[next]->mD3DTexture;

        vpDesc.Width = DWORD(fullWidth);
        vpDesc.Height = DWORD(fullHeight);
        pDevice->SetViewport(&vpDesc);
        SetTargetSize(effect, h, proj, fullWidth, fullHeight);
        if (h.techGIUpsample && R.GIFullSurf)
        {
            effect->SetTexture(h.SSRResultTex2D, R.GIResult);
            DrawEffectPass(pDevice, effect, h.techGIUpsample, R.GIFullSurf, fullWidth, fullHeight, kProfGIUpsample);
            R.GIResult = R.GIFullTex->mD3DTexture;
        }

        if ((R.SSRDebugMode() == R.kGIDebugMode || historyDebug) && R.SSRDebugSurf && h.techSSRDebug)
        {
            effect->SetTexture(h.SSRResultTex2D, historyDebug ? historyDebug : R.GIResult);
            effect->SetFloat(h.fDebugMode, float(historyDebug ? R.kGIHistoryDebugMode : R.kGIDebugMode));
            DrawEffectPass(pDevice, effect, h.techSSRDebug, R.SSRDebugSurf, fullWidth, fullHeight);
            R.bSSRDebugValid = true;
        }

        for (size_t i = 0; i < std::size(kSSRRenderStates); ++i)
            pDevice->SetRenderState(kSSRRenderStates[i].state, savedRenderStates[i]);
        for (DWORD slot = 0; slot < kSSRSamplerSlots; ++slot)
            for (size_t i = 0; i < std::size(kSSRSamplerStates); ++i)
                pDevice->SetSamplerState(slot, kSSRSamplerStates[i].state, savedSamplerStates[slot][i]);
        pDevice->SetPixelShaderConstantF(0, savedPSConsts, kPSConstCount);
        pDevice->SetVertexShaderConstantF(0, savedVSConsts, kVSConstCount);
        for (DWORD slot = 0; slot < kSSRTextureSlots; ++slot)
        {
            SetTextureBoth(pDevice, slot, oldTextures[slot]);
            SAFE_RELEASE(oldTextures[slot]);
        }
        pDevice->SetRenderTarget(0, rt0);
        pDevice->SetDepthStencilSurface(ds);
        pDevice->SetViewport(&oldViewport);
        pDevice->SetFVF(oldFVF);
        pDevice->SetVertexDeclaration(oldDecl);
        pDevice->SetStreamSource(0, oldVB, oldOffset, oldStride);
        SAFE_RELEASE(rt0);
        SAFE_RELEASE(ds);
        SAFE_RELEASE(oldDecl);
        SAFE_RELEASE(oldVB);
    }

    // As the fog pass begins, once every light is drawn: scatters the light under the skin of
    // scene, the lit scene the fog pass reads (see SkinLight_PS in SSR.fx), and returns the scene
    // with it, or null when it does not run. SSR Debug 9 shows what counts as skin.
    static IDirect3DBaseTexture9* RenderSkinScattering(IDirect3DDevice9* pDevice, IDirect3DBaseTexture9* scene)
    {
        auto& R = PostFxResources;
        auto& h = R.SSREffectHandles;
        ID3DXEffect* effect = R.SSREffect;
        const bool scatter = R.SkinScatteringEnabled() && R.fSkinScatteringStrength > 0.0f;
        const bool debug = R.SSRDebugMode() == R.kSkinDebugMode && R.SSRDebugSurf && h.techSkinDebug;
        if ((!scatter && !debug) || !pDevice || !effect || !scene || scene->GetType() != D3DRTYPE_TEXTURE ||
            !h.techSkinLight || !h.techSkinScatter || !h.techSkinScatterFinal || !R.mMaterialIdRT || !R.mMaterialIdRT->mD3DTexture ||
            !R.mDiffuseRT || !R.mDiffuseRT->mD3DTexture || !R.mDepthRT || !R.mDepthRT->mD3DTexture ||
            !R.SkinLightSurf[0] || !R.SkinLightSurf[1] || R.SkinCamera[2] <= 0.0f)
            return nullptr;

        D3DSURFACE_DESC sceneDesc = {}, lightDesc = {};
        if (FAILED(static_cast<IDirect3DTexture9*>(scene)->GetLevelDesc(0, &sceneDesc)) || FAILED(R.SkinLightSurf[0]->GetDesc(&lightDesc)) ||
            sceneDesc.Width != lightDesc.Width || sceneDesc.Height != lightDesc.Height)
            return nullptr;
        const float width = float(lightDesc.Width);
        const float height = float(lightDesc.Height);

        effect->SetTexture(h.SceneTex2D, scene);
        effect->SetTexture(h.SkinIDTex2D, R.mMaterialIdRT->mD3DTexture);
        effect->SetTexture(h.AlbedoTex2D, R.mDiffuseRT->mD3DTexture);
        effect->SetTexture(h.DepthTex2D, LightingDepth());
        SetDepthRange(effect, h, R.SkinCamera[2], R.SkinCamera[3]);
        effect->SetFloat(h.fSkinStrength, R.fSkinScatteringStrength);

        IDirect3DSurface9* rt0 = nullptr;
        IDirect3DSurface9* ds = nullptr;
        IDirect3DVertexDeclaration9* oldDecl = nullptr;
        IDirect3DVertexBuffer9* oldVB = nullptr;
        UINT oldOffset = 0, oldStride = 0;
        DWORD oldFVF = 0;
        D3DVIEWPORT9 oldViewport;
        pDevice->GetFVF(&oldFVF);
        pDevice->GetVertexDeclaration(&oldDecl);
        pDevice->GetStreamSource(0, &oldVB, &oldOffset, &oldStride);
        pDevice->GetRenderTarget(0, &rt0);
        pDevice->GetDepthStencilSurface(&ds);
        pDevice->GetViewport(&oldViewport);

        IDirect3DBaseTexture9* oldTextures[kSSRTextureSlots] = {};
        DWORD savedRenderStates[std::size(kSSRRenderStates)] = {};
        DWORD savedSamplerStates[kSSRSamplerSlots][std::size(kSSRSamplerStates)] = {};
        for (DWORD slot = 0; slot < kSSRTextureSlots; ++slot)
            pDevice->GetTexture(slot, &oldTextures[slot]);
        pDevice->GetPixelShaderConstantF(0, savedPSConsts, kPSConstCount);
        pDevice->GetVertexShaderConstantF(0, savedVSConsts, kVSConstCount);
        for (size_t i = 0; i < std::size(kSSRRenderStates); ++i)
        {
            pDevice->GetRenderState(kSSRRenderStates[i].state, &savedRenderStates[i]);
            pDevice->SetRenderState(kSSRRenderStates[i].state, kSSRRenderStates[i].value);
        }
        for (DWORD slot = 0; slot < kSSRSamplerSlots; ++slot)
            for (size_t i = 0; i < std::size(kSSRSamplerStates); ++i)
            {
                pDevice->GetSamplerState(slot, kSSRSamplerStates[i].state, &savedSamplerStates[slot][i]);
                pDevice->SetSamplerState(slot, kSSRSamplerStates[i].state, kSSRSamplerStates[i].value);
            }

        pDevice->SetDepthStencilSurface(nullptr);
        pDevice->SetStreamSource(0, nullptr, 0, 0);
        pDevice->SetVertexDeclaration(nullptr);
        pDevice->SetFVF(D3DFVF_XYZRHW | D3DFVF_TEX1);
        D3DVIEWPORT9 vpDesc = {};
        vpDesc.Width = DWORD(width);
        vpDesc.Height = DWORD(height);
        vpDesc.MaxZ = 1.0f;
        pDevice->SetViewport(&vpDesc);

        IDirect3DBaseTexture9* result = nullptr;
        if (scatter)
        {
            DrawEffectPass(pDevice, effect, h.techSkinLight, R.SkinLightSurf[0], width, height, kProfSkinLight);
            // A kernel unit is half SkinScatteringWidth, and a metre at view depth 1 spans _11 / 2
            // of the screen across and _22 / 2 down.
            const float unit = R.fSkinScatteringWidth * 0.25f;
            D3DXVECTOR4 step(R.SkinCamera[0] * unit, 0.0f, 0.0f, R.fSkinScatteringWidth * 0.5f);
            effect->SetVector(h.vec4SkinStep, &step);
            effect->SetTexture(h.SkinLightTex2D, R.SkinLightTex[0]->mD3DTexture);
            DrawEffectPass(pDevice, effect, h.techSkinScatter, R.SkinLightSurf[1], width, height, kProfSkinScatter);
            step = D3DXVECTOR4(0.0f, R.SkinCamera[1] * unit, 0.0f, R.fSkinScatteringWidth * 0.5f);
            effect->SetVector(h.vec4SkinStep, &step);
            effect->SetTexture(h.SkinLightTex2D, R.SkinLightTex[1]->mD3DTexture);
            DrawEffectPass(pDevice, effect, h.techSkinScatterFinal, R.SkinLightSurf[0], width, height, kProfSkinFinal);
            result = R.SkinLightTex[0]->mD3DTexture;
        }
        SSRTrace::Line("skin: scatter %d, near %.4f far %.1f", int(scatter), R.SkinCamera[2], R.SkinCamera[3]);

        if (debug)
        {
            effect->SetTexture(h.SkinLightTex2D, result ? result : scene);
            DrawEffectPass(pDevice, effect, h.techSkinDebug, R.SSRDebugSurf, width, height);
            R.bSSRDebugValid = true;
        }

        for (size_t i = 0; i < std::size(kSSRRenderStates); ++i)
            pDevice->SetRenderState(kSSRRenderStates[i].state, savedRenderStates[i]);
        for (DWORD slot = 0; slot < kSSRSamplerSlots; ++slot)
            for (size_t i = 0; i < std::size(kSSRSamplerStates); ++i)
                pDevice->SetSamplerState(slot, kSSRSamplerStates[i].state, savedSamplerStates[slot][i]);
        pDevice->SetPixelShaderConstantF(0, savedPSConsts, kPSConstCount);
        pDevice->SetVertexShaderConstantF(0, savedVSConsts, kVSConstCount);
        for (DWORD slot = 0; slot < kSSRTextureSlots; ++slot)
        {
            SetTextureBoth(pDevice, slot, oldTextures[slot]);
            SAFE_RELEASE(oldTextures[slot]);
        }
        pDevice->SetRenderTarget(0, rt0);
        pDevice->SetDepthStencilSurface(ds);
        pDevice->SetViewport(&oldViewport);
        pDevice->SetFVF(oldFVF);
        pDevice->SetVertexDeclaration(oldDecl);
        pDevice->SetStreamSource(0, oldVB, oldOffset, oldStride);
        SAFE_RELEASE(rt0);
        SAFE_RELEASE(ds);
        SAFE_RELEASE(oldDecl);
        SAFE_RELEASE(oldVB);
        return result;
    }

    // The game's light shaft loop (CE 0xAC2A09) draws a shaft with the shadowed shaft technique
    // when a shadow map is found for its light. Headlights nearly always have one there, and a
    // shaft drawn with it shows nothing, so unless VolumetricLightHeadlightShadow is on, the draw
    // hook clears that choice for lights with the headlight flag and they take the plain one.
    static inline SafetyHookMid shShaftDraw{};
    // A headlight's cone is round and aimed level, so the upper half of its shaft rose metres
    // above the road. Before the loop draws a shaft (CE 0xAC4580) it copies the light's direction
    // and tangent to its stack; for headlights the aim hook turns those copies down by
    // VolumetricLightHeadlightPitch, about the level axis across the beam. The light itself, which
    // lights the road and casts the headlight's shadow, keeps its aim.
    static inline SafetyHookMid shShaftAim{};

    // Traffic lights and fires (0x200) get no shadow map, so their contact shadow stood alone: at
    // night, in the shadow of a street light, the light of a traffic light coloured the ground
    // right up to a car, and its contact shadow cut a black band along the car into that colour.
    // At the top of the loop that draws each of the frame's lights (CE 0xAC10B7, edi the light
    // + 0x28, its flags at edi + 0x20), contact shadows go off for those lights and back on after.
    static inline SafetyHookMid shLocalContactLight{};

    // c206 for the light about to be drawn: a headlight's (a spot light of 8 m or more with the
    // vehicle flag, as InstallShaftHooks tells them) the car's level right and half its lamps'
    // spacing, from which the shaders take its highlight as one from each lamp; other lights none.
    // Only the beam of both lamps is split: the game draws a car's two lamps as one light between
    // them with the "headlights" projected texture (CE 0xA3E070), and a lamp left alone by a broken
    // one as a light of its own at that lamp with none (0xA3DE90), whose highlight stays one.
    static bool IsTwinHeadlight(const rage::CLightSource& light)
    {
        return light.mProjTexHash != 0;
    }

    static void SetLightGGXShape(const rage::CLightSource& light)
    {
        auto& R = PostFxResources;
        float shape[4] = {};
        if (R.GGXLightingEnabled() && R.fLightsGGX > 0.0f && R.fLightsGGXHeadlights > 0.0f && light.mType == rage::LT_SPOT &&
            (light.mFlags & rage::LF_VEHICLE) && light.mRadius >= 8.0f)
        {
            // dir x up, level
            const float kx = light.mDirection.y, ky = -light.mDirection.x;
            const float len = std::sqrt(kx * kx + ky * ky);
            if (len > 1e-3f)
            {
                shape[0] = kx / len;
                shape[1] = ky / len;
                shape[3] = IsTwinHeadlight(light) ? R.fLightsGGXHeadlights : 0.0f;
                ++R.nLightGGXHeadlights;
            }
        }
        ++R.nLightGGXLights;
        if (std::memcmp(shape, R.LightGGXShape, sizeof(shape)) == 0)
            return;
        auto pDevice = rage::grcDevice::GetD3DDevice();
        if (!pDevice)
            return;
        std::memcpy(R.LightGGXShape, shape, sizeof(shape));
        pDevice->SetPixelShaderConstantF(206, shape, 1);
    }

    // A headlight the light loop is about to draw, as SetLightGGXShape tells them, kept for its glints.
    static void CollectGlintLight(const rage::CLightSource& light)
    {
        auto& R = PostFxResources;
        if (R.nGlintCandidates >= R.kGlintCandidates || light.mType != rage::LT_SPOT || !(light.mFlags & rage::LF_VEHICLE) ||
            light.mRadius < 8.0f || light.mIntensity <= 0.0f)
            return;
        auto& g = R.GlintCandidates[R.nGlintCandidates++];
        g.position[0] = light.mPosition.x;
        g.position[1] = light.mPosition.y;
        g.position[2] = light.mPosition.z;
        g.direction[0] = light.mDirection.x;
        g.direction[1] = light.mDirection.y;
        g.direction[2] = light.mDirection.z;
        g.colour[0] = light.mColor.x * light.mIntensity;
        g.colour[1] = light.mColor.y * light.mIntensity;
        g.colour[2] = light.mColor.z * light.mIntensity;
        g.radius = light.mRadius;
        g.distance = 0.0f;
        g.twin = IsTwinHeadlight(light);
        // What the game gives headlights, for the log: the ranges of intensity and outer cone.
        if (R.nGlintCandidates == 1)
        {
            R.GlintsLastIntensity[0] = R.GlintsLastIntensity[1] = light.mIntensity;
            R.GlintsLastCone[0] = R.GlintsLastCone[1] = light.mOuterConeAngle;
        }
        R.GlintsLastIntensity[0] = (std::min)(R.GlintsLastIntensity[0], light.mIntensity);
        R.GlintsLastIntensity[1] = (std::max)(R.GlintsLastIntensity[1], light.mIntensity);
        R.GlintsLastCone[0] = (std::min)(R.GlintsLastCone[0], light.mOuterConeAngle);
        R.GlintsLastCone[1] = (std::max)(R.GlintsLastCone[1], light.mOuterConeAngle);
    }

    // VehicleBoxShadows: a light whose shadow map holds no cars (no dynamic slot this frame: lamps lit from their cache
    // or casting no shadows, most headlights) left the cars under it without a shadow, with only the dark frame of the
    // contact shadow round them. Each car is taken as a box with rounded edges, which the light shaders shadow
    // (shaders/patches/local_light_vehicle_box_shadows.patch, c143-c149). The boxes are taken on the main thread as the
    // game hands the frame's light list over (CE 0xac2dd0) and kept with that list, so the render thread shades a frame
    // with the cars where they stood when its lights were made.
    struct VehicleBoxShadows
    {
        struct Box
        {
            float centre[3], halfLength;
            float forward[2], halfWidth, halfHeight;
            uintptr_t vehicle;
        };
        static constexpr int kBoxes = 160, kPerLight = 3, kLists = 3;
        struct List
        {
            std::atomic<uintptr_t> lights{0}; // the light list these boxes go with
            int count = 0;
            float focus[3] = {};              // the player, where the boxes' shadows are seen
            Box boxes[kBoxes];
        };
        static inline List lists[kLists]{};
        static inline int nextList = 0;
        static inline uintptr_t* lightListBuilt = nullptr; // CE 0x103eedc, the list the main thread fills
        static inline uintptr_t* lightListDrawn = nullptr; // CE 0x103eed0, the list the render thread draws
        static inline uintptr_t modelInfos = 0;             // CE 0x1295cd8, model infos by index
        static inline SafetyHookMid shHandOver{};
        // For the log: frames drawn with their boxes found or not, lights shaded with some.
        static inline std::atomic<uint32_t> framesMatched{0}, framesUnmatched{0}, lightsWithBoxes{0};
        static inline const List* drawn = nullptr;
        static inline uintptr_t drawnFor = 0;
        static inline bool constantsOn = true;
        // The cars each lamp took last, render thread.
        // What each lamp took last, render thread: its cars, and since when its own map shows them.
        struct LampState
        {
            std::array<uintptr_t, kPerLight> cars{};
            bool mapped = false;
            DWORD since = 0;
        };
        static inline std::unordered_map<uint64_t, LampState> held;
        // For the log: the nearest car's model bounds (least, most) and its box, as taken.
        static inline float nearestBounds[6] = {}, nearestBox[4] = {}, nearestDistance = -1.0f;

        // Main thread: every car's box, the nearest the player first when there are more than fit.
        static void Capture()
        {
            auto& list = lists[nextList];
            nextList = (nextList + 1) % kLists;
            list.lights.store(0, std::memory_order_release);
            list.count = 0;
            if (!PostFxResources.VehicleBoxShadowsEnabled() || !lightListBuilt || !modelInfos)
                return;
            float focus[3] = {};
            if (!CPlayer::getLocalPlayerPed || !CEntity::GetPosition(CPlayer::getLocalPlayerPed(), focus))
                return;
            static std::array<std::pair<float, Box>, 256> found{};
            int n = 0;
            float nearest = 1.0e30f;
            CVehicle::ForEachVehicle([&](uintptr_t vehicle)
            {
                if (n >= int(found.size()))
                    return;
                const float* m = CEntity::GetMatrix(vehicle);
                const auto index = *reinterpret_cast<const int16_t*>(vehicle + 0x2E);
                if (!m || index < 0 || index >= 31000)
                    return;
                const auto info = *reinterpret_cast<const uintptr_t*>(modelInfos + index * 4);
                if (!info)
                    return;
                // The model's bounds (+0x20 least, +0x30 most, as GET_MODEL_DIMENSIONS reads them): x right, y forward, z up.
                const auto lo = reinterpret_cast<const float*>(info + 0x20), hi = reinterpret_cast<const float*>(info + 0x30);
                const float halfWidth = (hi[0] - lo[0]) * 0.5f, halfLength = (hi[1] - lo[1]) * 0.5f;
                // From the wheels' bottom, so the shadow always meets the car where its contact shadow is: the ground
                // under the car lies in the box and is shadowed whole (only a car's or ped's pixel above the box's foot
                // escapes it, by the material IDs).
                const float bottom = lo[2];
                const float halfHeight = (hi[2] - bottom) * 0.5f;
                // Cars, vans, buses and bikes; not helicopters with their rotors, nor anything broken.
                if (!(halfWidth > 0.1f && halfWidth < 2.0f && halfLength > 0.2f && halfLength < 10.0f && halfHeight > 0.1f && halfHeight < 2.5f))
                    return;
                const float flat = std::sqrt(m[4] * m[4] + m[5] * m[5]);
                if (!(flat > 0.5f))
                    return;
                const float local[3] = { (lo[0] + hi[0]) * 0.5f, (lo[1] + hi[1]) * 0.5f, (bottom + hi[2]) * 0.5f };
                Box box{};
                for (int i = 0; i < 3; ++i)
                    box.centre[i] = m[12 + i] + m[i] * local[0] + m[4 + i] * local[1] + m[8 + i] * local[2];
                if (!std::isfinite(box.centre[0]) || !std::isfinite(box.centre[1]) || !std::isfinite(box.centre[2]))
                    return;
                box.forward[0] = m[4] / flat;
                box.forward[1] = m[5] / flat;
                box.halfLength = halfLength;
                box.halfWidth = halfWidth;
                box.halfHeight = halfHeight;
                box.vehicle = vehicle;
                const float x = box.centre[0] - focus[0], y = box.centre[1] - focus[1], z = box.centre[2] - focus[2];
                const float d2 = x * x + y * y + z * z;
                if (d2 < nearest)
                {
                    nearest = d2;
                    std::copy_n(lo, 3, nearestBounds);
                    std::copy_n(hi, 3, nearestBounds + 3);
                    nearestBox[0] = box.halfLength; nearestBox[1] = box.halfWidth; nearestBox[2] = box.halfHeight;
                    nearestBox[3] = box.centre[2] - m[14]; // the box's middle above the car's position
                }
                found[n++] = { d2, box };
            });
            if (n > kBoxes)
                std::nth_element(found.begin(), found.begin() + kBoxes, found.begin() + n,
                    [](const auto& a, const auto& b) { return a.first < b.first; });
            nearestDistance = n ? std::sqrt(nearest) : -1.0f;
            std::copy_n(focus, 3, list.focus);
            list.count = (std::min)(n, kBoxes);
            for (int i = 0; i < list.count; ++i)
                list.boxes[i] = found[i].second;
            list.lights.store(*lightListBuilt, std::memory_order_release);
        }

        static void Install()
        {
            // The hand over of the frame's light list: mov ecx, [index]; xor eax, eax; inc ecx; cmp ecx, 3; cmove ecx, eax;
            // mov eax, [the list filled]; and where the render thread takes a list: add esi, lists; mov [drawn], esi
            auto pattern = hook::pattern("8B 0D ? ? ? ? 33 C0 41 83 F9 03 0F 44 C8 A1 ? ? ? ? A3");
            auto drawnPattern = hook::pattern("81 C6 ? ? ? ? 89 35 ? ? ? ? 8B 49 0C");
            if (pattern.empty() || drawnPattern.empty())
                return;
            lightListBuilt = *pattern.get_first<uintptr_t*>(16);
            lightListDrawn = *drawnPattern.get_first<uintptr_t*>(8);
            modelInfos = GameBase() + 0xE95CD8;
            shHandOver = safetyhook::create_mid(pattern.get_first(0), [](SafetyHookContext&) { Capture(); });
        }

        // Render thread, for the light about to be drawn: up to three cars its light reaches, the nearest the light first,
        // without the car the light belongs to; none where the light's own map shows the cars.
        static bool SetForLight(IDirect3DDevice9* pDevice, const rage::CLightSource& light, bool mapHasCars)
        {
            auto& R = PostFxResources;
            float constants[7][4] = {};
            int used = 0;
            float strength = 0.0f;
            if (R.VehicleBoxShadowsEnabled() && lightListDrawn &&
                (R.bVehicleBoxShadowsFromCarLights || !(light.mFlags & rage::LF_VEHICLE)) &&
                (light.mType == rage::LT_POINT || light.mType == rage::LT_SPOT || light.mType == rage::LT_CLAMPED))
            {
                const uintptr_t list = *lightListDrawn;
                // Looked up again once its entry has been reused for a later list too.
                if (list != drawnFor || (drawn && drawn->lights.load(std::memory_order_acquire) != list))
                {
                    drawnFor = list;
                    drawn = nullptr;
                    for (auto& l : lists)
                        if (l.lights.load(std::memory_order_acquire) == list)
                            drawn = &l;
                    (drawn ? framesMatched : framesUnmatched).fetch_add(1, std::memory_order_relaxed);
                }
                // Which three: the nearest the light, weighed by how near the player they stand, where their shadows
                // are seen; those the light took last frame are held at half that, so a car passing by the lamp does
                // not push a standing one out, and its shadow from that lamp flickered. Lamps are told apart by where
                // they stand, a quarter metre apart.
                const uint64_t lightKey = (uint64_t(uint32_t(int32_t(std::floor(light.mPosition.x * 4.0f))) & 0x1FFFFF) << 42) |
                    (uint64_t(uint32_t(int32_t(std::floor(light.mPosition.y * 4.0f))) & 0x1FFFFF) << 21) |
                    uint64_t(uint32_t(int32_t(std::floor(light.mPosition.z * 4.0f))) & 0x1FFFFF);
                if (held.size() > 4096)
                    held.clear();
                auto& state = held[lightKey];
                auto& last = state.cars;
                // A lamp whose own map shows the cars (a shadow slot) has the real shadow: its boxes fade out over
                // VehicleBoxShadowsSlotFade after it takes the slot, so the shadow does not jump; when it loses the
                // slot they are back at once, before the real one is gone. VehicleBoxShadowsWithSlots keeps them.
                strength = 1.0f;
                if (mapHasCars)
                {
                    if (!state.mapped)
                        state.since = GetTickCount();
                    if (!R.bVehicleBoxShadowsWithSlots)
                        strength = R.nVehicleBoxShadowSlotFadeMs ?
                            1.0f - float(GetTickCount() - state.since) / float(R.nVehicleBoxShadowSlotFadeMs) : 0.0f;
                }
                state.mapped = mapHasCars;
                std::pair<float, const Box*> nearest[kPerLight] = {};
                for (int i = 0; drawn && strength > 0.0f && i < drawn->count; ++i)
                {
                    const auto& box = drawn->boxes[i];
                    const float dx = light.mPosition.x - box.centre[0], dy = light.mPosition.y - box.centre[1], dz = light.mPosition.z - box.centre[2];
                    const float reach = light.mRadius + box.halfLength + box.halfWidth + box.halfHeight;
                    const float d2 = dx * dx + dy * dy + dz * dz;
                    if (d2 > reach * reach)
                        continue;
                    const float fx = box.centre[0] - drawn->focus[0], fy = box.centre[1] - drawn->focus[1], fz = box.centre[2] - drawn->focus[2];
                    float score = d2 * (1.0f + (fx * fx + fy * fy + fz * fz) / 400.0f);
                    if (std::find(last.begin(), last.end(), box.vehicle) != last.end())
                        score *= 0.5f;
                    if (used == kPerLight && score >= nearest[kPerLight - 1].first)
                        continue;
                    // A lamp no higher than the car's roof: real lamps stand above cars, but some in tunnels light from
                    // the road or walls with no lamp there, and a box's shadow from them spread over the whole road.
                    if (!(light.mFlags & rage::LF_VEHICLE) && dz < box.halfHeight + 0.3f)
                    {
                        NoteLowLight(light, box, dz);
                        continue;
                    }
                    // A light within its own car (headlights, tail lights) is not shadowed by it.
                    const float along = dx * box.forward[0] + dy * box.forward[1], across = dx * box.forward[1] - dy * box.forward[0];
                    if (std::abs(along) < box.halfLength + 0.5f && std::abs(across) < box.halfWidth + 0.5f && std::abs(dz) < box.halfHeight + 0.5f)
                        continue;
                    int at = used < kPerLight ? used++ : kPerLight - 1;
                    for (; at > 0 && nearest[at - 1].first > score; --at)
                        nearest[at] = nearest[at - 1];
                    nearest[at] = { score, &box };
                }
                last = {};
                for (int i = 0; i < used; ++i)
                    last[i] = nearest[i].second->vehicle;
                // The model's bounds take in mirrors, aerials and bumpers, and the roof's height runs over the bonnet and
                // boot too: the box is that much smaller, its bottom kept where it is.
                const float rounding = R.fVehicleBoxShadowRounding;
                const auto& scale = R.fVehicleBoxShadowScale;
                for (int i = 0; i < used; ++i)
                {
                    const auto& box = *nearest[i].second;
                    const float halfHeight = box.halfHeight * scale[2];
                    // The lower the light, the longer the shadow, and the plainer its box's square shape: across the way
                    // the light comes, the box narrows down to VehicleBoxShadowsLowLightNarrow of itself (the length for a
                    // light to the side, the width for one ahead or behind); a light overhead keeps it whole.
                    float along = 1.0f, across = 1.0f;
                    {
                        const float dx = light.mPosition.x - box.centre[0], dy = light.mPosition.y - box.centre[1], dz = light.mPosition.z - box.centre[2];
                        const float flat = std::sqrt(dx * dx + dy * dy), d = std::sqrt(flat * flat + dz * dz);
                        if (flat > 0.01f && d > 0.01f)
                        {
                            const float t = std::clamp((dz / d - 0.3f) / 0.6f, 0.0f, 1.0f);
                            const float narrow = 1.0f - (1.0f - R.fVehicleBoxShadowLowLightNarrow) * (1.0f - t * t * (3.0f - 2.0f * t));
                            const float a = std::abs(dx * box.forward[0] + dy * box.forward[1]) / flat; // 1: ahead or behind
                            along = 1.0f + (narrow - 1.0f) * (1.0f - a);
                            across = 1.0f + (narrow - 1.0f) * a;
                        }
                    }
                    float* c = constants[i * 2];
                    c[0] = box.centre[0]; c[1] = box.centre[1]; c[2] = box.centre[2] - (box.halfHeight - halfHeight);
                    c[3] = (std::max)(box.halfLength * scale[0] * along - rounding, 0.05f);
                    c[4] = box.forward[0]; c[5] = box.forward[1];
                    c[6] = (std::max)(box.halfWidth * scale[1] * across - rounding, 0.05f);
                    c[7] = (std::max)(halfHeight - rounding, 0.05f);
                }
            }
            if (!used)
            {
                if (constantsOn)
                    Off(pDevice);
                return false;
            }
            lightsWithBoxes.fetch_add(1, std::memory_order_relaxed);
            // The boxes left over stand far away, small.
            for (int i = used; i < kPerLight; ++i)
            {
                float* c = constants[i * 2];
                c[0] = c[1] = c[2] = 1.0e6f;
                c[3] = c[6] = c[7] = 0.05f;
                c[4] = 1.0f;
            }
            constants[6][0] = R.fVehicleBoxShadowLightSize;
            constants[6][1] = R.fVehicleBoxShadowRounding;
            // A car's or ped's pixel nearer a box than this is on that car or in it, which its box does not shadow: the
            // box is smaller than the car and rounded, so roof, bonnet and spoiler stand out of it.
            constants[6][2] = R.fVehicleBoxShadowSelfMargin;
            constants[6][3] = (std::min)(strength, 1.0f);
            pDevice->SetPixelShaderConstantF(143, constants[0], 7);
            constantsOn = true;
            return true;
        }

        // For the log: lamps whose boxes were left out as they stand no higher than the roof, and the last of them.
        static inline std::atomic<uint32_t> lowLights{0};
        static inline rage::CLightSource lastLowLight{};
        static inline float lastLowHeight = 0.0f;
        static void NoteLowLight(const rage::CLightSource& light, const Box& box, float aboveMiddle)
        {
            lowLights.fetch_add(1, std::memory_order_relaxed);
            lastLowLight = light;
            lastLowHeight = aboveMiddle + box.halfHeight; // above the box's bottom
        }

        // Before and after the main view's lights: lights drawn for other views (reflections, mirrors) take none.
        static void Off(IDirect3DDevice9* pDevice)
        {
            const float off[4] = {};
            pDevice->SetPixelShaderConstantF(149, off, 1);
            constantsOn = false;
        }
    };

    // Whether the game has a shadow map for this light this frame, read as its own lookup does (CE 0x925db0, which
    // deferred lighting calls for each light; called again here it would clear a cache entry's flag): off unless
    // [0x1036780]; buffer [0x1174794]; a cached map where (buffer * 16 + cache index) * 0x100 + 0x119d1d0 is set;
    // a dynamic one in the 7 keys at 0x119fc08 + buffer * 0x880, 0x110 apart. Without the layout, by the flags alone
    static inline uintptr_t ShadowLookupBase = 0;
    // Only a dynamic slot counts. A cached map holds the static world only; with CacheWithCars it holds cars too, but
    // it is redrawn by turns, so a car that just drove under the lamp is not in it yet, and its contact shadow stood
    // alone under street lamps all the same.
    static bool HasShadowMap(const rage::CLightSource& light)
    {
        if (!(light.mFlags & (rage::LF_STATIC_SHADOW | rage::LF_DYNAMIC_SHADOW)))
            return false;
        if (!ShadowLookupBase)
            return true;
        const auto at = [](uintptr_t address) { return ShadowLookupBase + address - 0x400000; };
        if (!*reinterpret_cast<const uint8_t*>(at(0x1036780)))
            return false;
        const int buffer = *reinterpret_cast<const int*>(at(0x1174794));
        if (buffer < 0 || buffer > 1)
            return true;
        if (light.mFlags & rage::LF_DYNAMIC_SHADOW)
            for (int i = 0; i < 7; i++)
                if (*reinterpret_cast<const uint32_t*>(at(0x119FC08) + buffer * 0x880 + i * 0x110) == uint32_t(light.mCastShadows))
                    return true;
        return false;
    }

    static void InstallLocalContactLightHook()
    {
        auto pattern = hook::pattern("83 C7 28 89 7C 24 1C 8B 47 1C 85 C0");
        if (pattern.empty())
            return;
        if (auto base = GameBase(); shadow_lookup_layout::Validate(base))
            ShadowLookupBase = base;
        shLocalContactLight = safetyhook::create_mid(pattern.get_first(7), [](SafetyHookContext& regs)
        {
            auto& R = PostFxResources;
            if (!R.bLocalContactPass)
                return;
            if (nLightingStage == 1)
                SetLightingStage(rage::grcDevice::GetD3DDevice(), 2);
            const auto& light = *reinterpret_cast<const rage::CLightSource*>(regs.edi - 0x28);
            SetLightGGXShape(light);
            CollectGlintLight(light);
            auto pDevice = rage::grcDevice::GetD3DDevice();
            if (!pDevice)
                return;
            const bool mapped = HasShadowMap(light);
            const bool boxed = VehicleBoxShadows::SetForLight(pDevice, light, mapped);
            if (R.LocalContactShadowConsts[7] == 0.0f)
                return;
            // A contact shadow only belongs where a shadow shows the car: the light's own map in a dynamic slot, or
            // the boxes of the cars it reaches. Elsewhere (no slot, lit from its cache, lights casting no shadows) it
            // stood alone, a dark frame around a car with no shadow, and LocalContactShadowsWithoutShadowMap applies. In a
            // room scene, lights inside only (0x20 without 0x40) that cast no shadows at all keep theirs, as most in
            // buildings do. Tunnel lamps are flagged inside too, and there the frame ran along the car from back to front
            // under every lamp it passed.
            const bool insideOnly = R.bInteriorScene && (light.mFlags & 0x60) == 0x20 &&
                !(light.mFlags & (rage::LF_STATIC_SHADOW | rage::LF_DYNAMIC_SHADOW));
            const float intensity = (light.mFlags & 0x200) ? 0.0f
                : R.fLocalContactShadowUnshadowed >= 1.0f || insideOnly || mapped || boxed
                    ? R.fLocalContactShadowIntensity
                    : R.fLocalContactShadowIntensity * R.fLocalContactShadowUnshadowed;
            const bool off = intensity <= 0.0f;
            if (!off && intensity != R.fLocalContactLightIntensity)
            {
                float consts[4] = { R.LocalContactShadowConsts[0], R.LocalContactShadowConsts[1], R.LocalContactShadowConsts[2], intensity };
                pDevice->SetPixelShaderConstantF(202, consts, 1);
                R.fLocalContactLightIntensity = intensity;
            }
            if (off == R.bLocalContactLightOff)
                return;
            R.bLocalContactLightOff = off;
            const float none[4] = {};
            pDevice->SetPixelShaderConstantF(203, off ? none : &R.LocalContactShadowConsts[4], 1);
        });
    }

    // The start of the game's light shaft loop (CE 0xac29cd, the shaft count read), for the profiler.
    static inline SafetyHookMid shShaftLoopStart{};

    static void InstallShaftLoopProfiler()
    {
        auto pattern = hook::pattern("8B 35 ? ? ? ? 89 74 24 38 E8 ? ? ? ? 50 E8 ? ? ? ? 83 C4 08 80 7D 08 00 74 0E FF 35 ? ? ? ? E8 ? ? ? ? 83 C4 04 85 F6 0F 8E");
        if (pattern.empty())
            return;
        shShaftLoopStart = safetyhook::create_mid(pattern.get_first(0), [](SafetyHookContext&)
        {
            auto pDevice = rage::grcDevice::GetD3DDevice();
            if (nLightingStage > 0)
                SetLightingStage(pDevice, 3);
            // Every light of the main view is drawn by now, and its target is still the lit scene.
            auto& R = PostFxResources;
            if (R.bLocalContactPass && !R.bGlintsDone)
            {
                R.bGlintsDone = true;
                RenderHeadlightGlints(pDevice);
            }
        });
    }

    static void InstallShaftHooks()
    {
        auto& R = PostFxResources;
        if (!R.nVolumetricLightHeadlightFlag)
            return;
        if (!R.bVolumetricLightHeadlightShadow)
        {
            auto pattern = hook::pattern("A1 ? ? ? ? 0F 28 05 ? ? ? ? 0F 29 84 24 A0 00 00 00 F3 0F 10 44 06 70");
            if (!pattern.empty())
            {
                static uintptr_t* list = *pattern.get_first<uintptr_t*>(1);
                shShaftDraw = safetyhook::create_mid(pattern.get_first(0), [](SafetyHookContext& regs)
                {
                    const auto& light = *reinterpret_cast<const rage::CLightSource*>(*list + regs.esi);
                    if (PostFxResources.VolumetricLight() && (light.mFlags & PostFxResources.nVolumetricLightHeadlightFlag))
                        *reinterpret_cast<uint8_t*>(regs.esp + 0xF) = 0;
                });
            }
        }
        if (R.fVolumetricLightHeadlightPitch != 0.0f)
        {
            // The copies of direction (esp + 0xF0) and tangent (esp + 0xE0), then push 0; the
            // hook sits on the load of the outer cone angle right after, eax the light list and
            // esi this light's offset in it.
            auto pattern = hook::pattern("F3 0F 10 04 06 F3 0F 11 84 24 F0 00 00 00 F3 0F 10 44 06 04 F3 0F 11 84 24 F4 00 00 00 "
                                         "F3 0F 10 44 06 08 F3 0F 11 84 24 F8 00 00 00 F3 0F 10 44 06 10 F3 0F 11 84 24 E0 00 00 00 "
                                         "F3 0F 10 44 06 14 F3 0F 11 84 24 E4 00 00 00 F3 0F 10 44 06 18 6A 00 "
                                         "F3 0F 11 84 24 EC 00 00 00 F3 0F 10 6C 06 5C");
            if (!pattern.empty())
            {
                static const float pitch = R.fVolumetricLightHeadlightPitch * 3.14159265f / 180.0f;
                static const float c = std::cos(pitch), s = -std::sin(pitch);
                shShaftAim = safetyhook::create_mid(pattern.get_first(0x5B), [](SafetyHookContext& regs)
                {
                    const auto& light = *reinterpret_cast<const rage::CLightSource*>(regs.eax + regs.esi);
                    if (!PostFxResources.VolumetricLight() || !(light.mFlags & PostFxResources.nVolumetricLightHeadlightFlag))
                        return;
                    // 4 bytes further off esp for the push 0.
                    float* dir = reinterpret_cast<float*>(regs.esp + 0xF4);
                    float* tangent = reinterpret_cast<float*>(regs.esp + 0xE4);
                    // The level axis across the beam, dir x up.
                    float kx = dir[1], ky = -dir[0];
                    const float len = std::sqrt(kx * kx + ky * ky);
                    if (len < 1e-3f)
                        return; // aimed straight up or down
                    kx /= len;
                    ky /= len;
                    // Rodrigues: v cos + (k x v) sin + k (k . v)(1 - cos), turning down.
                    auto turn = [&](float* v)
                    {
                        const float kv = kx * v[0] + ky * v[1];
                        const float cx = ky * v[2], cy = -kx * v[2], cz = kx * v[1] - ky * v[0];
                        v[0] = v[0] * c + cx * s + kx * kv * (1.0f - c);
                        v[1] = v[1] * c + cy * s + ky * kv * (1.0f - c);
                        v[2] = v[2] * c + cz * s;
                    };
                    turn(dir);
                    turn(tangent);
                });
            }
        }
    }

    // Most people's heads and every player's hands are drawn with gta_ped, the shader of clothes,
    // which cannot tell skin in a pixel; the component loops of the ped draw can. Each turn of
    // the loop (CE 0xAB7C20 for component peds, 0xAB7EB0 the other) sets c150.x for its
    // component: a quarter step for HEAD and HAND, which gta_ped and gta_ped_reflect add to the
    // material ID as the skin shaders do (ped_skin_scattering_mask.patch), 0 for the rest. FACE,
    // the last turn, leaves it at 0 for whatever is drawn next.
    static inline SafetyHookMid shPedSkinComponent[2]{};

    static void MarkPedSkin(uintptr_t component)
    {
        auto& R = PostFxResources;
        const bool on = R.SkinScatteringEnabled() || R.SSRDebugMode() == R.kSkinDebugMode;
        const float mark[4] = { (on && (component == 0 || component == 4)) ? 0.25f / 255.0f : 0.0f, 0.0f, 0.0f, 0.0f };
        if (auto pDevice = rage::grcDevice::GetD3DDevice())
            pDevice->SetPixelShaderConstantF(150, mark, 1);
    }

    static void InstallPedSkinHooks()
    {
        auto pattern = hook::pattern("8B 44 24 24 8B B6 28 01 00 00 0F B6 7C 28 5C 0F B7 46 0C");
        if (!pattern.empty())
            shPedSkinComponent[0] = safetyhook::create_mid(pattern.get_first(0), [](SafetyHookContext& regs) { MarkPedSkin(regs.ebp); });
        pattern = hook::pattern("80 78 5C 00 74 09 83 FE 03 0F 84 ? ? ? ? 80 BE ? ? ? ? 00 0F 84");
        if (!pattern.empty())
            shPedSkinComponent[1] = safetyhook::create_mid(pattern.get_first(0), [](SafetyHookContext& regs) { MarkPedSkin(regs.esi); });
    }

    // Runs as the game copies each light into the frame's draw list, on the main thread.
    static void OnAfterCopyLight(rage::CLightSource* light)
    {
        auto& R = PostFxResources;
        if (!light)
            return;
        // Building Fill Lights off: the large exterior map lights, no interior, vehicle, traffic
        // light or fire, that flood whole squares go dark.
        constexpr uint32_t fillMask = rage::LF_MAP | rage::LF_INTERIOR | rage::LF_EXTERIOR | rage::LF_VEHICLE | rage::LF_TRAFFIC;
        if (!R.bFillLights && (light->mFlags & fillMask) == (rage::LF_MAP | rage::LF_EXTERIOR) &&
            (light->mType == rage::LT_POINT || light->mType == rage::LT_SPOT) && light->mRadius >= R.fFillLightsMinRadius)
            light->mIntensity = 0.0f;
        if (!R.VolumetricLight() || R.fVolumetricLightIntensity <= 0.0f)
            return;

        float cameraPos[3]{};
        GameCamera::Position(cameraPos);
        const float dx = cameraPos[0] - light->mPosition.x;
        const float dy = cameraPos[1] - light->mPosition.y;
        const float dz = cameraPos[2] - light->mPosition.z;
        const float distance = std::sqrt(dx * dx + dy * dy + dz * dz);

        // Spot lights of 8 to 20 m, most of lamppost.img, none of a vehicle, traffic light, the
        // unculled source or LF_10, and none with a shaft already; headlights are the spot lights
        // of at least 8 m with the headlight flag (smaller ones are tail and brake lights),
        // whatever their other flags.
        if (light->mType != rage::LT_SPOT || (light->mFlags & rage::LF_SHAFT))
            return;
        const uint32_t headlightFlag = R.nVolumetricLightHeadlightFlag;
        const bool headlight = headlightFlag && (light->mFlags & headlightFlag) && light->mRadius >= 8.0f &&
                               R.fVolumetricLightHeadlightIntensity > 0.0f;
        constexpr uint32_t noShaft = rage::LF_TRAFFIC | rage::LF_VEHICLE | rage::LF_UNCULLED | rage::LF_10 | rage::LF_SHAFT;
        if (!headlight && (light->mRadius < 8.0f || light->mRadius > 20.0f || (light->mFlags & noShaft)))
            return;
        const float fadeStart = R.fVolumetricLightMaxDistance * 0.3f;
        const float x = std::clamp((distance - fadeStart) / (R.fVolumetricLightMaxDistance - fadeStart), 0.0f, 1.0f);
        const float fade = 1.0f - x * x * (3.0f - 2.0f * x);
        if (fade <= 0.0f)
            return;

        light->mFlags |= rage::LF_SHAFT;
        if (headlight)
            light->mFlags |= R.nVolumetricLightHeadlightAddFlags;
        light->mVolumeIntensity = (headlight ? R.fVolumetricLightHeadlightIntensity : R.fVolumetricLightIntensity) * fade;
        // The shaft mesh reaches about 0.66 to 0.85 of radius times scale (light shaft VS), so
        // headlights take a scale that gives the same length to low beams of 33 m and high
        // beams of 75 to 98 m, never past their own reach.
        light->mVolumeScale = headlight ? (std::min)(R.fVolumetricLightHeadlightLength / light->mRadius, 1.0f)
                                        : R.fVolumetricLightScale;
    }

public:
    // What deferred_lighting reads besides the game's own inputs: s3 the SSR result (the cleared
    // SSR target while SSR is off, else a transparent 1x1), s9 the contact shadows while they
    // are valid, s8 the indirect light, c202-c204 the local light contact shadow constants, and
    // c201, c205 and s11 (the material IDs) the light on skin.
    static void BindLightingInputs(IDirect3DDevice9* pDevice)
    {
        auto& R = PostFxResources;
        // gDirectionalLight is a RAGE global, the same register in every shader: the direction the
        // sun's (or moon's) light travels. The clouds take the reverse of it.
        {
            float light[4] = {};
            if (SUCCEEDED(pDevice->GetPixelShaderConstantF(17, light, 1)))
            {
                const float len = std::sqrt(light[0] * light[0] + light[1] * light[1] + light[2] * light[2]);
                if (len > 0.9f && len < 1.1f)
                {
                    for (int i = 0; i < 3; ++i)
                        R.CloudLightDir[i] = -light[i] / len;
                    R.nCloudLightFrame = FrameHistory::Frame();
                }
            }
        }
        // s9 is read by no game shader, and the car glass takes it over right after lighting.
        if (R.bContactValid && R.ContactResult)
        {
            BindSampler(pDevice, 9, R.ContactResult, D3DTEXF_POINT);
            R.bContactBound = true;
        }
        else if (R.bContactBound)
        {
            SetTextureBoth(pDevice, 9, nullptr);
            R.bContactBound = false;
        }
        pDevice->SetPixelShaderConstantF(202, R.LocalContactShadowConsts, 3);
        R.bLocalContactPass = true;
        VehicleBoxShadows::Off(pDevice);
        R.bLocalContactLightOff = false;
        R.fLocalContactLightIntensity = R.LocalContactShadowConsts[3];

        // The sun on skin: c201 the scale of the N.L curve less 1, c205 its offset and the red penumbra.
        {
            const float k = R.SkinScatteringEnabled() ? R.fSkinLighting : 0.0f;
            const float wrap[3] = { 0.35f * k, 0.25f * k, 0.2f * k };
            float scale[4] = {}, offset[4] = {};
            for (int i = 0; i < 3; ++i)
            {
                scale[i] = 1.0f / (1.0f + wrap[i]) - 1.0f;
                offset[i] = wrap[i] / (1.0f + wrap[i]);
            }
            offset[3] = (std::min)(0.2f * k, 1.0f);
            pDevice->SetPixelShaderConstantF(201, scale, 1);
            pDevice->SetPixelShaderConstantF(205, offset, 1);
        }
        // The GGX highlights. c200: x the strength (0 the game's own), halved since the shaders
        // divide by twice the visibility's denominator, y the Fresnel rise, z the lights' radius
        // squared, w the lobe's stretch along the light, squared. c165: the fill lights' share,
        // the sun's (0 its own highlight), the environment BRDF's blend, the highlight's ceiling. c206, the headlights'
        // spacing, is set per light (InstallLocalContactLightHook); s13 the G-buffer's specular for
        // the fill lights, read by no game shader while the lights are drawn.
        {
            const bool enabled = R.GGXLightingEnabled();
            const bool on = enabled && R.fLightsGGX > 0.0f;
            const float stretch = 1.0f + R.fLightsGGXStretch;
            const float c200[4] = { on ? R.fLightsGGX * 0.5f : 0.0f, R.fLightsGGXFresnel, R.fLightsGGXSize * R.fLightsGGXSize, stretch * stretch };
            const float c165[4] = { on ? R.fLightsGGXFillLights : 0.0f, on ? R.fLightsGGXSun : 0.0f, enabled ? R.fLightsGGXEnvironment : 0.0f, R.fLightsGGXMax };
            pDevice->SetPixelShaderConstantF(200, c200, 1);
            pDevice->SetPixelShaderConstantF(165, c165, 1);
            // c183: alpha squared = w / (x n + 2), the peak times y + z alpha squared; c184: x a widening
            // added to alpha squared, y how far the stretch turns from the light to the view.
            const float soft = R.fLightsGGXSoft;
            const float c183[4] = { 4.0f - 3.0f * soft, 1.0f - soft, 32.0f * soft, 2.0f + 1.38f * soft };
            const float c184[4] = { 0.04f * soft, R.fLightsGGXStretchView, 0.0f, 0.0f };
            pDevice->SetPixelShaderConstantF(183, c183, 1);
            pDevice->SetPixelShaderConstantF(184, c184, 1);
            std::memset(R.LightGGXShape, 0, sizeof(R.LightGGXShape));
            pDevice->SetPixelShaderConstantF(206, R.LightGGXShape, 1);
            R.nGlintCandidates = 0;
            R.bGlintsDone = false;
            if (R.mSpecularRT && R.mSpecularRT->mD3DTexture)
            {
                BindSampler(pDevice, 13, R.mSpecularRT->mD3DTexture, D3DTEXF_POINT);
                R.bSpecularBound = true;
            }
        }
        // The sun on materials with no specular map (x) and the cloud shadows (yzw, c198, c199, s12).
        {
            float threshold = 0.0f, bias = 0.0f, thickness = 0.0f;
            if (R.bCloudParamsRegistered)
            {
                threshold = rage::grmShaderInfo::getShaderParamData(R.CloudThresholdIdx)[0];
                bias = rage::grmShaderInfo::getShaderParamData(R.CloudBiasIdx)[0];
                thickness = rage::grmShaderInfo::getShaderParamData(R.CloudThicknessIdx)[0];
            }
            // Until the sky has been drawn once its parameters read zero, which is no cloud at all.
            R.bCloudLastFromGame = !(threshold == 0.0f && bias == 0.0f);
            if (threshold == 0.0f && bias == 0.0f)
            {
                threshold = 1.6f;
                bias = 0.6f;
            }
            if (thickness <= 0.0f)
                thickness = 1.0f;
            float coverageShift = R.fCloudShadowsCoverage;
            float deckHeight = R.fCloudShadowsHeight;
            const double seconds = CTimer::m_snTimeInMilliseconds ? *CTimer::m_snTimeInMilliseconds * 0.001 : 0.0;
            R.UpdateCloudLayer(seconds);
            // The game's flat clouds under the volumetric ones, by VolumetricCloudsVanilla:
            // gta_atmoscatt_clouds draws them where CloudThreshold * noise - CloudBias is above
            // 0, so threshold * v and bias towards 1 by 1 - v fade them out at 0. Its high layer
            // stays.
            if (R.bCloudParamsRegistered)
            {
                const bool fade = R.VolumetricCloudsOn() && R.fVolumetricCloudsVanilla < 1.0f;
                const float v = R.fVolumetricCloudsVanilla;
                auto& thresholdOverride = rage::grmShaderInfo::getShaderParamOverride(R.CloudThresholdIdx);
                auto& biasOverride = rage::grmShaderInfo::getShaderParamOverride(R.CloudBiasIdx);
                thresholdOverride.mul = { v, 1.0f, 1.0f, 1.0f };
                thresholdOverride.add = {};
                biasOverride.mul = { v, 1.0f, 1.0f, 1.0f };
                biasOverride.add = { 1.0f - v, 0.0f, 0.0f, 0.0f };
                thresholdOverride.on = fade;
                biasOverride.on = fade;
            }
            // The shadows work out the clouds' coverage as Clouds.fx does (deferred_lighting_sun_under_clouds
            // patch): from the volumetric clouds' layer while they are on, otherwise from CloudShadowsCoverage
            // about a cover of 0.4 at CloudShadowsHeight. The game's threshold, bias and thickness are only
            // logged.
            float cover = std::clamp(0.4f + coverageShift, 0.02f, 1.0f);
            if (R.VolumetricCloudsOn())
            {
                // An overcast sheet's evened coverage is left out: under it the sun is weak anyway.
                cover = (std::max)(R.Cloud.coverage, 0.02f);
                deckHeight = R.Cloud.base + R.Cloud.thickness * 0.33f;
            }
            R.fCloudLastThreshold = threshold;
            R.fCloudLastBias = bias;
            R.fCloudLastThickness = thickness;

            auto noise = (R.fCloudShadows > 0.0f || R.VolumetricCloudsOn()) ? R.CloudNoiseTex() : nullptr;
            const float strength = noise ? R.fCloudShadows : 0.0f; // 0 leaves the shadows out
            const float invScale = 1.0f / R.fCloudShadowsScale;
            // The wind blows the same way all the time; only how far it has carried the noise
            // changes, wrapped to one tile so the offset keeps its precision.
            const float windX = static_cast<float>(std::fmod(R.fCloudDrift * 0.93, 1.0));
            const float windY = static_cast<float>(std::fmod(R.fCloudDrift * 0.37, 1.0));

            R.fCloudWindX = windX;
            R.fCloudWindY = windY;
            R.fCloudSeconds = seconds;
            float c197[4] = { R.fSpecularSheen, strength, deckHeight, invScale };
            float c198[4] = { windX, windY, cover, R.kCloudWeatherReach };
            float c199[4] = { 0.0f, R.fCloudShadowsSoftness, R.CloudMorphPhase(), R.kCloudMorphReach / R.fCloudShadowsScale };
            if (R.nCloudShadowsDebug == 1)
            {
                // Full cover everywhere.
                c199[0] = 1.0f;
            }
            else if (R.nCloudShadowsDebug == 2)
            {
                // Cover 1, so the coverage is close to the map itself, twenty times finer, so its
                // blotches show around the player; at the clouds' scale they spanned hundreds of
                // metres and the ground looked evenly lit.
                c198[2] = 1.0f;
                c198[3] = 0.0f;
                c197[3] *= 20.0f;
            }
            pDevice->SetPixelShaderConstantF(197, c197, 1);
            pDevice->SetPixelShaderConstantF(198, c198, 1);
            pDevice->SetPixelShaderConstantF(199, c199, 1);
            std::memcpy(R.CloudShadowConsts, c197, sizeof(c197));
            std::memcpy(R.CloudShadowConsts + 4, c198, sizeof(c198));
            std::memcpy(R.CloudShadowConsts + 8, c199, sizeof(c199));
            if (noise)
            {
                // s12 is read only by G-buffer and particle shaders, none of which draw while the
                // lights do. On s7, rage_postfx's, the sun read another texture and the shadows
                // never showed.
                // Every state: the G-buffer pass leaves texturequality's detail texture states on s12
                // (kDetailStage), and the sun read zero from the noise although it was bound.
                DWORD srgb = 0, maxMip = 0, minFilter = 0, lodBias = 0;
                pDevice->GetSamplerState(12, D3DSAMP_SRGBTEXTURE, &srgb);
                pDevice->GetSamplerState(12, D3DSAMP_MAXMIPLEVEL, &maxMip);
                pDevice->GetSamplerState(12, D3DSAMP_MINFILTER, &minFilter);
                pDevice->GetSamplerState(12, D3DSAMP_MIPMAPLODBIAS, &lodBias);
                R.CloudSamplerBefore[0] = srgb;
                R.CloudSamplerBefore[1] = maxMip;
                R.CloudSamplerBefore[2] = minFilter;
                R.CloudSamplerBefore[3] = lodBias;
                SetTextureBoth(pDevice, 12, noise);
                pDevice->SetSamplerState(12, D3DSAMP_ADDRESSU, D3DTADDRESS_WRAP);
                pDevice->SetSamplerState(12, D3DSAMP_ADDRESSV, D3DTADDRESS_WRAP);
                pDevice->SetSamplerState(12, D3DSAMP_ADDRESSW, D3DTADDRESS_WRAP);
                pDevice->SetSamplerState(12, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
                pDevice->SetSamplerState(12, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
                pDevice->SetSamplerState(12, D3DSAMP_MIPFILTER, D3DTEXF_LINEAR);
                pDevice->SetSamplerState(12, D3DSAMP_MAXMIPLEVEL, 0);
                pDevice->SetSamplerState(12, D3DSAMP_MIPMAPLODBIAS, 0);
                pDevice->SetSamplerState(12, D3DSAMP_MAXANISOTROPY, 1);
                pDevice->SetSamplerState(12, D3DSAMP_SRGBTEXTURE, FALSE);
                R.bCloudNoiseBound = true;
            }
        }
        // The light volumes do not read the material IDs themselves (local_light_on_skin.patch).
        // s11 is read by no game shader, and the car glass takes it over right after lighting;
        // s10 is the game's StippleTexture, which the final post fx pass reads as its colour LUT.
        if (R.mMaterialIdRT && R.mMaterialIdRT->mD3DTexture)
        {
            BindSampler(pDevice, 11, R.mMaterialIdRT->mD3DTexture, D3DTEXF_POINT);
            R.bMaterialIdBound = true;
        }

        // Indirect light, black while there is none; unbound after lighting.
        BindSampler(pDevice, 8, R.GIResult ? static_cast<IDirect3DBaseTexture9*>(R.GIResult) : R.TransparentTex(), D3DTEXF_LINEAR);
        R.bGIBound = true;

        IDirect3DBaseTexture9* tex = R.TransparentTex();
        if (R.SSRResult)
            tex = R.SSRResult;
        else if (R.SSRTex && R.SSRTex->mD3DTexture)
            tex = R.SSRTex->mD3DTexture; // cleared while SSR is off
        SetTextureBoth(pDevice, 3, tex);
        SSRTrace::Line("bind for lighting: s3 %s, ssr valid this frame %d", SSRTrace::TextureName(tex).c_str(), int(R.bSSRValidThisFrame));
        pDevice->SetSamplerState(3, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
        pDevice->SetSamplerState(3, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
        pDevice->SetSamplerState(3, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
        pDevice->SetSamplerState(3, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
        pDevice->SetSamplerState(3, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
    }

    // Runs as the first command of the lighting phase's list (OnBuildRenderList), before the
    // command in that list that renders AO, SSR and contact shadows, so what it binds is last
    // frame's. For the accumulated SSR that was the other target of the pair, and lighting
    // showed last frame's reflections, which swung off the car while the camera turned and came
    // back when it stopped; the contact shadows' validity and constants were a frame old too.
    // RenderPedAndVehicleFakeShadows binds this frame's once they are done.
    static void BindSSRTexture()
    {
        if (auto pDevice = rage::grcDevice::GetD3DDevice())
        {
            nLightingStage = 0; // a frame that never closed its lighting leaves nothing open here
            ProfilerMark(pDevice, kProfLighting, true);
            BindLightingInputs(pDevice);
        }
    }

    static void BindSampler(IDirect3DDevice9* pDevice, DWORD slot, IDirect3DBaseTexture9* tex, DWORD filter)
    {
        SetTextureBoth(pDevice, slot, tex);
        SetSamplerStateBoth(pDevice, slot, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
        SetSamplerStateBoth(pDevice, slot, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
        SetSamplerStateBoth(pDevice, slot, D3DSAMP_MAGFILTER, filter);
        SetSamplerStateBoth(pDevice, slot, D3DSAMP_MINFILTER, filter);
        SetSamplerStateBoth(pDevice, slot, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
    }

    // Right after deferred lighting: binds this frame's camera, and the textures the fog pass
    // copies the depth and the lit opaque scene into, for the patched car glass shaders. Anything missing
    // leaves s9 empty, and the glass keeps the game's environment map.
    static void PrepareGlassReflections()
    {
        auto& R = PostFxResources;
        auto pDevice = rage::grcDevice::GetD3DDevice();
        if (!pDevice)
            return;
        SetLightingStage(pDevice, 0);
        ProfilerMark(pDevice, kProfLighting, false);

        if (R.bContactBound)
        {
            SetTextureBoth(pDevice, 9, nullptr);
            R.bContactBound = false;
        }
        if (R.bGIBound)
        {
            SetTextureBoth(pDevice, 8, nullptr);
            R.bGIBound = false;
        }
        if (R.bMaterialIdBound)
        {
            SetTextureBoth(pDevice, 11, nullptr);
            R.bMaterialIdBound = false;
        }
        if (R.bSpecularBound)
        {
            SetTextureBoth(pDevice, 13, nullptr);
            R.bSpecularBound = false;
        }
        if (R.bCloudNoiseBound)
        {
            // Whether the noise was still there once the lights were drawn, for the log.
            IDirect3DBaseTexture9* bound = nullptr;
            pDevice->GetTexture(12, &bound);
            R.bCloudNoiseSurvived = bound && bound == R.CloudNoiseTexture;
            SAFE_RELEASE(bound);
            SetTextureBoth(pDevice, 12, nullptr);
            R.bCloudNoiseBound = false;
        }
        // Lights drawn for other views (reflections, mirrors) must not march with this camera,
        // nor light skin by this view's material IDs.
        const float noLocalContactShadows[4] = {};
        pDevice->SetPixelShaderConstantF(203, noLocalContactShadows, 1);
        R.bLocalContactPass = false;
        VehicleBoxShadows::Off(pDevice);
        pDevice->SetPixelShaderConstantF(201, noLocalContactShadows, 1);
        pDevice->SetPixelShaderConstantF(205, noLocalContactShadows, 1);
        pDevice->SetPixelShaderConstantF(197, noLocalContactShadows, 1);
        pDevice->SetPixelShaderConstantF(200, noLocalContactShadows, 1);
        pDevice->SetPixelShaderConstantF(165, noLocalContactShadows, 1);

        bool ok = R.bGlassFrameValid && R.bGlassReflections && R.SSREnabled() && R.PreAlphaDepthCopyRT &&
                  R.PreAlphaDepthCopyRT->mD3DTexture && R.SSRHistoryTex && R.SSRHistoryTex->mD3DTexture;
        R.bGlassFrameValid = false;

        if (ok && !R.GlassParamsTex)
            ok = SUCCEEDED(pDevice->CreateTexture(5, 1, 1, D3DUSAGE_DYNAMIC, D3DFMT_A32B32G32R32F, D3DPOOL_DEFAULT, &R.GlassParamsTex, nullptr));
        if (ok)
        {
            D3DLOCKED_RECT locked = {};
            ok = SUCCEEDED(R.GlassParamsTex->LockRect(0, &locked, nullptr, D3DLOCK_DISCARD));
            if (ok)
            {
                // A32B32G32R32F stores each texel as r, g, b, a floats.
                memcpy(locked.pBits, R.GlassParams, sizeof(R.GlassParams));
                R.GlassParamsTex->UnlockRect(0);
            }
        }

        if (!ok)
        {
            UnbindGlassReflections();
            return;
        }

        BindSampler(pDevice, 9, R.GlassParamsTex, D3DTEXF_POINT);
        BindSampler(pDevice, 11, R.PreAlphaDepthCopyRT->mD3DTexture, D3DTEXF_POINT);
        BindSampler(pDevice, 13, R.SSRHistoryTex->mD3DTexture, D3DTEXF_LINEAR);
        R.bGlassBound = true;
    }

    // Once the main scene is done, so car glass drawn by other render phases (reflections,
    // mirrors) never marches with this camera's data.
    static void UnbindGlassReflections()
    {
        auto& R = PostFxResources;
        auto pDevice = rage::grcDevice::GetD3DDevice();
        if (!pDevice || !R.bGlassBound)
            return;
        SetTextureBoth(pDevice, 9, nullptr);
        SetTextureBoth(pDevice, 11, nullptr);
        SetTextureBoth(pDevice, 13, nullptr);
        R.bGlassBound = false;
    }

    PostFX()
    {
        FusionFix::onInitEventAsync() += []()
        {
            if (GetD3DX9_43DLL())
            {
                PostFxResources.Readini();
                PostFxResources.RegisterCloudParams();
                FrameGeneration::SetSharpener(SharpenShownFrame);

                auto pattern = find_pattern("E8 ? ? ? ? 8B 4F ? E8 ? ? ? ? 8B 4F ? E8 ? ? ? ? 5F", "E8 ? ? ? ? 8B 4F ? E8 ? ? ? ? 8B 4F ? E8 ? ? ? ? 33 C0");
                hbDrawPrimitivePostFX.fun = injector::MakeCALL(pattern.get_first(0), DrawPrimitivePostFX).get();

                pattern = find_pattern("E8 ? ? ? ? 8D 44 24 ? 50 8B CF E8 ? ? ? ? 8D 84 24", "E8 ? ? ? ? 8D 44 24 ? 50 8B CE E8 ? ? ? ? 8D 8C 24 ? ? ? ? 51 8B CE E8 ? ? ? ? 8D 94 24");
                hbDrawSkyHook.fun = injector::MakeCALL(pattern.get_first(0), DrawSky).get();

                pattern = hook::pattern("6A 00 6A 00 6A 00 6A 00 6A 03 57 E8 ? ? ? ? 5F 5E 8B E5 5D C3");
                if (!pattern.empty())
                    hbDrawSkyReflection.fun = injector::MakeCALL(pattern.get_first(11), DrawSkyReflection).get();

                pattern = find_pattern("E8 ? ? ? ? 6A ? FF B7 ? ? ? ? 8B CF FF 77 ? E8 ? ? ? ? 5F", "E8 ? ? ? ? 8B 8E ? ? ? ? 8B 56 ? 6A ? 51");
                hbDrawCallPostFX.fun = injector::MakeCALL(pattern.get_first(0), DrawCallPostFX).get();

                // Downsampling of the scene for bloom and exposure, the first pass of the game's post processing
                pattern = hook::pattern("6A 00 FF B7 ? ? ? ? FF 77 18 E8 ? ? ? ? FF 77 18 8B 4F 60");
                if (!pattern.empty())
                    hbDrawCallDownsample.fun = injector::MakeCALL(pattern.get_first(11), DrawCallDownsample).get();
                else
                {
                    pattern = hook::pattern("8B 46 18 6A 00 52 50 8B CE E8 ? ? ? ? 8B 4E 18 8B 46 60");
                    if (!pattern.empty())
                        hbDrawCallDownsample.fun = injector::MakeCALL(pattern.get_first(9), DrawCallDownsample).get();
                }

                pattern = find_pattern("55 8B EC 83 E4 F0 81 EC D8 00 00 00 56 57 E8 ? ? ? ?");
                if (!pattern.empty())
                    shWaterRender = safetyhook::create_inline(pattern.get_first(0), WaterRenderHook);

                pattern = find_pattern("F3 0F 10 05 ? ? ? ? F3 0F 11 44 24 08 FF 74 24 08");
                if (!pattern.empty())
                    PostFxResources.pWaterLevel = *pattern.get_first<const float*>(4);

                {
                    CRenderPhaseDeferredLighting_LightsToScreen::OnAfterCopyLight() += OnAfterCopyLight;
                    InstallShaftHooks();
                    InstallLocalContactLightHook();
                    VehicleBoxShadows::Install();
                    InstallShaftLoopProfiler();
                    InstallPedSkinHooks();
                    CRenderPhaseDeferredLighting_LightsToScreen::OnBuildRenderList() += []()
                    {
                        PostFxResources.bInteriorScene = Natives::IsInteriorScene();
                        auto cb = new T_CB_Generic_NoArgs(BindSSRTexture);
                        if (cb)
                            cb->Append();
                    };
                    CRenderPhaseDeferredLighting_LightsToScreen::OnAfterBuildRenderList() += []()
                    {
                        auto cb = new T_CB_Generic_NoArgs(PrepareGlassReflections);
                        if (cb)
                            cb->Append();
                    };
                }

                // The fog pass takes the pre-alpha depth copy and SSR's scene history, so it is
                // hooked whatever EnablePreAlphaDepth says; without it SSR never saw the scene.
                {
                    pattern = hook::pattern("FF B6 ? ? ? ? 6A ? E8 ? ? ? ? 5E 8B E5 5D C3");
                    if (!pattern.empty())
                    {
                        hbDrawCallFog.fun = injector::MakeCALL(pattern.get_first(8), DrawCallFog).get();
                    }
                    else
                    {
                        pattern = hook::pattern("6A ? 8B CE E8 ? ? ? ? 5E 8B E5");
                        hbDrawCallFog.fun = injector::MakeCALL(pattern.get_first(4), DrawCallFog).get();
                    }
                }

                pattern = find_pattern("55 8B EC 83 E4 ? 8B 0D ? ? ? ? 8B 15 ? ? ? ? 8B 41", "55 8B EC 83 E4 ? 8B 0D ? ? ? ? 8B 41 ? 8B 15");
                RenderPedAndVehicleFakeShadowsInlineHook = safetyhook::create_inline(pattern.get_first(0), RenderPedAndVehicleFakeShadows); 
            }
        };
    }
} PostFX;