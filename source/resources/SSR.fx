texture DepthTex2D, HistoryTex2D, SpecularTex2D, SurfaceTex2D, NormalTex2D, SSRResultTex2D, DebugTex2D;
texture PreWaterTex2D, PostWaterTex2D;
texture PrevDepthTex2D;
texture SSRAccumTex2D;
texture AlbedoTex2D, GIPrevTex2D;
texture SceneTex2D, SkinIDTex2D, SkinLightTex2D;

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
    MinFilter = POINT;
    MagFilter = POINT;
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

sampler2D SSRResultTex
{
    Texture = <SSRResultTex2D>;
};

sampler2D DebugTex
{
    Texture = <DebugTex2D>;
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
uniform float fNearPlane;
uniform float fFarDivNear;
uniform float4 vec4ProjInfo;

uniform float4 vec4ViewToPrevClip[4];
uniform float fUsePrevDepth; // 1 when PrevDepthTex holds the depth HistoryTex was taken with

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
uniform float fUseWaterMask;        // 1 limits the water pass to pixels the game drew water on

uniform float4 vec4WaterToView[3];
uniform float4 vec4WaterWorldX;
uniform float4 vec4WaterWorldY;

uniform float fDebugMode; // SSR debug view from the graphics menu, see SSRDebug_PS
uniform float fUseGBufferNormals; // 1 reads the G-buffer normal, 0 rebuilds it from depth
uniform float fDenoiseRadius;     // SSR smoothing radius in pixels, see SSRDenoise_PS
uniform float fDenoiseSSROnly;    // 1 while smoothing SSR, 0 while smoothing contact shadows
uniform float fPassThinObjects;   // 1 lets a ray that went far behind an object carry on
uniform float fStepJitter;        // 1 shifts each pixel's steps by up to one step (set per pass: SSR and contact shadows each have their own switch)
uniform float fJitterOffset;      // added to each pixel's step offset (set per pass: SSR and contact shadows), changed every frame while they accumulate
uniform float fTowardCamera;      // 0..1, how far reflections pointing back at the camera reach
uniform float fReflectionBlur;    // blur radius in pixels a reflection reaches at fMaxDistance, 0 keeps it sharp
uniform float fDistanceFade;      // reflections fade out towards this distance from the surface, 0 disables
uniform float fTemporalBlend;     // share of last frame's SSR kept each frame, 0 while there is none to keep
uniform float fTemporalAnySurface; // 1 while accumulating indirect light, which every surface gets, not only glossy ones

// Contact shadows, see ContactShadows_PS.
uniform float4 vec4SunView;         // direction towards the sun in reconstruction space, w 0 if unknown
uniform float fCSLength;            // world units a contact shadow ray travels
uniform float fCSThickness;         // how deep behind the scene a sample may land and still occlude
uniform float fCSMaxViewDistance;   // contact shadows fade out towards this view distance
uniform float fCSIntensity;         // strength, 0..1

// Screen space indirect light, see SSGI_PS.
uniform float fGIRayLength;         // world units an indirect light ray travels
uniform float fGIThickness;         // how deep behind the scene a sample may land and still be a hit
uniform float fGIMaxViewDistance;   // indirect light fades out towards this view distance
uniform float fGIIntensity;         // multiplier on the light gathered
uniform float fGIMaxBrightness;     // brightness a single hit may bring, so a headlight or neon sign does not flare
uniform float fGIFeedback;          // share of last frame's indirect light a hit takes back out, 0 while there is none
uniform float fGIOcclusion;         // 0..1, how much of the ambient the indirect light takes the place of where its rays hit

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

    float2 r = radiusPixels * vec2InvViewportSize;

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

// jitter in (0, 1] shifts every step of this pixel's ray by up to one step.
// distanceFade: reflections fade out towards this distance from the surface, 0 disables.
float4 TraceReflection(float3 C, float3 n, float blurPixels, float jitter, float distanceFade)
{
    float z = C.z;
    float3 V = normalize(C);
    float3 R = reflect(V, n);

    // Rays reflected back towards the camera see the side of things the screen does not show:
    // a door with a ped crouching between it and the camera should show his front, the screen
    // holds his back, and the hit jumps between his back, his outline and what is behind him.
    // At fTowardCamera 0 they fade from a cosine of 0.25 between view and reflection and are
    // gone at -0.25, which keeps the ped out of the door; at 1 they keep full weight down to
    // -0.5 and fade by -0.8, which lets a roof seen steeply from above (about -0.5) reflect.
    float cosVR = dot(V, R);
    float facing = lerp(saturate(cosVR * 2.0 + 0.5), saturate((cosVR + 0.8) / 0.3), fTowardCamera);
    if (facing <= 0.0)
        return 0.0;

    float3 P0 = C + n * max(fMaxDistance / (float) NUM_STEPS * 0.1, z * 0.01);

    // With the camera looking down at a roof or bonnet more steeply than about 45 degrees,
    // the reflected ray heads up the screen towards what stands behind the car, but its view
    // depth shrinks. Those rays are traced too, stopping in front of the near plane so both
    // ends stay projectable; dropping them made reflections vanish as the camera tilted down.
    float len = fMaxDistance;
    if (R.z < 0.0)
        len = min(len, (P0.z - fNearPlane * 2.0) / -R.z);
    if (len <= 0.0)
        return 0.0;
    float3 P1 = P0 + R * len;

    float2 uv0 = ViewToUV(P0);
    float2 uv1 = ViewToUV(P1);
    float invZ0 = 1.0 / P0.z;
    float invZ1 = 1.0 / P1.z;

    float2 dUV = uv1 - uv0;
    float2 tEdge = (step(0.0, dUV) - uv0) / (abs(dUV) < 1e-5 ? 1e-5 : dUV);
    float tEnd = clamp(min(tEdge.x, tEdge.y), 0.0, 1.0);

    // A ray short on screen needs fewer steps: a car far away reflects over a few dozen pixels,
    // and all NUM_STEPS there sampled each pixel several times. About one step per two pixels
    // of the ray keeps the last and longest step, twice the average, within a few pixels.
    float rayPixels = length(dUV * tEnd / vec2InvViewportSize);
    float steps = clamp(ceil(rayPixels * 0.5), 12.0, (float) NUM_STEPS);

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

    // One loop, no nested refinement inside it: D3DX compiles this effect while the game
    // loads, and an unrolled refinement inside the march made it take long enough to look
    // like a hang.
    [loop]
    for (int i = 0; i < NUM_STEPS; ++i)
    {
        if ((float) i >= steps)
            break;
        float s = ((float) i + jitter) / steps;
        float t = tEnd * s * s;

        float2 sampleUV = lerp(uv0, uv1, t);
        float rayZ = 1.0 / lerp(invZ0, invZ1, t);

        float delta = rayZ - LinearDepth(sampleUV);

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
            float crossDelta = zc - LinearDepth(lerp(uv0, uv1, tc));
            float crossThickness = abs(rayZ - 1.0 / lerp(invZ0, invZ1, prevT)) + fThickness;
            if (crossDelta <= crossThickness)
            {
                hit = 1.0;
                hitLo = prevT;
                hitHi = t;
                break;
            }
            if (fPassThinObjects <= 0.0)
                break;
        }

        prevT = t;
        prevDelta = delta;
    }

    if (hit <= 0.0)
        return 0.0;

    // Binary refinement between the last sample in front of the scene and the first behind it.
    float lo = hitLo;
    float hi = hitHi;
    [unroll]
    for (int j = 0; j < NUM_REFINE_STEPS; ++j)
    {
        float mid = (lo + hi) * 0.5;
        float midZ = 1.0 / lerp(invZ0, invZ1, mid);
        if (midZ - LinearDepth(lerp(uv0, uv1, mid)) > 0.0)
            hi = mid;
        else
            lo = mid;
    }

    float2 finalUV = lerp(uv0, uv1, hi);
    float hitZ = 1.0 / lerp(invZ0, invZ1, hi);
    float hitDelta = max(hitZ - LinearDepth(finalUV), 0.0);
    float hitThickness = abs(hitZ - 1.0 / lerp(invZ0, invZ1, lo)) + fThickness;
    float3 hitP = ViewPosFromUVZ(finalUV, hitZ);

    float2 histUV = HistoryUV(hitP);

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

    // The colour comes from the history, the hit from this frame's depth. Next to an outline
    // the history pixel can belong to what is behind, a white roof behind a ped's legs, and
    // his reflection got a bright rim. The depth copied with the history tells them apart:
    // clip.w is the hit's view depth in the history's camera.
    [branch]
    if (fUsePrevDepth > 0.0)
    {
        float prevHitZ = dot(float4(hitP, 1.0), float4(vec4ViewToPrevClip[0].w, vec4ViewToPrevClip[1].w,
                                                       vec4ViewToPrevClip[2].w, vec4ViewToPrevClip[3].w));
        float prevZ = pow(fFarDivNear, tex2Dlod(PrevDepthTex, float4(histUV, 0, 0)).r) * fNearPlane;
        confidence *= 1.0 - smoothstep(hitThickness * 0.75, hitThickness, abs(prevZ - prevHitZ));
    }

    float3 colour = SampleHistoryBlurred(histUV, blurPixels * saturate(rayLen / fMaxDistance));

    if (any(colour != colour))
        return 0.0;

    return float4(colour, confidence);
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
        float wet = fWetness * smoothstep(0.75, 0.9, GBufferNormalWorld(uv).z);
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

float4 SSR_PS(float2 uv : TEXCOORD0, float2 vPos : VPOS) : COLOR0
{
    float wetOnly;
    float surfaceWeight = SSRSurfaceWeight(uv, wetOnly);
    if (surfaceWeight <= 0.0)
        return 0.0;

    float3 C = ReconstructViewPos(vPos, LinearDepth(uv));

    float3 n;
    [branch]
    if (fUseGBufferNormals > 0.0)
        n = GBufferNormal(uv);
    else
        n = ReconstructNormal(uv, C);
    n = (dot(n, C) > 0.0) ? -n : n;

    // While accumulating, fJitterOffset moves every pixel's steps on each frame, so the
    // accumulation averages the steps out and fewer of them do.
    float jitter = fStepJitter > 0.0 ? 1.0 - frac(1.0 - PixelJitter(vPos) + fJitterOffset) : 1.0;
    float4 r = TraceReflection(C, n, fReflectionBlur, jitter, fDistanceFade);
    // On wet ground the game's reflection strength, which deferred_lighting multiplies SSR by,
    // is that of dry asphalt, so the reflection is drawn brighter there.
    return float4(r.rgb * (1.0 + (fWetGroundBoost - 1.0) * wetOnly), saturate(r.a * surfaceWeight * fIntensity));
}

float3 WaterNormal(float2 worldXY, float distSq)
{
    float near = max(1.0 - distSq * 0.0004, 0.0);

    float2 slope = (tex2D(SurfaceTex, worldXY * 0.002).zw - 0.5) * 0.0512 * (1.0 - near);
    slope += (tex2D(SurfaceTex, worldXY * 0.01).zw - 0.5) * 1.024;
    slope += (tex2D(SurfaceTex, worldXY * 0.0454545468).zw - 0.5) * 0.465454549 * near;

    float3 nWorld = normalize(float3(slope * fWaterNormalStrength, 1.0));

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

    float4 r = TraceReflection(C, n, fWaterBlur, 1.0, 0.0);

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

    if (fDebugMode < 3.5)
    {
        if (sky)
            return float4(0.0, 0.0, 0.0, 1.0);
        float3 n;
        if (uv.x < 0.5)
            n = ReconstructNormal(uv, C);
        else
            n = GBufferNormal(uv);
        n = (dot(n, C) > 0.0) ? -n : n;
        if (abs(uv.x - 0.5) < vec2InvViewportSize.x)
            return float4(1.0, 1.0, 1.0, 1.0);
        return float4(n * 0.5 + 0.5, 1.0);
    }

    if (sky || ssr.a <= 0.0)
        return float4(0.0, 0.0, 0.0, 1.0);
    float3 V = normalize(ViewToWorld(C));
    float3 R = reflect(V, GBufferNormalWorld(uv));
    float fade = saturate(R.z * 5.0); // deferred_lighting's own horizon fade
    return float4(ssr.a * (1.0 - fade), ssr.a * fade, 0.0, 1.0);
}

// Each pixel decides on its own whether its ray hit and where, so neighbours on a car panel
// pick slightly different points and the reflection looks grainy. A small depth aware blur,
// in premultiplied form so misses (alpha 0) neither darken the colour nor bleed a halo, and
// weighted by depth so a bonnet does not pick up the road behind it. Contact shadows and
// indirect light (fDenoiseSSROnly 0) are plain values, alpha included: indirect light keeps
// the share of the ambient it replaces there.
float4 SSRDenoise_PS(float2 uv : TEXCOORD0) : COLOR0
{
    static const float2 taps[12] =
    {
        float2(-0.326, -0.406), float2(-0.840, -0.074), float2(-0.696,  0.457),
        float2(-0.203,  0.621), float2( 0.962, -0.195), float2( 0.473, -0.480),
        float2( 0.519,  0.767), float2( 0.185, -0.893), float2( 0.507,  0.064),
        float2( 0.896,  0.412), float2(-0.322, -0.933), float2(-0.792, -0.598)
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
    float4 s[12];
    bool same = true;
    [unroll]
    for (int i = 0; i < 12; ++i)
    {
        s[i] = tex2Dlod(SSRResultTex, float4(uv + taps[i] * radius, 0, 0));
        same = same && all(s[i] == centre);
    }
    [branch]
    if (same)
        return centre;

    float centreZ = LinearDepth(uv);
    float colourWeight = fDenoiseSSROnly > 0.0 ? centre.a : 1.0;
    float4 sum = float4(centre.rgb * colourWeight, colourWeight);
    float alphaSum = centre.a;
    float weightSum = 1.0;

    [unroll]
    for (int j = 0; j < 12; ++j)
    {
        float w = exp(-dot(taps[j], taps[j]) * 2.0);
        w *= saturate(1.0 - abs(LinearDepth(uv + taps[j] * radius) - centreZ) / (centreZ * 0.02));
        colourWeight = w * (fDenoiseSSROnly > 0.0 ? s[j].a : 1.0);
        sum += float4(s[j].rgb * colourWeight, colourWeight);
        alphaSum += w * s[j].a;
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

    // While accumulating, fJitterOffset moves every pixel's steps on each frame, so the
    // accumulation averages the steps out instead of keeping one frame's noise.
    float jitter = fStepJitter > 0.0 ? 1.0 - frac(1.0 - PixelJitter(vPos) + fJitterOffset) : 1.0;
    float occlusion = 0.0;
    float prevZ = P0.z;

    [loop]
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
    float2 prevUV = HistoryUV(C);
    float keep = fTemporalBlend;
    if (any(prevUV <= 0.0) || any(prevUV >= 1.0))
        keep = 0.0;
    else if (fUsePrevDepth > 0.0)
    {
        float4 clip = C.x * vec4ViewToPrevClip[0] + C.y * vec4ViewToPrevClip[1]
                    + C.z * vec4ViewToPrevClip[2] + vec4ViewToPrevClip[3];
        float prevZ = pow(fFarDivNear, tex2Dlod(PrevDepthTex, float4(prevUV, 0, 0)).r) * fNearPlane;
        if (abs(prevZ - clip.w) > 0.05 * clip.w + 0.1)
            keep = 0.0;
    }

    float history = clamp(tex2Dlod(SSRAccumTex, float4(prevUV, 0, 0)).r, lo, hi);
    return float4(lerp(current, history, keep), 0.0, 0.0, 1.0);
}

// Blends this frame's SSR (SSRResultTex, after smoothing) with last frame's accumulation
// (SSRAccumTex), taken where the surface was last frame. Taking it where the reflected image
// was, which is right for a flat mirror, looked no different on car paint or van sides and
// cost a second render target the SSR pass wrote at full size. The history is clamped to the
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

float4 SSRTemporal_PS(float2 uv : TEXCOORD0, float2 vPos : VPOS) : COLOR0
{
    if (fTemporalAnySurface > 0.0 ? tex2Dlod(DepthTex, float4(uv, 0, 0)).r >= 0.9999 : SSRSurfaceWeight(uv) <= 0.0)
        return 0.0;

    float4 current = TemporalPremultiply(tex2Dlod(SSRResultTex, float4(uv, 0, 0)));
    if (fTemporalBlend <= 0.0)
        return TemporalResult(current);

    float4 m1 = current, m2 = current * current;
    [unroll]
    for (int y = -1; y <= 1; ++y)
    {
        [unroll]
        for (int x = -1; x <= 1; ++x)
        {
            if (x == 0 && y == 0)
                continue;
            float4 s = TemporalPremultiply(tex2Dlod(SSRResultTex, float4(uv + float2(x, y) * vec2InvViewportSize, 0, 0)));
            m1 += s;
            m2 += s * s;
        }
    }
    m1 /= 9.0;
    float4 spread = sqrt(max(m2 / 9.0 - m1 * m1, 0.0));
    // A neighbourhood all alike clamps any history to the current value, as where a glossy
    // surface reflects nothing this frame.
    [branch]
    if (all(spread <= 0.0))
        return TemporalResult(current);
    float4 lo = m1 - 1.5 * spread, hi = m1 + 1.5 * spread;

    float3 C = ReconstructViewPos(vPos, LinearDepth(uv));
    float2 prevUV = HistoryUV(C);
    float keep = fTemporalBlend;
    if (any(prevUV <= 0.0) || any(prevUV >= 1.0))
        keep = 0.0;
    else if (fUsePrevDepth > 0.0)
    {
        // Last frame's surface where the history is taken must be about as far as this one.
        float4 clip = C.x * vec4ViewToPrevClip[0] + C.y * vec4ViewToPrevClip[1]
                    + C.z * vec4ViewToPrevClip[2] + vec4ViewToPrevClip[3];
        float prevZ = pow(fFarDivNear, tex2Dlod(PrevDepthTex, float4(prevUV, 0, 0)).r) * fNearPlane;
        if (abs(prevZ - clip.w) > 0.05 * clip.w + 0.1)
            keep = 0.0;
    }

    float4 history = clamp(TemporalPremultiply(tex2Dlod(SSRAccumTex, float4(prevUV, 0, 0))), lo, hi);
    return TemporalResult(lerp(current, history, keep));
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
    // fJitterOffset moves both on every frame while it accumulates, as for SSR.
    float jitter = 1.0 - frac(1.0 - PixelJitter(vPos) + fJitterOffset);
    float jitter2 = 1.0 - frac(1.0 - PixelJitter(vPos.yx + float2(17.0, 59.0)) + fJitterOffset);
    float3 sum = 0.0;
    float hits = 0.0;

    [loop]
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
        [loop]
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
                        float3 L = tex2Dlod(HistoryTex, float4(histUV, 0, 0)).rgb;
                        L -= tex2Dlod(AlbedoTex, float4(sampleUV, 0, 0)).rgb *
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
    pass P0
    {
        VertexShader = compile vs_3_0 FullscreenQuadVS();
        PixelShader = compile ps_3_0 SSR_PS();
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
