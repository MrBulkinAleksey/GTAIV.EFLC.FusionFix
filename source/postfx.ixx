module;

#include <common.hxx>
#include <d3dx9tex.h>
#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <mutex>

export module postfx;

import common;
import comvars;
import d3dx9_43;
import framehistory;
import hdr;
import natives;
import settings;
import shaders;
import renderscale;
import temporal;

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
        D3DXHANDLE NormalTex2D, SSRResultTex2D, DebugTex2D, fDebugMode, techSSRDebug, techSSRDebugCopy, techWaterReflectionDebug;
        D3DXHANDLE fUseGBufferNormals;
        D3DXHANDLE PreWaterTex2D, PostWaterTex2D, fUseWaterMask, PrevDepthTex2D, fUsePrevDepth;
        D3DXHANDLE fDenoiseRadius, fDenoiseSSROnly, techSSRDenoise, fPassThinObjects, fStepJitter, fTowardCamera, fReflectionBlur, fDistanceFade, fFallback, fSpreadRadius;
        D3DXHANDLE vec4SunView, fCSLength, fCSThickness, fCSMaxViewDistance, fCSIntensity, techContactShadows;
        D3DXHANDLE techContactTemporal, vec2NoiseOffset, techContactUpsample;
        D3DXHANDLE vec2InvViewportSize, fNearPlane, fFarDivNear, vec4ProjInfo;
        D3DXHANDLE fMaxDistance, fThickness, fEdgeFade, fIntensity;
        D3DXHANDLE vec4ViewToPrevClip, fGlossBoost, fGlossCutoff, fWetness, fWetGroundBoost;
        D3DXHANDLE vec4WaterPlane, fWaterIntensity, fWaterBlur;
        D3DXHANDLE fWaterNormalStrength, vec4WaterToView, vec4WaterWorldX, vec4WaterWorldY;
        D3DXHANDLE techSSR, techSSRWater;
        D3DXHANDLE SSRAccumTex2D, fTemporalBlend, techSSRTemporal, SSRFallbackTex2D;
        D3DXHANDLE MotionTex2D, fUseMotion, vec2MotionJitter;
        D3DXHANDLE fTemporalAnySurface, fGIRayLength, fGIThickness, fGIMaxViewDistance, fGIIntensity, techSSGI;
        D3DXHANDLE fGIMaxBrightness, techGIUpsample, AlbedoTex2D, GIPrevTex2D, fGIFeedback, fGIOcclusion, fGIRespectAO;
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
    static constexpr int kSkinDebugMode = 9;
    // SSR Debug 10: the water reflection map (WATER_REFLECTION_COLOUR) over the finished frame, see
    // WaterReflectionDebug_PS in SSR.fx.
    static constexpr int kWaterReflectionDebugMode = 10;
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
    float fLocalContactShadowLength = 0.5f;
    float fLocalContactShadowThickness = 0.2f;
    float fLocalContactShadowMaxDistance = 40.0f;
    float fLocalContactShadowIntensity = 1.0f;
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
    // PostFxProfiler: GPU time of FusionFix's passes to FusionFix.PostFx.log, see ProfilerNextFrame.
    bool bPostFxProfiler = false;
    float fSSRTowardCamera = 0.0f;
    float fSSRReflectionBlur = 0.0f;
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
    // Accumulation over frames (SSRTemporal_PS in SSR.fx): each frame blends the smoothed
    // result with the previous accumulation into the other target of a pair, one pair per
    // resolution. ScreenSpaceReflectionsTemporal is the share of the history kept, 0 turns
    // it off. The history is taken where the surface was last frame.
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
    // gloss cutoff, drawn this many times brighter (fWetGroundBoost in SSR.fx); 0 turns it off.
    float fSSRWetGround = 2.0f;
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
        D3DXHANDLE AOHistoryTex2D, PrevDepthTex2D, MotionTex2D, vec4ViewToPrevClip, fUseMotion, vec2MotionJitter, fUsePrevDepth, fTemporalBlend;

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
                         { &AOEffectHandles.fUsePrevDepth, "fUsePrevDepth" }, { &AOEffectHandles.fTemporalBlend, "fTemporalBlend" } })
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
                auto& h = SSREffectHandles;
                h.DepthTex2D = SSREffect->GetParameterByName(nullptr, "DepthTex2D");
                h.HistoryTex2D = SSREffect->GetParameterByName(nullptr, "HistoryTex2D");
                h.PrevDepthTex2D = SSREffect->GetParameterByName(nullptr, "PrevDepthTex2D");
                h.fUsePrevDepth = SSREffect->GetParameterByName(nullptr, "fUsePrevDepth");
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
                h.techWaterReflectionDebug = SSREffect->GetTechniqueByName("WaterReflectionDebug");
                h.SSRAccumTex2D = SSREffect->GetParameterByName(nullptr, "SSRAccumTex2D");
                h.SSRFallbackTex2D = SSREffect->GetParameterByName(nullptr, "SSRFallbackTex2D");
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
        fSSRIntensity = std::clamp(iniReader.ReadFloat("POSTFX", "ScreenSpaceReflectionsIntensity", 1.0f), 0.0f, 1.0f);
        fSSRGlossBoost = std::clamp(iniReader.ReadFloat("POSTFX", "ScreenSpaceReflectionsGlossBoost", 2.0f), 0.0f, 8.0f);
        fSSRGlossCutoff = std::clamp(iniReader.ReadFloat("POSTFX", "ScreenSpaceReflectionsGlossCutoff", 0.5f), 0.0f, 1.0f);
        fSSRWetGround = std::clamp(iniReader.ReadFloat("POSTFX", "ScreenSpaceReflectionsWetGround", 2.0f), 0.0f, 8.0f);
        fSSRWaterIntensity = std::clamp(iniReader.ReadFloat("POSTFX", "ScreenSpaceReflectionsWaterIntensity", 1.0f), 0.0f, 1.0f);
        fSSRWaterLevelOffset = iniReader.ReadFloat("POSTFX", "ScreenSpaceReflectionsWaterLevelOffset", 0.0f);
        fSSRWaterBlur = std::clamp(iniReader.ReadFloat("POSTFX", "ScreenSpaceReflectionsWaterBlur", 3.0f), 0.0f, 32.0f);
        fSSRWaterNormalStrength = std::clamp(iniReader.ReadFloat("POSTFX", "ScreenSpaceReflectionsWaterRipple", 1.0f), 0.0f, 4.0f);
        bSSRGBufferNormals = iniReader.ReadInteger("POSTFX", "ScreenSpaceReflectionsGBufferNormals", 1) != 0;
        fSSRDenoiseRadius = std::clamp(iniReader.ReadFloat("POSTFX", "ScreenSpaceReflectionsSmoothing", 2.0f), 0.0f, 8.0f);
        bSSRPassThinObjects = iniReader.ReadInteger("POSTFX", "ScreenSpaceReflectionsPastThinObjects", 1) != 0;
        bSSRStepJitter = iniReader.ReadInteger("POSTFX", "ScreenSpaceReflectionsStepJitter", 1) != 0;
        bSSRTemporalJitter = iniReader.ReadInteger("POSTFX", "ScreenSpaceReflectionsTemporalJitter", 1) != 0;
        bPostFxProfiler = iniReader.ReadInteger("POSTFX", "PostFxProfiler", 0) != 0;
        fSSRTemporalBlend = std::clamp(iniReader.ReadFloat("POSTFX", "ScreenSpaceReflectionsTemporal", 0.85f), 0.0f, 0.97f);
        fSSRTowardCamera = std::clamp(iniReader.ReadFloat("POSTFX", "ScreenSpaceReflectionsTowardCamera", 0.0f), 0.0f, 1.0f);
        fSSRReflectionBlur = std::clamp(iniReader.ReadFloat("POSTFX", "ScreenSpaceReflectionsBlur", 0.0f), 0.0f, 32.0f);
        fSSRDistanceFade = std::clamp(iniReader.ReadFloat("POSTFX", "ScreenSpaceReflectionsDistanceFade", 0.0f), 0.0f, 100.0f);
        fSSRFallback = std::clamp(iniReader.ReadFloat("POSTFX", "ScreenSpaceReflectionsFallback", 0.8f), 0.0f, 1.0f);
        fContactShadowLength = std::clamp(iniReader.ReadFloat("POSTFX", "ContactShadowsLength", 0.3f), 0.05f, 10.0f);
        fContactShadowThickness = std::clamp(iniReader.ReadFloat("POSTFX", "ContactShadowsThickness", 0.15f), 0.01f, 10.0f);
        fContactShadowMaxDistance = std::clamp(iniReader.ReadFloat("POSTFX", "ContactShadowsMaxDistance", 60.0f), 1.0f, 1000.0f);
        fContactShadowIntensity = std::clamp(iniReader.ReadFloat("POSTFX", "ContactShadowsIntensity", 1.0f), 0.0f, 1.0f);
        bContactShadowStepJitter = iniReader.ReadInteger("POSTFX", "ContactShadowsStepJitter", 1) != 0;
        bContactShadowsHalfRes = iniReader.ReadInteger("POSTFX", "ContactShadowsHalfResolution", 1) != 0;
        fContactTemporalBlend = std::clamp(iniReader.ReadFloat("POSTFX", "ContactShadowsTemporal", 0.8f), 0.0f, 0.95f);
        fGIIntensity = std::clamp(iniReader.ReadFloat("POSTFX", "ScreenSpaceIndirectLightIntensity", 1.5f), 0.0f, 8.0f);
        fGIMaxBrightness = std::clamp(iniReader.ReadFloat("POSTFX", "ScreenSpaceIndirectLightMaxBrightness", 4.0f), 0.05f, 8.0f);
        fGIOcclusion = std::clamp(iniReader.ReadFloat("POSTFX", "ScreenSpaceIndirectLightOcclusion", 1.0f), 0.0f, 1.0f);
        fSkinScatteringWidth = std::clamp(iniReader.ReadFloat("POSTFX", "SkinScatteringWidth", 0.03f), 0.001f, 0.1f);
        fSkinScatteringStrength = std::clamp(iniReader.ReadFloat("POSTFX", "SkinScatteringStrength", 1.0f), 0.0f, 2.0f);
        fSkinLighting = std::clamp(iniReader.ReadFloat("POSTFX", "SkinLighting", 1.0f), 0.0f, 2.0f);
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
        fLocalContactShadowIntensity = std::clamp(iniReader.ReadFloat("POSTFX", "LocalContactShadowsIntensity", 1.0f), 0.0f, 1.0f);
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

