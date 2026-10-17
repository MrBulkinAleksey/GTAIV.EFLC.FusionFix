// GPU grass (gpugrass.ixx): blades of grass grown on the ground triangles the procedural manager holds, drawn into
// the G-buffer at the end of its pass, so the game's lighting, shadows and fog take them as any other surface.
//
// One mesh of kBlades blades (stream 0) is drawn once per piece of ground (stream 1, instanced): a piece is a
// triangle of at most a metre and a half square, its corner and two edges; each blade stands at its own barycentric
// place on it, turned and sized by its own random values, and a piece keeps only the first `share` of its blades
// (by area, the surface's grass and the distance); the others go to one point and draw nothing.
//
// Each blade is one of three kinds, picked at random by the piece's mix of them, which is the mix of the props
// procedural.dat puts on its surface: grass (proc_grass*), weeds (*weed*), small plants with broad leaves
// (*proc_plant_*). Over the ground the grass grows in patches, taller and shorter, in tall tufts, greener and drier,
// some in flower (heads of red, white, yellow, violet); a quarter of its colour is the ground's under its root
// (GroundTex, a copy of the G-buffer's colour before the grass), so it takes the hue of the lawn, dirt or verge it grows
// on. Blades are long and thin, arc over in four segments, rounded across, lighter and yellower to the tip, with a
// faint broad highlight. Seen against the sun they let it through (their normal turns to it, so the lighting and its
// shadows take it).
//
// Bending, as the grass props have it (world_grass_wind.patch): the cars and people near (up to kBodies, Grass Bending)
// push the blades away from their box and lay them down under it, helicopters with their blades turning blow them
// away from under them, and the trample grid (TrampleTex, Grass Trample) keeps them down where something passed.
//
// G-buffer, as gta_default's deferred pixel shader writes it: colour, normal * 0.5 + 0.5, (no specular, the gloss of
// a material with no specular map, the occlusion), and the log depth of FusionShaders (log2(w / near) / log2(far /
// near)) to the depth buffer. The material ID target is left as the ground had it.

float4x4 matViewProj;
float4 vec4Wind;        // the wind's direction over the gust's wavelength (xy), the gust's phase, the flutter's phase (cycles)
float4 vec4WindDir;     // the wind's unit direction (xy), bend and flutter at the tip per metre of height
float4 vec4Blade;       // height scale, width scale, the ground's share in the colour, the debug colours (1 = by surface)
float4 vec4Eye;         // the camera's position
float4 vec4Sun;         // towards the sun (xyz), how much the blades let it through (0 at night)
float fNearPlane;
float fFarDivNear;
// The bodies, three each: (centre xy, forward xy), (half length, half width, 1 / reach, strength), (the ground they
// stand on, 1 for a helicopter's rotor, 0, 0); none with no strength
#define kBodies 8
float4 vec4Bodies[kBodies * 3];
float4 vec4Bend;        // a body's push at the tip per metre of height, 1 / the trample grid's side (m), its strength (0 = off)

sampler2D GroundTex : register(s0);    // a copy of _DEFERRED_GBUFFER_0_ before the grass
sampler2D TrampleTex : register(s1);   // vertex texture: direction (rg), strength (b), the ground's height modulo 64 m (a)

struct VSInput
{
    float4 blade : TEXCOORD0;   // barycentric u, v, its turn (radians), its height (0..1)
    float4 vertex : TEXCOORD1;  // up the blade (0..1), across it (-1..1), the blade's place among them (0..1), random
    float4 corner : TEXCOORD2;  // the piece's corner, a random value for its turn
    float4 edge1 : TEXCOORD3;   // its first edge, the share of its blades kept
    float4 edge2 : TEXCOORD4;   // its second edge, its surface's tag / 255 (debug colours)
    float4 kinds : TEXCOORD5;   // the share of grass, of weeds (the rest small plants), the surface's own grass (0..1)
};

struct VSOutput
{
    float4 position : POSITION;
    float4 colour : TEXCOORD0;  // the blade's own colour, occlusion
    float3 normal : TEXCOORD1;
    float2 w : TEXCOORD2;       // clip w, specular (×0.5, as the G-buffer has it)
    float4 ground : TEXCOORD3;  // the root's clip x, y, w; light from root to tip
    float3 tuft : TEXCOORD4;    // across the blade (-1..1), up it (0..1), 1 + a random value for a tuft (0 for a blade)
    float4 view : TEXCOORD5;    // from the camera to the vertex, how much the blade lets the sun through
};

float3 TagColour(float tag)
{
    float3 c = frac(tag * float3(17.13, 31.71, 53.37) * 255.0 / 97.0);
    return 0.15 + 0.7 * c;
}

