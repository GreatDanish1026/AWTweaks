// Reconstruction of Alan Wake Remastered's HBAO.rfx "coarseAOPS": NVIDIA HBAO+'s
// horizon-based ambient occlusion for one quarter-resolution, deinterleaved layer.
// Rebuilt from the game's DXIL; NUM_DIRECTIONS 4, 6 or 8 gives the three shipped
// variants (4 steps each). ENHANCED 1 adds GTAO (ground-truth ambient occlusion), switched
// on by AWTweaks through the "HBAO:Bias" tweakable (see DecodeSettings); without that it
// runs exactly the rebuilt code. DEBUG_STRIPES 1 adds a test pattern. Bindings, constant
// layout and signatures match the original, so it drops into the game's pipeline unchanged.
//
// Output: x = AO (1 = unoccluded), y = view depth (for the blur), z = w = 0;
//         1 everywhere when the radius is under a pixel.

cbuffer Globals : register(b0)
{
    row_major float4x4 g_mWorldToView;          // 0
    row_major float4x4 g_mViewToWorld;          // 64
    row_major float4x4 g_mViewToClip;           // 128
    row_major float4x4 g_mClipToView;           // 192
    row_major float4x4 g_mWorldToClip;          // 256
    row_major float4x4 g_mClipToWorld;          // 320
    float4 g_vViewPoint;                        // 384
    float g_fInvNear;                           // 400
    float g_fFar;                               // 404
    float2 g_vScreenRes;                        // 408
    float2 g_vInvScreenRes;                     // 416
    float2 g_vRenderRes;                        // 424
    float2 g_vInvRenderRes;                     // 432
    bool g_bHDR;                                // 440
    float g_fHDRPaperWhite;                     // 444
    float g_fHDRPeakWhite;                      // 448
    float g_fSimulationTime;                    // 452
    float2 g_vExposureXB;                       // 456
    float2 g_vShadowBufferReconstructionSlopeBias; // 464
    float2 g_vShadowMapRes;                     // 472
    float2 g_vJitterOffset;                     // 480
    float g_fPCFBias_d3t;                       // 488
    float g_fNumPCFSamples_d3t;                 // 492
    float g_fCascadePCFSize_d3t;                // 496
    float g_fPCFSize_d3t;                       // 500
    float4 UVToViewAB;                          // 512: view xy = (A * uv + B) * depth
    float2 PixelSize;                           // 528
    float4 PixelSizeDepthThresholdNegInvSharpness; // 544: xy quarter-res texel, z/w depth fade
    float4 NegInvR2RadToScreenBackgrdForegrd;   // 560: -1/R^2, radius to screen, radius clamps
    float4 BiasSmallScaleLargeScale;            // 576: N.V bias, small/large scale amounts
    float4 PixelSizePowerExp;                   // 592
    float4 BlurDepthSharpness;                  // 608
    float2 PerPassOffset;                       // 624: this layer's offset in the 4x4 pattern
    float4 PerPassJitter;                       // 640: xy rotation (cos, sin), z step jitter
};

SamplerState g_sReconstructedNormals : register(s0);
SamplerState g_sQuarterRezViewDepth : register(s1);
Texture2D<float4> g_sReconstructedNormalsTex : register(t0);
Texture2D<float4> g_sQuarterRezViewDepthTex : register(t1);

#ifndef NUM_DIRECTIONS
#define NUM_DIRECTIONS 4
#endif
#ifndef DEBUG_STRIPES
#define DEBUG_STRIPES 0
#endif
#ifndef ENHANCED
#define ENHANCED 0
#endif
#define NUM_STEPS 4

static const float kPi = 3.14159265;

float3 FetchQuarterResViewPos(float2 uv)
{
    float z = g_sQuarterRezViewDepthTex.Sample(g_sQuarterRezViewDepth, uv).x;
    return float3((UVToViewAB.xy * uv + UVToViewAB.zw) * z, z);
}

float2 RotateDirection(float2 v, float2 cosSin)
{
    return float2(v.x * cosSin.x - v.y * cosSin.y, v.x * cosSin.y + v.y * cosSin.x);
}

float ComputeAO(float3 p, float3 n, float3 s, float negInvR2)
{
    float3 v = s - p;
    float vdotv = dot(v, v);
    float ndotv = dot(n, v) * rsqrt(vdotv);
    return saturate(vdotv * negInvR2 + 1.0) * saturate(ndotv - BiasSmallScaleLargeScale.x);
}

struct RadiusParams
{
    float radiusPixels;
    float negInvR2;
};

