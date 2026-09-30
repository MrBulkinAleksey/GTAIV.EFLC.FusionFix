module;

#include <common.hxx>
#include <d3dx9tex.h>
#include <algorithm>

export module postfx;

import common;
import comvars;
import d3dx9_43;
import natives;
import settings;
import shaders;

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

#ifndef SAFE_RELEASE
#define SAFE_RELEASE(p) { if (p) { (p)->Release(); (p)=NULL; } }
#endif

std::optional<std::reference_wrapper<int32_t>> UsePostFxAA;

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
    D3DXMATRIX SSRPrevViewProj = {};
    bool bSSRPrevViewProjValid = false;
    D3DXVECTOR4 SSRReprojRows[4] = {};
    bool bSSRReprojValid = false;
    // Set once the fog pass has copied this frame's scene into SSRHistoryTex; SSR runs before
    // that and sees last frame's, water may run after it.
    bool bSSRHistoryThisFrame = false;

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
        D3DXHANDLE PreWaterTex2D, PostWaterTex2D, fUseWaterMask, PrevDepthTex2D, fUsePrevDepth;
        D3DXHANDLE fDenoiseRadius, fDenoiseSSROnly, techSSRDenoise, fPassThinObjects, fStepJitter, fTowardCamera, fReflectionBlur, fDistanceFade;
        D3DXHANDLE vec4SunView, fCSLength, fCSThickness, fCSMaxThickness, fCSMaxViewDistance, fCSIntensity, techContactShadows;
        D3DXHANDLE techContactTemporal, fJitterOffset;
        D3DXHANDLE vec2InvViewportSize, fNearPlane, fFarDivNear, vec4ProjInfo;
        D3DXHANDLE fMaxDistance, fThickness, fEdgeFade, fIntensity;
        D3DXHANDLE vec4ViewToPrevClip, fGlossBoost, fGlossCutoff;
        D3DXHANDLE vec4WaterPlane, fWaterIntensity, fWaterBlur;
        D3DXHANDLE fWaterNormalStrength, vec4WaterToView, vec4WaterWorldX, vec4WaterWorldY;
        D3DXHANDLE techSSR, techSSRWater;
        D3DXHANDLE SSRAccumTex2D, SSRHitTex2D, fTemporalBlend, techSSRTemporal;
    } SSREffectHandles = {};

    // PREF_SSR: 0 off, 1 half resolution, 2 full resolution.
    static bool SSREnabled() { static auto p = FusionFixSettings.GetRef("PREF_SSR"); return p && p->get() != 0; }
    static bool SSRHalfRes() { static auto p = FusionFixSettings.GetRef("PREF_SSR"); return p && p->get() == 1; }
    // SSR debug view from the graphics menu (PREF_SSR_DEBUG), see SSRDebug_PS in SSR.fx.
    // Built after the SSR pass and shown over the finished frame.
    // Contact shadows (ContactShadows_PS in SSR.fx), bound to s9 for deferred_lighting
    // (shaders/patches/deferred_lighting_contact_shadows.patch) and unbound right after it.
    static bool ContactShadowsEnabled() { static auto p = FusionFixSettings.GetRef("PREF_CONTACTSHADOWS"); return p && p->get() != 0; }
    float fContactShadowLength = 0.3f;
    float fContactShadowThickness = 0.15f;
    float fContactShadowMaxThickness = 2.0f;
    float fContactShadowMaxDistance = 60.0f;
    float fContactShadowIntensity = 1.0f;
    bool bContactShadowStepJitter = true;
    // Contact shadows from street lights and headlights, marched in the light shaders
    // themselves (shaders/patches/local_light_contact_shadows.patch); they follow the Contact
    // Shadows menu toggle.
    bool bLocalContactShadows = true;
    float fLocalContactShadowLength = 0.5f;
    float fLocalContactShadowThickness = 0.2f;
    float fLocalContactShadowMaxThickness = 2.0f; // c203.y, see ContactShadowsMaxThickness
    // c203.z: 1 paints what the local contact shadows take from each light red, to see them.
    bool bLocalContactShadowsDebug = false;
    float fLocalContactShadowMaxDistance = 40.0f;
    float fLocalContactShadowIntensity = 1.0f;
    // c202 ray length, thickness, max view distance and strength; c203 the main camera's _34, the
    // max thickness, the debug flag and 12345 in w while they are on; c204 its _11, _22, _31, _32. Set right before lighting, as
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
    uint32_t nVolumetricLightHeadlightFlag = 0x100;
    float fVolumetricLightHeadlightIntensity = 2.0f;
    // Headlight shaft size in metres, radius times scale for the shaft mesh.
    float fVolumetricLightHeadlightLength = 25.0f;
    // Flags headlights get besides the shaft bit, none by default: the shaft draw loop (CE
    // 0xAC2A09) looks at no flag but the shaft bit and 0x10, so 0x1 made no difference.
    uint32_t nVolumetricLightHeadlightAddFlags = 0;
    // Headlight shafts with the headlight's shadow map, which leaves them unseen (see
    // InstallShaftHooks).
    bool bVolumetricLightHeadlightShadow = false;
    // Building Fill Lights in the graphics menu (PREF_FILL_LIGHTS): off darkens the large
    // exterior map lights, reaching at least FillLightsMinRadius, that flood whole squares and
    // building fronts. They made scenes look washed out, and at some cell edges (a garage at
    // x -900 in Algonquin) the game stops sending them with a step of the camera.
    float fFillLightsMinRadius = 30.0f;
    static bool FillLights() { static auto p = FusionFixSettings.GetRef("PREF_FILL_LIGHTS"); return p ? p->get() != 0 : true; }
    rage::grcRenderTargetPC* ContactRawTex = nullptr;
    IDirect3DSurface9* ContactRawSurf = nullptr;
    rage::grcRenderTargetPC* ContactTex = nullptr;
    IDirect3DSurface9* ContactSurf = nullptr;
    // Accumulation over frames (ContactTemporal_PS in SSR.fx): each frame blends the smoothed
    // contact shadows with the previous accumulation into the other target of the pair, and
    // moves every pixel's step offset on. ContactShadowsTemporal is the share of the history
    // kept, 0 turns it off. ContactPrevViewProj is kept here, as SSR's is only while SSR is on.
    rage::grcRenderTargetPC* ContactAccumTex[2] = {};
    IDirect3DSurface9* ContactAccumSurf[2] = {};
    int nContactAccumIndex = 0;
    bool bContactAccumValid = false;
    D3DXMATRIX ContactPrevViewProj = {};
    uint32_t nContactFrame = 0;
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
    float fSSRTowardCamera = 0.0f;
    float fSSRReflectionBlur = 0.0f;
    float fSSRDistanceFade = 0.0f;
    rage::grcRenderTargetPC* SSRDenoisedTex = nullptr;
    IDirect3DSurface9* SSRDenoisedSurf = nullptr;
    bool bSSRDenoised = false;
    // The same pair at half the resolution, for the Half quality setting; deferred_lighting
    // reads it with bilinear filtering. Both pairs are kept, so switching needs no reset.
    rage::grcRenderTargetPC* SSRHalfTex = nullptr;
    IDirect3DSurface9* SSRHalfSurf = nullptr;
    rage::grcRenderTargetPC* SSRHalfDenoisedTex = nullptr;
    IDirect3DSurface9* SSRHalfDenoisedSurf = nullptr;
    // Accumulation over frames (SSRTemporal_PS in SSR.fx): each frame blends the smoothed
    // result with the previous accumulation into the other target of a pair, one pair per
    // resolution. ScreenSpaceReflectionsTemporal is the share of the history kept, 0 turns
    // it off. The history is taken where the reflected image was last frame (SSRHitTex).
    // The pair is why lighting must get the result only once SSR is done, see BindSSRTexture.
    rage::grcRenderTargetPC* SSRAccumTex[2][2] = {}; // [half][ping-pong]
    IDirect3DSurface9* SSRAccumSurf[2][2] = {};
    // The reflected ray lengths of this frame's SSR pass, its second output, [half].
    rage::grcRenderTargetPC* SSRHitTex[2] = {};
    IDirect3DSurface9* SSRHitSurf[2] = {};
    int nSSRAccumIndex = 0;
    bool bSSRAccumValid = false;
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
    int nSSRSteps = 48;
    int nSSRRefineSteps = 8;
    float fSSRMaxDistance = 24.0f;
    float fSSRThickness = 0.3f;
    float fSSREdgeFade = 0.1f;
    float fSSRIntensity = 1.0f;
    float fSSRGlossBoost = 2.0f;
    float fSSRGlossCutoff = 0.5f;
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

    struct
    {
        D3DXHANDLE AOTexture2D, AOCamDepthTexture2D, DepthTex2D;

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
            D3DXMACRO defines[] = {
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
            D3DXMACRO defines[] = {
                {"NUM_STEPS", steps.c_str()},
                {"NUM_REFINE_STEPS", refineSteps.c_str()},
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
                h.vec4SunView = SSREffect->GetParameterByName(nullptr, "vec4SunView");
                h.fCSLength = SSREffect->GetParameterByName(nullptr, "fCSLength");
                h.fCSThickness = SSREffect->GetParameterByName(nullptr, "fCSThickness");
                h.fCSMaxThickness = SSREffect->GetParameterByName(nullptr, "fCSMaxThickness");
                h.fCSMaxViewDistance = SSREffect->GetParameterByName(nullptr, "fCSMaxViewDistance");
                h.fCSIntensity = SSREffect->GetParameterByName(nullptr, "fCSIntensity");
                h.techContactShadows = SSREffect->GetTechniqueByName("ContactShadows");
                h.techContactTemporal = SSREffect->GetTechniqueByName("ContactTemporal");
                h.fJitterOffset = SSREffect->GetParameterByName(nullptr, "fJitterOffset");
                h.techSSRDenoise = SSREffect->GetTechniqueByName("SSRDenoise");
                h.techSSRDebug = SSREffect->GetTechniqueByName("SSRDebug");
                h.techSSRDebugCopy = SSREffect->GetTechniqueByName("SSRDebugCopy");
                h.SSRAccumTex2D = SSREffect->GetParameterByName(nullptr, "SSRAccumTex2D");
                h.SSRHitTex2D = SSREffect->GetParameterByName(nullptr, "SSRHitTex2D");
                h.fTemporalBlend = SSREffect->GetParameterByName(nullptr, "fTemporalBlend");
                h.techSSRTemporal = SSREffect->GetTechniqueByName("SSRTemporal");
            }
        }

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

        nSSRSteps = std::clamp(iniReader.ReadInteger("POSTFX", "ScreenSpaceReflectionsSteps", 48), 4, 128);
        nSSRRefineSteps = std::clamp(iniReader.ReadInteger("POSTFX", "ScreenSpaceReflectionsRefineSteps", 8), 0, 16);
        fSSRMaxDistance = std::max(1.0f, iniReader.ReadFloat("POSTFX", "ScreenSpaceReflectionsMaxDistance", 24.0f));
        fSSRThickness = std::max(0.0f, iniReader.ReadFloat("POSTFX", "ScreenSpaceReflectionsThickness", 0.3f));
        fSSREdgeFade = std::clamp(iniReader.ReadFloat("POSTFX", "ScreenSpaceReflectionsEdgeFade", 0.1f), 0.001f, 0.5f);
        fSSRIntensity = std::clamp(iniReader.ReadFloat("POSTFX", "ScreenSpaceReflectionsIntensity", 1.0f), 0.0f, 1.0f);
        fSSRGlossBoost = std::clamp(iniReader.ReadFloat("POSTFX", "ScreenSpaceReflectionsGlossBoost", 2.0f), 0.0f, 8.0f);
        fSSRGlossCutoff = std::clamp(iniReader.ReadFloat("POSTFX", "ScreenSpaceReflectionsGlossCutoff", 0.5f), 0.0f, 1.0f);
        fSSRWaterIntensity = std::clamp(iniReader.ReadFloat("POSTFX", "ScreenSpaceReflectionsWaterIntensity", 1.0f), 0.0f, 1.0f);
        fSSRWaterLevelOffset = iniReader.ReadFloat("POSTFX", "ScreenSpaceReflectionsWaterLevelOffset", 0.0f);
        fSSRWaterBlur = std::clamp(iniReader.ReadFloat("POSTFX", "ScreenSpaceReflectionsWaterBlur", 3.0f), 0.0f, 32.0f);
        fSSRWaterNormalStrength = std::clamp(iniReader.ReadFloat("POSTFX", "ScreenSpaceReflectionsWaterRipple", 1.0f), 0.0f, 4.0f);
        bSSRGBufferNormals = iniReader.ReadInteger("POSTFX", "ScreenSpaceReflectionsGBufferNormals", 1) != 0;
        fSSRDenoiseRadius = std::clamp(iniReader.ReadFloat("POSTFX", "ScreenSpaceReflectionsSmoothing", 2.0f), 0.0f, 8.0f);
        bSSRPassThinObjects = iniReader.ReadInteger("POSTFX", "ScreenSpaceReflectionsPastThinObjects", 1) != 0;
        bSSRStepJitter = iniReader.ReadInteger("POSTFX", "ScreenSpaceReflectionsStepJitter", 1) != 0;
        fSSRTemporalBlend = std::clamp(iniReader.ReadFloat("POSTFX", "ScreenSpaceReflectionsTemporal", 0.85f), 0.0f, 0.97f);
        fSSRTowardCamera = std::clamp(iniReader.ReadFloat("POSTFX", "ScreenSpaceReflectionsTowardCamera", 0.0f), 0.0f, 1.0f);
        fSSRReflectionBlur = std::clamp(iniReader.ReadFloat("POSTFX", "ScreenSpaceReflectionsBlur", 0.0f), 0.0f, 32.0f);
        fSSRDistanceFade = std::clamp(iniReader.ReadFloat("POSTFX", "ScreenSpaceReflectionsDistanceFade", 0.0f), 0.0f, 100.0f);
        fContactShadowLength = std::clamp(iniReader.ReadFloat("POSTFX", "ContactShadowsLength", 0.3f), 0.05f, 10.0f);
        fContactShadowThickness = std::clamp(iniReader.ReadFloat("POSTFX", "ContactShadowsThickness", 0.15f), 0.01f, 10.0f);
        fContactShadowMaxThickness = std::clamp(iniReader.ReadFloat("POSTFX", "ContactShadowsMaxThickness", 2.0f), 0.0f, 10.0f);
        fContactShadowMaxDistance = std::clamp(iniReader.ReadFloat("POSTFX", "ContactShadowsMaxDistance", 60.0f), 1.0f, 1000.0f);
        fContactShadowIntensity = std::clamp(iniReader.ReadFloat("POSTFX", "ContactShadowsIntensity", 1.0f), 0.0f, 1.0f);
        bContactShadowStepJitter = iniReader.ReadInteger("POSTFX", "ContactShadowsStepJitter", 1) != 0;
        fContactTemporalBlend = std::clamp(iniReader.ReadFloat("POSTFX", "ContactShadowsTemporal", 0.8f), 0.0f, 0.95f);
        bLocalContactShadows = iniReader.ReadInteger("POSTFX", "LocalContactShadows", 1) != 0;
        fLocalContactShadowLength = std::clamp(iniReader.ReadFloat("POSTFX", "LocalContactShadowsLength", 0.5f), 0.05f, 10.0f);
        fLocalContactShadowThickness = std::clamp(iniReader.ReadFloat("POSTFX", "LocalContactShadowsThickness", 0.2f), 0.01f, 5.0f);
        fLocalContactShadowMaxThickness = std::clamp(iniReader.ReadFloat("POSTFX", "LocalContactShadowsMaxThickness", 2.0f), 0.0f, 10.0f);
        bLocalContactShadowsDebug = iniReader.ReadInteger("POSTFX", "LocalContactShadowsDebug", 0) != 0;
        fLocalContactShadowMaxDistance = std::clamp(iniReader.ReadFloat("POSTFX", "LocalContactShadowsMaxDistance", 40.0f), 1.0f, 1000.0f);
        fLocalContactShadowIntensity = std::clamp(iniReader.ReadFloat("POSTFX", "LocalContactShadowsIntensity", 1.0f), 0.0f, 1.0f);
        fVolumetricLightIntensity = std::clamp(iniReader.ReadFloat("POSTFX", "VolumetricLightIntensity", 4.0f), 0.0f, 20.0f);
        fVolumetricLightScale = std::clamp(iniReader.ReadFloat("POSTFX", "VolumetricLightScale", 0.25f), 0.0f, 2.0f);
        fVolumetricLightMaxDistance = std::clamp(iniReader.ReadFloat("POSTFX", "VolumetricLightMaxDistance", 100.0f), 10.0f, 1000.0f);
        nVolumetricLightHeadlightFlag = uint32_t(iniReader.ReadInteger("POSTFX", "VolumetricLightHeadlightFlag", 0x100));
        fVolumetricLightHeadlightIntensity = std::clamp(iniReader.ReadFloat("POSTFX", "VolumetricLightHeadlightIntensity", 2.0f), 0.0f, 20.0f);
        fVolumetricLightHeadlightLength = std::clamp(iniReader.ReadFloat("POSTFX", "VolumetricLightHeadlightLength", 25.0f), 1.0f, 200.0f);
        nVolumetricLightHeadlightAddFlags = uint32_t(iniReader.ReadInteger("POSTFX", "VolumetricLightHeadlightAddFlags", 0));
        fFillLightsMinRadius = std::clamp(iniReader.ReadFloat("POSTFX", "FillLightsMinRadius", 30.0f), 0.0f, 1000.0f);
        bVolumetricLightHeadlightShadow = iniReader.ReadInteger("POSTFX", "VolumetricLightHeadlightShadow", 0) != 0;
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

        rage::grcRenderTargetDesc desc{};
        desc.mMultisampleCount = 0;
        desc.field_0 = 1;
        desc.field_12 = 1;
        desc.mDepthRT = nullptr;
        desc.field_8 = 1;
        desc.field_10 = 1;
        desc.field_11 = 1;
        desc.field_24 = false;
        desc.mFormat = rage::GRCFMT_A16B16G16R16F;

        auto CreateEmptyRT = [](const char* name, int32_t a2, uint32_t width, uint32_t height, uint32_t bitsPerPixel, rage::grcRenderTargetDesc* desc) -> rage::grcRenderTargetPC*
        {
            auto rt = rage::grcTextureFactory::GetInstance()->CreateRenderTarget(name, a2, width, height, bitsPerPixel, desc);
            rage::grcDevice::grcResolveFlags resolveFlags{};
            rage::grcTextureFactoryPC::GetInstance()->LockRenderTarget(0, rt, nullptr);
            rage::grcTextureFactoryPC::GetInstance()->UnlockRenderTarget(0, &resolveFlags);
            return rt;
        };

        FullScreenTex_temp1 = CreateEmptyRT("FullScreenTex_temp1", 3, Width, Height, 64, &desc);

        desc.mFormat = rage::GRCFMT_A8R8G8B8;
        FullScreenTex_temp2 = CreateEmptyRT("FullScreenTex_temp2", 3, Width, Height, 32, &desc);

        //desc.mFormat = rage::GRCFMT_G16R16F;
        // 
        //pShadowBlurTex1 = CreateEmptyRT("pShadowBlurTex1", 3, Width, Height, 32, &desc);
        //pShadowBlurTex2 = CreateEmptyRT("pShadowBlurTex2", 3, Width, Height, 32, &desc);

        desc.mFormat = rage::GRCFMT_X8R8G8B8;

        edgesTex = CreateEmptyRT("edgesTex", 3, Width, Height, 32, &desc);

        desc.mFormat = rage::GRCFMT_A8R8G8B8;

        blendTex = CreateEmptyRT("blendTex", 3, Width, Height, 32, &desc);

        desc.mFormat = rage::GRCFMT_A16B16G16R16F;

        FullScreenDownsampleTex = CreateEmptyRT("FullScreenDownsampleTex", 3, Width / 2, Height / 2, 64, &desc);
        FullScreenDownsampleTex2 = CreateEmptyRT("FullScreenDownsampleTex2", 3, Width / 2, Height / 2, 64, &desc);

        // Always taken: SSR, its history check and the car glass read it. EnablePreAlphaDepth
        // decides only whether depth of field and sun shafts use it.
        desc.mFormat = rage::GRCFMT_R32F;
        PreAlphaDepthCopyRT = CreateEmptyRT("PreAlphaDepthCopy", 3, Width, Height, 32, &desc);


        if (!SMAA_areaTex)
            D3DXCreateTextureFromResourceExW(pDevice, hm, MAKEINTRESOURCEW(IDR_AreaTex), 160, 560, 1, 0, D3DFMT_UNKNOWN, D3DPOOL_MANAGED, D3DX_FILTER_LINEAR, D3DX_FILTER_LINEAR, 0, NULL, NULL, &SMAA_areaTex);
        if (!SMAA_searchTex)
            D3DXCreateTextureFromResourceExW(pDevice, hm, MAKEINTRESOURCEW(IDR_SearchTex), 64, 16, 1, 0, D3DFMT_UNKNOWN, D3DPOOL_MANAGED, D3DX_FILTER_LINEAR, D3DX_FILTER_LINEAR, 0, NULL, NULL, &SMAA_searchTex);
    }
};

