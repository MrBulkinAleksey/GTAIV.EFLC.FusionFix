texture DepthTex2D, HistoryTex2D, SpecularTex2D, SurfaceTex2D, NormalTex2D, SSRResultTex2D, DebugTex2D;
texture PreWaterTex2D, PostWaterTex2D;
texture PrevDepthTex2D;
texture SSRAccumTex2D;
texture SSRFallbackTex2D;
texture SSRHitDistTex2D;
texture MarchDepthTex2D, HiZDepthTex2D, HiZDepthTex2D1;
texture MotionTex2D;
texture AlbedoTex2D, AlbedoLinearTex2D, GIPrevTex2D;
texture SceneTex2D, SkinIDTex2D, SkinLightTex2D;
texture ContactInTex2D, ContactHistTex2D;

sampler2D DepthTex
{
    Texture = <DepthTex2D>;
};

sampler2D HistoryTex
{
    Texture = <HistoryTex2D>;
    MinFilter = LINEAR;
    MagFilter = LINEAR;
};

// The log depth the fog pass copied along with HistoryTex, so it matches the history colour.
sampler2D PrevDepthTex
{
    Texture = <PrevDepthTex2D>;
    AddressU = Clamp;
    AddressV = Clamp;
    MinFilter = LINEAR;
    MagFilter = LINEAR;
    MipFilter = NONE;
};

sampler2D SpecularTex
{
    Texture = <SpecularTex2D>;
};

sampler2D NormalTex
{
    Texture = <NormalTex2D>;
};

// The smoothing writes this at half size even when SSR runs full, so the passes that read it
// afterwards - the accumulation, the debug view, deferred_lighting - want it bilinear: at the
// same size a bilinear read of pixel centres is the point read they had, and across sizes it is
// the upsample a blurred signal survives.
sampler2D SSRResultTex
{
    Texture = <SSRResultTex2D>;
    AddressU = Clamp;
    AddressV = Clamp;
    MinFilter = LINEAR;
    MagFilter = LINEAR;
    MipFilter = NONE;
};

// Contact shadows beside indirect light in the passes that take both (GIContact*_PS): this frame's, read as
// SSRResultTex is, and last frame's accumulation, as SSRAccumTex is.
sampler2D ContactInTex
{
    Texture = <ContactInTex2D>;
};

sampler2D ContactHistTex
{
    Texture = <ContactHistTex2D>;
    AddressU = Clamp;
    AddressV = Clamp;
    MinFilter = LINEAR;
    MagFilter = LINEAR;
    MipFilter = NONE;
};

// SSRFallback_PS's guess where SSR_PS fills in, the same size as SSRResultTex.
sampler2D SSRFallbackTex
{
    Texture = <SSRFallbackTex2D>;
    AddressU = Clamp;
    AddressV = Clamp;
    MinFilter = POINT;
    MagFilter = POINT;
    MipFilter = NONE;
};

// R: how far SSRTrace_PS's ray went to its hit, over fMaxDistance (0 for a miss).
// G: the reflected hit's own screen-space motion, in full-size pixels. A is
// negative for an untraced surface. Same size as SSRResultTex.
sampler2D SSRHitDistTex
{
    Texture = <SSRHitDistTex2D>;
    AddressU = Clamp;
    AddressV = Clamp;
    MinFilter = POINT;
    MagFilter = POINT;
    MipFilter = NONE;
};

// View depth at half the full size, the nearest of each 2x2 pixels (MarchDepth_PS), which
// SSRTrace_PS's march steps through; see TraceHit.
sampler2D MarchDepthTex
{
    Texture = <MarchDepthTex2D>;
    AddressU = Clamp;
    AddressV = Clamp;
    MinFilter = POINT;
    MagFilter = POINT;
    MipFilter = NONE;
};

sampler2D HiZDepthTex
{
    Texture = <HiZDepthTex2D>;
    AddressU = Clamp;
    AddressV = Clamp;
    MinFilter = POINT;
    MagFilter = POINT;
    MipFilter = NONE;
};

// The next level of the same pyramid, its cells twice as wide. A ray whose steps are already
// wider than a level's cells cannot skip anything with it, so long rays take this one; see
// TraceHit. Both are exact 2x2 minima of the level before, so either stays conservative.
sampler2D HiZDepthTex1
{
    Texture = <HiZDepthTex2D1>;
    AddressU = Clamp;
    AddressV = Clamp;
    MinFilter = POINT;
    MagFilter = POINT;
    MipFilter = NONE;
};

sampler2D DebugTex
{
    Texture = <DebugTex2D>;
};

// Temporal AA's motion vectors, see TemporalHistoryUV.
sampler2D MotionTex
{
    Texture = <MotionTex2D>;
    AddressU = Clamp;
    AddressV = Clamp;
    MinFilter = POINT;
    MagFilter = POINT;
    MipFilter = NONE;
};

// Last frame's accumulated SSR, see SSRTemporal_PS.
sampler2D SSRAccumTex
{
    Texture = <SSRAccumTex2D>;
    AddressU = Clamp;
    AddressV = Clamp;
    MinFilter = LINEAR;
    MagFilter = LINEAR;
    MipFilter = NONE;
};

// This frame's diffuse colour (_DEFERRED_GBUFFER_0_), and the indirect light deferred_lighting
// added last frame, see SSGI_PS.
sampler2D AlbedoTex
{
    Texture = <AlbedoTex2D>;
    AddressU = Clamp;
    AddressV = Clamp;
    MinFilter = POINT;
    MagFilter = POINT;
    MipFilter = NONE;
};
// The same, filtered, for GIHitBox. Its own parameter: BindEffectSamplers finds a sampler's texture
// by the sampler's name, and one sharing AlbedoTex2D was left with whatever the game had bound.
sampler2D AlbedoLinearTex
{
    Texture = <AlbedoLinearTex2D>;
    AddressU = Clamp;
    AddressV = Clamp;
    MinFilter = LINEAR;
    MagFilter = LINEAR;
    MipFilter = NONE;
};

sampler2D GIPrevTex
{
    Texture = <GIPrevTex2D>;
    AddressU = Clamp;
    AddressV = Clamp;
    MinFilter = LINEAR;
    MagFilter = LINEAR;
    MipFilter = NONE;
};

// Light scattering under the skin, see SkinScatter_PS: the lit scene before fog, the material
// IDs (_STENCIL_BUFFER_), and the light on skin being blurred.
sampler2D SceneTex
{
    Texture = <SceneTex2D>;
    AddressU = Clamp;
    AddressV = Clamp;
    MinFilter = POINT;
    MagFilter = POINT;
    MipFilter = NONE;
};

sampler2D SkinIDTex
{
    Texture = <SkinIDTex2D>;
    AddressU = Clamp;
    AddressV = Clamp;
    MinFilter = POINT;
    MagFilter = POINT;
    MipFilter = NONE;
};

sampler2D SkinLightTex
{
    Texture = <SkinLightTex2D>;
    AddressU = Clamp;
    AddressV = Clamp;
    MinFilter = POINT;
    MagFilter = POINT;
    MipFilter = NONE;
};

// The scene right before and right after the game draws water; they differ only where
// water was actually drawn.
sampler2D PreWaterTex
{
    Texture = <PreWaterTex2D>;
    AddressU = Clamp;
    AddressV = Clamp;
    MinFilter = POINT;
    MagFilter = POINT;
    MipFilter = NONE;
};

sampler2D PostWaterTex
{
    Texture = <PostWaterTex2D>;
    AddressU = Clamp;
    AddressV = Clamp;
    MinFilter = POINT;
    MagFilter = POINT;
    MipFilter = NONE;
};

sampler2D SurfaceTex
{
    Texture = <SurfaceTex2D>;
    AddressU = Wrap;
    AddressV = Wrap;
    MinFilter = LINEAR;
    MagFilter = LINEAR;
};

uniform float2 vec2InvViewportSize;
uniform float2 vec2InvHistorySize; // full-size history texel, independent of SSR resolution
uniform float fNearPlane;
uniform float fFarDivNear;
uniform float4 vec4ProjInfo;

uniform float4 vec4ViewToPrevClip[4];
// Temporal AA's motion vectors are bound (MotionTex): previous - current position in texture coordinates, without
// the jitter, which vec2MotionJitter (previous - current) adds back for the jittered histories.
uniform float fUseMotion;
uniform float2 vec2MotionJitter;
uniform float fUsePrevDepth; // 1 when PrevDepthTex holds the depth HistoryTex was taken with
uniform float2 vec2PrevDepthRange; // fNearPlane and fFarDivNear of the scene PrevDepthTex was drawn in, see PrevLinearDepth

uniform float fMaxDistance;     // world units to march before giving up
uniform float fThickness;       // how deep behind a surface still counts as a hit
uniform float fEdgeFade;        // 0..0.5, screen fraction over which to fade at borders
uniform float fIntensity;       // final multiplier on confidence
uniform float fGlossBoost;      // extra intensity on shiny materials, 0 disables
uniform float fGlossCutoff;     // gloss below this is matte and reflects nothing
uniform float fWetness;         // 0 dry to 1 raining, from the game's rain amount; 0 while wet ground is off
uniform float fWetGroundBoost;  // how many times brighter reflections on wet ground are drawn
uniform float fWaterIntensity;  // final multiplier for the water pass
uniform float4 vec4WaterPlane;  // water plane in reconstruction space, (normal.xyz, d)
uniform float fWaterBlur;       // reflection blur radius in pixels at max ray distance
uniform float fWaterNormalStrength; // ripple slope multiplier, 0 gives a flat mirror
uniform float4 vec4WaterRings;      // the rain's rings: strength, seconds, fade with distance (scale, offset); 0 none
uniform float fUseWaterMask;        // 1 limits the water pass to pixels the game drew water on

uniform float4 vec4WaterToView[3];
uniform float4 vec4WaterWorldX;
uniform float4 vec4WaterWorldY;

uniform float fDebugMode; // SSR debug view from the graphics menu, see SSRDebug_PS
uniform float fUseGBufferNormals; // 1 reads the G-buffer normal, 0 rebuilds it from depth
uniform float fDenoiseRadius;     // SSR smoothing radius in pixels, see SSRDenoise_PS
uniform float fDenoiseSSROnly;    // 1 while smoothing SSR, 0 while smoothing contact shadows
uniform float fDenoiseHitDistance; // hit-distance MRT is valid for SSR's spatial filter
uniform float fPassThinObjects;   // 1 lets a ray that went far behind an object carry on
uniform float fStepsPerPixel;     // march steps per pixel of a ray on this pass's target, 1 / SSRStepPixels
uniform float fUseHiZ;            // depth hierarchy levels built this frame, 0 without them
uniform float fStochasticSSR;
uniform float2 vec2HiZSize;       // cells across the viewport of the finer level, the grid TraceHit walks
uniform float2 vec2HiZSize1;      // the coarser level's, half as many each way
uniform float fMarchFullDepth;    // 1 marches through the full size depth instead of MarchDepthTex (SSRMarchFullDepth, temporary, to compare)
uniform float fStepJitter;        // 1 shifts each pixel's steps by up to one step (set per pass: SSR and contact shadows each have their own switch)
uniform float2 vec2NoiseOffset;   // pixels, moves where PixelJitter is read every frame while the passes accumulate (FrameHistory::NoiseOffset), 0 otherwise
uniform float fTowardCamera;      // 0..1, how far reflections pointing back at the camera reach
uniform float fReflectionBlur;    // blur radius in pixels a reflection reaches at fMaxDistance, 0 keeps it sharp
uniform float fDistanceFade;      // reflections fade out towards this distance from the surface, 0 disables
uniform float fTemporalBlend;     // share of last frame's SSR kept each frame, 0 while there is none to keep
uniform float fTemporalAnySurface; // 1 while accumulating indirect light, which every surface gets, not only glossy ones
uniform float fFallback;          // 0..1, strength of the fill from past what hid the ray (ScreenFallback), see SSRFallback_PS
uniform float fNeighbourFill;     // 0..1, strength of the fill from the hits around a miss (NeighbourFill, SSRSpread_PS)
uniform float fFillMinDistance;   // over fMaxDistance: hits nearer than this along their ray fill no misses (0: all do)
uniform float fVehicleScale = 1.0; // SSRVehicleUnits / (10 x the game's c39.w) on car bodies, SkinIDTex their material IDs
uniform float fFillPixelScale;     // full-size pixels to this pass's pixels
uniform float fSpreadRadius;      // pixels, how far SSRSpread_PS looks this step
uniform float fContactBlend;      // GIContactTemporal_PS: share of last frame's contact shadows kept, 0 while there is none

// Contact shadows, see ContactShadows_PS.
uniform float4 vec4SunView;         // direction towards the sun in reconstruction space, w 0 if unknown
uniform float fCSLength;            // world units a contact shadow ray travels
uniform float fCSThickness;         // how deep behind the scene a sample may land and still occlude
uniform float fCSMaxViewDistance;   // contact shadows fade out towards this view distance
uniform float fCSIntensity;         // strength, 0..1
uniform float fCSGrassGloss;        // GPU grass's gloss in 1023ths (GpuGrass.fx), 0 = none: no contact shadows on it

// Screen space indirect light, see SSGI_PS.
uniform float fGIRayLength;         // world units an indirect light ray travels
uniform float fGIThickness;         // how deep behind the scene a sample may land and still be a hit
uniform float fGIMaxViewDistance;   // indirect light fades out towards this view distance
uniform float fGIIntensity;         // multiplier on the light gathered
uniform float fGIMaxBrightness;     // brightness a single hit may bring, so a headlight or neon sign does not flare
uniform float fGIFeedback;          // share of last frame's indirect light a hit takes back out, 0 while there is none
uniform float fGIOcclusion;         // 0..1, how much of the ambient the indirect light takes the place of where its rays hit
uniform float fGIRespectAO;         // 1 when SpecularTex holds the occlusion deferred_lighting multiplies the ambient by, see SSGI_PS

// Light scattering under the skin, see SkinScatter_PS.
uniform float4 vec4SkinStep;        // xy: screen offset of one kernel unit at view depth 1, along this pass; w: metres in one kernel unit
uniform float fSkinStrength;        // 0..2, the share of light that scatters, times the skin profile's

#ifndef GI_RAYS
#define GI_RAYS 4
#endif
#ifndef GI_STEPS
#define GI_STEPS 8
#endif

#ifndef CS_STEPS
#define CS_STEPS 16
#endif

static const float HISTORY_CLAMP = 8.0;
static const float SSR_SCALE = 1.0;

float3 SampleHistory(float2 uv)
{
    return clamp(tex2D(HistoryTex, uv).rgb, 0.0, HISTORY_CLAMP) * SSR_SCALE;
}

float3 SampleHistoryBlurred(float2 uv, float radiusPixels)
{
    float3 c = SampleHistory(uv);

    if (radiusPixels <= 0.0)
        return c;

    // HistoryTex is full size even when SSR is traced at half resolution.
    // The blur radius is in history pixels, not pixels of the current pass.
    float2 r = radiusPixels * vec2InvHistorySize;

    c += SampleHistory(uv + r);
    c += SampleHistory(uv - r);
    c += SampleHistory(uv + float2(r.x, -r.y));
    c += SampleHistory(uv + float2(-r.x, r.y));

    return c * 0.2;
}

#ifndef NUM_STEPS
#define NUM_STEPS 48
#endif
#ifndef NUM_REFINE_STEPS
#define NUM_REFINE_STEPS 8
#endif

float LinearDepth(float2 uv)
{
    return pow(fFarDivNear, tex2Dlod(DepthTex, float4(uv, 0, 0)).r) * fNearPlane;
}

// Last frame's view depth at uv. Its log depth goes back to view depth with last frame's near and
// far planes: the game moves the near plane as the camera closes in on a wall, and decoded with this
// frame's, the depth of every surface differed from last frame's, which dropped every history.
float PrevLinearDepth(float2 uv)
{
    return pow(vec2PrevDepthRange.y, tex2Dlod(PrevDepthTex, float4(uv, 0, 0)).r) * vec2PrevDepthRange.x;
}

float3 ReconstructViewPos(float2 S, float z)
{
    return float3(((S.xy + 0.5f) * vec4ProjInfo.xy + vec4ProjInfo.zw) * z, z);
}

float2 ViewToUV(float3 P)
{
    float2 S = ((P.xy / P.z) - vec4ProjInfo.zw) / vec4ProjInfo.xy;
    return S * vec2InvViewportSize;
}

