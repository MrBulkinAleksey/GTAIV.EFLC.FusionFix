// GPU grass (gpugrass.ixx): cards of grass, each a clump of thin blades from a texture (CardTex), grown on the ground
// triangles the procedural manager holds, drawn into the G-buffer at the end of its pass, so the game's lighting,
// shadows and fog take them as any other surface.
//
// One mesh of kCards cards (stream 0) is drawn once per piece of ground (stream 1, instanced): a piece is a triangle of
// at most a metre and a half square, its corner and two edges; each card stands at its own barycentric place on it,
// turned and sized by its own random values, and a piece keeps only the first `share` of its cards (by area, the
// surface's grass and the distance); the others go to one point and draw nothing. A card is an upright quad of three
// rows, so the wind bends it; CardTex (gpugrass.ixx MakeCardTexture) holds four clumps side by side: two of grass, one
// of weeds and broad leaves, one of grass with a few flowers. Its texels are not colours but what the pixel shader
// colours them by: each blade's own shade (r; heads of flowers at the top of it), how far up the blade (g), where
// across it (b, the blade's rounding), coverage (a, clipped at a half; the mips keep the coverage of the full size).
//
// Each card is one of three kinds, picked at random by the piece's mix of them, which is the mix of the props
// procedural.dat puts on its surface: grass (proc_grass*), weeds (*weed*), small plants with broad leaves
// (*proc_plant_*). Over the ground the grass grows in patches, taller and shorter, greener and drier, some in flower
// (heads of red, white, yellow, violet); a little of its colour is the ground's under its root (GroundTex, a copy of
// the G-buffer's colour before the grass). Blades are lush green (vec4Green), dark at the root, yellower to the tip
// (vec4Tip), a few dry ones among them (vec4Dry), with a faint broad highlight. Seen against the sun they let it
// through (their normal turns to it, so the lighting and its shadows take it) and glow warm.
//
// Bending, as the grass props have it (world_grass_wind.patch): the cars and people near (up to kBodies, Grass Bending)
// push the cards away from their box and lay them down under it, helicopters with their blades turning blow them
// away from under them, and the trample grid (TrampleTex, Grass Trample) keeps them down where something passed.
//
// G-buffer, as gta_default's deferred pixel shader writes it: colour, normal * 0.5 + 0.5, (no specular, the gloss of
// a material with no specular map, the occlusion), and the log depth of FusionShaders (log2(w / near) / log2(far /
// near)) to the depth buffer. The material ID target is left as the ground had it. The log depth is made per vertex
// (LogDepthClip), not written by the pixel shader: a written depth turns off the GPU's depth test before the pixel
// shader, and the cards' many layers were all shaded, even those behind others; over a card's few centimetres the
// log interpolated linearly is as good as exact.

float4x4 matViewProj;
float4 vec4Wind;        // the wind's direction over the gust's wavelength (xy), the gust's phase, the flutter's phase (cycles)
float4 vec4WindDir;     // the wind's unit direction (xy), bend and flutter at the tip per metre of height
float4 vec4Blade;       // height scale, width scale, the ground's share in the colour, the debug colours (1 = by surface)
float4 vec4Eye;         // the camera's position
float4 vec4Sun;         // towards the sun (xyz), how much the blades let it through (0 at night)
float4 vec4Green;       // the grass's colour in its greenest patches (the others darker and yellower)
float4 vec4Tip;         // what the blades turn to towards their tips
float4 vec4Dry;         // dry blades and dry patches
float fNearPlane;
float fFarDivNear;
// The bodies, three each: (centre xy, forward xy), (half length, half width, 1 / reach, strength), (the ground they
// stand on, 1 for a helicopter's rotor, 0, 0); none with no strength
#define kBodies 8
float4 vec4Bodies[kBodies * 3];
float4 vec4Bend;        // a body's push at the tip per metre of height, 1 / the trample grid's side (m), the grid's texture
                        // coordinate offset (GrassShared::TrampleUVOffset; far off without the grid)

