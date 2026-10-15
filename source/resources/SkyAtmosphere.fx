// PBR sky (pbrsky.ixx): the colour of the clear sky from light scattered in an Earth-like atmosphere, after
// Hillaire, "A Scalable and Production Ready Sky and Atmosphere Rendering Technique" (EGSR 2020). Three tables:
//  - SkyTransmittance (256 x 64): how much sunlight gets through the air from a height towards a direction;
//  - SkyMultiScatter (32 x 32): the light scattered more than once, as seen from a height with the sun at an angle;
//  - SkyView (192 x 108): the sky seen from the camera, by the angle away from the sun's side and the elevation.
// The first two change only with the air (its haze), the third with the sun every frame. gta_atmoscatt_clouds
// (sky_pbr.patch) reads SkyView in place of the timecycle's gradient. The table keeps the timecycle's brightness at
// every height: each direction is scaled by what the sky at the same height a right angle from the sun needs to take
// the timecycle's colour there (SkyColor, plus the azimuth colours towards the horizon, as the dome's vertex shader
// makes it), by brightness and by hue as far as fPalette says. What the atmosphere adds is how the sky changes
// around: brighter and warmer towards the sun, cooler away from it, the sunset's bands, the earth's shadow. A knee
// keeps the bright side within twice the timecycle's colour there, so neither the sky nor the clouds matched to it
// come out brighter than the game's; only the glow within 20 degrees of the sun rises higher, white as the sunlight.
//
// Units: kilometres. The tables' own frame is z up, the sun at azimuth 0.

#define PI 3.14159265

// The planet and the top of its air.
#define GROUND_RADIUS 6360.0
#define TOP_RADIUS 6460.0

// Rayleigh (air), Mie (haze) and ozone, per kilometre at sea level, and their scale heights.
static const float3 kRayleighScattering = float3(5.802e-3, 13.558e-3, 33.1e-3);
static const float kRayleighHeight = 8.0;
static const float kMieScattering = 3.996e-3;
static const float kMieExtinction = 4.440e-3;
static const float kMieHeight = 1.2;
static const float3 kOzoneAbsorption = float3(0.650e-3, 1.881e-3, 0.085e-3);
#define OZONE_CENTRE 25.0
#define OZONE_HALF_WIDTH 15.0

#define TRANSMITTANCE_STEPS 40
#define MULTISCATTER_STEPS 20
#define SKY_STEPS 30

#define TRANSMITTANCE_SIZE float2(256.0, 64.0)
#define MULTISCATTER_SIZE 32.0

sampler2D TransmittanceTex : register(s0);
sampler2D MultiScatterTex : register(s1);

float4 vec4Air;       // x haze (Mie) multiplier, y camera height in km, z ground albedo, w Mie phase g
float3 vec3Sun;       // towards the sun in the tables' frame: (cos elevation, 0, sin elevation)
float3 vec3SkyColour;  // the timecycle's SkyColor
float4 vec4Azimuth;    // xyz the mean of its azimuth colours times AzimuthStrength, w AzimuthHeight
float4 vec4Match;      // x fPalette: 0 brightness only, 1 the timecycle's hue too; y the ceiling next to the sun; z, w the
                       // knee and the ceiling of the sky's brightness elsewhere, in times the timecycle's at that height

struct Medium
{
    float3 scattering;
    float3 extinction;
    float3 rayleigh;
    float mie;
};

Medium SampleMedium(float height)
{
    Medium m;
    const float h = max(height, 0.0);
    const float rayleighDensity = exp(-h / kRayleighHeight);
    const float mieDensity = exp(-h / kMieHeight) * vec4Air.x;
    const float ozoneDensity = max(0.0, 1.0 - abs(h - OZONE_CENTRE) / OZONE_HALF_WIDTH);
    m.rayleigh = kRayleighScattering * rayleighDensity;
    m.mie = kMieScattering * mieDensity;
    m.scattering = m.rayleigh + m.mie;
    m.extinction = m.rayleigh + kMieExtinction * mieDensity + kOzoneAbsorption * ozoneDensity;
    return m;
}