float2 HistoryUV(float3 P)
{
    float4 clip = P.x * vec4ViewToPrevClip[0]
                + P.y * vec4ViewToPrevClip[1]
                + P.z * vec4ViewToPrevClip[2]
                +       vec4ViewToPrevClip[3];

    if (clip.w <= 0.0)
        return float2(-1.0, -1.0);

    return (clip.xy / clip.w) * float2(0.5, -0.5) + 0.5;
}

// Where the surface at uv was last frame, for the accumulations. The camera alone takes it to HistoryUV; with
// temporal AA's motion vectors, peds, vehicles and objects that moved themselves are followed too. Last frame's depth
// can only confirm surfaces that moved with the camera (checkDepth), a moving one was elsewhere and its neighbourhood
// clamp has to do.
float2 TemporalHistoryUVWithMotion(float2 uv, float3 C, out bool checkDepth, out float2 objectMotion)
{
    float2 prevUV = HistoryUV(C);
    checkDepth = true;
    objectMotion = 0.0;
    [branch]
    if (fUseMotion > 0.0)
    {
        float2 moved = uv + tex2Dlod(MotionTex, float4(uv, 0, 0)).xy + vec2MotionJitter;
        objectMotion = moved - prevUV;
        // More than a pixel and a half from where the camera alone takes it
        if (any(abs(moved - prevUV) > 1.5 * vec2InvViewportSize))
            checkDepth = false;
        prevUV = moved;
    }
    return prevUV;
}

float2 TemporalHistoryUV(float2 uv, float3 C, out bool checkDepth)
{
    float2 unusedMotion;
    return TemporalHistoryUVWithMotion(uv, C, checkDepth, unusedMotion);
}

float3 ViewPosFromUVZ(float2 uv, float z)
{
    return ReconstructViewPos(uv / vec2InvViewportSize - 0.5f, z);
}

float3 ViewPosAtUV(float2 uv)
{
    return ViewPosFromUVZ(uv, LinearDepth(uv));
}

float3 ReconstructNormal(float2 uv, float3 C)
{
    float2 dx = float2(vec2InvViewportSize.x, 0.0);
    float2 dy = float2(0.0, vec2InvViewportSize.y);

    float3 l = ViewPosAtUV(uv - dx);
    float3 r = ViewPosAtUV(uv + dx);
    float3 d = ViewPosAtUV(uv - dy);
    float3 u = ViewPosAtUV(uv + dy);

    float3 dpdx = (abs(l.z - C.z) < abs(r.z - C.z)) ? (C - l) : (r - C);
    float3 dpdy = (abs(d.z - C.z) < abs(u.z - C.z)) ? (C - d) : (u - C);

    return normalize(cross(dpdy, dpdx));
}

// _DEFERRED_GBUFFER_1_ decoded the way deferred_lighting decodes it, in world space.
float3 GBufferNormalWorld(float2 uv)
{
    float4 g = tex2Dlod(NormalTex, float4(uv, 0, 0));
    float3 f = frac(g.w * float3(0.998046875, 7.984375, 63.875));
    f.xy -= f.yz * 0.125;
    return normalize(g.xyz * 256.0 + f - 127.999992);
}

// The same normal in reconstruction space. Rebuilding normals from depth does not work here:
// depth precision makes neighbouring pixels of a car panel read the same depth, so the normal
// faces the camera and the reflected ray heads straight back, and roads turn into noise.
float3 GBufferNormal(float2 uv)
{
    float3 n = GBufferNormalWorld(uv);
    return normalize(float3(dot(vec4WaterToView[0].xyz, n),
                            dot(vec4WaterToView[1].xyz, n),
                            dot(vec4WaterToView[2].xyz, n)));
}

// Interleaved gradient noise in (0, 1], fixed per pixel so it does not flicker between frames.
float PixelJitter(float2 pixel)
{
    return 1.0 - frac(52.9829189 * frac(dot(pixel, float2(0.06711056, 0.00583715))));
}

// For debug view 3: the view depth of the last TraceHit's hit, and its ray's view
// depth direction (negative towards the camera).
static float gTraceHitZ = 0.0;
static float gTraceRayZ = 0.0;
static float gTraceHitMotion = 0.0; // reflected hit's own motion, in full-size pixels
// Share of the last TraceHit's steps that a depth hierarchy jump took away, for debug view 12:
// whether the hierarchy pays for its pass and its boundary arithmetic in this scene.
static float gTraceSkipShare = 0.0;
// Which of its levels the last TraceHit walked, for the same view: a level nothing chose would
// cost its pass and change nothing, which the skipped steps alone would not show.
static float gTraceHiZLevel = 0.0;
// The screen path of the last SetupReflectionRay's ray, for ScreenFallback: from gTraceUV0 to
// gTraceUVEnd, where it leaves the screen; gTracePath is 0 when the ray was not marched.
static float2 gTraceUV0 = 0.0;
static float2 gTraceUVEnd = 0.0;
static float gTracePath = 0.0;

// The reflected ray of the surface at C with normal n, on the screen: where it starts (P0, uv0)
// and where it would end (uv1) at fMaxDistance, its inverse view depths for perspective
// correct stepping, the share of it before it leaves the screen (tEnd) and how much rays
// pointing back at the camera keep (facing). False where it is not traced. Also sets the
// gTrace globals, so SSR_PS can follow the same path as SSRTrace_PS without tracing again.
struct ReflectionRay
{
    float3 P0;
    float2 uv0, uv1;
    float invZ0, invZ1;
    float tEnd;
    float facing;
    float len; // from P0 to the far end, uv1, in view space
};

// Fraction of a projected segment that remains on screen. A nearly stationary
// axis must not acquire a positive denominator when it actually points left/up.
float SSRScreenRayEnd(float2 uv, float2 direction)
{
    float tx = 1.0, ty = 1.0;
    if (abs(direction.x) > 1e-8)
        tx = ((direction.x > 0.0 ? 1.0 : 0.0) - uv.x) / direction.x;
    if (abs(direction.y) > 1e-8)
        ty = ((direction.y > 0.0 ? 1.0 : 0.0) - uv.y) / direction.y;
    return clamp(min(tx, ty), 0.0, 1.0);
}

bool SetupReflectionRay(float3 C, float3 n, float3 microNormal, out ReflectionRay ray)
{
    ray = (ReflectionRay) 0;
    gTracePath = 0.0;
    float z = C.z;
    float3 V = normalize(C);
    float3 R = reflect(V, microNormal);
    gTraceRayZ = R.z;
    // A microfacet can reflect below the geometric surface; it cannot see the scene.
    if (dot(R, n) <= 0.0)
        return false;

    // Rays reflected back towards the camera see the side of things the screen does not show:
    // a door with a ped crouching between it and the camera should show his front, the screen
    // holds his back, and the hit jumps between his back, his outline and what is behind him.
    // At fTowardCamera 0 they fade from a cosine of 0.25 between view and reflection and are
    // gone at -0.25, which keeps the ped out of the door; at 1 they keep full weight down to
    // -0.5 and fade by -0.8, which lets a roof seen steeply from above (about -0.5) reflect.
    float cosVR = dot(V, R);
    ray.facing = lerp(saturate(cosVR * 2.0 + 0.5), saturate((cosVR + 0.8) / 0.3), fTowardCamera);
    if (ray.facing <= 0.0)
        return false;

    ray.P0 = C + n * max(fMaxDistance / (float) NUM_STEPS * 0.1, z * 0.01);

    // With the camera looking down at a roof or bonnet more steeply than about 45 degrees,
    // the reflected ray heads up the screen towards what stands behind the car, but its view
    // depth shrinks. Those rays are traced too, stopping in front of the near plane so both
    // ends stay projectable; dropping them made reflections vanish as the camera tilted down.
    float len = fMaxDistance;
    if (R.z < 0.0)
        len = min(len, (ray.P0.z - fNearPlane * 2.0) / -R.z);
    if (len <= 0.0)
        return false;
    float3 P1 = ray.P0 + R * len;
    ray.len = len;

    ray.uv0 = ViewToUV(ray.P0);
    ray.uv1 = ViewToUV(P1);
    ray.invZ0 = 1.0 / ray.P0.z;
    ray.invZ1 = 1.0 / P1.z;

    float2 dUV = ray.uv1 - ray.uv0;
    ray.tEnd = SSRScreenRayEnd(ray.uv0, dUV);
    gTraceUV0 = ray.uv0;
    gTraceUVEnd = lerp(ray.uv0, ray.uv1, ray.tEnd);
    gTracePath = 1.0;
    return true;
}

// The view depth the march compares its ray with at uv, see TraceHit.
float MarchDepth(float2 uv)
{
    return fMarchFullDepth > 0.0 ? LinearDepth(uv) : tex2Dlod(MarchDepthTex, float4(uv, 0, 0)).r;
}

// Steps a ray of this many pixels of the pass's target gets: about one per fStepsPerPixel, capped
// at NUM_STEPS, and floored at four rather than at the ray's own length only below that. A ray of
// a far car spans a handful of pixels; twelve steps there sampled the same two pixels six times
// over and paid for six iterations that could not find anything one sample would not. Four keeps
// a sub-pixel ray able to hit the pixel it lives in at all, which is what the old floor was for.
float SSRStepCount(float rayPixels)
{
    return clamp(ceil(rayPixels * fStepsPerPixel), min(max(rayPixels, 4.0), 12.0), (float) NUM_STEPS);
}

