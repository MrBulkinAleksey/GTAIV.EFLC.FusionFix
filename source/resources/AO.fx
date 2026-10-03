//  Modified Shader Model 3.0 port of SAO (Scalable Ambient Obscurance) (https://casual-effects.com/research/McGuire2012SAO/index.html)
//
//
//  Open Source under the "BSD" license: http://www.opensource.org/licenses/bsd-license.php
//
//  Copyright (c) 2011-2012, NVIDIA
//  All rights reserved.
//
//  Redistribution and use in source and binary forms, with or without modification, are permitted provided that the following conditions are met:
//
//  Redistributions of source code must retain the above copyright notice, this list of conditions and the following disclaimer.
//  Redistributions in binary form must reproduce the above copyright notice, this list of conditions and the following disclaimer in the documentation and/or other materials provided with the distribution.
//  THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

texture AOTexture2D, AOCamDepthTexture2D, DepthTex2D, NormalTex2D;
texture AOHistoryTex2D, PrevDepthTex2D, MotionTex2D, AlbedoTex2D;

sampler2D AOCamDepthTexture
{
    Texture = <AOCamDepthTexture2D>;
    AddressU = Clamp;
    AddressV = Clamp;
    MipFilter = Point;
    MinFilter = Point;
    MagFilter = Point;
};

sampler2D AOTexture
{
    Texture = <AOTexture2D>;
    AddressU = Clamp;
    AddressV = Clamp;
    MipFilter = Linear;
    MinFilter = Linear;
    MagFilter = Linear;
};

sampler2D DepthTex
{
    Texture = <DepthTex2D>;
    AddressU = Clamp;
    AddressV = Clamp;
    MipFilter = Point;
    MinFilter = Point;
    MagFilter = Point;
};

// _DEFERRED_GBUFFER_1_, the normals GTAO works with.
sampler2D NormalTex
{
    Texture = <NormalTex2D>;
    AddressU = Clamp;
    AddressV = Clamp;
    MipFilter = Point;
    MinFilter = Point;
    MagFilter = Point;
};

// Last frame's accumulated GTAO, see TemporalAO_PS.
sampler2D AOHistoryTex
{
    Texture = <AOHistoryTex2D>;
    AddressU = Clamp;
    AddressV = Clamp;
    MipFilter = None;
    MinFilter = Linear;
    MagFilter = Linear;
};

// The log depth the history was taken with (PreAlphaDepthCopy, last frame's).
sampler2D PrevDepthTex
{
    Texture = <PrevDepthTex2D>;
    AddressU = Clamp;
    AddressV = Clamp;
    MipFilter = None;
    MinFilter = Point;
    MagFilter = Point;
};

// Temporal AA's motion vectors, as in SSR.fx's TemporalHistoryUV.
sampler2D MotionTex
{
    Texture = <MotionTex2D>;
    AddressU = Clamp;
    AddressV = Clamp;
    MipFilter = None;
    MinFilter = Point;
    MagFilter = Point;
};

// _DEFERRED_GBUFFER_0_, the surfaces' colour, for the multiple bounce approximation.
sampler2D AlbedoTex
{
    Texture = <AlbedoTex2D>;
    AddressU = Clamp;
    AddressV = Clamp;
    MipFilter = None;
    MinFilter = Point;
    MagFilter = Point;
};

uniform float2 vec2InvViewportSize;
uniform float fNearPlane;
uniform float fFarPlane;
uniform float fFarDivNear;
uniform float fRadius;
uniform float fBias;
uniform float fIntensity;
uniform float fProjScale;
uniform float4 vec4ProjInfo;
uniform float2 vec2BlurDirection;
uniform float4 vec4WorldToView[3]; // world to reconstruction space rotation, for the G-buffer normals
uniform float fUseNormals;         // 1 when NormalTex holds the G-buffer normals, 0 rebuilds them from depth
uniform float fGTAOStrength;       // exponent on GTAO's visibility, 1 as computed
uniform float fThinOccluders;      // 0..1, how much sooner occluders far in front or behind fade out, see ComputeGTAO
uniform float2 vec2NoiseOffset;    // pixels, moves where the noise is read every frame while GTAO accumulates, 0 otherwise
uniform float fMultiBounce;        // 1 brightens occlusion on light surfaces, which bounce light into their own corners
// Accumulation over frames, see TemporalAO_PS.
uniform float4 vec4ViewToPrevClip[4];
uniform float fUseMotion;
uniform float2 vec2MotionJitter;
uniform float fUsePrevDepth;
uniform float fTemporalBlend;      // share of last frame's GTAO kept, 0 while there is none to keep