PostFxResource PostFxResources;

// For now: a trace of SSR's frames, to find why reflections show in the pause menu and not in
// play. Ctrl+Shift+F11 writes the settings and the next kFrames frames into GTAIV-ssr-trace.log
// next to the ini: every call of the passes around SSR with its viewport, where it left and what
// D3D returned, what lighting gets on s3, and for the first frames what the SSR targets hold,
// read back from the card. The post fx pass arms and flushes it once a frame; the rest only adds
// lines while it is armed.
namespace SSRTrace
{
    static std::filesystem::path path;
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
            { R.mDepthRT, "depth" }, { R.mSpecularRT, "specular" }, { R.mNormalRT, "normal" }, { R.PreAlphaDepthCopyRT, "prevDepth" } };
        for (auto [rt, name] : known)
            if (rt && rt->mD3DTexture == texture)
                return name;
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
            pDevice->SetTexture(15, probe);
            IDirect3DBaseTexture9* now15 = nullptr;
            pDevice->GetTexture(15, &now15);
            takes = now15 == probe ? "yes" : "NO";
            SAFE_RELEASE(now15);
            pDevice->SetTexture(15, old15);
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
            try
            {
                std::ofstream out(path, std::ios::app);
                out << text << '\n';
            }
            catch (...) {}
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
            pDevice->SetTexture(i, prePostFx[i]);
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
            pDevice->SetTexture(3, nullptr);
            pDevice->SetTexture(8, nullptr);
            pDevice->SetTexture(9, nullptr);
            pDevice->SetTexture(11, nullptr);
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
        for (int i = 0; i < 2; ++i)
        {
            SAFE_RELEASE(PostFxResources.SSRTraceSurf[i]);
            SAFE_RELEASE(PostFxResources.SSRFallbackSurf[i]);
            SAFE_RELEASE(PostFxResources.SSRSpreadSurf[i]);
        }
        for (auto* rt : { &PostFxResources.SSRHalfTex, &PostFxResources.SSRHalfDenoisedTex, &PostFxResources.SSRTraceTex[0],
                          &PostFxResources.SSRTraceTex[1], &PostFxResources.SSRFallbackTex[0], &PostFxResources.SSRFallbackTex[1],
                          &PostFxResources.SSRSpreadTex[0], &PostFxResources.SSRSpreadTex[1] })
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

    static void __fastcall OnDeviceReset()
    {
        PostFxResources.mNormalRT = rage::grcTextureFactoryPC::GetRTByName("_DEFERRED_GBUFFER_1_");
        PostFxResources.mDiffuseRT = rage::grcTextureFactoryPC::GetRTByName("_DEFERRED_GBUFFER_0_");
        PostFxResources.mSpecularRT = rage::grcTextureFactoryPC::GetRTByName("_DEFERRED_GBUFFER_2_");
        PostFxResources.mDepthRT = rage::grcTextureFactoryPC::GetRTByName("_DEFERRED_GBUFFER_3_");
        // Not the stencil buffer: the G-buffer pass writes each material's ID (whole steps of 1/255)
        // to it, as R32F or R16F, for the lighting and fog shaders.
        PostFxResources.mMaterialIdRT = rage::grcTextureFactoryPC::GetRTByName("_STENCIL_BUFFER_");
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
                }
            }

            PostFxResources.ContactRawTex = rage::CreateEmptyRenderTarget("ContactShadowRawTex", width, height, 64, aoDesc, PostFxResources.ContactRawSurf);
            if (PostFxResources.bContactShadowsHalfRes)
            {
                PostFxResources.ContactRawHalfTex = rage::CreateEmptyRenderTarget("ContactShadowRawHalfTex", width / 2, height / 2, 64, aoDesc, PostFxResources.ContactRawHalfSurf);
            }
            PostFxResources.ContactTex = rage::CreateEmptyRenderTarget("ContactShadowTex", width, height, 64, aoDesc, PostFxResources.ContactSurf);
            if (PostFxResources.fContactTemporalBlend > 0.0f)
            {
                static const char* names[2] = { "ContactShadowAccumTex0", "ContactShadowAccumTex1" };
                for (int i = 0; i < 2; ++i)
                    PostFxResources.ContactAccumTex[i] = rage::CreateEmptyRenderTarget(names[i], width, height, 64, aoDesc, PostFxResources.ContactAccumSurf[i]);
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

        initialized = true;
    }

    static void NewFog()
    {
        IDirect3DDevice9* pDevice = rage::grcDevice::GetD3DDevice();

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

                        // No need to set texture here as the desired depth texture already set (GBufferTextureSampler3)

                        pDevice->SetPixelShader(PostFxResources.Blit_PS);

                        pDevice->DrawPrimitive(D3DPT_TRIANGLELIST, 0, 2);
                    }
                }
            }

            // The lit scene, sampler 1 of the fog pass, with the light scattered under the skin
            // if that runs; the fog, the copy below and SSR's history all take it.
            IDirect3DBaseTexture9* scene = prevTex[1];
            if (auto skin = RenderSkinScattering(pDevice, prevTex[1]))
                scene = skin;

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

                    pDevice->SetTexture(0, scene);

                    pDevice->SetPixelShader(PostFxResources.Blit_PS);

                    pDevice->DrawPrimitive(D3DPT_TRIANGLELIST, 0, 2);

                    pDevice->SetSamplerState(0, D3DSAMP_MINFILTER, prevMinFilter[0]);
                    pDevice->SetSamplerState(0, D3DSAMP_MAGFILTER, prevMagFilter[0]);
                    pDevice->SetSamplerState(0, D3DSAMP_MIPFILTER, prevMipFilter[0]);
                    pDevice->SetSamplerState(0, D3DSAMP_ADDRESSU, prevAddressU[0]);
                    pDevice->SetSamplerState(0, D3DSAMP_ADDRESSV, prevAddressV[0]);
                    pDevice->SetTexture(0, prevTex[0]);

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

                    pDevice->SetTexture(1, scene);
                    hbDrawPrimitivePostFX.fun();
                    pDevice->SetTexture(1, prevTex[1]);
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
        pDevice->GetPixelShader(&oldps);
        pDevice->GetVertexShader(&oldvs);

        saveRenderState();

        pDevice->SetSamplerState(2, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);

        // new 
        PostFx3(pDevice, oldps, oldvs);

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

    // Sharpening (CAS.hlsl) of the finished frame, after anti-aliasing and before the HUD. The
    // frame is copied to FullScreenTex_temp2, which anti-aliasing has read by now, and sharpened
    // from there back into the back buffer.
    static void ApplySharpening(IDirect3DDevice9* pDevice, IDirect3DPixelShader9* pShader, IDirect3DVertexShader9* vShader)
    {
        static auto sharpening = FusionFixSettings.GetRef("PREF_SHARPENING");
        auto& R = PostFxResources;
        if (!sharpening || sharpening->get() <= 0 || !R.CAS_PS || !R.backBuffer || !R.FullScreenTex_temp2 || !R.FullScreenSurface_temp2)
            return;
        if (FAILED(pDevice->StretchRect(R.backBuffer, nullptr, R.FullScreenSurface_temp2, nullptr, D3DTEXF_NONE)))
            return;

        // Low, medium and high; the peak CAS weighs the neighbours with is -1 / lerp(8, 5, sharpness).
        static constexpr float kSharpness[] = { 0.3f, 0.6f, 1.0f };
        const float sharpness = kSharpness[std::clamp(sharpening->get(), 1, 3) - 1];
        const float params[4] = { -1.0f / (8.0f - 3.0f * sharpness), 0.0f, 0.0f, 0.0f };

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

        pDevice->SetRenderTarget(0, R.backBuffer);
        pDevice->SetTexture(2, R.FullScreenTex_temp2->mD3DTexture);
        pDevice->SetPixelShaderConstantF(200, params, 1);
        pDevice->SetPixelShader(R.CAS_PS);
        pDevice->SetVertexShader(vShader);
        pDevice->DrawPrimitive(D3DPT_TRIANGLELIST, 0, 2);

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
                    //    //pDevice->SetTexture(2, 0);
                    //    pDevice->SetRenderTarget(0, PostFxResources.pShadowBlurSurf1);
                    //    //pDevice->SetTexture(2, PostFxResources.textureRead);
                    //    pDevice->DrawPrimitive(D3DPT_TRIANGLELIST, 0, 2);
                    //    pDevice->SetTexture(3, 0);
                    //
                    //    pDevice->SetPixelShader(PostFxResources.DeferredShadowBlurCircle_ps);
                    //    pDevice->SetRenderTarget(0, PostFxResources.pShadowBlurSurf2);
                    //    pDevice->SetTexture(11, PostFxResources.pShadowBlurTex1->mD3DTexture);
                    //    pDevice->DrawPrimitive(D3DPT_TRIANGLELIST, 0, 2);
                    //
                    //    pDevice->SetTexture(11, 0);
                    //
                    //    pDevice->SetPixelShader(PostFxResources.SSAO_blend_ps);
                    //    pDevice->SetRenderTarget(0, PostFxResources.renderTargetSurf);
                    //    pDevice->SetTexture(2, PostFxResources.textureRead);
                    //    pDevice->SetTexture(3, PostFxResources.pShadowBlurTex2->mD3DTexture);
                    //    pDevice->DrawPrimitive(D3DPT_TRIANGLELIST, 0, 2);
                    //    PostFxResources.swapbuffers();
                    //    pDevice->SetTexture(2, PostFxResources.textureRead);
                    //    pDevice->SetTexture(3, PostFxResources.prePostFx[3]);
                    //    pDevice->SetPixelShader(pShader);
                    //}

                    // Temporal anti-aliasing resolves the HDR scene before everything else. Normally ResolveScene did
                    // it before the game computed bloom and exposure.
                    if (TemporalAA::GetMode() != TemporalAA::Mode::Off && !TemporalAA::IsSceneResolved() && !RenderScale::IsActive())
                    {
                        if (TemporalAA::Resolve(pDevice, PostFxResources.textureRead, PostFxResources.renderTargetTex, PostFxResources.renderTargetSurf))
                        {
                            PostFxResources.swapbuffers();
                            pDevice->SetPixelShader(pShader);
                        }
                    }

                    if (PostFxResources.useStippleFilter && PostFxResources.stipple_filter_ps)
                    {
                        pDevice->SetPixelShader(PostFxResources.stipple_filter_ps);
                        pDevice->SetRenderTarget(0, PostFxResources.renderTargetSurf);
                        pDevice->SetTexture(2, PostFxResources.textureRead);
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
                                pDevice->SetSamplerState(8, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
                                pDevice->SetSamplerState(8, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
                                pDevice->SetSamplerState(8, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
                                pDevice->SetSamplerState(2, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);

                                pDevice->SetPixelShader(PostFxResources.dof_blur_ps);
                                pDevice->SetRenderTarget(0, PostFxResources.FullScreenDownsampleSurf);
                                pDevice->SetTexture(8, PostFxResources.HalfScreenTex);
                                pDevice->DrawPrimitive(D3DPT_TRIANGLELIST, 0, 2);

                                pDevice->SetPixelShader(PostFxResources.depth_of_field_tent_ps);
                                pDevice->SetRenderTarget(0, PostFxResources.FullScreenDownsampleSurf2);
                                pDevice->SetTexture(8, PostFxResources.FullScreenDownsampleTex->mD3DTexture);
                                pDevice->DrawPrimitive(D3DPT_TRIANGLELIST, 0, 2);

                                pDevice->SetPixelShader(PostFxResources.dof_coc_ps);
                                pDevice->SetRenderTarget(0, PostFxResources.renderTargetSurf);
                                if (PostFxResources.bEnablePreAlphaDepth)
                                    pDevice->SetTexture(1, PostFxResources.PreAlphaDepthCopyRT->mD3DTexture);
                                pDevice->SetTexture(2, PostFxResources.textureRead);
                                pDevice->SetTexture(8, PostFxResources.FullScreenDownsampleTex2->mD3DTexture);
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
                                    pDevice->SetTexture(1, PostFxResources.PreAlphaDepthCopyRT->mD3DTexture);
                                pDevice->SetTexture(2, PostFxResources.textureRead);
                                pDevice->SetTexture(13, PostFxResources.DiffuseTex);
                                pDevice->DrawPrimitive(D3DPT_TRIANGLELIST, 0, 2);

                                // sample sunshafts from a cropped texture
                                pDevice->SetPixelShader(PostFxResources.SSDraw_PS);
                                pDevice->SetRenderTarget(0, PostFxResources.FullScreenDownsampleSurf2);
                                pDevice->SetTexture(11, PostFxResources.FullScreenDownsampleTex->mD3DTexture);
                                pDevice->DrawPrimitive(D3DPT_TRIANGLELIST, 0, 2);

                                // second sunshafts pass
                                pDevice->SetPixelShader(PostFxResources.SSDraw_PS);
                                pDevice->SetRenderTarget(0, PostFxResources.FullScreenDownsampleSurf);
                                pDevice->SetTexture(11, PostFxResources.FullScreenDownsampleTex2->mD3DTexture);
                                pDevice->DrawPrimitive(D3DPT_TRIANGLELIST, 0, 2);

                                // add sunshafts to screen
                                pDevice->SetPixelShader(PostFxResources.SSAdd_PS);
                                pDevice->SetRenderTarget(0, PostFxResources.renderTargetSurf);
                                pDevice->SetTexture(2, PostFxResources.textureRead);
                                pDevice->SetTexture(11, PostFxResources.FullScreenDownsampleTex->mD3DTexture);

                                pDevice->DrawPrimitive(D3DPT_TRIANGLELIST, 0, 2);
                                PostFxResources.swapbuffers();

                                pDevice->SetPixelShader(pShader);
                            }
                        }
                    }

                    // game postfx
                    {
                        for (int i = 0; i < 4; i++)
                        {
                            pDevice->SetTexture(i, PostFxResources.prePostFx[i]);
                            pDevice->SetSamplerState(i, D3DSAMP_MAGFILTER, PostFxResources.Samplers[i]);
                        }

                        if (IsPostFxAA())
                            pDevice->SetRenderTarget(0, PostFxResources.FullScreenSurface_temp2);
                        else
                            pDevice->SetRenderTarget(0, PostFxResources.backBuffer);

                        if (PostFxResources.bEnablePreAlphaDepth)
                            pDevice->SetTexture(1, PostFxResources.PreAlphaDepthCopyRT->mD3DTexture);
                        pDevice->SetTexture(2, PostFxResources.textureRead);
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
                        // FXAA
                        if ((UsePostFxAA->get() == FusionFixSettings.AntialiasingText.eFXAA) && PostFxResources.FxaaPS)
                        {
                            pDevice->SetPixelShader(PostFxResources.FxaaPS);

                            // pDevice->SetRenderTarget(0, PostFxResources.renderTargetSurf);
                            pDevice->SetRenderTarget(0, PostFxResources.backBuffer);

                            pDevice->SetTexture(2, PostFxResources.FullScreenTex_temp2->mD3DTexture);
                            // pDevice->SetTexture(2, PostFxResources.textureRead);

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
                            pDevice->SetTexture(0, PostFxResources.FullScreenTex_temp2->mD3DTexture);
                            pDevice->Clear(0, 0, D3DCLEAR_TARGET, 0, 0, 0);
                            pDevice->DrawPrimitive(D3DPT_TRIANGLELIST, 0, 2);

                            // SMAA_BlendingWeightsCalculation
                            pDevice->SetPixelShader(PostFxResources.SMAA_BlendingWeightsCalculation);
                            pDevice->SetVertexShader(PostFxResources.SMAA_BlendingWeightsCalculationVS);
                            pDevice->SetRenderTarget(0, PostFxResources.blendSurf);
                            pDevice->SetTexture(1, PostFxResources.edgesTex->mD3DTexture);
                            pDevice->SetTexture(2, PostFxResources.SMAA_areaTex);
                            pDevice->SetTexture(3, PostFxResources.SMAA_searchTex);
                            pDevice->Clear(0, 0, D3DCLEAR_TARGET, 0, 0, 0);
                            pDevice->DrawPrimitive(D3DPT_TRIANGLELIST, 0, 2);

                            // SMAA_NeighborhoodBlending
                            pDevice->SetPixelShader(PostFxResources.SMAA_NeighborhoodBlending);
                            pDevice->SetVertexShader(PostFxResources.SMAA_NeighborhoodBlendingVS);

                            // pDevice->SetRenderTarget(0, PostFxResources.renderTargetSurf);
                            pDevice->SetRenderTarget(0, PostFxResources.backBuffer);

                            pDevice->SetTexture(0, PostFxResources.FullScreenTex_temp2->mD3DTexture);
                            pDevice->SetTexture(4, PostFxResources.blendTex->mD3DTexture);

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

                            pDevice->SetTexture(0, PostFxResources.prePostFx[0]);
                            pDevice->SetTexture(1, PostFxResources.prePostFx[1]);
                            pDevice->SetTexture(2, PostFxResources.FullScreenTex_temp2->mD3DTexture);
                            pDevice->SetTexture(3, PostFxResources.prePostFx[3]);
                            pDevice->SetTexture(4, PostFxResources.prePostFx[4]);
                            pDevice->SetPixelShader(pShader);
                            pDevice->SetVertexShader(vShader);
                        }
                    }

                    ApplySharpening(pDevice, pShader, vShader);

                    for (int i = 0; i < PostfxTextureCount; i++)
                    {
                        pDevice->SetTexture(i, PostFxResources.prePostFx[i]);
                        pDevice->SetSamplerState(i, D3DSAMP_MAGFILTER, PostFxResources.Samplers[i]);
                        SAFE_RELEASE(PostFxResources.prePostFx[i]);
                    }
                    return S_OK;
                }

                for (int i = 0; i < PostfxTextureCount; i++)
                {
                    pDevice->SetTexture(i, PostFxResources.prePostFx[i]);
                    pDevice->SetSamplerState(i, D3DSAMP_MAGFILTER, PostFxResources.Samplers[i]);
                    SAFE_RELEASE(PostFxResources.prePostFx[i]);
                }
                return S_FALSE;
            }
        }

        for (int i = 0; i < PostfxTextureCount; i++)
        {
            pDevice->SetTexture(i, PostFxResources.prePostFx[i]);
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
                device->SetTexture(slot, textures[slot]);
                SAFE_RELEASE(textures[slot]);
            }
        }

        SavedSamplerSlots(const SavedSamplerSlots&) = delete;
        SavedSamplerSlots& operator=(const SavedSamplerSlots&) = delete;
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
    // averages in milliseconds per frame are written to FusionFix.PostFx.log next to GTAIV.exe.
    // Lights is the lighting phase less the AO, SSR, contact shadow and indirect light passes that
    // run inside it: the game's lights with their local contact shadows, and the light shafts.
    // Off, no query is made.
    enum ProfilerSection { kProfAO, kProfSSR, kProfContact, kProfGI, kProfWater, kProfLighting, kProfSections };
    static constexpr int kProfilerFrames = 4;
    static constexpr int kProfilerAverage = 120;
    struct ProfilerFrame
    {
        IDirect3DQuery9* disjoint = nullptr;
        IDirect3DQuery9* freq = nullptr;
        IDirect3DQuery9* start = nullptr;
        IDirect3DQuery9* stop = nullptr;
        IDirect3DQuery9* begin[kProfSections] = {};
        IDirect3DQuery9* end[kProfSections] = {};
        bool issued = false;
        bool used[kProfSections] = {};
    };
    static inline ProfilerFrame profilerFrames[kProfilerFrames];
    static inline int nProfilerFrame = -1;
    static inline double profilerSums[kProfSections + 1] = {}; // the last is the whole frame
    static inline int nProfilerSamples = 0;
    static inline bool bProfilerLogStarted = false;

    static void ReleaseProfilerFrame(ProfilerFrame& f)
    {
        SAFE_RELEASE(f.disjoint);
        SAFE_RELEASE(f.freq);
        SAFE_RELEASE(f.start);
        SAFE_RELEASE(f.stop);
        for (int i = 0; i < kProfSections; ++i)
        {
            SAFE_RELEASE(f.begin[i]);
            SAFE_RELEASE(f.end[i]);
        }
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
        for (int i = 0; ok && i < kProfSections; ++i)
            ok = SUCCEEDED(pDevice->CreateQuery(D3DQUERYTYPE_TIMESTAMP, &f.begin[i])) &&
                 SUCCEEDED(pDevice->CreateQuery(D3DQUERYTYPE_TIMESTAMP, &f.end[i]));
        if (!ok)
            ReleaseProfilerFrame(f);
        return ok;
    }

    static void WriteProfilerLine()
    {
        FILE* log = _wfopen((GetExeModulePath() / L"FusionFix.PostFx.log").c_str(), bProfilerLogStarted ? L"a" : L"w");
        if (log)
        {
            if (!bProfilerLogStarted)
                fprintf(log, "GPU milliseconds per frame, averaged over %d frames. lights: the game's lights with their "
                             "local contact shadows, and the light shafts\n", kProfilerAverage);
            const double n = double(nProfilerSamples);
            fprintf(log, "frame %6.2f   AO %5.2f   SSR %5.2f   contact shadows %5.2f   indirect light %5.2f   water SSR %5.2f   lights %6.2f\n",
                    profilerSums[kProfSections] / n, profilerSums[kProfAO] / n, profilerSums[kProfSSR] / n,
                    profilerSums[kProfContact] / n, profilerSums[kProfGI] / n, profilerSums[kProfWater] / n,
                    profilerSums[kProfLighting] / n);
            fclose(log);
            bProfilerLogStarted = true;
        }
        std::fill(std::begin(profilerSums), std::end(profilerSums), 0.0);
        nProfilerSamples = 0;
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
        double ms[kProfSections] = {};
        for (int i = 0; i < kProfSections; ++i)
        {
            UINT64 b = 0, e = 0;
            if (f.used[i] && f.begin[i]->GetData(&b, sizeof(b), 0) == S_OK && f.end[i]->GetData(&e, sizeof(e), 0) == S_OK && e >= b)
                ms[i] = double(e - b) * 1000.0 / double(freq);
        }
        ms[kProfLighting] = (std::max)(ms[kProfLighting] - ms[kProfAO] - ms[kProfSSR] - ms[kProfContact] - ms[kProfGI], 0.0);
        for (int i = 0; i < kProfSections; ++i)
            profilerSums[i] += ms[i];
        profilerSums[kProfSections] += double(stop - start) * 1000.0 / double(freq);
        if (++nProfilerSamples >= kProfilerAverage)
            WriteProfilerLine();
    }

    // Once a frame, as post processing begins: closes this frame's queries and opens the next.
    static void ProfilerNextFrame(IDirect3DDevice9* pDevice)
    {
        auto& R = PostFxResources;
        if (!R.bPostFxProfiler || !pDevice)
            return;
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
        std::fill(std::begin(f.used), std::end(f.used), false);
        f.disjoint->Issue(D3DISSUE_BEGIN);
        f.start->Issue(D3DISSUE_END);
        f.issued = true;
    }

    static void ProfilerMark(IDirect3DDevice9* pDevice, int section, bool begin)
    {
        if (!PostFxResources.bPostFxProfiler || !pDevice || nProfilerFrame < 0)
            return;
        auto& f = profilerFrames[nProfilerFrame];
        if (!f.issued)
            return;
        (begin ? f.begin : f.end)[section]->Issue(D3DISSUE_END);
        if (!begin)
            f.used[section] = true;
    }

    // Binds every sampler of the pixel shader of the pass just begun to the texture its effect
    // parameter holds, found through the shader's constant table (sampler X reads X2D in SSR.fx
    // and AO.fx). D3DX left some holding what the game had bound, about four a frame in the SSR
    // passes: a diagnostic pass read a G-buffer texture where it sampled the depth.
    // Each shader's samplers are looked up for every draw, from its bytecode and constant table,
    // a few dozen draws a frame. Kept per effect and shader they went wrong in play: the smoothing
    // and accumulation of SSR left registers with what the game had bound, a G-buffer texture
    // where they sampled the specular one, and SSR came out empty, while it worked in the pause
    // menu, where the game had bound others.

    static std::vector<std::pair<UINT, D3DXHANDLE>> FindEffectSamplers(ID3DXEffect* effect, IDirect3DPixelShader9* ps)
    {
        std::vector<std::pair<UINT, D3DXHANDLE>> samplers;
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
            if (FAILED(table->GetConstantDesc(table->GetConstant(nullptr, i), &desc, &count)) ||
                desc.RegisterSet != D3DXRS_SAMPLER || !desc.Name)
                continue;
            if (D3DXHANDLE param = effect->GetParameterByName(nullptr, (std::string(desc.Name) + "2D").c_str()))
                samplers.emplace_back(desc.RegisterIndex, param);
        }
        table->Release();
        return samplers;
    }

    static void BindEffectSamplers(IDirect3DDevice9* pDevice, ID3DXEffect* effect)
    {
        IDirect3DPixelShader9* ps = nullptr;
        if (FAILED(pDevice->GetPixelShader(&ps)) || !ps)
            return;
        const auto samplers = FindEffectSamplers(effect, ps);
        const void* shader = ps;
        ps->Release();

        std::string traced;
        for (const auto& [reg, param] : samplers)
        {
            IDirect3DBaseTexture9* want = nullptr;
            IDirect3DBaseTexture9* have = nullptr;
            effect->GetTexture(param, &want);
            pDevice->GetTexture(reg, &have);
            if (want != have)
                pDevice->SetTexture(reg, want);
            if (SSRTrace::Active())
            {
                D3DXPARAMETER_DESC desc = {};
                effect->GetParameterDesc(param, &desc);
                traced += " s" + std::to_string(reg) + "=" + (desc.Name ? desc.Name : "?") + ":" + SSRTrace::TextureName(want) +
                    (want != have ? "(was " + SSRTrace::TextureName(have) + ")" : "");
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
            !R.SSRTraceSurf[0] || !R.SSRFallbackSurf[0])
        {
            SSRTrace::Line("ssr: left, effect %p depth %p history %p vp %p intensity %.2f trace %p fallback %p",
                static_cast<void*>(R.SSREffect), static_cast<void*>(R.mDepthRT), static_cast<void*>(R.SSRHistoryTex),
                static_cast<void*>(vp), R.fSSRIntensity, static_cast<void*>(R.SSRTraceSurf[0]), static_cast<void*>(R.SSRFallbackSurf[0]));
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

        effect->SetTexture(h.DepthTex2D, R.mDepthRT->mD3DTexture);
        effect->SetTexture(h.HistoryTex2D, R.SSRHistoryTex->mD3DTexture);
        // Last frame's fog pass copied this depth along with the history; this frame's has not
        // run yet.
        const bool prevDepth = R.PreAlphaDepthCopyRT && R.PreAlphaDepthCopyRT->mD3DTexture;
        effect->SetTexture(h.PrevDepthTex2D, prevDepth ? R.PreAlphaDepthCopyRT->mD3DTexture : nullptr);
        effect->SetFloat(h.fUsePrevDepth, prevDepth ? 1.0f : 0.0f);

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
        const bool temporal = R.fSSRTemporalBlend > 0.0f && h.techSSRTemporal && R.SSRAccumSurf[half][0] && R.SSRAccumSurf[half][1];
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
        // CWeather::Rain eases towards the weather's rain amount, 0.3 for drizzle, 0.7 for rain
        // and 1.0 for a thunderstorm, so the ground wets and dries with it; rain counts as wet.
        // Telling ground from walls takes the G-buffer normals, and the gloss the specular one.
        const float rain = CWeather::Rain ? *CWeather::Rain : 0.0f;
        const bool wetGround = R.fSSRWetGround > 0.0f && hasNormals && hasSpecular;
        effect->SetFloat(h.fWetness, wetGround ? std::clamp(rain / 0.7f, 0.0f, 1.0f) : 0.0f);
        effect->SetFloat(h.fWetGroundBoost, R.fSSRWetGround);

        UINT passes = 0;
        IDirect3DBaseTexture9* oldTextures[kSSRTextureSlots] = {};
        DWORD savedRenderStates[std::size(kSSRRenderStates)] = {};
        DWORD savedSamplerStates[kSSRSamplerSlots][std::size(kSSRSamplerStates)] = {};
        {
            for (DWORD slot = 0; slot < kSSRTextureSlots; ++slot)
                pDevice->GetTexture(slot, &oldTextures[slot]);
            pDevice->SetTexture(3, nullptr);

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
            // 0, SSR_PS then does not read it), and the two together.
            auto draw = [&](UINT pass, IDirect3DSurface9* target)
            {
                const HRESULT rtHr = pDevice->SetRenderTarget(0, target);
                pDevice->Clear(0, nullptr, D3DCLEAR_TARGET, D3DCOLOR_ARGB(0, 0, 0, 0), 1.0f, 0);
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
            draw(0, R.SSRTraceSurf[half]);
            effect->SetTexture(h.SSRResultTex2D, R.SSRTraceTex[half]->mD3DTexture);
            IDirect3DTexture9* fill = R.SSRFallbackTex[half]->mD3DTexture;
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
            effect->SetTexture(h.SSRFallbackTex2D, fill);
            draw(3, ssrSurf);
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
            const HRESULT drawHr = pDevice->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, screenVertices, sizeof(ScreenVertex));
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
            pDevice->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, screenVertices, sizeof(ScreenVertex));
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
                pDevice->SetTexture(slot, oldTextures[slot]);
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

        effect->SetTexture(h.DepthTex2D, R.mDepthRT->mD3DTexture);
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
        pDevice->SetTexture(3, nullptr);

        effect->SetTexture(h.SurfaceTex2D, oldTextures[0]);
        effect->SetFloat(h.fWaterNormalStrength, oldTextures[0] ? R.fSSRWaterNormalStrength : 0.0f);

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

        effect->SetTechnique(h.techSSRWater);
        effect->Begin(&passes, 0);
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
            pDevice->SetTexture(slot, oldTextures[slot]);
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

    static void __cdecl WaterRenderHook(int a1)
    {
        auto& R = PostFxResources;
        // The game renders water for other views too, into targets of other sizes: each call
        // took two full screen copies and a full screen pass, and a target of another size
        // had both mask copies released and created again, even with no water on screen.
        // Only the main scene's water gets reflections, once a frame.
        const bool mainScene = R.SSREnabled() && R.SSREffect && R.fSSRWaterIntensity > 0.0f &&
                               !R.bWaterDoneThisFrame && RenderTargetIsScreenSized();
        R.bWaterMaskCaptured = mainScene && CopyRenderTargetToWaterMask(0);
        shWaterRender.unsafe_ccall<void>(a1);
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
            SavedSamplerSlots savedSamplers(pDevice);
            SavedShaderConstants savedConstants(pDevice);

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
            PostFxResources.DepthTex = PostFxResources.mDepthRT->mD3DTexture;
            IDirect3DSurface9* SpecularRT;
            PostFxResources.SpecularTex->GetSurfaceLevel(0, &SpecularRT);

            UINT passes = 0;
            ID3DXEffect* effect = PostFxResources.AOEffect;
            effect->Begin(&passes, 0); assert(passes == 7);
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

                effect->BeginPass(gtao ? 5 : 2); // 1 SAO, 2 GTAO
                BindEffectSamplers(pDevice, effect);
                pDevice->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, screenVertices, sizeof(ScreenVertex));
                effect->EndPass();

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
                    effect->BeginPass(6);
                    BindEffectSamplers(pDevice, effect);
                    pDevice->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, screenVertices, sizeof(ScreenVertex));
                    effect->EndPass();

                    R.nAOAccumIndex = next;
                    R.nAOAccumFrame = FrameHistory::Frame();
                    blurSource = R.AOAccumTex[next]->mD3DTexture;
                }
                else
                    R.nAOAccumFrame = 0;

                float hor[2] = { invViewportSize[0] * PostFxResources.fAmbientOcclusionBlurRadius, 0.0 };
                float ver[2] = { 0.0, invViewportSize[1] * PostFxResources.fAmbientOcclusionBlurRadius };
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

                // final output
                pDevice->SetRenderTarget(0, SpecularRT);
                effect->SetTexture(h.AOTexture2D, PostFxResources.nAmbientOcclusionBlurPasses > 0 ? aoTex : blurSource);

                effect->BeginPass(4);
                BindEffectSamplers(pDevice, effect);
                pDevice->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, screenVertices, sizeof(ScreenVertex));
                effect->EndPass();
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
        DrawWaterReflectionDebug();
        SSRTrace::Tick();
    }

    static void DrawWaterReflectionDebug()
    {
        if (PostFxResources.SSRDebugMode() != PostFxResources.kWaterReflectionDebugMode)
            return;
        auto rt = rage::grcTextureFactoryPC::GetRTByName("WATER_REFLECTION_COLOUR");
        if (rt)
            DrawDebugTexture(rt->mD3DTexture, PostFxResources.SSREffectHandles.techWaterReflectionDebug);
    }

    // Replaces the finished frame with the SSR debug view chosen in the graphics menu.
    static void DrawSSRDebugOverlay()
    {
        auto& R = PostFxResources;
        SSRTrace::Line("debug overlay: valid %d", int(R.bSSRDebugValid));
        if (!R.bSSRDebugValid || !R.SSRDebugTex || !R.SSREffect || !R.SSREffectHandles.techSSRDebugCopy)
            return;
        R.bSSRDebugValid = false;
        DrawDebugTexture(R.SSRDebugTex->mD3DTexture, R.SSREffectHandles.techSSRDebugCopy);
    }

    // Draws texture over the finished frame with technique, which reads it as DebugTex.
    static void DrawDebugTexture(IDirect3DTexture9* texture, D3DXHANDLE technique)
    {
        auto& R = PostFxResources;
        IDirect3DDevice9* pDevice = rage::grcDevice::GetD3DDevice();
        if (!pDevice || !texture || !R.SSREffect || !technique)
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
        effect->SetTexture(h.DebugTex2D, texture);
        UINT passes = 0;
        effect->SetTechnique(technique);
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
            pDevice->SetTexture(slot, oldTextures[slot]);
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

    // Render scale: from here on FullScreenCopy is a texture of the screen size, with the scene upscaled by DLSS
    // or FSR, or stretched when neither runs
    static void UpscaleScene()
    {
        if (!RenderScale::IsActive() || !PostFxResources.FullScreenTex_temp1 || !PostFxResources.FullScreenTex_temp1->mD3DTexture)
            return;

        auto pDevice = rage::grcDevice::GetD3DDevice();
        IDirect3DTexture9* scene = nullptr;
        IDirect3DSurface9* sceneSurface = nullptr;
        IDirect3DSurface9* output = nullptr;
        if (!RenderScale::BeginPost(pDevice, scene, sceneSurface, output))
            return;

        auto upscaled = PostFxResources.FullScreenTex_temp1->mD3DTexture;
        IDirect3DSurface9* upscaledSurface = nullptr;
        upscaled->GetSurfaceLevel(0, &upscaledSurface);

        auto mode = TemporalAA::GetMode();
        if (upscaledSurface && (mode == TemporalAA::Mode::DLAA || mode == TemporalAA::Mode::FSR) &&
            TemporalAA::Resolve(pDevice, scene, upscaled, upscaledSurface))
            pDevice->StretchRect(upscaledSurface, nullptr, output, nullptr, D3DTEXF_POINT);
        else
            pDevice->StretchRect(sceneSurface, nullptr, output, nullptr, D3DTEXF_LINEAR);
        SAFE_RELEASE(upscaledSurface);
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
        auto scene = PostFxResources.mFullScreenRT->mD3DTexture;
        auto resolved = PostFxResources.FullScreenTex_temp1->mD3DTexture;
        IDirect3DSurface9* sceneSurface = nullptr;
        IDirect3DSurface9* resolvedSurface = nullptr;
        scene->GetSurfaceLevel(0, &sceneSurface);
        resolved->GetSurfaceLevel(0, &resolvedSurface);
        if (sceneSurface && resolvedSurface && TemporalAA::Resolve(pDevice, scene, resolved, resolvedSurface))
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

    static inline injector::hook_back<int(__fastcall*)(int, void*, int, int, char, char, int, char)> hbDrawSkyHook;
    static int __fastcall DrawSky(int _this, void* edx, int a2, int a3, char a4, char a5, int a6, char a7)
    {
        auto pDevice = rage::grcDevice::GetD3DDevice();
        IDirect3DPixelShader9* pShader = nullptr;
        HRESULT hr = S_FALSE;
        pDevice->GetPixelShader(&pShader);
        // atmoscatt clouds
        if (PostFxResources.DiffuseTex != nullptr)
        {
            IDirect3DSurface9* DiffuseSurf = nullptr;
            PostFxResources.DiffuseTex->GetSurfaceLevel(0, &DiffuseSurf);
            if (DiffuseSurf)
            {
                IDirect3DSurface9* oldRenderTarget1 = 0;
                pDevice->GetRenderTarget(1, &oldRenderTarget1);
                pDevice->SetRenderTarget(1, DiffuseSurf);
                hr = hbDrawSkyHook.fun(_this, edx, a2, a3, a4, a5, a6, a7);
                pDevice->SetPixelShader(pShader);
                pDevice->SetRenderTarget(1, oldRenderTarget1);
                hr = hbDrawSkyHook.fun(_this, edx, a2, a3, a4, a5, a6, a7);
                SAFE_RELEASE(oldRenderTarget1);
                SAFE_RELEASE(DiffuseSurf);
                SAFE_RELEASE(pShader);
                return hr;
            }
            SAFE_RELEASE(DiffuseSurf);
            SAFE_RELEASE(pShader);
        }
        return hbDrawSkyHook.fun(_this, edx, a2, a3, a4, a5, a6, a7);
    }

    static inline SafetyHookInline RenderPedAndVehicleFakeShadowsInlineHook;
    static DWORD __cdecl RenderPedAndVehicleFakeShadows(DWORD a1)
    {
        DWORD result = RenderPedAndVehicleFakeShadowsInlineHook.unsafe_ccall<DWORD>(a1);

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
            BindLightingInputs(pDevice);

        return result;
    }

    // Draws one full screen quad of the SSR effect's technique into target, then restores the
    // device state it touched.
    static void DrawEffectPass(IDirect3DDevice9* pDevice, ID3DXEffect* effect, D3DXHANDLE technique, IDirect3DSurface9* target, float width, float height)
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
        UINT passes = 0;
        effect->SetTechnique(technique);
        effect->Begin(&passes, 0);
        effect->BeginPass(0);
        effect->CommitChanges();
        BindEffectSamplers(pDevice, effect);
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
        effect->SetTexture(h.DepthTex2D, R.mDepthRT->mD3DTexture);
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

        if (R.bContactShadowsHalfRes && R.ContactRawHalfSurf && h.techContactUpsample)
        {
            // The march at half size, with the pixel size and reconstruction basis of that size,
            // then back to full size for the rest.
            const float halfWidth = float(DWORD(width) / 2), halfHeight = float(DWORD(height) / 2);
            SetTargetSize(effect, h, proj, halfWidth, halfHeight);
            DrawEffectPass(pDevice, effect, h.techContactShadows, R.ContactRawHalfSurf, halfWidth, halfHeight);
            SetTargetSize(effect, h, proj, width, height);
            effect->SetTexture(h.SSRResultTex2D, R.ContactRawHalfTex->mD3DTexture);
            DrawEffectPass(pDevice, effect, h.techContactUpsample, R.ContactRawSurf, width, height);
        }
        else
            DrawEffectPass(pDevice, effect, h.techContactShadows, R.ContactRawSurf, width, height);

        // The same depth aware smoothing SSR uses; the raw result has alpha 1 everywhere, so
        // it is a plain weighted blur.
        IDirect3DTexture9* result = R.ContactRawTex->mD3DTexture;
        if (R.fSSRDenoiseRadius > 0.0f && h.techSSRDenoise)
        {
            effect->SetTexture(h.SSRResultTex2D, R.ContactRawTex->mD3DTexture);
            effect->SetFloat(h.fDenoiseRadius, R.fSSRDenoiseRadius);
            effect->SetFloat(h.fDenoiseSSROnly, 0.0f);
            DrawEffectPass(pDevice, effect, h.techSSRDenoise, R.ContactSurf, width, height);
            result = R.ContactTex->mD3DTexture;
        }
        else
        {
            pDevice->StretchRect(R.ContactRawSurf, nullptr, R.ContactSurf, nullptr, D3DTEXF_NONE);
        }
        R.ContactResult = R.ContactTex->mD3DTexture;

        if (temporal)
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
            effect->SetTexture(h.SSRResultTex2D, R.ContactTex->mD3DTexture);
            effect->SetTexture(h.SSRAccumTex2D, R.ContactAccumTex[prev]->mD3DTexture);
            effect->SetTexture(h.PrevDepthTex2D, prevDepth ? R.PreAlphaDepthCopyRT->mD3DTexture : nullptr);
            effect->SetFloat(h.fUsePrevDepth, prevDepth ? 1.0f : 0.0f);
            effect->SetVectorArray(h.vec4ViewToPrevClip, reprojRows, 4);
            BindMotionVectors(effect, accumWasValid);
            effect->SetFloat(h.fTemporalBlend, accumWasValid ? R.fContactTemporalBlend : 0.0f);
            DrawEffectPass(pDevice, effect, h.techContactTemporal, R.ContactAccumSurf[next], width, height);

            result = R.ContactAccumTex[next]->mD3DTexture;
            R.ContactResult = result;
            R.nContactAccumIndex = next;
            R.nContactAccumFrame = FrameHistory::Frame();
        }

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
            pDevice->SetTexture(slot, oldTextures[slot]);
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
        effect->SetTexture(h.DepthTex2D, R.mDepthRT->mD3DTexture);
        effect->SetTexture(h.HistoryTex2D, R.SSRHistoryTex->mD3DTexture);
        effect->SetTexture(h.PrevDepthTex2D, prevDepth ? R.PreAlphaDepthCopyRT->mD3DTexture : nullptr);
        effect->SetFloat(h.fUsePrevDepth, prevDepth ? 1.0f : 0.0f);
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

        DrawEffectPass(pDevice, effect, h.techSSGI, R.GIRawSurf, width, height);

        // The same depth aware smoothing SSR uses; the gather has alpha 1 everywhere, so it is a
        // plain weighted blur. The radius is in full size pixels.
        IDirect3DTexture9* gathered = R.GIRawTex->mD3DTexture;
        if (R.fSSRDenoiseRadius > 0.0f && h.techSSRDenoise)
        {
            effect->SetTexture(h.SSRResultTex2D, gathered);
            effect->SetFloat(h.fDenoiseRadius, R.fSSRDenoiseRadius * 0.5f);
            effect->SetFloat(h.fDenoiseSSROnly, 0.0f);
            DrawEffectPass(pDevice, effect, h.techSSRDenoise, R.GIDenoisedSurf, width, height);
            gathered = R.GIDenoisedTex->mD3DTexture;
        }

        const int prev = R.nGIAccumIndex, next = prev ^ 1;
        effect->SetTexture(h.SSRResultTex2D, gathered);
        effect->SetTexture(h.SSRAccumTex2D, R.GIAccumTex[prev]->mD3DTexture);
        const bool history = FrameHistory::CanReproject(R.nGIAccumFrame);
        BindMotionVectors(effect, history);
        effect->SetFloat(h.fTemporalBlend, history ? R.fGITemporalBlend : 0.0f);
        effect->SetFloat(h.fTemporalAnySurface, 1.0f);
        DrawEffectPass(pDevice, effect, h.techSSRTemporal, R.GIAccumSurf[next], width, height);
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
            DrawEffectPass(pDevice, effect, h.techGIUpsample, R.GIFullSurf, fullWidth, fullHeight);
            R.GIResult = R.GIFullTex->mD3DTexture;
        }

        if (R.SSRDebugMode() == R.kGIDebugMode && R.SSRDebugSurf && h.techSSRDebug)
        {
            effect->SetTexture(h.SSRResultTex2D, R.GIResult);
            effect->SetFloat(h.fDebugMode, float(R.kGIDebugMode));
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
            pDevice->SetTexture(slot, oldTextures[slot]);
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
        effect->SetTexture(h.DepthTex2D, R.mDepthRT->mD3DTexture);
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
            DrawEffectPass(pDevice, effect, h.techSkinLight, R.SkinLightSurf[0], width, height);
            // A kernel unit is half SkinScatteringWidth, and a metre at view depth 1 spans _11 / 2
            // of the screen across and _22 / 2 down.
            const float unit = R.fSkinScatteringWidth * 0.25f;
            D3DXVECTOR4 step(R.SkinCamera[0] * unit, 0.0f, 0.0f, R.fSkinScatteringWidth * 0.5f);
            effect->SetVector(h.vec4SkinStep, &step);
            effect->SetTexture(h.SkinLightTex2D, R.SkinLightTex[0]->mD3DTexture);
            DrawEffectPass(pDevice, effect, h.techSkinScatter, R.SkinLightSurf[1], width, height);
            step = D3DXVECTOR4(0.0f, R.SkinCamera[1] * unit, 0.0f, R.fSkinScatteringWidth * 0.5f);
            effect->SetVector(h.vec4SkinStep, &step);
            effect->SetTexture(h.SkinLightTex2D, R.SkinLightTex[1]->mD3DTexture);
            DrawEffectPass(pDevice, effect, h.techSkinScatterFinal, R.SkinLightSurf[0], width, height);
            result = R.SkinLightTex[0]->mD3DTexture;
        }

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
            pDevice->SetTexture(slot, oldTextures[slot]);
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

    static void InstallLocalContactLightHook()
    {
        auto pattern = hook::pattern("83 C7 28 89 7C 24 1C 8B 47 1C 85 C0");
        if (pattern.empty())
            return;
        shLocalContactLight = safetyhook::create_mid(pattern.get_first(7), [](SafetyHookContext& regs)
        {
            auto& R = PostFxResources;
            if (!R.bLocalContactPass || R.LocalContactShadowConsts[7] == 0.0f)
                return;
            const bool off = (*reinterpret_cast<const uint32_t*>(regs.edi + 0x20) & 0x200) != 0;
            if (off == R.bLocalContactLightOff)
                return;
            R.bLocalContactLightOff = off;
            auto pDevice = rage::grcDevice::GetD3DDevice();
            if (!pDevice)
                return;
            const float none[4] = {};
            pDevice->SetPixelShaderConstantF(203, off ? none : &R.LocalContactShadowConsts[4], 1);
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
        // s9 is read by no game shader, and the car glass takes it over right after lighting.
        if (R.bContactValid && R.ContactResult)
        {
            BindSampler(pDevice, 9, R.ContactResult, D3DTEXF_POINT);
            R.bContactBound = true;
        }
        else if (R.bContactBound)
        {
            pDevice->SetTexture(9, nullptr);
            R.bContactBound = false;
        }
        pDevice->SetPixelShaderConstantF(202, R.LocalContactShadowConsts, 3);
        R.bLocalContactPass = true;
        R.bLocalContactLightOff = false;

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
        pDevice->SetTexture(3, tex);
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
            ProfilerMark(pDevice, kProfLighting, true);
            BindLightingInputs(pDevice);
        }
    }

    static void BindSampler(IDirect3DDevice9* pDevice, DWORD slot, IDirect3DBaseTexture9* tex, DWORD filter)
    {
        pDevice->SetTexture(slot, tex);
        pDevice->SetSamplerState(slot, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
        pDevice->SetSamplerState(slot, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
        pDevice->SetSamplerState(slot, D3DSAMP_MAGFILTER, filter);
        pDevice->SetSamplerState(slot, D3DSAMP_MINFILTER, filter);
        pDevice->SetSamplerState(slot, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
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
        ProfilerMark(pDevice, kProfLighting, false);

        if (R.bContactBound)
        {
            pDevice->SetTexture(9, nullptr);
            R.bContactBound = false;
        }
        if (R.bGIBound)
        {
            pDevice->SetTexture(8, nullptr);
            R.bGIBound = false;
        }
        if (R.bMaterialIdBound)
        {
            pDevice->SetTexture(11, nullptr);
            R.bMaterialIdBound = false;
        }
        // Lights drawn for other views (reflections, mirrors) must not march with this camera,
        // nor light skin by this view's material IDs.
        const float noLocalContactShadows[4] = {};
        pDevice->SetPixelShaderConstantF(203, noLocalContactShadows, 1);
        R.bLocalContactPass = false;
        pDevice->SetPixelShaderConstantF(201, noLocalContactShadows, 1);
        pDevice->SetPixelShaderConstantF(205, noLocalContactShadows, 1);

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
        pDevice->SetTexture(9, nullptr);
        pDevice->SetTexture(11, nullptr);
        pDevice->SetTexture(13, nullptr);
        R.bGlassBound = false;
    }

    PostFX()
    {
        FusionFix::onInitEventAsync() += []()
        {
            if (GetD3DX9_43DLL())
            {
                PostFxResources.Readini();
                SSRTrace::path = CIniReader("").GetIniPath().parent_path() / "GTAIV-ssr-trace.log";

                auto pattern = find_pattern("E8 ? ? ? ? 8B 4F ? E8 ? ? ? ? 8B 4F ? E8 ? ? ? ? 5F", "E8 ? ? ? ? 8B 4F ? E8 ? ? ? ? 8B 4F ? E8 ? ? ? ? 33 C0");
                hbDrawPrimitivePostFX.fun = injector::MakeCALL(pattern.get_first(0), DrawPrimitivePostFX).get();

                pattern = find_pattern("E8 ? ? ? ? 8D 44 24 ? 50 8B CF E8 ? ? ? ? 8D 84 24", "E8 ? ? ? ? 8D 44 24 ? 50 8B CE E8 ? ? ? ? 8D 8C 24 ? ? ? ? 51 8B CE E8 ? ? ? ? 8D 94 24");
                hbDrawSkyHook.fun = injector::MakeCALL(pattern.get_first(0), DrawSky).get();

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
                    InstallPedSkinHooks();
                    CRenderPhaseDeferredLighting_LightsToScreen::OnBuildRenderList() += []()
                    {
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