// Marches the reflected ray of the surface at C with normal n. The hit is (where to read it in
// HistoryTex, confidence 0..1, the ray's length over fMaxDistance), 0 for a miss; HitColour
// turns it into a colour.
// jitter in (0, 1] shifts every step of this pixel's ray by up to one step.
// distanceFade: reflections fade out towards this distance from the surface, 0 disables.
float4 TraceHit(float3 C, float3 n, float3 microNormal, float jitter, float distanceFade)
{
    gTraceHitMotion = 0.0;
    gTraceSkipShare = 0.0;
    gTraceHiZLevel = 0.0;
    ReflectionRay ray;
    if (!SetupReflectionRay(C, n, microNormal, ray))
        return 0.0;
    float facing = ray.facing;
    float3 P0 = ray.P0;
    float2 uv0 = ray.uv0;
    float2 uv1 = ray.uv1;
    float invZ0 = ray.invZ0;
    float invZ1 = ray.invZ1;
    float tEnd = ray.tEnd;
    float2 dUV = uv1 - uv0;

    // A ray short on screen needs fewer steps: a car far away reflects over a few dozen pixels,
    // and all NUM_STEPS there sampled each pixel several times. About one step per two pixels
    // of the ray (fStepsPerPixel 0.5) keeps the last and longest step, twice the average, within
    // a few pixels; with the steps moving every frame the accumulation makes up for fewer.
    float rayPixels = length(dUV * tEnd / vec2InvViewportSize);
    float steps = SSRStepCount(rayPixels);

    // Steps grow with the square of their index: a few centimetres next to the surface, where
    // a ped standing by a car is, about twice the even spacing at the far end. Even steps a
    // metre apart stepped over a leg next to the bonnet, so only some pixels caught it and its
    // reflection came out as several shifted slices.
    // Each pixel starts its steps up to one step later (jitter), so neighbouring rows do not
    // all catch or all miss a thin object such as a tree trunk in step with each other; that
    // showed as regular bands, the offset leaves fine noise that the smoothing pass removes.
    float hit = 0.0;
    float hitLo = 0.0;
    float hitHi = 0.0;
    float prevT = 0.0;
    float prevDelta = -1.0;
    // How far the ray travelled hidden behind what the screen shows, and how far it had come
    // when it first went far behind something (-1 if it never did), as shares of ray.len: the
    // view space point at t is P0 + (P1 - P0) * t * invZ1 * rayZ, so its distance from P0 takes
    // two multiplications instead of building the point and its length every step.
    float hidden = 0.0;
    float firstHidden = -1.0;
    float prevU = 0.0;
    // Steps a depth hierarchy jump took away, for gTraceSkipShare.
    float skipped = 0.0;
    // Rough paint is not a mirror: its lobe sees the surface from a spread of directions, so a
    // hit a little behind or in front of the depth the buffer holds is still a hit of the lobe,
    // and the bracket that counts as one widens with roughness. Mirror paint keeps fThickness
    // exactly, and without a specular target there is no gloss to widen it by.
    float gloss = fGlossCutoff >= 0.0 ? tex2Dlod(SpecularTex, float4(uv0, 0, 0)).y : 1.0;
    float hitSlab = fThickness * (1.0 + 1.5 * (1.0 - gloss));

    // March and refinement share one bounded loop. A conservative 2x2 depth can report
    // the silhouette of a neighbouring pixel; reject that candidate in full-size depth
    // and resume the ray instead of turning the entire ray into a miss. Allow one retry.
    // There is no nested refinement loop: D3DX compiles this effect at startup.
    // The march reads MarchDepthTex, the nearest depth of each 2x2 pixels, already linear: its
    // reads fall four times as close together in the texture cache, which is most of what a
    // step costs, and need no decoding. Nearest, so a ray does not slip past a thin pole between
    // two of its samples; the refinement below finds the hit in the full size depth.
    int marchIndex = 0;
    int refineLeft = -1;
    int candidates = 0;
    float candidateEnd = 0.0;
    float acceptedSurfaceZ = 0.0;
    bool verifyCoarseDepth = false;
    float rayPixelsPerT = max(abs(dUV.x / vec2InvViewportSize.x), abs(dUV.y / vec2InvViewportSize.y));
    // A jump only pays when a cell holds several steps. cellSteps is how many of the finer
    // level's cells the ray crosses on its longer axis, so steps / cellSteps is what one cell
    // holds; below two, the ray's steps are wider than half a cell and the coarser level, whose
    // cells are twice as wide, is the one that can skip. Both levels are minima of the same
    // march depth, so either is conservative and the hit does not move. Counting cells rather
    // than pixels keeps this the same at either SSR size: the grid is the viewport's, while the
    // steps are the pass's, and at half size a ray takes half as many of them.
    float cellSteps = max(abs(dUV.x) * vec2HiZSize.x, abs(dUV.y) * vec2HiZSize.y);
    float hiZLevel = (fUseHiZ > 1.0 && cellSteps * 2.0 > steps) ? 1.0 : 0.0;
    float2 hiZSize = hiZLevel > 0.0 ? vec2HiZSize1 : vec2HiZSize;
    gTraceHiZLevel = hiZLevel;
    [loop] [fastopt]
    for (int iteration = 0; iteration < NUM_STEPS + 2 * (NUM_REFINE_STEPS + 1); ++iteration)
    {
        [branch]
        if (refineLeft >= 0)
        {
            // Stop only when the bracket is small both on screen and in depth. A ray
            // nearly along the view axis can cover metres inside a fraction of a pixel.
            bool narrow = (hitHi - hitLo) * rayPixelsPerT <= 0.125;
            if (narrow)
                narrow = abs(1.0 / lerp(invZ0, invZ1, hitHi) - 1.0 / lerp(invZ0, invZ1, hitLo)) <= 0.25 * hitSlab;
            if (refineLeft > 0 && !narrow)
            {
                float mid = (hitLo + hitHi) * 0.5;
                float midZ = 1.0 / lerp(invZ0, invZ1, mid);
                if (midZ > LinearDepth(lerp(uv0, uv1, mid)))
                    hitHi = mid;
                else
                    hitLo = mid;
                --refineLeft;
                continue;
            }
            float candidateZ = 1.0 / lerp(invZ0, invZ1, hitHi);
            float surfaceZ = LinearDepth(lerp(uv0, uv1, hitHi));
            float thickness = abs(candidateZ - 1.0 / lerp(invZ0, invZ1, hitLo)) + hitSlab;
            if (abs(candidateZ - surfaceZ) < thickness)
            {
                hit = 1.0;
                acceptedSurfaceZ = surfaceZ;
                break;
            }
            if (candidates >= 2)
                break;

            // Resume after the rejected interval. Full-size depth, not the dilated
            // coarse silhouette, determines whether this point is actually hidden.
            float endZ = 1.0 / lerp(invZ0, invZ1, candidateEnd);
            float endDelta = endZ - LinearDepth(lerp(uv0, uv1, candidateEnd));
            float endU = candidateEnd * invZ1 * endZ;
            if (endDelta > 0.0)
            {
                if (firstHidden < 0.0)
                    firstHidden = prevU;
                if (fPassThinObjects <= 0.0)
                    break;
                hidden += endU - prevU;
            }
            prevT = candidateEnd;
            prevU = endU;
            prevDelta = endDelta;
            verifyCoarseDepth = endDelta <= 0.0;
            refineLeft = -1;
            continue;
        }
        if ((float) marchIndex >= steps || marchIndex >= NUM_STEPS)
            break;
        int i = marchIndex++;
        float s = ((float) i + jitter) / steps;
        float t = tEnd * s * s;

        float2 sampleUV = lerp(uv0, uv1, t);
        float rayZ = 1.0 / lerp(invZ0, invZ1, t);

        // Jump to the last original sample before this cell's exit. The minimum
        // depth proves the whole segment clear; sampling the endpoint preserves the
        // original crossing interpolation when the next sample enters another cell.
        [branch]
        if (fUseHiZ > 0.0 && prevDelta <= 0.0 && tEnd > 1e-6)
        {
            float2 cell = floor(sampleUV * hiZSize);
            float boundaryX = (cell.x + (dUV.x > 0.0 ? 1.0 : 0.0)) / hiZSize.x;
            float boundaryY = (cell.y + (dUV.y > 0.0 ? 1.0 : 0.0)) / hiZSize.y;
            float tx = abs(dUV.x) > 1e-6 ? (boundaryX - uv0.x) / dUV.x : tEnd;
            float ty = abs(dUV.y) > 1e-6 ? (boundaryY - uv0.y) / dUV.y : tEnd;
            float exitT = min(tEnd, min(tx, ty));
            // Steps use t = tEnd * ((i + jitter) / steps)^2. Stay strictly
            // before the boundary so a point-filtered read cannot enter the next cell.
            float endIndex = min(steps - 1.0, min((float)(NUM_STEPS - 1),
                floor(steps * sqrt(saturate(exitT / tEnd)) - jitter - 1e-3)));
            if (endIndex >= (float)(i + 2))
            {
                float endS = (endIndex + jitter) / steps;
                float endT = tEnd * endS * endS;
                float2 endUV = lerp(uv0, uv1, endT);
                // ps_3_0 has no dynamic sampler index, so the level this ray chose is a branch;
                // every iteration of the ray takes the same side.
                float nearest;
                [branch]
                if (hiZLevel > 0.0)
                    nearest = tex2Dlod(HiZDepthTex1, float4(sampleUV, 0, 0)).r;
                else
                    nearest = tex2Dlod(HiZDepthTex, float4(sampleUV, 0, 0)).r;
                float endZ = 1.0 / lerp(invZ0, invZ1, endT);
                if (max(rayZ, endZ) < nearest)
                {
                    prevT = endT;
                    prevU = endT * invZ1 * endZ;
                    prevDelta = endZ - MarchDepth(endUV);
                    skipped += endIndex - (float)i;
                    marchIndex = (int)endIndex + 1;
                    continue;
                }
            }
        }
        float delta = rayZ - MarchDepth(sampleUV);
        // Do not immediately rediscover the same rejected coarse silhouette. Until
        // the ray leaves it, use full depth to find the actual surface behind it.
        if (verifyCoarseDepth)
        {
            if (delta > 0.0)
                delta = rayZ - LinearDepth(sampleUV);
            else
                verifyCoarseDepth = false;
        }

        // The ray went behind the scene since the last sample, which was in front of it.
        // Estimate where it crossed from the two samples and judge the thickness there, not
        // at this sample, where it depended on where the step happened to land. Far behind
        // means the ray passed behind a thin object standing in front of what it was
        // crossing, such as a trunk in front of a wall: it carries on and may still hit the
        // wall, instead of ending as a miss.
        if (delta > 0.0 && prevDelta <= 0.0)
        {
            float tc = lerp(prevT, t, saturate(-prevDelta / max(delta - prevDelta, 1e-5)));
            float zc = 1.0 / lerp(invZ0, invZ1, tc);
            float2 crossUV = lerp(uv0, uv1, tc);
            float crossDelta = zc - (verifyCoarseDepth ? LinearDepth(crossUV) : MarchDepth(crossUV));
            float crossThickness = abs(rayZ - 1.0 / lerp(invZ0, invZ1, prevT)) + hitSlab;
            if (crossDelta <= crossThickness)
            {
                hitLo = prevT;
                hitHi = t;
                candidateEnd = t;
                refineLeft = NUM_REFINE_STEPS;
                ++candidates;
                continue;
            }
            if (firstHidden < 0.0)
                firstHidden = prevU;
            if (fPassThinObjects <= 0.0)
                break;
        }

        float u = t * invZ1 * rayZ;
        if (delta > 0.0)
            hidden += u - prevU;
        prevU = u;
        prevT = t;
        prevDelta = delta;
    }

    // skipped cannot pass steps: the jumps only move marchIndex forward, and it stays under it.
    gTraceSkipShare = skipped / steps;
    if (hit <= 0.0)
        return 0.0;
    hidden *= ray.len;
    firstHidden *= ray.len;

    // The shared loop already validated this full-size depth bracket.
    float lo = hitLo;
    float hi = hitHi;

    float2 finalUV = lerp(uv0, uv1, hi);
    float hitZ = 1.0 / lerp(invZ0, invZ1, hi);
    float surfZ = acceptedSurfaceZ;
    // Behind the surface, as the refinement leaves a ray that crossed it, or in front of it: the
    // march's nearest of four pixels can show a crossing next to a pole's outline that the full
    // size depth does not have, and the ray there runs on in front of what lies beyond.
    float hitDelta = abs(hitZ - surfZ);
    float hitThickness = abs(hitZ - 1.0 / lerp(invZ0, invZ1, lo)) + hitSlab;
    float3 hitP = ViewPosFromUVZ(finalUV, hitZ);
    gTraceHitZ = hitP.z;

    // The surface the depth buffer holds where the ray hit; hitP is up to a thickness off it.
    float3 surfP = ViewPosFromUVZ(finalUV, surfZ);
    // Motion vectors carry the hit to its previous pixel, but do not carry its previous Z.
    // Use the same camera-only depth validation policy as the accumulation passes.
    bool checkHitDepth;
    float2 hitMotion;
    float2 histUV = TemporalHistoryUVWithMotion(finalUV, surfP, checkHitDepth, hitMotion);
    float2 hitMotionPixels = abs(hitMotion / vec2InvHistorySize);
    gTraceHitMotion = min(max(hitMotionPixels.x, hitMotionPixels.y), 255.0);

    float2 edge = saturate(min(min(finalUV, histUV), 1.0 - max(finalUV, histUV)) / max(fEdgeFade, 1e-4));
    float e = min(edge.x, edge.y);
    float confidence = e * e * (3.0 - 2.0 * e);

    float rayLen = length(hitP - C);

    confidence *= facing;
    confidence *= saturate((1.0 - rayLen / fMaxDistance) * 4.0);
    // Car paint is no perfect mirror: it shows what stands next to it and barely what stands
    // metres away, such as a ped between the camera and a door at night.
    if (distanceFade > 0.0)
        confidence *= 1.0 - smoothstep(distanceFade * 0.5, distanceFade, rayLen);
    confidence *= 1.0 - smoothstep(hitThickness * 0.75, hitThickness, hitDelta);
    // Behind an object the screen holds nothing, so a ray passing there may have run into
    // something it does not show. Behind a trunk that is a few dozen centimetres; behind a ped
    // standing metres in front of a door, with the camera turned so the door shows next to his
    // shoulder, rays from the door ran a metre or more behind him and took the colour of the
    // pavement beyond: a bright strip on the door beside him. The longer the hidden stretch,
    // the less a hit after it counts. That is only for rays that went out of view right off
    // the surface: one that crossed half a metre of the scene first and then passed behind
    // the player to the road beyond hit the same road as the rays beside his figure, and
    // fading it left the figure darker on the car.
    if (firstHidden >= 0.0 && firstHidden < 0.5)
        confidence *= 1.0 - smoothstep(0.4, 0.8, hidden);

    // The colour comes from the history, the hit from this frame's depth. Next to an outline
    // the history pixel can belong to what is in front or behind: a white roof behind a ped's
    // legs gave his reflection a bright rim, and a ped a few metres in front of the road a car
    // door reflects, moved by a pixel or two since, put his dark figure into the door. The
    // depth copied with the history tells them apart. It is compared at the surface the depth
    // buffer holds, with the temporal passes' tolerance: the ray's thickness grows with its
    // step to metres far out and let the ped through. clip.w is the view depth in the
    // history's camera.
    [branch]
    if (fUsePrevDepth > 0.0 && checkHitDepth)
    {
        float prevSurfZ = dot(float4(surfP, 1.0), float4(vec4ViewToPrevClip[0].w, vec4ViewToPrevClip[1].w,
                                                         vec4ViewToPrevClip[2].w, vec4ViewToPrevClip[3].w));
        float2 depthUV = (floor(histUV / vec2InvHistorySize) + 0.5) * vec2InvHistorySize;
        float prevZ = PrevLinearDepth(depthUV);
        float tolerance = 0.05 * prevSurfZ + 0.1;
        confidence *= 1.0 - smoothstep(tolerance * 0.5, tolerance, abs(prevZ - prevSurfZ));
    }

    return float4(histUV, confidence, saturate(rayLen / fMaxDistance));
}

// Explicit LOD keeps the extra point-centred reads inside the depth-edge branch
// on ps_3_0; implicit derivatives would have to be evaluated outside it.
float3 SampleHistoryPoint(float2 uv)
{
    return clamp(tex2Dlod(HistoryTex, float4(uv, 0, 0)).rgb, 0.0, HISTORY_CLAMP) * SSR_SCALE;
}

// Bilinear colour alone blends the bright background into an object's silhouette.
// On a depth edge, discard colour taps from a different previous-frame surface. In
// flat regions keep the hardware bilinear lookup. History and depth are full size
// even when SSR itself is half size.
float3 SampleHitHistory(float2 uv)
{
    if (fUsePrevDepth <= 0.0)
        return SampleHistory(uv);
    float2 pixel = uv / vec2InvHistorySize - 0.5;
    float2 base = (floor(pixel) + 0.5) * vec2InvHistorySize;
    float2 f = frac(pixel);
    float2 dx = float2(vec2InvHistorySize.x, 0.0);
    float2 dy = float2(0.0, vec2InvHistorySize.y);
    float d00 = tex2Dlod(PrevDepthTex, float4(base, 0, 0)).r;
    float d10 = tex2Dlod(PrevDepthTex, float4(base + dx, 0, 0)).r;
    float d01 = tex2Dlod(PrevDepthTex, float4(base + dy, 0, 0)).r;
    float d11 = tex2Dlod(PrevDepthTex, float4(base + dx + dy, 0, 0)).r;
    float reference = f.x < 0.5 ? (f.y < 0.5 ? d00 : d01) : (f.y < 0.5 ? d10 : d11);
    // Stored depth is logarithmic; 0.002 is about a 2% separation for a 10000:1 range.
    float4 same = 1.0 - step(0.002, abs(float4(d00, d10, d01, d11) - reference));
    if (all(same > 0.0))
        return SampleHistory(uv);
    float4 w = same * float4((1.0 - f.x) * (1.0 - f.y), f.x * (1.0 - f.y),
                             (1.0 - f.x) * f.y, f.x * f.y);
    float sum = dot(w, 1.0);
    float3 colour = SampleHistoryPoint(base) * w.x + SampleHistoryPoint(base + dx) * w.y
                  + SampleHistoryPoint(base + dy) * w.z + SampleHistoryPoint(base + dx + dy) * w.w;
    return colour / max(sum, 1e-5);
}

// The colour of a hit from TraceHit, blurred by blurPixels at fMaxDistance, with its confidence.
float4 HitColour(float4 hit, float blurPixels)
{
    if (hit.z <= 0.0)
        return 0.0;
    float radius = blurPixels * hit.w;
    float3 colour = radius > 0.0 ? SampleHistoryBlurred(hit.xy, radius) : SampleHitHistory(hit.xy);
    if (any(colour != colour))
        return 0.0;
    return float4(colour, hit.z);
}

// How strongly SSR may show at this pixel, 0 where the ray is not worth tracing: the sky,
// matte surfaces, and surfaces deferred_lighting would not show a reflection on anyway. It
// scales its reflection by 2 * x * z of _DEFERRED_GBUFFER_2_, so only a pixel where x or z is
// exactly zero (in 8 bits) is skipped, as foliage stores both as zero. Testing the product
// instead skipped car paint, whose small x times a small z fell under one 8-bit step though
// the game still multiplies the result up into a visible reflection.
// Gloss alone decides matte: weighting by x classed every car body as matte.
// While it rains, ground facing up (roads, pavements) reflects too though its gloss is under
// fGlossCutoff: the game keeps the gloss it has dry and only strengthens its own sky reflection
// in the rain. wetOnly is how much of the weight comes from that alone, for SSR_PS to brighten.
float SSRSurfaceWeight(float2 uv, out float wetOnly)
{
    wetOnly = 0.0;
    if (tex2Dlod(DepthTex, float4(uv, 0, 0)).r >= 0.9999)
        return 0.0; // sky
    if (fGlossCutoff < 0.0)
        return 1.0; // no specular G-buffer bound
    float3 spec = saturate(tex2Dlod(SpecularTex, float4(uv, 0, 0)).xyz);
    if (min(spec.x, spec.z) < 0.5 / 255.0)
        return 0.0;
    float gloss = spec.y;
    float weight = smoothstep(fGlossCutoff, fGlossCutoff + 0.2, gloss) * (1.0 + fGlossBoost * gloss);
    [branch]
    if (fWetness > 0.0)
    {
        // The wet ground pass's film (gloss 0.46, a fifth of its sheen on grass) reflects a third as much as
        // its puddles, which go by their own gloss of 0.9 above: at full weight over all the ground, tree trunks
        // showed in the roads and lawns, and the puddles did not stand out of the film around them.
        float film = 0.35 * smoothstep(0.2, 0.46, gloss);
        float wet = fWetness * film * smoothstep(0.75, 0.9, GBufferNormalWorld(uv).z);
        wetOnly = saturate(wet - weight);
        weight = max(weight, wet);
    }
    return weight;
}

float SSRSurfaceWeight(float2 uv)
{
    float wetOnly;
    return SSRSurfaceWeight(uv, wetOnly);
}

// A reflection for rays hidden behind something well nearer the camera than the reflecting
// surface, the player standing in front of a car, from last frame's scene just past his outline
// along the ray's path. Rays to the road hidden behind him cut a hole of his shape into the car
// beside him. weight is 0 where it does not apply: above the horizon, where the game's own map
// shows the sky, and for rays nothing nearer hid, which NeighbourFill takes.
float3 ScreenFallback(float3 C, float3 R, out float weight)
{
    float3 Rw = R.x * vec4WaterToView[0].xyz + R.y * vec4WaterToView[1].xyz + R.z * vec4WaterToView[2].xyz;
    weight = fFallback * (1.0 - saturate(Rw.z * 5.0)); // deferred_lighting's own horizon fade
    if (weight <= 0.0)
        return 0.0;

    // Where the ray went behind the player on its way across the screen, what it would have
    // reached is most likely what lies just past his outline along the same path: the lit road
    // the rays beside his figure hit. The road next to the car lies in its shadow, and taken
    // from there the figure showed darker than the rest of the door.
    bool behind = false;
    [branch]
    if (gTracePath > 0.0)
    {
        int past = 0;
        [loop] [fastopt]
        for (int k = 1; k <= 24; ++k)
        {
            float2 pathUV = lerp(gTraceUV0, gTraceUVEnd, k / 24.0);
            float pathZ = LinearDepth(pathUV);
            if (pathZ < C.z * 0.7)
            {
                behind = true;
                past = 0;
                continue;
            }
            if (!behind)
                continue;
            // Not the first point past his outline: the road there lies in his own shadow,
            // and a pixel of movement puts him there in last frame's scene.
            if (++past < 2)
                continue;
            float3 pathP = ViewPosFromUVZ(pathUV, pathZ);
            float2 pathHist = HistoryUV(pathP);
            if (any(pathHist <= 0.0) || any(pathHist >= 1.0))
                break;
            if (fUsePrevDepth > 0.0)
            {
                float prevPathZ = dot(float4(pathP, 1.0), float4(vec4ViewToPrevClip[0].w, vec4ViewToPrevClip[1].w,
                                                                 vec4ViewToPrevClip[2].w, vec4ViewToPrevClip[3].w));
                float prevZ = PrevLinearDepth(pathHist);
                if (abs(prevZ - prevPathZ) > 0.05 * prevPathZ + 0.1)
                    continue;
            }
            float2 r = 0.01 * float2(vec2InvViewportSize.x / vec2InvViewportSize.y, 1.0);
            float3 c = 0.0;
            c += tex2Dlod(HistoryTex, float4(pathHist, 0, 0)).rgb;
            c += tex2Dlod(HistoryTex, float4(saturate(pathHist + float2(r.x, 0.0)), 0, 0)).rgb;
            c += tex2Dlod(HistoryTex, float4(saturate(pathHist - float2(r.x, 0.0)), 0, 0)).rgb;
            c += tex2Dlod(HistoryTex, float4(saturate(pathHist + float2(0.0, r.y)), 0, 0)).rgb;
            c += tex2Dlod(HistoryTex, float4(saturate(pathHist - float2(0.0, r.y)), 0, 0)).rgb;
            // Near enough to what the ray would have hit to take its full weight, as the hits
            // beside it do; at fFallback's 0.8 the figure stayed a shade darker.
            weight = 1.0 - saturate(Rw.z * 5.0);
            return clamp(c * 0.2, 0.0, HISTORY_CLAMP) * SSR_SCALE;
        }
    }
    // No point past him: NeighbourFill takes over.
    weight = 0.0;
    return 0.0;
}

