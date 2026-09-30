texture DepthTex2D, HistoryTex2D, SpecularTex2D, SurfaceTex2D, NormalTex2D, SSRResultTex2D, DebugTex2D;
texture PreWaterTex2D, PostWaterTex2D;
texture PrevDepthTex2D;
texture SSRAccumTex2D, SSRHitTex2D;

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

// This frame's reflected ray lengths, 0 for a miss, see SSR_PS.
sampler2D SSRHitTex
{
    Texture = <SSRHitTex2D>;
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
uniform float fTowardCamera;      // 0..1, how far reflections pointing back at the camera reach
uniform float fReflectionBlur;    // blur radius in pixels a reflection reaches at fMaxDistance, 0 keeps it sharp
uniform float fDistanceFade;      // reflections fade out towards this distance from the surface, 0 disables
uniform float fTemporalBlend;     // share of last frame's SSR kept each frame, 0 while there is none to keep
uniform float fJitterOffset;      // added to each pixel's step offset, changed every frame while SSR accumulates
uniform float fTemporalFollowImage; // 1 takes the history where the reflected image was, 0 where the surface was
uniform float fTemporalDebug;     // SSR debug modes 8 to 10 as 1 to 3, see SSRTemporalDebug, else 0
uniform float4 vec4CameraPos;     // camera position in world space, for SSRTemporalDebug

// Contact shadows, see ContactShadows_PS.
uniform float4 vec4SunView;         // direction towards the sun in reconstruction space, w 0 if unknown
uniform float fCSLength;            // world units a contact shadow ray travels
uniform float fCSThickness;         // how deep behind the scene a sample may land and still occlude
uniform float fCSMaxThickness;      // deeper than that, up to this, it occludes only if the ray stays behind the scene
uniform float fCSMaxViewDistance;   // contact shadows fade out towards this view distance
uniform float fCSIntensity;         // strength, 0..1

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

// PixelJitter moved on by fJitterOffset: while SSR accumulates, each frame's steps land
// elsewhere and the accumulation averages the noise out instead of freezing it on screen.
float SSRJitter(float2 pixel)
{
    return 1.0 - frac(1.0 - PixelJitter(pixel) + fJitterOffset);
}

// jitter in (0, 1] shifts every step of this pixel's ray by up to one step.
// distanceFade: reflections fade out towards this distance from the surface, 0 disables.
// hitDist: length of the reflected ray to what it hit, 0 for a miss.
float4 TraceReflection(float3 C, float3 n, float blurPixels, float jitter, float distanceFade, out float hitDist)
{
    hitDist = 0.0;
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
    hitDist = rayLen;

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
float SSRSurfaceWeight(float2 uv)
{
    if (tex2Dlod(DepthTex, float4(uv, 0, 0)).r >= 0.9999)
        return 0.0; // sky
    if (fGlossCutoff < 0.0)
        return 1.0; // no specular G-buffer bound
    float3 spec = saturate(tex2Dlod(SpecularTex, float4(uv, 0, 0)).xyz);
    if (min(spec.x, spec.z) < 0.5 / 255.0)
        return 0.0;
    float gloss = spec.y;
    return smoothstep(fGlossCutoff, fGlossCutoff + 0.2, gloss) * (1.0 + fGlossBoost * gloss);
}

// COLOR1 holds the reflected ray's length for SSRTemporal_PS, 0 for a miss.
struct SSROutput
{
    float4 colour : COLOR0;
    float4 hit : COLOR1;
};

SSROutput SSR_PS(float2 uv : TEXCOORD0, float2 vPos : VPOS)
{
    SSROutput o;
    o.colour = 0.0;
    o.hit = 0.0;
    float surfaceWeight = SSRSurfaceWeight(uv);
    if (surfaceWeight <= 0.0)
        return o;

    float3 C = ReconstructViewPos(vPos, LinearDepth(uv));

    float3 n;
    [branch]
    if (fUseGBufferNormals > 0.0)
        n = GBufferNormal(uv);
    else
        n = ReconstructNormal(uv, C);
    n = (dot(n, C) > 0.0) ? -n : n;

    float hitDist;
    float4 r = TraceReflection(C, n, fReflectionBlur, fStepJitter > 0.0 ? SSRJitter(vPos) : 1.0, fDistanceFade, hitDist);
    o.colour = float4(r.rgb, saturate(r.a * surfaceWeight * fIntensity));
    o.hit = float4(o.colour.a > 0.0 ? hitDist : 0.0, 0.0, 0.0, 1.0);
    return o;
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

    float hitDist;
    float4 r = TraceReflection(C, n, fWaterBlur, 1.0, 0.0, hitDist);

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
//   5: _DEFERRED_GBUFFER_2_ as stored: red specular intensity, green gloss, blue the
//      reflection strength deferred_lighting uses; see SSRSurfaceWeight
//   8-10: what the accumulation pass wrote instead of reflections, see SSRTemporalDebug

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

    if (fDebugMode > 7.5)
        return float4(ssr.rgb * ssr.a, 1.0);

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
// weighted by depth so a bonnet does not pick up the road behind it.
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
    float centreZ = LinearDepth(uv);
    float2 radius = fDenoiseRadius * vec2InvViewportSize;

    float4 sum = float4(centre.rgb * centre.a, centre.a);
    float weightSum = 1.0;

    [unroll]
    for (int i = 0; i < 12; ++i)
    {
        float2 tapUV = uv + taps[i] * radius;
        float4 s = tex2Dlod(SSRResultTex, float4(tapUV, 0, 0));
        float w = exp(-dot(taps[i], taps[i]) * 2.0);
        w *= saturate(1.0 - abs(LinearDepth(tapUV) - centreZ) / (centreZ * 0.02));
        sum += w * float4(s.rgb * s.a, s.a);
        weightSum += w;
    }

    float a = sum.a / weightSum;
    float3 colour = sum.a > 1e-4 ? sum.rgb / sum.a : centre.rgb;
    return float4(colour, a);
}

// Contact shadows: a short ray from each pixel towards the sun through the depth buffer. The
// game's sun shadow map is too coarse for the contact between a ped's feet or a car's tyres
// and the ground; this fills that in. The result is occlusion (0 lit, 1 shadowed), so an
// unbound sampler in deferred_lighting changes nothing. No temporal accumulation: history
// reprojected for the camera trailed behind moving peds, so the result is smoothed spatially.
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

    float jitter = fStepJitter > 0.0 ? PixelJitter(vPos) : 1.0;
    float occlusion = 0.0;
    float prevZ = P0.z;
    // A sample deeper behind the scene than fCSThickness may be inside something thick, such
    // as a car, or merely behind something thin, such as a ped's leg a metre in front of the
    // ground. Past a leg the ray soon comes out in front of the scene again, past a car it
    // does not, so such a sample occludes only if no later sample of the ray is in front.
    float deepT = -1.0;

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
            deepT = -1.0;
            break;
        }
        if (delta >= thickness && delta < fCSMaxThickness + thickness)
        {
            if (deepT < 0.0)
                deepT = t;
        }
        else if (deepT >= 0.0)
        {
            // In front of the scene again, or behind something much nearer: it went past.
            deepT = -2.0;
        }
    }
    if (deepT >= 0.0)
        occlusion = 1.0 - deepT * deepT;

    float fade = 1.0 - smoothstep(fCSMaxViewDistance * 0.75, fCSMaxViewDistance, C.z);
    // With the sun grazing the surface the ray runs along it and any seam blocks it; the game's
    // own lighting already darkens such surfaces, so contact shadows fade out there.
    fade *= saturate(dot(n, L) * 5.0);
    return float4(saturate(occlusion * fade * fCSIntensity), 0.0, 0.0, 1.0);
}

