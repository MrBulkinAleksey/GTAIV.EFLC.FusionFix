// AMD FidelityFX Contrast Adaptive Sharpening (CAS), without the scaling part, run on the
// finished frame before the HUD (Sharpening in the graphics menu). It sharpens each pixel by
// how much contrast its neighbourhood has left, so edges that are already crisp do not ring
// and flat areas do not turn into noise.
//
// The algorithm of CAS from AMD FidelityFX, MIT license, Copyright (c) 2020 Advanced Micro Devices, Inc.

uniform float4 globalScreenSize : register(c44);    // zw: one pixel in texture coordinates
uniform float4 CASParams        : register(c200);   // x: the peak, -1 / lerp(8, 5, sharpness)
sampler2D Scene                 : register(s2);     // the frame, point sampled

// From rage_postfxVS0.
struct PS_IN
{
    float2 texcoord : TEXCOORD;
};

float3 Tap(float2 uv, float2 offset)
{
    return tex2Dlod(Scene, float4(uv + offset * globalScreenSize.zw, 0.0f, 0.0f)).rgb;
}

float4 ApplyCAS(PS_IN i) : COLOR
{
    // a b c
    // d e f
    // g h i
    float2 uv = i.texcoord;
    float3 a = Tap(uv, float2(-1.0f, -1.0f));
    float3 b = Tap(uv, float2( 0.0f, -1.0f));
    float3 c = Tap(uv, float2( 1.0f, -1.0f));
    float3 d = Tap(uv, float2(-1.0f,  0.0f));
    float3 e = Tap(uv, float2( 0.0f,  0.0f));
    float3 f = Tap(uv, float2( 1.0f,  0.0f));
    float3 g = Tap(uv, float2(-1.0f,  1.0f));
    float3 h = Tap(uv, float2( 0.0f,  1.0f));
    float3 k = Tap(uv, float2( 1.0f,  1.0f));

    // Soft minimum and maximum: the cross plus the whole 3x3, which rounds them off.
    float3 mn = min(min(min(d, e), min(f, b)), h);
    float3 mn2 = min(mn, min(min(a, c), min(g, k)));
    mn += mn2;
    float3 mx = max(max(max(d, e), max(f, b)), h);
    float3 mx2 = max(mx, max(max(a, c), max(g, k)));
    mx += mx2;

    // How far the neighbourhood stays from black and white decides how much it is sharpened.
    float3 amp = saturate(min(mn, 2.0f - mx) / max(mx, 1.0e-5f));
    amp = sqrt(amp);

    // The cross weighted negatively, normalised so that flat colour stays the same.
    float3 w = amp * CASParams.x;
    float3 colour = ((b + d + f + h) * w + e) / (1.0f + 4.0f * w);
    // Not clamped to 1: with HDR output the back buffer holds highlights above it, which amp leaves unsharpened.
    return float4(max(colour, 0.0f), 1.0f);
}

// The same with frame generation (see framegeneration.ixx), on the frames as they are shown: the frame generation
// works on the frame without sharpening, whose shadows and edges it follows better, and both the rendered and the
// generated frame are sharpened afterwards. The HUD is in them by then: where the finished frame (Present) differs from
// the frame before the HUD (HudLess) is HUD, left as it is and kept out of its neighbours' sharpening. The generated
// frame takes the HUD from the rendered one, so the same comparison tells where it is in both.
uniform float4 CASMaskedParams  : register(c201);   // x: 1 / paper white with HDR output (scRGB), 0 without; zw: one pixel
sampler2D Present               : register(s3);
sampler2D HudLess               : register(s4);

bool IsHud(float2 uv)
{
    float3 p = tex2Dlod(Present, float4(uv, 0.0f, 0.0f)).rgb;
    float3 h = tex2Dlod(HudLess, float4(uv, 0.0f, 0.0f)).rgb;
    // Loose enough for the console gamma, which the game draws on the 8-bit back buffer and the copy gets in 16-bit float
    return any(abs(p - h) > 0.01f + 0.01f * max(p, h));
}

// scRGB is linear and paper white is above 1: brought to about the gamma of the frame CAS ran on before the HUD
float3 ToCas(float3 c)
{
    return CASMaskedParams.x > 0.0f ? pow(max(c * CASMaskedParams.x, 0.0f), 1.0f / 2.2f) : c;
}

float3 FromCas(float3 c)
{
    return CASMaskedParams.x > 0.0f ? pow(max(c, 0.0f), 2.2f) / CASMaskedParams.x : c;
}

float3 MaskedTap(float2 uv, float2 offset, float3 centre)
{
    float2 at = uv + offset * CASMaskedParams.zw;
    return IsHud(at) ? centre : ToCas(tex2Dlod(Scene, float4(at, 0.0f, 0.0f)).rgb);
}

float4 ApplyCASMasked(PS_IN i) : COLOR
{
    float2 uv = i.texcoord;
    float4 centre = tex2Dlod(Scene, float4(uv, 0.0f, 0.0f));
    [branch]
    if (IsHud(uv))
        return centre;

    float3 e = ToCas(centre.rgb);
    float3 a = MaskedTap(uv, float2(-1.0f, -1.0f), e);
    float3 b = MaskedTap(uv, float2( 0.0f, -1.0f), e);
    float3 c = MaskedTap(uv, float2( 1.0f, -1.0f), e);
    float3 d = MaskedTap(uv, float2(-1.0f,  0.0f), e);
    float3 f = MaskedTap(uv, float2( 1.0f,  0.0f), e);
    float3 g = MaskedTap(uv, float2(-1.0f,  1.0f), e);
    float3 h = MaskedTap(uv, float2( 0.0f,  1.0f), e);
    float3 k = MaskedTap(uv, float2( 1.0f,  1.0f), e);

    float3 mn = min(min(min(d, e), min(f, b)), h);
    float3 mn2 = min(mn, min(min(a, c), min(g, k)));
    mn += mn2;
    float3 mx = max(max(max(d, e), max(f, b)), h);
    float3 mx2 = max(mx, max(max(a, c), max(g, k)));
    mx += mx2;

    float3 amp = sqrt(saturate(min(mn, 2.0f - mx) / max(mx, 1.0e-5f)));
    float3 w = amp * CASParams.x;
    float3 colour = ((b + d + f + h) * w + e) / (1.0f + 4.0f * w);
    return float4(FromCas(max(colour, 0.0f)), centre.a);
}