// The taps of the fill around a miss, NeighbourFill's and SSRSpread_PS's, turned by an angle and
// shrunk by up to 0.6 of their reach, both different at each pixel and, while the passes
// accumulate, each frame. The same ring at every pixel put a thin reflection, a kerb or a car's
// edge, into the misses around it once for each ring, 9, 18 and 36 pixels off: two or three
// shifted copies beside it. Turned, they fall apart into grain that the smoothing and the
// accumulation take out.
float2x2 FillTurn(float2 vPos)
{
    float angle = PixelJitter(vPos + vec2NoiseOffset) * 6.2831853;
    float scale = lerp(0.4, 1.0, PixelJitter(vPos.yx + float2(37.0, 11.0) + vec2NoiseOffset));
    float sn, cs;
    sincos(angle, sn, cs);
    return float2x2(cs, -sn, sn, cs) * scale;
}

// Misses among hits, SSRTrace_PS's in SSRResultTex: the reflection of the hits around them, those
// on about the same surface only. A miss next to
// what a car reflects, or among the hits on a door, showed the game's own map there, which holds
// only the sky: a dark rim around the reflection at night and dark grain on the doors. The
// blurred scene around the point a metre and a half along the ray, which filled them before,
// was duller than the reflection around them and dimmed it. Where no hit is near, weight is 0
// and the game's map stays, as it should where SSR finds nothing at all.
float3 NeighbourFill(float2 uv, float2 vPos, float z, out float weight)
{
    float2x2 turn = FillTurn(vPos);
    static const float2 taps[12] =
    {
        float2( 2.0,  0.0), float2(-2.0,  0.0), float2( 0.0,  2.0), float2( 0.0, -2.0),
        float2( 4.0,  4.0), float2(-4.0,  4.0), float2( 4.0, -4.0), float2(-4.0, -4.0),
        float2( 9.0,  0.0), float2(-9.0,  0.0), float2( 0.0,  9.0), float2( 0.0, -9.0)
    }; // full-size pixels
    float3 sum = 0.0;
    float sumW = 0.0;
    float fillGloss = fGlossCutoff >= 0.0 ? tex2Dlod(SpecularTex, float4(uv, 0, 0)).y : 0.0;
    float3 fillNormal = 0.0;
    if (fUseGBufferNormals > 0.0)
        fillNormal = GBufferNormalWorld(uv);
    [loop] [fastopt]
    for (int i = 0; i < 12; ++i)
    {
        float2 tapUV = uv + mul(turn, taps[i]) * fFillPixelScale * vec2InvViewportSize;
        float4 hit = tex2Dlod(SSRResultTex, float4(tapUV, 0, 0));
        float w = max(hit.a, 0.0) * saturate(1.0 - abs(LinearDepth(tapUV) - z) / (0.05 * z + 0.05));
        if (fUseGBufferNormals > 0.0 && w > 0.0)
            w *= smoothstep(0.8, 0.98, dot(fillNormal, GBufferNormalWorld(tapUV)));
        if (fGlossCutoff >= 0.0 && w > 0.0)
            w *= saturate(1.0 - abs(fillGloss - tex2Dlod(SpecularTex, float4(tapUV, 0, 0)).y) / 0.2);
        // A miss beside the reflection of something standing close to the surface (the player at a car's bonnet) most
        // likely sees what is behind it, not it: filled from such hits the misses around his reflection took its dark
        // colour, a halo of him 9 to 36 pixels wide. Only hits farther along their ray than fFillMinDistance fill.
        [branch]
        if (w > 0.0 && fFillMinDistance > 0.0)
            w *= smoothstep(0.5 * fFillMinDistance, fFillMinDistance, tex2Dlod(SSRHitDistTex, float4(tapUV, 0, 0)).r);
        [branch]
        if (w > 0.0)
        {
            sum += hit.rgb * w;
            sumW += w;
        }
    }
    weight = fNeighbourFill * saturate(sumW / 2.0);
    return sumW > 1e-3 ? sum / sumW : 0.0;
}

// The surface position and normal SSR traces from at this pixel.
float3 SSRSurface(float2 uv, float2 vPos, out float3 n)
{
    float3 C = ReconstructViewPos(vPos, LinearDepth(uv));
    [branch]
    if (fUseGBufferNormals > 0.0)
        n = GBufferNormal(uv);
    else
        n = ReconstructNormal(uv, C);
    n = (dot(n, C) > 0.0) ? -n : n;
    return C;
}

// SSR runs in passes, as one pixel shader it took about a thousand instruction slots where
// ps_3_0 promises 512: SSRTrace_PS marches the rays and reads the history where they hit, into
// a half float target of the same size, colour and confidence as the single pass had them;
// SSRFallback_PS and SSRSpread_PS work out what fills in where the rays found next to nothing
// into another, and SSR_PS puts the two together into the reflection. The march once handed
// on where it hit, for SSR_PS to read the history there; the reflections came out dull.
// The second target gets how far the ray went (SSRHitDistTex), for the accumulation.
struct TraceOutput
{
    float4 colour : COLOR0;
    float4 distance : COLOR1;
};

// MarchDepthTex for SSRTrace_PS's march, drawn at half the full size (vec2InvViewportSize is its
// pixel): the nearest view depth of the four full size pixels under each of its own.
float4 MarchDepth_PS(float2 uv : TEXCOORD0) : COLOR0
{
    float2 r = 0.25 * vec2InvViewportSize;
    float a = tex2Dlod(DepthTex, float4(uv - r, 0, 0)).r;
    float b = tex2Dlod(DepthTex, float4(uv + r, 0, 0)).r;
    float c = tex2Dlod(DepthTex, float4(uv + float2(r.x, -r.y), 0, 0)).r;
    float d = tex2Dlod(DepthTex, float4(uv + float2(-r.x, r.y), 0, 0)).r;
    // The log depth grows with the view depth, so its smallest is the nearest.
    return pow(fFarDivNear, min(min(a, b), min(c, d))) * fNearPlane;
}

// Reduce the linear half-size depth to the nearest depth of each 2x2 block.
float4 HiZDepth_PS(float2 uv : TEXCOORD0) : COLOR0
{
    float2 r = 0.25 * vec2InvViewportSize;
    float a = tex2Dlod(MarchDepthTex, float4(uv - r, 0, 0)).r;
    float b = tex2Dlod(MarchDepthTex, float4(uv + r, 0, 0)).r;
    float c = tex2Dlod(MarchDepthTex, float4(uv + float2(r.x, -r.y), 0, 0)).r;
    float d = tex2Dlod(MarchDepthTex, float4(uv + float2(-r.x, r.y), 0, 0)).r;
    return min(min(a, b), min(c, d));
}

// What SSRTrace_PS writes into both its targets where SSRSurfaceWeight is 0: the passes after it
// tell those pixels by one read of its target instead of the depth, specular and, in the rain,
// normal reads of SSRSurfaceWeight each. Both targets are half float, so it stays negative.
static const float4 kNoSurface = -1.0;

bool NoSurface(float4 trace)
{
    return trace.a < -0.5;
}

// Sample the view-dependent GGX visible-normal distribution. The stored gloss is
// artistic, so the roughness mapping is intentionally conservative. The game applies
// its own Fresnel/environment scaling when it composites SSR.
// Heitz, Sampling the GGX Distribution of Visible Normals, JCGT 7(4), 2018.
float3 SSRMicroNormal(float2 uv, float2 vPos, float3 n, float3 C)
{
    if (fStochasticSSR <= 0.0)
        return n;
    float gloss = saturate(tex2Dlod(SpecularTex, float4(uv, 0, 0)).y);
    float roughness = 1.0 - gloss;
    float alpha = max(roughness * roughness, 0.002);
    // No early out for a mirror-like surface, though at the clamp its lobe is the normal: the
    // branch itself costs the march six ps_3_0 slots, which it does not have to spare, and paint
    // glossier than 0.955 is rare enough that the samples it would skip are too.
    float u = min(PixelJitter(vPos + float2(83.0, 47.0) + vec2NoiseOffset), 0.9999);
    float phi = 6.2831853 * PixelJitter(vPos.yx + float2(17.0, 59.0) + vec2NoiseOffset.yx);
    float3 up = abs(n.z) < 0.999 ? float3(0.0, 0.0, 1.0) : float3(1.0, 0.0, 0.0);
    float3 tangent = normalize(cross(up, n));
    float3 bitangent = cross(n, tangent);
    float3 view = -normalize(C);
    float3 localView = float3(dot(view, tangent), dot(view, bitangent), max(dot(view, n), 1e-4));
    float3 stretchedView = normalize(float3(alpha * localView.xy, localView.z));
    float lensq = dot(stretchedView.xy, stretchedView.xy);
    float3 t1 = lensq > 1e-8 ? float3(-stretchedView.y, stretchedView.x, 0.0) * rsqrt(lensq)
                              : float3(1.0, 0.0, 0.0);
    float3 t2 = cross(stretchedView, t1);
    float radius = sqrt(u);
    float sine, cosine;
    sincos(phi, sine, cosine);
    float p1 = radius * cosine;
    float p2 = radius * sine;
    float blend = 0.5 * (1.0 + stretchedView.z);
    p2 = lerp(sqrt(max(1.0 - p1 * p1, 0.0)), p2, blend);
    float3 visible = p1 * t1 + p2 * t2 + sqrt(max(1.0 - p1 * p1 - p2 * p2, 0.0)) * stretchedView;
    float3 localNormal = normalize(float3(alpha * visible.xy, max(visible.z, 0.0)));
    return normalize(localNormal.x * tangent + localNormal.y * bitangent + localNormal.z * n);
}

TraceOutput SSRTrace_PS(float2 uv : TEXCOORD0, float2 vPos : VPOS)
{
    TraceOutput o = (TraceOutput)0;
    if (SSRSurfaceWeight(uv) <= 0.0)
    {
        o.colour = kNoSurface;
        o.distance = kNoSurface;
        return o;
    }
    float3 n;
    float3 C = SSRSurface(uv, vPos, n);

    // While accumulating, vec2NoiseOffset moves every pixel's steps on each frame, so the
    // accumulation averages the steps out and fewer of them do.
    float jitter = fStepJitter > 0.0 ? PixelJitter(vPos + vec2NoiseOffset) : 1.0;
    float3 microNormal = SSRMicroNormal(uv, vPos, n, C);
    float4 hit = TraceHit(C, n, microNormal, jitter, fDistanceFade);
    float distance = hit.z > 0.0 ? hit.w : 0.0;
    // .b carries the share of this ray's steps the depth hierarchy skipped and .a which of its
    // levels the ray walked, both for debug view 12. The passes that read this target take .r
    // and .g, and .a only through NoSurface, which the kNoSurface above still marks.
    o.distance = float4(distance, hit.z > 0.0 ? gTraceHitMotion : 0.0, gTraceSkipShare, gTraceHiZLevel);
    // Debug 11: the previous-scene UV from which this hit reads colour, R=x, G=y.
    // This makes a suspect reflected pixel traceable back to its source on screen.
    if (fDebugMode > 10.5 && fDebugMode < 11.5)
    {
        o.colour = float4(hit.xy, hit.z, hit.z);
        return o;
    }
    // Debug view 3 (for now): what the ray hit in place of the hit, which SSR_PS passes on, see
    // SSRDebug_PS.
    if (fDebugMode > 2.5 && fDebugMode < 3.5)
    {
        float nearer = gTraceHitZ < C.z - 0.25 ? 1.0 : 0.0;
        o.colour = float4(nearer, 1.0 - nearer, gTraceRayZ < 0.0 ? 1.0 : 0.0, hit.z);
        return o;
    }
    // Rough paint reflects a spread of directions even where the ray hit, so its hit wants
    // blurring by the lobe's width as well as by the ray's length; a mirror wants neither.
    // fReflectionBlur scales both terms, so at its default of 0 the shipped picture does not
    // move at all, and the water's reflection keeps asking for its own blur alone.
    float gloss = fGlossCutoff >= 0.0 ? tex2Dlod(SpecularTex, float4(uv, 0, 0)).y : 1.0;
    float rough = 1.0 - gloss;
    o.colour = HitColour(hit, fReflectionBlur * (1.0 + 2.0 * rough * rough));
    return o;
}

// Only rays that found next to nothing are filled in, fully at confidence 0 and not at all from
// kFallbackBelow up. A hit faded only part of the way, at the screen's edge or by distance,
// keeps its own colour over the game's map.
static const float kFallbackBelow = 0.1;

// What fills in where SSRTrace_PS's ray in SSRResultTex found next to nothing, ScreenFallback or
// NeighbourFill, and its weight; 0 where neither applies. It follows the ray's path on the screen, which
// SetupReflectionRay works out again, for the mirror direction rather than the trace's sample: a
// stochastic ray is one noisy estimate of a lobe, while the guess for its miss stands for the
// whole lobe, whose mean direction the mirror ray is. Sampling again here also cost a second
// VNDF for every pixel the trace found nothing on, which tracing stochastically makes many.
float4 SSRFallback_PS(float2 uv : TEXCOORD0, float2 vPos : VPOS) : COLOR0
{
    float4 trace = tex2Dlod(SSRResultTex, float4(uv, 0, 0));
    if (NoSurface(trace) || trace.a >= kFallbackBelow)
        return 0.0;
    float z = 0.0;
    float weight = 0.0;
    float3 fallback = 0.0;
    [branch]
    if (fFallback > 0.0)
    {
        float3 n;
        float3 C = SSRSurface(uv, vPos, n);
        z = C.z;
        ReflectionRay ray;
        SetupReflectionRay(C, n, n, ray);
        fallback = ScreenFallback(C, reflect(normalize(C), n), weight);
    }
    [branch]
    if (weight <= 0.0 && fNeighbourFill > 0.0)
    {
        if (fFallback <= 0.0)
            z = LinearDepth(uv);
        fallback = NeighbourFill(uv, vPos, z, weight);
    }
    return float4(fallback, weight);
}

// One step of a cascade after SSRFallback_PS: misses still empty or faint take what the last step
// filled in around them, fSpreadRadius pixels out, on about the same surface, a fifth weaker. The
// first step reaches only the hits within nine pixels, and a wider hole, as around what stands
// near a car, kept a dark rim. Weakening with each step leaves the game's own map showing
// through where SSR finds nothing over a wide area. Reads the last step from SSRFallbackTex.
float4 SSRSpread_PS(float2 uv : TEXCOORD0, float2 vPos : VPOS) : COLOR0
{
    float2x2 turn = FillTurn(vPos);
    float4 own = tex2Dlod(SSRFallbackTex, float4(uv, 0, 0));
    float4 trace = tex2Dlod(SSRResultTex, float4(uv, 0, 0));
    if (own.a >= max(fFallback, fNeighbourFill) * 0.99 || NoSurface(trace) || trace.a >= kFallbackBelow)
        return own;
    // A cross, not a ring of eight: FillTurn spins it by a different angle at every pixel and
    // every frame, so the diagonals the cross leaves out are covered by the neighbours and by
    // the accumulation, while each step of the cascade pays four depth, gloss and normal reads
    // instead of eight. The alpha keeps the eight taps' scale: half of them, undivided.
    static const float2 taps[4] =
    {
        float2( 1.0,  0.0), float2(-1.0,  0.0), float2( 0.0,  1.0), float2( 0.0, -1.0)
    };
    float z = LinearDepth(uv);
    float fillGloss = fGlossCutoff >= 0.0 ? tex2Dlod(SpecularTex, float4(uv, 0, 0)).y : 0.0;
    float3 fillNormal = 0.0;
    if (fUseGBufferNormals > 0.0)
        fillNormal = GBufferNormalWorld(uv);
    float3 sum = 0.0;
    float sumW = 0.0, top = 0.0;
    [loop] [fastopt]
    for (int i = 0; i < 4; ++i)
    {
        float2 tapUV = uv + mul(turn, taps[i]) * fSpreadRadius * fFillPixelScale * vec2InvViewportSize;
        float4 tap = tex2Dlod(SSRFallbackTex, float4(tapUV, 0, 0));
        float w = tap.a * saturate(1.0 - abs(LinearDepth(tapUV) - z) / (0.05 * z + 0.05));
        if (fUseGBufferNormals > 0.0 && w > 0.0)
            w *= smoothstep(0.8, 0.98, dot(fillNormal, GBufferNormalWorld(tapUV)));
        if (fGlossCutoff >= 0.0 && w > 0.0)
            w *= saturate(1.0 - abs(fillGloss - tex2Dlod(SpecularTex, float4(tapUV, 0, 0)).y) / 0.2);
        sum += tap.rgb * w;
        sumW += w;
        top = max(top, w);
    }
    float a = max(own.a, 0.8 * min(top, saturate(sumW)));
    if (sumW <= 1e-3 || a <= own.a)
        return own;
    // own keeps its share, the spread fills up the rest.
    return float4((own.rgb * own.a + sum / sumW * (a - own.a)) / a, a);
}