void ScaleRadius(inout RadiusParams r, float scale)
{
    r.radiusPixels *= scale;
    r.negInvR2 *= 1.0 / (scale * scale);
}

RadiusParams GetAORadiusParams(float viewDepth)
{
    RadiusParams r;
    r.radiusPixels = NegInvR2RadToScreenBackgrdForegrd.y / viewDepth;
    r.negInvR2 = NegInvR2RadToScreenBackgrdForegrd.x;
    if (NegInvR2RadToScreenBackgrdForegrd.z != -1.0)
        ScaleRadius(r, max(1.0, NegInvR2RadToScreenBackgrdForegrd.z / r.radiusPixels));
    if (NegInvR2RadToScreenBackgrdForegrd.w != -1.0)
        ScaleRadius(r, min(1.0, NegInvR2RadToScreenBackgrdForegrd.w / r.radiusPixels));
    return r;
}

// ---------------------------------------------------------------- settings channel
// AWTweaks rounds "HBAO:Bias" (the N.V bias, BiasSmallScaleLargeScale.x, read only by this
// pass) to 0.001 and adds code * 0.000001, code 1..30 = GTAO at strength code / 10. No
// code - the game's own value, or the mod toggled off with F10 - means stock.
float GtaoStrength()
{
    float v = BiasSmallScaleLargeScale.x;
    float rounded = floor(v * 1000.0 + 0.05) * 0.001;
    int code = (int)round((v - rounded) * 1000000.0);
    return code >= 1 && code <= 30 ? code * 0.1 : 0.0;
}

// GTAO occludes less than HBAO+ away from creases; at strength 1 its darkest contact
// shading matches HBAO+'s.
static const float kGtaoCalibration = 2.0;

#if ENHANCED
// ---------------------------------------------------------------- GTAO
// Jimenez et al. 2016: per slice, find the highest horizon on both sides within the AO
// radius, then integrate the cosine-weighted visibility between them analytically.
// Same radius, falloff and quarter-res texel snapping as HBAO+; twice its samples.
static const int kSlices = 4;      // each searched both ways
static const int kSliceSteps = 6;  // per side
// Thickness heuristic: a sample below the current horizon pulls it back down a little,
// so thin objects in front (poles, railings, leaves) don't cast wide dark halos.
static const float kHorizonDecay = 0.15;

float Gtao(float2 uv, float3 p, float3 n, RadiusParams r)
{
    float3 v = normalize(-p);
    float stepPixels = r.radiusPixels / (kSliceSteps + 1);
    float visibility = 0.0, weight = 0.0;
    [unroll] for (int slice = 0; slice < kSlices; ++slice)
    {
        float angle = (slice + 0.5) * (kPi / kSlices);
        float2 dir = RotateDirection(float2(cos(angle), sin(angle)), PerPassJitter.xy);
        // A step along the screen at constant depth, in view space.
        float3 sliceDir = normalize(float3(dir * UVToViewAB.xy, 0.0));
        float3 orthoDir = sliceDir - dot(sliceDir, v) * v;
        float3 axis = normalize(cross(orthoDir, v));
        float3 projN = n - axis * dot(n, axis);
        float projLen = length(projN);
        float cosN = saturate(dot(projN, v) / max(projLen, 1e-4));
        float nAngle = (dot(orthoDir, projN) < 0.0 ? -1.0 : 1.0) * acos(cosN);

        float horizonCos[2];
        [unroll] for (int side = 0; side < 2; ++side)
        {
            float2 sideDir = side ? dir : -dir;
            float rayPixels = PerPassJitter.z * stepPixels + 1.0;
            float h = -1.0;
            [unroll] for (int k = 0; k < kSliceSteps; ++k)
            {
                float2 snapped = round(rayPixels * sideDir) * PixelSizeDepthThresholdNegInvSharpness.xy + uv;
                float3 d = FetchQuarterResViewPos(snapped) - p;
                float dd = dot(d, d);
                float c = dot(d, v) * rsqrt(max(dd, 1e-8));
                float falloff = saturate(dd * r.negInvR2 + 1.0);
                c = lerp(-1.0, c, falloff);  // fade with distance
                // Only samples within the radius may lower the horizon: beyond it they say
                // nothing about thickness, and would erase a real occluder close by.
                h = c > h ? c : lerp(h, c, kHorizonDecay * falloff);
                rayPixels += stepPixels;
            }
            horizonCos[side] = h;
        }
        float h0 = nAngle + max(-acos(horizonCos[0]) - nAngle, -kPi * 0.5);
        float h1 = nAngle + min(acos(horizonCos[1]) - nAngle, kPi * 0.5);
        float sinN = sin(nAngle), cosNA = cos(nAngle);
        float arc = (-cos(2.0 * h0 - nAngle) + cosNA + 2.0 * h0 * sinN) +
                    (-cos(2.0 * h1 - nAngle) + cosNA + 2.0 * h1 * sinN);
        // Divided by the same integral with nothing in the way (horizons at n +- 90
        // degrees), so an open surface is exactly 1 whatever the slice's tilt.
        float open = 4.0 * (cosNA + nAngle * sinN);
        visibility += projLen * saturate(arc / max(open, 1e-4));
        weight += projLen;
    }
    return visibility / max(weight, 1e-4);
}
#endif

