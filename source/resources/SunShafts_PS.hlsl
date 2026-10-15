// Settings
#define NUM_SAMPLES 24
#define USE_CLOUD_MASK 1
#define USE_HORIZON_FADE 1

// Aliases
#define Intensity        FusionPS208.x // Overall intensity of the effect.
#define Density          FusionPS208.y // How long the rays are.
#define Decay            FusionPS208.z // How fast the rays fade out from the center.
#define FlareIntensity   FusionPS208.w // Sun lens flare's strength, 0 off (Sun Flare in the graphics menu).
#define Exposure         1.0f          // Effectively the same as Intensity, but the value is squared with two passes.
#define Weight           1.0f          // Same as Exposure, but only affects offset samples and not the center.

#define VolumetricFog    FusionPS210.y
#define SunDirection     float4(FusionPS216.xyz, 0.0f)
#define TexelSize        globalScreenSize.zw
#define AspectRatio      globalScreenSize.x * globalScreenSize.wz
#define CameraPos        gViewInverse[3].xyz
#define LogDepthParams   FusionPS209

// Vanilla resources
uniform row_major float4x4 gWorldViewProj : register(c8);
uniform row_major float4x4 gViewInverse   : register(c12);
uniform float4 globalScreenSize           : register(c44);     // This is at full resolution.
sampler2D GBufferTextureSampler3          : register(s1);
sampler2D HDRSampler                      : register(s2);

// Custom resources
uniform float4 FusionPS208                : register(c208);
uniform float4 FusionPS209                : register(c209);
uniform float4 FusionPS210                : register(c210);
uniform float4 FusionPS216                : register(c216);
sampler2D pHDRDownsampleTex               : register(s11);     // Custom downsample.
sampler2D DiffuseTex                      : register(s13);     // Diffuse; cloud mask.

// From rage_postfxVS0.
struct PS_IN
{
    float2 texcoord : TEXCOORD;
    float3 viewray : TEXCOORD1;
};

float DecodeLogDepth(float depth)
{
    return pow(abs(LogDepthParams.z), depth) * LogDepthParams.w;
}

float2 getScreenLightPos()
{
    float4 pos = mul(SunDirection, gWorldViewProj);
    pos.xy = float2(pos.x, -pos.y) / pos.w;
    return pos.xy * 0.5f + 0.5f;
}

float2 FixHalfPixelOffset(float2 uv)
{
    // The game applies the half pixel offset on the CPU, so it still uses the full resolution value in our custom half resolution passes.
    // Undo full resolution half pixel offset.
    uv -= 0.5f * TexelSize.xy;
    // Add half resolution half pixel offset.
    uv += 0.5f * TexelSize.xy * 2.0f;
    return uv;
}

float SunMask(float3 viewray, float depth)
{
    float3 viewdir = normalize(DecodeLogDepth(depth) * -viewray);
    float mask = dot(viewdir, SunDirection.xyz);
    // Sun baseline = ~saturate(mask * 249.995773315f - 248.995773315f).
    return mask >= 0.996f ? 1.0f : 0.0f;
}

float SkyMask(float depth)
{
    if (VolumetricFog == 1.0f)
        // Hack, includes some of the depth range if volumetric fog is enabled.
        return saturate(depth * 10.0f - 9.0f);
    else
        return depth == 1.0f ? 1.0f : 0.0f;
}

float EdgeFade(float2 uv)
{
    float2 rectangle = min(uv, 1.0f - uv) * AspectRatio;
    return saturate(32.0f * min(rectangle.x, rectangle.y));
}

float HorizonFade()
{
    // Fade as the sun rises or sets.
    // ~0.209101 = 07:00 / 19:00
    float azimuth = smoothstep(0.0f, 0.209101f, SunDirection.z);
    // Restore visibility at high altitudes.
    float altitude = 1.0f - exp2(-0.01f * max(CameraPos.z, 0.0f));
    return max(azimuth, altitude);
}

float4 SSPrepass(PS_IN i) : COLOR
{
    float2 uv = FixHalfPixelOffset(i.texcoord.xy);
    
    float3 color = tex2D(HDRSampler, uv).xyz;
    float depth = tex2D(GBufferTextureSampler3, uv).x;
    
    // Restrict to sky.
    color *= SkyMask(depth);
    // Restrict to sun.
    color *= SunMask(i.viewray.xyz, depth);
    // Fade out at the edges of the screen.
    color *= EdgeFade(uv);
    // Multiply by TimecycExt intensity.
    color *= Intensity;
    
    // Apply clouds occlusion.
    #if USE_CLOUD_MASK
        float clouds = tex2D(DiffuseTex, uv).x;
        color *= clouds;
    #endif
    // Fade out near the horizon if volumetric fog is enabled.
    #if USE_HORIZON_FADE
        color *= VolumetricFog == 1.0f ? HorizonFade() : 1.0f;
    #endif
    
    return float4(color, 1.0f);
}

