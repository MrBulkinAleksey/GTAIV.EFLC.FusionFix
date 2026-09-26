// Screen space lighting: reflections, contact shadows, indirect light (SSGI), plus the
// linear depth copy and temporal resolve they share. All passes run before deferred
// lighting; deferred_lighting samples the results (s3 reflections, s7 contact shadows,
// s8 indirect light).

texture DepthTex2D, HistoryTex2D, SpecularTex2D, SurfaceTex2D, NormalTex2D, PrevSSRTex2D;
texture PrevDepthTex2D, CurTex2D, PrevTex2D;
texture PreWaterTex2D, PostWaterTex2D;

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

sampler2D SpecularTex
{
    Texture = <SpecularTex2D>;
};

sampler2D NormalTex
{
    Texture = <NormalTex2D>;
};

sampler2D PrevSSRTex
{
    Texture = <PrevSSRTex2D>;
    MinFilter = LINEAR;
    MagFilter = LINEAR;
};

// Linear view depth of the previous frame, used to reject reprojected history that
// belonged to a different surface.
sampler2D PrevDepthTex
{
    Texture = <PrevDepthTex2D>;
    AddressU = Clamp;
    AddressV = Clamp;
    MinFilter = POINT;
    MagFilter = POINT;
    MipFilter = NONE;
};

sampler2D CurTex
{
    Texture = <CurTex2D>;
    AddressU = Clamp;
    AddressV = Clamp;
    MinFilter = LINEAR;
    MagFilter = LINEAR;
    MipFilter = NONE;
};