// Cheap hash without sin (vertex slots are few)
float Hash(float2 p)
{
    float3 q = frac(p.xyx * 0.1031);
    q += dot(q, q.yzx + 33.33);
    return frac((q.x + q.y) * q.z);
}

// Smooth value noise, 0..1
float Noise(float2 p)
{
    float2 i = floor(p);
    float2 f = p - i;
    f = f * f * (3.0 - 2.0 * f);
    float a = Hash(i), b = Hash(i + float2(1.0, 0.0)), c = Hash(i + float2(0.0, 1.0)), d = Hash(i + float2(1.0, 1.0));
    return lerp(lerp(a, b, f.x), lerp(c, d, f.x), f.y);
}

// What presses the grass at `root` (a blade of `height`): the push's direction times its strength (xy), the strongest (z)
float3 Bending(float3 root, float height)
{
    float2 push = 0.0;
    float most = 0.0;
    [loop] for (int i = 0; i < kBodies; ++i)
    {
        float4 a = vec4Bodies[i * 3], b = vec4Bodies[i * 3 + 1], k = vec4Bodies[i * 3 + 2];
        // In the body's own axes: along it, across it; how far past its box
        float2 d = root.xy - a.xy;
        float2 local = float2(dot(d, a.zw), d.x * a.w - d.y * a.z);
        float gap = length(max(abs(local) - b.xy, 0.0));
        float strength = b.w;
        float invReach = b.z;
        float above = k.x - root.z;
        if (k.y > 0.5)
        {
            // A rotor's downwash: from 0.3 m above the ground, wider and weaker up to 30 m
            invReach = 1.0 / (6.0 + 0.4 * clamp(above, 0.0, 30.0));
            float fall = saturate(1.0 - above / 30.0);
            strength *= saturate((above - 0.3) / 0.7) * fall * fall;
        }
        else
            strength *= abs(above) <= height + 1.0 ? 1.0 : 0.0;
        float w = saturate(1.0 - gap * invReach);
        w = w * w * strength;
        float2 away = local / (b.xy + 0.3);
        away *= min(rsqrt(dot(away, away) + 1e-5), 1.0);
        push += float2(away.x * a.z + away.y * a.w, away.x * a.w - away.y * a.z) * w;
        most = max(most, w);
    }
    // Trampled: the grid's cell, where its ground is near the blade's
    float4 trample = tex2Dlod(TrampleTex, float4(root.xy * vec4Bend.y, 0.0, 0.0));
    float wrap = abs(frac(root.z / 64.0 - trample.a + 0.5) - 0.5) * 64.0;
    float trampled = saturate(2.5 - wrap) * trample.b * vec4Bend.z;
    push += (trample.rg * 2.0 - 1.0) * trampled;
    most = max(most, trampled);
    push *= min(most * rsqrt(dot(push, push) + 1e-5), 1.0);
    return float3(push, most);
}

