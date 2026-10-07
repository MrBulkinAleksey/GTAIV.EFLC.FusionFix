// Glints of headlights (RenderHeadlightGlints in postfx.ixx): the lamps themselves as glossy
// surfaces mirror them, added to the lit scene once the game has drawn its lights.
//
// The game lights with a headlight only inside its cone, so a car's paint, a shop window or a wet
// road beside or behind the beam never showed the lamp, though its glass glows towards anyone in
// front of it. Here each headlight is two small spheres, its lamps, seen from any side but behind,
// and every glossy pixel takes their GGX highlight: the energy the sphere's size spreads (Karis),
// so a mirror shows each lamp as a point of the same brightness near and far, and rough paint a
// wider, dimmer spot. Inside the beam, where the game's own highlight (GGX too, with LightsGGX)
// lights the surface, this one gives way to it.
//
// What glints: gloss over vec4Glint.y, as SSR decides what reflects (car paint about 0.8, a wet
// film 0.55 a little, puddles 0.9), so dry asphalt, walls and skin are left alone.

sampler2D DepthTex : register(s0);    // the log depth, copied at the end of the G-buffer pass
sampler2D NormalTex : register(s1);   // _DEFERRED_GBUFFER_1_, world normals
sampler2D SpecularTex : register(s2); // _DEFERRED_GBUFFER_2_: specular intensity, gloss, AO

#define MAX_LIGHTS 16

float4 vec4ProjInfo;
float fNearPlane;
float fFarDivNear;
// View to world rows, the camera's position in w.
float4 vec4WorldX;
float4 vec4WorldY;
float4 vec4WorldZ;
float2 vec2InvSize;   // 1 / the G-buffer's size
float4 vec4Glint;     // x strength, y gloss from which surfaces glint, z the lamps' radius in metres, w the highlight's ceiling
float fLightCount;
// Per headlight: the light's position and half its lamps' spacing; the way it shines and how far
// from it the game's own highlight reaches; the car's level right; its colour times intensity.
float4 vec4LightPos[MAX_LIGHTS];
float4 vec4LightDir[MAX_LIGHTS];
float4 vec4LightRight[MAX_LIGHTS];
float4 vec4LightColour[MAX_LIGHTS];

float4 Glints_PS(float2 vpos : VPOS) : COLOR0
{
    float2 uv = (vpos + 0.5) * vec2InvSize;
    float4 spec = tex2D(SpecularTex, uv);
    float glossy = smoothstep(vec4Glint.y, vec4Glint.y + 0.2, spec.y);
    clip(glossy - 1e-3);

    float raw = tex2D(DepthTex, uv).r;
    clip(0.99999 - raw); // sky

    float4 g = tex2D(NormalTex, uv);
    float3 f = frac(g.w * float3(0.998046875, 7.984375, 63.875));
    f.xy -= f.yz * 0.125;
    float3 N = normalize(g.xyz * 256.0 + f - 127.999992);

    float z = pow(abs(fFarDivNear), raw) * fNearPlane;
    float3 v = float3((vpos + 0.5) * vec4ProjInfo.xy + vec4ProjInfo.zw, 1.0);
    float3 dir = float3(dot(v, vec4WorldX.xyz), dot(v, vec4WorldY.xyz), dot(v, vec4WorldZ.xyz));
    float3 P = float3(vec4WorldX.w, vec4WorldY.w, vec4WorldZ.w) + dir * z;
    float3 V = normalize(-dir);
    float NV = dot(N, V);
    clip(NV);
    // Normal mapped pixels turned nearly edge on sparkle, as with the lamps' GGX.
    float edge = smoothstep(0.0, 0.08, NV);

    // Gloss is sqrt(power / 512) of the game's Phong lobe pow(R.L, n), whose width is GGX's at
    // alpha squared 2 / (4n + 2).
    float n = clamp(spec.y * spec.y * 512.0, 1.0, 512.0);
    float a = sqrt(2.0 / (4.0 * n + 2.0));

    float3 sum = 0.0;
    [loop]
    for (int i = 0; i < MAX_LIGHTS; ++i)
    {
        [branch]
        if (i >= fLightCount)
            break;
        float4 light = vec4LightPos[i];
        float4 fwd = vec4LightDir[i];
        float3 right = vec4LightRight[i].xyz;
        [unroll]
        for (int side = -1; side <= 1; side += 2)
        {
            float3 Lv = light.xyz + right * (light.w * side) - P;
            float d2 = max(dot(Lv, Lv), 1e-4);
            float invD = rsqrt(d2);
            float3 L = Lv * invD;
            float NL = saturate(dot(N, L));

            // The lamp's glass glows ahead and to the sides, not behind; inside the beam, as near as
            // the game's own highlight reaches, that one stands.
            float cosE = dot(-L, fwd.xyz);
            float emit = smoothstep(-0.15, 0.5, cosE);
            emit *= 1.0 - smoothstep(0.85, 0.95, cosE) * saturate(1.0 - d2 * invD * fwd.w);

            // A sphere of radius r widens the lobe by about r / 2d, and its energy spreads with it.
            float aw = saturate(a + vec4Glint.z * 0.5 * invD);
            float aw2 = aw * aw;
            float3 H = normalize(L + V);
            float NH = saturate(dot(N, H));
            float LH = saturate(dot(L, H));
            float t = NH * NH * (aw2 - 1.0) + 1.0;
            float D = aw2 / (3.14159265 * t * t) * (a * a / aw2);
            float vis = 0.5 / max(NL * (NV * (1.0 - aw) + aw) + NV * (NL * (1.0 - aw) + aw), 1e-4);
            float F = 0.04 + 0.96 * pow(1.0 - LH, 5.0);
            float lobe = min(D * vis, vec4Glint.w) * F * NL;
            sum += vec4LightColour[i].rgb * (lobe * emit / max(d2, 1.0));
        }
    }
    // The game gives both lamps one light: half its colour from each.
    return float4(sum * (vec4Glint.x * glossy * edge * 0.5), 0.0);
}

technique Glints
{
    pass P0
    {
        PixelShader = compile ps_3_0 Glints_PS();
    }
}