#ifndef NUM_SAMPLES
#define NUM_SAMPLES 9
#endif
#ifndef LOG_MAX_OFFSET
#define LOG_MAX_OFFSET 3
#endif
#ifndef MAX_MIP_LEVEL
#define MAX_MIP_LEVEL 5
#endif
#ifndef FAR_CLIP
#define FAR_CLIP 150.0f
#endif
#ifndef NUM_SPIRAL_TURNS
#define NUM_SPIRAL_TURNS 7
#endif
#ifndef GTAO_SLICES
#define GTAO_SLICES 3
#endif
#ifndef GTAO_STEPS
#define GTAO_STEPS 4
#endif
static const float PI = 3.14159265;

float LogDepthToViewZ(float logDepth)
{
    return pow(fFarDivNear, logDepth) * fNearPlane;
}

// Reconstruct view-space position from depth
float3 ReconstructViewPos(float2 S, float z)
{
    return float3(((S.xy + 0.5f) * vec4ProjInfo.xy + vec4ProjInfo.zw) * z, z);
}

float3 ReconstructNormal(float3 pos)
{
    return normalize(cross(ddy(pos), ddx(pos)));
}

float GetRandomRotationAngle(float2 vPos)
{
    return 2 * PI * frac(52.9829189 * frac(dot(vPos, float2(0.06711056, 0.00583715))));
}

float3 getOffsetPosition(float2 ssC, float2 unitOffset, float ssR)
{
    float2 ssP = ssC + ssR * unitOffset;

    int mipLevel = clamp((int) floor(log2(ssR)) - LOG_MAX_OFFSET, 0, MAX_MIP_LEVEL);

    // --- Sample depth ---
    float z = tex2Dlod(
        AOCamDepthTexture,
        float4(ssP * vec2InvViewportSize, 0.0, (float) mipLevel)
    ).r;

    // --- Reconstruct view-space position ---
    return ReconstructViewPos(ssP, z);
}

float2 tapLocation(int sampleNumber, float spinAngle, out float ssR)
{
	// Radius relative to ssR
    float alpha = float(sampleNumber + 0.5) * (1.0 / float(NUM_SAMPLES));
    float angle = alpha * (NUM_SPIRAL_TURNS * 2.0 * PI) + spinAngle;

    ssR = alpha;
    return float2(cos(angle), sin(angle));
}

float sampleAO(in float2 ssC, in float3 C, in float3 n_C, in float ssDiskRadius, in int tapIndex, in float randomPatternRotationAngle)
{
	// Offset on the unit disk, spun for this pixel
    float ssR;
    float2 unitOffset = tapLocation(tapIndex, randomPatternRotationAngle, ssR);
    ssR *= ssDiskRadius;

	// The occluding point in camera space
    float3 Q = getOffsetPosition(ssC, unitOffset, ssR);

    float3 v = Q - C;

    float vv = dot(v, v);
    float vn = dot(v, n_C);

    const float epsilon = 0.01;
    float f = max((fRadius * fRadius) - vv, 0.0);
    return f * f * f * max((vn - fBias) / (epsilon + vv), 0.0);
}

static const float BilateralDepthTreshold = 0.03;

float2 BilateralBlur(sampler2D Texture, sampler2D CamDepthTexture, in float2 uv)
{ // thanks to Parallellines0451
    static const float GaussianWeights[5] =
    {
	0.204163688715,
	0.180173822911,
	0.123831536806,
	0.0662822452864,
	0.0276305506389
    };

    float2 refTap;
    refTap.x = tex2D(Texture, uv).x;
    refTap.y = tex2Dlod(CamDepthTexture, float4(uv, 0, 0)).x;
    
    const float blurThreshold = BilateralDepthTreshold * refTap.y;

    float blurredValue = refTap.x * GaussianWeights[0];
    
    float2 texel = vec2BlurDirection;

	[unroll]
    for (int i = -4; i <= 4; ++i)
    {
        if (i == 0)
            continue;
        float2 offsetUV = uv + (i * texel);
        float2 tap;
        tap.x = tex2D(Texture, offsetUV).x;
        tap.y = tex2D(CamDepthTexture, offsetUV).x;
        float weight = GaussianWeights[abs(i)];

        blurredValue += weight * ((abs(refTap.y - tap.y) < blurThreshold) ? tap.x : refTap.x);
    }

    return float2(blurredValue, refTap.y);
}