// The reflection from SSRTrace_PS's hit in SSRResultTex, SSRFallback_PS's guess in
// SSRFallbackTex filling in where the ray found next to nothing (premultiplied).
float4 SSR_PS(float2 uv : TEXCOORD0, float2 vPos : VPOS) : COLOR0
{
    float4 hit = tex2Dlod(SSRResultTex, float4(uv, 0, 0));
    if (NoSurface(hit))
        return 0.0;
    float wetOnly;
    float surfaceWeight = SSRSurfaceWeight(uv, wetOnly);

    if ((fDebugMode > 2.5 && fDebugMode < 3.5) || (fDebugMode > 10.5 && fDebugMode < 11.5))
        return hit;
    float4 r = hit;
    [branch]
    if (max(fFallback, fNeighbourFill) > 0.0 && r.a < 1.0)
    {
        float4 fallback = tex2Dlod(SSRFallbackTex, float4(uv, 0, 0));
        float fill = fallback.a * (1.0 - r.a) * saturate(1.0 - r.a / kFallbackBelow);
        float a = r.a + fill;
        r.rgb = a > 1e-4 ? (r.rgb * r.a + fallback.rgb * fill) / a : 0.0;
        r.a = a;
    }
    // On wet ground the game's reflection strength, which deferred_lighting multiplies SSR by,
    // is that of dry asphalt, so the reflection is drawn brighter there.
    r.rgb *= 1.0 + (fWetGroundBoost - 1.0) * wetOnly;
    // Car bodies (material category 2: the ID less its 128 and 8 bits): deferred_lighting scales the reflection by
    // 10 x c39.w for its dim parabolic map, which SSR's scene light does not need. With Vehicle Paint's gloss SSR
    // shows in full on clean paint, and at night lamp-lit poles and walls turned roofs white.
    [branch]
    if (fVehicleScale != 1.0)
    {
        float id = floor(tex2Dlod(SkinIDTex, float4(uv, 0, 0)).r * 255.0 + 0.5);
        id -= id >= 128.0 ? 128.0 : 0.0;
        if (fmod(id, 8.0) == 2.0)
            r.rgb *= fVehicleScale;
    }
    return float4(r.rgb, saturate(r.a * surfaceWeight * fIntensity));
}

// One grid of the rain's rings on open water, as water_rain_rings.patch draws them in the game's
// water shader (the same cells, hash, times and profile), so this reflection ripples with the
// game's: a drop in each cell at its own time, its ring running out to 0.4 of the cell and fading.
float2 WaterRingGrid(float2 p, float cell, float rate, float seed)
{
    float2 q = p / cell + seed;
    float2 id = floor(q);
    float h = frac(52.9829178 * frac(dot(id, float2(0.0671105608, 0.00583714992))));
    float h2 = frac(52.9829178 * frac(dot(id + 17.0, float2(0.0671105608, 0.00583714992))));
    float2 d = frac(q) - (0.5 + (float2(h, h2) - 0.5) * 0.3);
    float r = sqrt(max(dot(d, d), 0.001));
    float phase = frac(vec4WaterRings.y * rate + h);
    float x = (r - 0.4 * phase) * cell;
    return d / r * (x * exp(-100.0 * x * x) * (1.0 - phase) * (1.0 - phase));
}

float3 WaterNormal(float2 worldXY, float distSq)
{
    float near = max(1.0 - distSq * 0.0004, 0.0);

    float2 slope = (tex2D(SurfaceTex, worldXY * 0.002).zw - 0.5) * 0.0512 * (1.0 - near);
    slope += (tex2D(SurfaceTex, worldXY * 0.01).zw - 0.5) * 1.024;
    slope += (tex2D(SurfaceTex, worldXY * 0.0454545468).zw - 0.5) * 0.465454549 * near;
    slope *= fWaterNormalStrength;

    [branch]
    if (vec4WaterRings.x > 0.0)
    {
        float fade = saturate(sqrt(distSq) * vec4WaterRings.z + vec4WaterRings.w);
        float2 rings = WaterRingGrid(worldXY, 1.0, 0.7, 0.0) + WaterRingGrid(worldXY, 1.37, 0.57, 5.3) +
                       WaterRingGrid(worldXY, 0.83, 0.86, 11.7);
        slope += rings * 7.0 * vec4WaterRings.x * fade;
    }

    float3 nWorld = normalize(float3(slope, 1.0));

    return float3(dot(vec4WaterToView[0].xyz, nWorld),
                  dot(vec4WaterToView[1].xyz, nWorld),
                  dot(vec4WaterToView[2].xyz, nWorld));
}

float4 SSRWater_PS(float2 uv : TEXCOORD0, float2 vPos : VPOS) : COLOR0
{
    // The water plane extends under everything, including tunnels below water level, so
    // only reflect where the game actually drew water this frame.
    if (fUseWaterMask > 0.0)
    {
        float3 diff = abs(tex2Dlod(PostWaterTex, float4(uv, 0, 0)).rgb - tex2Dlod(PreWaterTex, float4(uv, 0, 0)).rgb);
        if (max(diff.r, max(diff.g, diff.b)) < 1e-4)
            return 0.0;
    }

    float3 dir = float3((vPos + 0.5f) * vec4ProjInfo.xy + vec4ProjInfo.zw, 1.0);

    float denom = dot(vec4WaterPlane.xyz, dir);
    if (abs(denom) < 1e-6)
        return 0.0;

    float t = -vec4WaterPlane.w / denom;

    if (t <= 0.0 || t >= LinearDepth(uv))
        return 0.0;

    float3 C = dir * t;

    float2 worldXY = float2(dot(vec4WaterWorldX.xyz, C) + vec4WaterWorldX.w,
                            dot(vec4WaterWorldY.xyz, C) + vec4WaterWorldY.w);

    float3 n = WaterNormal(worldXY, dot(C, C));
    n = (dot(n, C) > 0.0) ? -n : n;

    float4 r = HitColour(TraceHit(C, n, n, 1.0, 0.0), fWaterBlur);

    return float4(r.rgb, saturate(r.a * fWaterIntensity));
}

// SSR debug view (graphics menu). Built right after the SSR pass, while the G-buffer is intact,
// and shown over the finished frame, so tone mapping and exposure do not change the colours.
//   1: what SSR hands to deferred_lighting, the reflected colour times its confidence
//   2: where rays go: green a hit (brightness is confidence), red a glossy pixel whose ray
//      found nothing, dark blue a matte pixel that is not traced, black the sky
//   3: surface normals, left half rebuilt from depth (what SSR uses), right half from the
//      G-buffer
//   4: green what SSR found and the game shows, red what SSR found but deferred_lighting
//      fades out, because it keeps reflections only when they point above the horizon
//   6: car glass, drawn by the patched glass shaders themselves: green a hit, red a miss,
//      blue how much the fade for reflections pointing back at the camera keeps
//   7: contact shadows alone, white lit, black shadowed
//   8: indirect light alone
//   5: _DEFERRED_GBUFFER_2_ as stored: red specular intensity, green gloss, blue the
//      reflection strength deferred_lighting uses; see SSRSurfaceWeight

// vec4WaterToView rotates world into reconstruction space; its transpose rotates back.
float3 ViewToWorld(float3 v)
{
    return v.x * vec4WaterToView[0].xyz + v.y * vec4WaterToView[1].xyz + v.z * vec4WaterToView[2].xyz;
}

float4 SSRDebug_PS(float2 uv : TEXCOORD0, float2 vPos : VPOS) : COLOR0
{
    // As many white squares in the top left corner as the mode number, so screenshots say
    // which view they show.
    float2 cell = floor(vPos / 24.0);
    float2 inCell = frac(vPos / 24.0);
    if (cell.y == 0.0 && cell.x < fDebugMode && all(inCell > 0.2) && all(inCell < 0.8))
        return float4(1.0, 1.0, 1.0, 1.0);

    float4 ssr = tex2Dlod(SSRResultTex, float4(uv, 0, 0));

    // 12: what the depth hierarchy did, straight from the trace's second target: red the share
    // of this ray's steps its jumps took away, green where the ray hit something, blue where it
    // walked the coarser level, dark grey a pixel SSR did not trace, as in view 3. Needs the
    // distance target, so temporal, the fill or the smoothing has to be on for it to be written.
    if (fDebugMode > 11.5 && fDebugMode < 12.5)
    {
        float4 info = tex2Dlod(SSRHitDistTex, float4(uv, 0, 0));
        if (info.a < -0.5)
            return float4(0.12, 0.12, 0.12, 1.0);
        return float4(saturate(info.b), info.r > 0.0 ? 0.8 : 0.0, info.a > 0.5 ? 0.8 : 0.0, 1.0);
    }

    // 11: raw history coordinates, not modulated by hit confidence.
    if (fDebugMode > 10.5 && fDebugMode < 11.5)
        return float4(ssr.rgb, 1.0);

    // 8: indirect light alone (SSRResultTex holds it in this mode)
    if (fDebugMode > 7.5)
        return float4(ssr.rgb, 1.0);

    // 7: contact shadows, white lit, black shadowed (SSRResultTex holds them in this mode)
    if (fDebugMode > 6.5)
        return float4((1.0 - ssr.xxx), 1.0);
    float rawDepth = tex2Dlod(DepthTex, float4(uv, 0, 0)).r;
    bool sky = rawDepth >= 0.9999;

    if (fDebugMode < 1.5)
        return float4(ssr.rgb * ssr.a, 1.0);

    if (fDebugMode < 2.5)
    {
        if (sky)
            return float4(0.0, 0.0, 0.0, 1.0);
        if (SSRSurfaceWeight(uv) <= 0.0)
            return float4(0.0, 0.0, 0.25, 1.0);
        if (ssr.a <= 0.0)
            return float4(0.6, 0.0, 0.0, 1.0);
        return float4(0.0, 0.2 + 0.8 * ssr.a, 0.0, 1.0);
    }

    if (fDebugMode > 4.5)
    {
        if (sky)
            return float4(0.0, 0.0, 0.0, 1.0);
        return float4(saturate(tex2Dlod(SpecularTex, float4(uv, 0, 0)).xyz), 1.0);
    }

    float3 C = ViewPosFromUVZ(uv, LinearDepth(uv));

    // 3 (for now): what each ray hit, which SSR_PS writes in place of the colour, read before
    // smoothing and accumulation. Red: something nearer the camera than the surface reflecting
    // it, green: something farther; blue added where the ray heads towards the camera; brighter
    // the more the hit counts. Black a miss, dark grey a surface SSR does not trace.
    if (fDebugMode < 3.5)
    {
        if (sky)
            return float4(0.0, 0.0, 0.0, 1.0);
        if (SSRSurfaceWeight(uv) <= 0.0)
            return float4(0.12, 0.12, 0.12, 1.0);
        if (ssr.a <= 0.0)
            return float4(0.0, 0.0, 0.0, 1.0);
        return float4(ssr.rgb * (0.25 + 0.75 * ssr.a), 1.0);
    }

    if (sky || ssr.a <= 0.0)
        return float4(0.0, 0.0, 0.0, 1.0);
    float3 V = normalize(ViewToWorld(C));
    float3 R = reflect(V, GBufferNormalWorld(uv));
    float fade = saturate(R.z * 5.0); // deferred_lighting's own horizon fade
    return float4(ssr.a * (1.0 - fade), ssr.a * fade, 0.0, 1.0);
}

// SSRHitDistTex stores distance divided by fMaxDistance; zero means a miss.
// A nearby person and the road several metres behind them can reflect on the same
// bonnet. Reflecting-surface depth and normal alone cannot tell those hits apart.
float SSRHitDistanceWeight(float a, float b)
{
    if (a <= 0.0 || b <= 0.0)
        return (a <= 0.0 && b <= 0.0) ? 1.0 : 0.0;
    float nearest = min(a, b) * fMaxDistance;
    float tolerance = 0.1 + 0.1 * nearest;
    float difference = abs(a - b) * fMaxDistance;
    return 1.0 - smoothstep(tolerance, 2.0 * tolerance, difference);
}

// A stochastic ray can miss even when a nearby ray hits the same reflection.
// Let those hits bound history for a missed centre, while keeping misses out of
// a hit centre's window: lowering hit confidence there reveals the bright map.
float SSRTemporalHitWeight(float centre, float tap, float stochastic)
{
    if (stochastic > 0.0 && centre == 0.0 && tap > 0.0)
        return 1.0;
    return SSRHitDistanceWeight(centre, tap);
}

// Previous accumulated colour may depict a different position of a moving hit.
// Preserve history for static geometry and progressively favour this frame as
// the hit's own screen-space motion grows. A miss has no motion measurement.
float SSRMovingHitHistoryWeight(float motionPixels)
{
    return 1.0 - smoothstep(1.5, 8.0, motionPixels);
}

float SSRTemporalMotionWeight(float hitDistance, float hitMotion, bool stochasticMiss,
                              float neighbourMotion, float neighbourMotionWeight)
{
    if (hitDistance > 0.0)
        return SSRMovingHitHistoryWeight(hitMotion);
    if (stochasticMiss && neighbourMotionWeight > 0.0)
        return SSRMovingHitHistoryWeight(neighbourMotion / neighbourMotionWeight);
    return 1.0;
}

// A single stochastic miss is an uncertain observation when nearby rays on
// the same receiver still hit. Slow the loss of its history so the lighter
// game reflection map does not form a ring around an otherwise solid hit.
// The caller still rejects invalid/offscreen history and applies the normal
// neighbourhood clamp; a moving hit still reduces the weight. Cap the boost
// so a long run of misses cannot retain stale detail almost indefinitely.
float SSRStochasticMissKeep(float keep, float blend, bool stochasticMiss,
                            float currentAlpha, float nearbyHitWeight)
{
    if (stochasticMiss && currentAlpha <= 1e-4 && nearbyHitWeight > 0.0)
        return max(keep, min(keep * (2.0 - blend), 0.95));
    return keep;
}

