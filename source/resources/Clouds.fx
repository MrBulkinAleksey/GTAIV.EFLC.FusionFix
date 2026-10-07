// Volumetric clouds (RenderVolumetricClouds in postfx.ixx): one layer of cloud marched from the camera at
// half size (CloudsMarch, which keeps sums of what the light needs), lit from those sums
// (CloudsLight, matched to the sky's brightness from CloudsSkyRef), accumulated over frames
// (CloudsResolve) and blended into the lit scene at full size right before the fog pass
// (CloudsComposite), so the fog, SSR's history and everything after see them. The march and the
// light are apart so each has ps_3_0's 512 slots to itself; the reflections take both in one pass
// (Clouds). Wherever the scene is nearer than the cloud the ray stops at it, so clouds show over
// the sky and over anything far enough behind them alike.
//
// The layer's coverage is CoverageTex, the same tiling noise, scale and wind as the cloud shadows on
// the ground, so each shadow lies under its cloud. DetailTex, a tiling Worley volume in two channels,
// erodes the edges.
// Light: the sun reaching each sample through the cloud above it (Beer's law with softer lobes for the
// light scattered more than once), lit the way gta_atmoscatt_clouds lights its own clouds:
// CloudColor, brightened by up to CloudInscatteringRange towards the sun (cos^2, that side only) where the cloud
// is thin, darkened where the sun does not reach, plus SunsetColor on the side towards the sun. A
// forward lobe some 30 degrees wide adds the glow of thin cloud around the sun, and a soft knee rolls
// the brightest of it off below a ceiling, so it neither clips to white nor flattens the shading.
// What the sun lights also takes the hue of the game's sun colour.

// Steps a ray may take in all, coarse and fine.
#ifndef CLOUD_STEPS
#define CLOUD_STEPS 192
#endif
// A coarse step in fine steps, and the fine steps in a row that must find no cloud to go coarse again.
#define COARSE_STEP 4.0
// A fine step's share of the distance further out. At a hundredth a cloud 15 km off was crossed in
// 150 m steps: its light changed sharply from one to the next, the rows of pixels took their steps
// at the same heights, and thin cloud came out in horizontal stripes. Half that needs CLOUD_STEPS 192
// for the far clouds to be crossed at all.
#define FAR_STEP 0.005
#define FINE_MISSES 4.0
#ifndef LIGHT_STEPS
#define LIGHT_STEPS 5
#endif
// CloudNoiseTex's size.
#define COVERAGE_SIZE 1024.0
// The dome and the rounded base take part of every heap's footprint, the more the smaller the heaps:
// on the 6 x 6 map a cover of 0.4 left 31% of the sky under cloud. Measured over the map, 1.75 times
// the cover brought the share back to the cover from 0.25 to 0.45; at 0.7 it stopped at 0.57, the
// rest left to the overcast sheet. Since the billows eat through the whole cloud, 2.75 keeps the
// fair weathers' share as it was (the cloudy and wet ones, held at the top of the range, take less
// detail instead). The shadows, on a flat deck, take the cover as it is.
#define COVER_GAIN 2.75

