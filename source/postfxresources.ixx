module;

#include <common.hxx>
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
#include <chrono>
#include <filesystem>
#include <vector>
#include <unordered_map>
#include "FusionLog.hpp"

export module postfxresources;

import common;
import comvars;
import d3dx9_43;
import framegeneration;
import framehistory;
import hdr;
import natives;
import occlusion;
import settings;
import shaders;
import renderscale;
import renderskip;
import temporal;
import upscaler;
import postfxprofiler;

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
#define IDR_HIZ_FX                               141
#define IDR_SKYATMOSPHERE_FX                     142

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

export std::optional<std::reference_wrapper<int32_t>> UsePostFxAA;

export bool IsPostFxAA()
{
    if (!UsePostFxAA)
        return false;
    auto aa = UsePostFxAA->get();
    return aa == FusionFixSettings.AntialiasingText.eFXAA || aa == FusionFixSettings.AntialiasingText.eSMAA;
}

// Binding through both the game's device wrapper and the real device: see RageDirect3DDevice9::SetTextureBoth. SSGI's
// accumulation read the G-buffer normals as its depth while the right depth was "bound" through the wrapper alone.
using RageDirect3DDevice9::RealDevice;
using RageDirect3DDevice9::SetTextureBoth;
using RageDirect3DDevice9::SetSamplerStateBoth;
using namespace PostFxProfiler;

// The sampler states SSR.fx declares (MinFilter, MagFilter, MipFilter, AddressU, AddressV), read from its source once
// it is created: D3DX put them on its own registers too, and the trace showed samplers declared without a filter
// (SpecularTex, NormalTex) filtered linearly in some passes. Undeclared states are point and clamp, as
// kSSRSamplerStates leaves them.
export using SamplerStates = std::array<DWORD, 5>;
export std::unordered_map<std::string, SamplerStates> SSRSamplerStates;

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


export constexpr int PostfxTextureCount = 15;