// _DEFERRED_GBUFFER_1_ decoded the way deferred_lighting decodes it, turned into
// reconstruction space (as GBufferNormal in SSR.fx).
float3 GBufferViewNormal(float2 uv)
{
    float4 g = tex2Dlod(NormalTex, float4(uv, 0, 0));
    float3 f = frac(g.w * float3(0.998046875, 7.984375, 63.875));
    f.xy -= f.yz * 0.125;
    float3 n = normalize(g.xyz * 256.0 + f - 127.999992);
    return normalize(float3(dot(vec4WorldToView[0].xyz, n),
                            dot(vec4WorldToView[1].xyz, n),
                            dot(vec4WorldToView[2].xyz, n)));
}

float FastAcos(float x)
{
    float a = abs(x);
    float r = (-0.156583 * a + 1.570796) * sqrt(1.0 - a);
    return x >= 0.0 ? r : PI - r;
}

// Ground truth ambient occlusion (Jimenez et al. 2016, after Intel's XeGTAO): in a few slices
// through the view direction, the highest horizon on either side is found by stepping across
// the screen, and the cosine weighted share of the hemisphere above the normal that lies
// between the two horizons is integrated exactly. Unlike SAO's sum of point obscurances it
// gives the visible fraction of the sky, so a corner, a gap under a car or the ground along a
// wall darkens as much as it is closed in, and open ground keeps its full light. Occluders
// fade out over the outer part of fRadius, so a building behind a ped does not darken him.
float ComputeGTAO(float2 ssC, float2 uv, float3 C, float3 n)
{
    float3 viewV = normalize(-C);
    float radiusPx = min(abs(fProjScale) * fRadius / C.z, 0.25 / vec2InvViewportSize.y);
    if (radiusPx < 1.0)
        return 1.0;

    float falloffRange = 0.615 * fRadius;
    float falloffMul = -1.0 / falloffRange;
    float falloffAdd = (fRadius - falloffRange) / falloffRange + 1.0;

    // Interleaved gradient noise: the slices turn and the steps shift from pixel to pixel,
    // which the blur afterwards evens out.
    // While accumulating, vec2NoiseOffset reads the noise elsewhere every frame, so the slices
    // turn on and the accumulation averages many of them.
    float2 noisePos = ssC + vec2NoiseOffset;
    float noiseSlice = frac(52.9829189 * frac(dot(noisePos, float2(0.06711056, 0.00583715))));
    float noiseStep = frac(52.9829189 * frac(dot(noisePos.yx + float2(5.0, 13.0), float2(0.06711056, 0.00583715))));

    // Real loops, not unrolled, to stay within ps_3_0's 512 instruction slots.
    float visibility = 0.0;
    [loop]
    for (int slice = 0; slice < GTAO_SLICES; ++slice)
    {
        float phi = ((float) slice + noiseSlice) * (PI / (float) GTAO_SLICES);
        float2 omega = float2(cos(phi), sin(phi));

        // The slice's direction in reconstruction space: where a step along omega on the
        // screen leads at this depth.
        float3 dirV = ReconstructViewPos(ssC + omega, C.z) - C;
        float3 orthoDir = normalize(dirV - dot(dirV, viewV) * viewV);
        float3 axis = normalize(cross(orthoDir, viewV));
        float3 projN = n - axis * dot(n, axis);
        float projLen = length(projN);
        float cosN = saturate(dot(projN, viewV) / max(projLen, 1e-4));
        float angN = (dot(orthoDir, projN) >= 0.0 ? 1.0 : -1.0) * FastAcos(cosN);

        // Horizons start at the tangent plane on each side.
        float lowCos0 = cos(angN + PI * 0.5);
        float lowCos1 = cos(angN - PI * 0.5);
        float horizonCos0 = lowCos0;
        float horizonCos1 = lowCos1;

        [loop]
        for (int step = 0; step < GTAO_STEPS; ++step)
        {
            // Denser next to the pixel, where small creases are.
            float t = ((float) step + noiseStep) / (float) GTAO_STEPS;
            t *= t;
            float offsetPx = max(t * radiusPx, (float) step + 1.0);
            float2 offset = omega * offsetPx;

            float3 delta0 = getOffsetPosition(ssC, omega, offsetPx) - C;
            float3 delta1 = getOffsetPosition(ssC, -omega, offsetPx) - C;
            float len0 = length(delta0);
            float len1 = length(delta1);
            // A thin object, a pole or a railing, raises the horizon as if a wall stood
            // there, and the ground around it darkened in its shape. Stretching the depth part
            // of the distance (XeGTAO's thin occluder compensation) lets samples far in front
            // or behind fade out sooner.
            float3 thin = float3(1.0, 1.0, 1.0 + fThinOccluders);
            float fall0 = saturate(length(delta0 * thin) * falloffMul + falloffAdd);
            float fall1 = saturate(length(delta1 * thin) * falloffMul + falloffAdd);
            float sampleCos0 = lerp(lowCos0, dot(delta0, viewV) / max(len0, 1e-4), fall0);
            float sampleCos1 = lerp(lowCos1, dot(delta1, viewV) / max(len1, 1e-4), fall1);
            horizonCos0 = max(horizonCos0, sampleCos0);
            horizonCos1 = max(horizonCos1, sampleCos1);
        }

        float h0 = -FastAcos(horizonCos1);
        float h1 = FastAcos(horizonCos0);
        h0 = angN + clamp(h0 - angN, -PI * 0.5, PI * 0.5);
        h1 = angN + clamp(h1 - angN, -PI * 0.5, PI * 0.5);
        float sinN = sin(angN);
        float arc0 = (cosN + 2.0 * h0 * sinN - cos(2.0 * h0 - angN)) * 0.25;
        float arc1 = (cosN + 2.0 * h1 * sinN - cos(2.0 * h1 - angN)) * 0.25;
        visibility += projLen * (arc0 + arc1);
    }
    visibility /= (float) GTAO_SLICES;
    return pow(saturate(visibility), fGTAOStrength);
}

