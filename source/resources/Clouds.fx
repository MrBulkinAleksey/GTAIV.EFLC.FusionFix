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
// reaches and the cloud's inscattering range for the bright rim towards the sun. What the sun lights
// also takes the hue of the game's sun colour.

// Steps a ray may take in all, coarse and fine.
#ifndef CLOUD_STEPS
#define CLOUD_STEPS 128
#endif
// A coarse step in fine steps, and the fine steps in a row that must find no cloud to go coarse again.
#define COARSE_STEP 4.0
#define FINE_MISSES 4.0
#ifndef LIGHT_STEPS
#define LIGHT_STEPS 3
#endif
// CloudNoiseTex's size.
#define COVERAGE_SIZE 256.0

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
float3 vec3SunTint;       // the hue of the game's SunColor at its brightness 1, mixed towards white by VolumetricCloudsSunTint
float4 vec4Layer;         // base height, thickness, 1 / coverage scale, coverage
float4 vec4Wind;          // coverage offset (xy), detail offset (zw)
float4 vec4Shape;         // extinction per metre at full density, 1 / detail scale, detail strength, haze distance
float fMaxDistance;
float fStratus;           // 0 separate heaps of cloud, 1 a sheet: the weather's overcast
float fEvolution;         // how far the detail has drifted up through itself, so the billows change
float fTranslucency;      // how much less the thinnest cloud hides of what is behind it
float fFrameJitter;       // the frame's share of a step, so the march's noise changes every frame
float fWarp;              // how far, in coverage texture units, the outline wanders with height

float3 ViewRay(float2 pixel)
{
    return float3((pixel + 0.5) * vec4ProjInfo.xy + vec4ProjInfo.zw, 1.0);
}

// Interleaved gradient noise in [0, 1), fixed per pixel.
float PixelJitter(float2 pixel)
{
    return frac(52.9829189 * frac(dot(pixel, float2(0.06711056, 0.00583715))));
}

// Density 0 to 1 at p.
// - The coverage map is Perlin-Worley noise: cellular, so clouds come out as round heaps with clear
//   sky between them; plain value noise gave blurred blots that rose to points on its peaks.
// - Cloud is where the map is above (1 - cover)^2.
// - With height the density is lowered by up to a quarter and fades out over the top fifth, so a
//   heap narrows towards a rounded top; at the base it is half as dense, which softens the bottom.
// - The detail erodes only near the edges, where the density is low: round Worley billows of about
//   a sixteenth to a quarter of DetailScale, and a finer octave at about a fifth of that.
// - Last a soft compressor, d (1 + k) / (1 + k d) with k from 3 at the base to 12 at the top, makes
//   the inside dense quickly while the edges stay soft; the linear ramp it replaces cut the clouds'
//   faces like moulded plastic.
float EdgeWeight(float d)
{
    float x = saturate(1.0 - d);
    return x * (2.0 - x);
}