export class PostFxResource
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
        D3DXHANDLE fDenoiseRadius, fDenoiseSSROnly, techSSRDenoise, fPassThinObjects, fStepJitter, fTowardCamera, fReflectionBlur, fDistanceFade, fFallback, fNeighbourFill, fFillMinDistance, fSpreadRadius, fVehicleScale;
        D3DXHANDLE vec4SunView, fCSLength, fCSThickness, fCSMaxViewDistance, fCSIntensity, fCSGrassGloss, techContactShadows;
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
        D3DXHANDLE techGIContactDenoise, techGIContactTemporal, techGIContactUpsample, ContactInTex2D, ContactHistTex2D, fContactBlend;
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
    // the game's are pow(R.L, n), the same peak at every gloss and nothing past the lobe. GGXLights
    // swaps in a GGX lobe of the same width with a height correlated Smith term, so glossy surfaces
    // get a bright core with a long soft tail, times that strength; 0 keeps the game's everywhere.
    // - GGXFresnel: how far the reflectance rises from 0.125 head on towards 1 at grazing angles.
    // - GGXLampSize: the lights' radius in metres, which widens the lobe by its angle, so small
    //   lamps leave no tiny specks.
    // - GGXStretch: the lobe is that much wider along the light projected onto the surface,
    //   so wet roads streak towards lamps.
    // - GGXHeadlightSpacing: half the spacing of a car's lamps. The game lights both with one light
    //   between them; its highlight is taken as two, that far either way along the car's right, so
    //   a wet road shows a streak from each lamp.
    // - GGXFillLights: lights the game draws with no highlight at all (fillerVolumePoint) get
    //   this much of one, from the G-buffer's specular on s13.
    // - GGXSun: the sun's highlight the same way, 0 keeps the game's.
    // - GGXEnvironment: on rough surfaces (gloss under about 0.25: concrete, plaster) the sky's
    //   reflection takes its Fresnel from the split sum environment BRDF, so they stop shining at
    //   grazing angles; asphalt and anything glossier, and the reflection's blur, stay the game's.
    //   0 keeps the game's, values between blend.
    float fLightsGGX = 1.0f;
    // GGX Lighting in the graphics menu (PREF_GGX_LIGHTING, [POSTFX] GGX): off keeps the game's
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
    // - GGXMax: the most a GGX highlight may reach, in the game's own peak (its pow(R.L, n) at 1).
    //   GGX keeps the energy, so a narrow lobe peaks far above it: at 8, normal mapped clothes, skin and
    //   wood burnt into white patches and grazing edges into white strips.
    //   The highlight also fades out as the surface turns away from the camera (N.V under 0.08,
    //   local_light_specular_sheen_and_fade.patch), where normal mapped pixels sparkled.
    float fLightsGGXMax = 2.0f;
    // - GGXSoft (c183, c184.x; local_light_specular_soft_lobe.patch): 0 the lobe above, 1 a softer
    //   look: as wide as a Blinn lobe of the same exponent 1.3 times as rough, widened by 0.2 more at any
    //   distance, and its peak held at the game's whatever the gloss, so rough surfaces get a broad
    //   glow and glossy ones no brighter core. Values between blend.
    // - GGXStretchView (c184.y): 0 stretches the lobe along the light projected onto the surface,
    //   1 along the view, so streaks on wet roads run straight towards the camera. Both agree at the
    //   highlight's centre; only its tail turns.
    float fLightsGGXSoft = 0.0f;
    float fLightsGGXStretchView = 1.0f;
    // - GGXGlints (HeadlightGlints.fx, RenderHeadlightGlints): the lamps of headlights seen in
    //   glossy surfaces from any side but behind, not only inside their beam; 0 none. GGXGlintsSize
    //   the lamps' radius in metres, GGXGlintsGloss the gloss from which surfaces glint,
    //   GGXGlintsMax the ceiling of the GGX lobe before Fresnel.
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
    // WetGroundDebug (graphics menu, Debug): 1 material categories in colours, 2 wetness red, puddles green, rings blue
    static int WetGroundDebug() { static auto p = FusionFixSettings.GetRef("PREF_WET_GROUND_DEBUG"); return p ? p->get() : 0; }
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
    // sun light at all; 2 the raw noise as the coverage, to see whether the sun reads the noise (graphics menu, Debug).
    static int CloudShadowsDebug() { static auto p = FusionFixSettings.GetRef("PREF_CLOUD_SHADOWS_DEBUG"); return p ? p->get() : 0; }
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
    float fVolumetricCloudsMinLight = 0.1f;
    // [WEATHER] VolumetricCloudsScatterTail: the light scattered many times inside a cloud in four octaves at a
    // fifth of the extinction each (Wrenninge's, as al-ro's clouds have it), which reach deep and fall off
    // smoothly; 0: three at half each, as before, spent 0.3 of the layer in, with VolumetricCloudsMinLight 0.25.
    bool bVolumetricCloudsScatterTail = true;
    // [WEATHER] VolumetricCloudsLightJitter: the steps towards the sun slid per pixel and frame, so a cloud's body
    // shades smoothly instead of in layers copying its top (Clouds.fx's fLightJitter). Hidden, live.
    bool bVolumetricCloudsLightJitter = true;
    // The clouds' sunlit side against the sky behind them, in times its brightness.
    float fVolumetricCloudsSkyMatch = 2.0f;
    bool bVolumetricCloudsWeather = true;
    // [WEATHER] PbrSkyHorizonHue: the Physical Sky takes the timecycle's hue (fPalette of it) down to the horizon, as
    // before; off, only its brightness within 10 degrees of the horizon, and the hue there is the air's own.
    bool bPbrSkyHorizonHue = false;
    float fVolumetricCloudsVanilla = 0.0f;
    float fVolumetricCloudsTranslucency = 0.25f;
    float fVolumetricCloudsEvolution = 1.0f;
    float fVolumetricCloudsSaturation = 1.0f;
    float fVolumetricCloudsMottle = 0.4f;
    // How far up, in billows, a heap's density pushes its billows, so they follow its shape.
    float fVolumetricCloudsDetailFlow = 0.0f;
    // The shaded side and the bases against the game's cloud colour, and how much of the view's
    // extinction the sun's light takes inside a cloud: the clouds' contrast.
    float fVolumetricCloudsShade = 0.65f;
    float fVolumetricCloudsAbsorption = 0.35f;
    // VolumetricCloudsDebug (graphics menu, Debug): the view Clouds.fx shows, 2 to 15, from menu steps 1 to 14; 0 off.
    static int VolumetricCloudsDebug() { static auto p = FusionFixSettings.GetRef("PREF_CLOUDS_DEBUG"); const int step = p ? p->get() : 0; return step > 0 ? step + 1 : 0; }
    // The clouds in the reflection map (water, mirrors), at this brightness against the clouds.
    bool bVolumetricCloudsReflections = true;
    bool bVolumetricCloudsReflectionsAtNight = false;
    float fVolumetricCloudsReflectionBrightness = 1.0f;
    // CloudReflectionsHalfResolution: the reflections' clouds worked out at half size and laid over at full
    bool bVolumetricCloudsReflectionsHalfRes = true;
    // CloudsHalfResolution: the scene's clouds marched and lit at half size, accumulated over frames and laid over
    // at full size; off, all of it at full size, sharper, about four times the march's cost (restart).
    bool bVolumetricCloudsHalfRes = true;
    // One a size: the water's reflection (1280x512) and the reflection map (1024x1024) take turns every frame, and one
    // target made again for each had the game's books of its video memory (made through the device wrapper, freed
    // past them) grow by a gigabyte in 10 s, until the streaming let the whole world go
    struct CloudReflectionHalfTarget { IDirect3DTexture9* tex = nullptr; IDirect3DSurface9* surf = nullptr; UINT width = 0, height = 0; uint32_t used = 0; };
    CloudReflectionHalfTarget CloudReflectionHalf[3];
    IDirect3DTexture9* CloudReflectionHalfTex = nullptr;    // the one for this reflection, set by ReflectionHalfTarget
    IDirect3DSurface9* CloudReflectionHalfSurf = nullptr;

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
    bool bPlayerInCar = false; // set with bInteriorScene, as the lights' render list is built
    const char* szWetGroundStatus = "not run yet";
    // Hi-Z for occlusion culling (HiZ.fx, BuildHiZ): the levels of 4 x 4 pixels at the end of the G-buffer pass, made
    // afresh whenever the depth copy's size changes, and a ring of copies in system memory being read back, each with
    // the query that says the GPU is done with it and the camera it was drawn with
    ID3DXEffect* HiZEffect = nullptr;
    HRESULT hrHiZEffect = S_OK;
    static constexpr int kHiZLevels = 4;
    static constexpr int kHiZReadbacks = 3;
    static constexpr int kHiZMaxWidth = 320;
    IDirect3DTexture9* HiZTex[kHiZLevels] = {};
    IDirect3DSurface9* HiZSurf[kHiZLevels] = {};
    UINT HiZWidth[kHiZLevels] = {}, HiZHeight[kHiZLevels] = {};
    int nHiZLevels = 0;
    UINT nHiZSourceWidth = 0, nHiZSourceHeight = 0;
    struct HiZReadback
    {
        IDirect3DSurface9* memory = nullptr;
        IDirect3DQuery9* query = nullptr;
        bool pending = false;
        UINT width = 0, height = 0;
        float scaleU = 0.0f, scaleV = 0.0f;
        float viewProjection[16] = {};
        float position[3] = {};
        float nearPlane = 0.0f, farPlane = 0.0f;
        uint32_t frame = 0;
    } HiZReadbacks[kHiZReadbacks];
    void ReleaseHiZ()
    {
        for (int i = 0; i < kHiZLevels; ++i)
        {
            SAFE_RELEASE(HiZSurf[i]);
            SAFE_RELEASE(HiZTex[i]);
        }
        for (auto& r : HiZReadbacks)
        {
            SAFE_RELEASE(r.memory);
            SAFE_RELEASE(r.query);
            r.pending = false;
        }
        nHiZLevels = 0;
        nHiZSourceWidth = nHiZSourceHeight = 0;
    }
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
    // [WEATHER] VolumetricCloudsPerlinWorley: the detail volume as Perlin-Worley noise (Perlin lifted by the
    // Worley billows, as in Horizon Zero Dawn's clouds): billows joined by wisps and ragged plumes instead of
    // round billows alone. Off, the plain Worley billows. The texture is built again when it changes.
    bool bVolumetricCloudsPerlinWorley = true;
    // [WEATHER] VolumetricCloudsLumps: how far, in metres either way, the clouds' outline bulges and dents by
    // billows, so the sides and tops seen from afar are lumpy rather than smooth (Clouds.fx's fLump). 0: off.
    float fVolumetricCloudsLumps = 50.0f;
    bool bCloudDetailPerlinWorley = false; // what CloudDetailTexture was built with
    // Volumetric Clouds in the graphics menu (PREF_VOLUMETRIC_CLOUDS, the same VolumetricClouds key)
    // turns them on and off at once; the game's flat clouds and the cloud shadows follow.
    bool VolumetricCloudsEnabled() const
    {
        static auto p = FusionFixSettings.GetRef("PREF_VOLUMETRIC_CLOUDS");
        return p ? p->get() != 0 : bVolumetricClouds;
    }
    bool VolumetricCloudsOn() const { return VolumetricCloudsEnabled() && CloudsEffect != nullptr; }

    // PBR sky (pbrsky.ixx, SkyAtmosphere.fx): the transmittance and multiple scattering tables, drawn again when the
    // haze changes, and the sky seen from the camera, drawn once a frame before the first sky; gta_atmoscatt_clouds
    // reads it on s10 (sky_pbr.patch).
    ID3DXEffect* SkyEffect = nullptr;
    HRESULT hrSkyEffect = S_OK;
    rage::grcRenderTargetPC* SkyTransmittanceTex = nullptr;
    IDirect3DSurface9* SkyTransmittanceSurf = nullptr;
    rage::grcRenderTargetPC* SkyMultiScatterTex = nullptr;
    IDirect3DSurface9* SkyMultiScatterSurf = nullptr;
    rage::grcRenderTargetPC* SkyViewTex = nullptr;
    IDirect3DSurface9* SkyViewSurf = nullptr;
    float fSkyTablesHaze = -1.0f;   // the haze the first two tables were drawn for, < 0 not drawn
    uint32_t nSkyViewFrame = 0;     // FrameHistory::Frame() SkyView was drawn in, 0 never
    // The strength the sky last took the table at, and why it did not, for the Ctrl+Shift+F10 log.
    float fPbrSkyLastStrength = 0.0f, fPbrSkyLastHaze = 0.0f, fPbrSkyLastSunHeight = 0.0f;
    const char* szPbrSkyStatus = "not run yet";
    uint32_t nPbrSkyDraws = 0;
    // Physical Sky in the graphics menu (PREF_PBR_SKY, [WEATHER] PbrSky)
    bool PbrSkyEnabled() const
    {
        static auto p = FusionFixSettings.GetRef("PREF_PBR_SKY");
        return p ? p->get() != 0 : true;
    }
    // Aerial Perspective in the graphics menu (PREF_AERIAL_PERSPECTIVE, [WEATHER] AerialPerspective): 0 off, 1 to 3 how
    // thick the air is; the fog takes the Physical Sky's colour without it too. What the last fog pass took, for the
    // Ctrl+Shift+F10 log.
    int AerialPerspectiveLevel() const
    {
        static auto p = FusionFixSettings.GetRef("PREF_AERIAL_PERSPECTIVE");
        return p ? p->get() : 2;
    }
    // Aerial Perspective Debug View in Graphics > Debug ([DEBUG] AerialPerspectiveDebug): the air 25 times as thick.
    bool AerialPerspectiveDebug() const
    {
        static auto p = FusionFixSettings.GetRef("PREF_AERIAL_DEBUG");
        return p && p->get() != 0;
    }
    float fAerialLastShare = 0.0f, fAerialLastStrength = 0.0f, fAerialLastUnits = 0.0f, fAerialLastHaze = 0.0f;
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
    size_t CloudMoonPositionIdx = 0, CloudSkyColorIdx = 0, TopCloudIdx = 0;
    // The lit and shaded colours the clouds were last drawn with, and the sky's clamp, for the log.
    float CloudLastLit[3] = {}, CloudLastShade[3] = {}, CloudLastClamp[3] = {}, CloudLastCeiling = 0.0f, CloudLastLightStrength = 0.0f;
    // The physical sky's share in the clouds' light, its table's colour times this on screen, and the sun's hue it gave.
    float CloudLastPbrShare = 0.0f, CloudLastSkyUnits = 0.0f, CloudLastSunHue[3] = {};
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
        TopCloudIdx = rage::grmShaderInfo::registerShaderParam("gta_atmoscatt_clouds.fxc", "TopCloudBiasDetailThresholdHeight");
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
    bool bContactShadowsMoon = true; // ContactShadowsMoon
    float fContactShadowIntensity = 1.0f;
    bool bContactShadowStepJitter = true;
    // ContactShadowsHalfResolution: the march runs at half size into ContactRawHalfTex and
    // ContactUpsample_PS brings it to full size, weighing by depth, before the smoothing.
    bool bContactShadowsHalfRes = true;
    // FusedLightingPasses: indirect light and contact shadows, both marched at half size, smoothed, accumulated and
    // brought up to full size by three passes for both (two targets each) instead of three each
    bool bFusedLightingPasses = true;
    // DepthBoundsLights: each light of the main view drawn only where the scene's depth lies within its sphere
    // (the depth bounds test, see SetLightDepthBounds in postfx.ixx)
    bool bDepthBoundsLights = true;
    // LightEarlyOut: the light shaders leave the shadow map's filter and the GGX lobes out for pixels outside the light's
    // sphere, where the light gives nothing (zz_sphere_early_out.patch, b5); contact shadows, boxes and shapes skip them already
    bool bLightEarlyOut = true;
    // LightVolumeDepth: the light volumes' vertex shaders give the log depth the light shaders wrote, so the depth test
    // drops pixels behind the volume before the light shader runs (zz_volume_depth.patch, vs b5); off, none are dropped
    bool bLightVolumeDepth = true;
    // Contact shadows from street lights and headlights, marched in the light shaders
    // themselves (shaders/patches/local_light_contact_shadows.patch); they follow the Contact
    // Shadows menu toggle.
    bool bLocalContactShadows = true;
    // From binding the lighting inputs to the end of deferred lighting, the light loop may turn
    // c203 off for a light and back on (see InstallLocalContactLightHook).
    bool bLocalContactPass = false;
    bool bLocalContactLightOff = false;
    // Lights without a shadow map this frame (most headlights, lamps left without a slot): this much of their contact shadow is kept.
    // All of it (1) since Vehicle Box Shadows shadow the cars under them: at 0 a lamp that lost its slot to a passing car's
    // lights lost the contact shadows of all under it, and they blinked as cars went by (trace of the slots, 2026-10-23).
    // With Vehicle Box Shadows off, 0: there the car had no shadow, and its contact shadow was only a dark frame around it.
    float fLocalContactShadowUnshadowed = 1.0f;
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
    float fVehicleBoxShadowRounding = 0.2f;
    float fVehicleBoxShadowContact = 0.0f;  // VehicleBoxShadowsContact
    float fVehicleBoxShadowEndRounding = 0.4f; // metres the box's front and back draw in at the sides, like a car's nose
    float fVehicleBoxShadowScale[3] = { 1.0f, 0.95f, 0.9f }; // length, width, height, of the model's bounds
    // How far a box's shadow follows the light: 1 away from it, 0 straight down under the car whatever the light's way.
    float fVehicleBoxShadowFollowLight = 1.0f;
    float fVehicleBoxShadowLowLightNarrow = 1.0f;
    float fVehicleBoxShadowLowLightHeight = 0.6f;
    // Car lights (headlights, tail lights) shine low and close, so a box's shadow from them spreads over the whole road
    // ahead and shows its shape; off by default, lamps only.
    bool bVehicleBoxShadowsFromCarLights = false;
    // Car lights' maps show the lamp posts (nightshadows' PostCarLightShadows): their post shapes are left out then
    bool bPostShadowsFromCarLights = false;
    // A lamp that takes a shadow slot, whose map then shows the cars, fades its boxes out over SlotFade ms, so the
    // shadow under a car does not jump between the two; WithSlots keeps them under its real shadow for good.
    bool bVehicleBoxShadowsWithSlots = false;
    bool bVehicleBoxShadowsUntilLeaving = true; // VehicleBoxShadowsUntilLeaving, see VehicleBoxShadows::SetForLight
    bool bVehicleBoxShadowsBlend = true; // VehicleBoxShadowsBlend, read with the others
    uint32_t nVehicleBoxShadowSlotFadeMs = 600;
    uint32_t nVehicleBoxShadowFadeInMs = 300, nVehicleBoxShadowFadeOutMs = 300, nVehicleBoxShadowSizeFadeMs = 300;
    float fVehicleBoxShadowsMaxDistance = 200.0f; // metres from the camera to a lamp's light past which it has no boxes
    int32_t nVehicleBoxShadowsPerLamp = 3; // cars a lamp shadows as boxes, up to five; each costs its lit pixels about as much again
    int32_t nVehicleBoxShadowsPerCar = 2;  // lamps that may shadow one car as a box, the strongest on it; 0 any number
    float fVehicleBoxShadowsMinLight = 0.04f; // the least share of a lamp's light at the car, (1 - distance / radius)^2, for a box
    bool bVehicleBoxShadowsLive = false;
    float fVehicleBoxShadowLeadFrames = -1.0f;
    bool bVehicleBoxShadowsLog = false; // VehicleBoxShadowsLog, see VehicleBoxShadows::Trace
    // PedCapsuleShadows: people on foot under a light whose map does not show them get the shadow of a standing capsule
    // of that radius and height, its foot that far below the ped's position (see VehicleBoxShadows)
    bool bPedCapsuleShadows = true;
    float fPedCapsuleRadius = 0.25f, fPedCapsuleHeight = 1.75f, fPedCapsuleFootBelow = 1.0f;
    // PedCapsuleAddedLights: capsules under lights whose shadow is ours, not the map's (facades' model and glow lights,
    // traffic signals: AddedLightsLast's), which hardly ever take a slot; 0 leaves them out (a blob with no lamp to be
    // seen, before the player's shadow followed the bones). PedCapsuleLightSize: the light's size for the capsule's
    // penumbra, PedCapsuleSoftening that much more for each metre the light stands over 2 m above the ped's foot (a high
    // light's shadow came out as sharp as a lamp's at head height); PedCapsuleBaseSoftness the least penumbra, in metres,
    // also at the feet, where it shrank to 5 cm and drew the shadow's foot square
    bool bPedCapsuleAddedLights = true;
    float fPedCapsuleLightSize = 0.26f, fPedCapsuleSoftening = 0.0f, fPedCapsuleBaseSoftness = 0.088f;
    // PedCapsuleDistanceSoftness: metres of penumbra more for each metre from the light, for the capsules and the bones
    // (zz_z_shape_distance_softness.patch, c142.x). The shadow map blurs by an angle, so a far light's map shadow is soft,
    // while a capsule's penumbra shrank with the light's distance: the player's shadow on a wall from a far lamp came out
    // as a sharp figure of pipes beside the soft shadows of the railings
    float fPedCapsuleDistanceSoftness = 0.03f;
    // PedBoneShadows: the player's shadow from 25 capsules along the bones (legs, feet, hips, body, shoulders, arms,
    // hands, head), posed each frame, in place of the one standing capsule, with the props' shapes
    bool bPedBoneShadows = true;
    // PedBoneRadiusScale: the bone capsules' thickness, 1 as BodyShapes sets them
    float fPedBoneRadiusScale = 0.8f;
    // PedBoneShoulderWidth: how far out to the upper arms' bones the shoulders reach, 1 all the way
    float fPedBoneShoulderWidth = 0.8f;
    // PedBoneShadowsFollowDraw: the bone capsules moved to where the player was drawn this frame, from where the
    // bones were posed at the light list's hand over
    bool bPedBoneShadowsFollowDraw = true;
    // ShapeGroups: the light shaders read a light's shapes in four groups, each only near the way to the light from the
    // pixels it may shadow (zz_shape_groups.patch); 0 reads them all everywhere
    bool bShapeGroups = true;
    // PropShapeShadows: props within PropShapeDistance of the player get the shadow of a capsule or a box from the
    // lights near them, up to PropShapesPerLight shapes a light (see VehicleBoxShadows); a lamp post's post is
    // PropShapePostRadius thick, its arm PropShapeArmRadius
    bool bPropShapeShadows = true;
    // PropsInLampCaches (nightshadows): a cached lamp's map holds the props it lights, so they take no shapes from it
    bool bPropsInLampCaches = true;
    float fPropShapeDistance = 80.0f, fPropShapePostRadius = 0.12f, fPropShapeArmRadius = 0.08f;
    float fPropShapeScale = 0.85f; // of the bounds across, for a capsule's radius and a box's length and width
    bool bPropShapeViewCulling = true; // props whose shadow cannot fall in the camera's view take no shapes
    int32_t nPropShapesPerLight = 12;
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
    // FusedLightingPasses: contact shadows marched at half size and left for the indirect light's passes to smooth,
    // accumulate and bring up to full size along with its own (RenderIndirectLight), or FinishContactShadows when
    // those did not run; null when none waits. bContactPendingHistory: whether their accumulation had a history.
    IDirect3DTexture9* ContactPending = nullptr;
    bool bContactPendingHistory = false;
    bool bContactValid = false;
    bool bContactBound = false;
    static constexpr int kContactDebugMode = 7;
    static int SSRDebugMode() { static auto p = FusionFixSettings.GetRef("PREF_SSR_DEBUG"); return p ? p->get() : 0; }
    // The smoothed SSR result (SSRDenoise_PS) that deferred_lighting reads, when enabled.
    float fSSRDenoiseRadius = 2.0f;
    bool bSSRPassThinObjects = true;
    bool bSSRStepJitter = true;
    // SSRTemporalJitter: while SSR accumulates, the step offsets move on every
    // frame (vec2NoiseOffset in SSR.fx), so the accumulation averages them.
    bool bSSRTemporalJitter = true;
    float fSSRTowardCamera = 0.0f;
    float fSSRReflectionBlur = 0.0f;
    // SSRStepPixels: pixels of a ray on the march's target per step, 2 by default.
    float fSSRStepPixels = 2.0f;
    // SSRMarchFullDepth (graphics menu, Debug): the march steps through the full size
    // depth instead of SSRMarchDepthTex, to compare the two.
    static bool SSRMarchFullDepth() { static auto p = FusionFixSettings.GetRef("PREF_SSR_FULL_DEPTH"); return p && p->get() != 0; }
    float fSSRDistanceFade = 0.0f;
    float fSSRFallback = 0.8f;
    // SSRNeighbourFill: the fill of a miss from the hits around it (NeighbourFill, the spread), apart from SSRFallback's
    // from past what hid the ray, to tell which one smeared a reflection into copies around it.
    float fSSRNeighbourFill = 0.8f;
    // SSRNeighbourFillMinDistance (m): hits nearer than this along their ray fill no misses around them (0: all do)
    float fSSRNeighbourFillMinDistance = 3.0f;
    // deferred_lighting draws SSR's colour, the scene's own light, through the game's reflection scale meant for its
    // parabolic map: 10 times globalScalars.w (c39.w, read in BindLightingInputs, 0 until then) times 2 x specular times
    // its Fresnel. On car bodies, whose clean paint takes SSR in full by its gloss, SSR_PS divides that 10 x c39.w back
    // out, times SSRVehicleUnits (1 leaves the reflection at Fresnel x specular of the scene's light, too weak in game;
    // 10 with c39.w at 1 is the old reflection, never exceeded): lit poles and walls had turned roofs white at night.
    float fSSRVehicleUnits = 4.0f;
    float fGameReflectionScale = 0.0f;
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
    // resolution. SSRTemporal is the share of the history kept, 0 turns
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
    //       jitter flag (SSRCarWindowsStepJitter) and how far reflections
    //       pointing back at the camera reach (SSRTowardCamera, as in SSR)
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
            auto pDevice = RealDevice(rage::grcDevice::GetD3DDevice());
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
    // SSRWetGround: while it rains, SSR on ground facing up that is under the
    // gloss cutoff, drawn this many times brighter (fWetGroundBoost in SSR.fx); 0 turns it off. The
    // wet ground pass gives only puddles water's specular: its film keeps about dry asphalt's (0.08),
    // so at 1 cars on wet roads reflected half as bright as at 2, and 2 was brighter than the scene.
    // The film reflects at a third of the puddles' weight, less where its gloss is low (grass).
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
    int nAmbientOcclusionGTAOSlices = 2;
    int nAmbientOcclusionGTAOSteps = 4;
    float fAmbientOcclusionGTAOStrength = 1.0f;
    float fAmbientOcclusionGTAOThinOccluders = 0.5f;
    float fAmbientOcclusionTemporal = 0.9f;  // share of last frames' GTAO kept, 0 turns accumulation off
    bool bAmbientOcclusionMultiBounce = true;
    // AmbientOcclusionHalfResolution: AO is worked out, accumulated and blurred at half the size and read back by
    // depth when it is put on the screen
    bool bAmbientOcclusionHalfRes = true;

    struct
    {
        D3DXHANDLE AOTexture2D, AOCamDepthTexture2D, DepthTex2D, NormalTex2D;
        D3DXHANDLE vec4WorldToView, fUseNormals, fGTAOStrength, fThinOccluders, vec2NoiseOffset, fMultiBounce, AlbedoTex2D;
        D3DXHANDLE AOHistoryTex2D, PrevDepthTex2D, MotionTex2D, vec4ViewToPrevClip, fUseMotion, vec2MotionJitter, fUsePrevDepth, vec2PrevDepthRange, fTemporalBlend;
        D3DXHANDLE vec2AOTexelSize, fUpsample, fUpsampleMip;

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

    // D3DXCreateEffectFromResourceW, with the compiled effect kept in FusionFixShaderCache next to the plugin. D3DX took
    // seconds compiling the larger effects from their HLSL on the first frame of the world, each time the game started.
    // The file's name holds hashes of the source and of the defines with the flags: a new build or other settings
    // compile again, and the files of this effect for another source (other builds) are removed.
    static HRESULT CreateEffectCached(IDirect3DDevice9* pDevice, HMODULE hm, int resource, const char* name, const D3DXMACRO* defines,
                                      ID3DXEffect** effect, ID3DXBuffer** errors)
    {
        constexpr DWORD flags = 0;
        // The effects are large address aware, as the game is: without it D3DX took a parameter's handle above 2 GB for
        // its name and read it as a string. With it a name is no handle: parameters are set through their handles only
        constexpr DWORD createFlags = D3DXFX_LARGEADDRESSAWARE;
        const auto start = std::chrono::steady_clock::now();
        auto elapsedMs = [&] { return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count(); };

        HRSRC info = FindResourceW(hm, MAKEINTRESOURCEW(resource), RT_RCDATA);
        HGLOBAL handle = info ? LoadResource(hm, info) : nullptr;
        const void* source = handle ? LockResource(handle) : nullptr;
        const DWORD sourceSize = info ? SizeofResource(hm, info) : 0;
        if (!source || !sourceSize)
            return D3DXCreateEffectFromResourceW(pDevice, hm, MAKEINTRESOURCEW(resource), defines, nullptr, flags | createFlags, nullptr, effect, errors);

        auto fnv = [](uint64_t h, const void* data, size_t size)
        {
            for (size_t i = 0; i < size; ++i)
                h = (h ^ static_cast<const uint8_t*>(data)[i]) * 0x100000001B3ull;
            return h;
        };
        const uint64_t sourceHash = fnv(0xCBF29CE484222325ull, source, sourceSize);
        uint64_t defineHash = fnv(0xCBF29CE484222325ull, &flags, sizeof(flags));
        for (auto d = defines; d && d->Name; ++d)
        {
            const char* value = d->Definition ? d->Definition : "";
            defineHash = fnv(defineHash, d->Name, strlen(d->Name) + 1);
            defineHash = fnv(defineHash, value, strlen(value) + 1);
        }

        std::error_code ec;
        const auto directory = FusionLog::Directory() / L"FusionFixShaderCache";
        char sourceTag[24];
        sprintf_s(sourceTag, "%016llX", sourceHash);
        char fileName[128];
        sprintf_s(fileName, "%s_%s_%016llX.fxo", name, sourceTag, defineHash);
        const auto path = directory / fileName;

        if (FILE* file = nullptr; !_wfopen_s(&file, path.c_str(), L"rb") && file)
        {
            std::vector<uint8_t> compiled;
            fseek(file, 0, SEEK_END);
            const long size = ftell(file);
            fseek(file, 0, SEEK_SET);
            if (size > 0)
            {
                compiled.resize(size_t(size));
                if (fread(compiled.data(), 1, compiled.size(), file) != compiled.size())
                    compiled.clear();
            }
            fclose(file);
            if (!compiled.empty() &&
                D3DXCreateEffect(pDevice, compiled.data(), UINT(compiled.size()), nullptr, nullptr, createFlags, nullptr, effect, nullptr) == S_OK)
            {
                FusionLog::Write("PostFx", "Shaders", "%s: from the cache in %.0f ms", name, elapsedMs());
                return S_OK;
            }
            FusionLog::Write("PostFx", "Shaders", "%s: the cached file did not load, compiling again", name);
            std::filesystem::remove(path, ec);
        }

        ID3DXEffectCompiler* compiler = nullptr;
        ID3DXBuffer* compiled = nullptr;
        HRESULT hr = D3DXCreateEffectCompiler(static_cast<LPCSTR>(source), UINT(sourceSize), defines, nullptr, flags, &compiler, errors);
        if (hr == S_OK)
        {
            if (errors)
                SAFE_RELEASE(*errors);
            hr = compiler->CompileEffect(flags, &compiled, errors);
            compiler->Release();
        }
        if (hr != S_OK || !compiled)
        {
            SAFE_RELEASE(compiled);
            FusionLog::Write("PostFx", "Shaders", "%s: compiling failed, 0x%08lX", name, hr);
            return hr != S_OK ? hr : E_FAIL;
        }
        const double compileMs = elapsedMs();

        hr = D3DXCreateEffect(pDevice, compiled->GetBufferPointer(), compiled->GetBufferSize(), nullptr, nullptr, createFlags, nullptr, effect, nullptr);
        if (hr == S_OK)
        {
            const std::string prefix = std::string(name) + "_";
            for (auto& entry : std::filesystem::directory_iterator(directory, ec))
            {
                const auto other = entry.path().filename().string();
                if (other.starts_with(prefix) && other.compare(prefix.size(), 16, sourceTag) != 0)
                    std::filesystem::remove(entry.path(), ec);
            }
            std::filesystem::create_directories(directory, ec);
            auto temp = path;
            temp += L".tmp";
            bool written = false;
            if (FILE* file = nullptr; !_wfopen_s(&file, temp.c_str(), L"wb") && file)
            {
                written = fwrite(compiled->GetBufferPointer(), 1, compiled->GetBufferSize(), file) == compiled->GetBufferSize();
                written = fclose(file) == 0 && written;
            }
            written = written && MoveFileExW(temp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING);
            if (!written)
                std::filesystem::remove(temp, ec);
            FusionLog::Write("PostFx", "Shaders", "%s: compiled in %.0f ms, %s", name, compileMs, written ? "cached" : "NOT cached, writing failed");
        }
        else
            FusionLog::Write("PostFx", "Shaders", "%s: the compiled effect did not load, 0x%08lX", name, hr);
        compiled->Release();
        return hr;
    }

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
            }
            SAFE_RELEASE(bf1);
            SAFE_RELEASE(bf2);
        }

        if (!dof_coc_ps)
        {
            if (D3DXAssembleShaderFromResourceW(hm, MAKEINTRESOURCEW(IDR_dof_coc_ps), NULL, NULL, 0, &bf1, &bf2) == S_OK)
            {
                if (pDevice->CreatePixelShader((DWORD*)bf1->GetBufferPointer(), &dof_coc_ps) != S_OK || !dof_coc_ps)
                    SAFE_RELEASE(dof_coc_ps);
            }
            SAFE_RELEASE(bf1);
            SAFE_RELEASE(bf2);
        }

        if (!depth_of_field_tent_ps)
        {
            if (D3DXAssembleShaderFromResourceW(hm, MAKEINTRESOURCEW(IDR_depth_of_field_tent_ps), NULL, NULL, 0, &bf1, &bf2) == S_OK)
            {
                if (pDevice->CreatePixelShader((DWORD*)bf1->GetBufferPointer(), &depth_of_field_tent_ps) != S_OK || !depth_of_field_tent_ps)
                    SAFE_RELEASE(depth_of_field_tent_ps);
            }
            SAFE_RELEASE(bf1);
            SAFE_RELEASE(bf2);
        }

        if (!stipple_filter_ps)
        {
            if (D3DXAssembleShaderFromResourceW(hm, MAKEINTRESOURCEW(IDR_stipple_filter_ps), NULL, NULL, 0, &bf1, &bf2) == S_OK)
            {
                if (pDevice->CreatePixelShader((DWORD*)bf1->GetBufferPointer(), &stipple_filter_ps) != S_OK || !stipple_filter_ps)
                    SAFE_RELEASE(stipple_filter_ps);
            }
            SAFE_RELEASE(bf1);
            SAFE_RELEASE(bf2);
        }

        //hlsl: the bytecode fxc made at build time from the same source (premake5.lua), compiled here only if it is missing
        if (!SSDraw_PS && !loadCompiledShader(IDR_SSDraw_PS_compiled, SSDraw_PS))
        {
            if (D3DXCompileShaderFromResourceW(hm, MAKEINTRESOURCEW(IDR_SunShafts_PS), NULL, NULL, "SSDraw", "ps_3_0", 0, &bf1, &bf2, &ppConstantTable) == S_OK)
            {
                if (pDevice->CreatePixelShader((DWORD*)bf1->GetBufferPointer(), &SSDraw_PS) != S_OK || !SSDraw_PS)
                    SAFE_RELEASE(SSDraw_PS);
            }
            SAFE_RELEASE(bf1);
            SAFE_RELEASE(bf2);
            SAFE_RELEASE(ppConstantTable);
        }

        if (!SSPrepass_PS && !loadCompiledShader(IDR_SSPrepass_PS_compiled, SSPrepass_PS))
        {
            if (D3DXCompileShaderFromResourceW(hm, MAKEINTRESOURCEW(IDR_SunShafts_PS), NULL, NULL, "SSPrepass", "ps_3_0", 0, &bf1, &bf2, &ppConstantTable) == S_OK)
            {
                if (pDevice->CreatePixelShader((DWORD*)bf1->GetBufferPointer(), &SSPrepass_PS) != S_OK || !SSPrepass_PS)
                    SAFE_RELEASE(SSPrepass_PS);
            }
            SAFE_RELEASE(bf1);
            SAFE_RELEASE(bf2);
            SAFE_RELEASE(ppConstantTable);
        }

        if (!SSAdd_PS && !loadCompiledShader(IDR_SSAdd_PS_compiled, SSAdd_PS))
        {
            if (D3DXCompileShaderFromResourceW(hm, MAKEINTRESOURCEW(IDR_SunShafts_PS), NULL, NULL, "SSAdd", "ps_3_0", 0, &bf1, &bf2, &ppConstantTable) == S_OK)
            {
                if (pDevice->CreatePixelShader((DWORD*)bf1->GetBufferPointer(), &SSAdd_PS) != S_OK || !SSAdd_PS)
                    SAFE_RELEASE(SSAdd_PS);
            }
            SAFE_RELEASE(bf1);
            SAFE_RELEASE(bf2);
            SAFE_RELEASE(ppConstantTable);
        }

        if (!FxaaPS && !loadCompiledShader(IDR_FxaaPS_compiled, FxaaPS))
        {
            if (D3DXCompileShaderFromResourceW(hm, MAKEINTRESOURCEW(IDR_FXAA), NULL, NULL, "ApplyFXAA", "ps_3_0", 0, &bf1, &bf2, &ppConstantTable) == S_OK)
            {
                if (pDevice->CreatePixelShader((DWORD*)bf1->GetBufferPointer(), &FxaaPS) != S_OK || !FxaaPS)
                    SAFE_RELEASE(FxaaPS);
            }
            SAFE_RELEASE(bf1);
            SAFE_RELEASE(bf2);
            SAFE_RELEASE(ppConstantTable);
        }

        if (!CAS_PS && !loadCompiledShader(IDR_CAS_PS_compiled, CAS_PS))
        {
            if (D3DXCompileShaderFromResourceW(hm, MAKEINTRESOURCEW(IDR_CAS), NULL, NULL, "ApplyCAS", "ps_3_0", 0, &bf1, &bf2, &ppConstantTable) == S_OK)
            {
                if (pDevice->CreatePixelShader((DWORD*)bf1->GetBufferPointer(), &CAS_PS) != S_OK || !CAS_PS)
                    SAFE_RELEASE(CAS_PS);
            }
            SAFE_RELEASE(bf1);
            SAFE_RELEASE(bf2);
            SAFE_RELEASE(ppConstantTable);
        }

        if (!CASMasked_PS && !loadCompiledShader(IDR_CASMasked_PS_compiled, CASMasked_PS))
        {
            if (D3DXCompileShaderFromResourceW(hm, MAKEINTRESOURCEW(IDR_CAS), NULL, NULL, "ApplyCASMasked", "ps_3_0", 0, &bf1, &bf2, &ppConstantTable) == S_OK)
            {
                if (pDevice->CreatePixelShader((DWORD*)bf1->GetBufferPointer(), &CASMasked_PS) != S_OK || !CASMasked_PS)
                    SAFE_RELEASE(CASMasked_PS);
            }
            SAFE_RELEASE(bf1);
            SAFE_RELEASE(bf2);
            SAFE_RELEASE(ppConstantTable);
        }

        if (!SMAA_EdgeDetection && !loadCompiledShader(IDR_SMAA_EdgeDetection_compiled, SMAA_EdgeDetection))
        {
            if (D3DXCompileShaderFromResourceW(hm, MAKEINTRESOURCEW(IDR_SMAA), NULL, NULL, "DX9_SMAALumaEdgeDetectionPS", "ps_3_0", 0, &bf1, &bf2, &ppConstantTable) == S_OK)
            {
                if (pDevice->CreatePixelShader((DWORD*)bf1->GetBufferPointer(), &SMAA_EdgeDetection) != S_OK || !SMAA_EdgeDetection)
                    SAFE_RELEASE(SMAA_EdgeDetection);
            }
            SAFE_RELEASE(bf1);
            SAFE_RELEASE(bf2);
            SAFE_RELEASE(ppConstantTable);
        }

        if (!SMAA_BlendingWeightsCalculation && !loadCompiledShader(IDR_SMAA_BlendingWeightsCalculation_compiled, SMAA_BlendingWeightsCalculation))
        {
            if (D3DXCompileShaderFromResourceW(hm, MAKEINTRESOURCEW(IDR_SMAA), NULL, NULL, "DX9_SMAABlendingWeightCalculationPS", "ps_3_0", 0, &bf1, &bf2, &ppConstantTable) == S_OK)
            {
                if (pDevice->CreatePixelShader((DWORD*)bf1->GetBufferPointer(), &SMAA_BlendingWeightsCalculation) != S_OK || !SMAA_BlendingWeightsCalculation)
                    SAFE_RELEASE(SMAA_BlendingWeightsCalculation);
            }
            SAFE_RELEASE(bf1);
            SAFE_RELEASE(bf2);
            SAFE_RELEASE(ppConstantTable);
        }

        if (!SMAA_NeighborhoodBlending && !loadCompiledShader(IDR_SMAA_NeighborhoodBlending_compiled, SMAA_NeighborhoodBlending))
        {
            if (D3DXCompileShaderFromResourceW(hm, MAKEINTRESOURCEW(IDR_SMAA), NULL, NULL, "DX9_SMAANeighborhoodBlendingPS", "ps_3_0", 0, &bf1, &bf2, &ppConstantTable) == S_OK)
            {
                if (pDevice->CreatePixelShader((DWORD*)bf1->GetBufferPointer(), &SMAA_NeighborhoodBlending) != S_OK || !SMAA_NeighborhoodBlending)
                    SAFE_RELEASE(SMAA_NeighborhoodBlending);
            }
            SAFE_RELEASE(bf1);
            SAFE_RELEASE(bf2);
            SAFE_RELEASE(ppConstantTable);
        }

        if (!SMAA_EdgeDetectionVS && !loadCompiledShader(IDR_SMAA_EdgeDetectionVS_compiled, SMAA_EdgeDetectionVS))
        {
            if (D3DXCompileShaderFromResourceW(hm, MAKEINTRESOURCEW(IDR_SMAA), NULL, NULL, "DX9_SMAAEdgeDetectionVS", "vs_3_0", 0, &bf1, &bf2, &ppConstantTable) == S_OK)
            {
                if (pDevice->CreateVertexShader((DWORD*)bf1->GetBufferPointer(), &SMAA_EdgeDetectionVS) != S_OK || !SMAA_EdgeDetectionVS)
                    SAFE_RELEASE(SMAA_EdgeDetectionVS);
            }
            SAFE_RELEASE(bf1);
            SAFE_RELEASE(bf2);
            SAFE_RELEASE(ppConstantTable);
        }

        if (!SMAA_BlendingWeightsCalculationVS && !loadCompiledShader(IDR_SMAA_BlendingWeightsCalculationVS_compiled, SMAA_BlendingWeightsCalculationVS))
        {
            if (D3DXCompileShaderFromResourceW(hm, MAKEINTRESOURCEW(IDR_SMAA), NULL, NULL, "DX9_SMAABlendingWeightCalculationVS", "vs_3_0", 0, &bf1, &bf2, &ppConstantTable) == S_OK)
            {
                if (pDevice->CreateVertexShader((DWORD*)bf1->GetBufferPointer(), &SMAA_BlendingWeightsCalculationVS) != S_OK || !SMAA_BlendingWeightsCalculationVS)
                    SAFE_RELEASE(SMAA_BlendingWeightsCalculationVS);
            }
            SAFE_RELEASE(bf1);
            SAFE_RELEASE(bf2);
            SAFE_RELEASE(ppConstantTable);
        }

        if (!SMAA_NeighborhoodBlendingVS && !loadCompiledShader(IDR_SMAA_NeighborhoodBlendingVS_compiled, SMAA_NeighborhoodBlendingVS))
        {
            if (D3DXCompileShaderFromResourceW(hm, MAKEINTRESOURCEW(IDR_SMAA), NULL, NULL, "DX9_SMAANeighborhoodBlendingVS", "vs_3_0", 0, &bf1, &bf2, &ppConstantTable) == S_OK)
            {
                if (pDevice->CreateVertexShader((DWORD*)bf1->GetBufferPointer(), &SMAA_NeighborhoodBlendingVS) != S_OK || !SMAA_NeighborhoodBlendingVS)
                    SAFE_RELEASE(SMAA_NeighborhoodBlendingVS);
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
            }
            SAFE_RELEASE(bf1);
            SAFE_RELEASE(bf2);
        }

        // Tried once: one that does not build would ask again on every device reset
        static bool aoEffectTried = false;
        if (!AOEffect && !aoEffectTried)
        {
            aoEffectTried = true;
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
            if (CreateEffectCached(rage::grcDevice::GetD3DDevice(), hm, IDR_AO_FX, "AO", defines, &AOEffect, &errors) != S_OK)
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
                         { &AOEffectHandles.fTemporalBlend, "fTemporalBlend" }, { &AOEffectHandles.vec2AOTexelSize, "vec2AOTexelSize" },
                         { &AOEffectHandles.fUpsample, "fUpsample" }, { &AOEffectHandles.fUpsampleMip, "fUpsampleMip" } })
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
            SAFE_RELEASE(errors);
        }

        static bool ssrEffectTried = false;
        if (!SSREffect && !ssrEffectTried)
        {
            ssrEffectTried = true;
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
            if (CreateEffectCached(rage::grcDevice::GetD3DDevice(), hm, IDR_SSR_FX, "SSR", defines, &SSREffect, &errors) != S_OK)
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
                h.fNeighbourFill = SSREffect->GetParameterByName(nullptr, "fNeighbourFill");
                h.fFillMinDistance = SSREffect->GetParameterByName(nullptr, "fFillMinDistance");
                h.fVehicleScale = SSREffect->GetParameterByName(nullptr, "fVehicleScale");
                h.fSpreadRadius = SSREffect->GetParameterByName(nullptr, "fSpreadRadius");
                h.vec4SunView = SSREffect->GetParameterByName(nullptr, "vec4SunView");
                h.fCSLength = SSREffect->GetParameterByName(nullptr, "fCSLength");
                h.fCSThickness = SSREffect->GetParameterByName(nullptr, "fCSThickness");
                h.fCSMaxViewDistance = SSREffect->GetParameterByName(nullptr, "fCSMaxViewDistance");
                h.fCSIntensity = SSREffect->GetParameterByName(nullptr, "fCSIntensity");
                h.fCSGrassGloss = SSREffect->GetParameterByName(nullptr, "fCSGrassGloss");
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
                h.techGIContactDenoise = SSREffect->GetTechniqueByName("GIContactDenoise");
                h.techGIContactTemporal = SSREffect->GetTechniqueByName("GIContactTemporal");
                h.techGIContactUpsample = SSREffect->GetTechniqueByName("GIContactUpsample");
                h.ContactInTex2D = SSREffect->GetParameterByName(nullptr, "ContactInTex2D");
                h.ContactHistTex2D = SSREffect->GetParameterByName(nullptr, "ContactHistTex2D");
                h.fContactBlend = SSREffect->GetParameterByName(nullptr, "fContactBlend");
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
            SAFE_RELEASE(errors);
        }

        // Like the clouds: without it the ground stays dry.
        static bool wetGroundEffectTried = false;
        if (!WetGroundEffect && !wetGroundEffectTried)
        {
            wetGroundEffectTried = true;
            ID3DXBuffer* errors = nullptr;
            hrWetGroundEffect = CreateEffectCached(rage::grcDevice::GetD3DDevice(), hm, IDR_WETGROUND_FX, "WetGround", nullptr, &WetGroundEffect, &errors);
            if (hrWetGroundEffect != S_OK)
            {
                WetGroundEffect = nullptr;
                if (errors)
                    MessageBoxA(nullptr, (LPCSTR)errors->GetBufferPointer(), "Error building shader!", MB_OK);
            }
            SAFE_RELEASE(errors);
        }

        // Without it nothing is culled by occlusion.
        static bool hiZEffectTried = false;
        if (!HiZEffect && !hiZEffectTried && Occlusion::Enabled())
        {
            hiZEffectTried = true;
            ID3DXBuffer* errors = nullptr;
            hrHiZEffect = CreateEffectCached(rage::grcDevice::GetD3DDevice(), hm, IDR_HIZ_FX, "HiZ", nullptr, &HiZEffect, &errors);
            if (hrHiZEffect != S_OK)
            {
                HiZEffect = nullptr;
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
            hrHeadlightGlintsEffect = CreateEffectCached(rage::grcDevice::GetD3DDevice(), hm, IDR_HEADLIGHTGLINTS_FX, "HeadlightGlints", nullptr, &HeadlightGlintsEffect, &errors);
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
            hrCloudsEffect = CreateEffectCached(rage::grcDevice::GetD3DDevice(), hm, IDR_CLOUDS_FX, "Clouds", nullptr, &CloudsEffect, &errors);
            if (hrCloudsEffect != S_OK)
            {
                CloudsEffect = nullptr;
                if (errors)
                    MessageBoxA(nullptr, (LPCSTR)errors->GetBufferPointer(), "Error building shader!", MB_OK);
            }
            SAFE_RELEASE(errors);
        }

        // Like the clouds': without it the sky keeps the timecycle's gradient.
        static bool skyEffectTried = false;
        if (!SkyEffect && !skyEffectTried)
        {
            skyEffectTried = true;
            ID3DXBuffer* errors = nullptr;
            hrSkyEffect = CreateEffectCached(rage::grcDevice::GetD3DDevice(), hm, IDR_SKYATMOSPHERE_FX, "SkyAtmosphere", nullptr, &SkyEffect, &errors);
            if (hrSkyEffect != S_OK)
            {
                SkyEffect = nullptr;
                if (errors)
                    MessageBoxA(nullptr, (LPCSTR)errors->GetBufferPointer(), "Error building shader!", MB_OK);
            }
            SAFE_RELEASE(errors);
        }

        TemporalAA::LoadShaders(pDevice);

        return ShadersFinishedLoading();
    }

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

    // A setting of the ini read straight into a field: a float or an integer clamped to [lo, hi], or a flag (not 0).
    // The settings read otherwise, or that depend on others, follow their table in ReadLiveIni and Readini.
    struct IniSetting
    {
        enum class Kind : uint8_t { Float, Int, Flag } kind;
        const char* section;
        const char* key;
        float PostFxResource::* floatField;
        int32_t PostFxResource::* intField;
        bool PostFxResource::* flagField;
        float floatDefault, floatMin, floatMax;
        int32_t intDefault, intMin, intMax;
    };

    static constexpr IniSetting IniFloat(const char* section, const char* key, float PostFxResource::* field, float def, float lo, float hi)
    {
        return { IniSetting::Kind::Float, section, key, field, nullptr, nullptr, def, lo, hi, 0, 0, 0 };
    }

    static constexpr IniSetting IniInt(const char* section, const char* key, int32_t PostFxResource::* field, int32_t def, int32_t lo, int32_t hi)
    {
        return { IniSetting::Kind::Int, section, key, nullptr, field, nullptr, 0.0f, 0.0f, 0.0f, def, lo, hi };
    }

    static constexpr IniSetting IniFlag(const char* section, const char* key, bool PostFxResource::* field, int32_t def)
    {
        return { IniSetting::Kind::Flag, section, key, nullptr, nullptr, field, 0.0f, 0.0f, 0.0f, def, 0, 0 };
    }

    template <size_t N>
    void ReadIniSettings(CIniReader& iniReader, const IniSetting (&settings)[N])
    {
        for (const auto& s : settings)
        {
            switch (s.kind)
            {
            case IniSetting::Kind::Float:
                this->*s.floatField = std::clamp(iniReader.ReadFloat(s.section, s.key, s.floatDefault), s.floatMin, s.floatMax);
                break;
            case IniSetting::Kind::Int:
                this->*s.intField = std::clamp(iniReader.ReadInteger(s.section, s.key, s.intDefault), s.intMin, s.intMax);
                break;
            case IniSetting::Kind::Flag:
                this->*s.flagField = iniReader.ReadInteger(s.section, s.key, s.intDefault) != 0;
                break;
            }
        }
    }

    // The settings used afresh every frame, which Ctrl+Shift+F10 reads again from the ini while the
    // game runs (TickIniReload). Anything that sizes targets or builds shaders stays in Readini.
    void ReadLiveIni(CIniReader& iniReader)
    {
        static constexpr IniSetting kSettings[] =
        {
            IniFloat("LIGHTING", "ContactShadowsIntensity", &PostFxResource::fContactShadowIntensity, 1.0f, 0.0f, 1.0f),
            IniFloat("LIGHTING", "IndirectLightOcclusion", &PostFxResource::fGIOcclusion, 1.0f, 0.0f, 1.0f),
            IniFloat("LIGHTING", "LocalContactShadowsIntensity", &PostFxResource::fLocalContactShadowIntensity, 1.0f, 0.0f, 1.0f),
            IniFloat("LIGHTING", "LocalContactShadowsWithoutShadowMap", &PostFxResource::fLocalContactShadowUnshadowed, 1.0f, 0.0f, 1.0f),
            IniFloat("NIGHTSHADOWS", "VehicleBoxShadowsLightSize", &PostFxResource::fVehicleBoxShadowLightSize, 0.5f, 0.0f, 4.0f),
            IniFloat("NIGHTSHADOWS", "VehicleBoxShadowsRounding", &PostFxResource::fVehicleBoxShadowRounding, 0.2f, 0.0f, 0.99f),
            IniFloat("NIGHTSHADOWS", "VehicleBoxShadowsContact", &PostFxResource::fVehicleBoxShadowContact, 0.0f, 0.0f, 1.0f),
            IniFloat("NIGHTSHADOWS", "VehicleBoxShadowsEndRounding", &PostFxResource::fVehicleBoxShadowEndRounding, 0.3f, 0.0f, 2.0f),
            IniFlag("NIGHTSHADOWS", "VehicleBoxShadowsFromCarLights", &PostFxResource::bVehicleBoxShadowsFromCarLights, 0),
            IniFlag("NIGHTSHADOWS", "PostShadowsFromCarLights", &PostFxResource::bPostShadowsFromCarLights, 0),
            IniFlag("NIGHTSHADOWS", "VehicleBoxShadowsWithSlots", &PostFxResource::bVehicleBoxShadowsWithSlots, 0),
            IniFlag("NIGHTSHADOWS", "VehicleBoxShadowsUntilLeaving", &PostFxResource::bVehicleBoxShadowsUntilLeaving, 1),
            // VehicleBoxShadowsBlend: the light shaders take the darkest of a light's shadow map, contact shadow and boxes
            // (local_light_with_shadow_blend.patch) instead of all three multiplied, so the contact shadow goes with the
            // boxes whole. Each car still has one kind of shadow from a lamp, its map's or its box (UntilLeaving): kept with
            // a slot too, the box and the map's shadow showed side by side, two shadows from one lamp.
            IniFlag("NIGHTSHADOWS", "VehicleBoxShadowsBlend", &PostFxResource::bVehicleBoxShadowsBlend, 1),
            IniFlag("NIGHTSHADOWS", "VehicleBoxShadowsLive", &PostFxResource::bVehicleBoxShadowsLive, 0),
            IniFloat("NIGHTSHADOWS", "VehicleBoxShadowsLeadFrames", &PostFxResource::fVehicleBoxShadowLeadFrames, -1.0f, -3.0f, 5.0f),
            IniFlag("DEBUG", "VehicleBoxShadowsLog", &PostFxResource::bVehicleBoxShadowsLog, 0),
            IniFlag("NIGHTSHADOWS", "PedCapsuleShadows", &PostFxResource::bPedCapsuleShadows, 1),
            IniFloat("NIGHTSHADOWS", "PedCapsuleRadius", &PostFxResource::fPedCapsuleRadius, 0.25f, 0.05f, 1.0f),
            IniFloat("NIGHTSHADOWS", "PedCapsuleHeight", &PostFxResource::fPedCapsuleHeight, 1.75f, 0.5f, 3.0f),
            IniFloat("NIGHTSHADOWS", "PedCapsuleFootBelow", &PostFxResource::fPedCapsuleFootBelow, 1.0f, -1.0f, 2.0f),
            IniFlag("NIGHTSHADOWS", "PedCapsuleAddedLights", &PostFxResource::bPedCapsuleAddedLights, 1),
            IniFloat("NIGHTSHADOWS", "PedCapsuleLightSize", &PostFxResource::fPedCapsuleLightSize, 0.26f, 0.0f, 4.0f),
            IniFloat("NIGHTSHADOWS", "PedCapsuleSoftening", &PostFxResource::fPedCapsuleSoftening, 0.0f, 0.0f, 1.0f),
            IniFloat("NIGHTSHADOWS", "PedCapsuleBaseSoftness", &PostFxResource::fPedCapsuleBaseSoftness, 0.088f, 0.01f, 1.0f),
            IniFloat("NIGHTSHADOWS", "PedCapsuleDistanceSoftness", &PostFxResource::fPedCapsuleDistanceSoftness, 0.03f, 0.0f, 0.5f),
            IniFlag("NIGHTSHADOWS", "PedBoneShadows", &PostFxResource::bPedBoneShadows, 1),
            IniFloat("NIGHTSHADOWS", "PedBoneRadiusScale", &PostFxResource::fPedBoneRadiusScale, 0.8f, 0.3f, 2.0f),
            IniFloat("NIGHTSHADOWS", "PedBoneShoulderWidth", &PostFxResource::fPedBoneShoulderWidth, 0.8f, 0.3f, 1.2f),
            IniFlag("NIGHTSHADOWS", "PedBoneShadowsFollowDraw", &PostFxResource::bPedBoneShadowsFollowDraw, 1),
            IniFlag("NIGHTSHADOWS", "ShapeGroups", &PostFxResource::bShapeGroups, 1),
            IniFlag("PERFORMANCE", "FusedLightingPasses", &PostFxResource::bFusedLightingPasses, 1),
            IniFlag("PERFORMANCE", "DepthBoundsLights", &PostFxResource::bDepthBoundsLights, 1),
            IniFlag("PERFORMANCE", "LightEarlyOut", &PostFxResource::bLightEarlyOut, 1),
            IniFlag("PERFORMANCE", "LightVolumeDepth", &PostFxResource::bLightVolumeDepth, 1),
            IniFlag("NIGHTSHADOWS", "PropShapeShadows", &PostFxResource::bPropShapeShadows, 1),
            IniFlag("NIGHTSHADOWS", "PropsInLampCaches", &PostFxResource::bPropsInLampCaches, 1),
            IniFloat("NIGHTSHADOWS", "PropShapeDistance", &PostFxResource::fPropShapeDistance, 80.0f, 10.0f, 200.0f),
            IniFloat("NIGHTSHADOWS", "PropShapePostRadius", &PostFxResource::fPropShapePostRadius, 0.12f, 0.03f, 0.5f),
            IniFloat("NIGHTSHADOWS", "PropShapeArmRadius", &PostFxResource::fPropShapeArmRadius, 0.08f, 0.02f, 0.5f),
            IniInt("NIGHTSHADOWS", "PropShapesPerLight", &PostFxResource::nPropShapesPerLight, 12, 1, 32),
            IniFloat("NIGHTSHADOWS", "PropShapeScale", &PostFxResource::fPropShapeScale, 0.85f, 0.3f, 1.2f),
            IniFlag("NIGHTSHADOWS", "PropShapeViewCulling", &PostFxResource::bPropShapeViewCulling, 1),
            IniInt("NIGHTSHADOWS", "VehicleBoxShadowsPerLamp", &PostFxResource::nVehicleBoxShadowsPerLamp, 3, 1, 5),
            IniInt("NIGHTSHADOWS", "VehicleBoxShadowsPerCar", &PostFxResource::nVehicleBoxShadowsPerCar, 2, 0, 8),
            IniFloat("NIGHTSHADOWS", "VehicleBoxShadowsMinLight", &PostFxResource::fVehicleBoxShadowsMinLight, 0.04f, 0.0f, 1.0f),
            IniFloat("NIGHTSHADOWS", "VehicleBoxShadowsMaxDistance", &PostFxResource::fVehicleBoxShadowsMaxDistance, 200.0f, 0.0f, 1000.0f),
            IniFloat("NIGHTSHADOWS", "VehicleBoxShadowsFollowLight", &PostFxResource::fVehicleBoxShadowFollowLight, 1.0f, 0.0f, 1.0f),
            IniFloat("NIGHTSHADOWS", "VehicleBoxShadowsLowLightNarrow", &PostFxResource::fVehicleBoxShadowLowLightNarrow, 1.0f, 0.2f, 1.0f),
            IniFloat("NIGHTSHADOWS", "VehicleBoxShadowsLowLightHeight", &PostFxResource::fVehicleBoxShadowLowLightHeight, 0.6f, 0.2f, 1.0f),
            IniFloat("LIGHTING", "SkinLighting", &PostFxResource::fSkinLighting, 1.0f, 0.0f, 2.0f),
            IniFloat("LIGHTING", "SpecularSheen", &PostFxResource::fSpecularSheen, 0.1f, 0.0f, 50.0f),
            IniFloat("LIGHTING", "GGXLights", &PostFxResource::fLightsGGX, 1.0f, 0.0f, 4.0f),
            IniFloat("LIGHTING", "GGXFresnel", &PostFxResource::fLightsGGXFresnel, 0.5f, 0.0f, 1.0f),
            IniFloat("LIGHTING", "GGXLampSize", &PostFxResource::fLightsGGXSize, 0.05f, 0.0f, 2.0f),
            IniFloat("LIGHTING", "GGXStretch", &PostFxResource::fLightsGGXStretch, 0.5f, 0.0f, 4.0f),
            IniFloat("LIGHTING", "GGXHeadlightSpacing", &PostFxResource::fLightsGGXHeadlights, 0.65f, 0.0f, 2.0f),
            IniFloat("LIGHTING", "GGXFillLights", &PostFxResource::fLightsGGXFillLights, 0.5f, 0.0f, 2.0f),
            IniFloat("LIGHTING", "GGXSun", &PostFxResource::fLightsGGXSun, 1.0f, 0.0f, 4.0f),
            IniFloat("LIGHTING", "GGXEnvironment", &PostFxResource::fLightsGGXEnvironment, 1.0f, 0.0f, 1.0f),
            IniFloat("LIGHTING", "GGXMax", &PostFxResource::fLightsGGXMax, 2.0f, 0.1f, 16.0f),
            IniFloat("REFLECTIONS", "SSRIntensity", &PostFxResource::fSSRIntensity, 1.0f, 0.0f, 1.0f),
            IniFloat("REFLECTIONS", "SSRWetGround", &PostFxResource::fSSRWetGround, 1.5f, 0.0f, 8.0f),
            IniFloat("REFLECTIONS", "SSRStepPixels", &PostFxResource::fSSRStepPixels, 2.0f, 1.0f, 8.0f),
            IniFloat("REFLECTIONS", "SSRSmoothing", &PostFxResource::fSSRDenoiseRadius, 2.0f, 0.0f, 8.0f),
            IniFlag("REFLECTIONS", "SSRPastThinObjects", &PostFxResource::bSSRPassThinObjects, 1),
            IniFlag("REFLECTIONS", "SSRStepJitter", &PostFxResource::bSSRStepJitter, 1),
            IniFlag("REFLECTIONS", "SSRTemporalJitter", &PostFxResource::bSSRTemporalJitter, 1),
            IniFloat("REFLECTIONS", "SSRTemporal", &PostFxResource::fSSRTemporalBlend, 0.85f, 0.0f, 0.97f),
            IniFloat("REFLECTIONS", "SSRFallback", &PostFxResource::fSSRFallback, 0.8f, 0.0f, 1.0f),
            IniFloat("REFLECTIONS", "SSRNeighbourFill", &PostFxResource::fSSRNeighbourFill, 0.8f, 0.0f, 1.0f),
            IniFloat("REFLECTIONS", "SSRNeighbourFillMinDistance", &PostFxResource::fSSRNeighbourFillMinDistance, 3.0f, 0.0f, 50.0f),
            IniFloat("LIGHTING", "GGXSoft", &PostFxResource::fLightsGGXSoft, 0.0f, 0.0f, 1.0f),
            IniFloat("LIGHTING", "GGXStretchView", &PostFxResource::fLightsGGXStretchView, 1.0f, 0.0f, 1.0f),
            IniFloat("LIGHTING", "GGXGlints", &PostFxResource::fLightsGGXGlints, 1.0f, 0.0f, 8.0f),
            IniFloat("LIGHTING", "GGXGlintsSize", &PostFxResource::fLightsGGXGlintsSize, 0.1f, 0.01f, 1.0f),
            IniFloat("LIGHTING", "GGXGlintsGloss", &PostFxResource::fLightsGGXGlintsGloss, 0.5f, 0.0f, 1.0f),
            IniFloat("LIGHTING", "GGXGlintsMax", &PostFxResource::fLightsGGXGlintsMax, 64.0f, 1.0f, 1000.0f),
            IniFloat("WEATHER", "WetGround", &PostFxResource::fWetGround, 1.0f, 0.0f, 1.0f),
            IniFloat("WEATHER", "WetGroundPuddles", &PostFxResource::fWetGroundPuddles, 0.35f, 0.0f, 1.0f),
            IniFloat("WEATHER", "WetGroundPuddleSize", &PostFxResource::fWetGroundPuddleSize, 24.0f, 2.0f, 500.0f),
            IniFloat("WEATHER", "WetGroundRipples", &PostFxResource::fWetGroundRipples, 1.0f, 0.0f, 3.0f),
            IniFloat("WEATHER", "WetGroundDarkening", &PostFxResource::fWetGroundDarkening, 1.0f, 0.0f, 2.0f),
            IniFloat("WEATHER", "WetGroundWetting", &PostFxResource::fWetGroundWetting, 30.0f, 0.0f, 3600.0f),
            IniFloat("WEATHER", "WetGroundDrying", &PostFxResource::fWetGroundDrying, 240.0f, 0.0f, 3600.0f),
            IniFloat("WEATHER", "CloudShadows", &PostFxResource::fCloudShadows, 0.6f, 0.0f, 1.0f),
            IniFloat("WEATHER", "CloudShadowsHeight", &PostFxResource::fCloudShadowsHeight, 1200.0f, 100.0f, 10000.0f),
            IniFloat("WEATHER", "CloudShadowsScale", &PostFxResource::fCloudShadowsScale, 16000.0f, 100.0f, 50000.0f),
            IniFloat("WEATHER", "CloudShadowsWind", &PostFxResource::fCloudShadowsWind, 6.0f, 0.0f, 100.0f),
            IniFloat("WEATHER", "CloudShadowsSoftness", &PostFxResource::fCloudShadowsSoftness, 3.0f, 0.0f, 8.0f),
            IniFloat("WEATHER", "CloudShadowsCoverage", &PostFxResource::fCloudShadowsCoverage, 0.0f, -1.0f, 1.0f),
            IniFlag("WEATHER", "VolumetricClouds", &PostFxResource::bVolumetricClouds, 1),
            IniFloat("WEATHER", "VolumetricCloudsCoverage", &PostFxResource::fVolumetricCloudsCoverage, 0.4f, 0.0f, 1.0f),
            IniFloat("WEATHER", "VolumetricCloudsBase", &PostFxResource::fVolumetricCloudsBase, 800.0f, 50.0f, 10000.0f),
            IniFloat("WEATHER", "VolumetricCloudsThickness", &PostFxResource::fVolumetricCloudsThickness, 600.0f, 50.0f, 5000.0f),
            IniFloat("WEATHER", "VolumetricCloudsDensity", &PostFxResource::fVolumetricCloudsDensity, 0.03f, 0.0005f, 1.0f),
            IniFloat("WEATHER", "VolumetricCloudsDetail", &PostFxResource::fVolumetricCloudsDetail, 0.6f, 0.0f, 1.0f),
            IniFloat("WEATHER", "VolumetricCloudsDetailScale", &PostFxResource::fVolumetricCloudsDetailScale, 1300.0f, 20.0f, 10000.0f),
            IniFloat("WEATHER", "VolumetricCloudsHaze", &PostFxResource::fVolumetricCloudsHaze, 25000.0f, 1000.0f, 200000.0f),
            IniFloat("WEATHER", "VolumetricCloudsMaxDistance", &PostFxResource::fVolumetricCloudsMaxDistance, 40000.0f, 1000.0f, 200000.0f),
            IniFloat("WEATHER", "VolumetricCloudsBrightness", &PostFxResource::fVolumetricCloudsBrightness, 1.0f, 0.0f, 4.0f),
            IniFloat("WEATHER", "VolumetricCloudsSunTint", &PostFxResource::fVolumetricCloudsSunTint, 0.6f, 0.0f, 1.0f),
            IniFloat("WEATHER", "VolumetricCloudsMoonlight", &PostFxResource::fVolumetricCloudsMoonlight, 0.2f, 0.0f, 2.0f),
            IniFloat("WEATHER", "VolumetricCloudsSkyLight", &PostFxResource::fVolumetricCloudsSkyLight, 0.5f, 0.0f, 1.0f),
            IniFloat("WEATHER", "VolumetricCloudsMinLight", &PostFxResource::fVolumetricCloudsMinLight, 0.1f, 0.0f, 0.9f),
            IniFlag("WEATHER", "VolumetricCloudsScatterTail", &PostFxResource::bVolumetricCloudsScatterTail, 1),
            IniFlag("WEATHER", "VolumetricCloudsLightJitter", &PostFxResource::bVolumetricCloudsLightJitter, 1),
            IniFloat("WEATHER", "VolumetricCloudsSkyMatch", &PostFxResource::fVolumetricCloudsSkyMatch, 2.0f, 0.0f, 20.0f),
            IniFlag("WEATHER", "VolumetricCloudsWeather", &PostFxResource::bVolumetricCloudsWeather, 1),
            IniFlag("WEATHER", "VolumetricCloudsPerlinWorley", &PostFxResource::bVolumetricCloudsPerlinWorley, 1),
            IniFloat("WEATHER", "VolumetricCloudsLumps", &PostFxResource::fVolumetricCloudsLumps, 50.0f, 0.0f, 1000.0f),
            IniFlag("WEATHER", "PbrSkyHorizonHue", &PostFxResource::bPbrSkyHorizonHue, 0),
            IniFloat("WEATHER", "VolumetricCloudsVanilla", &PostFxResource::fVolumetricCloudsVanilla, 0.0f, 0.0f, 1.0f),
            IniFloat("WEATHER", "VolumetricCloudsTranslucency", &PostFxResource::fVolumetricCloudsTranslucency, 0.25f, 0.0f, 0.9f),
            IniFloat("WEATHER", "VolumetricCloudsEvolution", &PostFxResource::fVolumetricCloudsEvolution, 1.0f, 0.0f, 10.0f),
            IniFloat("WEATHER", "VolumetricCloudsSaturation", &PostFxResource::fVolumetricCloudsSaturation, 1.0f, 0.0f, 2.0f),
            IniFloat("WEATHER", "VolumetricCloudsMottle", &PostFxResource::fVolumetricCloudsMottle, 0.4f, 0.0f, 1.0f),
            IniFloat("WEATHER", "VolumetricCloudsDetailFlow", &PostFxResource::fVolumetricCloudsDetailFlow, 0.0f, 0.0f, 4.0f),
            IniFloat("WEATHER", "VolumetricCloudsShade", &PostFxResource::fVolumetricCloudsShade, 0.65f, 0.0f, 2.0f),
            IniFloat("WEATHER", "VolumetricCloudsAbsorption", &PostFxResource::fVolumetricCloudsAbsorption, 0.35f, 0.05f, 3.0f),
            IniFlag("REFLECTIONS", "CloudReflections", &PostFxResource::bVolumetricCloudsReflections, 1),
            IniFlag("PERFORMANCE", "CloudReflectionsAtNight", &PostFxResource::bVolumetricCloudsReflectionsAtNight, 0),
            IniFloat("REFLECTIONS", "CloudReflectionsBrightness", &PostFxResource::fVolumetricCloudsReflectionBrightness, 1.0f, 0.0f, 10.0f),
            IniFlag("PERFORMANCE", "CloudReflectionsHalfResolution", &PostFxResource::bVolumetricCloudsReflectionsHalfRes, 1),
            IniFlag("PERFORMANCE", "CloudsHalfResolution", &PostFxResource::bVolumetricCloudsHalfRes, 1),
            IniFloat("REFLECTIONS", "SSREdgeFade", &PostFxResource::fSSREdgeFade, 0.1f, 0.001f, 0.5f),
            IniFloat("REFLECTIONS", "SSRGlossBoost", &PostFxResource::fSSRGlossBoost, 2.0f, 0.0f, 8.0f),
            IniFloat("REFLECTIONS", "SSRGlossCutoff", &PostFxResource::fSSRGlossCutoff, 0.5f, 0.0f, 1.0f),
            IniFloat("REFLECTIONS", "SSRWaterIntensity", &PostFxResource::fSSRWaterIntensity, 1.0f, 0.0f, 1.0f),
            IniFloat("REFLECTIONS", "SSRWaterBlur", &PostFxResource::fSSRWaterBlur, 3.0f, 0.0f, 32.0f),
            IniFloat("REFLECTIONS", "SSRWaterRipple", &PostFxResource::fSSRWaterNormalStrength, 1.0f, 0.0f, 4.0f),
            IniFlag("REFLECTIONS", "SSRGBufferNormals", &PostFxResource::bSSRGBufferNormals, 1),
            IniFloat("REFLECTIONS", "SSRTowardCamera", &PostFxResource::fSSRTowardCamera, 0.0f, 0.0f, 1.0f),
            IniFloat("REFLECTIONS", "SSRBlur", &PostFxResource::fSSRReflectionBlur, 0.0f, 0.0f, 32.0f),
            IniFloat("REFLECTIONS", "SSRDistanceFade", &PostFxResource::fSSRDistanceFade, 0.0f, 0.0f, 100.0f),
            IniFloat("REFLECTIONS", "SSRVehicleUnits", &PostFxResource::fSSRVehicleUnits, 4.0f, 0.0f, 100.0f),
            IniFloat("LIGHTING", "ContactShadowsLength", &PostFxResource::fContactShadowLength, 0.3f, 0.05f, 10.0f),
            IniFloat("LIGHTING", "ContactShadowsThickness", &PostFxResource::fContactShadowThickness, 0.15f, 0.01f, 10.0f),
            IniFloat("LIGHTING", "ContactShadowsMaxDistance", &PostFxResource::fContactShadowMaxDistance, 60.0f, 1.0f, 1000.0f),
            IniFlag("LIGHTING", "ContactShadowsMoon", &PostFxResource::bContactShadowsMoon, 1),
            IniFlag("LIGHTING", "ContactShadowsStepJitter", &PostFxResource::bContactShadowStepJitter, 1),
            IniFloat("LIGHTING", "ContactShadowsTemporal", &PostFxResource::fContactTemporalBlend, 0.8f, 0.0f, 0.95f),
            IniFloat("LIGHTING", "IndirectLightIntensity", &PostFxResource::fGIIntensity, 1.5f, 0.0f, 8.0f),
            IniFloat("LIGHTING", "IndirectLightMaxBrightness", &PostFxResource::fGIMaxBrightness, 4.0f, 0.05f, 8.0f),
            IniFloat("LIGHTING", "SkinScatteringWidth", &PostFxResource::fSkinScatteringWidth, 0.03f, 0.001f, 0.1f),
            IniFloat("LIGHTING", "SkinScatteringStrength", &PostFxResource::fSkinScatteringStrength, 1.0f, 0.0f, 2.0f),
            IniFloat("LIGHTING", "IndirectLightRayLength", &PostFxResource::fGIRayLength, 4.0f, 0.1f, 20.0f),
            IniFloat("LIGHTING", "IndirectLightThickness", &PostFxResource::fGIThickness, 0.5f, 0.01f, 10.0f),
            IniFloat("LIGHTING", "IndirectLightMaxDistance", &PostFxResource::fGIMaxDistance, 60.0f, 1.0f, 1000.0f),
            IniFloat("LIGHTING", "IndirectLightTemporal", &PostFxResource::fGITemporalBlend, 0.9f, 0.0f, 0.97f),
            IniFlag("LIGHTING", "LocalContactShadows", &PostFxResource::bLocalContactShadows, 1),
            IniFloat("LIGHTING", "LocalContactShadowsLength", &PostFxResource::fLocalContactShadowLength, 0.5f, 0.05f, 10.0f),
            IniFloat("LIGHTING", "LocalContactShadowsThickness", &PostFxResource::fLocalContactShadowThickness, 0.2f, 0.01f, 5.0f),
            IniFloat("LIGHTING", "LocalContactShadowsMaxDistance", &PostFxResource::fLocalContactShadowMaxDistance, 40.0f, 1.0f, 1000.0f),
            IniFloat("LIGHTING", "VolumetricLightIntensity", &PostFxResource::fVolumetricLightIntensity, 4.0f, 0.0f, 20.0f),
            IniFloat("LIGHTING", "VolumetricLightScale", &PostFxResource::fVolumetricLightScale, 0.25f, 0.0f, 2.0f),
            IniFloat("LIGHTING", "VolumetricLightMaxDistance", &PostFxResource::fVolumetricLightMaxDistance, 100.0f, 10.0f, 1000.0f),
            IniFloat("LIGHTING", "VolumetricLightHeadlightIntensity", &PostFxResource::fVolumetricLightHeadlightIntensity, 2.0f, 0.0f, 20.0f),
            IniFloat("LIGHTING", "VolumetricLightHeadlightLength", &PostFxResource::fVolumetricLightHeadlightLength, 25.0f, 1.0f, 200.0f),
            IniFlag("LIGHTING", "FillLights", &PostFxResource::bFillLights, 0),
            IniFloat("LIGHTING", "FillLightsMinRadius", &PostFxResource::fFillLightsMinRadius, 30.0f, 0.0f, 1000.0f),
            IniFloat("LIGHTING", "VolumetricLightHeadlightPitch", &PostFxResource::fVolumetricLightHeadlightPitch, 6.0f, -45.0f, 45.0f),
            IniFlag("REFLECTIONS", "SSRCarWindows", &PostFxResource::bGlassReflections, 1),
            IniFloat("REFLECTIONS", "SSRCarWindowsLength", &PostFxResource::fGlassReflectionsLength, 15.0f, 1.0f, 100.0f),
            IniFloat("REFLECTIONS", "SSRCarWindowsThickness", &PostFxResource::fGlassReflectionsThickness, 0.5f, 0.05f, 10.0f),
            IniFlag("REFLECTIONS", "SSRCarWindowsStepJitter", &PostFxResource::bGlassStepJitter, 1),
            IniFloat("POSTFX", "AmbientOcclusionGTAOStrength", &PostFxResource::fAmbientOcclusionGTAOStrength, 1.0f, 0.0f, 4.0f),
            IniFloat("POSTFX", "AmbientOcclusionGTAOThinOccluders", &PostFxResource::fAmbientOcclusionGTAOThinOccluders, 0.5f, 0.0f, 1.0f),
            IniFlag("POSTFX", "AmbientOcclusionGTAOMultiBounce", &PostFxResource::bAmbientOcclusionMultiBounce, 1),
        };
        ReadIniSettings(iniReader, kSettings);
        fSSRMaxDistance = std::max(1.0f, iniReader.ReadFloat("REFLECTIONS", "SSRMaxDistance", 24.0f));
        fSSRThickness = std::max(0.0f, iniReader.ReadFloat("REFLECTIONS", "SSRThickness", 0.3f));
        fSSRWaterLevelOffset = iniReader.ReadFloat("REFLECTIONS", "SSRWaterLevelOffset", 0.0f);
        nVolumetricLightHeadlightAddFlags = uint32_t(iniReader.ReadInteger("LIGHTING", "VolumetricLightHeadlightAddFlags", 0));

        if (bVehicleBoxShadowsBlend)
            fVehicleBoxShadowContact = 1.0f;
        nVehicleBoxShadowSizeFadeMs = static_cast<uint32_t>(std::clamp(iniReader.ReadInteger("NIGHTSHADOWS", "VehicleBoxShadowsSizeFade", 300), 0, 5000));
        nVehicleBoxShadowFadeInMs = static_cast<uint32_t>(std::clamp(iniReader.ReadInteger("NIGHTSHADOWS", "VehicleBoxShadowsFadeIn", 300), 0, 5000));
        nVehicleBoxShadowFadeOutMs = static_cast<uint32_t>(std::clamp(iniReader.ReadInteger("NIGHTSHADOWS", "VehicleBoxShadowsFadeOut", 300), 0, 5000));
        nVehicleBoxShadowSlotFadeMs = static_cast<uint32_t>(std::clamp(iniReader.ReadInteger("NIGHTSHADOWS", "VehicleBoxShadowsSlotFade", 600), 0, 5000));
        fVehicleBoxShadowScale[0] = std::clamp(iniReader.ReadFloat("NIGHTSHADOWS", "VehicleBoxShadowsLength", 1.0f), 0.3f, 1.2f);
        fVehicleBoxShadowScale[1] = std::clamp(iniReader.ReadFloat("NIGHTSHADOWS", "VehicleBoxShadowsWidth", 0.95f), 0.3f, 1.2f);
        fVehicleBoxShadowScale[2] = std::clamp(iniReader.ReadFloat("NIGHTSHADOWS", "VehicleBoxShadowsHeight", 0.95f), 0.3f, 1.2f);
        PostFxProfiler::bEnabled = iniReader.ReadInteger("DEBUG", "PostFxProfiler", 0) != 0;
        nWetGroundMaterials = iniReader.ReadInteger("WEATHER", "WetGroundMaterials", 1) & 0xFF;
    }

    void Readini()
    {
        CIniReader iniReader("");
        static constexpr IniSetting kSettings[] =
        {
            IniFlag("SRF", "StippleFilter", &PostFxResource::useStippleFilter, 1),
            IniFlag("POSTFX", "EnablePreAlphaDepth", &PostFxResource::bEnablePreAlphaDepth, 1),
            IniInt("REFLECTIONS", "SSRSteps", &PostFxResource::nSSRSteps, 32, 4, 128),
            IniInt("REFLECTIONS", "SSRRefineSteps", &PostFxResource::nSSRRefineSteps, 8, 0, 16),
            IniFlag("PERFORMANCE", "ContactShadowsHalfResolution", &PostFxResource::bContactShadowsHalfRes, 1),
            IniInt("LIGHTING", "IndirectLightRays", &PostFxResource::nGIRays, 4, 1, 16),
            IniInt("LIGHTING", "IndirectLightSteps", &PostFxResource::nGISteps, 8, 2, 32),
            IniFlag("LIGHTING", "VolumetricLightHeadlightShadow", &PostFxResource::bVolumetricLightHeadlightShadow, 0),
            IniInt("POSTFX", "AmbientOcclusionGTAOSlices", &PostFxResource::nAmbientOcclusionGTAOSlices, 2, 1, 8),
            IniInt("POSTFX", "AmbientOcclusionGTAOSteps", &PostFxResource::nAmbientOcclusionGTAOSteps, 4, 1, 16),
            IniFloat("POSTFX", "AmbientOcclusionGTAOTemporal", &PostFxResource::fAmbientOcclusionTemporal, 0.9f, 0.0f, 0.98f),
            IniFlag("PERFORMANCE", "AmbientOcclusionHalfResolution", &PostFxResource::bAmbientOcclusionHalfRes, 1),
        };
        ReadIniSettings(iniReader, kSettings);

        EnablePostfx = iniReader.ReadInteger("SRF", "EnablePostfx", 1);



        ReadLiveIni(iniReader);
        nVolumetricLightHeadlightFlag = uint32_t(iniReader.ReadInteger("LIGHTING", "VolumetricLightHeadlightFlag", rage::LF_VEHICLE));

        nAmbientOcclusionBlurPasses = iniReader.ReadInteger("POSTFX", "AmbientOcclusionBlurPasses", 1);
        nAmbientOcclusionSamples = iniReader.ReadInteger("POSTFX", "AmbientOcclusionSamples", 9);
        nAmbientOcclusionLogMaxOffset = iniReader.ReadInteger("POSTFX", "AmbientOcclusionLogMaxOffset", 3);
        nAmbientOcclusionMaxMipLevel = iniReader.ReadInteger("POSTFX", "AmbientOcclusionMaxMipLevel", 5);
        fAmbientOcclusionFarClip = iniReader.ReadFloat("POSTFX", "AmbientOcclusionFarClip", 150.0f);
        fAmbientOcclusionBlurRadius = iniReader.ReadFloat("POSTFX", "AmbientOcclusionBlurRadius", 2.0f);

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
            D3DXCreateTextureFromResourceExW(RealDevice(pDevice), hm, MAKEINTRESOURCEW(IDR_AreaTex), 160, 560, 1, 0, D3DFMT_UNKNOWN, D3DPOOL_MANAGED, D3DX_FILTER_LINEAR, D3DX_FILTER_LINEAR, 0, NULL, NULL, &SMAA_areaTex);
        if (!SMAA_searchTex)
            D3DXCreateTextureFromResourceExW(RealDevice(pDevice), hm, MAKEINTRESOURCEW(IDR_SearchTex), 64, 16, 1, 0, D3DFMT_UNKNOWN, D3DPOOL_MANAGED, D3DX_FILTER_LINEAR, D3DX_FILTER_LINEAR, 0, NULL, NULL, &SMAA_searchTex);
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
    bool wide = SUCCEEDED(RealDevice(pDevice)->CreateTexture(size, size, 0, 0, D3DFMT_L16, D3DPOOL_MANAGED, &CloudNoiseTexture, nullptr));
    if (!wide && FAILED(RealDevice(pDevice)->CreateTexture(size, size, 0, 0, D3DFMT_L8, D3DPOOL_MANAGED, &CloudNoiseTexture, nullptr)))
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
// channels from different points. Managed, so it survives device resets. With
// VolumetricCloudsPerlinWorley, Perlin-Worley noise in its place (below).
IDirect3DVolumeTexture9* PostFxResource::CloudDetailTex()
{
    if (CloudDetailTexture && bCloudDetailPerlinWorley == bVolumetricCloudsPerlinWorley)
        return CloudDetailTexture;
    auto pDevice = rage::grcDevice::GetD3DDevice();
    if (!pDevice)
        return CloudDetailTexture;
    if (CloudDetailTexture)
    {
        CloudDetailTexture->Release();
        CloudDetailTexture = nullptr;
    }

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

        if (!bVolumetricCloudsPerlinWorley)
            continue;
        // Perlin-Worley: three octaves of tiling Perlin noise (4, 8 and 16 cells a tile, as the Worley's),
        // lifted by the Worley billows, remap(perlin, 0, 1, worley, 1): where the Perlin is high the gaps
        // between billows fill in, so the billows join up into plumes with wisps between them. Then given
        // the Worley's own spread of values, rank for rank, so the clouds' erosion eats as much as before
        // and only its shape changes.
        std::vector<float> lifted(value[channel].size());
        for (int z = 0; z < size; ++z)
            for (int y = 0; y < size; ++y)
                for (int x = 0; x < size; ++x)
                {
                    float perlin = 0.0f, amplitude = 1.0f, total = 0.0f;
                    for (int octave = 0; octave < 3; ++octave, amplitude *= 0.5f)
                    {
                        const int cells = cellCounts[octave];
                        const float cell = static_cast<float>(size) / cells;
                        const float fx = (x + 0.5f) / cell, fy = (y + 0.5f) / cell, fz = (z + 0.5f) / cell;
                        const int cx = static_cast<int>(fx), cy = static_cast<int>(fy), cz = static_cast<int>(fz);
                        const float tx = fx - cx, ty = fy - cy, tz = fz - cz;
                        // One of the cube's twelve edge directions at each corner.
                        auto corner = [&](int dx, int dy, int dz) {
                            static constexpr int8_t kGradients[12][3] = { { 1, 1, 0 }, { -1, 1, 0 }, { 1, -1, 0 }, { -1, -1, 0 },
                                                                          { 1, 0, 1 }, { -1, 0, 1 }, { 1, 0, -1 }, { -1, 0, -1 },
                                                                          { 0, 1, 1 }, { 0, -1, 1 }, { 0, 1, -1 }, { 0, -1, -1 } };
                            const auto& g = kGradients[hash((cx + dx) % cells, (cy + dy) % cells, (cz + dz) % cells, seeds[channel] + 100 + octave) % 12];
                            return g[0] * (tx - dx) + g[1] * (ty - dy) + g[2] * (tz - dz);
                        };
                        auto fade = [](float t) { return t * t * t * (t * (t * 6.0f - 15.0f) + 10.0f); };
                        auto mix = [](float a, float b, float t) { return a + (b - a) * t; };
                        const float ux = fade(tx), uy = fade(ty), uz = fade(tz);
                        const float n = mix(mix(mix(corner(0, 0, 0), corner(1, 0, 0), ux), mix(corner(0, 1, 0), corner(1, 1, 0), ux), uy),
                                            mix(mix(corner(0, 0, 1), corner(1, 0, 1), ux), mix(corner(0, 1, 1), corner(1, 1, 1), ux), uy), uz);
                        perlin += amplitude * (0.5f + 0.5f * n);
                        total += amplitude;
                    }
                    perlin = std::clamp(perlin / total, 0.0f, 1.0f);
                    const size_t i = (z * size + y) * size + x;
                    lifted[i] = value[channel][i] + perlin * (1.0f - value[channel][i]);
                }
        std::vector<float> spread = value[channel];
        std::sort(spread.begin(), spread.end());
        std::vector<int> order(lifted.size());
        for (size_t i = 0; i < order.size(); ++i)
            order[i] = static_cast<int>(i);
        std::stable_sort(order.begin(), order.end(), [&](int a, int b) { return lifted[a] < lifted[b]; });
        for (size_t rank = 0; rank < order.size(); ++rank)
            value[channel][order[rank]] = spread[rank];
    }

    // 16 bits a channel for the same reason as the coverage; 8 where G16R16 volumes are missing.
    bool wide = SUCCEEDED(RealDevice(pDevice)->CreateVolumeTexture(size, size, size, 1, 0, D3DFMT_G16R16, D3DPOOL_MANAGED, &CloudDetailTexture, nullptr));
    if (!wide && FAILED(RealDevice(pDevice)->CreateVolumeTexture(size, size, size, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &CloudDetailTexture, nullptr)))
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
    bCloudDetailPerlinWorley = bVolumetricCloudsPerlinWorley;
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
// overcast sheet over most of it, smooth, dense, with dark bases and little glow, churning (1.5). Fog has no clouds.
static constexpr PostFxResource::CloudLayer kWeatherClouds[8] =
{
    //  cover   base   thick   dens  strat  wind   abs   transl detail glow  sun   evol  round  match  soft
    { 0.25f,  700.0f,  900.0f, 1.67f, 0.0f, 0.50f, 0.8f, 1.3f, 1.2f,  7.0f, 0.70f, 0.4f, 0.10f, 1.00f, 0.0f }, // EXTRASUNNY
    { 0.40f,  600.0f, 1200.0f, 1.33f, 0.0f, 1.00f, 0.9f, 1.2f, 1.1f,  6.0f, 0.55f, 0.7f, 0.15f, 1.00f, 0.0f }, // SUNNY
    { 0.45f,  700.0f, 1100.0f, 0.67f, 0.0f, 1.17f, 1.0f, 1.2f, 0.87f, 6.0f, 0.50f, 0.7f, 0.12f, 1.00f, 1.0f }, // SUNNY_WINDY
    { 0.70f,  500.0f,  900.0f, 1.00f, 0.2f, 0.67f, 1.3f, 1.0f, 0.5f,  4.0f, 0.20f, 0.7f, 0.15f, 0.65f, 1.0f }, // CLOUDY
    { 0.95f,  300.0f, 1000.0f, 0.83f, 0.6f, 0.33f, 2.0f, 0.5f, 0.3f,  2.0f, 0.25f, 1.5f, 0.15f, 1.00f, 0.0f }, // RAIN
    { 0.85f,  400.0f,  900.0f, 0.67f, 0.4f, 1.00f, 1.6f, 0.7f, 0.4f,  3.0f, 0.40f, 0.8f, 0.15f, 0.80f, 1.0f }, // DRIZZLE
    { 0.00f,  600.0f,  600.0f, 0.20f, 0.3f, 1.67f, 1.0f, 1.0f, 0.8f,  3.0f, 0.30f, 0.4f, 0.15f, 1.00f, 0.0f }, // FOGGY
    { 0.95f,  300.0f, 1200.0f, 0.80f, 0.5f, 1.00f, 2.2f, 0.5f, 0.35f, 2.0f, 0.25f, 1.4f, 0.08f, 1.00f, 0.0f }, // LIGHTNING
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

export PostFxResource PostFxResources;