sampler2D GroundTex : register(s0);    // a copy of _DEFERRED_GBUFFER_0_ before the grass
sampler2D CardTex : register(s7);      // the four clumps of blades, mipmapped, linear
sampler2D TrampleTex : register(s1);   // vertex texture: direction (rg), strength (b), the ground's height modulo 64 m (a)
sampler2D EdgeTex : register(s2);      // vertex texture, R16F, linear: how far from ground without grass (0..1), per 0.5 m cell
float4 vec4EdgeGrid;    // EdgeTex's origin (xy), 1 / its side (m), 1 with EdgeTex

struct VSInput
{
    float4 blade : TEXCOORD0;   // barycentric u, v, its turn (radians), its height (0..1)
    float4 vertex : TEXCOORD1;  // up the card (0, 0.5, 1), across it (-1, 1), the card's place among them (0..1), random
    float4 corner : TEXCOORD2;  // the piece's corner, a random value for its turn
    float4 edge1 : TEXCOORD3;   // its first edge, the share of its cards kept
    float4 edge2 : TEXCOORD4;   // its second edge, its surface's tag / 255 (debug colours)
    float4 kinds : TEXCOORD5;   // the share of grass, of weeds (the rest small plants), 1 / the share over which a
                                // card rises (GpuGrass::RiseScale), the surface's own grass (0..1)
};

struct VSOutput
{
    float4 position : POSITION;
    float4 colour : TEXCOORD0;  // the card's own colour, how dense the grass is around it (dark roots)
    float4 normal : TEXCOORD1;  // its normal, how far it has faded in (Card.fade)
    float specular : TEXCOORD2; // specular (×0.5, as the G-buffer has it)
    float4 ground : TEXCOORD3;  // the root's clip x, y, w; the clump's light (darker in clumps)
    float4 uv : TEXCOORD4;      // CardTex's coordinates, how far off (0 near, 1 far), how dry
    float4 view : TEXCOORD5;    // from the camera to the vertex, how much the card lets the sun through
    float4 across : TEXCOORD6;  // across the card, times the blades' rounding (xyz); 1 on a card with flowers
    float3 petals : TEXCOORD7;  // the flowers' colour
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

// What presses the grass at `root` (a card of `height`): the push's direction times its strength (xy), the strongest (z)
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
    // Trampled: the grid's cell, where its ground is near the card's
    float4 trample = tex2Dlod(TrampleTex, float4(root.xy * vec4Bend.y + vec4Bend.zw, 0.0, 0.0));
    float wrap = abs(frac(root.z / 64.0 - trample.a + 0.5) - 0.5) * 64.0;
    float trampled = saturate(2.5 - wrap) * trample.b;
    push += (trample.rg * 2.0 - 1.0) * trampled;
    most = max(most, trampled);
    push *= min(most * rsqrt(dot(push, push) + 1e-5), 1.0);
    return float3(push, most);
}

// A card's place on its piece, and whether it is drawn: past the piece's share, thinned out by an edge of the grass
// (a sixth of them kept at the edge itself) or past 15 m one in two going to make the others twice as wide (LOD), it
// is not
bool CardKept(VSInput input, out float3 root, out float edge, out float distance, out float keep)
{
    float2 uv = input.blade.xy;
    if (uv.x + uv.y > 1.0)
        uv = 1.0 - uv;
    root = input.corner.xyz + input.edge1.xyz * uv.x + input.edge2.xyz * uv.y;
    // At the root itself, filtered between the cells; without EdgeTex none (hard edges)
    edge = vec4EdgeGrid.w > 0.0 ? tex2Dlod(EdgeTex, float4((root.xy - vec4EdgeGrid.xy) * vec4EdgeGrid.z, 0.0, 0.0)).r : 1.0;
    distance = length(root - vec4Eye.xyz);
    float thinned = 0.5 * saturate((distance - 15.0) / 15.0);
    // How far each test is from leaving the card out, in the share of cards: MakeCard shortens it over the last
    // twelfth, so no card appears or goes at once as the camera moves
    keep = min(edge * 1.2 + 0.15 - frac(input.vertex.w * 7.13 + 0.31), frac(input.vertex.w * 5.37 + 0.71) - thinned);
    return input.vertex.z < input.edge1.w && keep >= 0.0;
}

