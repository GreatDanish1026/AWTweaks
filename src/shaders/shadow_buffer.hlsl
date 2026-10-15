// Reconstruction of Alan Wake Remastered's ShadowBuffer.rfx "update_shadow_buffer_PS":
// the screen-space sun/moon shadow pass every material shader reads (g_sShadowBuffer).
// Rebuilt from the game's DXIL; compiled four ways, matching the four shipped variants:
//   NUM_TAPS 12 or 10 (Vogel-disk PCF taps), REGION_START 1 (fade in at the
//   cascade's start) or 0 (fade out towards its end). build_shaders.ps1 compiles them
//   all into shadow_buffer_blobs.h, three ways each:
//   - rebuilt (bit-identical to the game's shader),
//   - ENHANCED 1: adds sun-size soft shadows (PCSS) and screen-space contact shadows,
//     switched on and tuned live by AWTweaks through the "Cascade Filter Size" tweakable
//     (see DecodeSettings); without that it runs exactly the rebuilt code,
//   - DEBUG_STRIPES 1: rebuilt plus a test pattern.
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
#ifndef ENHANCED
#define ENHANCED 0
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

// ---------------------------------------------------------------- settings channel
// Settings ride in the "Shadow Filtering d3t:Cascade Filter Size" tweakable
// (g_fCascadePCFSize_d3t): the engine only uploads constants the game's own shader
// reads, and this one is read by nothing else. AWTweaks rounds the size to 0.1 and
// adds code * 0.00001, code = 1 + sunSize * 100 + contact:
//   sunSize 0..49: apparent sun/moon diameter in 0.1 degree steps (real sun: 5)
//   contact 0..99: contact shadow ray length in 2 cm steps (0 = off)
// That moves the filter radius by under 0.4%. No code - the game's own value, or the
// mod toggled off with F10 - means stock.
struct Settings
{
    bool on;
    float filterSize;     // the size without the code
    float tanHalfSun;
    float contactLength;  // metres
};

Settings DecodeSettings()
{
    Settings st;
    float v = g_fCascadePCFSize_d3t;
    st.filterSize = floor(v * 10.0 + 0.001) * 0.1;
    int code = (int)round((v - st.filterSize) * 100000.0);
    st.on = code >= 1 && code <= 5000;
    int p = st.on ? code - 1 : 0;
    st.tanHalfSun = tan(radians((p / 100) * 0.1) * 0.5);
    st.contactLength = (p % 100) * 0.02;
    if (!st.on)
        st.filterSize = v;
    return st;
}

#if ENHANCED

static const int kSearchTaps = 16;
static const int kFilterTaps = 24;
static const float kMaxRadiusTexels = 40.0;  // kernel cap; keeps taps inside the cascade's atlas slot
static const float kGoldenAngle = 2.39996323;

float2 Vogel(int i, int n, float rotation)
{
    float r = sqrt((i + 0.5) / n);
    float a = i * kGoldenAngle + rotation;
    return r * float2(cos(a), sin(a));
}

// Percentage-closer soft shadows for a directional light: average the depth of the
// occluders around the receiver, then widen the filter by how far they are above it.
float SoftShadow(float3 lightPos, float compareZ, float stockRadius, float rotation, Settings st)
{
    // Light-space scale from the projection: shadow-map UV and depth units per metre.
    float uvPerMetre = length(g_mShadowProjection[0].xyz);
    float zPerMetre = length(g_mShadowProjection[2].xyz);
    float texel = 1.0 / g_vShadowMapRes.x;
    float maxRadius = kMaxRadiusTexels * texel;
    // Each metre of occluder height widens the penumbra by tan(sun radius) metres.
    float widen = st.tanHalfSun * uvPerMetre / zPerMetre;

    // Blocker search over the largest kernel we'd use.
    float blockers = 0.0, blockerZ = 0.0;
    [unroll] for (int i = 0; i < kSearchTaps; ++i)
    {
        float2 uv = lightPos.xy + Vogel(i, kSearchTaps, rotation) * maxRadius;
        float d = g_sShadowMapTex.SampleLevel(g_sLinearDepthBuffer, uv, 0);
        if (d < compareZ)
        {
            blockers += 1.0;
            blockerZ += d;
        }
    }
    if (blockers == 0.0)
        return 1.0;
    blockerZ /= blockers;
    float radius = clamp((compareZ - blockerZ) * widen, stockRadius, maxRadius);

    // Wider kernels reach across sloped receivers: bias each tap by its distance
    // (a 45-degree slope stays clean) on top of the game's constant bias.
    float slopeBias = zPerMetre / uvPerMetre;
    float lit = 0.0;
    [unroll] for (int j = 0; j < kFilterTaps; ++j)
    {
        float2 o = Vogel(j, kFilterTaps, rotation) * radius;
        lit += g_sShadowMapTex.SampleCmpLevelZero(g_sComparisonSampler, lightPos.xy + o,
                                                  compareZ - slopeBias * length(o));
    }
    return lit / kFilterTaps;
}

