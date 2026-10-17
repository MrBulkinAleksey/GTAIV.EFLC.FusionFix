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
// (*proc_plant_*). Over the ground the grass grows in patches, taller and shorter, greener and drier, and its colour is
// half the ground's under its root (GroundTex, a copy of the G-buffer's colour before the grass), so it takes the hue
// of the lawn, dirt or verge it grows on.
//
// G-buffer, as gta_default's deferred pixel shader writes it: colour, normal * 0.5 + 0.5, (no specular, the gloss of
// a material with no specular map, the occlusion), and the log depth of FusionShaders (log2(w / near) / log2(far /
// near)) to the depth buffer. The material ID target is left as the ground had it.

float4x4 matViewProj;
float4 vec4Wind;        // the wind's direction over the gust's wavelength (xy), the gust's phase, the flutter's phase (cycles)
float4 vec4WindDir;     // the wind's unit direction (xy), bend and flutter at the tip per metre of height
float4 vec4Blade;       // height scale, width scale, the ground's share in the colour, the debug colours (1 = by surface)
float4 vec4Eye;         // the camera's position
float fNearPlane;
float fFarDivNear;

sampler2D GroundTex : register(s0);    // a copy of _DEFERRED_GBUFFER_0_ before the grass

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
    float w : TEXCOORD2;
    float4 ground : TEXCOORD3;  // the root's clip x, y, w; light from root to tip
};

float3 TagColour(float tag)
{
    float3 c = frac(tag * float3(17.13, 31.71, 53.37) * 255.0 / 97.0);
    return 0.15 + 0.7 * c;
}

float Hash(float2 p)
{
    return frac(sin(dot(p, float2(12.9898, 78.233))) * 43758.5453);
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
        output.w = 1.0;
        output.ground = 0.0;
        return output;
    }

    float random = input.vertex.w;
    float t = input.vertex.x;

    // The blade's kind: grass, weed or small plant
    float pick = frac(random * 13.713 + input.corner.w * 7.31 + input.blade.w * 3.17);
    float isWeed = pick >= input.kinds.x && pick < input.kinds.x + input.kinds.y ? 1.0 : 0.0;
    float isPlant = pick >= input.kinds.x + input.kinds.y ? 1.0 : 0.0;
    float isGrass = 1.0 - isWeed - isPlant;

    // Patches over the ground: some taller, some shorter, clumps within them, some drier
    float patch = Noise(root.xy * 0.13);
    float clump = Noise(root.xy * 0.9 + 31.7);
    float dry = smoothstep(0.5, 0.85, Noise(root.xy * 0.06 + 17.3));

    float h = input.blade.w;
    float height = isGrass * (0.14 + 0.28 * h * h) + isWeed * (0.28 + 0.45 * h) + isPlant * (0.10 + 0.16 * h);
    height *= lerp(0.6, 1.3, patch) * lerp(0.75, 1.15, clump) * lerp(0.55, 1.0, input.kinds.w) * vec4Blade.x;
    float width = (isGrass * 0.028 + isWeed * 0.04 + isPlant * 0.075) * vec4Blade.y;
    // Wider far off, where fewer of them stand and thin ones only flicker
    float distance = length(root - vec4Eye.xyz);
    width *= 1.0 + saturate((distance - 6.0) / 24.0) * 1.5;
    // Grass and weeds narrow to the tip; leaves are widest in the middle
    float profile = lerp(1.0 - t, sin(t * 3.1415927) * 1.1 + 0.2 * (1.0 - t), isPlant);
    // How far the blade leans out from its root: leaves spread, grass and weeds stand
    float lean = isGrass * 0.45 + isWeed * 0.3 + isPlant * 1.1;

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
    offset *= t * t;
    // Never longer than the blade; the tip drops as it bends
    float reach = length(offset);
    offset *= min(1.0, 0.9 * height / max(reach, 1e-4));
    float drop = dot(offset, offset) / (2.0 * height);

    float3 position = root + across * (input.vertex.y * width * 0.5 * profile) + float3(offset, t * height - drop);

    output.position = mul(float4(position, 1.0), matViewProj);
    output.w = output.position.w;
    // Lit mostly from above, as grass seen as a lawn; leaves lie flatter and face up more
    output.normal = normalize(facing * lerp(0.35, 0.2, isPlant) + float3(0.0, 0.0, 1.0));

    // Its own colour: by kind, drier in dry patches (weeds most), each blade a little different
    float3 colour = isGrass * float3(0.20, 0.27, 0.08) + isWeed * float3(0.27, 0.28, 0.11) + isPlant * float3(0.13, 0.24, 0.07);
    colour = lerp(colour, float3(0.40, 0.35, 0.18), dry * lerp(0.55, 0.85, isWeed) * (1.0 - isPlant * 0.6));
    colour *= 1.0 + (random - 0.5) * 0.4;
    colour = vec4Blade.w > 0.0 ? TagColour(input.edge2.w) : colour;
    output.colour = float4(saturate(colour), lerp(0.45, 1.0, t));

    float4 rootClip = mul(float4(root, 1.0), matViewProj);
    output.ground = float4(rootClip.xy, rootClip.w, lerp(0.55, 1.1, t) * lerp(1.0, 0.85, clump * isGrass));
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
    // The ground's colour under the root, kept no brighter than a lawn so pavement doesn't bleach the weeds on it
    float2 screen = input.ground.xy / max(input.ground.z, 1e-3) * float2(0.5, -0.5) + 0.5;
    float3 ground = tex2Dlod(GroundTex, float4(screen, 0.0, 0.0)).rgb;
    float luminance = dot(ground, float3(0.299, 0.587, 0.114));
    ground *= min(1.0, 0.3 / max(luminance, 0.01)) * 1.15;
    bool onScreen = input.ground.z > 0.05 && all(screen == saturate(screen));
    float share = onScreen && vec4Blade.w <= 0.0 ? vec4Blade.z : 0.0;
    float3 colour = lerp(input.colour.rgb, ground, share) * (vec4Blade.w > 0.0 ? lerp(0.6, 1.0, input.ground.w / 1.1) : input.ground.w);

    output.colour = float4(saturate(colour), 1.0);
    output.normal = float4(normalize(input.normal) * 0.5 + 0.5, 1.0);
    // No specular; gloss 258 / 1023 marks a material without a specular map (world_no_specular_mark.patch)
    output.specular = float4(0.0, 0.252199411, input.colour.a, 1.0);
    output.depth = log2(max(input.w / fNearPlane, 1.0)) / log2(fFarDivNear);
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