VSOutput GrassVS(VSInput input)
{
    VSOutput output;
    float2 uv = input.blade.xy;
    if (uv.x + uv.y > 1.0)
        uv = 1.0 - uv;
    float3 root = input.corner.xyz + input.edge1.xyz * uv.x + input.edge2.xyz * uv.y;
    // Blades past the piece's share: all their vertices at one point behind the camera, nothing drawn
    [branch] if (input.vertex.z >= input.edge1.w)
    {
        output.position = float4(0.0, 0.0, -1.0, 1.0);
        output.colour = 0.0;
        output.normal = float3(0.0, 0.0, 1.0);
        output.w = float2(1.0, 0.0);
        output.ground = 0.0;
        output.tuft = 0.0;
        output.view = 0.0;
        return output;
    }

    float random = input.vertex.w;
    float t = input.vertex.x;

    // The blade's kind: grass, weed or small plant
    float pick = frac(random * 13.713 + input.corner.w * 7.31 + input.blade.w * 3.17);
    float isWeed = pick >= input.kinds.x && pick < input.kinds.x + input.kinds.y ? 1.0 : 0.0;
    float isPlant = pick >= input.kinds.x + input.kinds.y ? 1.0 : 0.0;
    float isGrass = 1.0 - isWeed - isPlant;

    // Patches over the ground: some taller, some shorter, tall tufts within them, some drier, some in flower
    float patch = Noise(root.xy * 0.13);
    float clump = Noise(root.xy * 0.9 + 31.7);
    float dry = smoothstep(0.5, 0.85, Noise(root.xy * 0.06 + 17.3));
    float bloom = smoothstep(0.55, 0.8, Noise(root.xy * 0.21 + 5.1));
    float second = frac(random * 31.17 + input.corner.w * 3.73);
    float isFlower = (1.0 - isPlant) * (second < bloom * 0.08 ? 1.0 : 0.0);

    // Long thin blades, curving over; tufts stand taller
    float h = input.blade.w;
    float height = isGrass * (0.2 + 0.45 * h * h) + isWeed * (0.35 + 0.5 * h) + isPlant * (0.10 + 0.16 * h);
    height *= lerp(0.6, 1.25, patch) * (clump > 0.62 ? 1.6 : lerp(0.75, 1.1, clump)) * lerp(0.4, 1.0, input.kinds.w) * vec4Blade.x;
    height *= 1.0 + 0.25 * isFlower;
    float width = (isGrass * 0.02 + isWeed * 0.03 + isPlant * 0.075) * vec4Blade.y;
    // Wider far off, where fewer of them stand and thin ones only flicker; past 12 m more and more grass and weed blades
    // are tufts of three stalks on one wider quad (cut out in the pixel shader), all of them past 26 m
    float distance = length(root - vec4Eye.xyz);
    width *= 1.0 + saturate((distance - 6.0) / 24.0) * 1.5;
    float isTuft = (1.0 - isPlant) * (1.0 - isFlower) * (frac(random * 3.31 + 0.17) < saturate((distance - 12.0) / 14.0) ? 1.0 : 0.0);
    width *= 1.0 + 2.2 * isTuft;
    // Grass and weeds narrow to the tip; leaves are widest in the middle; a flower is a thin stalk with a head
    float profile = lerp(1.0 - t, sin(t * 3.1415927) * 1.1 + 0.2 * (1.0 - t), isPlant);
    profile = isFlower > 0.0 ? (t > 0.75 ? 3.0 : 0.4) : profile;
    // How far the blade leans out from its root: grass arcs over, weeds stand, leaves spread
    float lean = isGrass * 0.7 + isWeed * 0.3 + isPlant * 1.1;
    lean *= 1.0 - 0.7 * isFlower;

    float turn = input.blade.z + input.corner.w * 6.2831853;
    float s, c;
    sincos(turn, s, c);
    float3 across = float3(c, s, 0.0);
    float3 facing = float3(-s, c, 0.0);

    // The wind as the props have it: a gust travelling downwind and a flutter across it, both by position
    float gust = frac(dot(root.xy, vec4Wind.xy) + vec4Wind.z) * 6.2831853 - 3.1415927;
    float flutter = frac((root.x + root.y) * 1.3 + vec4Wind.w + random) * 6.2831853 - 3.1415927;
    float bend = (sin(gust) * 0.55 + 0.6) * vec4WindDir.z * height;
    float side = sin(flutter) * vec4WindDir.w * height;
    float2 offset = vec4WindDir.xy * bend + float2(-vec4WindDir.y, vec4WindDir.x) * side;
    // Each blade's own lean, so the grass doesn't stand straight
    offset += facing.xy * (height * (random * 0.8 + 0.2) * lean);
    // Pressed: the wind dies under the push, the blade lies down
    float3 pressed = vec4Bend.x > 0.0 ? Bending(root, height) : 0.0;
    offset = offset * (1.0 - pressed.z) + pressed.xy * (vec4Bend.x * height);
    offset *= t * t;
    // Never longer than the blade; the tip drops as it bends
    float reach = length(offset);
    offset *= min(1.0, 0.9 * height / max(reach, 1e-4));
    float drop = dot(offset, offset) / (2.0 * height);

    float3 position = root + across * (input.vertex.y * width * 0.5 * profile) + float3(offset, t * height * (1.0 - 0.5 * pressed.z) - drop);

    output.position = mul(float4(position, 1.0), matViewProj);
    output.w = float2(output.position.w, 0.0);
    // Lit mostly from above, as grass seen as a lawn, rounded across the blade; leaves and flowers face up more
    float rounding = isFlower > 0.0 && t > 0.75 ? 0.0 : 0.45;
    output.normal = normalize(facing * lerp(0.35, 0.15, max(isPlant, isFlower)) + across * (input.vertex.y * rounding) + float3(0.0, 0.0, 1.0));

    // Its own colour: a hue by patch, by kind, darker at the root and lighter, yellower at the tip; drier in dry
    // patches (weeds most); each blade a little different
    float3 base = lerp(float3(0.17, 0.31, 0.09), float3(0.30, 0.36, 0.08), patch);
    float3 colour = isGrass * base + isWeed * float3(0.27, 0.30, 0.10) + isPlant * float3(0.13, 0.24, 0.07);
    colour = lerp(colour, float3(0.42, 0.37, 0.17), dry * lerp(0.5, 0.85, isWeed) * (1.0 - isPlant * 0.6));
    colour *= 1.0 + (random - 0.5) * 0.35;
    colour = lerp(colour, colour * float3(1.25, 1.12, 0.9) + float3(0.04, 0.03, 0.0), t * t * (1.0 - isPlant));
    // Flowers: red, white, yellow or violet heads
    float hue = frac(second * 7.77 + patch * 2.0);
    float3 petals = hue < 0.3 ? float3(0.55, 0.07, 0.04) : hue < 0.55 ? float3(0.72, 0.72, 0.66) : hue < 0.8 ? float3(0.75, 0.58, 0.08) : float3(0.38, 0.2, 0.55);
    colour = isFlower > 0.0 && t > 0.75 ? petals : colour;
    colour = vec4Blade.w > 0.0 ? TagColour(input.edge2.w) : colour;
    output.colour = float4(saturate(colour), lerp(0.4, 1.0, t));
    // A faint sheen on the blades, none on flowers and leaves
    output.w.y = 0.06 * t * isGrass * (1.0 - isFlower) * (1.0 - dry * 0.5);

    float4 rootClip = mul(float4(root, 1.0), matViewProj);
    output.ground = float4(rootClip.xy, rootClip.w, lerp(0.5, 1.05, t) * lerp(1.0, 0.85, clump * isGrass));
    output.tuft = float3(input.vertex.y, t, isTuft * (1.0 + random));
    output.view = float4(position - vec4Eye.xyz, t * (1.0 - isPlant) * (1.0 - dry * 0.4));
    return output;
}

