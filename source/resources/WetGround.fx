// Wet ground in the rain (RenderWetGround in postfx.ixx): one pass at the end of the G-buffer pass,
// before any light reads it, from copies of _DEFERRED_GBUFFER_0_ to _2_ back into them, so the sun,
// the lamps, the sky's reflection and SSR all see the same wet surfaces.
//
// - Wet surfaces darken (water fills the pores) and turn glossy; walls darken only a little.
// - On flat ground puddles gather where the noise map is highest: the map is rank equalised, so
//   its threshold is the share of the ground under water. In them the colour darkens further, the
//   surface turns to a mirror and its normal flattens to the water's.
// - Rain falling into puddles rings them: three hashed grids of expanding rings tilt the normal
//   radially, so every light and reflection ripples with them.
//
// What gets wet: the material IDs (_STENCIL_BUFFER_) whose category (the ID less its 128 and 8
// bits) vec4Allow0/1 lets through, so cars and people stay as they are, and only where the
// G-buffer's vertex colour (_DEFERRED_GBUFFER_2_.z) says the sky reaches: under bridges and
// awnings the ground stays dry.

sampler2D DepthTex : register(s0);    // the log depth, copied at the end of the G-buffer pass
sampler2D AlbedoTex : register(s1);   // copies of _DEFERRED_GBUFFER_0_ to _2_
sampler2D NormalTex : register(s2);
sampler2D SpecularTex : register(s3);
sampler2D MaterialTex : register(s4); // _STENCIL_BUFFER_, the material IDs
sampler2D NoiseTex : register(s5);    // the clouds' coverage map, tiling, rank equalised

float4 vec4ProjInfo;
float fNearPlane;
float fFarDivNear;
// View to world rows, the camera's position in w.
float4 vec4WorldX;
float4 vec4WorldY;
float4 vec4WorldZ;
float4 vec4Wet;     // wetness 0..1, the share of flat ground under puddles, rain now 0..1, seconds
float4 vec4Shape;   // 1 / puddle tile in metres, ripple strength, ripples' far end in metres, darkening
float4 vec4Allow0;  // material categories 0..3 that get wet (1) or not (0)
float4 vec4Allow1;  // and 4..7
float fDebug;       // WetGroundDebug: 1 the material categories, 2 wetness, puddles and rings

struct Targets
{
    float4 albedo : COLOR0;
    float4 normal : COLOR1;
    float4 specular : COLOR2;
};

float Hash(float2 p)
{
    return frac(sin(dot(p, float2(127.1, 311.7))) * 43758.5453);
}

// One grid of rings: a drop lands somewhere in each cell at a random time of its cycle, and the
// ring it makes runs out to most of the cell and fades. Returns the normal's tilt, away from the
// drop on the ring's outer side.
float2 Rings(float2 p, float cell, float rate, float seed)
{
    float2 q = p / cell + seed;
    float2 id = floor(q);
    float h = Hash(id);
    float2 centre = 0.5 + (float2(h, Hash(id + 17.0)) - 0.5) * 0.3;
    float2 d = frac(q) - centre;
    float r = length(d);
    float phase = frac(vec4Wet.w * rate + h);
    float x = r - 0.4 * phase;
    float profile = cos(40.0 * x * cell) * exp(-400.0 * x * x * cell * cell) * (1.0 - phase) * (1.0 - phase);
    return d / max(r, 1e-3) * profile;
}

