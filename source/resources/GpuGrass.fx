// GPU grass (gpugrass.ixx): blades of grass grown on the ground triangles the procedural manager holds, drawn into
// the G-buffer at the end of its pass, so the game's lighting, shadows and fog take them as any other surface.
//
// One mesh of kBlades blades (stream 0) is drawn once per piece of ground (stream 1, instanced): a piece is a
// triangle of at most a metre and a half square, its corner and two edges; each blade stands at its own barycentric
// place on it, turned and sized by its own random values, and a piece keeps only the first `share` of its blades
// (by area, the surface's grass and the distance); the others go to one point and draw nothing.
//
// G-buffer, as gta_default's deferred pixel shader writes it: colour, normal * 0.5 + 0.5, (no specular, the gloss of
// a material with no specular map, the occlusion), and the log depth of FusionShaders (log2(w / near) / log2(far /
// near)) to the depth buffer. The material ID target is left as the ground had it.

float4x4 matViewProj;
float4 vec4Wind;        // the wind's direction over the gust's wavelength (xy), the gust's phase, the flutter's phase (cycles)
float4 vec4WindDir;     // the wind's unit direction (xy), bend and flutter at the tip per metre of height
float4 vec4Blade;       // shortest height, extra height, width at the root, the debug colours (1 = by surface)
float4 vec4ColourRoot;  // at the root (rgb), the occlusion there (a)
float4 vec4ColourTip;   // at the tip (rgb), how much the colour varies between blades (a)
float fNearPlane;
float fFarDivNear;

struct VSInput
{
    float4 blade : TEXCOORD0;   // barycentric u, v, its turn (radians), its height (0..1)
    float4 vertex : TEXCOORD1;  // up the blade (0..1), across it (-1..1), the blade's place among them (0..1), random
    float4 corner : TEXCOORD2;  // the piece's corner, a random value for its turn
    float4 edge1 : TEXCOORD3;   // its first edge, the share of its blades kept
    float4 edge2 : TEXCOORD4;   // its second edge, its surface's tag / 255 (debug colours)
};

struct VSOutput
{
    float4 position : POSITION;
    float4 colour : TEXCOORD0;  // albedo, occlusion
    float3 normal : TEXCOORD1;
    float w : TEXCOORD2;
};

float3 TagColour(float tag)
{
    float3 c = frac(tag * float3(17.13, 31.71, 53.37) * 255.0 / 97.0);
    return 0.15 + 0.7 * c;
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
        return output;
    }

    float turn = input.blade.z + input.corner.w * 6.2831853;
    float s, c;
    sincos(turn, s, c);
    float3 across = float3(c, s, 0.0);
    float3 facing = float3(-s, c, 0.0);

    float height = vec4Blade.x + vec4Blade.y * input.blade.w;
    float t = input.vertex.x;
    float t2 = t * t;
    float random = input.vertex.w;

    // The wind as the props have it: a gust travelling downwind and a flutter across it, both by position
    float gust = frac(dot(root.xy, vec4Wind.xy) + vec4Wind.z) * 6.2831853 - 3.1415927;
    float flutter = frac((root.x + root.y) * 1.3 + vec4Wind.w + random) * 6.2831853 - 3.1415927;
    float bend = (sin(gust) * 0.55 + 0.6) * vec4WindDir.z * height;
    float side = sin(flutter) * vec4WindDir.w * height;
    float2 offset = vec4WindDir.xy * bend + float2(-vec4WindDir.y, vec4WindDir.x) * side;
    // Each blade's own lean, so the grass doesn't stand straight
    offset += facing.xy * (height * (random - 0.35) * 0.45);
    offset *= t2;
    float drop = dot(offset, offset) / (2.0 * height);

    float3 position = root + across * (input.vertex.y * vec4Blade.z * 0.5 * (1.0 - t)) + float3(offset, t * height - drop);

    output.position = mul(float4(position, 1.0), matViewProj);
    output.w = output.position.w;
    // Lit mostly from above, as grass seen as a lawn: the blade's own facing only tilts it
    output.normal = normalize(facing * 0.35 + float3(0.0, 0.0, 1.0));

    float3 colour = lerp(vec4ColourRoot.rgb, vec4ColourTip.rgb, t);
    colour *= 1.0 + (random - 0.5) * vec4ColourTip.a;
    colour = vec4Blade.w > 0.0 ? TagColour(input.edge2.w) * lerp(0.6, 1.0, t) : colour;
    output.colour = float4(saturate(colour), lerp(vec4ColourRoot.a, 1.0, t));
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
    output.colour = float4(input.colour.rgb, 1.0);
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
