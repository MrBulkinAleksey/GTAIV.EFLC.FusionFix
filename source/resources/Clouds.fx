// Volumetric clouds (RenderVolumetricClouds in postfx.ixx): one layer of cloud marched from the camera at
// half size (Clouds), accumulated over frames (CloudsResolve) and blended into the lit scene at full
// size right before the fog pass (CloudsComposite), so the fog, SSR's history and everything after see
// them. Wherever the scene is nearer than the cloud the ray stops at it, so clouds show over
// the sky and over anything far enough behind them alike.
//
// The layer's coverage is CoverageTex, the same tiling noise, scale and wind as the cloud shadows on
// the ground, so each shadow lies under its cloud. DetailTex, a tiling Worley volume, erodes the edges.
// Light: the sun reaching each sample through the cloud above it (Beer's law with softer lobes for the
// light scattered more than once), lit the way gta_atmoscatt_clouds lights its own clouds:
// CloudColor, brightened by up to CloudInscatteringRange along the sun's axis (cos^2) where the cloud
// is thin, darkened where the sun does not reach, plus SunsetColor on the side towards the sun. A
// forward lobe some 30 degrees wide adds the glow of thin cloud around the sun, and a soft knee rolls
// the brightest of it off below a ceiling, so it neither clips to white nor flattens the shading.
// What the sun lights also takes the hue of the game's sun colour.

// Steps a ray may take in all, coarse and fine.
#ifndef CLOUD_STEPS
#define CLOUD_STEPS 128
#endif
// A coarse step in fine steps, and the fine steps in a row that must find no cloud to go coarse again.
#define COARSE_STEP 4.0
#define FINE_MISSES 4.0
#ifndef LIGHT_STEPS
#define LIGHT_STEPS 5
#endif
// CloudNoiseTex's size.
#define COVERAGE_SIZE 1024.0

sampler2D DepthTex : register(s0);
sampler2D CoverageTex : register(s1);
sampler3D DetailTex : register(s2);
sampler2D CurrentTex : register(s3);   // this frame's clouds at half size (CloudsResolve)
sampler2D HistoryTex : register(s4);   // the clouds accumulated up to last frame (CloudsResolve)
sampler2D CloudTex : register(s5);     // the accumulated clouds (CloudsComposite)
sampler2D SceneTex : register(s6);     // the lit scene, the sky in it, behind the clouds (Clouds at half size)

float2 vec2InvViewportSize;
float4 vec4ProjInfo;
float fNearPlane;
float fFarDivNear;

// The view's axes in world space (xyz of each row is a column of the view to world rotation) and the
// camera position (w).
float4 vec4WorldX;
float4 vec4WorldY;
float4 vec4WorldZ;

float3 vec3SunDir;        // world, towards the sun, or the moon at night
float3 vec3LitColour;     // the game's cloud colour, exposed
float3 vec3ShadeColour;   // the game's cloud colour darkened by VolumetricCloudsShade, exposed
float3 vec3SunsetColour;  // the game's sunset colour, exposed
float fSilver;            // the game's CloudInscatteringRange: the brightening along the sun's axis
float fLightStrength;     // the sun's light, fading out below the horizon, or the moon's once it has handed over
float fCeiling;
// VolumetricCloudsSkyMatch: how many times brighter than the sky behind them the clouds' sunlit side
// is; 0 leaves them at the game's cloud colour. fLitLuma is that sunlit side's luma.
float fSkyMatch;
float fLitLuma;           // the brightest channel the cloud rolls off towards (at most the sky's clamp without HDR)
float3 vec3SunTint;       // the hue of the game's SunColor at its brightness 1, mixed towards white by VolumetricCloudsSunTint
float4 vec4Layer;         // base height, thickness, 1 / coverage scale, coverage
float4 vec4Wind;          // coverage offset (xy), detail offset (zw)
float4 vec4Shape;         // extinction per metre at full density, 1 / detail scale, detail strength, haze distance
float fMaxDistance;
float2 vec2DepthTexel;    // one texel of the full size depth target, in texture coordinates
// Last frame's view projection without translation or jitter, by columns (xyz) of its x, y and w, to
// find where a direction stood then; vec4History.x is 1 when there is a history to use, .y the share
// of this frame, .zw one texel of the half size targets.
float4 vec4PrevX;
float4 vec4PrevY;
float4 vec4PrevW;
float4 vec4History;
float fStratus;           // 0 separate heaps of cloud, 1 a sheet: the weather's overcast
float fEvolution;         // how far the detail has drifted up through itself, so the billows change
float fTranslucency;      // how much less the thinnest cloud hides of what is behind it
float fGlow;              // the glow's strength around the sun, by the weather
float fLightAbsorption;   // the share of the extinction the sun's light takes inside a cloud
float fDebug;             // VolumetricCloudsDebug 1: grey by how much sun reaches each sample, white all of it;
                          // 2: the sky read behind them, so the clouds vanish where it is read right