struct PSOutput
{
    float4 colour : COLOR0;
    float4 normal : COLOR1;
    float4 specular : COLOR2;
    float depth : DEPTH;
};

PSOutput GrassPS(VSOutput input)
{
    PSOutput output;
    // A tuft: three stalks across the quad, each narrowing to its own height
    [branch] if (input.tuft.z > 0.5)
    {
        float u = input.tuft.x * 1.5 + 1.5;
        float strand = floor(u);
        float top = 1.0 - 0.35 * frac(strand * 0.618 + input.tuft.z * 7.3);
        clip(0.42 * saturate(1.0 - input.tuft.y / top) - abs(frac(u) - 0.5));
    }
    // The ground's colour under the root, kept no brighter than a lawn so pavement doesn't bleach the weeds on it
    float2 screen = input.ground.xy / max(input.ground.z, 1e-3) * float2(0.5, -0.5) + 0.5;
    float3 ground = tex2Dlod(GroundTex, float4(screen, 0.0, 0.0)).rgb;
    float luminance = dot(ground, float3(0.299, 0.587, 0.114));
    ground *= min(1.0, 0.3 / max(luminance, 0.01)) * 1.15;
    bool onScreen = input.ground.z > 0.05 && all(screen == saturate(screen));
    float share = onScreen && vec4Blade.w <= 0.0 ? vec4Blade.z : 0.0;
    float3 colour = lerp(input.colour.rgb, ground, share) * (vec4Blade.w > 0.0 ? lerp(0.6, 1.0, input.ground.w / 1.05) : input.ground.w);

    // Against the sun: a thin blade lets it through. Its normal turns to the sun, so the game's lighting (and its sun
    // shadow) lights it as from the front, and it warms a little
    float3 normal = normalize(input.normal);
    float backlit = saturate(dot(normalize(input.view.xyz), vec4Sun.xyz));
    backlit *= backlit * backlit * input.view.w * vec4Sun.w;
    normal = normalize(lerp(normal, vec4Sun.xyz, backlit * 0.8));
    colour *= 1.0 + backlit * float3(0.35, 0.3, 0.05);

    output.colour = float4(saturate(colour), 1.0);
    output.normal = float4(normal * 0.5 + 0.5, 1.0);
    // A faint broad highlight (gloss sqrt(24 / 512)); without one, gloss 258 / 1023 marks a material without a specular
    // map (world_no_specular_mark.patch)
    output.specular = float4(input.w.y, input.w.y > 0.001 ? 0.2165 : 0.252199411, input.colour.a, 1.0);
    output.depth = log2(max(input.w.x / fNearPlane, 1.0)) / log2(fFarDivNear);
    return output;
}

technique Grass
{
    pass P0
    {
        VertexShader = compile vs_3_0 GrassVS();
        PixelShader = compile ps_3_0 GrassPS();
    }
}