struct Card
{
    float3 root;
    float t, random, height, width, lean, column;
    float3 across, facing;
    float isGrass, isWeed, isPlant, isFlower;
    float patch, clump, dry, second, distant;
    float fade;     // 0..1: a card with flowers fading in as it appears, by a dither the temporal AA smooths (ClipFade)
};

Card MakeCard(VSInput input, float3 root, float edge, float distance, float keep)
{
    Card b;
    b.root = root;
    b.random = input.vertex.w;
    b.t = input.vertex.x;
    // How far off, for what blurs into a lawn there: 0 within 8 m, 1 past 28 m
    b.distant = saturate((distance - 8.0) / 20.0);

    // The card's kind: grass, weed or small plant
    float pick = frac(b.random * 13.713 + input.corner.w * 7.31 + input.blade.w * 3.17);
    b.isWeed = pick >= input.kinds.x && pick < input.kinds.x + input.kinds.y ? 1.0 : 0.0;
    b.isPlant = pick >= input.kinds.x + input.kinds.y ? 1.0 : 0.0;
    b.isGrass = 1.0 - b.isWeed - b.isPlant;

    // Patches over the ground: some taller, some shorter, clumps within them, some drier, some in flower (both of
    // those from the patches' noise, a second field of it costs too many vertex slots)
    b.patch = Noise(root.xy * 0.13);
    b.clump = Noise(root.xy * 0.9 + 31.7);
    float wide = Noise(root.xy * 0.06 + 17.3);
    b.dry = smoothstep(0.55, 0.9, wide);
    float bloom = smoothstep(0.55, 0.8, frac(wide * 2.7 + b.patch * 0.6));
    b.second = frac(b.random * 31.17 + input.corner.w * 3.73);
    b.isFlower = b.isGrass * (b.second < bloom * 0.12 ? 1.0 : 0.0);

    // A thick lawn knee-high in its tall patches; weeds taller, leaves low
    float h = input.blade.w;
    float height = b.isGrass * (0.24 + 0.3 * h) + b.isWeed * (0.32 + 0.4 * h) + b.isPlant * (0.12 + 0.14 * h);
    height *= lerp(0.65, 1.3, b.patch) * lerp(0.85, 1.15, b.clump) * lerp(0.45, 1.0, input.kinds.w) * vec4Blade.x;
    // Shorter towards an edge; and the cards last kept rise out of the ground as the share grows (the camera coming
    // nearer) over about two metres of the way, so they neither pop in nor grow slowly over tens of metres far off;
    // by edges and LOD over the last twelfth. Flowers fade in at their full height instead: tall bright heads shooting
    // up out of the ground caught the eye
    float rise = saturate(min((input.edge1.w - input.vertex.z) * input.kinds.z, keep * 12.0) + 0.05);
    b.height = height * lerp(0.35, 1.0, edge) * (b.isFlower > 0.0 ? 1.0 : rise);
    b.fade = b.isFlower > 0.0 ? rise : 1.0;
    // As wide as tall or so (CardTex's clumps are half as wide as tall, the blades spread); wider far off, where
    // fewer of them stand (twice past 30 m for the half CardKept left out)
    float width = height * (b.isGrass * 0.85 + b.isWeed * 0.75 + b.isPlant * 1.1) * vec4Blade.y;
    b.width = width * (1.0 + saturate((distance - 10.0) / 30.0) * 0.4) / (1.0 - 0.5 * saturate((distance - 15.0) / 15.0));
    // CardTex's clump: grass one of two, weeds and leaves the third, grass in flower the fourth
    b.column = b.isFlower > 0.0 ? 3.0 : b.isGrass > 0.0 ? (frac(b.random * 5.31 + 0.27) < 0.5 ? 0.0 : 1.0) : 2.0;
    // How far the card leans out from its root (the blades in it curve on their own)
    b.lean = b.isGrass * 0.25 + b.isWeed * 0.15 + b.isPlant * 0.35;

    float turn = input.blade.z + input.corner.w * 6.2831853;
    float s, c;
    sincos(turn, s, c);
    b.across = float3(c, s, 0.0);
    b.facing = float3(-s, c, 0.0);
    return b;
}