// SSR debug modes 8 to 10 (fTemporalDebug 1 to 3), what SSRTemporal_PS writes instead of
// reflections, shown as is. They tell apart why reflections slide while the camera moves.
//   1: a checkerboard of 1 m cells fixed to the world, kept over frames as reflections are,
//      taken where the surface was last frame. It stays sharp while the camera moves only if
//      vec4ViewToPrevClip finds last frame's spot; if not, it smears along the motion. Around
//      outlines it leaves ghosts either way, as nothing rejects the history there.
//   2: the same checkerboard, not kept over frames, to compare with.
//   3: the reflected ray lengths this pass reads (SSRHitTex): green, brighter the longer, up
//      to fMaxDistance; dark red a glossy pixel whose length is 0, as for a miss.
float4 SSRTemporalDebug(float2 uv, float2 vPos)
{
    if (tex2Dlod(DepthTex, float4(uv, 0, 0)).r >= 0.9999)
        return 0.0;

    if (fTemporalDebug > 2.5)
    {
        if (SSRSurfaceWeight(uv) <= 0.0)
            return float4(0.0, 0.0, 0.0, 1.0);
        float hitDist = tex2Dlod(SSRHitTex, float4(uv, 0, 0)).r;
        if (hitDist <= 0.0)
            return float4(0.6, 0.0, 0.0, 1.0);
        return float4(0.0, 0.2 + 0.8 * saturate(hitDist / fMaxDistance), 0.0, 1.0);
    }

    float3 C = ReconstructViewPos(vPos, LinearDepth(uv));
    float3 cell = floor(ViewToWorld(C) + vec4CameraPos.xyz);
    float3 current = frac((cell.x + cell.y + cell.z) * 0.5) > 0.25 ? 0.9 : 0.1;
    float2 prevUV = HistoryUV(C);
    if (fTemporalDebug > 1.5 || fTemporalBlend <= 0.0 || any(prevUV <= 0.0) || any(prevUV >= 1.0))
        return float4(current, 1.0);
    float3 history = tex2Dlod(SSRAccumTex, float4(prevUV, 0, 0)).rgb;
    return float4(lerp(current, history, fTemporalBlend), 1.0);
}