// Each pixel decides on its own whether its ray hit and where, so neighbours on a car panel
// pick slightly different points and the reflection looks grainy. A small depth aware blur,
// in premultiplied form so misses (alpha 0) neither darken the colour nor bleed a halo, and
// weighted by depth so a bonnet does not pick up the road behind it. Contact shadows and
// indirect light (fDenoiseSSROnly 0) are plain values, alpha included: indirect light keeps
// the share of the ambient it replaces there.
float4 SSRDenoise_PS(float2 uv : TEXCOORD0) : COLOR0
{
    // In pairs opposite each other, so the blur takes as much from every side and moves nothing. The Poisson
    // disk before leaned 6% of the radius up: indirect light gathers last frame's scene, which holds last frame's
    // smoothed indirect light, and the lean added up frame after frame into noise flowing up the screen.
    static const float2 taps[12] =
    {
        float2(-0.326, -0.406), float2( 0.326,  0.406),
        float2(-0.840, -0.074), float2( 0.840,  0.074),
        float2(-0.696,  0.457), float2( 0.696, -0.457),
        float2(-0.203,  0.621), float2( 0.203, -0.621),
        float2( 0.962, -0.195), float2(-0.962,  0.195),
        float2( 0.473, -0.480), float2(-0.473,  0.480)
    };

    // Most of the screen is sky, roads and walls, which SSR does not trace and deferred_lighting
    // would not show a reflection on: twelve taps there were spent for nothing. Contact shadows,
    // smoothed here too, fall on those surfaces, so they skip this.
    if (fDenoiseSSROnly > 0.0 && SSRSurfaceWeight(uv) <= 0.0)
        return 0.0;

    float4 centre = tex2Dlod(SSRResultTex, float4(uv, 0, 0));
    float2 radius = fDenoiseRadius * vec2InvViewportSize;

    // Where every tap reads what the centre does, the blur gives the centre back, whatever the
    // weights: for contact shadows most of the screen, lit through. Telling that takes the
    // colour taps alone, not the depth of every tap, which is most of what the pass costs.
    bool same = true;
    [loop] [fastopt]
    for (int i = 0; i < 12; ++i)
    {
        float4 tap = tex2Dlod(SSRResultTex, float4(uv + taps[i] * radius, 0, 0));
        if (!all(tap == centre))
        {
            same = false;
            break;
        }
    }
    [branch]
    if (same)
        return centre;

    float centreZ = LinearDepth(uv);
    float centreHitDistance = 0.0;
    if (fDenoiseSSROnly > 0.0 && fDenoiseHitDistance > 0.0)
        centreHitDistance = tex2Dlod(SSRHitDistTex, float4(uv, 0, 0)).r;
    // A crease can separate surfaces at the same depth. Keep their reflection samples apart.
    float3 centreNormal = 0.0;
    if (fDenoiseSSROnly > 0.0 && fUseGBufferNormals > 0.0)
        centreNormal = GBufferNormalWorld(uv);
    float colourWeight = fDenoiseSSROnly > 0.0 ? centre.a : 1.0;
    float4 sum = float4(centre.rgb * colourWeight, colourWeight);
    float alphaSum = centre.a;
    float weightSum = 1.0;

    // Reread colour in this loop: the same taps are cached, and keeping twelve
    // colours in registers made the distance-aware shader too large for ps_3_0.
    [loop] [fastopt]
    for (int j = 0; j < 12; ++j)
    {
        float w = exp(-dot(taps[j], taps[j]) * 2.0);
        w *= saturate(1.0 - abs(LinearDepth(uv + taps[j] * radius) - centreZ) / (centreZ * 0.02));
        if (fDenoiseSSROnly > 0.0 && fUseGBufferNormals > 0.0 && w > 0.0)
            w *= smoothstep(0.8, 0.98, dot(centreNormal, GBufferNormalWorld(uv + taps[j] * radius)));
        if (fDenoiseSSROnly > 0.0 && fDenoiseHitDistance > 0.0 && w > 0.0)
            w *= SSRHitDistanceWeight(centreHitDistance, tex2Dlod(SSRHitDistTex, float4(uv + taps[j] * radius, 0, 0)).r);
        float4 sample = tex2Dlod(SSRResultTex, float4(uv + taps[j] * radius, 0, 0));
        colourWeight = w * (fDenoiseSSROnly > 0.0 ? sample.a : 1.0);
        sum += float4(sample.rgb * colourWeight, colourWeight);
        alphaSum += w * sample.a;
        weightSum += w;
    }

    float a = alphaSum / weightSum;
    float3 colour = sum.a > 1e-4 ? sum.rgb / sum.a : centre.rgb;
    return float4(colour, a);
}

// Contact shadows: a short ray from each pixel towards the sun through the depth buffer. The
// game's sun shadow map is too coarse for the contact between a ped's feet or a car's tyres
// and the ground; this fills that in. The result is occlusion (0 lit, 1 shadowed), so an
// unbound sampler in deferred_lighting changes nothing. It is smoothed spatially, then
// accumulated over frames by ContactTemporal_PS.
float4 ContactShadows_PS(float2 uv : TEXCOORD0, float2 vPos : VPOS) : COLOR0
{
    float rawDepth = tex2Dlod(DepthTex, float4(uv, 0, 0)).r;
    if (rawDepth >= 0.9999 || vec4SunView.w <= 0.0)
        return float4(0.0, 0.0, 0.0, 1.0);
    // GPU grass: its blades sway into each other's rays and its flat flower heads face other ways than their
    // normals, so the shadow flickered on them
    if (fCSGrassGloss > 0.0 && abs(tex2Dlod(SpecularTex, float4(uv, 0, 0)).g * 1023.0 - fCSGrassGloss) < 0.5)
        return float4(0.0, 0.0, 0.0, 1.0);

    float3 C = ViewPosFromUVZ(uv, pow(fFarDivNear, rawDepth) * fNearPlane);
    if (C.z >= fCSMaxViewDistance)
        return float4(0.0, 0.0, 0.0, 1.0);

    float3 L = vec4SunView.xyz;
    float3 n;
    [branch]
    if (fUseGBufferNormals > 0.0)
        n = GBufferNormal(uv);
    else
        n = ReconstructNormal(uv, C);
    n = (dot(n, C) > 0.0) ? -n : n;
    if (dot(n, L) <= 0.0)
        return float4(0.0, 0.0, 0.0, 1.0); // facing away from the sun, the game already darkens it

    // Depth gets coarser with distance, so the start moves further off the surface there;
    // otherwise distant tile seams and kerb edges shadowed themselves as dotted lines.
    float3 P0 = C + n * (0.03 + C.z * 0.005);

    // A ray heading back towards the camera must stay in front of the near plane.
    float len = fCSLength;
    if (L.z < 0.0)
        len = min(len, (P0.z - fNearPlane * 2.0) / -L.z);
    if (len <= 0.0)
        return float4(0.0, 0.0, 0.0, 1.0);

    // While accumulating, vec2NoiseOffset moves every pixel's steps on each frame, so the
    // accumulation averages the steps out instead of keeping one frame's noise.
    float jitter = fStepJitter > 0.0 ? PixelJitter(vPos + vec2NoiseOffset) : 1.0;
    float occlusion = 0.0;
    float prevZ = P0.z;

    [loop] [fastopt]
    for (int i = 0; i < CS_STEPS; ++i)
    {
        float t = ((float) i + jitter) / (float) CS_STEPS;
        float3 P = P0 + L * (len * t);
        float2 sampleUV = ViewToUV(P);
        if (any(sampleUV <= 0.0) || any(sampleUV >= 1.0))
            break;

        // The depth buffer only holds the front of things. Anything a sample lands behind by
        // less than the thickness occludes; the thickness grows by the depth this step covered,
        // so a long step does not jump over a ped.
        float delta = P.z - LinearDepth(sampleUV);
        float thickness = abs(P.z - prevZ) + fCSThickness;
        prevZ = P.z;
        if (delta > 0.0 && delta < thickness)
        {
            occlusion = 1.0 - t * t; // occluders further along the ray cast softer shadows
            break;
        }
    }

    float fade = 1.0 - smoothstep(fCSMaxViewDistance * 0.75, fCSMaxViewDistance, C.z);
    // With the sun grazing the surface the ray runs along it and any seam blocks it; the game's
    // own lighting already darkens such surfaces, so contact shadows fade out there.
    fade *= saturate(dot(n, L) * 5.0);
    return float4(saturate(occlusion * fade * fCSIntensity), 0.0, 0.0, 1.0);
}

// Contact shadows marched at half size (SSRResultTex), brought up to full size: each of the four
// half size pixels around this one weighs by how near it is, as bilinear filtering would, and
// by how close the depth it marched from is to this pixel's, so a shadow on the ground does not
// spread up a ped's leg or onto the wall behind a kerb. vec2InvViewportSize is the full size.
float4 ContactUpsample_PS(float2 uv : TEXCOORD0) : COLOR0
{
    float z = LinearDepth(uv);
    float2 halfSize = floor(0.5 / vec2InvViewportSize);
    float2 p = uv * halfSize - 0.5;
    float2 f = frac(p);
    float2 base = (floor(p) + 0.5) / halfSize;
    float sum = 0.0, weightSum = 0.0;
    [unroll]
    for (int y = 0; y < 2; ++y)
    {
        [unroll]
        for (int x = 0; x < 2; ++x)
        {
            float2 tapUV = base + float2(x, y) / halfSize;
            float bilinear = (x ? f.x : 1.0 - f.x) * (y ? f.y : 1.0 - f.y);
            // The depth the half size pass read at that pixel's centre, as it read it.
            float w = bilinear * (saturate(1.0 - abs(LinearDepth(tapUV) - z) / (z * 0.02)) + 1e-3);
            sum += w * tex2Dlod(SSRResultTex, float4(tapUV, 0, 0)).r;
            weightSum += w;
        }
    }
    return float4(sum / max(weightSum, 1e-6), 0.0, 0.0, 1.0);
}

// Blends this frame's contact shadows (SSRResultTex, after smoothing) with last frame's
// accumulation (SSRAccumTex), taken where the surface was last frame; a shadow lies on its
// surface, so that is where it was. The history is clamped between the least and the most
// shadow of this frame's 3x3 neighbourhood, so the shadow of a ped that walked on cannot stay
// behind him, and it is dropped where last frame's depth shows another surface.
float4 ContactTemporal_PS(float2 uv : TEXCOORD0, float2 vPos : VPOS) : COLOR0
{
    float current = tex2Dlod(SSRResultTex, float4(uv, 0, 0)).r;
    if (fTemporalBlend <= 0.0 || tex2Dlod(DepthTex, float4(uv, 0, 0)).r >= 0.9999)
        return float4(current, 0.0, 0.0, 1.0);

    float lo = current, hi = current;
    [unroll]
    for (int y = -1; y <= 1; ++y)
    {
        [unroll]
        for (int x = -1; x <= 1; ++x)
        {
            if (x == 0 && y == 0)
                continue;
            float s = tex2Dlod(SSRResultTex, float4(uv + float2(x, y) * vec2InvViewportSize, 0, 0)).r;
            lo = min(lo, s);
            hi = max(hi, s);
        }
    }
    // A neighbourhood all alike clamps any history to the current value: lit ground, mostly.
    [branch]
    if (hi <= lo)
        return float4(current, 0.0, 0.0, 1.0);

    float3 C = ReconstructViewPos(vPos, LinearDepth(uv));
    bool checkDepth;
    float2 prevUV = TemporalHistoryUV(uv, C, checkDepth);
    float keep = fTemporalBlend;
    if (any(prevUV <= 0.0) || any(prevUV >= 1.0))
        keep = 0.0;
    else if (fUsePrevDepth > 0.0 && checkDepth)
    {
        float4 clip = C.x * vec4ViewToPrevClip[0] + C.y * vec4ViewToPrevClip[1]
                    + C.z * vec4ViewToPrevClip[2] + vec4ViewToPrevClip[3];
        float prevZ = PrevLinearDepth(prevUV);
        if (abs(prevZ - clip.w) > 0.05 * clip.w + 0.1)
            keep = 0.0;
    }

    float history = clamp(tex2Dlod(SSRAccumTex, float4(prevUV, 0, 0)).r, lo, hi);
    return float4(lerp(current, history, keep), 0.0, 0.0, 1.0);
}

// Blends this frame's SSR (SSRResultTex, after smoothing) with last frame's accumulation
// (SSRAccumTex), taken where the reflected image was last frame: a reflection moves over its
// surface as the camera does, the further its hit the more, and taken where the surface was
// the history left a second, shifted copy of a car or a lamp beside this frame's, which the
// neighbourhood clamp let through on paint reflecting much the same around it. Its image lies
// behind the surface along the view ray, as far as the ray went (SSRHitDistTex); a miss, and
// a surface that moved by itself, keep the surface's own place. The history is clamped to the
// mean of this frame's 3x3 neighbourhood give or take 1.5 times its spread, so it cannot
// bring back what is no longer there, and is dropped where last frame's depth shows another
// surface.
// Blending is premultiplied: a miss (alpha 0) must fade a reflection out, not darken its colour.
// Indirect light (fTemporalAnySurface) blends as it is: its alpha is the share of the ambient
// it replaces, see SSGI_PS.
float4 TemporalPremultiply(float4 c)
{
    return fTemporalAnySurface > 0.0 ? c : float4(c.rgb * c.a, c.a);
}

float4 TemporalResult(float4 c)
{
    return fTemporalAnySurface > 0.0 ? c : float4(c.a > 1e-4 ? c.rgb / c.a : 0.0, c.a);
}

// SSRAccumTex stores straight RGB and confidence. Premultiply each texel before
// bilinear interpolation; filtering straight RGB first mixes miss colours into
// valid hits and gives the wrong radiance at a confidence edge. GI stores plain
// values and must keep its existing hardware-filtered lookup.
float4 SSRTemporalHistory(float2 uv)
{
    if (fTemporalAnySurface > 0.0)
        return tex2Dlod(SSRAccumTex, float4(uv, 0, 0));

    float2 pixel = uv / vec2InvViewportSize - 0.5;
    float2 base = (floor(pixel) + 0.5) * vec2InvViewportSize;
    float2 f = frac(pixel);
    float2 dx = float2(vec2InvViewportSize.x, 0.0);
    float2 dy = float2(0.0, vec2InvViewportSize.y);
    float4 a = TemporalPremultiply(tex2Dlod(SSRAccumTex, float4(base, 0, 0)));
    float4 b = TemporalPremultiply(tex2Dlod(SSRAccumTex, float4(base + dx, 0, 0)));
    float4 c = TemporalPremultiply(tex2Dlod(SSRAccumTex, float4(base + dy, 0, 0)));
    float4 d = TemporalPremultiply(tex2Dlod(SSRAccumTex, float4(base + dx + dy, 0, 0)));
    return lerp(lerp(a, b, f.x), lerp(c, d, f.x), f.y);
}

// Debug view 10 (fDebugMode 10, indirect light only): what the accumulation does with each pixel,
// drawn by a second run of SSRTemporal_PS that leaves the real one alone. Red: last frame's light
// dropped because last frame's depth there shows another surface, white: it was off screen; green: kept, darker as the clamp to this frame's
// neighbourhood pulls it further; blue: how noisy this frame's light is around the pixel, its
// spread over its mean. Magenta: no history at all; yellow: a
// neighbourhood all alike, which takes this frame's value as it is.
bool GIHistoryDebug()
{
    return fTemporalAnySurface > 0.0 && fDebugMode > 9.5;
}