// SAO, or GTAO with gtao: two shaders, as together they took more than the 512 instruction
// slots ps_3_0 promises, and where the device would not create the shader the whole
// post-processing stayed off and the screen went black.
float4 ComputeAO(float2 uv, float2 vPos, uniform bool gtao)
{
    float depth = tex2D(AOCamDepthTexture, uv).r;

    clip(FAR_CLIP - depth);
    
	// World space point being shaded
        
    float2 ssC = vPos;
    float3 C = ReconstructViewPos(ssC, depth);

    float randomPatternRotationAngle = GetRandomRotationAngle(ssC);

	// Reconstruct normals from positions. These will lead to 1-pixel black lines
	// at depth discontinuities, however the blur will wipe those out so they are not visible
	// in the final image.
    float3 n_C = ReconstructNormal(C);

    float A;
    if (gtao)
    {
        float3 n = n_C;
        if (fUseNormals > 0.0)
            n = GBufferViewNormal(uv);
        n = (dot(n, C) > 0.0) ? -n : n;
        A = ComputeGTAO(ssC, uv, C, n);
    }
    else
    {
        // Choose the screen-space sample radius
        // proportional to the projected area of the sphere
        float ssDiskRadius = -fProjScale * fRadius / C.z;

        float sum = 0.0;
        [unroll]
        for (int i = 0; i < NUM_SAMPLES; ++i)
        {
            sum += sampleAO(ssC, C, n_C, ssDiskRadius, i, randomPatternRotationAngle);
        }

        float temp = fRadius * fRadius * fRadius;
        sum /= temp * temp;
        A = max(0.0, 1.0 - sum * fIntensity * (5.0 / NUM_SAMPLES));
    }

	// Bilateral box-filter over a quad for free, respecting depth edges
	// (the difference that this makes is subtle)
    
    int2 pixel = ssC;
    float depthThreshold = BilateralDepthTreshold * C.z;
    
    if (abs(ddx(C.z)) < depthThreshold)
    {
        A -= ddx(A) * (float(pixel.x % 2) - 0.5);
    }
    if (abs(ddy(C.z)) < depthThreshold)
    {
        A -= ddy(A) * (float(pixel.y % 2) - 0.5);
    }
    
    static const float nearFade = FAR_CLIP - FAR_CLIP * 0.95f;  
    static const float farFade = FAR_CLIP;
    
    float t = saturate((C.z - nearFade) / (farFade - nearFade));
    A = lerp(A, 1.0f, t);
    
    return float4(A, A, A, 1.0);
}

