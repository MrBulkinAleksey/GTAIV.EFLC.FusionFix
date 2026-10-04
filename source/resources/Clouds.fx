// Volumetric clouds (RenderVolumetricClouds in postfx.ixx): one layer of cloud marched from the camera
// and blended into the lit scene right before the fog pass, so the fog, SSR's history and everything
// after see them. Wherever the scene is nearer than the cloud the ray stops at it, so clouds show over
// the sky and over anything far enough behind them alike.
//
// The layer's coverage is CoverageTex, the same tiling noise, scale and wind as the cloud shadows on
// the ground, so each shadow lies under its cloud. DetailTex, a tiling Worley volume, erodes the edges.
// Light: the sun reaching each sample through the cloud above it (Beer's law with a softer second lobe
// for the light scattered more than once), a two lobe phase function for the bright rim towards the
// sun, and the colours the game's own clouds take from the timecycle, the way gta_atmoscatt_clouds
// mixes them: its cloud colour darkened in the shade, with the sunset colour added where the sun
// reaches and the cloud's inscattering range for the bright rim towards the sun.

#ifndef CLOUD_STEPS
#define CLOUD_STEPS 32
#endif
#ifndef LIGHT_STEPS
#define LIGHT_STEPS 3
#endif

sampler2D DepthTex : register(s0);
sampler2D CoverageTex : register(s1);
sampler3D DetailTex : register(s2);

float2 vec2InvViewportSize;
float4 vec4ProjInfo;
float fNearPlane;
float fFarDivNear;

// The view's axes in world space (xyz of each row is a column of the view to world rotation) and the
// camera position (w).
float4 vec4WorldX;
float4 vec4WorldY;
float4 vec4WorldZ;

float3 vec3SunDir;        // world, towards the sun
float3 vec3LitColour;     // the game's cloud colour plus its sunset colour, exposed
float3 vec3ShadeColour;   // the game's cloud colour darkened, exposed
float fSilver;            // the game's CloudInscatteringRange: the rim towards the sun
float4 vec4Layer;         // base height, thickness, 1 / coverage scale, coverage
float4 vec4Wind;          // coverage offset (xy), detail offset (zw)
float4 vec4Shape;         // extinction per metre at full density, 1 / detail scale, detail strength, haze distance
float fMaxDistance;

float3 ViewRay(float2 pixel)
{
    return float3((pixel + 0.5) * vec4ProjInfo.xy + vec4ProjInfo.zw, 1.0);
}

// Interleaved gradient noise in [0, 1), fixed per pixel.
float PixelJitter(float2 pixel)
{
    return frac(52.9829189 * frac(dot(pixel, float2(0.06711056, 0.00583715))));
}

// Density 0 to 1 at p. The coverage sets where cloud is; higher up in the layer it takes ever denser
// coverage to stay cloud, so each cloud narrows towards its top into a dome. With the same coverage
// at every height the sides stood straight up and the clouds looked like towers. Towards the base it
// takes denser coverage again, so the bottoms curve up towards the edges instead of being cut flat.
// Past its edge a cloud turns dense within a third of the way to full coverage: spread over all of
// it, the edges were hundreds of metres of haze.
// The detail then cuts billows into it: where the Worley noise is low, between its cells, the
// density is lowered and what is left stretched back to 0 to 1, so the billows keep crisp edges.
float Density(float3 p, bool detail)
{
    float h = (p.z - vec4Layer.x) / vec4Layer.y;
    float c = tex2Dlod(CoverageTex, float4(p.xy * vec4Layer.z + vec4Wind.xy, 0, 0)).r;
    float cover = max(vec4Layer.w, 0.02);
    float bottom = saturate(1.0 - h * 4.0);
    float threshold = (1.0 - cover) + cover * (0.8 * h * h + 0.5 * bottom * bottom);
    float d = saturate((c - threshold) / max((1.0 - threshold) * 0.35, 0.02)) * saturate(h * 20.0) * saturate((1.0 - h) * 10.0);
    [branch]
    if (detail && d > 0.0)
    {
        float n = tex3Dlod(DetailTex, float4(p * vec4Shape.y + float3(vec4Wind.zw, 0.0), 0)).r;
        float erode = (1.0 - n) * vec4Shape.z;
        d = saturate((d - erode) / max(1.0 - erode, 0.05));
    }
    return d;
}

float HenyeyGreenstein(float cosTheta, float g)
{
    float g2 = g * g;
    return (1.0 - g2) / pow(max(1.0 + g2 - 2.0 * g * cosTheta, 1e-4), 1.5);
}

