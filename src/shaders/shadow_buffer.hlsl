// Reconstruction of Alan Wake Remastered's ShadowBuffer.rfx "update_shadow_buffer_PS":
// the screen-space sun/moon shadow pass every material shader reads (g_sShadowBuffer).
// Rebuilt from the game's DXIL; compiled four ways, matching the four shipped variants:
//   NUM_TAPS 12 or 10 (Vogel-disk PCF taps), REGION_START 1 (fade in at the
//   cascade's start) or 0 (fade out towards its end). DEBUG_STRIPES 1 adds a visible
//   test pattern. build_shaders.ps1 compiles them all into shadow_buffer_blobs.h.
// Bindings, constant layout and signatures are identical to the original so it drops
// into the game's pipeline (root signature) unchanged.
//
// Output: x = shadow (1 = lit), y/z = view depth packed into two channels,
//         w = cascade blend weight.

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
    float2 g_vOutputRes;                        // 504
    float g_fRegionStart;                       // 512
    float g_fRegionEnd;                         // 516
    row_major float3x4 g_mShadowProjection;     // 528
    float g_fZThreshold;                        // 576
    float g_fFrontClipPlaneZ;                   // 580
    float g_fBackClipPlaneZ;                    // 584
    float g_fPCFSize;                           // 588
};

SamplerState g_sLinearDepthBuffer : register(s0);
SamplerComparisonState g_sComparisonSampler : register(s1);
Texture2D<float> g_sLinearDepthBufferTex : register(t0);
Texture2D<float> g_sShadowMapTex : register(t1);

#ifndef NUM_TAPS
#define NUM_TAPS 12
#endif
#ifndef REGION_START
#define REGION_START 0
#endif

// Tap tables: exact float32 constants from the original (angle i*2.4, radius sqrt((i+0.5)/N)).
static const float kAngle[12] = {0.0, 2.4000000953674316, 4.800000190734863, 7.200000286102295,
                                 9.600000381469727, 12.0, 14.40000057220459, 16.80000114440918,
                                 19.200000762939453, 21.600000381469727, 24.0, 26.400001525878906};
#if NUM_TAPS == 12
static const float kRadius[12] = {0.20412415266036987, 0.3535534143447876, 0.4564354717731476,
                                  0.5400617718696594, 0.6123723983764648, 0.6770032048225403,
                                  0.7359800934791565, 0.7905694842338562, 0.8416253924369812,
                                  0.8897565007209778, 0.9354143142700195, 0.9789450168609619};
static const float kInvTaps = 0.0833333358168602;
#elif NUM_TAPS == 10
static const float kRadius[10] = {0.22360679507255554, 0.3872983455657959, 0.5, 0.5916079878807068,
                                  0.6708203554153442, 0.741619884967804, 0.8062257766723633,
                                  0.866025447845459, 0.9219543933868408, 0.9746794104576111};
static const float kInvTaps = 0.10000000149011612;
#else
#error NUM_TAPS must be 10 or 12
#endif

float4 main(float4 svPos : SV_Position, float4 viewRay : TEXCOORD0) : SV_Target
{
    // View-space position from linear depth.
    float2 uv = svPos.xy / g_vRenderRes;
    float depth = g_sLinearDepthBufferTex.Sample(g_sLinearDepthBuffer, uv);
    // 'precise' keeps the original's evaluation order (it was built with a 2019 DXC that
    // didn't reassociate these), so results stay bit-identical.
    precise float3 viewPos = (depth * viewRay.xyz) * g_fInvNear;

    // Into this cascade's region of the shadow atlas.
    float3 lightPos = mul(g_mShadowProjection, float4(viewPos, 1.0));
    precise float halfRadius = (g_fPCFSize * 2000.0 * g_fCascadePCFSize_d3t) * 0.5;
    float2 radius = halfRadius / g_vShadowMapRes;
    float compareZ = lightPos.z - g_fPCFBias_d3t;

    // Interleaved gradient noise rotates the Vogel disk per pixel.
    float rotation = frac(frac(dot(svPos.xy, float2(0.06711056, 0.00583715))) * 52.9829189) * 6.28318548;

    float lit = 0.0;
    [unroll] for (int i = 0; i < NUM_TAPS; ++i)
    {
        float a = rotation + kAngle[i];
        float2 offset = radius * kRadius[i] * float2(cos(a), sin(a));
        lit += g_sShadowMapTex.SampleCmpLevelZero(g_sComparisonSampler, lightPos.xy + offset, compareZ);
    }
    float shadow = lit * kInvTaps;
#if DEBUG_STRIPES
    // Proof that the replacement is live: sun/moon shadows vanish in 32-pixel stripes.
    if (frac(svPos.x / 64.0) < 0.5)
        shadow = 1.0;
#endif

    // View depth packed into two channels (z/2000 and z/7.8125, 8 bits each).
    float z = viewPos.z;
    float packedHi = frac(z * 0.0005) - frac(z * 0.128) * 0.00390625;
    float packedLo = frac(z * 0.128) - frac(z * 32.768) * 0.00390625;

#if REGION_START
    float blend = saturate((z - g_fRegionStart) / (g_fRegionStart * asfloat(0x3E4CCCD0))); // 0.20000005: the original's exact constant
#else
    float t = saturate((z - g_fRegionEnd * 0.5) / (g_fRegionEnd * 0.5));
    float blend = 1.0 - t * t * (3.0 - 2.0 * t);
#endif
    return float4(shadow, packedHi, packedLo, blend);
}