float fFrameJitter;       // the frame's share of a step, so the march's noise changes every frame
float fWarp;              // how far, in coverage texture units, the outline wanders with height
float4 vec4Morph;         // the map's slow morph: phase, reach in texture units; the weather map's scale and its reach

// How far the scene is along the view ray at uv, the nearest of the full size pixels around it (a
// half size pixel covers four): with the farthest, the clouds behind a roof spilled onto its edge.
// The sky leaves the depth target as it was cleared, at one end or the other, and counts as far.
float RawToFar(float raw)
{
    return (raw > 0.0 && raw < 0.99999) ? raw : 1.0;
}

float SceneDistance(float2 uv, float rayScale)
{
    // Two diagonal texels of the four: the other two would take the march past its slots.
    float2 o = vec2DepthTexel * 0.5;
    float raw = min(RawToFar(tex2Dlod(DepthTex, float4(uv - o, 0, 0)).r), RawToFar(tex2Dlod(DepthTex, float4(uv + o, 0, 0)).r));
    return raw < 1.0 ? pow(fFarDivNear, raw) * fNearPlane * rayScale : 1e9;
}

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
// - The coverage map holds separate heaps spread evenly over the sky (CloudNoiseTex), equalised so
//   that cloud, where the map is above 1 - cover, takes that share of the sky.
// - The density across a heap is squared, soft at its edges, and at height h only what is above
//   0.8 h^2 of it stays: the heap narrows to a dome over a flat base. Lowering the density by a
//   quarter with height left the evenly spread heaps standing as pillars with walls. It fades out
//   over the top seventh, and at the base it is half as dense, which softens the bottom.
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

// The large weather map at p, 0 to 1: where the sky is cloudier and the heaps taller. It is the
// coverage map read 1 / vec4Morph.z times larger, from a blurred mip.
float Weather(float2 uv)
{
    return tex2Dlod(CoverageTex, float4(uv * vec4Morph.z + 0.31, 0, 6)).r;
}

// The map's slow morph: the coordinates swing by up to vec4Morph.y in waves a fifth and a quarter of
// the map across, their phase moving on with time, so the heaps change shape as they drift.
float2 Morph(float2 uv)
{
    return uv + float2(sin(uv.y * 31.4159 + vec4Morph.x), cos(uv.x * 25.1327 - vec4Morph.x * 1.2)) * vec4Morph.y;
}

// The weather (-1 to 1) and the morph's offset at p, worked out once for a sample of the march and
// taken for its steps towards the sun as well: within those few hundred metres neither changes much.
struct Place
{
    float weather;
    float2 morph;
};

Place PlaceAt(float3 p)
{
    Place place;
    float2 uv0 = p.xy * vec4Layer.z + vec4Wind.xy;
    place.weather = Weather(uv0) * 2.0 - 1.0;
    place.morph = Morph(uv0) - uv0;
    return place;
}

// The coarse search's test, cheap and on the safe side: whether there may be cloud near p, from a
// blurred mip of the map (its texels, 32 to the map, span the morph's reach) against the cover of the cloudiest
// weather.
bool MayBeCloud(float3 p)
{
    float h = (p.z - vec4Layer.x) / vec4Layer.y;
    if (h <= 0.0 || h >= 1.25)
        return false;
    float c = tex2Dlod(CoverageTex, float4(p.xy * vec4Layer.z + vec4Wind.xy, 0, 5)).r;
    float cover = saturate(max(vec4Layer.w, 0.02) * (1.0 + abs(vec4Morph.w)));
    return c > 0.9 - cover;
}