// Screen-space contact shadows: march from the pixel towards the sun through the depth
// buffer and darken if something in front of the camera blocks the ray. Catches the
// small-scale contact the shadow map is too coarse for.
// Why the march ended, for the Debug view: 0 hit, 1/3 ray went behind the camera,
// 2/3 ray left the screen, 1 no hit along the whole ray.
static float g_contactExit;

float2 PixelOf(float3 p, float2 svPosXY, float4 viewRay, float2 rayStep)
{
    float2 target = p.xy * (viewRay.z / p.z);
    return svPosXY + (target - viewRay.xy) / rayStep;
}

float ContactShadow(float2 svPosXY, float4 viewRay, float2 rayStep, float3 viewPos, float rotation, Settings st)
{
    // Direction to the light in view space: the shadow projection's UV rows are
    // perpendicular to it, and shadow depth grows away from the light.
    float3 c = cross(g_mShadowProjection[0].xyz, g_mShadowProjection[1].xyz);
    float3 toLight = normalize(c) * (dot(g_mShadowProjection[2].xyz, c) > 0.0 ? -1.0 : 1.0);

    // The view ray is interpolated linearly across the screen, so its per-pixel
    // derivatives (rayStep) map a view-space point back to a pixel.
    float depthScale = viewRay.z * g_fInvNear;  // linear depth -> view z

    const int kSteps = 12;
    float stepLength = st.contactLength / kSteps;
    float jitter = frac(rotation * 0.15915494 + 0.5);  // reuse the noise: 0..1
    float thickness = max(0.15, st.contactLength * 0.5);
    float occlusion = 0.0;
    g_contactExit = 1.0;
    [loop] for (int i = 0; i < kSteps; ++i)
    {
        float3 p = viewPos + toLight * (stepLength * (i + jitter));
        if (p.z <= 0.05)
        {
            g_contactExit = 1.0 / 3.0;
            break;
        }
        float2 uv = PixelOf(p, svPosXY, viewRay, rayStep) / g_vRenderRes;
        if (any(uv < 0.0) || any(uv > 1.0))
        {
            g_contactExit = 2.0 / 3.0;
            break;
        }
        float sceneZ = g_sLinearDepthBufferTex.SampleLevel(g_sLinearDepthBuffer, uv, 0) * depthScale;
        float behind = p.z - sceneZ;
        if (behind > 0.02 && behind < thickness)
        {
            // Fade hits near the end of the ray so the shadow tapers.
            occlusion = 1.0 - (float)i / kSteps;
            g_contactExit = 0.0;
            break;
        }
    }
    // Fade out with camera distance (the shadow map covers far detail well).
    return 1.0 - occlusion * saturate(2.0 - viewPos.z / 25.0);
}
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

#if ENHANCED
    float2 rayStep = float2(ddx(viewRay.x), ddy(viewRay.y));  // outside any branch
#endif

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
#if ENHANCED
    Settings st = DecodeSettings();
    if (st.on)
    {
        if (st.tanHalfSun > 0.0)
            shadow = SoftShadow(lightPos, compareZ, radius.x * kRadius[NUM_TAPS - 1], rotation, st);
        if (st.contactLength > 0.0)  // sky is far away: the distance fade drops it
            shadow = min(shadow, ContactShadow(svPos.xy, viewRay, rayStep, viewPos, rotation, st));
    }
#endif
#if DEBUG_STRIPES
    Settings dbg = DecodeSettings();
    if (dbg.on && dbg.contactLength > 0.0)
    {
        // Contact shadow diagnostics, in three vertical panels of the sun/moon light:
        //  left:   the contact shadow alone (dark = occluded)
        //  middle: screen mapping self-check - bright where a pixel's own position maps
        //          back to within half a pixel of itself, dark where it doesn't
        //  right:  why the march ended - black hit, dark grey behind the camera,
        //          light grey left the screen, white nothing hit
        float panel = svPos.x / g_vRenderRes.x * 3.0;
        float contact = ContactShadow(svPos.xy, viewRay, rayStep, viewPos, rotation, dbg);
        if (panel < 1.0)
            shadow = contact;
        else if (panel < 2.0)
            shadow = saturate(1.0 - length(PixelOf(viewPos, svPos.xy, viewRay, rayStep) - svPos.xy) * 2.0);
        else
            shadow = g_contactExit;
    }
    else
    {
        // Proof that the replacement is live: sun/moon shadows vanish in stripes, 32 pixels
        // wide normally and 128 while AWTweaks sends soft/contact settings.
        float stripe = dbg.on ? 128.0 : 32.0;
        if (frac(svPos.x / (2.0 * stripe)) < 0.5)
            shadow = 1.0;
    }
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