float4 ComputeAO_PS(float2 uv : TEXCOORD0, float2 vPos : VPOS) : COLOR0
{
    return ComputeAO(uv, vPos, false);
}

float4 ComputeGTAO_PS(float2 uv : TEXCOORD0, float2 vPos : VPOS) : COLOR0
{
    return ComputeAO(uv, vPos, true);
}

// Accumulation of GTAO over frames: the slices turn every frame (vec2NoiseOffset), and last
// frame's result, followed to where this pixel was (the camera, or temporal AA's motion
// vectors for what moved itself), is blended in, so a few slices a frame add up to many.
// The history is clamped to the neighbourhood of this frame's values, and dropped where last
// frame's depth there shows another surface, which a moving car uncovers.
float4 TemporalAO_PS(float2 uv : TEXCOORD0, float2 vPos : VPOS) : COLOR0
{
    float current = tex2Dlod(AOTexture, float4(uv, 0, 0)).r;
    if (fTemporalBlend <= 0.0)
        return float4(current, 0, 0, 1);

    float m1 = 0.0, m2 = 0.0;
    [unroll]
    for (int y = -1; y <= 1; ++y)
    {
        [unroll]
        for (int x = -1; x <= 1; ++x)
        {
            float v = tex2Dlod(AOTexture, float4(uv + float2(x, y) * vec2InvViewportSize, 0, 0)).r;
            m1 += v;
            m2 += v * v;
        }
    }
    m1 /= 9.0;
    float spread = sqrt(max(m2 / 9.0 - m1 * m1, 0.0));
    float lo = m1 - 1.5 * spread - 0.02, hi = m1 + 1.5 * spread + 0.02;

    float3 C = ReconstructViewPos(vPos, tex2Dlod(AOCamDepthTexture, float4(uv, 0, 0)).r);
    float4 clip = C.x * vec4ViewToPrevClip[0] + C.y * vec4ViewToPrevClip[1] + C.z * vec4ViewToPrevClip[2] + vec4ViewToPrevClip[3];
    if (clip.w <= 0.0)
        return float4(current, 0, 0, 1);
    float2 prevUV = (clip.xy / clip.w) * float2(0.5, -0.5) + 0.5;
    bool checkDepth = true;
    if (fUseMotion > 0.0)
    {
        float2 moved = uv + tex2Dlod(MotionTex, float4(uv, 0, 0)).xy + vec2MotionJitter;
        if (any(abs(moved - prevUV) > 1.5 * vec2InvViewportSize))
            checkDepth = false;
        prevUV = moved;
    }

    float keep = fTemporalBlend;
    if (any(prevUV <= 0.0) || any(prevUV >= 1.0))
        keep = 0.0;
    else if (fUsePrevDepth > 0.0 && checkDepth)
    {
        float prevZ = pow(fFarDivNear, tex2Dlod(PrevDepthTex, float4(prevUV, 0, 0)).r) * fNearPlane;
        if (abs(prevZ - clip.w) > 0.05 * clip.w + 0.1)
            keep = 0.0;
    }

    float history = clamp(tex2Dlod(AOHistoryTex, float4(prevUV, 0, 0)).r, lo, hi);
    return float4(lerp(current, history, keep), 0, 0, 1);
}

// Light surfaces bounce light into their own corners, so they darken less there than dark
// ones (Jimenez et al. 2016's fit of multiple bounces from the occlusion and the colour).
float MultiBounce(float ao, float albedo)
{
    float a = 2.0404 * albedo - 0.3324;
    float b = -4.7951 * albedo + 0.6417;
    float c = 2.7552 * albedo + 0.6903;
    return max(ao, ((ao * a + b) * ao + c) * ao);
}

float4 BlurAOToBuffer_PS(float2 uv : TEXCOORD0) : COLOR0
{
    float2 blur = BilateralBlur(AOTexture, AOCamDepthTexture, uv);
    return float4(blur.x, blur.x, blur.x, 1);
}

float4 OutputAO_PS(float2 uv : TEXCOORD0) : COLOR0
{
    float ao = tex2D(AOTexture, uv).r;
    [branch]
    if (fMultiBounce > 0.0)
        ao = MultiBounce(ao, dot(tex2Dlod(AlbedoTex, float4(uv, 0, 0)).rgb, float3(0.2126, 0.7152, 0.0722)));
    return float4(0, 0, 0, 1.0 - ao);
}

