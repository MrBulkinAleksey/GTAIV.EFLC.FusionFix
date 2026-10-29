// Water Far Layer (waterfarlayer.ixx): the far part of the water's reflection, drawn into a layer of its own now and
// then and laid into the reflection every frame from it, at the end of the reflection's pass. Far off only the
// direction matters, so each pixel's direction in the reflection's view now is taken into the view the layer was drawn
// with (matReproject: the turn between the two, then the layer's projection) and the layer read there. Only where the
// reflection has drawn nothing (the depth test against the far plane: sky and clouds write no depth), and only where
// the layer has something (its depth under 1).

float4x4 matReproject;  // a direction in the reflection's view now (x, y, -1, 0) to the layer's clip x, y, z, w
float4 vec4Size;        // 1 / width, 1 / height of the reflection, 1 / its projection's x and y scale

sampler2D LayerColour : register(s0);   // the layer, A16B16G16R16F, point
sampler2D LayerDepth : register(s1);    // its depth, INTZ, point

float4 CompositePS(float2 vPos : VPOS) : COLOR0
{
    float2 ndc = float2((vPos.x + 0.5) * vec4Size.x * 2.0 - 1.0, 1.0 - (vPos.y + 0.5) * vec4Size.y * 2.0);
    float4 c = mul(float4(ndc.x * vec4Size.z, ndc.y * vec4Size.w, -1.0, 0.0), matReproject);
    // Behind the layer's view, or outside it: nothing
    clip(c.w - 1e-4);
    float2 uv = c.xy / c.w * float2(0.5, -0.5) + 0.5;
    clip(float4(uv, 1.0 - uv));
    float depth = tex2Dlod(LayerDepth, float4(uv, 0.0, 0.0)).r;
    clip(0.999999 - depth);
    return float4(tex2Dlod(LayerColour, float4(uv, 0.0, 0.0)).rgb, 1.0);
}

technique Composite
{
    pass P0
    {
        PixelShader = compile ps_3_0 CompositePS();
    }
}