sampler2D DepthTex : register(s0);
sampler2D CoverageTex : register(s1);
sampler3D DetailTex : register(s2);
sampler2D CurrentTex : register(s3);   // this frame's clouds at half size (CloudsResolve)
sampler2D HistoryTex : register(s4);   // the clouds accumulated up to last frame (CloudsResolve)
sampler2D CloudTex : register(s5);     // the accumulated clouds (CloudsComposite)
sampler2D SceneTex : register(s6);     // the lit scene, the sky in it (CloudsSkyRef, and the halo behind the clouds in CloudsLight)
sampler2D SkyRefTex : register(s7);    // the sky's mean colour this frame (CloudsSkyRef's target)
sampler2D MarchTex0 : register(s8);    // the march's sums: transmittance, sun, shade, silver (CloudsLight)
sampler2D MarchTex1 : register(s9);    // and glow, first hit

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
float fMinLight;          // VolumetricCloudsMinLight: the least of the sun's light any part of a cloud keeps
float fMottle;            // VolumetricCloudsMottle: how much the small billows at the surface fleck the light
float fBacklight;         // with the sun low, the share of their light the clouds straight towards it keep
float fGlowBoost;         // and how much stronger the glow of their thin edges is
float2 vec2Compress;      // the density's compressor at the layer's base and top
float fLightStrength;     // the sun's light, fading out below the horizon, or the moon's once it has handed over
float fCeiling;
// VolumetricCloudsSkyMatch, how many times brighter than the sky behind them the clouds' sunlit side
// is, over that sunlit side's luma; 0 leaves them at the game's cloud colour.
float fSkyMatch;           // the brightest channel the cloud rolls off towards (at most the sky's clamp without HDR)
// How much the sky's own hue on screen (CloudsSkyRef) tints the shaded side (x) and the sunlit side
// (y), and VolumetricCloudsSaturation (z); 0 while the march cannot read the scene.
float3 vec3SkyHue;
float3 vec3SunTint;       // the hue of the game's SunColor at its brightness 1, mixed towards white by VolumetricCloudsSunTint
float3 vec3GlowColour;    // the game's cloud colour, exposed, in the sun's own hue: the sunlight straight through
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
float fGlow;
float fSunPower;
float fBaseRound;         // how far the base's edges curl in, by the weather          // how much brighter the cloud near the sun in the sky is, all of it, by the weather
float2 vec2Shear;
float fCurl;              // how far the billows are swept along the coarse noise at the tops, in their own size         // the coverage's offset at the layer's top: the tops lean downwind, drawn out by the wind              // the glow's strength around the sun, by the weather
float fLightAbsorption;   // the share of the extinction the sun's light takes inside a cloud
float fDebug;             // VolumetricCloudsDebug: 2 the sky brightness matched to, 3 to 11 one term of the light alone (Light)
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
//   1.15 h^1.5 of it stays: the heap narrows from its flat base up to a dome, and a weaker heap stops
//   lower, so heaps differ in height as they do in size. At 0.9 h a heap's dense core still stood
//   at the layer's top and was cut flat there: seen from the side, a slab. Lowering the density by a quarter with
//   height left the evenly spread heaps standing as pillars with walls, and 0.8 h^2 still ran each
//   one up the layer's whole thickness with flat sides. An overcast sheet, (1 - stratus)^2, keeps
//   most of its depth. It fades out over the top seventh, and at the base it is half as dense,
//   which softens the bottom.
// - The detail eats through the whole cloud: round Worley billows of about a sixteenth to a quarter
//   of DetailScale break it into ragged pieces with gaps, and a finer octave at about a fifth of that
//   frays the edges, more where the density is low. Eroded near the edges alone, the clouds had soft
//   marshmallow outlines and no holes.
// - Last a soft compressor, d (1 + k) / (1 + k d) with k from 3 at the base to 12 at the top (1 to 3
//   in cloudy and windy weather, whose edges thin out into smoke), makes the inside dense quickly
//   while the edges stay soft; the linear ramp it replaces cut the clouds'
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
    float base;       // the base's height there
    float thickness;  // and the layer's thickness
};

Place PlaceAt(float3 p)
{
    Place place;
    float2 uv0 = p.xy * vec4Layer.z + vec4Wind.xy;
    place.weather = Weather(uv0) * 2.0 - 1.0;
    place.morph = Morph(uv0) - uv0;
    // Cloudier parts of the sky have taller heaps, by a quarter either way. Real cumulus share a
    // base, where the rising air cools to its dew point, but it is not drawn with a ruler: up to a
    // tenth of the layer higher where the weather map is weaker, and level under an overcast sheet,
    // whose deck would open a strip of sky at the horizon.
    place.thickness = vec4Layer.y * (1.0 + 0.25 * place.weather);
    place.base = vec4Layer.x + vec4Layer.y * 0.1 * (0.5 - 0.5 * place.weather) * (1.0 - fStratus);
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
    float cover = saturate(max(vec4Layer.w, 0.02) * COVER_GAIN * (1.0 + abs(vec4Morph.w)));
    return c > 0.9 - cover;
}