// Blends this frame's SSR (SSRResultTex, after smoothing) with last frame's accumulation
// (SSRAccumTex). A reflection moves like the mirror image of what it shows, which lies behind
// the surface along the view ray, as far behind it as the reflected ray was long (SSRHitTex):
// the history is taken where that image was last frame. Taking it where the surface was left
// reflections trailing off car paint while the camera moved. The history is clamped to the
// mean of this frame's 3x3 neighbourhood give or take 1.5 times its spread, so it cannot bring
// back what is no longer there, and is dropped where last frame's depth shows another surface.
// Blending is premultiplied: a miss (alpha 0) must fade a reflection out, not darken its colour.
float4 SSRTemporal_PS(float2 uv : TEXCOORD0, float2 vPos : VPOS) : COLOR0
{
    if (fTemporalDebug > 0.0)
        return SSRTemporalDebug(uv, vPos);

    if (SSRSurfaceWeight(uv) <= 0.0)
        return 0.0;

    float4 current = tex2Dlod(SSRResultTex, float4(uv, 0, 0));
    current.rgb *= current.a;
    if (fTemporalBlend <= 0.0)
        return float4(current.a > 1e-4 ? current.rgb / current.a : 0.0, current.a);

    float4 m1 = current, m2 = current * current;
    [unroll]
    for (int y = -1; y <= 1; ++y)
    {
        [unroll]
        for (int x = -1; x <= 1; ++x)
        {
            if (x == 0 && y == 0)
                continue;
            float4 s = tex2Dlod(SSRResultTex, float4(uv + float2(x, y) * vec2InvViewportSize, 0, 0));
            s.rgb *= s.a;
            m1 += s;
            m2 += s * s;
        }
    }
    m1 /= 9.0;
    float4 spread = sqrt(max(m2 / 9.0 - m1 * m1, 0.0));
    float4 lo = m1 - 1.5 * spread, hi = m1 + 1.5 * spread;

    float3 C = ReconstructViewPos(vPos, LinearDepth(uv));
    float hitDist = tex2Dlod(SSRHitTex, float4(uv, 0, 0)).r * fTemporalFollowImage;
    float2 prevUV = HistoryUV(C + normalize(C) * hitDist);
    float keep = fTemporalBlend;
    if (any(prevUV <= 0.0) || any(prevUV >= 1.0))
        keep = 0.0;
    else if (fUsePrevDepth > 0.0)
    {
        // Last frame's surface where the history is taken must be about as far as this one.
        // Following the image it is a neighbouring spot of the surface, not this one, so the
        // margin is wider there.
        float4 clip = C.x * vec4ViewToPrevClip[0] + C.y * vec4ViewToPrevClip[1]
                    + C.z * vec4ViewToPrevClip[2] + vec4ViewToPrevClip[3];
        float prevZ = pow(fFarDivNear, tex2Dlod(PrevDepthTex, float4(prevUV, 0, 0)).r) * fNearPlane;
        float margin = hitDist > 0.0 ? 0.15 * clip.w + 0.3 : 0.05 * clip.w + 0.1;
        if (abs(prevZ - clip.w) > margin)
            keep = 0.0;
    }

    float4 history = tex2Dlod(SSRAccumTex, float4(prevUV, 0, 0));
    history.rgb *= history.a;
    history = clamp(history, lo, hi);

    float4 result = lerp(current, history, keep);
    return float4(result.a > 1e-4 ? result.rgb / result.a : 0.0, result.a);
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

technique ContactShadows
{
    pass P0
    {
        VertexShader = compile vs_3_0 FullscreenQuadVS();
        PixelShader = compile ps_3_0 ContactShadows_PS();
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