float4 Clouds_PS(float2 uv : TEXCOORD0, float2 vpos : VPOS) : COLOR0
{
    float3 v = ViewRay(vpos);
    float3 dir = float3(dot(v, vec4WorldX.xyz), dot(v, vec4WorldY.xyz), dot(v, vec4WorldZ.xyz));
    float rayScale = length(dir);
    dir /= rayScale;
    float3 origin = float3(vec4WorldX.w, vec4WorldY.w, vec4WorldZ.w);

    // Where the ray is inside the layer, cut short by the scene in front.
    float base = vec4Layer.x;
    float top = vec4Layer.x + vec4Layer.y;
    float dz = abs(dir.z) > 1e-4 ? dir.z : 1e-4;
    float tBase = (base - origin.z) / dz;
    float tTop = (top - origin.z) / dz;
    float t0 = max(min(tBase, tTop), 0.0);
    float t1 = min(max(tBase, tTop), fMaxDistance);
    // The sky leaves the depth target as it was cleared, at one end or the other.
    float rawDepth = tex2Dlod(DepthTex, float4(uv, 0, 0)).r;
    if (rawDepth > 0.0 && rawDepth < 0.99999)
        t1 = min(t1, pow(fFarDivNear, rawDepth) * fNearPlane * rayScale);
    if (t1 <= t0)
        return float4(0.0, 0.0, 0.0, 1.0);
    // Near the horizon a ray crosses tens of kilometres of the layer, which the steps sampled as
    // stripes. It marches no further than eight thicknesses into it; beyond, the haze hides it.
    t1 = min(t1, t0 + vec4Layer.y * 8.0);

    float dt = (t1 - t0) / CLOUD_STEPS;
    float t = t0 + dt * PixelJitter(vpos);
    float cosTheta = dot(dir, vec3SunDir);
    float phase = lerp(HenyeyGreenstein(cosTheta, -0.25), HenyeyGreenstein(cosTheta, 0.6), 0.7);
    float sigma = vec4Shape.x;

    float transmittance = 1.0;
    float3 colour = 0.0;
    float firstHit = -1.0;

    [loop]
    for (int i = 0; i < CLOUD_STEPS; ++i)
    {
        float3 p = origin + dir * t;
        float d = Density(p, true);
        [branch]
        if (d > 0.01)
        {
            if (firstHit < 0.0)
                firstHit = t;

            // The cloud between the sample and the sun, without the detail.
            float lightDepth = 0.0;
            float stepLength = vec4Layer.y * 0.08;
            float3 q = p;
            [loop]
            for (int j = 0; j < LIGHT_STEPS; ++j)
            {
                q += vec3SunDir * stepLength;
                lightDepth += Density(q, false) * stepLength;
                stepLength *= 2.0;
            }
            // Light scattered many times inside a cloud gets far deeper than the sun's direct beam,
            // which is what keeps real clouds bright: three octaves, each with half the extinction
            // and half the weight of the one before. With the direct beam alone the sun barely
            // reached the faces seen from below and the clouds came out dark.
            float tau = lightDepth * sigma;
            float sun = (exp(-tau) + 0.5 * exp(-0.5 * tau) + 0.25 * exp(-0.25 * tau)) / 1.75;
            // Darker towards the base, where the sky above is hidden by the cloud itself.
            float h = saturate((p.z - base) / vec4Layer.y);
            float3 shade = vec3ShadeColour * lerp(0.7, 1.0, sqrt(h));
            // The sun lights what it reaches, a little more facing it; past that the forward lobe
            // adds the rim, scaled by the game's inscattering range.
            float3 lit = lerp(shade, vec3LitColour, sun * (0.6 + 0.4 * min(phase, 1.0)));
            lit += vec3LitColour * sun * max(phase - 1.0, 0.0) * 0.1 * fSilver;

            float stepTransmittance = exp(-d * sigma * dt);
            colour += transmittance * (1.0 - stepTransmittance) * lit;
            transmittance *= stepTransmittance;
            if (transmittance < 0.01)
                break;
        }
        t += dt;
    }

    // Haze: distant cloud fades into what is behind it.
    float haze = firstHit >= 0.0 ? exp(-firstHit / vec4Shape.w) : 0.0;
    float cover = (1.0 - transmittance) * haze;
    return float4(colour * haze, 1.0 - cover);
}

void FullscreenQuadVS(in float4 iPos : POSITION, in float2 iUV : TEXCOORD0,
                      out float4 oPos : POSITION, out float2 oUV : TEXCOORD0)
{
    oPos = iPos;
    oUV = iUV;
}

technique Clouds
{
    pass Clouds
    {
        VertexShader = compile vs_3_0 FullscreenQuadVS();
        PixelShader = compile ps_3_0 Clouds_PS();
    }
}
