texture DepthTex2D, HistoryTex2D, SpecularTex2D, SurfaceTex2D;

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

uniform float4 vec4WaterToView[3];
uniform float4 vec4WaterWorldX;
uniform float4 vec4WaterWorldY;

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
#define NUM_STEPS 24
#endif
#ifndef NUM_REFINE_STEPS
#define NUM_REFINE_STEPS 4
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

float4 TraceReflection(float3 C, float3 n, float blurPixels)
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
        float t = dt * (float) (i + 1);

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
    float3 C = ReconstructViewPos(vPos, LinearDepth(uv));

    float3 n = ReconstructNormal(uv, C);
    n = (dot(n, C) > 0.0) ? -n : n;

    float4 r = TraceReflection(C, n, 0.0);

    float2 spec = saturate(tex2D(SpecularTex, uv).xy);
    float gloss = sqrt(spec.x * spec.y);
    r.a *= smoothstep(fGlossCutoff, fGlossCutoff + 0.2, gloss) * (1.0 + fGlossBoost * gloss);

    return float4(r.rgb, saturate(r.a * fIntensity));
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

    float4 r = TraceReflection(C, n, fWaterBlur);

    return float4(r.rgb, saturate(r.a * fWaterIntensity));
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