Targets Wet_PS(float2 uv : TEXCOORD0, float2 vpos : VPOS)
{
    Targets o;
    o.albedo = tex2D(AlbedoTex, uv);
    o.normal = tex2D(NormalTex, uv);
    o.specular = tex2D(SpecularTex, uv);

    float raw = tex2D(DepthTex, uv).r;
    [branch]
    if (raw <= 0.0 || raw >= 0.99999)
        return o; // sky

    // The category: the ID in whole steps, less its 128 and 8 bits.
    float id = floor(tex2D(MaterialTex, uv).r * 255.0 + 0.4);
    id -= id >= 128.0 ? 128.0 : 0.0;
    float category = fmod(id, 8.0);
    float allow = dot(vec4Allow0, float4(category == 0.0, category == 1.0, category == 2.0, category == 3.0)) +
                  dot(vec4Allow1, float4(category == 4.0, category == 5.0, category == 6.0, category == 7.0));

    [branch]
    if (fDebug == 1.0)
    {
        // Categories 0..7 as colours, darker with the 8 bit set.
        static const float3 kColours[8] = { float3(0.6, 0.6, 0.6), float3(1, 0, 0), float3(0, 1, 0), float3(0, 0, 1),
                                            float3(1, 1, 0), float3(1, 0, 1), float3(0, 1, 1), float3(1, 0.5, 0) };
        float3 c = kColours[0];
        for (int i = 1; i < 8; ++i)
            c = category == i ? kColours[i] : c;
        float rawId = floor(tex2D(MaterialTex, uv).r * 255.0 + 0.4);
        c *= fmod(rawId, 16.0) >= 8.0 ? 0.45 : 1.0;
        c = rawId >= 128.0 ? c.bgr : c;
        o.albedo.rgb = c;
        o.specular.xy = 0.0;
        return o;
    }

    float4 g = o.normal;
    float3 f = frac(g.w * float3(0.998046875, 7.984375, 63.875));
    f.xy -= f.yz * 0.125;
    float3 N = normalize(g.xyz * 256.0 + f - 127.999992);

    float sky = smoothstep(0.25, 0.6, o.specular.z);
    float wet = vec4Wet.x * allow * sky;
    [branch]
    if (wet <= 0.0)
    {
        if (fDebug == 2.0)
        {
            o.albedo.rgb = float3(0.0, 0.0, allow * 0.3);
            o.specular.xy = 0.0;
        }
        return o;
    }

    float z = pow(abs(fFarDivNear), raw) * fNearPlane;
    float3 v = float3((vpos + 0.5) * vec4ProjInfo.xy + vec4ProjInfo.zw, 1.0);
    float3 dir = float3(dot(v, vec4WorldX.xyz), dot(v, vec4WorldY.xyz), dot(v, vec4WorldZ.xyz));
    float3 P = float3(vec4WorldX.w, vec4WorldY.w, vec4WorldZ.w) + dir * z;
    float distance = length(dir * z);

    // Ground faces up; walls keep a third of the darkening and no sheen or puddles. Puddles fade out
    // from about 3 degrees of slope to 6.
    float ground = smoothstep(0.7, 0.95, N.z);
    float flat = smoothstep(0.9945, 0.9986, N.z);

    // Two octaves of the map, the second breaking up the first's edges.
    float n = tex2D(NoiseTex, P.xy * vec4Shape.x).r * 0.8 + tex2D(NoiseTex, P.xy * vec4Shape.x * 3.7 + 0.31).r * 0.2;
    float threshold = 1.0 - vec4Wet.y * wet;
    float puddle = flat * smoothstep(threshold, threshold + 0.06, n);

    // Darker: a third on walls, all of it on the ground, more in water.
    float darken = wet * lerp(0.33, 1.0, ground) * vec4Shape.w;
    o.albedo.rgb *= 1.0 - darken * 0.4;
    o.albedo.rgb *= 1.0 - puddle * 0.5;

    // Gloss as sqrt(power / 512), specular intensity halved, as the G-buffer stores them: a wet film
    // to about power 150, water to about 410. The game multiplies the sky's reflection by ten times
    // twice the specular intensity, so water takes little more than the film: at 0.5 puddles at
    // night were pale grey sheets of reflected sky, and every ring sparkled.
    float filmGloss = lerp(o.specular.y, max(o.specular.y, 0.55), wet * ground);
    float filmSpec = lerp(o.specular.x, max(o.specular.x, 0.12), wet * ground);
    o.specular.y = lerp(filmGloss, 0.9, puddle);
    o.specular.x = lerp(filmSpec, 0.15, puddle);

    // The water's surface is level, and the rain rings it.
    float3 Nw = normalize(lerp(N, float3(0.0, 0.0, 1.0), puddle));
    float near = saturate((vec4Shape.z - distance) / (vec4Shape.z * 0.6));
    float ringStrength = vec4Shape.y * vec4Wet.z * puddle * near;
    float2 tilt = 0.0;
    [branch]
    if (ringStrength > 0.0)
    {
        tilt = Rings(P.xy, 0.35, 1.0, 0.0) + Rings(P.xy, 0.35 * 1.37, 0.81, 5.3) + Rings(P.xy, 0.35 * 0.83, 1.23, 11.7);
        Nw = normalize(Nw + float3(tilt * ringStrength * 0.3, 0.0));
    }
    o.normal.xyz = Nw * 0.5 + 0.5;

    [branch]
    if (fDebug == 2.0)
    {
        o.albedo.rgb = float3(wet, puddle, saturate(abs(tilt.x) + abs(tilt.y)) * (ringStrength > 0.0));
        o.specular.xy = 0.0;
    }
    return o;
}

technique Wet
{
    pass P0
    {
        PixelShader = compile ps_3_0 Wet_PS();
    }
}