// The vertex at the wind's phases `wind.zw` (gust, flutter), pressed as `pressed` (Bending)
float3 CardPosition(VSInput input, Card b, float4 wind, float3 pressed)
{
    float height = b.height;
    // The wind as the props have it: a gust travelling downwind and a flutter across it, both by position
    float gust = frac(dot(b.root.xy, wind.xy) + wind.z) * 6.2831853 - 3.1415927;
    float flutter = frac((b.root.x + b.root.y) * 1.3 + wind.w + b.random) * 6.2831853 - 3.1415927;
    float bend = (sin(gust) * 0.55 + 0.6) * vec4WindDir.z * height;
    float side = sin(flutter) * vec4WindDir.w * height;
    float2 offset = vec4WindDir.xy * bend + float2(-vec4WindDir.y, vec4WindDir.x) * side;
    // Each card's own lean, so the grass doesn't stand straight
    offset += b.facing.xy * (height * (b.random * 0.8 + 0.2) * b.lean);
    // Pressed: the wind dies under the push, the card lies down
    offset = offset * (1.0 - pressed.z) + pressed.xy * (vec4Bend.x * height);
    offset *= b.t * b.t;
    // Never longer than the card; the top drops as it bends
    float reach = length(offset);
    offset *= min(1.0, 0.9 * height / max(reach, 1e-4));
    float drop = dot(offset, offset) / (2.0 * height);
    // The blades spread out a little towards the top
    return b.root + b.across * (input.vertex.y * b.width * 0.5 * (1.0 + 0.3 * b.t)) + float3(offset, b.t * height * (1.0 - 0.5 * pressed.z) - drop);
}

// `clip` with the log depth for its z (as z / w): what the pixel shader would write, interpolated per vertex
float4 LogDepthClip(float4 clip, float bias)
{
    clip.z = (log2(max(clip.w / fNearPlane, 1.0)) / log2(fFarDivNear) - bias) * clip.w;
    return clip;
}

// CardTex's coordinates of the vertex
float2 CardUV(VSInput input, Card b)
{
    return float2((input.vertex.y * 0.5 + 0.5 + b.column) * 0.25, 1.0 - b.t);
}