float4 SSDraw(PS_IN i) : COLOR
{
    // https://developer.nvidia.com/gpugems/gpugems3/part-ii-light-and-shadows/chapter-13-volumetric-light-scattering-post-process
    
    float2 uv = FixHalfPixelOffset(i.texcoord.xy);

    // Calculate vector from pixel to light source in screen space.
    float2 deltaTexCoord = uv - getScreenLightPos();
    // Divide by number of samples and scale by control factor.
    deltaTexCoord *= (1.0f / NUM_SAMPLES) * Density;
    // Store initial sample.
    float3 color = tex2D(pHDRDownsampleTex, uv).xyz;
    // Set up illumination decay factor.
    float illuminationDecay = 1.0f;
    // Evaluate summation from Equation 3 NUM_SAMPLES iterations.
    [unroll]
    for (int i = 0; i < NUM_SAMPLES; i++)
    {
        // Step sample location along ray.
        uv -= deltaTexCoord;
        // Retrieve sample at new location.
        float3 sample = tex2D(pHDRDownsampleTex, uv).xyz;
        // Apply sample attenuation scale/decay factors.
        sample *= illuminationDecay * Weight;
        // Accumulate combined color.
        color += sample;
        // Update exponential decay factor.
        illuminationDecay *= Decay;
    }
    // Output final color with a further scale control factor.
    return float4(color * Exposure, 1.0f);
}

// A camera lens's flare from the sun, drawn as the shafts are added: a soft core and an eight spike starburst
// around the sun, and ghosts along the line from the sun through the screen's centre, coloured like the coatings
// of a lens (amber, green, magenta, blue), their edges split into red, green and blue as a lens disperses them.
// Its light is the frame's own at the sun, five reads around it, where the sky shows it and the clouds let it
// through, so buildings, bridges and clouds put it out as they cover the sun.
float SunVisible(float2 uv)
{
    float depth = tex2Dlod(GBufferTextureSampler3, float4(uv, 0, 0)).x;
    float visible = SkyMask(depth);
    #if USE_CLOUD_MASK
        visible *= tex2Dlod(DiffuseTex, float4(uv, 0, 0)).x;
    #endif
    return visible;
}

float Ghost(float2 d, float radius)
{
    return smoothstep(radius, radius * 0.6f, length(d));
}

float3 SunFlare(float2 uv)
{
    float4 pos = mul(SunDirection, gWorldViewProj);
    if (FlareIntensity <= 0.0f || pos.w <= 0.0f)
        return 0.0f;
    float2 sun = float2(pos.x, -pos.y) / pos.w * 0.5f + 0.5f;
    float onScreen = saturate(20.0f * min(min(sun.x, 1.0f - sun.x), min(sun.y, 1.0f - sun.y)));
    if (onScreen <= 0.0f)
        return 0.0f;

    // The sun's light where it shows
    float2 spread = 4.0f * TexelSize;
    float3 light = 0.0f;
    float shown = 0.0f;
    const float2 taps[5] = { float2(0, 0), float2(1, 0), float2(-1, 0), float2(0, 1), float2(0, -1) };
    [unroll]
    for (int t = 0; t < 5; t++)
    {
        float2 at = sun + taps[t] * spread;
        float v = SunVisible(at);
        light += tex2Dlod(HDRSampler, float4(at, 0, 0)).rgb * v;
        shown += v;
    }
    light *= 0.2f * onScreen * FlareIntensity;
    #if USE_HORIZON_FADE
        light *= VolumetricFog == 1.0f ? HorizonFade() : 1.0f;
    #endif
    if (shown <= 0.0f)
        return 0.0f;

    // Distances on the screen, in heights
    float2 aspect = float2(AspectRatio.x, 1.0f);
    float2 d = (uv - sun) * aspect;
    float r = length(d);
    float angle = atan2(d.y, d.x);
    float core = exp(-r * 40.0f) * 0.6f + exp(-r * 8.0f) * 0.08f;
    float spikes = pow(abs(cos(angle * 4.0f)), 96.0f) * exp(-r * 9.0f) * 0.12f;
    float3 flare = (core + spikes) * light;

    // Ghosts: where along the sun's line through the centre (0 the sun, 1 the centre, 2 its mirror), radius, colour
    float2 axis = (0.5f - sun);
    const float4 ghosts[6] = { float4(0.6f, 0.05f, 0, 0), float4(1.25f, 0.035f, 0, 0), float4(1.5f, 0.09f, 0, 0),
                               float4(1.8f, 0.06f, 0, 0), float4(2.1f, 0.14f, 0, 0), float4(2.5f, 0.04f, 0, 0) };
    const float3 tints[6] = { float3(1.0f, 0.6f, 0.25f), float3(0.35f, 1.0f, 0.5f), float3(0.9f, 0.35f, 0.9f),
                              float3(0.4f, 0.6f, 1.0f), float3(1.0f, 0.75f, 0.4f), float3(0.6f, 1.0f, 0.9f) };
    float3 ghostSum = 0.0f;
    [unroll]
    for (int g = 0; g < 6; g++)
    {
        float2 centre = sun + axis * ghosts[g].x;
        float2 gd = (uv - centre) * aspect;
        float radius = ghosts[g].y;
        float3 split = float3(Ghost(gd, radius * 1.04f), Ghost(gd, radius), Ghost(gd, radius * 0.96f));
        ghostSum += split * tints[g];
    }
    // Fainter as the sun nears the centre, where real ghosts bunch up behind its glare
    flare += ghostSum * light * 0.01f * saturate(length(axis * aspect) * 3.0f);
    return flare;
}

float4 SSAdd(PS_IN i) : COLOR
{
    float4 color = tex2D(HDRSampler, i.texcoord) + tex2D(pHDRDownsampleTex, i.texcoord);
    color.rgb += SunFlare(i.texcoord);
    return color;
}