sampler2D PrevTex
{
    Texture = <PrevTex2D>;
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

uniform float4 vec4WaterToView[3]; // world to view rotation, also used for G-buffer normals
uniform float4 vec4WaterWorldX;
uniform float4 vec4WaterWorldY;

uniform float fUseGBufferNormals; // 1 reads the G-buffer normal, 0 rebuilds it from depth
uniform float fTemporalBlend;     // weight of the previous frame's reflection, 0 disables accumulation
uniform float fFrameIndex;        // varies the ray start offset between frames
uniform float fPrevDepthValid;    // 0 until a previous frame's linear depth exists

uniform float4 vec4SunView;       // direction towards the directional light, in view space
uniform float fCSLength;          // contact shadow ray length in world units
uniform float fCSThickness;       // how deep behind a surface still occludes the light
uniform float fCSMaxViewDistance; // contact shadows fade out towards this view depth
uniform float fCSIntensity;       // 1 fully removes direct light at an occluded pixel

uniform float fGIRayLength;       // indirect light ray length in world units
uniform float fGIThickness;
uniform float fGIIntensity;
uniform float fGIMaxViewDistance;

uniform float fResolveBlend;      // weight of the reprojected history in the resolve pass

static const float HISTORY_CLAMP = 8.0;
static const float SSR_SCALE = 1.0;

float3 SampleHistory(float2 uv)
{
    return clamp(tex2D(HistoryTex, uv).rgb, 0.0, HISTORY_CLAMP) * SSR_SCALE;
}

// For use inside dynamic loops, where gradient based sampling is not allowed.
float3 SampleHistoryLod(float2 uv)
{
    return clamp(tex2Dlod(HistoryTex, float4(uv, 0, 0)).rgb, 0.0, HISTORY_CLAMP) * SSR_SCALE;
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
#define NUM_STEPS 24
#endif
#ifndef NUM_REFINE_STEPS
#define NUM_REFINE_STEPS 4
#endif
#ifndef CS_STEPS
#define CS_STEPS 12
#endif
#ifndef GI_RAYS
#define GI_RAYS 2
#endif
#ifndef GI_STEPS
#define GI_STEPS 10
#endif

float RawDepth(float2 uv)
{
    return tex2Dlod(DepthTex, float4(uv, 0, 0)).r;
}

float LinearDepth(float2 uv)
{
    return pow(fFarDivNear, RawDepth(uv)) * fNearPlane;
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

// Where this view space point was on screen last frame. Fails when it was off screen or
// when the previous frame saw a different surface there (disocclusion).
bool Reproject(float3 P, out float2 prevUV)
{
    prevUV = float2(-1.0, -1.0);

    float4 clip = P.x * vec4ViewToPrevClip[0]
                + P.y * vec4ViewToPrevClip[1]
                + P.z * vec4ViewToPrevClip[2]
                +       vec4ViewToPrevClip[3];

    if (fPrevDepthValid <= 0.0 || clip.w <= 0.0)
        return false;

    prevUV = (clip.xy / clip.w) * float2(0.5, -0.5) + 0.5;
    if (any(prevUV <= 0.0) || any(prevUV >= 1.0))
        return false;

    float prevZ = tex2Dlod(PrevDepthTex, float4(prevUV, 0, 0)).r;
    return abs(prevZ - clip.w) < clip.w * 0.05 + 0.05;
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

float3 WorldToView(float3 v)
{
    return float3(dot(vec4WaterToView[0].xyz, v),
                  dot(vec4WaterToView[1].xyz, v),
                  dot(vec4WaterToView[2].xyz, v));
}

// Same decoding deferred_lighting applies to _DEFERRED_GBUFFER_1_: 8 bits per axis
// in rgb with the fractional bits packed into alpha, in world space.
float3 GBufferNormal(float2 uv)
{
    float4 g = tex2Dlod(NormalTex, float4(uv, 0, 0));
    float3 f = frac(g.w * float3(0.998046875, 7.984375, 63.875));
    f.xy -= f.yz * 0.125;
    return normalize(WorldToView(g.xyz * 256.0 + f - 127.999992));
}

// Interleaved gradient noise, shifted every frame so accumulation sees new ray offsets.
// Kept in (0, 1] so the first step never lands on the ray origin.
float RayJitter(float2 pixel)
{
    pixel += fFrameIndex * 5.588238;
    return 1.0 - frac(52.9829189 * frac(dot(pixel, float2(0.06711056, 0.00583715))));
}

float4 TraceReflection(float3 C, float3 n, float blurPixels, float jitter)
{
    float z = C.z;
    float3 V = normalize(C);
    float3 R = reflect(V, n);

    if (R.z <= 0.0)
        return 0.0;

    float3 P0 = C + n * max(fMaxDistance / (float) NUM_STEPS * 0.1, z * 0.01);
    float3 P1 = P0 + R * fMaxDistance;   // R.z > 0, so P1.z > P0.z > 0 and both project

    float2 uv0 = ViewToUV(P0);
    float2 uv1 = ViewToUV(P1);
    float invZ0 = 1.0 / P0.z;
    float invZ1 = 1.0 / P1.z;

    float2 dUV = uv1 - uv0;
    float2 tEdge = (step(0.0, dUV) - uv0) / (abs(dUV) < 1e-5 ? 1e-5 : dUV);
    float tEnd = clamp(min(tEdge.x, tEdge.y), 0.0, 1.0);

    float dt = tEnd / (float) NUM_STEPS;

    float tHit = 0.0;
    float hitDelta = 0.0;
    float hitThickness = 1.0;
    float prevRayZ = P0.z;

    [loop]
    for (int i = 0; i < NUM_STEPS; ++i)
    {
        float t = dt * ((float) i + jitter);

        float2 sampleUV = lerp(uv0, uv1, t);
        float rayZ = 1.0 / lerp(invZ0, invZ1, t);

        float delta = rayZ - LinearDepth(sampleUV);

        if (delta > 0.0)
        {
            float thickness = (rayZ - prevRayZ) + fThickness;
            if (delta < thickness)
            {
                tHit = t;
                hitDelta = delta;
                hitThickness = thickness;
            }
            break;
        }

        prevRayZ = rayZ;
    }

    if (tHit <= 0.0)
        return 0.0;

    float lo = tHit - dt;
    float hi = tHit;
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
    float3 hitP = ViewPosFromUVZ(finalUV, 1.0 / lerp(invZ0, invZ1, hi));

    float2 histUV = HistoryUV(hitP);

    float2 edge = saturate(min(min(finalUV, histUV), 1.0 - max(finalUV, histUV)) / max(fEdgeFade, 1e-4));
    float e = min(edge.x, edge.y);
    float confidence = e * e * (3.0 - 2.0 * e);

    float rayLen = length(hitP - C);

    confidence *= saturate(dot(V, R) * 2.0 + 0.5);
    confidence *= saturate((1.0 - rayLen / fMaxDistance) * 4.0);
    confidence *= 1.0 - smoothstep(hitThickness * 0.75, hitThickness, hitDelta);

    float3 colour = SampleHistoryBlurred(histUV, blurPixels * saturate(rayLen / fMaxDistance));

    if (any(colour != colour))
        return 0.0;

    return float4(colour, confidence);
}

float4 SSR_PS(float2 uv : TEXCOORD0, float2 vPos : VPOS) : COLOR0
{
    float rawDepth = RawDepth(uv);
    if (rawDepth >= 0.9999)
        return 0.0; // sky

    // Matte surfaces reflect nothing, so skip the ray march for them entirely.
    float2 spec = saturate(tex2Dlod(SpecularTex, float4(uv, 0, 0)).xy);
    float gloss = sqrt(spec.x * spec.y);
    float glossWeight = smoothstep(fGlossCutoff, fGlossCutoff + 0.2, gloss) * (1.0 + fGlossBoost * gloss);
    if (glossWeight <= 0.0)
        return 0.0;

    // uv rather than VPOS, so the pass also works into a reduced resolution target.
    float3 C = ViewPosFromUVZ(uv, pow(fFarDivNear, rawDepth) * fNearPlane);

    float3 n;
    [branch]
    if (fUseGBufferNormals > 0.0)
        n = GBufferNormal(uv);
    else
        n = ReconstructNormal(uv, C);
    n = (dot(n, C) > 0.0) ? -n : n;

    float4 r = TraceReflection(C, n, 0.0, (fTemporalBlend > 0.0) ? RayJitter(vPos) : 1.0);
    r.a = saturate(r.a * glossWeight * fIntensity);

    if (fTemporalBlend > 0.0)
    {
        // Reproject this surface point into the previous frame's reflection and blend in
        // premultiplied form, so a miss on either side does not darken the colour.
        float2 prevUV;
        if (Reproject(C, prevUV))
        {
            float4 prev = tex2Dlod(PrevSSRTex, float4(prevUV, 0, 0));
            if (all(prev == prev))
            {
                float4 cur = float4(r.rgb * r.a, r.a);
                float4 acc = lerp(cur, float4(prev.rgb * prev.a, prev.a), fTemporalBlend);
                r = float4(acc.rgb / max(acc.a, 1e-4), acc.a);
            }
        }
    }

    return r;
}

float3 WaterNormal(float2 worldXY, float distSq)
{
    float near = max(1.0 - distSq * 0.0004, 0.0);

    float2 slope = (tex2D(SurfaceTex, worldXY * 0.002).zw - 0.5) * 0.0512 * (1.0 - near);
    slope += (tex2D(SurfaceTex, worldXY * 0.01).zw - 0.5) * 1.024;
    slope += (tex2D(SurfaceTex, worldXY * 0.0454545468).zw - 0.5) * 0.465454549 * near;

    return WorldToView(normalize(float3(slope * fWaterNormalStrength, 1.0)));
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

    float4 r = TraceReflection(C, n, fWaterBlur, 1.0);

    return float4(r.rgb, saturate(r.a * fWaterIntensity));
}

// Copy of this frame's linear view depth, read next frame by Reproject.
float4 LinearDepth_PS(float2 uv : TEXCOORD0) : COLOR0
{
    return float4(LinearDepth(uv), 0.0, 0.0, 1.0);
}

float3 SurfaceNormal(float2 uv, float3 C)
{
    float3 n;
    [branch]
    if (fUseGBufferNormals > 0.0)
        n = GBufferNormal(uv);
    else
        n = ReconstructNormal(uv, C);
    return (dot(n, C) > 0.0) ? -n : n;
}

// Offsets of a second, decorrelated noise value for the SSGI ray directions.
float RayJitter2(float2 pixel)
{
    return RayJitter(pixel.yx + float2(17.0, 59.0));
}

// Short rays towards the directional light catch the small occluders shadow maps miss.
// Output is occlusion (0 lit, 1 shadowed) so an unbound sampler leaves lighting alone.
float4 ContactShadows_PS(float2 uv : TEXCOORD0, float2 vPos : VPOS) : COLOR0
{
    float rawDepth = RawDepth(uv);
    if (rawDepth >= 0.9999 || vec4SunView.w <= 0.0)
        return 0.0;

    float3 C = ViewPosFromUVZ(uv, pow(fFarDivNear, rawDepth) * fNearPlane);
    if (C.z >= fCSMaxViewDistance)
        return 0.0;

    float3 L = vec4SunView.xyz;
    float3 n = SurfaceNormal(uv, C);
    if (dot(n, L) <= 0.0)
        return 0.0; // already facing away from the light

    float3 P0 = C + n * (0.02 + C.z * 0.002);

    // A ray heading back towards the camera must stay in front of the near plane.
    float len = fCSLength;
    if (L.z < 0.0)
        len = min(len, (P0.z - fNearPlane * 2.0) / -L.z);
    if (len <= 0.0)
        return 0.0;

    float jitter = RayJitter(vPos);
    float occlusion = 0.0;

    [loop]
    for (int i = 0; i < CS_STEPS; ++i)
    {
        float t = ((float) i + jitter) / (float) CS_STEPS;
        float3 P = P0 + L * (len * t);
        float2 sampleUV = ViewToUV(P);
        if (any(sampleUV <= 0.0) || any(sampleUV >= 1.0))
            break;

        float delta = P.z - LinearDepth(sampleUV);
        if (delta > 0.0 && delta < fCSThickness)
        {
            occlusion = 1.0 - t * t; // occluders further along the ray cast softer shadows
            break;
        }
    }

    float fade = 1.0 - smoothstep(fCSMaxViewDistance * 0.75, fCSMaxViewDistance, C.z);
    return float4(saturate(occlusion * fade * fCSIntensity), 0.0, 0.0, 1.0);
}

// One bounce of indirect light: cosine distributed rays over the G-buffer normal pick up
// the previous frame's lit colour where they hit. deferred_lighting multiplies the result
// by albedo and adds it to the ambient term.
float4 SSGI_PS(float2 uv : TEXCOORD0, float2 vPos : VPOS) : COLOR0
{
    float rawDepth = RawDepth(uv);
    if (rawDepth >= 0.9999)
        return 0.0;

    float3 C = ViewPosFromUVZ(uv, pow(fFarDivNear, rawDepth) * fNearPlane);
    if (C.z >= fGIMaxViewDistance)
        return 0.0;

    float3 n = SurfaceNormal(uv, C);
    float3 up = (abs(n.z) < 0.999) ? float3(0.0, 0.0, 1.0) : float3(1.0, 0.0, 0.0);
    float3 T = normalize(cross(up, n));
    float3 B = cross(n, T);

    float3 P0 = C + n * (0.05 + C.z * 0.002);
    float jitter = RayJitter(vPos);
    float jitter2 = RayJitter2(vPos);
    float3 sum = 0.0;

    [loop]
    for (int r = 0; r < GI_RAYS; ++r)
    {
        float u1 = frac(jitter + (float) r * 0.618034);
        float u2 = frac(jitter2 + (float) r * 0.7548777);
        float phi = 6.2831853 * u1;
        float sinTheta = sqrt(u2);
        float3 dir = T * (cos(phi) * sinTheta) + B * (sin(phi) * sinTheta) + n * sqrt(1.0 - u2);

        float len = fGIRayLength;
        if (dir.z < 0.0)
            len = min(len, (P0.z - fNearPlane * 2.0) / -dir.z);
        if (len <= 0.0)
            continue;

        [loop]
        for (int i = 0; i < GI_STEPS; ++i)
        {
            float t = ((float) i + jitter) / (float) GI_STEPS;
            float3 P = P0 + dir * (len * t);
            float2 sampleUV = ViewToUV(P);
            if (any(sampleUV <= 0.0) || any(sampleUV >= 1.0))
                break;

            float delta = P.z - LinearDepth(sampleUV);
            if (delta > 0.0 && delta < fGIThickness)
            {
                float2 histUV = HistoryUV(P);
                if (all(histUV > 0.0) && all(histUV < 1.0))
                    sum += SampleHistoryLod(histUV) * (1.0 - t * t);
                break;
            }
        }
    }

    float fade = 1.0 - smoothstep(fGIMaxViewDistance * 0.75, fGIMaxViewDistance, C.z);
    float3 gi = sum * (fGIIntensity * fade / (float) GI_RAYS);
    if (any(gi != gi))
        return 0.0;
    return float4(gi, 1.0);
}

// Blends a noisy pass with its reprojected history, dropping history on disocclusion.
float4 TemporalResolve_PS(float2 uv : TEXCOORD0) : COLOR0
{
    float4 cur = tex2Dlod(CurTex, float4(uv, 0, 0));

    float rawDepth = RawDepth(uv);
    if (rawDepth >= 0.9999 || fResolveBlend <= 0.0)
        return cur;

    float2 prevUV;
    if (!Reproject(ViewPosFromUVZ(uv, pow(fFarDivNear, rawDepth) * fNearPlane), prevUV))
        return cur;

    float4 prev = tex2Dlod(PrevTex, float4(prevUV, 0, 0));
    if (any(prev != prev))
        return cur;

    return lerp(cur, prev, fResolveBlend);
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

technique LinearDepthCopy
{
    pass P0
    {
        VertexShader = compile vs_3_0 FullscreenQuadVS();
        PixelShader = compile ps_3_0 LinearDepth_PS();
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

technique SSGI
{
    pass P0
    {
        VertexShader = compile vs_3_0 FullscreenQuadVS();
        PixelShader = compile ps_3_0 SSGI_PS();
    }
}

technique TemporalResolve
{
    pass P0
    {
        VertexShader = compile vs_3_0 FullscreenQuadVS();
        PixelShader = compile ps_3_0 TemporalResolve_PS();
    }
}