float4 SSRTemporal_PS(float2 uv : TEXCOORD0, float2 vPos : VPOS) : COLOR0
{
    // SSR's march marked the pixels it skipped in SSRHitDistTex as well (kNoSurface).
    float4 hitInfo = fTemporalAnySurface <= 0.0 ? tex2Dlod(SSRHitDistTex, float4(uv, 0, 0)) : 0.0;
    if (fTemporalAnySurface > 0.0 ? tex2Dlod(DepthTex, float4(uv, 0, 0)).r >= 0.9999
                                  : NoSurface(hitInfo))
        return 0.0;

    float4 current = TemporalPremultiply(tex2Dlod(SSRResultTex, float4(uv, 0, 0)));
    if (fTemporalBlend <= 0.0)
        return GIHistoryDebug() ? float4(1.0, 0.0, 1.0, 1.0) : TemporalResult(current);

    float4 m1 = current, m2 = current * current;
    float sampleWeight = 1.0;
    float neighbourMotion = 0.0, neighbourMotionWeight = 0.0;
    float centreHitDistance = hitInfo.r;
    bool stochasticMiss = fTemporalAnySurface <= 0.0 && fStochasticSSR > 0.0 && centreHitDistance == 0.0;
    float centreDepth = stochasticMiss ? LinearDepth(uv) : 0.0;
    [loop] [fastopt]
    for (int y = -1; y <= 1; ++y)
    {
        [loop] [fastopt]
        for (int x = -1; x <= 1; ++x)
        {
            if (x == 0 && y == 0)
                continue;
            float2 tapUV = uv + float2(x, y) * vec2InvViewportSize;
            float4 s = TemporalPremultiply(tex2Dlod(SSRResultTex, float4(tapUV, 0, 0)));
            float w = 1.0;
            if (fTemporalAnySurface <= 0.0)
            {
                float4 tapHit = tex2Dlod(SSRHitDistTex, float4(tapUV, 0, 0));
                float tapHitDistance = tapHit.r;
                w = SSRTemporalHitWeight(centreHitDistance, tapHitDistance, fStochasticSSR);
                // A hit across the receiver's silhouette is not evidence of a
                // stochastic miss on this surface.
                if (stochasticMiss && tapHitDistance > 0.0)
                {
                    w *= saturate(1.0 - abs(LinearDepth(tapUV) - centreDepth) / (0.02 * centreDepth + 0.01));
                    neighbourMotion += w * tapHit.g;
                    neighbourMotionWeight += w;
                }
            }
            m1 += w * s;
            m2 += w * s * s;
            sampleWeight += w;
        }
    }
    m1 /= sampleWeight;
    float4 spread = sqrt(max(m2 / sampleWeight - m1 * m1, 0.0));
    // Stochastic radiance can vary between frames even when spatial filtering made this
    // frame uniform. Apply its RGB window before the uniform-neighbourhood early out.
    // Alpha is still clamped to the neighbourhood; an entirely empty one cannot retain old hits.
    if (fTemporalAnySurface <= 0.0 && fStochasticSSR > 0.0)
        spread.rgb = max(spread.rgb, 0.15 * abs(m1.rgb));
    // A neighbourhood all alike clamps any history to the current value, as where a glossy
    // surface reflects nothing this frame.
    [branch]
    if (all(spread <= 0.0))
        return GIHistoryDebug() ? float4(1.0, 1.0, 0.0, 1.0) : TemporalResult(current);
    // Indirect light arrives smoothed, so its 3x3 neighbourhood barely spreads even where it is
    // noisy at a larger scale, and 1.5 times that spread held the history to this frame's blotches
    // (debug view 10 showed them black). The depth test already drops the history of another
    // surface; the window only has to catch light that changed, so it is at least a quarter of
    // the light either way.
    if (fTemporalAnySurface > 0.0)
        spread = max(spread, 0.25 * abs(m1));
    float4 lo = m1 - 1.5 * spread, hi = m1 + 1.5 * spread;

    float3 C = ReconstructViewPos(vPos, LinearDepth(uv));
    bool checkDepth;
    float2 prevUV = TemporalHistoryUV(uv, C, checkDepth);
    [branch]
    if (fTemporalAnySurface <= 0.0 && checkDepth)
    {
        float hitDist = hitInfo.r * fMaxDistance;
        // Last frame's depth there is the reflecting surface's again, which the test below
        // compares with this one's: off the surface, as past a car's outline, the history goes.
        if (hitDist > 0.0)
            prevUV = HistoryUV(C + normalize(C) * hitDist);
    }
    float keep = fTemporalBlend;
    if (fTemporalAnySurface <= 0.0)
        keep *= SSRTemporalMotionWeight(hitInfo.r, hitInfo.g, stochasticMiss, neighbourMotion, neighbourMotionWeight);
    bool offScreen = any(prevUV <= 0.0) || any(prevUV >= 1.0);
    if (offScreen)
        keep = 0.0;
    else if (fUsePrevDepth > 0.0 && checkDepth)
    {
        // Last frame's surface where the history is taken must be about as far as this one.
        float4 clip = C.x * vec4ViewToPrevClip[0] + C.y * vec4ViewToPrevClip[1]
                    + C.z * vec4ViewToPrevClip[2] + vec4ViewToPrevClip[3];
        float prevZ = PrevLinearDepth(prevUV);
        if (abs(prevZ - clip.w) > 0.05 * clip.w + 0.1)
            keep = 0.0;
    }

    keep = SSRStochasticMissKeep(keep, fTemporalBlend, stochasticMiss, current.a, neighbourMotionWeight);

    [branch]
    if (GIHistoryDebug())
    {
        // While the SSR trace runs (fDebugMode 10.75) the numbers instead, for it to read back: this
        // frame's view depth, last frame's where the history is taken, what last frame's should be
        // through last frame's camera, and in alpha 1 for a depth test, 2 off screen, 4 with motion.
        [branch]
        if (fDebugMode > 10.5)
        {
            float4 clip = C.x * vec4ViewToPrevClip[0] + C.y * vec4ViewToPrevClip[1]
                        + C.z * vec4ViewToPrevClip[2] + vec4ViewToPrevClip[3];
            return float4(C.z, PrevLinearDepth(prevUV), clip.w,
                          (checkDepth ? 1.0 : 0.0) + (offScreen ? 2.0 : 0.0) + (fUseMotion > 0.0 ? 4.0 : 0.0));
        }
        static const float3 kLum = float3(0.2126, 0.7152, 0.0722);
        float noise = saturate(dot(spread.rgb, kLum) / max(dot(m1.rgb, kLum), 1e-3));
        float3 h = tex2Dlod(SSRAccumTex, float4(prevUV, 0, 0)).rgb;
        float clamped = saturate(dot(abs(h - clamp(h, lo.rgb, hi.rgb)), kLum) / max(dot(h, kLum), 1e-3));
        if (offScreen)
            return float4(1.0, 1.0, 1.0, 1.0);
        return float4(keep > 0.0 ? 0.0 : 1.0, keep > 0.0 ? 1.0 - clamped : 0.0, noise, 1.0);
    }

    float4 history = clamp(SSRTemporalHistory(prevUV), lo, hi);
    return TemporalResult(lerp(current, history, keep));
}

// The average of the 4x4 full size pixels around uv, in four bilinear reads (vec2InvViewportSize
// is the half size pixel here). A ray's hit reads last frame's scene where the surface was and this
// frame's diffuse colour where it is; with the camera moving these are a fraction of a pixel apart,
// and on wallpaper stripes or cracked plaster the colour taken back out missed its own stripe, which
// showed as noise wherever temporal AA did not smooth it. Indirect light needs no finer detail.
float3 GIHitBox(sampler2D tex, float2 uv)
{
    float2 r = 0.5 * vec2InvViewportSize;
    return 0.25 * (tex2Dlod(tex, float4(uv + r, 0, 0)).rgb + tex2Dlod(tex, float4(uv - r, 0, 0)).rgb +
                   tex2Dlod(tex, float4(uv + float2(r.x, -r.y), 0, 0)).rgb +
                   tex2Dlod(tex, float4(uv + float2(-r.x, r.y), 0, 0)).rgb);
}

// One bounce of indirect light: rays spread over the hemisphere around the G-buffer normal,
// denser towards the normal (cosine weighted, so each ray counts the same), pick up last
// frame's lit scene where they hit. deferred_lighting adds the result to its ambient term
// before multiplying by albedo, so a red wall tints the white floor next to it. The game's
// ambient stands for the open sky, which a ray that hits something does not see: alpha holds
// the share of rays that hit, weighted as their light, and deferred_lighting takes that share
// (times fGIOcclusion) off the ambient, so the light of the surroundings takes its place. Added
// on top of the full ambient it washed out the contact shadows and barely showed. The history
// holds last frame's indirect light too, so light bounces on from frame to frame, at its true
// strength whatever fGIIntensity is (see fGIFeedback).
float4 SSGI_PS(float2 uv : TEXCOORD0, float2 vPos : VPOS) : COLOR0
{
    float rawDepth = tex2Dlod(DepthTex, float4(uv, 0, 0)).r;
    if (rawDepth >= 0.9999)
        return 0.0;

    float3 C = ReconstructViewPos(vPos, pow(fFarDivNear, rawDepth) * fNearPlane);
    if (C.z >= fGIMaxViewDistance)
        return 0.0;

    float3 n;
    [branch]
    if (fUseGBufferNormals > 0.0)
        n = GBufferNormal(uv);
    else
        n = ReconstructNormal(uv, C);
    n = (dot(n, C) > 0.0) ? -n : n;
    float3 up = (abs(n.z) < 0.999) ? float3(0.0, 0.0, 1.0) : float3(1.0, 0.0, 0.0);
    float3 T = normalize(cross(up, n));
    float3 B = cross(n, T);

    // Off the surface by more with distance, as depth gets coarser; otherwise rays hit the
    // surface they start from and it lights itself.
    float3 P0 = C + n * (0.05 + C.z * 0.003);
    // vec2NoiseOffset moves both on every frame while it accumulates, as for SSR.
    float jitter = PixelJitter(vPos + vec2NoiseOffset);
    float jitter2 = PixelJitter(vPos.yx + float2(17.0, 59.0) + vec2NoiseOffset.yx);
    float3 sum = 0.0;
    float hits = 0.0;

    [loop] [fastopt]
    for (int r = 0; r < GI_RAYS; ++r)
    {
        float u1 = frac(jitter + (float) r * 0.618034);
        float u2 = frac(jitter2 + (float) r * 0.7548777);
        float phi = 6.2831853 * u1;
        float sinTheta = sqrt(u2);
        float3 dir = T * (cos(phi) * sinTheta) + B * (sin(phi) * sinTheta) + n * sqrt(1.0 - u2);

        // A ray heading back towards the camera must stay in front of the near plane.
        float len = fGIRayLength;
        if (dir.z < 0.0)
            len = min(len, (P0.z - fNearPlane * 2.0) / -dir.z);
        if (len <= 0.0)
            continue;

        float prevZ = P0.z;
        [loop] [fastopt]
        for (int i = 0; i < GI_STEPS; ++i)
        {
            // Steps grow with the distance, denser next to the surface but reaching the ray's
            // end; with steps growing with its square, most fell within its first fifth, and a
            // wall got nothing from the sunlit pavement a metre or two below.
            float s = ((float) i + jitter) / (float) GI_STEPS;
            float t = s * sqrt(s);
            float3 P = P0 + dir * (len * t);
            float2 sampleUV = ViewToUV(P);
            if (any(sampleUV <= 0.0) || any(sampleUV >= 1.0))
                break;

            float delta = P.z - LinearDepth(sampleUV);
            float thickness = abs(P.z - prevZ) + fGIThickness;
            prevZ = P.z;
            if (delta > 0.0 && delta < thickness)
            {
                // Only a surface facing the ray lights it. Next to its start a ray often lands
                // behind the very surface it left, which depth precision puts in its way, and
                // that surface lit itself in its own colour; others it reaches from behind.
                // Such a sample is no hit, and the ray marches on.
                float facingRay = -1.0;
                if (fUseGBufferNormals > 0.0)
                {
                    float3 hitN = GBufferNormal(sampleUV);
                    hitN = (dot(hitN, P) > 0.0) ? -hitN : hitN;
                    facingRay = dot(hitN, dir);
                }
                if (facingRay < -0.1)
                {
                    float2 histUV = HistoryUV(P);
                    if (all(histUV > 0.0) && all(histUV < 1.0))
                    {
                        // Last frame's lighting added the surface's colour times the indirect
                        // light it got. All of that but an intensity 1 share comes back out:
                        // otherwise fGIIntensity multiplied every bounce again, and at 3 surfaces
                        // next to each other lit each other brighter every frame, up to the caps.
                        float3 L = GIHitBox(HistoryTex, histUV);
                        L -= GIHitBox(AlbedoLinearTex, sampleUV) *
                             tex2Dlod(GIPrevTex, float4(histUV, 0, 0)).rgb * fGIFeedback;
                        L = clamp(L, 0.0, HISTORY_CLAMP);
                        float lum = dot(L, float3(0.2126, 0.7152, 0.0722));
                        L *= min(1.0, fGIMaxBrightness / max(lum, 1e-4));
                        sum += L * (1.0 - t * t); // fades out towards the ray's end, not along it
                        hits += 1.0 - t * t;
                    }
                    break;
                }
            }
        }
    }

    float fade = 1.0 - smoothstep(fGIMaxViewDistance * 0.75, fGIMaxViewDistance, C.z);
    float3 gi = sum * (fGIIntensity * fade / (float) GI_RAYS);
    float occlusion = saturate(hits * fade / (float) GI_RAYS) * fGIOcclusion;
    // deferred_lighting multiplies the ambient by the occlusion in _DEFERRED_GBUFFER_2_ (the
    // game's own and SAO's or GTAO's) and then by what this leaves of it, so a corner both
    // found was darkened twice and the indirect light that should take the ambient's place
    // showed little. Only what the rays find closed in beyond that occlusion is taken off:
    // the ambient left is the smaller of the two instead of their product.
    [branch]
    if (fGIRespectAO > 0.0)
    {
        float ao = tex2Dlod(SpecularTex, float4(uv, 0, 0)).z;
        float open = 1.0 - occlusion;
        occlusion = open < ao ? 1.0 - open / max(ao, 1e-3) : 0.0;
    }
    if (any(gi != gi) || occlusion != occlusion)
        return 0.0;
    return float4(gi, occlusion);
}

// Indirect light from half to full resolution, for deferred_lighting. Of the four half size
// texels around a pixel, those whose depth is close to the pixel's weigh most, so light from
// behind an object's outline does not spill onto it as bilinear filtering let it. The share of
// the ambient it replaces (alpha) comes along. vec2InvViewportSize is the full size pixel.
float4 GIUpsample_PS(float2 uv : TEXCOORD0) : COLOR0
{
    float rawDepth = tex2Dlod(DepthTex, float4(uv, 0, 0)).r;
    if (rawDepth >= 0.9999)
        return 0.0;
    float z = pow(fFarDivNear, rawDepth) * fNearPlane;

    float2 halfTexel = vec2InvViewportSize * 2.0;
    float2 pos = uv / halfTexel - 0.5;
    float2 base = floor(pos);
    float2 f = pos - base;
    float4 sum = 0.0;
    float weightSum = 0.0;
    float4 nearest = 0.0;
    float nearestDiff = 1e30;
    static const float2 corners[4] = { float2(0.0, 0.0), float2(1.0, 0.0), float2(0.0, 1.0), float2(1.0, 1.0) };
    [unroll]
    for (int i = 0; i < 4; ++i)
    {
        float2 o = corners[i];
        float2 tuv = (base + o + 0.5) * halfTexel;
        float4 gi = tex2Dlod(SSRResultTex, float4(tuv, 0, 0));
        float diff = abs(LinearDepth(tuv) - z);
        float2 b = lerp(1.0 - f, f, o);
        float w = b.x * b.y / (1.0 + diff / (0.02 * z + 0.02));
        sum += gi * w;
        weightSum += w;
        if (diff < nearestDiff)
        {
            nearestDiff = diff;
            nearest = gi;
        }
    }
    return weightSum > 1e-3 ? sum / weightSum : nearest;
}

// Indirect light and contact shadows, both marched at half size, go through their smoothing, accumulation and
// bringing up to full size together (FusedLightingPasses): each of these passes does for both what
// SSRDenoise_PS, SSRTemporal_PS (fTemporalAnySurface 1) and GIUpsample_PS do for indirect light in SSRResultTex
// and ContactTemporal_PS and ContactUpsample_PS for contact shadows in ContactInTex, into two targets at once,
// and the depths each pass weighs by, most of what it costs, and the history's place are read once for both.
struct GIContactOutput
{
    float4 gi : COLOR0;
    float4 contact : COLOR1;
};

GIContactOutput GIContactDenoise_PS(float2 uv : TEXCOORD0)
{
    static const float2 taps[12] =
    {
        float2(-0.326, -0.406), float2( 0.326,  0.406),
        float2(-0.840, -0.074), float2( 0.840,  0.074),
        float2(-0.696,  0.457), float2( 0.696, -0.457),
        float2(-0.203,  0.621), float2( 0.203, -0.621),
        float2( 0.962, -0.195), float2(-0.962,  0.195),
        float2( 0.473, -0.480), float2(-0.473,  0.480)
    };

    GIContactOutput o;
    o.gi = tex2Dlod(SSRResultTex, float4(uv, 0, 0));
    o.contact = tex2Dlod(ContactInTex, float4(uv, 0, 0));
    float2 radius = fDenoiseRadius * vec2InvViewportSize;

    // Each comes back as it is where every tap reads what its centre does, as in SSRDenoise_PS. The taps are not
    // kept for the weighing: twelve of both took more temp registers than ps_3_0 has, and more registers run
    // fewer pixels at once; read again they come from the texture cache. Contact shadows are read for their red
    // alone, all their accumulation and upsampling take.
    bool giSame = true, contactSame = true;
    [loop]
    for (int i = 0; i < 12; ++i)
    {
        float4 tapUV = float4(uv + taps[i] * radius, 0, 0);
        giSame = giSame && all(tex2Dlod(SSRResultTex, tapUV) == o.gi);
        contactSame = contactSame && tex2Dlod(ContactInTex, tapUV).r == o.contact.r;
    }
    [branch]
    if (giSame && contactSame)
        return o;

    // SSRDenoise_PS with fDenoiseSSROnly 0: the weighted mean of all four channels
    float centreZ = LinearDepth(uv);
    float4 giSum = o.gi;
    float contactSum = o.contact.r;
    float weightSum = 1.0;
    [loop]
    for (int j = 0; j < 12; ++j)
    {
        float2 tap = uv + taps[j] * radius;
        float w = exp(-dot(taps[j], taps[j]) * 2.0);
        w *= saturate(1.0 - abs(LinearDepth(tap) - centreZ) / (centreZ * 0.02));
        giSum += w * tex2Dlod(SSRResultTex, float4(tap, 0, 0));
        contactSum += w * tex2Dlod(ContactInTex, float4(tap, 0, 0)).r;
        weightSum += w;
    }
    if (!giSame)
        o.gi = giSum / weightSum;
    if (!contactSame)
        o.contact = float4(contactSum / weightSum, 0.0, 0.0, 1.0);
    return o;
}