// crease: the coarse billows' noise at p, high in the folds between them (0 without the detail).
// soft: the density before the compressor below, which rises gently from the edge inwards.
float Density(float3 p, bool detail, Place place, bool full, out float crease, out float soft)
{
    crease = 0.0;
    soft = 0.0;
    float2 uv0 = p.xy * vec4Layer.z + vec4Wind.xy;
    // Cloudier parts of the sky have more cover and taller heaps, by vec4Morph.w either way.
    float weather = place.weather;
    float h = (p.z - place.base) / place.thickness;
    if (h <= 0.0 || h >= 1.0)
        return 0.0;
    // The wind's shear leans the heaps' tops downwind and draws them out.
    float2 uv = uv0 + place.morph + vec2Shear * h;
    float2 wander = 0.0;
    // The outline wanders with height: the coverage is read a little off, by a coarse octave of the
    // detail that changes up through the layer, so the heaps do not stand as walls drawn up from a
    // map. The steps towards the sun and the coarse search leave it out.
    [branch]
    if (detail)
    {
        float3 w = p * (vec4Shape.y * 0.35) + float3(vec4Wind.zw, fEvolution);
        wander = tex3Dlod(DetailTex, float4(w, 0)).rg - 0.5;
        uv += wander * fWarp;
    }
    // Filtered with a quintic curve between texels: linear filtering's kinks at the texel edges stood
    // out as creases down the clouds' sides.
    float2 texel = uv * COVERAGE_SIZE - 0.5;
    float2 cell = floor(texel);
    float2 f = texel - cell;
    f = f * f * f * (f * (f * 6.0 - 15.0) + 10.0);
    float c = tex2Dlod(CoverageTex, float4((cell + f + 0.5) / COVERAGE_SIZE, 0, 0)).r;

    float cover = saturate(max(vec4Layer.w, 0.02) * COVER_GAIN * (1.0 + vec4Morph.w * weather));
    // Overcast: the map evens out towards a sheet.
    c = lerp(c, max(c, 1.0 - cover * 0.5), fStratus);
    float threshold = 1.0 - cover;
    float d = saturate((c - threshold) / max(cover, 0.01));
    d *= d;
    // The base's edges curl in by fBaseRound over its bottom quarter, so a heap sits on a rounded
    // base rather than a sheared off one.
    // The one pass variant for the reflections (full false) leaves it out, and keeps a linear dome,
    // for the slots.
    float curl = full ? max(1.0 - 4.0 * h, 0.0) : 0.0;
    float dome = ((full ? h * sqrt(h) : h) * 1.15 + fBaseRound * curl * curl) * (1.0 - fStratus) * (1.0 - fStratus);
    d = saturate((d - dome) / max(1.0 - dome, 0.05)) * smoothstep(1.0, 0.85, h);
    if (d <= 0.0)
        return 0.0;

    [branch]
    if (detail)
    {
        // The billows are swept along the coarse noise, more towards the tops, so they trail into
        // wisps there instead of sitting as round bubbles.
        float3 q = p * vec4Shape.y + float3(vec4Wind.zw, fEvolution) + wander.xyx * (fCurl * h);
        // Our Worley volume is 1 at the cells' middles: 1 - n is high between the billows.
        float2 billow = tex3Dlod(DetailTex, float4(q, 0)).rg;
        float n = 1.0 - billow.r;
        crease = n;
        d -= vec4Shape.z * 1.5 * n * n;
        // The fine billows are read through the coarse ones' two channels, so they swirl around
        // them instead of sitting on an even grid: about their own size either way.
        float m = 1.0 - tex3Dlod(DetailTex, float4(q * 5.5 + 0.37 + (billow.rgr - 0.5), 0)).r;
        d -= vec4Shape.z * 0.8 * m * (0.6 * m + 0.4) * sqrt(EdgeWeight(d));
        // A ragged fringe under the base: the coarse billows eat the bottom eighth harder.
        d -= vec4Shape.z * 0.7 * n * saturate(1.0 - h * 8.0);
        if (d <= 0.0)
            return 0.0;
    }

    soft = d;
    float k = lerp(vec2Compress.x, vec2Compress.y, h);
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

// The world direction of the view ray through the half or full size pixel vpos.
float3 WorldRay(float2 vpos)
{
    float3 v = ViewRay(vpos);
    return normalize(float3(dot(v, vec4WorldX.xyz), dot(v, vec4WorldY.xyz), dot(v, vec4WorldZ.xyz)));
}

// What the march gathers along a ray for the light to be worked out from: the light is linear in
// these, so lighting the sums once gives what lighting each sample did. Each is a sum over the
// samples weighted by how much of the ray's view each one takes, T (1 - step transmittance).
struct CloudSums
{
    float transmittance;  // what is left of the view behind the cloud
    float sun;            // the sun's light reaching the samples
    float shade;          // the shaded colour's share: (1 - sun), darker towards the base
    float silver;         // the silver lining's: sun, more where the cloud is thin
    float glow;           // the glow's: thin cloud with little of it towards the sun
    float firstHit;       // how far the ray met its first cloud, or -1
    float top;            // the sun reaching the upper half of the cloud
    float height;         // the samples' height in the layer, 0 at the base
};

// The world direction of the view ray through the half or full size pixel vpos, and its length in
// view space units of depth.
float3 RayDirection(float2 vpos, out float rayScale)
{
    float3 v = ViewRay(vpos);
    float3 dir = float3(dot(v, vec4WorldX.xyz), dot(v, vec4WorldY.xyz), dot(v, vec4WorldZ.xyz));
    rayScale = length(dir);
    return dir / rayScale;
}

// steps, stepScale, lightSteps and lightStart: the march's budget. The scene's clouds take CLOUD_STEPS,
// 1, LIGHT_STEPS and 0.05; the reflections (Clouds_PS) a quarter of the steps, each four times as long,
// and two steps towards the sun from a quarter of the layer: drawn at the full size of a 1024 x 1024
// map every frame with the scene's budget, they cost five times the scene's clouds.
CloudSums March(float2 uv, float2 vpos, float3 dir, float rayScale, bool full, int steps, float stepScale, float lightSteps, float lightStart)
{
    CloudSums sums;
    sums.transmittance = 1.0;
    sums.sun = 0.0;
    sums.shade = 0.0;
    sums.silver = 0.0;
    sums.glow = 0.0;
    sums.firstHit = -1.0;
    sums.top = 0.0;
    sums.height = 0.0;
    float3 origin = float3(vec4WorldX.w, vec4WorldY.w, vec4WorldZ.w);

    // Where the ray is inside the layer, cut short by the scene in front.
    float base = vec4Layer.x;
    float curve = max(1.0 - dir.z * dir.z, 1e-6) / (2.0 * EARTH_RADIUS);
    float2 span = LayerSpan(dir, origin.z, curve);
    float t0 = span.x;
    float t1 = min(min(span.y, fMaxDistance), SceneDistance(uv, rayScale));
    [branch]
    if (t1 <= t0)
        return sums;
    // Beyond sixteen thicknesses into the layer the haze hides the cloud anyway.
    t1 = min(t1, t0 + vec4Layer.y * 16.0);
    float sigma = vec4Shape.x;

    // Empty sky is crossed in coarse steps that test the coverage alone; on finding cloud the ray
    // steps back and marches it in fine steps, a 24th of the layer's thickness near the camera and
    // a two hundredth of the distance further out, until FINE_MISSES fine steps in a row find none.
    // With even steps over the whole crossing a step near the horizon was 150 m long, and the
    // clouds came out smeared down the screen.
    // Each pixel starts up to a coarse step later: with the same coarse steps for every pixel,
    // thin cloud between two of them went missing in whole bands across the screen.
    float jitter = frac(PixelJitter(vpos) + fFrameJitter);
    float t = t0 + max(vec4Layer.y / 24.0, t0 * FAR_STEP) * stepScale * COARSE_STEP * jitter;
    float fineLeft = 0.0;
    // Looking away from the sun, how much the powder effect darkens the sun's light (below).
    float powderView = full ? 0.35 - 0.35 * dot(dir, vec3SunDir) : 0.0;

    // [fastopt]: without it D3DX spent close to a minute on this loop while the game loaded.
    [loop] [fastopt]
    for (int i = 0; i < steps; ++i)
    {
        if (t >= t1 || sums.transmittance < 0.01)
            break;
        float fine = max(vec4Layer.y / 24.0, t * FAR_STEP) * stepScale;
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
        float crease, soft;
        float d = Density(p, true, place, full, crease, soft);
        [branch]
        if (d > 0.01)
        {
            fineLeft = FINE_MISSES;
            if (sums.firstHit < 0.0)
                sums.firstHit = t;

            // The cloud between the sample and the sun, without the detail: from a twentieth of the
            // layer, each step twice the last, out past its whole thickness so the bases darken.
            float lightDepth = 0.0;
            float stepLength = vec4Layer.y * lightStart;
            float3 q = p;
            // A float counter, and no [fastopt]: with an int counter under [fastopt] the compiler
            // negated the loop's bound, and the loop broke before its first step. The light's march
            // found no cloud at all, every cloud took the sun whole, and the absorption did nothing.
            [loop]
            for (float j = 0.0; j < lightSteps - 0.5; j += 1.0)
            {
                q += vec3SunDir * stepLength;
                float unused, unusedSoft;
                lightDepth += Density(q, false, place, full, unused, unusedSoft) * stepLength;
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
            // shade and the clouds an even grey; at 0.2 the thin fair weather clouds, a few hundred
            // metres thick, let the sun through nearly whole and came out flat; 0.4 keeps both.
            float tau = lightDepth * sigma * fLightAbsorption;
            // Never below VolumetricCloudsMinLight of the sun, however deep in the cloud: for looks, the
            // bases and the shaded sides stay light and airy rather than heavy.
            float sun = lerp(fMinLight, 1.0, (exp(-tau) + 0.5 * exp(-0.5 * tau) + 0.25 * exp(-0.25 * tau)) / 1.75) * fLightStrength;
            // The billows shade each other: where a coarse billow stands a billow's size, 120 m,
            // towards the sun, the sample takes less of it, by its own density. The march above
            // leaves the detail out, and its first steps are too short for a billow to shade the
            // next: the clouds' faces showed the large shadows only, smooth over their billows. A
            // whole density sample there cost the march past its slots, for much the same look.
            [branch]
            if (full)
            {
                float3 towards = (p + vec3SunDir * 120.0) * vec4Shape.y + float3(vec4Wind.zw, fEvolution);
                // About the noise's middle, 0.5: by the noise itself every cloud took a third less sun
                // on average, and the sides facing away from the sun came out grey. Never brighter
                // than without it.
                sun *= min(1.0, exp(-3.0 * d * (tex3Dlod(DetailTex, float4(towards, 0)).r - 0.5)));
            }
            // The powder effect: light scattered many times builds up inside a cloud, so its thin
            // edges, seen from the sun's side, are darker than its depth, and the folds between its
            // billows read. Without it the clouds facing away from the sun came out as flat white.
            sun *= 1.0 - powderView * exp(-120.0 * d * sigma);
            // The folds between the billows take less light, as the billows round them shade them:
            // the light's march leaves the detail out, and without this the clouds' faces came out
            // as smooth gradients whatever their outline did.
            sun *= full ? 1.0 - 0.9 * crease * crease : 1.0;
            // Darker towards the base, where the sky above is hidden by the cloud itself, by height.
            float h = saturate((p.z - base) / vec4Layer.y);
            // Thin by the density before the compressor: past it the density is near 1 a few metres
            // in, so the silver lining and the glow lit a band a pixel or two wide at the very edge,
            // and every cloud came out with a white outline drawn round it.
            float thin = saturate(1.0 - soft);

            // Thin cloud lets more of what is behind it through, the wisps at the edges most.
            float stepTransmittance = exp(-d * lerp(1.0 - fTranslucency, 1.0, d) * sigma * fine);
            float weight = sums.transmittance * (1.0 - stepTransmittance);
            sums.sun += weight * sun;
            // The sky's light, which the shaded colour stands for, has its own shape: more of it at
            // the tops than at the base, less in the folds between the billows; about 1 on average.
            // With only a fifth darker at the base, a cloud the sun did not reach, its shaded side or
            // any cloud at dusk, came out one flat grey. The reflections leave the folds out, for the slots.
            sums.shade += weight * (1.0 - sun) * (0.65 + 0.65 * sqrt(h)) * (full ? 1.0 - 0.6 * crease * crease : 1.0);
            // The silver lining is the thin edges' alone: with a third of it on the thick body, a cloud
            // before the sun in game came out half again as bright as its own light.
            sums.silver += weight * sun * thin * thin;
            // The glow by the sun's light left after the cloud towards it, so the edges glow and the
            // middle, with the whole cloud between it and the sun, stays dark: weighted by the light
            // scattered many times instead, the whole cloud around the sun brightened evenly.
            // At the light's strength: past dusk, before the moon takes over, the edges glowed as if
            // the sun were still up; by night the glow follows the moon. However dense: weighted by
            // how thin the cloud was too, it lit only a pixel or two at the very edge, and none at all
            // once the billows ate through the clouds and left them dense right behind their outline;
            // the sun's light left is what makes the band, wide where the sun gets in.
            sums.glow += weight * exp(-tau) * fLightStrength;
            // VolumetricCloudsDebug 12 takes the tops' sum for the cloud the light's march found towards
            // the sun, in thicknesses of the layer at full density.
            sums.top += weight * (full && fDebug == 12.0 ? saturate(lightDepth / vec4Layer.y) : sun * smoothstep(0.5, 1.0, h));
            sums.height += weight * h;
            sums.transmittance *= stepTransmittance;
        }
        else
            fineLeft -= 1.0;
        t += fine;
    }
    return sums;
}

// VolumetricCloudsDebug 11: a share 0 to 1 as a colour the eye's adaptation does not change: blue
// under 0.15, green to 0.3, yellow to 0.5, orange to 0.7, red above.
float3 SunScale(float share)
{
    return share < 0.15 ? float3(0.1, 0.2, 1.0)
         : share < 0.3 ? float3(0.1, 1.0, 0.1)
         : share < 0.5 ? float3(1.0, 1.0, 0.1)
         : share < 0.7 ? float3(1.0, 0.5, 0.0)
         : float3(1.0, 0.05, 0.05);
}

// The clouds' colour from what the march gathered, premultiplied, with the share of the scene
// behind that shows through in alpha. full is a constant: the one pass variant leaves the matching
// to the sky (and its debug view), which the reflections do not use, the sun power, the tops'
// light and the undersides' darkening out, for the slots.
float4 Light(CloudSums sums, float3 dir, bool full, float2 uv)
{
    float cover = 1.0 - sums.transmittance;
    [branch]
    if (cover < 1e-3)
        return float4(0.0, 0.0, 0.0, 1.0);

    float cosTheta = dot(dir, vec3SunDir);
    // The sunset colour where the sun reaches, as the game adds it, and more towards the sun in a
    // lobe some 30 degrees wide: the clouds before a low sun take its glow.
    // Both lobes are powers of one exponential: exp(4x) is exp(2x) squared, exp(8x) that squared.
    float lobe2 = exp(2.0 * (cosTheta - 1.0));
    float lobe4 = lobe2 * lobe2;
    float sunsetLobe = 0.35 + 0.25 * cosTheta + 0.65 * lobe4;
    // The forward lobe: cloud around the sun in the sky glows where it is thin enough for its light
    // to come through, the bright rims of clouds against the sun. A core some 30 degrees wide and a
    // faint skirt beyond; a Henyey-Greenstein lobe of g 0.85 lit only the cloud within a few degrees
    // of the sun and left the rims a little way off it dark.
    float forward = (lobe4 * lobe4 + 0.3 * lobe2) * fSilver * fGlow;

    // The parts the sun reaches take its hue: warm in the evening, orange at sunset. The shaded
    // ones the cloud colour darkened and the sky's hue. The game's silver lining: up to
    // CloudInscatteringRange brighter along the sun's axis, in the thin cloud the sun reaches; a
    // constant 1.7 times the cloud colour in its place left the clouds at that peak from every side,
    // with nothing for the rim to rise above.
    float3 sunLit = vec3LitColour * vec3SunTint;
    float3 shadeColour = vec3ShadeColour;
    // The sky lights the clouds too, in the colour it has on screen: pink at dusk, grey in rain. The
    // game's SkyColor is not that colour, and the clouds took a bluish white under a pink sky.
    float3 skyRef = 0.0;
    float skyLuma = 0.0;
    [branch]
    if (full && fSkyMatch > 0.0)
    {
        skyRef = tex2Dlod(SkyRefTex, float4(0.5, 0.5, 0, 0)).rgb;
        skyLuma = dot(skyRef, float3(0.2126, 0.7152, 0.0722));
        float3 hue = skyLuma > 1e-4 ? lerp(1.0, clamp(skyRef / skyLuma, 0.0, 3.0), vec3SkyHue.z) : 1.0;
        shadeColour = lerp(shadeColour, dot(shadeColour, float3(0.2126, 0.7152, 0.0722)) * hue, vec3SkyHue.x);
        sunLit = lerp(sunLit, dot(sunLit, float3(0.2126, 0.7152, 0.0722)) * hue, vec3SkyHue.y);
    }
    // Each term apart, so VolumetricCloudsDebug 3 to 11 can show it alone.
    float3 termBase = shadeColour * sums.shade + sunLit * sums.sun;
    // Towards the sun only: the game's cos^2 lit the clouds' edges as brightly with the sun behind the
    // eye, and every cloud across the sky had a white outline.
    float towardsSun = max(cosTheta, 0.0);
    float3 termSilver = sunLit * (fSilver * towardsSun * towardsSun * sums.silver);
    float3 termSunset = vec3SunsetColour * (sunsetLobe * sums.sun);
    float3 termGlow = vec3GlowColour * (forward * sums.glow * (full ? fGlowBoost : 1.0));
    float3 termSunPower = 0.0;
    float3 termTop = 0.0;
    float shadeMul = 1.0;
    [branch]
    if (full)
    {
        // The cloud near the sun in the sky catches more of its light, all of it, the thick middle
        // too, in a softer lobe than the glow's, by the weather.
        // By the sun that reaches the cloud alone: the square root of it, which lifted the dark
        // bodies of clouds before the sun as much as their lit sides, washed them out in game.
        termSunPower = sunLit * (fSunPower * (0.45 * lobe4 * lobe4 + 0.2 * lobe2) * sums.sun);
        // The sunlit tops brighter still, at four tenths more.
        termTop = sunLit * (0.4 * sums.top);
        // The undersides darkened by the sky the cloud above them hides, by the cloud's height in the
        // layer alone, whatever the sun does: up to 15% at the base, none from two thirds up.
        shadeMul = 1.0 - 0.15 * (1.0 - smoothstep(0.0, 0.65, sums.height / cover));
        // Flecks: the small billows where the ray met the cloud catch a little more or less light,
        // so a grey body is not one even grey but mottled with lighter specks. Two octaves of the
        // detail, of some 60 to 250 metres and a third of that, about their middle, so the cloud
        // keeps its brightness on average. Faded out with distance, where they would only shimmer.
        // The march's light leaves them out: its sums are the cloud's whole depth, and the billows
        // it reads change the outline, not the light on the surface.
        float3 hit = float3(vec4WorldX.w, vec4WorldY.w, vec4WorldZ.w) + dir * sums.firstHit;
        hit.z += max(1.0 - dir.z * dir.z, 1e-6) / (2.0 * EARTH_RADIUS) * sums.firstHit * sums.firstHit;
        float3 q = hit * (vec4Shape.y * 1.5) + float3(vec4Wind.zw, fEvolution) + 0.53;
        float fleck = 0.65 * tex3Dlod(DetailTex, float4(q, 0)).r + 0.35 * tex3Dlod(DetailTex, float4(q * 2.7 + 0.21, 0)).g - 0.5;
        shadeMul *= 1.0 + 2.0 * fMottle * fleck * exp(-sums.firstHit / 12000.0);
    }
    float3 body = (termBase + termSilver + termSunset + termSunPower + termTop) * shadeMul;
    float3 colour = body + termGlow;

    // A soft knee from three quarters of the ceiling up, on the cloud's mean colour: the glow rises
    // towards it instead of clipping to white.
    float3 mean = colour / cover;
    float peak = max(max(mean.r, mean.g), mean.b);
    float knee = fCeiling * 0.75;
    float kneeMul = peak > knee ? (knee + fCeiling * 0.25 * (1.0 - exp((knee - peak) / (fCeiling * 0.25)))) / peak : 1.0;
    // With the sun low, the clouds towards it are lit from behind: their bodies darker than the sky
    // behind them, the glow of their thin edges kept (fBacklight, fGlowBoost); across the sky from it
    // they face its light and keep theirs.
    float backlit = full ? lerp(1.0, fBacklight, smoothstep(-0.2, 0.8, cosTheta)) : 1.0;
    colour = (body * backlit + termGlow) * kneeMul;

    // The clouds against the sky. The game's CloudColor at its HDR exposure came out several times
    // brighter than the sky it draws, past the tone mapping's white point: the whole cloud, lit
    // side, bases and rims, turned one flat white. Scaled as a whole, so its shading stays, until
    // its sunlit side is fSkyMatch times the sky's brightness this frame (CloudsSkyRef), the same for
    // every cloud. Matched to the sky right behind each one instead, the clouds round the sun took
    // its halo and shone cream while those across the sky, whose sunlit sides face the eye and
    // should be the brightest, sank to the grey of the sky there.
    // Haze: distant cloud fades into the sky.
    float haze = sums.firstHit >= 0.0 ? exp(-sums.firstHit / vec4Shape.w) : 0.0;
    [branch]
    if (full && fSkyMatch > 0.0)
    {
        float3 sky = tex2Dlod(SceneTex, float4(uv, 0, 0)).rgb;
        float behind = dot(sky, float3(0.2126, 0.7152, 0.0722));
        float skyMul = skyLuma > 1e-4 ? clamp(fSkyMatch * skyLuma, 0.1, 2.0) : 1.0;
        colour *= skyMul;
        // The silver lining against the sun: the half lit band at a cloud's edge, where it is there
        // but still lets the sky through, takes the glow the sky has right behind it past the sky's
        // brightness away from the sun, the halo, which is the same sunlight scattered forwards.
        // Within three times the sky and on the narrow band (4 cover (1 - cover))^2: at eight times
        // on the broad band, with the game's bright halo and the bloom, whole clouds near the sun
        // washed out white. By the weather's glow, so rain's deck shows little of it.
        float3 halo = sky * (min(behind, 3.0 * skyLuma) - skyLuma) / max(behind, 1e-4);
        float band = 4.0 * cover * (1.0 - cover);
        float3 termRim = max(halo, 0.0) * (band * band * fGlow / 6.0);
        colour += termRim;
        // The haze mixes in the sky behind the cloud held within twice the frame's sky, where it used
        // to let the sky through: a cloud 3 km off let a tenth of it through, and with the sun's
        // halo behind it, many times the sky in the game's HDR, whole clouds near the sun came out
        // a flat cream. The horizon, near the frame's sky, fades as before.
        float3 hazeSky = sky * min(1.0, 2.0 * skyLuma / max(behind, 1e-4));
        float3 termHaze = hazeSky * (cover * (1.0 - haze));

        // VolumetricCloudsDebug: 2 the clouds grey at the sky they are matched to; 3 to 10 one term of
        // their light alone, at the brightness it adds (3 shade and sun, 4 silver lining, 5 sunset
        // colour, 6 glow, 7 sun power, 8 tops, 9 rim against the sun, 10 haze); in colours
        // (SunScale): 11 the share of the sun reaching inside, 12 the cloud the light's march found
        // towards the sun in layer thicknesses, 13 the extinction per metre times 10, 14 the
        // absorption; 15 as 11 with the sun straight overhead.
        [branch]
        if (fDebug >= 2.0)
        {
            float3 lightMul = shadeMul * kneeMul * skyMul;
            float3 shown = fDebug == 2.0 ? skyRef * cover
                         : fDebug == 3.0 ? termBase * lightMul * haze
                         : fDebug == 4.0 ? termSilver * lightMul * haze
                         : fDebug == 5.0 ? termSunset * lightMul * haze
                         : fDebug == 6.0 ? termGlow * lightMul * haze
                         : fDebug == 7.0 ? termSunPower * lightMul * haze
                         : fDebug == 8.0 ? termTop * lightMul * haze
                         : fDebug == 9.0 ? termRim * haze
                         : fDebug == 10.0 ? termHaze
                         : fDebug == 11.0 || fDebug == 15.0 ? SunScale(sums.sun / cover) * skyLuma * cover
                         : fDebug == 12.0 ? SunScale(sums.top / cover) * skyLuma * cover
                         : fDebug == 13.0 ? SunScale(vec4Shape.x * 10.0) * skyLuma * cover
                         : SunScale(fLightAbsorption) * skyLuma * cover;
            return float4(shown, 1.0 - cover);
        }
        return float4(colour * haze + termHaze, 1.0 - cover);
    }
    return float4(colour * haze, 1.0 - cover * haze);
}

// In one pass: the reflections, and the full size fallback while the half size targets are missing.
float4 Clouds_PS(float2 uv : TEXCOORD0, float2 vpos : VPOS) : COLOR0
{
    float rayScale;
    float3 dir = RayDirection(vpos, rayScale);
    return Light(March(uv, vpos, dir, rayScale, false, CLOUD_STEPS / 4, 4.0, 2.0, 0.25), dir, false, uv);
}

// The march at half size, its sums in two targets: (transmittance, sun, shade, silver) and (glow,
// first hit, top, height).
void CloudsMarch_PS(float2 uv : TEXCOORD0, float2 vpos : VPOS, out float4 sums0 : COLOR0, out float4 sums1 : COLOR1)
{
    float rayScale;
    float3 dir = RayDirection(vpos, rayScale);
    CloudSums sums = March(uv, vpos, dir, rayScale, true, CLOUD_STEPS, 1.0, LIGHT_STEPS, 0.05);
    sums0 = float4(sums.transmittance, sums.sun, sums.shade, sums.silver);
    sums1 = float4(sums.glow, sums.firstHit, sums.top, sums.height);
}

// The light from the march's sums, at half size, for the accumulation.
float4 CloudsLight_PS(float2 uv : TEXCOORD0, float2 vpos : VPOS) : COLOR0
{
    float4 sums0 = tex2Dlod(MarchTex0, float4(uv, 0, 0));
    float4 sums1 = tex2Dlod(MarchTex1, float4(uv, 0, 0));
    CloudSums sums;
    sums.transmittance = sums0.x;
    sums.sun = sums0.y;
    sums.shade = sums0.z;
    sums.silver = sums0.w;
    sums.glow = sums1.x;
    sums.firstHit = sums1.y;
    sums.top = sums1.z;
    sums.height = sums1.w;
    return Light(sums, WorldRay(vpos), true, uv);
}

// The history read with a Catmull-Rom filter in five bilinear reads: read bilinearly each frame as the
// camera moved, the clouds blurred a little more every frame and came out soft.
float4 SampleCatmullRom(sampler2D tex, float2 uv, float2 texel)
{
    float2 position = uv / texel;
    float2 centre = floor(position - 0.5) + 0.5;
    float2 f = position - centre;
    float2 w0 = f * (-0.5 + f * (1.0 - 0.5 * f));
    float2 w1 = 1.0 + f * f * (-2.5 + 1.5 * f);
    float2 w2 = f * (0.5 + f * (2.0 - 1.5 * f));
    float2 w3 = f * f * (-0.5 + 0.5 * f);
    float2 w12 = w1 + w2;
    float2 p0 = (centre - 1.0) * texel;
    float2 p3 = (centre + 2.0) * texel;
    float2 p12 = (centre + w2 / w12) * texel;
    float4 sum = tex2Dlod(tex, float4(p12.x, p0.y, 0, 0)) * (w12.x * w0.y)
               + tex2Dlod(tex, float4(p0.x, p12.y, 0, 0)) * (w0.x * w12.y)
               + tex2Dlod(tex, float4(p12, 0, 0)) * (w12.x * w12.y)
               + tex2Dlod(tex, float4(p3.x, p12.y, 0, 0)) * (w3.x * w12.y)
               + tex2Dlod(tex, float4(p12.x, p3.y, 0, 0)) * (w12.x * w3.y);
    float weight = w12.x * w0.y + w0.x * w12.y + w12.x * w12.y + w3.x * w12.y + w12.x * w3.y;
    return sum / weight;
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
    float4 history = clamp(SampleCatmullRom(HistoryTex, prev, vec4History.zw), lo, hi);
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

technique CloudsMarch
{
    pass March
    {
        VertexShader = compile vs_3_0 FullscreenQuadVS();
        PixelShader = compile ps_3_0 CloudsMarch_PS();
    }
}

technique CloudsLight
{
    pass Light
    {
        VertexShader = compile vs_3_0 FullscreenQuadVS();
        PixelShader = compile ps_3_0 CloudsLight_PS();
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

// The sky's colour this frame, for every cloud alike: the mean of the sky on an 8 x 6 grid
// over the screen, where the depth is clear and more than 25 degrees from the sun, so its halo
// stays out. Blended into a 1 x 1 target a tenth a frame, so turning the camera does not flicker
// the clouds; a frame with no sky in it leaves the target as it was.
float4 vec4SkyRefProj;    // ProjInfo for a 1 x 1 viewport: the view ray of a point in texture coordinates
float4 CloudsSkyRef_PS(float2 uv : TEXCOORD0) : COLOR0
{
    float3 sum = 0.0;
    float count = 0.0;
    [loop]
    for (int y = 0; y < 6; ++y)
    {
        [loop]
        for (int x = 0; x < 8; ++x)
        {
            float2 s = (float2(x, y) + 0.5) / float2(8.0, 6.0);
            if (RawToFar(tex2Dlod(DepthTex, float4(s, 0, 0)).r) < 1.0)
                continue;
            float3 v = float3(s * vec4SkyRefProj.xy + vec4SkyRefProj.zw, 1.0);
            float3 dir = normalize(float3(dot(v, vec4WorldX.xyz), dot(v, vec4WorldY.xyz), dot(v, vec4WorldZ.xyz)));
            if (dot(dir, vec3SunDir) > 0.906)
                continue;
            sum += tex2Dlod(SceneTex, float4(s, 0, 0)).rgb;
            count += 1.0;
        }
    }
    return count > 0.0 ? float4(0.1 * sum / count, 0.9) : float4(0.0, 0.0, 0.0, 1.0);
}

technique CloudsSkyRef
{
    pass SkyRef
    {
        VertexShader = compile vs_3_0 FullscreenQuadVS();
        PixelShader = compile ps_3_0 CloudsSkyRef_PS();
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