float4 main(float4 svPos : SV_Position, float2 texCoord : TEXCOORD0) : SV_Target
{
    float2 fullResPos = floor(svPos.xy) * 4.0 + PerPassOffset;
    float2 uv = (PixelSizeDepthThresholdNegInvSharpness.xy * 0.25) * fullResPos;
    float3 p = FetchQuarterResViewPos(uv);

    RadiusParams r = GetAORadiusParams(p.z);
    if (r.radiusPixels < 1.0)
        return 1.0;

    precise float2 normalUv = (g_vRenderRes * uv) / g_vRenderRes;
    float3 n = g_sReconstructedNormalsTex.Sample(g_sReconstructedNormals, normalUv).xyz * 2.0 - 1.0;

#if ENHANCED
    float gtaoStrength = GtaoStrength();
    if (gtaoStrength > 0.0)
    {
        // Strength follows the game's AO amounts too (stock 1.1 small, 2 large).
        float occlusion = (1.0 - Gtao(uv, p, n, r)) * gtaoStrength * kGtaoCalibration *
                          ((BiasSmallScaleLargeScale.y + BiasSmallScaleLargeScale.z) * 0.5);
        if (PixelSizeDepthThresholdNegInvSharpness.w != -1.0)
            occlusion *= saturate((PixelSizeDepthThresholdNegInvSharpness.z * p.z + 1.0) *
                                  PixelSizeDepthThresholdNegInvSharpness.w);
        float gtaoResult = saturate(1.0 - occlusion);
#if DEBUG_STRIPES
        if (frac(svPos.x / 64.0) < 0.5)  // 128-pixel stripes: GTAO is on
            gtaoResult = 0.0;
#endif
        return float4(gtaoResult, p.z, 0.0, 0.0);
    }
#endif

    float stepPixels = r.radiusPixels / (NUM_STEPS + 1);
    const float alpha = 2.0 * kPi / NUM_DIRECTIONS;
    float smallScale = 0.0;
    float largeScale = 0.0;
    [unroll] for (int d = 0; d < NUM_DIRECTIONS; ++d)
    {
        float angle = alpha * d;
        float2 dir = RotateDirection(float2(cos(angle), sin(angle)), PerPassJitter.xy);
        float rayPixels = PerPassJitter.z * stepPixels + 1.0;
        {
            float2 snapped = round(rayPixels * dir) * PixelSizeDepthThresholdNegInvSharpness.xy + uv;
            smallScale += ComputeAO(p, n, FetchQuarterResViewPos(snapped), r.negInvR2);
            rayPixels += stepPixels;
        }
        [unroll] for (int s = 1; s < NUM_STEPS; ++s)
        {
            float2 snapped = round(rayPixels * dir) * PixelSizeDepthThresholdNegInvSharpness.xy + uv;
            largeScale += ComputeAO(p, n, FetchQuarterResViewPos(snapped), r.negInvR2);
            rayPixels += stepPixels;
        }
    }
    float ao = (smallScale * BiasSmallScaleLargeScale.y + largeScale * BiasSmallScaleLargeScale.z) /
               (NUM_DIRECTIONS * NUM_STEPS);
    if (PixelSizeDepthThresholdNegInvSharpness.w != -1.0)
        ao *= saturate((PixelSizeDepthThresholdNegInvSharpness.z * p.z + 1.0) *
                       PixelSizeDepthThresholdNegInvSharpness.w);
    float result = saturate(1.0 - ao * 2.0);
#if DEBUG_STRIPES
    // Proof that the replacement is live: full occlusion in 32-pixel stripes (8 pixels
    // in this quarter-resolution layer, every layer alike, so they survive reinterleaving).
    if (frac(svPos.x / 16.0) < 0.5)
        result = 0.0;
#endif
    return float4(result, p.z, 0.0, 0.0);
}
