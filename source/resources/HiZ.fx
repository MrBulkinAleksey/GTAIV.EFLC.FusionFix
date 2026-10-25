// Hi-Z for occlusion culling (BuildHiZ in postfx.ixx, the test in occlusion.ixx): at the end of the G-buffer pass,
// the farthest view depth, in metres, of every 4 x 4 pixels, and of every 4 x 4 of those again until the grid is
// small enough to read back. An entity whose nearest point lies behind the farthest depth of every texel it covers
// is hidden behind what was drawn.
//
// Only the world hides anything: pixels of the material categories fOccluders leaves out (the material ID less
// its 128 and 8 bits, as WetGround.fx takes it: cars, people) count as nothing in the way, so a bus driving off
// can't hide the building it uncovers in the frames the grid lags behind.

sampler2D DepthTex : register(s0);     // First: the log depth copied at the G-buffer's end; Reduce: the level before
sampler2D MaterialTex : register(s1);  // First: _STENCIL_BUFFER_, the material IDs

float2 vec2SrcSize;     // the source's size in texels
float fNearPlane;
float fFarDivNear;
float fNothing;         // the depth of a pixel where nothing hides anything
float fOccluders;       // a bit per material category 0..7 that hides what is behind it

float2 SourceUV(float2 base, float x, float y)
{
    return (min(base + float2(x, y), vec2SrcSize - 1.0) + 0.5) / vec2SrcSize;
}

// VPOS is the pixel's integer corner in D3D9; + 0.25 keeps the same pixel were it its centre
float4 First_PS(float2 vpos : VPOS) : COLOR0
{
    // The log depth grows with the view depth: the farthest is taken on it, and turned into metres once. A pixel
    // that hides nothing counts as 2, past any log depth.
    float2 base = floor(vpos + 0.25) * 4.0;
    float farthest = 0.0;
    [unroll] for (int y = 0; y < 4; ++y)
    {
        [unroll] for (int x = 0; x < 4; ++x)
        {
            float2 uv = SourceUV(base, x, y);
            float depth = tex2Dlod(DepthTex, float4(uv, 0, 0)).r;
            float category = fmod(floor(tex2Dlod(MaterialTex, float4(uv, 0, 0)).r * 255.0 + 0.4), 8.0);
            float occludes = fmod(floor(fOccluders * exp2(-category)), 2.0);
            farthest = max(farthest, occludes > 0.5 ? depth : 2.0);
        }
    }
    return float4(farthest > 1.5 ? fNothing : pow(fFarDivNear, farthest) * fNearPlane, 0.0, 0.0, 0.0);
}

float4 Reduce_PS(float2 vpos : VPOS) : COLOR0
{
    float2 base = floor(vpos + 0.25) * 4.0;
    float farthest = 0.0;
    [unroll] for (int y = 0; y < 4; ++y)
    {
        [unroll] for (int x = 0; x < 4; ++x)
            farthest = max(farthest, tex2Dlod(DepthTex, float4(SourceUV(base, x, y), 0, 0)).r);
    }
    return float4(farthest, 0.0, 0.0, 0.0);
}

technique First
{
    pass P0
    {
        PixelShader = compile ps_3_0 First_PS();
    }
}

technique Reduce
{
    pass P0
    {
        PixelShader = compile ps_3_0 Reduce_PS();
    }
}