VSOutput GrassVS(VSInput input)
{
    VSOutput output;
    float3 root;
    float edge, distance, keep;
    // Not drawn: all its vertices at one point behind the camera
    [branch] if (!CardKept(input, root, edge, distance, keep))
    {
        output.position = float4(0.0, 0.0, -1.0, 1.0);
        output.colour = 0.0;
        output.normal = float4(0.0, 0.0, 1.0, 1.0);
        output.specular = 0.0;
        output.ground = 0.0;
        output.uv = 0.0;
        output.view = 0.0;
        output.across = 0.0;
        output.petals = 0.0;
        return output;
    }
    Card b = MakeCard(input, root, edge, distance, keep);
    float3 pressed = vec4Bend.x > 0.0 ? Bending(root, b.height) : 0.0;
    float3 position = CardPosition(input, b, vec4Wind, pressed);

    output.position = LogDepthClip(mul(float4(position, 1.0), matViewProj), 0.0);
    // Lit mostly from above, as grass seen as a lawn; each blade rounded across (CardTex's b in the pixel shader);
    // leaves face up more. Far off the blades' own normals go over to the lawn's (up), so the light doesn't sparkle
    float3 normal = b.facing * lerp(0.35, 0.15, b.isPlant);
    output.normal = float4(normalize(normal * (1.0 - 0.8 * b.distant) + float3(0.0, 0.0, 1.0)), b.fade);
    output.across = float4(b.across * (0.5 * (1.0 - 0.8 * b.distant)), b.isFlower);

    // Its own colour: the green of the lawn, darker and yellower patch by patch, by kind; drier in dry patches (weeds
    // most); each card a little different, more so near (so the clumps part) than far (where they blur). Darker at
    // the roots and yellower to the tips blade by blade in the pixel shader
    float3 base = lerp(vec4Green.rgb * float3(0.72, 0.8, 0.7), vec4Green.rgb, b.patch);
    float3 colour = b.isGrass * base + b.isWeed * lerp(base, vec4Dry.rgb, 0.3) + b.isPlant * vec4Green.rgb * float3(0.6, 0.75, 0.7);
    colour = lerp(colour, vec4Dry.rgb, b.dry * lerp(0.45, 0.8, b.isWeed) * (1.0 - b.isPlant * 0.6));
    colour *= 1.0 + (b.random - 0.5) * lerp(0.35, 0.12, b.distant);
    colour = vec4Blade.w > 0.0 ? TagColour(input.edge2.w) : colour;
    // Flowers: red, white, yellow or violet heads
    float hue = frac(b.second * 7.77 + b.patch * 2.0);
    output.petals = hue < 0.3 ? float3(0.55, 0.12, 0.08) : hue < 0.55 ? float3(0.7, 0.68, 0.62) : hue < 0.8 ? float3(0.68, 0.55, 0.12) : float3(0.4, 0.26, 0.5);
    // The roots of dense grass near are in the dark of the blades around them
    float dense = saturate(input.edge1.w) * (1.0 - b.distant);
    output.colour = float4(saturate(colour), dense);
    // A faint sheen on the blades, none on leaves, nor far off
    output.specular = 0.05 * (1.0 - b.isPlant) * (1.0 - b.dry * 0.5) * (1.0 - b.distant);

    float4 rootClip = mul(float4(root, 1.0), matViewProj);
    output.ground = float4(rootClip.xy, rootClip.w, lerp(1.0, 0.85, b.clump * b.isGrass));
    output.uv = float4(CardUV(input, b), b.distant, b.dry);
    output.view = float4(position - vec4Eye.xyz, (1.0 - b.isPlant) * (1.0 - b.dry * 0.4) * (1.0 - 0.5 * b.distant));
    return output;
}

// Motion vectors (temporal AA, DLAA, FSR): the cards drawn again into the motion target over the camera's motion,
// which only knows the depth: where they are now and where they were at the wind's phases of the previous frame, both
// without jitter. Bending is taken as it is now.
float4x4 matViewProjNoJitter;
float4x4 matPrevViewProj;
float4 vec4WindPrev;    // vec4Wind of the previous frame
float fDepthBias;

struct MotionOutput
{
    float4 position : POSITION;
    float4 current : TEXCOORD0;  // clip x, y, w; Card.fade
    float3 previous : TEXCOORD1;
    float2 uv : TEXCOORD2;       // CardTex's coordinates
};

MotionOutput MotionVS(VSInput input)
{
    MotionOutput output;
    float3 root;
    float edge, distance, keep;
    [branch] if (!CardKept(input, root, edge, distance, keep))
    {
        output.position = float4(0.0, 0.0, -1.0, 1.0);
        output.current = float4(0.0, 0.0, 1.0, 1.0);
        output.previous = float3(0.0, 0.0, 1.0);
        output.uv = 0.0;
        return output;
    }
    Card b = MakeCard(input, root, edge, distance, keep);
    float3 pressed = vec4Bend.x > 0.0 ? Bending(root, b.height) : 0.0;
    float3 position = CardPosition(input, b, vec4Wind, pressed);
    float3 before = CardPosition(input, b, vec4WindPrev, pressed);
    // A hair nearer than the grass wrote itself, so the depth test keeps it where the grass is what is seen
    output.position = LogDepthClip(mul(float4(position, 1.0), matViewProj), fDepthBias);
    output.current = float4(mul(float4(position, 1.0), matViewProjNoJitter).xyw, b.fade);
    output.previous = mul(float4(before, 1.0), matPrevViewProj).xyw;
    output.uv = CardUV(input, b);
    return output;
}