GIContactOutput GIContactTemporal_PS(float2 uv : TEXCOORD0, float2 vPos : VPOS)
{
    GIContactOutput o;
    float4 gi = tex2Dlod(SSRResultTex, float4(uv, 0, 0));
    float contact = tex2Dlod(ContactInTex, float4(uv, 0, 0)).r;
    o.gi = gi;
    o.contact = float4(contact, 0.0, 0.0, 1.0);
    if (tex2Dlod(DepthTex, float4(uv, 0, 0)).r >= 0.9999)
    {
        o.gi = 0.0;
        return o;
    }

    // The 3x3 neighbourhoods: indirect light's mean and spread, contact shadows' least and most
    float4 m1 = gi, m2 = gi * gi;
    float lo = contact, hi = contact;
    [unroll]
    for (int y = -1; y <= 1; ++y)
    {
        [unroll]
        for (int x = -1; x <= 1; ++x)
        {
            if (x == 0 && y == 0)
                continue;
            float4 tapUV = float4(uv + float2(x, y) * vec2InvViewportSize, 0, 0);
            float4 g = tex2Dlod(SSRResultTex, tapUV);
            m1 += g;
            m2 += g * g;
            float c = tex2Dlod(ContactInTex, tapUV).r;
            lo = min(lo, c);
            hi = max(hi, c);
        }
    }
    m1 /= 9.0;
    float4 spread = sqrt(max(m2 / 9.0 - m1 * m1, 0.0));
    // Each kept as it is with no history or a neighbourhood all alike, which clamps any history to it
    bool giNow = fTemporalBlend <= 0.0 || all(spread <= 0.0);
    bool contactNow = fContactBlend <= 0.0 || hi <= lo;
    [branch]
    if (giNow && contactNow)
        return o;
    spread = max(spread, 0.25 * abs(m1));
    float4 giLo = m1 - 1.5 * spread, giHi = m1 + 1.5 * spread;

    // Both take the history where the surface was, dropped off screen and where last frame's depth shows another
    float3 C = ReconstructViewPos(vPos, LinearDepth(uv));
    bool checkDepth;
    float2 prevUV = TemporalHistoryUV(uv, C, checkDepth);
    float valid = 1.0;
    if (any(prevUV <= 0.0) || any(prevUV >= 1.0))
        valid = 0.0;
    else if (fUsePrevDepth > 0.0 && checkDepth)
    {
        float4 clip = C.x * vec4ViewToPrevClip[0] + C.y * vec4ViewToPrevClip[1]
                    + C.z * vec4ViewToPrevClip[2] + vec4ViewToPrevClip[3];
        float prevZ = PrevLinearDepth(prevUV);
        if (abs(prevZ - clip.w) > 0.05 * clip.w + 0.1)
            valid = 0.0;
    }

    if (!giNow)
        o.gi = lerp(gi, clamp(tex2Dlod(SSRAccumTex, float4(prevUV, 0, 0)), giLo, giHi), fTemporalBlend * valid);
    if (!contactNow)
        o.contact.r = lerp(contact, clamp(tex2Dlod(ContactHistTex, float4(prevUV, 0, 0)).r, lo, hi), fContactBlend * valid);
    return o;
}

// vec2InvViewportSize is the full size pixel here. The half size grid is ContactUpsample_PS's, the targets' own
// size (half the full size, rounded down); GIUpsample_PS's, twice the full size pixel, is the same for an even width.
GIContactOutput GIContactUpsample_PS(float2 uv : TEXCOORD0)
{
    float rawDepth = tex2Dlod(DepthTex, float4(uv, 0, 0)).r;
    float z = pow(fFarDivNear, rawDepth) * fNearPlane;
    float2 halfSize = floor(0.5 / vec2InvViewportSize);
    float2 p = uv * halfSize - 0.5;
    float2 f = frac(p);
    float2 base = (floor(p) + 0.5) / halfSize;

    float4 giSum = 0.0, nearest = 0.0;
    float giWeightSum = 0.0, nearestDiff = 1e30;
    float contactSum = 0.0, contactWeightSum = 0.0;
    [unroll]
    for (int y = 0; y < 2; ++y)
    {
        [unroll]
        for (int x = 0; x < 2; ++x)
        {
            float2 tapUV = base + float2(x, y) / halfSize;
            float bilinear = (x ? f.x : 1.0 - f.x) * (y ? f.y : 1.0 - f.y);
            float diff = abs(LinearDepth(tapUV) - z);
            float4 gi = tex2Dlod(SSRResultTex, float4(tapUV, 0, 0));
            float wg = bilinear / (1.0 + diff / (0.02 * z + 0.02));
            giSum += gi * wg;
            giWeightSum += wg;
            if (diff < nearestDiff)
            {
                nearestDiff = diff;
                nearest = gi;
            }
            float wc = bilinear * (saturate(1.0 - diff / (z * 0.02)) + 1e-3);
            contactSum += wc * tex2Dlod(ContactInTex, float4(tapUV, 0, 0)).r;
            contactWeightSum += wc;
        }
    }

    GIContactOutput o;
    o.gi = rawDepth >= 0.9999 ? 0.0 : (giWeightSum > 1e-3 ? giSum / giWeightSum : nearest);
    o.contact = float4(contactSum / max(contactWeightSum, 1e-6), 0.0, 0.0, 1.0);
    return o;
}

// Light scattering under the skin (separable screen space subsurface scattering, Jimenez et
// al.): once all lights are drawn, the light on skin is blurred with the profile of skin,
// which carries red furthest and green and blue less, along x (SkinScatter_PS) and then y
// (SkinScatterFinal_PS). What is blurred is the light, the lit colour over the diffuse colour,
// which is multiplied back after, so pores, freckles and stubble stay sharp. Skin is where the
// skin shaders, and gta_ped for the HEAD and HAND components (InstallPedSkinHooks), add a quarter
// step to the material ID (shaders/patches/ped_skin_scattering_mask.patch).
// The weights (x, y, z for red, green, blue, each summing to 1) and offsets (w, in half of
// SkinScatteringWidth) are the 17 sample kernel of Jimenez's SeparableSSS for its default skin:
// strength 0.48, 0.41, 0.28, falloff 1.0, 0.37, 0.3.
static const float4 kSkinKernel[17] =
{
    float4(0.536343, 0.624624, 0.748867,  0.000000),
    float4(0.003174, 0.000135, 0.000038, -2.000000),
    float4(0.010039, 0.000915, 0.000276, -1.531250),
    float4(0.014461, 0.003173, 0.001064, -1.125000),
    float4(0.021630, 0.007946, 0.003770, -0.781250),
    float4(0.034732, 0.015109, 0.008720, -0.500000),
    float4(0.057106, 0.028743, 0.017284, -0.281250),
    float4(0.058242, 0.065996, 0.041133, -0.125000),
    float4(0.032446, 0.065672, 0.053282, -0.031250),
    float4(0.032446, 0.065672, 0.053282,  0.031250),
    float4(0.058242, 0.065996, 0.041133,  0.125000),
    float4(0.057106, 0.028743, 0.017284,  0.281250),
    float4(0.034732, 0.015109, 0.008720,  0.500000),
    float4(0.021630, 0.007946, 0.003770,  0.781250),
    float4(0.014461, 0.003173, 0.001064,  1.125000),
    float4(0.010039, 0.000915, 0.000276,  1.531250),
    float4(0.003174, 0.000135, 0.000038,  2.000000),
};

// Material IDs are whole steps of 1/255; skin's is a quarter over.
float SkinMask(float2 uv)
{
    float id = tex2Dlod(SkinIDTex, float4(uv, 0, 0)).r * 255.0;
    return abs(frac(id + 0.5) - 0.75) < 0.125 ? 1.0 : 0.0;
}

float3 SkinAlbedo(float2 uv)
{
    return max(tex2Dlod(AlbedoTex, float4(uv, 0, 0)).rgb, 0.03);
}

// The light on skin, with its view depth in alpha; 0 where there is no skin.
float4 SkinLight_PS(float2 uv : TEXCOORD0) : COLOR0
{
    if (SkinMask(uv) <= 0.0)
        return 0.0;
    return float4(tex2Dlod(SceneTex, float4(uv, 0, 0)).rgb / SkinAlbedo(uv), LinearDepth(uv));
}

// SkinLightTex blurred along vec4SkinStep.xy around centre, its own texel.
float3 SkinBlur(float2 uv, float4 centre)
{
    float2 stepUV = vec4SkinStep.xy / centre.a;
    float3 sum = centre.rgb * kSkinKernel[0].rgb;
    [unroll]
    for (int i = 1; i < 17; ++i)
    {
        float4 s = tex2Dlod(SkinLightTex, float4(uv + kSkinKernel[i].w * stepUV, 0, 0));
        // Skin keeps its light across slopes of up to about 75 degrees to the view, give or take
        // what the half float depth holds; a bigger step in depth is another surface (a nose
        // past a cheek), and there, as where it is not skin, the centre's light stands in.
        float lateral = abs(kSkinKernel[i].w) * vec4SkinStep.w;
        float away = s.a > 0.0 ? saturate(abs(s.a - centre.a) / (4.0 * lateral + 0.005 + 0.002 * centre.a) - 1.0) : 1.0;
        sum += kSkinKernel[i].rgb * lerp(s.rgb, centre.rgb, away);
    }
    return sum;
}

float4 SkinScatter_PS(float2 uv : TEXCOORD0) : COLOR0
{
    float4 centre = tex2Dlod(SkinLightTex, float4(uv, 0, 0));
    if (centre.a <= 0.0)
        return 0.0;
    return float4(SkinBlur(uv, centre), centre.a);
}

// The scene with the scattered light on skin, for the fog pass. fSkinStrength above 1 takes more
// light out of the centre than the profile does, up to 2, where red keeps 7% of its own.
float4 SkinScatterFinal_PS(float2 uv : TEXCOORD0) : COLOR0
{
    float4 scene = tex2Dlod(SceneTex, float4(uv, 0, 0));
    float4 centre = tex2Dlod(SkinLightTex, float4(uv, 0, 0));
    if (centre.a <= 0.0)
        return scene;
    return float4(lerp(scene.rgb, SkinBlur(uv, centre) * SkinAlbedo(uv), fSkinStrength), scene.a);
}

// SSR Debug 9: skin in red over the scene in grey, and yellow where the scattering changes the
// scene, full at a quarter; SkinLightTex holds the scene after it, or the scene while it is off.
float4 SkinDebug_PS(float2 uv : TEXCOORD0) : COLOR0
{
    float3 scene = tex2Dlod(SceneTex, float4(uv, 0, 0)).rgb;
    float3 result = tex2Dlod(SkinLightTex, float4(uv, 0, 0)).rgb;
    float l = dot(scene, float3(0.2126, 0.7152, 0.0722));
    float change = saturate(4.0 * dot(abs(result - scene), 1.0) / (dot(scene, 1.0) + 1e-3));
    l = l / (1.0 + l);
    float3 c = SkinMask(uv) > 0.0 ? float3(0.4 + 0.6 * l, 0.1 * l, 0.1 * l) : l.xxx;
    return float4(lerp(c, float3(1.0, 1.0, 0.0), change), 1.0);
}

float4 SSRDebugCopy_PS(float2 uv : TEXCOORD0) : COLOR0
{
    return float4(tex2Dlod(DebugTex, float4(uv, 0, 0)).rgb, 1.0);
}

void FullscreenQuadVS(in float4 iPos : POSITION, in float2 iUV : TEXCOORD0,
                      out float4 oPos : POSITION, out float2 oUV : TEXCOORD0)
{
    oPos = iPos;
    oUV = iUV;
}

technique SSR
{
    pass Trace
    {
        VertexShader = compile vs_3_0 FullscreenQuadVS();
        PixelShader = compile ps_3_0 SSRTrace_PS();
    }
    pass Fallback // SSRResultTex2D: the trace pass's target
    {
        VertexShader = compile vs_3_0 FullscreenQuadVS();
        PixelShader = compile ps_3_0 SSRFallback_PS();
    }
    pass Spread // SSRResultTex2D: the trace pass's target, SSRFallbackTex2D the last fill
    {
        VertexShader = compile vs_3_0 FullscreenQuadVS();
        PixelShader = compile ps_3_0 SSRSpread_PS();
    }
    pass Resolve // SSRResultTex2D: the trace pass's target, SSRFallbackTex2D the last fill
    {
        VertexShader = compile vs_3_0 FullscreenQuadVS();
        PixelShader = compile ps_3_0 SSR_PS();
    }
    pass MarchDepth // into a half size R32F target, before the trace
    {
        VertexShader = compile vs_3_0 FullscreenQuadVS();
        PixelShader = compile ps_3_0 MarchDepth_PS();
    }
    pass HiZDepth
    {
        VertexShader = compile vs_3_0 FullscreenQuadVS();
        PixelShader = compile ps_3_0 HiZDepth_PS();
    }
}

technique SSRWater
{
    pass P0
    {
        VertexShader = compile vs_3_0 FullscreenQuadVS();
        PixelShader = compile ps_3_0 SSRWater_PS();
    }
}

technique SSRDebug
{
    pass P0
    {
        VertexShader = compile vs_3_0 FullscreenQuadVS();
        PixelShader = compile ps_3_0 SSRDebug_PS();
    }
}

technique SSRDenoise
{
    pass P0
    {
        VertexShader = compile vs_3_0 FullscreenQuadVS();
        PixelShader = compile ps_3_0 SSRDenoise_PS();
    }
}

technique SSRTemporal
{
    pass P0
    {
        VertexShader = compile vs_3_0 FullscreenQuadVS();
        PixelShader = compile ps_3_0 SSRTemporal_PS();
    }
}

technique SSGI
{
    pass P0
    {
        VertexShader = compile vs_3_0 FullscreenQuadVS();
        PixelShader = compile ps_3_0 SSGI_PS();
    }
}

technique GIUpsample
{
    pass P0
    {
        VertexShader = compile vs_3_0 FullscreenQuadVS();
        PixelShader = compile ps_3_0 GIUpsample_PS();
    }
}

technique ContactShadows
{
    pass P0
    {
        VertexShader = compile vs_3_0 FullscreenQuadVS();
        PixelShader = compile ps_3_0 ContactShadows_PS();
    }
}

technique ContactUpsample
{
    pass P0
    {
        VertexShader = compile vs_3_0 FullscreenQuadVS();
        PixelShader = compile ps_3_0 ContactUpsample_PS();
    }
}

technique GIContactDenoise
{
    pass P0
    {
        VertexShader = compile vs_3_0 FullscreenQuadVS();
        PixelShader = compile ps_3_0 GIContactDenoise_PS();
    }
}

technique GIContactTemporal
{
    pass P0
    {
        VertexShader = compile vs_3_0 FullscreenQuadVS();
        PixelShader = compile ps_3_0 GIContactTemporal_PS();
    }
}

technique GIContactUpsample
{
    pass P0
    {
        VertexShader = compile vs_3_0 FullscreenQuadVS();
        PixelShader = compile ps_3_0 GIContactUpsample_PS();
    }
}

technique ContactTemporal
{
    pass P0
    {
        VertexShader = compile vs_3_0 FullscreenQuadVS();
        PixelShader = compile ps_3_0 ContactTemporal_PS();
    }
}

technique SkinLight
{
    pass P0
    {
        VertexShader = compile vs_3_0 FullscreenQuadVS();
        PixelShader = compile ps_3_0 SkinLight_PS();
    }
}

technique SkinScatter
{
    pass P0
    {
        VertexShader = compile vs_3_0 FullscreenQuadVS();
        PixelShader = compile ps_3_0 SkinScatter_PS();
    }
}

technique SkinScatterFinal
{
    pass P0
    {
        VertexShader = compile vs_3_0 FullscreenQuadVS();
        PixelShader = compile ps_3_0 SkinScatterFinal_PS();
    }
}

technique SkinDebug
{
    pass P0
    {
        VertexShader = compile vs_3_0 FullscreenQuadVS();
        PixelShader = compile ps_3_0 SkinDebug_PS();
    }
}

technique SSRDebugCopy
{
    pass P0
    {
        VertexShader = compile vs_3_0 FullscreenQuadVS();
        PixelShader = compile ps_3_0 SSRDebugCopy_PS();
    }
}