float4 ComputeCamDepth_PS(float2 uv : TEXCOORD0) : COLOR0
{
    float viewZ = LogDepthToViewZ(tex2D(DepthTex, uv).r);
    return float4(viewZ, 0.0, 0.0, 0.0);
}

float2 vec2PrevMipSize;
float2 vec2PrevMipTexel;
int iPreviousMip;

float4 CamDepthMinify_PS(float2 fragCoord : VPOS) : COLOR0
{
    int2 ssP = int2(fragCoord.xy + 0.5);

    int2 offset = int2(ssP.y % 2, ssP.x % 2);
    int2 coord = ssP * 2 + offset;
    coord = clamp(coord, int2(0, 0), int2(vec2PrevMipSize) - 1);

    float2 uv = coord * vec2PrevMipTexel;

    return tex2Dlod(AOCamDepthTexture, float4(uv, 0, iPreviousMip));
}

void FullscreenQuadVS(in float4 iPos : POSITION, in float2 iUV : TEXCOORD0,
                      out float4 oPos : POSITION, out float2 oUV : TEXCOORD0)
{
    oPos = iPos;
    oUV = iUV;
}


technique AmbientOcclusion
{
    pass ComputeCamDepth
    {
        PixelShader = compile ps_3_0 ComputeCamDepth_PS();
        VertexShader = compile vs_3_0 FullscreenQuadVS();
        AlphaBlendEnable = FALSE;
        AlphaTestEnable = FALSE;
        ZEnable = 0;
        ZWriteEnable = FALSE;
        StencilEnable = FALSE;
    }
    pass CamDepthMinify
    {
        PixelShader = compile ps_3_0 CamDepthMinify_PS();
        VertexShader = compile vs_3_0 FullscreenQuadVS();
        AlphaBlendEnable = FALSE;
        AlphaTestEnable = FALSE;
        ZEnable = 0;
        ZWriteEnable = FALSE;
        StencilEnable = FALSE;
    }
    pass ComputeAO
    {
        PixelShader = compile ps_3_0 ComputeAO_PS();
        VertexShader = compile vs_3_0 FullscreenQuadVS();
        AlphaBlendEnable = FALSE;
        AlphaTestEnable = FALSE;
        ZEnable = 0;
        ZWriteEnable = FALSE;
        StencilEnable = FALSE;
        CullMode = NONE;
        FogEnable = FALSE;
        Clipping = FALSE;
    }
    pass BlurAOToBuffer
    {
        PixelShader = compile ps_3_0 BlurAOToBuffer_PS();
        VertexShader = compile vs_3_0 FullscreenQuadVS();
        AlphaBlendEnable = FALSE;
        AlphaTestEnable = FALSE;
        ZEnable = 0;
        ZWriteEnable = FALSE;
        StencilEnable = FALSE;
        CullMode = NONE;
        FogEnable = FALSE;
        Clipping = FALSE;
    }
    pass OutputAO
    {
        PixelShader = compile ps_3_0 OutputAO_PS();
        VertexShader = compile vs_3_0 FullscreenQuadVS();
        AlphaBlendEnable = TRUE;
        SrcBlend = SRCALPHA;
        DestBlend = INVSRCALPHA;
        AlphaTestEnable = FALSE;
        ZEnable = 0;
        ZWriteEnable = FALSE;
        StencilEnable = FALSE;
        CullMode = NONE;
        FogEnable = FALSE;
        Clipping = FALSE;   
        ColorWriteEnable = BLUE; // output ao
    }
    pass ComputeGTAO // in place of ComputeAO, see ComputeAO
    {
        PixelShader = compile ps_3_0 ComputeGTAO_PS();
        VertexShader = compile vs_3_0 FullscreenQuadVS();
        AlphaBlendEnable = FALSE;
        AlphaTestEnable = FALSE;
        ZEnable = 0;
        ZWriteEnable = FALSE;
        StencilEnable = FALSE;
        CullMode = NONE;
        FogEnable = FALSE;
        Clipping = FALSE;
    }
    pass TemporalAO // after ComputeGTAO, into the accumulation
    {
        PixelShader = compile ps_3_0 TemporalAO_PS();
        VertexShader = compile vs_3_0 FullscreenQuadVS();
        AlphaBlendEnable = FALSE;
        AlphaTestEnable = FALSE;
        ZEnable = 0;
        ZWriteEnable = FALSE;
        StencilEnable = FALSE;
        CullMode = NONE;
        FogEnable = FALSE;
        Clipping = FALSE;
    }
}