// Distance along a ray from a point r from the centre (on the z axis) with direction cosine mu to a sphere of the
// given radius, the nearer hit in front, or -1 if none.
float RaySphere(float r, float mu, float radius)
{
    const float b = r * mu;
    const float c = r * r - radius * radius;
    const float disc = b * b - c;
    if (disc < 0.0)
        return -1.0;
    const float s = sqrt(disc);
    const float t0 = -b - s;
    const float t1 = -b + s;
    return t0 > 0.0 ? t0 : (t1 > 0.0 ? t1 : -1.0);
}

// Bruneton's mapping of (height, direction cosine) to the transmittance table and back.
float2 TransmittanceUV(float r, float mu)
{
    const float H = sqrt(TOP_RADIUS * TOP_RADIUS - GROUND_RADIUS * GROUND_RADIUS);
    const float rho = sqrt(max(0.0, r * r - GROUND_RADIUS * GROUND_RADIUS));
    const float disc = r * r * (mu * mu - 1.0) + TOP_RADIUS * TOP_RADIUS;
    const float d = max(0.0, -r * mu + sqrt(max(disc, 0.0)));
    const float dMin = TOP_RADIUS - r;
    const float dMax = rho + H;
    return float2((d - dMin) / (dMax - dMin), rho / H);
}

void TransmittanceParams(float2 uv, out float r, out float mu)
{
    const float H = sqrt(TOP_RADIUS * TOP_RADIUS - GROUND_RADIUS * GROUND_RADIUS);
    const float rho = H * uv.y;
    r = sqrt(rho * rho + GROUND_RADIUS * GROUND_RADIUS);
    const float dMin = TOP_RADIUS - r;
    const float dMax = rho + H;
    const float d = dMin + uv.x * (dMax - dMin);
    mu = clamp((H * H - rho * rho - d * d) / (2.0 * r * max(d, 1e-4)), -1.0, 1.0);
}

// Sunlight reaching a point r from the centre with the sun at cosine mu from its zenith: none in the planet's shadow.
float3 SunTransmittance(float r, float mu)
{
    if (RaySphere(r, mu, GROUND_RADIUS) > 0.0)
        return 0.0;
    // Half a texel in from the edges: the table's corners hold its extremes.
    const float2 uv = TransmittanceUV(r, mu);
    const float2 texel = 1.0 / TRANSMITTANCE_SIZE;
    return tex2Dlod(TransmittanceTex, float4(uv * (1.0 - texel) + 0.5 * texel, 0, 0)).rgb;
}

float3 MultiScatter(float r, float mu)
{
    const float2 uv = float2(mu * 0.5 + 0.5, (r - GROUND_RADIUS) / (TOP_RADIUS - GROUND_RADIUS));
    const float texel = 1.0 / MULTISCATTER_SIZE;
    return tex2Dlod(MultiScatterTex, float4(saturate(uv) * (1.0 - texel) + 0.5 * texel, 0, 0)).rgb;
}

float RayleighPhase(float c)
{
    return 3.0 / (16.0 * PI) * (1.0 + c * c);
}

// Cornette-Shanks, Henyey-Greenstein with Rayleigh's shape for forward haze.
float MiePhase(float c, float g)
{
    const float g2 = g * g;
    const float k = 3.0 / (8.0 * PI) * (1.0 - g2) / (2.0 + g2);
    return k * (1.0 + c * c) / pow(max(1.0 + g2 - 2.0 * g * c, 1e-4), 1.5);
}

// The optical depth from a point r from the centre towards cosine mu up to the top of the air.
// A table's texel centres hold its parameters from 0 to 1 at the edge texels (the lookups above put them back).
float2 TexelToUnit(float2 uv, float2 size)
{
    return (uv - 0.5 / size) / (1.0 - 1.0 / size);
}

float4 Transmittance_PS(float2 uv : TEXCOORD0) : COLOR0
{
    float r, mu;
    TransmittanceParams(TexelToUnit(uv, TRANSMITTANCE_SIZE), r, mu);
    const float tMax = max(RaySphere(r, mu, TOP_RADIUS), 0.0);
    const float dt = tMax / TRANSMITTANCE_STEPS;
    float3 depth = 0.0;
    [loop]
    for (int i = 0; i < TRANSMITTANCE_STEPS; ++i)
    {
        const float t = (i + 0.5) * dt;
        const float height = sqrt(r * r + t * t + 2.0 * r * mu * t) - GROUND_RADIUS;
        depth += SampleMedium(height).extinction * dt;
    }
    return float4(exp(-depth), 1.0);
}