float fDitherFrame;     // the frame's turn of the fade's dither (0..63), so the temporal AA averages it out

// Fading in: pixels left out by interleaved gradient noise, other ones each frame
void ClipFade(float fade, float2 vPos)
{
    [branch] if (fade < 1.0)
    {
        float2 p = vPos + fDitherFrame * 5.588238;
        clip(fade - frac(52.9829189 * frac(dot(p, float2(0.06711056, 0.00583715)))));
    }
}

float4 MotionPS(MotionOutput input, float2 vPos : VPOS) : COLOR0
{
    clip(tex2D(CardTex, input.uv).a - 0.5);
    ClipFade(input.current.w, vPos);
    float2 current = input.current.xy / input.current.z;
    float2 previous = input.previous.xy / input.previous.z;
    return float4((previous - current) * float2(0.5, -0.5), 0.0, 1.0);
}

struct PSOutput
{
    float4 colour : COLOR0;
    float4 normal : COLOR1;
    float4 specular : COLOR2;
};

PSOutput GrassPS(VSOutput input, float2 vPos : VPOS)
{
    PSOutput output;
    // The blade under the pixel: its shade (r), how far up it (g), across it (b), coverage (a)
    float4 card = tex2D(CardTex, input.uv.xy);
    clip(card.a - 0.5);
    ClipFade(input.normal.w, vPos);
    float up = card.g;
    float shade = card.r;
    // The ground's colour under the root, kept no brighter than a lawn so pavement doesn't bleach the weeds on it
    float2 screen = input.ground.xy / max(input.ground.z, 1e-3) * float2(0.5, -0.5) + 0.5;
    float3 ground = tex2Dlod(GroundTex, float4(screen, 0.0, 0.0)).rgb;
    float luminance = dot(ground, float3(0.299, 0.587, 0.114));
    ground *= min(1.0, 0.3 / max(luminance, 0.01)) * 1.15;
    bool onScreen = input.ground.z > 0.05 && all(screen == saturate(screen));
    bool debug = vec4Blade.w > 0.0;
    float share = onScreen && !debug ? vec4Blade.z : 0.0;

    // Blade by blade: lighter and darker ones (less so far off), one in eight or so dry; yellower to the tip, dark at
    // the root where the grass is dense
    float3 colour = input.colour.rgb * lerp(1.0 + (shade - 0.5) * 0.6, 1.0, input.uv.z * 0.7);
    colour = lerp(colour, vec4Dry.rgb * (0.8 + 0.4 * shade), (shade < 0.12 ? 0.8 : 0.0) * (1.0 - input.uv.z));
    colour = lerp(colour, vec4Tip.rgb, up * up * 0.55 * (1.0 - input.uv.w * 0.5));
    // Heads of flowers: the top of CardTex's r on the flower clump
    colour = input.across.w > 0.0 && shade > 0.93 ? input.petals : colour;
    colour = lerp(colour, ground, share);
    float dense = input.colour.a;
    float light = lerp(lerp(0.55, 0.3, dense), 1.05, saturate(up * 1.3)) * input.ground.w;
    colour *= debug ? lerp(0.6, 1.0, light / 1.05) : light;

    // Rounded across the blade; against the sun a thin blade lets it through: its normal turns to the sun, so the
    // game's lighting (and its sun shadow) lights it as from the front, and it glows warm
    float3 normal = normalize(input.normal.xyz + input.across.xyz * (card.b * 2.0 - 1.0));
    float backlit = saturate(dot(normalize(input.view.xyz), vec4Sun.xyz));
    backlit *= backlit * backlit * input.view.w * vec4Sun.w * saturate(up * 1.5);
    normal = normalize(lerp(normal, vec4Sun.xyz, backlit * 0.8));
    colour *= 1.0 + backlit * float3(0.45, 0.4, 0.08);

    output.colour = float4(saturate(colour), 1.0);
    output.normal = float4(normal * 0.5 + 0.5, 1.0);
    // A faint broad highlight, gloss 221 / 1023 (power 24) on all of it: the mark by which contact shadows (SSR.fx
    // fCSGrassGloss) leave the grass out, where swaying blades and the flower heads made them flicker
    output.specular = float4(input.specular * up, 221.0 / 1023.0, lerp(lerp(0.45, 0.25, dense), 1.0, up), 1.0);
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

technique Motion
{
    pass P0
    {
        VertexShader = compile vs_3_0 MotionVS();
        PixelShader = compile ps_3_0 MotionPS();
    }
}

// Under the grass (GroundTint, drawn before the blades and the ground's colour copy they take from): the ground's own
// colour, pale soil and gravel textures between the blades, is multiplied towards vec4GroundTint.rgb, so the blades
// stand in a dark, grassy floor of their own colour instead of on bare ground. Where: the grass's edge grid (EdgeTex,
// 0 off the grass, 1 well inside it) at the pixel's place on the ground, ground facing up, the material categories
// vec4TintAllow0/1 let through (WetGroundMaterials: not cars or people), within fTintRange of the camera.
sampler2D TintDepthTex : register(s3);     // the depth buffer the G-buffer pass drew into (log depth), unbound meanwhile
sampler2D TintNormalTex : register(s4);    // _DEFERRED_GBUFFER_1_
sampler2D TintMaterialTex : register(s5);  // _STENCIL_BUFFER_, the material IDs
sampler2D TintEdgeTex : register(s6);      // EdgeTex for the pixel shader, linear
float4 vec4ProjInfo;
float4 vec4WorldX;      // view to world rows, the camera's position in w
float4 vec4WorldY;
float4 vec4WorldZ;
float4 vec4GroundTint;  // the colour the ground is multiplied towards (rgb), how far at full cover (w)
float4 vec4TintAllow0;  // material categories 0..3 tinted (1) or not (0)
float4 vec4TintAllow1;  // and 4..7
float fTintRange;

float4 GroundTintPS(float2 uv : TEXCOORD0, float2 vpos : VPOS) : COLOR0
{
    float raw = tex2D(TintDepthTex, uv).r;
    [branch]
    if (raw <= 0.0 || raw >= 0.99999)
        return 1.0; // sky
    float id = floor(tex2D(TintMaterialTex, uv).r * 255.0 + 0.4);
    id -= id >= 128.0 ? 128.0 : 0.0;
    float category = fmod(id, 8.0);
    float allow = dot(vec4TintAllow0, float4(category == 0.0, category == 1.0, category == 2.0, category == 3.0)) +
                  dot(vec4TintAllow1, float4(category == 4.0, category == 5.0, category == 6.0, category == 7.0));
    float3 n = tex2D(TintNormalTex, uv).xyz * 2.0 - 1.0;
    float ground = smoothstep(0.6, 0.85, n.z * rsqrt(max(dot(n, n), 1e-4)));
    float z = pow(abs(fFarDivNear), raw) * fNearPlane;
    float3 v = float3((vpos + 0.5) * vec4ProjInfo.xy + vec4ProjInfo.zw, 1.0);
    float3 dir = float3(dot(v, vec4WorldX.xyz), dot(v, vec4WorldY.xyz), dot(v, vec4WorldZ.xyz));
    float3 P = float3(vec4WorldX.w, vec4WorldY.w, vec4WorldZ.w) + dir * z;
    float2 g = (P.xy - vec4EdgeGrid.xy) * vec4EdgeGrid.z;
    float cover = all(g == saturate(g)) ? tex2D(TintEdgeTex, g).r : 0.0;
    float fade = saturate((fTintRange - length(dir * z)) * 0.1);
    float k = vec4GroundTint.w * allow * ground * cover * fade;
    return float4(lerp(1.0, vec4GroundTint.rgb, k), 1.0);
}

technique GroundTint
{
    pass P0
    {
        PixelShader = compile ps_3_0 GroundTintPS();
    }
}
