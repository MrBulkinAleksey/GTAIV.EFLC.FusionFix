texture DepthTex2D, HistoryTex2D, SpecularTex2D, SurfaceTex2D, NormalTex2D, SSRResultTex2D, DebugTex2D;
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

sampler2D SSRResultTex
{
    Texture = <SSRResultTex2D>;
};

sampler2D DebugTex
{
    Texture = <DebugTex2D>;
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

uniform float4 vec4WaterToView[3];
uniform float4 vec4WaterWorldX;
uniform float4 vec4WaterWorldY;

uniform float fDebugMode; // SSR debug view from the graphics menu, see SSRDebug_PS
uniform float fUseGBufferNormals; // 1 reads the G-buffer normal, 0 rebuilds it from depth
uniform float fDenoiseRadius;     // SSR smoothing radius in pixels, see SSRDenoise_PS
uniform float fPassThinObjects;   // 1 lets a ray that went far behind an object carry on
uniform float fStepJitter;        // 1 shifts each pixel's steps by up to one step
uniform float fTowardCamera;      // 0..1, how far reflections pointing back at the camera reach

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
float4 TraceReflection(float3 C, float3 n, float blurPixels, float jitter)
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
        float s = ((float) i + jitter) / (float) NUM_STEPS;
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
    confidence *= 1.0 - smoothstep(hitThickness * 0.75, hitThickness, hitDelta);

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

float4 SSR_PS(float2 uv : TEXCOORD0, float2 vPos : VPOS) : COLOR0
{
    float surfaceWeight = SSRSurfaceWeight(uv);
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

    float4 r = TraceReflection(C, n, 0.0, fStepJitter > 0.0 ? PixelJitter(vPos) : 1.0);
    return float4(r.rgb, saturate(r.a * surfaceWeight * fIntensity));
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

    float4 r = TraceReflection(C, n, fWaterBlur, 1.0);

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

technique SSRDebugCopy
{
    pass P0
    {
        VertexShader = compile vs_3_0 FullscreenQuadVS();
        PixelShader = compile ps_3_0 SSRDebugCopy_PS();
    }
}