// The light scattered more than once (Hillaire's Psi_ms): the second order from 64 directions around a point lit
// by the sun, summed as a series of ever higher orders, each the share f_ms of the one before.
float4 MultiScatter_PS(float2 texel : TEXCOORD0) : COLOR0
{
    const float2 uv = saturate(TexelToUnit(texel, MULTISCATTER_SIZE));
    const float sunMu = uv.x * 2.0 - 1.0;
    const float r = GROUND_RADIUS + saturate(uv.y) * (TOP_RADIUS - GROUND_RADIUS) + 0.01;
    const float3 sun = float3(sqrt(saturate(1.0 - sunMu * sunMu)), 0.0, sunMu);
    float3 second = 0.0;
    float3 share = 0.0;
    [loop]
    for (int j = 0; j < 8; ++j)
    {
        [loop]
        for (int k = 0; k < 8; ++k)
        {
            // Evenly over the sphere: equal steps of cos(theta) and of phi.
            const float cosTheta = 1.0 - (j + 0.5) / 4.0;
            const float sinTheta = sqrt(saturate(1.0 - cosTheta * cosTheta));
            const float phi = (k + 0.5) * (2.0 * PI / 8.0);
            const float3 dir = float3(sinTheta * cos(phi), sinTheta * sin(phi), cosTheta);
            const float mu = dir.z;
            const float tGround = RaySphere(r, mu, GROUND_RADIUS);
            const float tTop = RaySphere(r, mu, TOP_RADIUS);
            const float tMax = tGround > 0.0 ? tGround : max(tTop, 0.0);
            const float dt = tMax / MULTISCATTER_STEPS;
            float3 throughput = 1.0;
            float3 light = 0.0;
            float3 scatter = 0.0;
            for (int i = 0; i < MULTISCATTER_STEPS; ++i)
            {
                const float t = (i + 0.5) * dt;
                const float3 p = float3(0.0, 0.0, r) + dir * t;
                const float pr = length(p);
                const float3 up = p / pr;
                const Medium m = SampleMedium(pr - GROUND_RADIUS);
                const float3 stepTransmittance = exp(-m.extinction * dt);
                const float3 lit = m.scattering * SunTransmittance(pr, dot(up, sun)) / (4.0 * PI);
                light += throughput * (lit - lit * stepTransmittance) / max(m.extinction, 1e-7);
                scatter += throughput * (m.scattering - m.scattering * stepTransmittance) / max(m.extinction, 1e-7);
                throughput *= stepTransmittance;
            }
            // The ground, a diffuse reflector, lit by the sun.
            if (tGround > 0.0)
            {
                const float3 p = float3(0.0, 0.0, r) + dir * tGround;
                const float3 up = normalize(p);
                const float sunUp = dot(up, sun);
                light += throughput * SunTransmittance(GROUND_RADIUS, sunUp) * saturate(sunUp) * vec4Air.z / PI;
            }
            second += light;
            share += scatter;
        }
    }
    second /= 64.0;
    share /= 64.0;
    return float4(second / (1.0 - min(share, 0.99)), 1.0);
}

// The sky seen along dir (the tables' frame) from the camera, for unit sunlight.
float3 SkyRadiance(float3 dir)
{
    const float r = GROUND_RADIUS + max(vec4Air.y, 0.0) + 0.001;
    const float mu = dir.z;
    const float tGround = RaySphere(r, mu, GROUND_RADIUS);
    const float tTop = RaySphere(r, mu, TOP_RADIUS);
    const float tMax = tGround > 0.0 ? tGround : max(tTop, 0.0);
    const float c = dot(dir, vec3Sun);
    const float phaseR = RayleighPhase(c);
    const float phaseM = MiePhase(c, vec4Air.w);
    float3 throughput = 1.0;
    float3 light = 0.0;
    float tPrev = 0.0;
    [loop]
    for (int i = 0; i < SKY_STEPS; ++i)
    {
        // Denser near the camera, where the air is thickest along low rays.
        const float s = (i + 0.3) / SKY_STEPS;
        const float t = tMax * s * s;
        const float dt = t - tPrev;
        tPrev = t;
        const float3 p = float3(0.0, 0.0, r) + dir * t;
        const float pr = length(p);
        const float sunMu = dot(p / pr, vec3Sun);
        const Medium m = SampleMedium(pr - GROUND_RADIUS);
        const float3 sunLight = SunTransmittance(pr, sunMu);
        const float3 multi = MultiScatter(pr, sunMu);
        const float3 lit = m.rayleigh * (phaseR * sunLight + multi) + m.mie * (phaseM * sunLight + multi);
        const float3 stepTransmittance = exp(-m.extinction * dt);
        light += throughput * (lit - lit * stepTransmittance) / max(m.extinction, 1e-7);
        throughput *= stepTransmittance;
    }
    return light;
}