float Density(float3 p, bool detail, Place place)
{
    float2 uv0 = p.xy * vec4Layer.z + vec4Wind.xy;
    // Cloudier parts of the sky have more cover and taller heaps, by vec4Morph.w either way.
    float weather = place.weather;
    float thickness = vec4Layer.y * (1.0 + 0.25 * weather);
    float h = (p.z - vec4Layer.x) / thickness;
    if (h <= 0.0 || h >= 1.0)
        return 0.0;
    float2 uv = uv0 + place.morph;
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

    float cover = saturate(max(vec4Layer.w, 0.02) * (1.0 + vec4Morph.w * weather));
    // Overcast: the map evens out towards a sheet.
    c = lerp(c, max(c, 1.0 - cover * 0.5), fStratus);
    float threshold = 1.0 - cover;
    float d = saturate((c - threshold) / max(cover, 0.01));
    d *= d;
    float dome = h * h * 0.8 * (1.0 - fStratus);
    d = saturate((d - dome) / max(1.0 - dome, 0.05)) * smoothstep(1.0, 0.85, h);
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

// The Earth's curvature: the layer's shells are spheres about its centre, so at a horizontal distance
// r from the camera a point stands r^2 / 2R higher above them than above a flat layer, and the
// layer sinks towards the horizon. Along a ray that is a t^2 more, a = (1 - dir.z^2) / 2R.
#define EARTH_RADIUS 6371000.0

// The distances along the ray where it crosses the shell at height h: the roots of
// a t^2 + dz t + (z - h) = 0, in order; (1e9, -1e9), an empty span, when it never does.
float2 ShellCross(float a, float dz, float c)
{
    float disc = dz * dz - 4.0 * a * c;
    if (disc < 0.0)
        return float2(1e9, -1e9);
    float q = -0.5 * (dz + (dz >= 0.0 ? 1.0 : -1.0) * sqrt(disc));
    float r1 = q / a;
    float r2 = c / (abs(q) > 1e-9 ? q : 1e-9);
    return float2(min(r1, r2), max(r1, r2));
}

// The first span of the ray, from the camera on, inside the layer: below the top shell and above the
// base one. Looking down from above, the ray can leave through the base and come back up far off;
// only the first span counts.
float2 LayerSpan(float3 dir, float z, float a)
{
    float2 top = ShellCross(a, dir.z, z - (vec4Layer.x + vec4Layer.y * 1.25));
    float2 base = ShellCross(a, dir.z, z - vec4Layer.x);
    float2 first = float2(max(top.x, 0.0), min(top.y, base.x));
    float2 second = float2(max(max(top.x, base.y), 0.0), top.y);
    return first.y > first.x ? first : second;
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
    float curve = max(1.0 - dir.z * dir.z, 1e-6) / (2.0 * EARTH_RADIUS);
    float2 span = LayerSpan(dir, origin.z, curve);
    float t0 = span.x;
    float t1 = min(min(span.y, fMaxDistance), SceneDistance(uv, rayScale));
    if (t1 <= t0)
        return float4(0.0, 0.0, 0.0, 1.0);
    // Beyond sixteen thicknesses into the layer the haze hides the cloud anyway.
    t1 = min(t1, t0 + vec4Layer.y * 16.0);

    float cosTheta = dot(dir, vec3SunDir);
    // The forward lobe: cloud around the sun in the sky glows where it is thin enough for its light
    // to come through, the bright rims of clouds against the sun. A core some 30 degrees wide and a
    // faint skirt beyond; a Henyey-Greenstein lobe of g 0.85 lit only the cloud within a few degrees
    // of the sun and left the rims a little way off it dark.
    float forward = (exp(8.0 * (cosTheta - 1.0)) + 0.3 * exp(2.0 * (cosTheta - 1.0))) * fSilver * fGlow;
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
        p.z += curve * t * t;

        [branch]
        if (fineLeft <= 0.0)
        {
            if (MayBeCloud(p))
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

        Place place = PlaceAt(p);
        float d = Density(p, true, place);
        [branch]
        if (d > 0.01)
        {
            fineLeft = FINE_MISSES;
            if (firstHit < 0.0)
                firstHit = t;

            // The cloud between the sample and the sun, without the detail: from a twentieth of the
            // layer, each step twice the last, out past its whole thickness so the bases darken.
            float lightDepth = 0.0;
            float stepLength = vec4Layer.y * 0.05;
            float3 q = p;
            [loop] [fastopt]
            for (int j = 0; j < LIGHT_STEPS; ++j)
            {
                q += vec3SunDir * stepLength;
                lightDepth += Density(q, false, place) * stepLength;
                stepLength *= 2.0;
            }
            // Light scattered many times inside a cloud gets far deeper than the sun's direct beam,
            // which is what keeps real clouds bright: three octaves, each with half the extinction
            // and half the weight of the one before. With the direct beam alone the sun barely
            // reached the faces seen from below and the clouds came out dark.
            // A share of the extinction (VolumetricCloudsAbsorption): past the sun's direct beam the
            // light inside a cloud is scattered forwards mostly and gets through more cloud than the
            // eye's view does. At 0.6 the sun was spent within some 50 metres, about as deep as the
            // march's first sample of a cloud lies, so even the sides facing the sun came out in
            // shade and the clouds an even grey; at 0.2 they are lit, and the bases still darken
            // under the whole thickness the steps now reach.
            float tau = lightDepth * sigma * fLightAbsorption;
            float sun = (exp(-tau) + 0.5 * exp(-0.5 * tau) + 0.25 * exp(-0.25 * tau)) * (fLightStrength / 1.75);
            // Darker towards the base, where the sky above is hidden by the cloud itself. By height
            // alone: a sample of the cloud straight up cost a whole density lookup, the slots the
            // Earth's curvature needed.
            float h = saturate((p.z - base) / vec4Layer.y);
            float3 shade = vec3ShadeColour * lerp(0.55, 1.0, sqrt(h));
            // The parts the sun reaches take its hue: warm in the evening, orange at sunset.
            float3 sunLit = vec3LitColour * vec3SunTint;
            float3 lit = lerp(shade, sunLit, sun);
            // The game's silver lining: up to CloudInscatteringRange brighter along the sun's axis,
            // in the thin cloud the sun reaches. A constant 1.7 times the cloud colour in its place
            // left the clouds at that peak from every side, with nothing for the rim to rise above.
            float thin = saturate(1.0 - d);
            lit += sunLit * fSilver * cosTheta * cosTheta * lerp(0.35, 1.0, thin) * sun;
            // The sunset colour on the side towards the sun, as the game adds it.
            lit += vec3SunsetColour * sun * (0.35 + 0.25 * cosTheta);
            // The glow of thin cloud next to the sun, its light coming through.
            // By the sun's light left after the cloud towards it, so the edges glow and the middle,
            // with the whole cloud between it and the sun, stays dark: weighted by the light
            // scattered many times instead, the whole cloud around the sun brightened evenly.
            lit += sunLit * forward * thin * exp(-tau);
            // A soft knee from three quarters of the ceiling up: the glow rises towards it instead of
            // clipping to white.
            float peak = max(max(lit.r, lit.g), lit.b);
            float knee = fCeiling * 0.75;
            if (peak > knee)
                lit *= (knee + fCeiling * 0.25 * (1.0 - exp((knee - peak) / (fCeiling * 0.25)))) / peak;
            lit = fDebug == 1.0 ? sun * vec3LitColour.yyy : lit;

            // Thin cloud lets more of what is behind it through, the wisps at the edges most.
            float stepTransmittance = exp(-d * lerp(1.0 - fTranslucency, 1.0, d) * sigma * fine);
            colour += transmittance * (1.0 - stepTransmittance) * lit;
            transmittance *= stepTransmittance;
        }
        else
            fineLeft -= 1.0;
        t += fine;
    }

    // The clouds against the sky behind them. The game's CloudColor at its HDR exposure came out
    // several times brighter than the sky it draws, past the tone mapping's white point: the whole
    // cloud, lit side, bases and rims, turned one flat white. Scaled as a whole, so its shading
    // stays, until its sunlit side is fSkyMatch times the sky's luma here; the sky around the sun is
    // brighter, and so are the clouds before it.
    [branch]
    if (fSkyMatch > 0.0)
    {
        float3 sky = tex2Dlod(SceneTex, float4(uv, 0, 0)).rgb;
        float skyLuma = dot(sky, float3(0.2126, 0.7152, 0.0722));
        if (skyLuma > 1e-4)
            colour *= clamp(fSkyMatch * skyLuma / fLitLuma, 0.1, 2.0);
        if (fDebug == 2.0)
            colour = sky * (1.0 - transmittance);
    }

    // Haze: distant cloud fades into what is behind it.
    float haze = firstHit >= 0.0 ? exp(-firstHit / vec4Shape.w) : 0.0;
    float cover = (1.0 - transmittance) * haze;
    return float4(colour * haze, 1.0 - cover);
}

// The world direction of the view ray through the half or full size pixel vpos.
float3 WorldRay(float2 vpos)
{
    float3 v = ViewRay(vpos);
    return normalize(float3(dot(v, vec4WorldX.xyz), dot(v, vec4WorldY.xyz), dot(v, vec4WorldZ.xyz)));
}

// Accumulates the half size clouds over frames. The clouds are kilometres away, so last frame's
// pixel is found by the camera's turn alone, as a direction; its colour is held within the range
// of this frame's 3x3 neighbourhood, so drifting clouds and a moving camera leave no trails.
float4 CloudsResolve_PS(float2 uv : TEXCOORD0, float2 vpos : VPOS) : COLOR0
{
    float4 current = tex2Dlod(CurrentTex, float4(uv, 0, 0));
    [branch]
    if (vec4History.x <= 0.0)
        return current;

    float3 dir = WorldRay(vpos);
    float3 clip = float3(dot(dir, vec4PrevX.xyz), dot(dir, vec4PrevY.xyz), dot(dir, vec4PrevW.xyz));
    if (clip.z <= 0.0)
        return current;
    float2 prev = clip.xy / clip.z * float2(0.5, -0.5) + 0.5;
    if (any(prev < 0.0) || any(prev > 1.0))
        return current;

    float4 lo = current, hi = current;
    [unroll]
    for (int y = -1; y <= 1; ++y)
    {
        [unroll]
        for (int x = -1; x <= 1; ++x)
        {
            float4 s = tex2Dlod(CurrentTex, float4(uv + float2(x, y) * vec4History.zw, 0, 0));
            lo = min(lo, s);
            hi = max(hi, s);
        }
    }
    float4 history = clamp(tex2Dlod(HistoryTex, float4(prev, 0, 0)), lo, hi);
    return lerp(history, current, vec4History.y);
}

// Lays the accumulated clouds over the scene at full size. Where the pixel is something nearer than
// the cloud layer, the upscale must not spill cloud onto it from a sky pixel beside it.
float4 CloudsComposite_PS(float2 uv : TEXCOORD0, float2 vpos : VPOS) : COLOR0
{
    float raw = tex2Dlod(DepthTex, float4(uv, 0, 0)).r;
    [branch]
    if (raw > 0.0 && raw < 0.99999)
    {
        float3 v = ViewRay(vpos);
        float3 dir = float3(dot(v, vec4WorldX.xyz), dot(v, vec4WorldY.xyz), dot(v, vec4WorldZ.xyz));
        float rayScale = length(dir);
        dir /= rayScale;
        float tEnter = LayerSpan(dir, vec4WorldZ.w, max(1.0 - dir.z * dir.z, 1e-6) / (2.0 * EARTH_RADIUS)).x;
        if (pow(fFarDivNear, raw) * fNearPlane * rayScale < tEnter)
            return float4(0.0, 0.0, 0.0, 1.0);
    }
    return tex2Dlod(CloudTex, float4(uv, 0, 0));
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

technique CloudsResolve
{
    pass Resolve
    {
        VertexShader = compile vs_3_0 FullscreenQuadVS();
        PixelShader = compile ps_3_0 CloudsResolve_PS();
    }
}

technique CloudsComposite
{
    pass Composite
    {
        VertexShader = compile vs_3_0 FullscreenQuadVS();
        PixelShader = compile ps_3_0 CloudsComposite_PS();
    }
}