PostFxResource PostFxResources;

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
            pDevice->SetTexture(9, nullptr);
        }
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
        for (auto* rt : { &PostFxResources.SSRHalfTex, &PostFxResources.SSRHalfDenoisedTex })
        {
            if (*rt)
            {
                (*rt)->Destroy();
                *rt = nullptr;
            }
        }
        PostFxResources.SSRResult = nullptr;
        for (auto* rt : { &PostFxResources.ContactRawTex, &PostFxResources.ContactTex, &PostFxResources.ContactAccumTex[0],
                          &PostFxResources.ContactAccumTex[1] })
        {
            if (*rt)
            {
                (*rt)->Destroy();
                *rt = nullptr;
            }
        }
        SAFE_RELEASE(PostFxResources.ContactRawSurf);
        SAFE_RELEASE(PostFxResources.ContactSurf);
        SAFE_RELEASE(PostFxResources.ContactAccumSurf[0]);
        SAFE_RELEASE(PostFxResources.ContactAccumSurf[1]);
        PostFxResources.ContactResult = nullptr;
        PostFxResources.bContactAccumValid = false;
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
        for (int half = 0; half < 2; ++half)
        {
            SAFE_RELEASE(PostFxResources.SSRHitSurf[half]);
            if (PostFxResources.SSRHitTex[half])
            {
                PostFxResources.SSRHitTex[half]->Destroy();
                PostFxResources.SSRHitTex[half] = nullptr;
            }
        }
        PostFxResources.bSSRAccumValid = false;
        PostFxResources.bSSRDebugValid = false;
        PostFxResources.bSSRValidThisFrame = false;
        PostFxResources.bSSRPrevViewProjValid = false;
        PostFxResources.bSSRReprojValid = false;
    }

    static void __fastcall OnDeviceLost()
    {
        PostFxResources.ReleaseTextures();
        PostFxResources.ReleaseWaterMask();
        UnbindGlassReflections();
        SAFE_RELEASE(PostFxResources.GlassParamsTex);
        ReleaseScreenSpaceTargets();
        // PostFxResources.mSpecularAoRT    =nullptr;
        PostFxResources.mNormalRT = nullptr;
        PostFxResources.mDiffuseRT = nullptr;
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
        // PostFxResources.mStencilRT      = rage::grcTextureFactoryPC::GetRTByName( "_STENCIL_BUFFER_"      );
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

        rage::grcRenderTargetDesc aoDesc{};
        aoDesc.mMultisampleCount = 0;
        aoDesc.field_0 = 1;
        aoDesc.field_12 = 1;
        aoDesc.mDepthRT = nullptr;
        aoDesc.field_8 = 1;
        aoDesc.field_10 = 1;
        aoDesc.field_11 = 1;
        aoDesc.field_24 = false;

        auto CreateEmptyRT = [](const char* name, int32_t a2, uint32_t w, uint32_t h, uint32_t bpp, rage::grcRenderTargetDesc* d) -> rage::grcRenderTargetPC*
        {
            auto rt = rage::grcTextureFactory::GetInstance()->CreateRenderTarget(name, a2, w, h, bpp, d);
            rage::grcDevice::grcResolveFlags resolveFlags{};
            rage::grcTextureFactoryPC::GetInstance()->LockRenderTarget(0, rt, nullptr);
            rage::grcTextureFactoryPC::GetInstance()->UnlockRenderTarget(0, &resolveFlags);
            return rt;
        };

        aoDesc.mFormat = rage::GRCFMT_R32F;
        aoDesc.mLevels = PostFxResources.nAmbientOcclusionMaxMipLevel;
        PostFxResources.AOCamDepthTex = CreateEmptyRT("AOCamDepthTex", 3, width, height, 32, &aoDesc);

        aoDesc.mFormat = rage::GRCFMT_L8;
        aoDesc.mLevels = 1;
        PostFxResources.AOTex = CreateEmptyRT("AOTex", 3, width, height, 8, &aoDesc);
        PostFxResources.AOBlurTex = CreateEmptyRT("AOBlurTex", 3, width, height, 8, &aoDesc);

        {
            aoDesc.mFormat = rage::GRCFMT_A16B16G16R16F;
            aoDesc.mLevels = 1;
            PostFxResources.SSRTex = CreateEmptyRT("SSRTex", 3, width, height, 64, &aoDesc);
            if (PostFxResources.SSRTex && PostFxResources.SSRTex->mD3DTexture)
                PostFxResources.SSRTex->mD3DTexture->GetSurfaceLevel(0, &PostFxResources.SSRSurf);

            PostFxResources.SSRHistoryTex = CreateEmptyRT("SSRHistoryTex", 3, width, height, 64, &aoDesc);
            if (PostFxResources.SSRHistoryTex && PostFxResources.SSRHistoryTex->mD3DTexture)
                PostFxResources.SSRHistoryTex->mD3DTexture->GetSurfaceLevel(0, &PostFxResources.SSRHistorySurf);

            PostFxResources.SSRDenoisedTex = CreateEmptyRT("SSRDenoisedTex", 3, width, height, 64, &aoDesc);
            if (PostFxResources.SSRDenoisedTex && PostFxResources.SSRDenoisedTex->mD3DTexture)
                PostFxResources.SSRDenoisedTex->mD3DTexture->GetSurfaceLevel(0, &PostFxResources.SSRDenoisedSurf);

            PostFxResources.SSRHalfTex = CreateEmptyRT("SSRHalfTex", 3, width / 2, height / 2, 64, &aoDesc);
            if (PostFxResources.SSRHalfTex && PostFxResources.SSRHalfTex->mD3DTexture)
                PostFxResources.SSRHalfTex->mD3DTexture->GetSurfaceLevel(0, &PostFxResources.SSRHalfSurf);
            PostFxResources.SSRHalfDenoisedTex = CreateEmptyRT("SSRHalfDenoisedTex", 3, width / 2, height / 2, 64, &aoDesc);
            if (PostFxResources.SSRHalfDenoisedTex && PostFxResources.SSRHalfDenoisedTex->mD3DTexture)
                PostFxResources.SSRHalfDenoisedTex->mD3DTexture->GetSurfaceLevel(0, &PostFxResources.SSRHalfDenoisedSurf);

            PostFxResources.ContactRawTex = CreateEmptyRT("ContactShadowRawTex", 3, width, height, 64, &aoDesc);
            if (PostFxResources.ContactRawTex && PostFxResources.ContactRawTex->mD3DTexture)
                PostFxResources.ContactRawTex->mD3DTexture->GetSurfaceLevel(0, &PostFxResources.ContactRawSurf);
            PostFxResources.ContactTex = CreateEmptyRT("ContactShadowTex", 3, width, height, 64, &aoDesc);
            if (PostFxResources.ContactTex && PostFxResources.ContactTex->mD3DTexture)
                PostFxResources.ContactTex->mD3DTexture->GetSurfaceLevel(0, &PostFxResources.ContactSurf);
            if (PostFxResources.fContactTemporalBlend > 0.0f)
            {
                static const char* names[2] = { "ContactShadowAccumTex0", "ContactShadowAccumTex1" };
                for (int i = 0; i < 2; ++i)
                {
                    auto& rt = PostFxResources.ContactAccumTex[i];
                    rt = CreateEmptyRT(names[i], 3, width, height, 64, &aoDesc);
                    if (rt && rt->mD3DTexture)
                        rt->mD3DTexture->GetSurfaceLevel(0, &PostFxResources.ContactAccumSurf[i]);
                }
            }

            if (PostFxResources.fSSRTemporalBlend > 0.0f)
            {
                static const char* names[2][2] = { { "SSRAccumTex0", "SSRAccumTex1" }, { "SSRHalfAccumTex0", "SSRHalfAccumTex1" } };
                for (int half = 0; half < 2; ++half)
                    for (int i = 0; i < 2; ++i)
                    {
                        auto& rt = PostFxResources.SSRAccumTex[half][i];
                        rt = CreateEmptyRT(names[half][i], 3, half ? width / 2 : width, half ? height / 2 : height, 64, &aoDesc);
                        if (rt && rt->mD3DTexture)
                            rt->mD3DTexture->GetSurfaceLevel(0, &PostFxResources.SSRAccumSurf[half][i]);
                    }
                // The same format as the SSR target it is written along with: older hardware
                // takes multiple render targets only of one bit depth.
                static const char* hitNames[2] = { "SSRHitTex", "SSRHalfHitTex" };
                for (int half = 0; half < 2; ++half)
                {
                    auto& rt = PostFxResources.SSRHitTex[half];
                    rt = CreateEmptyRT(hitNames[half], 3, half ? width / 2 : width, half ? height / 2 : height, 64, &aoDesc);
                    if (rt && rt->mD3DTexture)
                        rt->mD3DTexture->GetSurfaceLevel(0, &PostFxResources.SSRHitSurf[half]);
                }
            }

            PostFxResources.SSRDebugTex = CreateEmptyRT("SSRDebugTex", 3, width, height, 64, &aoDesc);
            if (PostFxResources.SSRDebugTex && PostFxResources.SSRDebugTex->mD3DTexture)
                PostFxResources.SSRDebugTex->mD3DTexture->GetSurfaceLevel(0, &PostFxResources.SSRDebugSurf);

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

                    pDevice->SetTexture(0, prevTex[1]);

                    pDevice->SetPixelShader(PostFxResources.Blit_PS);

                    pDevice->DrawPrimitive(D3DPT_TRIANGLELIST, 0, 2);

                    pDevice->SetSamplerState(0, D3DSAMP_MINFILTER, prevMinFilter[0]);
                    pDevice->SetSamplerState(0, D3DSAMP_MAGFILTER, prevMagFilter[0]);
                    pDevice->SetSamplerState(0, D3DSAMP_MIPFILTER, prevMipFilter[0]);
                    pDevice->SetSamplerState(0, D3DSAMP_ADDRESSU, prevAddressU[0]);
                    pDevice->SetSamplerState(0, D3DSAMP_ADDRESSV, prevAddressV[0]);
                    pDevice->SetTexture(0, prevTex[0]);

                    if (PostFxResources.SSREnabled() && PostFxResources.SSRHistorySurf && PostFxResources.SSRSurf)
                    {
                        D3DVIEWPORT9 vpBeforeCapture;
                        pDevice->GetViewport(&vpBeforeCapture);

                        pDevice->SetRenderTarget(0, PostFxResources.SSRSurf);
                        pDevice->StretchRect(PostFxResources.HDRFullScreenSurface, nullptr, PostFxResources.SSRHistorySurf, nullptr, D3DTEXF_NONE);
                        pDevice->SetRenderTarget(0, PostFxResources.HDRFullScreenSurface);
                        PostFxResources.bSSRHistoryThisFrame = true;

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

                    hbDrawPrimitivePostFX.fun();
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

                        if (UsePostFxAA->get() > FusionFixSettings.AntialiasingText.eMO_OFF)
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
                    if (UsePostFxAA && UsePostFxAA->get() > FusionFixSettings.AntialiasingText.eMO_OFF)
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
                        if (UsePostFxAA->get() >= FusionFixSettings.AntialiasingText.eSMAA &&
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
        // The SSR pass writes its ray lengths to a second target while it accumulates.
        { D3DRS_COLORWRITEENABLE1, D3DCOLORWRITEENABLE_RED | D3DCOLORWRITEENABLE_GREEN | D3DCOLORWRITEENABLE_BLUE | D3DCOLORWRITEENABLE_ALPHA },
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
    static constexpr UINT kPSConstCount = 224;
    static constexpr UINT kVSConstCount = 256;
    static inline float savedPSConsts[kPSConstCount * 4];
    static inline float savedVSConsts[kVSConstCount * 4];

    static void MatrixMultiply(D3DXMATRIX& out, const D3DXMATRIX& a, const D3DXMATRIX& b)
    {
        D3DXMATRIX r;
        for (int i = 0; i < 4; ++i)
            for (int j = 0; j < 4; ++j)
                r.m[i][j] = a.m[i][0] * b.m[0][j] + a.m[i][1] * b.m[1][j]
                          + a.m[i][2] * b.m[2][j] + a.m[i][3] * b.m[3][j];
        out = r;
    }

    // Binds every sampler of the pixel shader of the pass just begun to the texture its effect
    // parameter holds, found through the shader's constant table (sampler X reads X2D in SSR.fx
    // and AO.fx). D3DX left some holding what the game had bound, about four a frame in the SSR
    // passes: a diagnostic pass read a G-buffer texture where it sampled the depth.
    static void BindEffectSamplers(IDirect3DDevice9* pDevice, ID3DXEffect* effect)
    {
        IDirect3DPixelShader9* ps = nullptr;
        if (FAILED(pDevice->GetPixelShader(&ps)) || !ps)
            return;
        std::vector<DWORD> function;
        UINT size = 0;
        if (SUCCEEDED(ps->GetFunction(nullptr, &size)) && size)
        {
            function.resize((size + 3) / 4);
            if (FAILED(ps->GetFunction(function.data(), &size)))
                function.clear();
        }
        ps->Release();
        ID3DXConstantTable* table = nullptr;
        if (function.empty() || FAILED(D3DXGetShaderConstantTable(function.data(), &table)) || !table)
            return;

        D3DXCONSTANTTABLE_DESC tableDesc = {};
        table->GetDesc(&tableDesc);
        for (UINT i = 0; i < tableDesc.Constants; ++i)
        {
            D3DXCONSTANT_DESC desc = {};
            UINT count = 1;
            if (FAILED(table->GetConstantDesc(table->GetConstant(nullptr, i), &desc, &count)) ||
                desc.RegisterSet != D3DXRS_SAMPLER || !desc.Name)
                continue;
            D3DXHANDLE param = effect->GetParameterByName(nullptr, (std::string(desc.Name) + "2D").c_str());
            if (!param)
                continue;
            IDirect3DBaseTexture9* want = nullptr;
            IDirect3DBaseTexture9* have = nullptr;
            effect->GetTexture(param, &want);
            pDevice->GetTexture(desc.RegisterIndex, &have);
            if (want != have)
                pDevice->SetTexture(desc.RegisterIndex, want);
            SAFE_RELEASE(want);
            SAFE_RELEASE(have);
        }
        table->Release();
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
            return;

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
            clearSSR();
            R.bSSRAccumValid = false;
            return;
        }

        rage::grcViewport* vp = rage::GetCurrentViewport();
        if (!R.SSREffect || !R.mDepthRT || !R.SSRHistoryTex || !vp || R.fSSRIntensity <= 0.0f)
        {
            clearSSR();
            R.bSSRAccumValid = false;
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

        float width = float(vp->mWidth);
        float height = float(vp->mHeight);

        // Half: the march and the smoothing run on the half size targets, created as the full
        // size halved; the debug view stays full size.
        const bool half = R.SSRHalfRes() && R.SSRHalfSurf && R.SSRHalfDenoisedSurf;
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

            float invViewportSize[] = { 1.0f / w, 1.0f / hgt };
            effect->SetFloatArray(h.vec2InvViewportSize, invViewportSize, 2);

            // Same reconstruction basis the AO pass uses.
            D3DXVECTOR4 projInfo;
            projInfo.x = -2.0f / (w * proj._11);
            projInfo.y = -2.0f / (hgt * proj._22);
            projInfo.z = (1.0f - proj._31) / proj._11;
            projInfo.w = (1.0f + proj._32) / proj._22;
            effect->SetVector(h.vec4ProjInfo, &projInfo);
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

        effect->SetFloat(h.fNearPlane, vp->mNearClip);
        effect->SetFloat(h.fFarDivNear, vp->mFarClip / vp->mNearClip);

        D3DXMATRIX viewProj;
        MatrixMultiply(viewProj, *(const D3DXMATRIX*)vp->mViewMatrix, *(const D3DXMATRIX*)vp->mProjectionMatrix);

        if (!R.bSSRPrevViewProjValid)
            R.SSRPrevViewProj = viewProj;

        D3DXVECTOR4 reprojRows[4];
        ViewToClipRows(vp, R.SSRPrevViewProj, reprojRows);
        effect->SetVectorArray(h.vec4ViewToPrevClip, reprojRows, 4);
        memcpy(R.SSRReprojRows, reprojRows, sizeof(reprojRows));
        R.bSSRReprojValid = true;

        R.SSRPrevViewProj = viewProj;
        R.bSSRPrevViewProjValid = true;

        effect->SetFloat(h.fMaxDistance, R.fSSRMaxDistance);
        effect->SetFloat(h.fThickness, R.fSSRThickness);
        effect->SetFloat(h.fEdgeFade, R.fSSREdgeFade);
        effect->SetFloat(h.fIntensity, R.fSSRIntensity);
        effect->SetFloat(h.fPassThinObjects, R.bSSRPassThinObjects ? 1.0f : 0.0f);
        effect->SetFloat(h.fStepJitter, R.bSSRStepJitter ? 1.0f : 0.0f);
        const bool temporal = R.fSSRTemporalBlend > 0.0f && h.techSSRTemporal && R.SSRAccumSurf[half][0] && R.SSRAccumSurf[half][1] &&
                              R.SSRHitSurf[half];
        effect->SetFloat(h.fTowardCamera, R.fSSRTowardCamera);
        effect->SetFloat(h.fReflectionBlur, R.fSSRReflectionBlur);
        effect->SetFloat(h.fDistanceFade, R.fSSRDistanceFade);

        // World to reconstruction space rotation, for the G-buffer normals and the debug view.
        const D3DXMATRIX& viewInv = *(const D3DXMATRIX*)vp->mViewInverseMatrix;
        {
            const float axisSign[3] = { -1.0f, 1.0f, (proj._34 < 0.0f) ? -1.0f : 1.0f };
            D3DXVECTOR4 toView[3];
            for (int row = 0; row < 3; ++row)
                toView[row] = D3DXVECTOR4(viewInv.m[row][0] * axisSign[row], viewInv.m[row][1] * axisSign[row],
                                          viewInv.m[row][2] * axisSign[row], 0.0f);
            effect->SetVectorArray(h.vec4WaterToView, toView, 3);
        }
        const bool hasNormals = R.mNormalRT && R.mNormalRT->mD3DTexture;
        if (hasNormals)
            effect->SetTexture(h.NormalTex2D, R.mNormalRT->mD3DTexture);
        effect->SetFloat(h.fUseGBufferNormals, (hasNormals && R.bSSRGBufferNormals) ? 1.0f : 0.0f);

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

            effect->SetTechnique(h.techSSR);
            effect->Begin(&passes, 0);
        }
        // SSR_PS writes its ray lengths to COLOR1: into SSRHitTex while accumulating, else
        // nowhere, never into whatever target the game left there.
        IDirect3DSurface9* oldRT1 = nullptr;
        pDevice->GetRenderTarget(1, &oldRT1);
        pDevice->SetRenderTarget(1, temporal ? R.SSRHitSurf[half ? 1 : 0] : nullptr);
        {
            pDevice->SetRenderTarget(0, ssrSurf);
            pDevice->Clear(0, nullptr, D3DCLEAR_TARGET, D3DCOLOR_ARGB(0, 0, 0, 0), 1.0f, 0);

            effect->BeginPass(0);
            effect->CommitChanges();
            BindEffectSamplers(pDevice, effect);
            pDevice->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, screenVertices, sizeof(ScreenVertex));
            effect->EndPass();
        }
        effect->End();
        pDevice->SetRenderTarget(1, nullptr);

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
            effect->Begin(&passes, 0);
            effect->BeginPass(0);
            effect->CommitChanges();
            BindEffectSamplers(pDevice, effect);
            pDevice->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, screenVertices, sizeof(ScreenVertex));
            effect->EndPass();
            effect->End();
            R.bSSRDenoised = true;
        }
        IDirect3DTexture9* ssrResult = R.bSSRDenoised ? denoisedTex : ssrTex;

        if (temporal)
        {
            const int sizeIndex = half ? 1 : 0;
            if (R.bSSRAccumHalf != half)
                R.bSSRAccumValid = false;
            R.bSSRAccumHalf = half;
            const int prev = R.nSSRAccumIndex, next = prev ^ 1;
            effect->SetTexture(h.SSRResultTex2D, ssrResult);
            effect->SetTexture(h.SSRAccumTex2D, R.SSRAccumTex[sizeIndex][prev]->mD3DTexture);
            effect->SetTexture(h.SSRHitTex2D, R.SSRHitTex[sizeIndex]->mD3DTexture);
            effect->SetFloat(h.fTemporalBlend, R.bSSRAccumValid ? R.fSSRTemporalBlend : 0.0f);
            pDevice->SetRenderTarget(0, R.SSRAccumSurf[sizeIndex][next]);
            effect->SetTechnique(h.techSSRTemporal);
            effect->Begin(&passes, 0);
            effect->BeginPass(0);
            effect->CommitChanges();
            BindEffectSamplers(pDevice, effect);
            pDevice->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, screenVertices, sizeof(ScreenVertex));
            effect->EndPass();
            effect->End();
            ssrResult = R.SSRAccumTex[sizeIndex][next]->mD3DTexture;
            R.nSSRAccumIndex = next;
            R.bSSRAccumValid = true;
        }
        else
            R.bSSRAccumValid = false;
        R.SSRResult = ssrResult;

        const int debugMode = R.SSRDebugMode();
        if (debugMode && debugMode < R.kGlassDebugMode && R.SSRDebugSurf && h.techSSRDebug && hasNormals)
        {
            if (half)
                setPassSize(width, height);
            effect->SetTexture(h.SSRResultTex2D, ssrResult);
            effect->SetFloat(h.fDebugMode, float(debugMode));

            pDevice->SetRenderTarget(0, R.SSRDebugSurf);
            effect->SetTechnique(h.techSSRDebug);
            effect->Begin(&passes, 0);
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
        pDevice->SetRenderTarget(1, oldRT1);
        SAFE_RELEASE(oldRT1);
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

        float width = float(vp->mWidth);
        float height = float(vp->mHeight);

        auto& h = R.SSREffectHandles;
        ID3DXEffect* effect = R.SSREffect;

        effect->SetTexture(h.DepthTex2D, R.mDepthRT->mD3DTexture);
        effect->SetTexture(h.HistoryTex2D, R.SSRHistoryTex->mD3DTexture);

        float invViewportSize[] = { 1.0f / width, 1.0f / height };
        effect->SetFloatArray(h.vec2InvViewportSize, invViewportSize, 2);
        effect->SetFloat(h.fNearPlane, vp->mNearClip);
        effect->SetFloat(h.fFarDivNear, vp->mFarClip / vp->mNearClip);

        D3DXVECTOR4 projInfo;
        projInfo.x = -2.0f / ((width) * proj._11);
        projInfo.y = -2.0f / ((height) * proj._22);
        projInfo.z = (1.0f - proj._31) / proj._11;
        projInfo.w = (1.0f + proj._32) / proj._22;
        effect->SetVector(h.vec4ProjInfo, &projInfo);

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
        for (int row = 0; row < 3; ++row)
            toView[row] = D3DXVECTOR4(viewInv.m[row][0] * axisSign[row], viewInv.m[row][1] * axisSign[row],
                                      viewInv.m[row][2] * axisSign[row], 0.0f);
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
        return desc.Width == screen.Width && desc.Height == screen.Height;
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
            RenderWaterReflections();
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
            effect->Begin(&passes, 0); assert(passes == 5);
            {
                rage::grcViewport* currGrcViewport = rage::GetCurrentViewport();

                IDirect3DTexture9* camDepthTex = PostFxResources.AOCamDepthTex->mD3DTexture;
                IDirect3DTexture9* aoTex = PostFxResources.AOTex->mD3DTexture;
                IDirect3DTexture9* aoBlurTex = PostFxResources.AOBlurTex->mD3DTexture;
                auto& camDepthSurf = PostFxResources.AOCamDepthSurf;
                IDirect3DSurface9* aoSurf = PostFxResources.AOSurf;
                IDirect3DSurface9* aoBlurSurf = PostFxResources.AOBlurSurf;

                float width = float(currGrcViewport->mWidth);
                float height = float(currGrcViewport->mHeight);

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
                effect->SetFloatArray(h.vec2InvViewportSize, invViewportSize, 2);
                effect->SetFloat(h.fNearPlane, currGrcViewport->mNearClip);
                effect->SetFloat(h.fFarPlane, currGrcViewport->mFarClip);
                effect->SetFloat(h.fFarDivNear, currGrcViewport->mFarClip / currGrcViewport->mNearClip);

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
                effect->SetFloat(h.fBias, PostFxResources.fAmbientOcclusionBias);
                effect->SetFloat(h.fIntensity, PostFxResources.fAmbientOcclusionIntensity);
                effect->SetFloat(h.fProjScale, proj._22 * 0.5f * height);

                D3DXVECTOR4 projInfo;
                projInfo.x = -2.0f / ((width)*proj._11);
                projInfo.y = -2.0f / ((height)*proj._22);
                projInfo.z = (1.0f - proj._31) / proj._11;
                projInfo.w = (1.0f + proj._32) / proj._22;

                effect->SetVector(h.vec4ProjInfo, &projInfo);

                effect->CommitChanges();

                effect->BeginPass(2);
                BindEffectSamplers(pDevice, effect);
                pDevice->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, screenVertices, sizeof(ScreenVertex));
                effect->EndPass();

                float hor[2] = { invViewportSize[0] * PostFxResources.fAmbientOcclusionBlurRadius, 0.0 };
                float ver[2] = { 0.0, invViewportSize[1] * PostFxResources.fAmbientOcclusionBlurRadius };
                effect->BeginPass(3);
                for (auto i = 0; i < PostFxResources.nAmbientOcclusionBlurPasses; ++i)
                {
                    pDevice->SetRenderTarget(0, aoBlurSurf);
                    effect->SetTexture(h.AOTexture2D, aoTex); // blur pass 1
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
                effect->SetTexture(h.AOTexture2D, aoTex);

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
    }

    // Replaces the finished frame with the SSR debug view chosen in the graphics menu.
    static void DrawSSRDebugOverlay()
    {
        auto& R = PostFxResources;
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

    static inline injector::hook_back<void(__stdcall*)()> hbDrawPrimitivePostFX;
    static void __stdcall DrawPrimitivePostFX()
    {
        if (bInsteadDrawPrimitiveFog)
        {
            bInsteadDrawPrimitiveFog = false;
            // Do not initialize shaders, RTs etc. here or we get a device reset error for some reason
            NewFog();
        }
        else if (bInsteadDrawPrimitivePostFX)
        {
            bInsteadDrawPrimitivePostFX = false;
            Init();
            NewPostFX();
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

        RenderAmbientOcclusion();
        RenderScreenSpaceReflections();
        RenderContactShadows();
        // deferred_lighting draws after this; BindSSRTexture bound last frame's results.
        if (auto pDevice = rage::grcDevice::GetD3DDevice())
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
        const bool accumWasValid = R.bContactAccumValid;
        R.bContactAccumValid = false;

        // For the light shaders, whatever becomes of the sun's pass below.
        {
            rage::grcViewport* camera = rage::GetCurrentViewport();
            const bool local = camera && R.bLocalContactShadows && R.ContactShadowsEnabled() && R.fLocalContactShadowIntensity > 0.0f;
            const D3DMATRIX proj = camera ? *(const D3DMATRIX*)camera->mProjectionMatrix : D3DMATRIX{};
            const float consts[12] =
            {
                R.fLocalContactShadowLength, R.fLocalContactShadowThickness, R.fLocalContactShadowMaxDistance, R.fLocalContactShadowIntensity,
                proj._34, R.fLocalContactShadowMaxThickness, R.bLocalContactShadowsDebug ? 1.0f : 0.0f, local ? 12345.0f : 0.0f,
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

        const float width = float(vp->mWidth);
        const float height = float(vp->mHeight);
        D3DMATRIX proj = *(D3DMATRIX*)vp->mProjectionMatrix;
        const D3DXMATRIX& viewInv = *(const D3DXMATRIX*)vp->mViewInverseMatrix;
        const float axisSign[3] = { -1.0f, 1.0f, (proj._34 < 0.0f) ? -1.0f : 1.0f };

        D3DXVECTOR4 toView[3];
        for (int row = 0; row < 3; ++row)
            toView[row] = D3DXVECTOR4(viewInv.m[row][0] * axisSign[row], viewInv.m[row][1] * axisSign[row],
                                      viewInv.m[row][2] * axisSign[row], 0.0f);
        D3DXVECTOR4 sun(0.0f, 0.0f, 0.0f, 1.0f);
        for (int row = 0; row < 3; ++row)
            (&sun.x)[row] = -(toView[row].x * light[0] + toView[row].y * light[1] + toView[row].z * light[2]) / lightLen;

        D3DXVECTOR4 projInfo;
        projInfo.x = -2.0f / (width * proj._11);
        projInfo.y = -2.0f / (height * proj._22);
        projInfo.z = (1.0f - proj._31) / proj._11;
        projInfo.w = (1.0f + proj._32) / proj._22;

        const bool hasNormals = R.mNormalRT && R.mNormalRT->mD3DTexture;
        const float invViewportSize[] = { 1.0f / width, 1.0f / height };
        effect->SetTexture(h.DepthTex2D, R.mDepthRT->mD3DTexture);
        if (hasNormals)
            effect->SetTexture(h.NormalTex2D, R.mNormalRT->mD3DTexture);
        effect->SetFloat(h.fUseGBufferNormals, (hasNormals && R.bSSRGBufferNormals) ? 1.0f : 0.0f);
        effect->SetVectorArray(h.vec4WaterToView, toView, 3);
        effect->SetFloatArray(h.vec2InvViewportSize, invViewportSize, 2);
        effect->SetFloat(h.fNearPlane, vp->mNearClip);
        effect->SetFloat(h.fFarDivNear, vp->mFarClip / vp->mNearClip);
        effect->SetVector(h.vec4ProjInfo, &projInfo);
        effect->SetVector(h.vec4SunView, &sun);
        effect->SetFloat(h.fStepJitter, R.bContactShadowStepJitter ? 1.0f : 0.0f);
        const bool temporal = R.fContactTemporalBlend > 0.0f && h.techContactTemporal && R.ContactAccumSurf[0] &&
                              R.ContactAccumSurf[1];
        // Golden ratio steps through the offsets, from an integer so they stay spread however
        // long the game runs.
        ++R.nContactFrame;
        effect->SetFloat(h.fJitterOffset, temporal ? float(R.nContactFrame * 2654435769u) * (1.0f / 4294967296.0f) : 0.0f);
        effect->SetFloat(h.fCSLength, R.fContactShadowLength);
        effect->SetFloat(h.fCSThickness, R.fContactShadowThickness);
        effect->SetFloat(h.fCSMaxThickness, R.fContactShadowMaxThickness);
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
            D3DXMATRIX viewProj;
            MatrixMultiply(viewProj, *(const D3DXMATRIX*)vp->mViewMatrix, *(const D3DXMATRIX*)vp->mProjectionMatrix);
            if (!accumWasValid)
                R.ContactPrevViewProj = viewProj;
            D3DXVECTOR4 reprojRows[4];
            ViewToClipRows(vp, R.ContactPrevViewProj, reprojRows);
            // Last frame's fog pass copied its depth; this frame's has not run yet.
            const bool prevDepth = R.PreAlphaDepthCopyRT && R.PreAlphaDepthCopyRT->mD3DTexture;

            const int prev = R.nContactAccumIndex, next = prev ^ 1;
            effect->SetTexture(h.SSRResultTex2D, R.ContactTex->mD3DTexture);
            effect->SetTexture(h.SSRAccumTex2D, R.ContactAccumTex[prev]->mD3DTexture);
            effect->SetTexture(h.PrevDepthTex2D, prevDepth ? R.PreAlphaDepthCopyRT->mD3DTexture : nullptr);
            effect->SetFloat(h.fUsePrevDepth, prevDepth ? 1.0f : 0.0f);
            effect->SetVectorArray(h.vec4ViewToPrevClip, reprojRows, 4);
            effect->SetFloat(h.fTemporalBlend, accumWasValid ? R.fContactTemporalBlend : 0.0f);
            DrawEffectPass(pDevice, effect, h.techContactTemporal, R.ContactAccumSurf[next], width, height);

            result = R.ContactAccumTex[next]->mD3DTexture;
            R.ContactResult = result;
            R.nContactAccumIndex = next;
            R.ContactPrevViewProj = viewProj;
            R.bContactAccumValid = true;
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

    // The game's light shaft loop (CE 0xAC2A09) draws a shaft with the shadowed shaft technique
    // when a shadow map is found for its light. Headlights nearly always have one there, and a
    // shaft drawn with it shows nothing, so unless VolumetricLightHeadlightShadow is on, the draw
    // hook clears that choice for lights with the headlight flag and they take the plain one.
    static inline SafetyHookMid shShaftDraw{};

    static void InstallShaftHooks()
    {
        auto& R = PostFxResources;
        if (!R.nVolumetricLightHeadlightFlag || R.bVolumetricLightHeadlightShadow)
            return;
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

    // Runs as the game copies each light into the frame's draw list, on the main thread.
    static void OnAfterCopyLight(rage::CLightSource* light)
    {
        auto& R = PostFxResources;
        if (!light)
            return;
        // Building Fill Lights off: the large exterior map lights (0x1 and 0x40, no interior 0x20,
        // vehicle 0x100 or traffic light and fire 0x200) that flood whole squares go dark.
        if (!R.FillLights() && (light->mFlags & 0x361) == 0x41 &&
            (light->mType == rage::LT_POINT || light->mType == rage::LT_SPOT) && light->mRadius >= R.fFillLightsMinRadius)
            light->mIntensity = 0.0f;
        if (!R.VolumetricLight() || R.fVolumetricLightIntensity <= 0.0f)
            return;

        Cam camera = 0;
        rage::Vector3 cameraPos{};
        Natives::GetRootCam(&camera);
        Natives::GetCamPos(camera, &cameraPos.x, &cameraPos.y, &cameraPos.z);
        const float dx = cameraPos.x - light->mPosition.x;
        const float dy = cameraPos.y - light->mPosition.y;
        const float dz = cameraPos.z - light->mPosition.z;
        const float distance = std::sqrt(dx * dx + dy * dy + dz * dz);

        // Light flags, from the game's own calls to its light submission (CE 0xABCC50, which
        // clears the shaft bit, and 0xABCCD0): 0x1 map (2dfx) lights, and effects; 0x2 and 0x10
        // two 2dfx flags, 0x10 also vehicle point lights; 0x4 casts a shadow; 0x8 light shaft;
        // 0x20/0x40 set by the game for interior/exterior; 0x80 lights the game never culls;
        // 0x100 vehicle lights, 0x400 more with it on headlights; 0x200 traffic lights, fires
        // and explosions (0x201).
        // Spot lights of 8 to 20 m, most of lamppost.img, and none of 0x398 or lights that have
        // a shaft already (8); headlights
        // are the spot lights of at least 8 m with the headlight flag (smaller ones are tail and
        // brake lights), whatever their other flags.
        if (light->mType != rage::LT_SPOT || (light->mFlags & 8))
            return;
        const uint32_t headlightFlag = R.nVolumetricLightHeadlightFlag;
        const bool headlight = headlightFlag && (light->mFlags & headlightFlag) && light->mRadius >= 8.0f &&
                               R.fVolumetricLightHeadlightIntensity > 0.0f;
        if (!headlight && (light->mRadius < 8.0f || light->mRadius > 20.0f || (light->mFlags & 0x398)))
            return;
        const float fadeStart = R.fVolumetricLightMaxDistance * 0.3f;
        const float x = std::clamp((distance - fadeStart) / (R.fVolumetricLightMaxDistance - fadeStart), 0.0f, 1.0f);
        const float fade = 1.0f - x * x * (3.0f - 2.0f * x);
        if (fade <= 0.0f)
            return;

        light->mFlags |= 8; // light shaft
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
    // are valid, and c202-c204 the local light contact shadow constants.
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

        IDirect3DBaseTexture9* tex = R.TransparentTex();
        if (R.SSRResult)
            tex = R.SSRResult;
        else if (R.SSRTex && R.SSRTex->mD3DTexture)
            tex = R.SSRTex->mD3DTexture; // cleared while SSR is off
        pDevice->SetTexture(3, tex);
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
            BindLightingInputs(pDevice);
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

        if (R.bContactBound)
        {
            pDevice->SetTexture(9, nullptr);
            R.bContactBound = false;
        }
        // Lights drawn for other views (reflections, mirrors) must not march with this camera.
        const float noLocalContactShadows[4] = {};
        pDevice->SetPixelShaderConstantF(203, noLocalContactShadows, 1);

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

                auto pattern = find_pattern("E8 ? ? ? ? 8B 4F ? E8 ? ? ? ? 8B 4F ? E8 ? ? ? ? 5F", "E8 ? ? ? ? 8B 4F ? E8 ? ? ? ? 8B 4F ? E8 ? ? ? ? 33 C0");
                hbDrawPrimitivePostFX.fun = injector::MakeCALL(pattern.get_first(0), DrawPrimitivePostFX).get();

                pattern = find_pattern("E8 ? ? ? ? 8D 44 24 ? 50 8B CF E8 ? ? ? ? 8D 84 24", "E8 ? ? ? ? 8D 44 24 ? 50 8B CE E8 ? ? ? ? 8D 8C 24 ? ? ? ? 51 8B CE E8 ? ? ? ? 8D 94 24");
                hbDrawSkyHook.fun = injector::MakeCALL(pattern.get_first(0), DrawSky).get();

                pattern = find_pattern("E8 ? ? ? ? 6A ? FF B7 ? ? ? ? 8B CF FF 77 ? E8 ? ? ? ? 5F", "E8 ? ? ? ? 8B 8E ? ? ? ? 8B 56 ? 6A ? 51");
                hbDrawCallPostFX.fun = injector::MakeCALL(pattern.get_first(0), DrawCallPostFX).get();

                pattern = find_pattern("55 8B EC 83 E4 F0 81 EC D8 00 00 00 56 57 E8 ? ? ? ?");
                if (!pattern.empty())
                    shWaterRender = safetyhook::create_inline(pattern.get_first(0), WaterRenderHook);

                pattern = find_pattern("F3 0F 10 05 ? ? ? ? F3 0F 11 44 24 08 FF 74 24 08");
                if (!pattern.empty())
                    PostFxResources.pWaterLevel = *pattern.get_first<const float*>(4);

                {
                    CRenderPhaseDeferredLighting_LightsToScreen::OnAfterCopyLight() += OnAfterCopyLight;
                    InstallShaftHooks();
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
                    pattern = hook::pattern("6A ? E8 ? ? ? ? 5E 8B E5 5D C3");
                    if (!pattern.empty())
                    {
                        hbDrawCallFog.fun = injector::MakeCALL(pattern.get_first(2), DrawCallFog).get();
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