float Density(float3 p, bool detail)
{
    float h = (p.z - vec4Layer.x) / vec4Layer.y;
    if (h <= 0.0 || h >= 1.0)
        return 0.0;
    float2 uv = p.xy * vec4Layer.z + vec4Wind.xy;
    // The outline wanders with height: the coverage is read a little off, by a coarse octave of the
    // detail that changes up through the layer, so the heaps do not stand as walls drawn up from a
    // map. The steps towards the sun and the coarse search leave it out.
    [branch]
    if (detail)
    {
        float3 w = p * (vec4Shape.y * 0.35) + float3(vec4Wind.zw, fEvolution);
        uv += (float2(tex3Dlod(DetailTex, float4(w, 0)).r, tex3Dlod(DetailTex, float4(w.yzx + 0.41, 0)).r) - 0.5) * fWarp;
    }
    // Filtered with a quintic curve between texels: linear filtering's kinks at the texel edges stood
    // out as creases down the clouds' sides.
    float2 texel = uv * COVERAGE_SIZE - 0.5;
    float2 cell = floor(texel);
    float2 f = texel - cell;
    f = f * f * f * (f * (f * 6.0 - 15.0) + 10.0);
    float c = tex2Dlod(CoverageTex, float4((cell + f + 0.5) / COVERAGE_SIZE, 0, 0)).r;

    float cover = max(vec4Layer.w, 0.02);
    // Overcast: the map evens out towards a sheet.
    c = lerp(c, max(c, 1.0 - cover * 0.5), fStratus);
    float threshold = (1.0 - cover) * (1.0 - cover);
    float d = saturate((c - threshold) / max(1.0 - threshold, 0.01));
    d = d * smoothstep(1.0, 0.8, h) - smoothstep(0.06, 0.95, h) * 0.25 * (1.0 - fStratus);
    if (d <= 0.0)
        return 0.0;

    [branch]
    if (detail)
    {
        float3 q = p * vec4Shape.y + float3(vec4Wind.zw, fEvolution);
        // Our Worley volume is 1 at the cells' middles: 1 - n is high between the billows.
        float n = 1.0 - tex3Dlod(DetailTex, float4(q, 0)).r;
        d -= vec4Shape.z * 0.66 * n * n * EdgeWeight(d);
        float m = 1.0 - tex3Dlod(DetailTex, float4(q * 5.5 + 0.37, 0)).r;
        d -= vec4Shape.z * 0.3 * m * (0.6 * m + 0.4) * EdgeWeight(d);
        if (d <= 0.0)
            return 0.0;
    }

    float k = lerp(3.0, 12.0, h);
    d = d * (1.0 + k) / (1.0 + k * d);
    return d * lerp(0.5, 1.0, smoothstep(0.02, 0.2, h));
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
    // Beyond sixteen thicknesses into the layer the haze hides the cloud anyway.
    t1 = min(t1, t0 + vec4Layer.y * 16.0);

    float cosTheta = dot(dir, vec3SunDir);
    float phase = lerp(HenyeyGreenstein(cosTheta, -0.25), HenyeyGreenstein(cosTheta, 0.6), 0.7);
    // A narrow forward lobe: cloud next to the sun in the sky glows where it is thin enough for its
    // light to come through, the bright gold rims of clouds against the sun.
    float forward = HenyeyGreenstein(cosTheta, 0.85);
    float sigma = vec4Shape.x;

    // Empty sky is crossed in coarse steps that test the coverage alone; on finding cloud the ray
    // steps back and marches it in fine steps, a 24th of the layer's thickness near the camera and
    // a hundredth of the distance further out, until FINE_MISSES fine steps in a row find none.
    // With even steps over the whole crossing a step near the horizon was 150 m long, and the
    // clouds came out smeared down the screen.
    // Each pixel starts up to a coarse step later: with the same coarse steps for every pixel,
    // thin cloud between two of them went missing in whole bands across the screen.
    float jitter = frac(PixelJitter(vpos) + fFrameJitter);
    float t = t0 + max(vec4Layer.y / 24.0, t0 * 0.01) * COARSE_STEP * jitter;
    float fineLeft = 0.0;
    float transmittance = 1.0;
    float3 colour = 0.0;
    float firstHit = -1.0;

    // [fastopt]: without it D3DX spent close to a minute on this loop while the game loaded.
    [loop] [fastopt]
    for (int i = 0; i < CLOUD_STEPS; ++i)
    {
        if (t >= t1 || transmittance < 0.01)
            break;
        float fine = max(vec4Layer.y / 24.0, t * 0.01);
        float3 p = origin + dir * t;

        [branch]
        if (fineLeft <= 0.0)
        {
            if (Density(p, false) > 0.0)
            {
                // Back by one coarse step, to march into the cloud from outside it. The fine steps
                // then run at least past where the cloud was found and FINE_MISSES more: stopping
                // as soon as they missed, where the detail had eaten the cloud at that point, the
                // coarse step found it again, stepped back again and spent the ray going in circles.
                t = max(t - fine * COARSE_STEP, t0);
                fineLeft = COARSE_STEP + FINE_MISSES;
            }
            else
                t += fine * COARSE_STEP;
            continue;
        }

        float d = Density(p, true);
        [branch]
        if (d > 0.01)
        {
            fineLeft = FINE_MISSES;
            if (firstHit < 0.0)
                firstHit = t;

            // The cloud between the sample and the sun, without the detail.
            float lightDepth = 0.0;
            float stepLength = vec4Layer.y * 0.08;
            float3 q = p;
            [loop] [fastopt]
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
            // A third of the extinction: past the sun's direct beam the light inside a cloud is
            // scattered forwards mostly, so it gets through far more cloud than the eye's view does.
            float tau = lightDepth * sigma * 0.33;
            float sun = (exp(-tau) + 0.5 * exp(-0.5 * tau) + 0.25 * exp(-0.25 * tau)) / 1.75;
            // Darker towards the base, where the sky above is hidden by the cloud itself, and
            // under more cloud: one sample a quarter of the layer straight up.
            float h = saturate((p.z - base) / vec4Layer.y);
            float skyAbove = exp(-2.0 * Density(p + float3(0.0, 0.0, vec4Layer.y * 0.25), false));
            float3 shade = vec3ShadeColour * lerp(0.7, 1.0, sqrt(h)) * lerp(0.75, 1.0, skyAbove);
            // The sun lights what it reaches, a little more facing it; past that the forward lobe
            // adds the rim, scaled by the game's inscattering range.
            // The parts the sun reaches take its hue: warm in the evening, orange at sunset.
            float3 sunLit = vec3LitColour * vec3SunTint;
            float3 lit = lerp(shade, sunLit, sun * (0.6 + 0.4 * min(phase, 1.0)));
            lit += sunLit * sun * max(phase - 1.0, 0.0) * 0.1 * fSilver;
            // The glow, most where the cloud is thin; past the scene's white point, for the bloom.
            lit += sunLit * exp(-tau * 0.5) * (1.0 - d) * min(forward * 0.02 * fSilver, 1.5);

            // Thin cloud lets more of what is behind it through, the wisps at the edges most.
            float stepTransmittance = exp(-d * lerp(1.0 - fTranslucency, 1.0, d) * sigma * fine);
            colour += transmittance * (1.0 - stepTransmittance) * lit;
            transmittance *= stepTransmittance;
        }
        else
            fineLeft -= 1.0;
        t += fine;
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
