// Shaders of temporallayer.ixx, what Fusion Fix adds to upstream's temporal anti-aliasing (temporal.ixx, Resolve.hlsl):
// the reactive mask, the rain after the resolve and the depth averaged over the jitter.
//
// Motion vectors are stored as (previous - current) in texture coordinates, jitter excluded:
// the history of a pixel is fetched at uv + motion.

// ---------------------------------------------------------------------------------------------
// Reactive mask: particles, glass and other transparent geometry have no motion vectors of their own.
// Their pixels are found by comparing the final scene with a copy taken right after the fog pass, before
// transparent geometry and visual effects are drawn, and follow the current frame more.

sampler2D SceneTex  : register(s0);
sampler2D OpaqueTex : register(s1); // luminance of the scene before transparent geometry

float4 gReactive : register(c0); // x: scale, y: maximum

float Luma(float3 c)
{
    return dot(c, float3(0.2126, 0.7152, 0.0722));
}

float4 PS_OpaqueLuma(float2 uv : TEXCOORD0) : COLOR0
{
    return Luma(tex2Dlod(SceneTex, float4(uv, 0.0, 0.0)).rgb).xxxx;
}

float ReactiveValue(float scene, float opaque, float4 params)
{
    // Compared after a tonemap, a bright particle on a dark background counts as much as a dark one on a
    // bright background
    scene = max(scene, 0.0);
    opaque = max(opaque, 0.0);
    float difference = abs(scene / (1.0 + scene) - opaque / (1.0 + opaque));
    return saturate(difference * params.x) * params.y;
}

float4 PS_Reactive(float2 uv : TEXCOORD0) : COLOR0
{
    float scene = Luma(tex2Dlod(SceneTex, float4(uv, 0.0, 0.0)).rgb);
    float opaque = tex2Dlod(OpaqueTex, float4(uv, 0.0, 0.0)).r;
    return ReactiveValue(scene, opaque, gReactive).xxxx;
}


// ---------------------------------------------------------------------------------------------
// Rain after the resolve: the rain has no motion vectors, so it's drawn into a copy of the scene and what it
// changed there is added to the picture once the resolve, DLAA or FSR made it. Added rather than blended, the
// layer works for any blend mode the rain draws with.

sampler2D RainTex      : register(s0); // the copy of the scene with the rain
sampler2D RainSceneTex : register(s1); // the scene without it

float4 PS_RainLayer(float2 uv : TEXCOORD0) : COLOR0
{
    // Clamped first: an infinite pixel of the scene would leave infinity minus infinity
    float3 rain = clamp(tex2Dlod(RainTex, float4(uv, 0.0, 0.0)).rgb, -65504.0, 65504.0);
    float3 scene = clamp(tex2Dlod(RainSceneTex, float4(uv, 0.0, 0.0)).rgb, -65504.0, 65504.0);
    return float4(rain - scene, 0.0);
}

sampler2D RainLayerTex : register(s0);

float4 gRainOffset : register(c0); // xy: where the jitter moved the layer, in its texture coordinates

float4 PS_RainComposite(float2 uv : TEXCOORD0) : COLOR0
{
    return float4(tex2Dlod(RainLayerTex, float4(uv + gRainOffset.xy, 0.0, 0.0)).rgb, 0.0);
}

// ---------------------------------------------------------------------------------------------
// Steady depth: the logarithmic depth averaged over the jitter, for what reads depth after the resolve or
// samples it at a few points (depth of field, sun shafts, the coronas' occlusion). On edges the jittered depth
// flips between the near and the far surface from frame to frame; there the reprojected history, clamped to the
// neighbourhood, takes most of the weight.

sampler2D SteadyNowTex    : register(s0); // _DEFERRED_GBUFFER_3_, logarithmic depth
sampler2D SteadyHistTex   : register(s1);
sampler2D SteadyMotionTex : register(s2);

float4 gSteadyTexel  : register(c0); // 1 / width, 1 / height, width, height
float4 gSteadyBlend  : register(c1); // x: minimum current weight, y: maximum, z: weight per pixel of motion, w: history valid
float4 gSteadyEdge   : register(c2); // x: depth range of the neighbourhood that counts as an edge

float4 PS_SteadyDepth(float2 uv : TEXCOORD0) : COLOR0
{
    const float2 offsets[8] =
    {
        float2(-1.0, -1.0), float2(0.0, -1.0), float2(1.0, -1.0), float2(-1.0, 0.0),
        float2(1.0, 0.0), float2(-1.0, 1.0), float2(0.0, 1.0), float2(1.0, 1.0)
    };

    float now = tex2Dlod(SteadyNowTex, float4(uv, 0.0, 0.0)).r;
    float nearest = now;
    float farthest = now;
    float2 nearestOffset = 0.0;

    [unroll]
    for (int i = 0; i < 8; ++i)
    {
        float d = tex2Dlod(SteadyNowTex, float4(uv + offsets[i] * gSteadyTexel.xy, 0.0, 0.0)).r;
        if (d < nearest)
        {
            nearest = d;
            nearestOffset = offsets[i];
        }
        farthest = max(farthest, d);
    }

    // Flat surfaces barely move with the jitter
    if (gSteadyBlend.w <= 0.0 || farthest - nearest <= gSteadyEdge.x)
        return float4(now, 0.0, 0.0, 1.0);

    float2 motion = tex2Dlod(SteadyMotionTex, float4(uv + nearestOffset * gSteadyTexel.xy, 0.0, 0.0)).xy;
    float2 historyUV = uv + motion;
    if (any(historyUV != saturate(historyUV)))
        return float4(now, 0.0, 0.0, 1.0);

    float history = clamp(tex2Dlod(SteadyHistTex, float4(historyUV, 0.0, 0.0)).r, nearest, farthest);
    float speed = length(motion * gSteadyTexel.zw);
    float weight = lerp(gSteadyBlend.x, gSteadyBlend.y, saturate(speed * gSteadyBlend.z));
    return float4(lerp(history, now, weight), 0.0, 0.0, 1.0);
}