// The SkyView table's mapping, which sky_pbr.patch repeats in gta_atmoscatt_clouds: u the angle away from the
// sun's side over pi, v the elevation, square-rooted about the horizon so the horizon gets most rows.
float3 SkyViewDir(float2 uv)
{
    const float azimuth = uv.x * PI;
    const float x = uv.y * 2.0 - 1.0;
    // v 0 the zenith, as the patch reads it (1 - elevation).
    const float elevation = -sign(x) * x * x * (PI / 2.0);
    const float ce = cos(elevation);
    return float3(ce * cos(azimuth), ce * sin(azimuth), sin(elevation));
}

float Luminance(float3 c)
{
    return dot(c, float3(0.2126, 0.7152, 0.0722));
}

// What the sky's colour is scaled by to take the timecycle's at a point: by brightness, or channel by channel.
float3 MatchScale(float3 pbr, float3 target, float palette)
{
    const float3 byBrightness = Luminance(target) / max(Luminance(pbr), 1e-7);
    const float3 byChannel = target / max(pbr, 1e-7);
    return lerp(byBrightness, byChannel, palette);
}

// The timecycle's sky at a height (the sine of the elevation), as gta_atmoscatt_clouds' vertex shader makes it with
// the mean of its west and east azimuth colours.
float3 TimecycleSky(float up)
{
    return vec3SkyColour + vec4Azimuth.xyz * (1.0 - saturate(up * vec4Azimuth.w));
}

float4 SkyView_PS(float2 uv : TEXCOORD0) : COLOR0
{
    const float3 dir = SkyViewDir(uv);
    const float3 sky = SkyRadiance(dir);
    // The same height a right angle from the sun takes the timecycle's colour there; below the horizon, the horizon's.
    const float3 side = SkyRadiance(float3(0.0, sqrt(saturate(1.0 - dir.z * dir.z)), dir.z));
    const float3 target = TimecycleSky(max(dir.z, 0.0));
    // The haze's glow about the sun, within some 20 degrees of it, most within 10: held at twice the timecycle's
    // there it was a dull beige smudge, at the timecycle's hue. There the ceiling rises towards vec4Match.y and the
    // hue is the air's own, the sunlight's.
    const float aureole = saturate((dot(dir, vec3Sun) - 0.9397) / (1.0 - 0.9397));
    const float nearSun = aureole * aureole;
    const float3 matched = sky * MatchScale(side, target, vec4Match.x * (1.0 - nearSun));
    // Towards a low sun the sky comes out many times the side's: past the knee its brightness rolls off towards the
    // ceiling, keeping the hue.
    const float l = Luminance(matched) / max(Luminance(target), 1e-7);
    const float knee = vec4Match.z, room = lerp(vec4Match.w, vec4Match.y, nearSun) - vec4Match.z;
    const float rolled = l <= knee ? l : knee + room * (1.0 - exp(-(l - knee) / room));
    return float4(matched * (rolled / max(l, 1e-7)), 1.0);
}

void FullscreenQuadVS(in float4 iPos : POSITION, in float2 iUV : TEXCOORD0,
                      out float4 oPos : POSITION, out float2 oUV : TEXCOORD0)
{
    oPos = iPos;
    oUV = iUV;
}

technique SkyTransmittance
{
    pass Transmittance
    {
        VertexShader = compile vs_3_0 FullscreenQuadVS();
        PixelShader = compile ps_3_0 Transmittance_PS();
    }
}

technique SkyMultiScatter
{
    pass MultiScatter
    {
        VertexShader = compile vs_3_0 FullscreenQuadVS();
        PixelShader = compile ps_3_0 MultiScatter_PS();
    }
}

technique SkyView
{
    pass View
    {
        VertexShader = compile vs_3_0 FullscreenQuadVS();
        PixelShader = compile ps_3_0 SkyView_PS();
    }
}
