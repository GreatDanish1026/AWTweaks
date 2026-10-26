// Rebuild of DeferredLight.rfx "spotlightPS", the shadow-mapped, projection-textured
// variant: every shadow-casting spotlight, Alan's flashlight among them, as a deferred
// pass into the light buffer (rgb = diffuse light, a = specular luminance). It shadows
// with a 10-tap Vogel-disk PCF of radius "Shadow Filtering d3t:Filter Size" texels,
// rotated per pixel by interleaved gradient noise, then lights with Ward (anisotropic,
// where the G-buffer has a tangent) or gloss-driven Blinn-Phong.
// Bindings, constant layout and signatures match the original, so it drops into the
// game's pipeline unchanged. ENHANCED 1 adds soft (lens-size) and contact shadows,
// switched on by AWTweaks through the low digits of Filter Size (see DecodeSettings);
// without that it runs exactly the rebuilt code. DEBUG_STRIPES 1 adds a test pattern.

#ifndef DEBUG_STRIPES
#define DEBUG_STRIPES 0
#endif
#ifndef ENHANCED
#define ENHANCED 0
#endif

cbuffer Globals : register(b0)
{
    float2 g_vRenderRes : packoffset(c26.z);
    float2 g_vShadowMapRes : packoffset(c29.z);
    float g_fPCFSize_d3t : packoffset(c31.y);
    row_major float4x4 g_mLightProjectionMatrix : packoffset(c90);
    float3 g_vLightPosition : packoffset(c114);
    float3 g_vLightColor : packoffset(c116);
    float4 g_vLightFalloff : packoffset(c117);
    row_major float4x4 g_mShadowProjectionMatrix : packoffset(c119);
    float g_fShadowLOD : packoffset(c123.x);
};

SamplerState g_sLinearDepthBuffer : register(s0);
SamplerState g_sNormalBuffer : register(s1);
SamplerState g_sTangentGlossBuffer : register(s2);
SamplerState g_sExposureTexture : register(s3);
SamplerComparisonState g_sComparisonSampler : register(s4);
SamplerState g_sLightProjectionMap : register(s5);
Texture2D<float4> g_sLinearDepthBufferTex : register(t0);
Texture2D<float4> g_sNormalBufferTex : register(t1);
Texture2D<float4> g_sTangentGlossBufferTex : register(t2);
Texture2D<float4> g_sExposureTextureTex : register(t3);
Texture2D<float> g_sShadowMapTex : register(t4);
Texture2D<float4> g_sLightProjectionMapTex : register(t5);

static const float kInvPi = asfloat(0x3EA2F983);

// Vogel disk: radius sqrt((i + 0.5) / 10), angle i * 2.4 rad (the game's float constants).
static const float kTapRadius[10] = {
    asfloat(0x3E64F92E), asfloat(0x3EC64BF8), 0.5, asfloat(0x3F17739F), asfloat(0x3F2BBAE2),
    asfloat(0x3F3DDACD), asfloat(0x3F4E64D0), asfloat(0x3F5DB3D8), asfloat(0x3F6C0534), asfloat(0x3F798497)};
static const float kTapAngle[10] = {
    0.0, asfloat(0x4019999A), asfloat(0x4099999A), asfloat(0x40E66667), asfloat(0x4119999A),
    12.0, asfloat(0x41666667), asfloat(0x41866667), asfloat(0x4199999A), asfloat(0x41ACCCCD)};

float ShadowPCF(float3 shadowPos, float2 svPos)
{
    float noise = frac(dot(svPos, float2(asfloat(0x3D897143), asfloat(0x3BBF4590))));
    float angle0 = frac(noise * asfloat(0x4253EE82)) * asfloat(0x40C90FDB);
    float2 radius = (g_fPCFSize_d3t * 0.5) / g_vShadowMapRes;
    float sum = 0.0;
    [unroll] for (int i = 0; i < 10; ++i)
    {
        float a = i == 0 ? angle0 : angle0 + kTapAngle[i];
        float2 o = float2(radius.x * kTapRadius[i] * cos(a), radius.y * kTapRadius[i] * sin(a));
        sum += g_sShadowMapTex.SampleCmpLevelZero(g_sComparisonSampler, o + shadowPos.xy, shadowPos.z);
    }
    return sum * asfloat(0x3DCCCCCD);
}

// ---------------------------------------------------------------- settings channel
// AWTweaks rounds "Shadow Filtering d3t:Filter Size" (g_fPCFSize_d3t) to 0.1 and adds
// code * 0.00001, code = 1 + lens * 100 + contact: lens radius in 0.2 cm steps (0..49),
// contact shadow length in 2 cm steps (0..99). No code - the game's own value, or the
// mod toggled off with F10 - means stock.
struct Settings
{
    bool on;
    float filterSize;     // the size without the code
    float lensRadius;     // metres
    float contactLength;  // metres
};

Settings DecodeSettings()
{
    Settings st;
    float v = g_fPCFSize_d3t;
    st.filterSize = floor(v * 10.0 + 0.001) * 0.1;
    int code = (int)round((v - st.filterSize) * 100000.0);
    st.on = code >= 1 && code <= 5000;
    int c = st.on ? code - 1 : 0;
    st.lensRadius = (c / 100) * 0.002;
    st.contactLength = (c % 100) * 0.02;
    if (!st.on)
        st.filterSize = v;
    return st;
}

#if ENHANCED
static const int kSearchTaps = 16;
static const int kFilterTaps = 24;
static const float kMaxRadiusTexels = 40.0;  // kernel cap
static const float kGoldenAngle = 2.39996323;

float2 Vogel(int i, int n, float rotation)
{
    float r = sqrt((i + 0.5) / n);
    float a = i * kGoldenAngle + rotation;
    return r * float2(cos(a), sin(a));
}

// Percentage-closer soft shadows for a spotlight with a lens of the given radius: find
// the occluders around the receiver, then widen the filter by lens * (receiver -
// blocker) / blocker, the penumbra a disc-shaped light casts.
float SoftShadow(float3 p, float3 shadowPos, float rotation, Settings st)
{
    // Shadow depth is perspective (z/w, z = a w + b along the light's rays). Measure a
    // and b at this pixel, from the receiver and a point twice as far from the light.
    float4 s0 = mul(g_mShadowProjectionMatrix, float4(p, 1.0));
    float4 s1 = mul(g_mShadowProjectionMatrix, float4(2.0 * p - g_vLightPosition, 1.0));
    float a = (s1.z - s0.z) / (s1.w - s0.w);
    float b = s0.z - a * s0.w;
    float receiverW = s0.w;
    float uvPerMetre = length(g_mShadowProjectionMatrix[0].xyz) / receiverW;

    float2 texel = 1.0 / g_vShadowMapRes;
    float maxRadius = kMaxRadiusTexels * texel.x;
    float stockRadius = (st.filterSize * 0.5) * texel.x;

    float blockers = 0.0, blockerZ = 0.0;
    [unroll] for (int i = 0; i < kSearchTaps; ++i)
    {
        float2 uv = shadowPos.xy + Vogel(i, kSearchTaps, rotation) * maxRadius;
        float d = g_sShadowMapTex.SampleLevel(g_sLinearDepthBuffer, uv, 0);
        if (d < shadowPos.z)
        {
            blockers += 1.0;
            blockerZ += d;
        }
    }
    if (blockers == 0.0)
        return 1.0;
    float blockerW = b / (blockerZ / blockers - a);  // back to distance along the light
    float penumbra = st.lensRadius * max(receiverW - blockerW, 0.0) / max(blockerW, 0.05);
    float radius = clamp(penumbra * uvPerMetre, stockRadius, maxRadius);

    // Wider kernels reach across sloped receivers: bias each tap by its distance so a
    // 45-degree slope stays clean (d(z/w)/dw = -b / w^2 per metre of depth).
    float slopeBias = abs(b) / (receiverW * receiverW) / uvPerMetre;
    float lit = 0.0;
    [unroll] for (int j = 0; j < kFilterTaps; ++j)
    {
        float2 o = Vogel(j, kFilterTaps, rotation) * radius;
        lit += g_sShadowMapTex.SampleCmpLevelZero(g_sComparisonSampler, shadowPos.xy + o,
                                                  shadowPos.z - slopeBias * length(o));
    }
    return lit / kFilterTaps;
}

// Screen-space contact shadows: march from the pixel towards the light through the
// depth buffer and darken if something in front of the camera blocks the ray.
float2 PixelOf(float3 q, float2 svPosXY, float3 viewRay, float2 rayStep)
{
    float2 target = q.xy * (viewRay.z / q.z);
    return svPosXY + (target - viewRay.xy) / rayStep;
}

float ContactShadow(float2 svPosXY, float3 viewRay, float2 rayStep, float3 p, float3 l, float distToLight,
                    float rotation, Settings st)
{
    const int kSteps = 12;
    float rayLength = min(st.contactLength, distToLight * 0.9);  // stop short of the lens
    float stepLength = rayLength / kSteps;
    float jitter = frac(rotation * 0.15915494 + 0.5);
    float thickness = max(0.1, rayLength * 0.5);
    float occlusion = 0.0;
    [loop] for (int i = 0; i < kSteps; ++i)
    {
        float3 q = p + l * (stepLength * (i + jitter));
        if (q.z <= 0.05)
            break;
        float2 uv = PixelOf(q, svPosXY, viewRay, rayStep) / g_vRenderRes;
        if (any(uv < 0.0) || any(uv > 1.0))
            break;
        float sceneZ = g_sLinearDepthBufferTex.SampleLevel(g_sLinearDepthBuffer, uv, 0).x;
        float behind = q.z - sceneZ;
        if (behind > 0.02 && behind < thickness)
        {
            occlusion = 1.0 - (float)i / kSteps;  // taper towards the end of the ray
            break;
        }
    }
    // Fade out with camera distance (the shadow map covers far detail well).
    return 1.0 - occlusion * saturate(2.0 - p.z / 25.0);
}
#endif

float4 main(float4 svPos : SV_Position, float3 viewRay : TEXCOORD0) : SV_Target
{
    float2 uv = svPos.xy / g_vRenderRes;
#if ENHANCED
    float2 rayStep = float2(ddx(viewRay.x), ddy(viewRay.y));  // outside any branch
#endif
    float3 n = g_sNormalBufferTex.Sample(g_sNormalBuffer, uv).xyz * 2.0 - 1.0;
    float4 tangentGloss = g_sTangentGlossBufferTex.Sample(g_sTangentGlossBuffer, uv);
    float depth = g_sLinearDepthBufferTex.Sample(g_sLinearDepthBuffer, uv).x;
    float3 p = depth * viewRay / viewRay.z;
    n = rsqrt(dot(n, n)) * n;

    float3 toLight = g_vLightPosition - p;
    float3 l = toLight * rsqrt(dot(toLight, toLight));
    float dist = sqrt(toLight.x * toLight.x + toLight.y * toLight.y + toLight.z * toLight.z);
    float atten = saturate(exp2(dist * asfloat(0xBFB8AA3B) * g_vLightFalloff.x) - g_vLightFalloff.y);
    // precise: the original multiplies falloff, projection map and shadow in this order.
    precise float3 color = atten * g_vLightColor;

    float4 proj = mul(g_mLightProjectionMatrix, float4(p, 1.0));
    float2 projUv = proj.xy / proj.w * 0.5;
    color *= g_sLightProjectionMapTex.Sample(g_sLightProjectionMap, float2(projUv.x + 0.5, 0.5 - projUv.y)).rgb;

    float4 s = mul(g_mShadowProjectionMatrix, float4(p, 1.0));
    float3 shadowPos = s.xyz / s.w;
#if ENHANCED
    Settings st = DecodeSettings();
    float shadow;
    [branch] if (st.on)
    {
        float rotation = frac(frac(dot(svPos.xy, float2(asfloat(0x3D897143), asfloat(0x3BBF4590)))) *
                              asfloat(0x4253EE82)) * asfloat(0x40C90FDB);
        shadow = st.lensRadius > 0.0 ? SoftShadow(p, shadowPos, rotation, st) : ShadowPCF(shadowPos, svPos.xy);
        if (st.contactLength > 0.0)
            shadow = min(shadow, ContactShadow(svPos.xy, viewRay, rayStep, p, l, dist, rotation, st));
    }
    else
        shadow = ShadowPCF(shadowPos, svPos.xy);
#else
    float shadow = ShadowPCF(shadowPos, svPos.xy);
#endif
    color *= (1.0 - g_fShadowLOD) + shadow * g_fShadowLOD;

    float3 v = -p;
    v = rsqrt(dot(v, v)) * v;
    float3 t = tangentGloss.xyz;
    float gloss = tangentGloss.w;
    float nDotL = dot(n, l);
    float diffuse, specular;
    if (any(t != 0.0))
    {
        // Ward: alpha x = gloss * 255 along the tangent, alpha y = 0.2.
        float3 b = cross(t, n);
        b *= rsqrt(dot(b, b));
        float nDotV = dot(n, v);
        specular = 0.0;
        if (nDotL >= 0.0 && nDotV >= 0.0)
        {
            float ax = gloss * 255.0;
            float3 h = v + l;
            h *= rsqrt(dot(h, h));
            float nDotH = dot(n, h);
            float bDotH = dot(b, h);
            float tDotH = dot(t, h);
            float norm = sqrt(nDotL / max(nDotV, asfloat(0x3CA3D70A))) / (gloss * asfloat(0x4420389A));
            // pow(x, 2) as the original compiler emitted it, not x * x.
            float e = (exp2(log2(abs(tDotH / ax)) * 2.0) + exp2(log2(abs(bDotH * 5.0)) * 2.0)) / (nDotH + 1.0);
            specular = norm * kInvPi * exp2(e * asfloat(0xC038AA3B));
        }
        diffuse = saturate(nDotL);
    }
    else
    {
        float hasGloss = gloss != 0.0 ? 1.0 : 0.0;
        float power = (max(5.0, pow(saturate(gloss), 3.0) * 3000.0) - 40.0) * hasGloss + 40.0;
        diffuse = saturate(nDotL);
        float3 h = v + l;
        h *= rsqrt(dot(h, h));
        specular = hasGloss * kInvPi * exp2(log2(max(dot(n, h), 0.0)) * power);
    }

    precise float vDiffuse = diffuse * kInvPi;  // not "fast" in the original either
    precise float3 lit = vDiffuse * color;
    float spec = dot(color, float3(asfloat(0x3E59B3D0), asfloat(0x3F371759), asfloat(0x3D93DD98))) * specular;
    float exposure = g_sExposureTextureTex.Sample(g_sExposureTexture, float2(0.0, 0.0)).x;
    precise float3 litExposed = lit * exposure;  // (diffuse * color) * exposure, in that order
    float4 result = min(float4(litExposed, spec * exposure), 32.0);
#if DEBUG_STRIPES
    // Stripes with no spotlight: 32 pixels wide, 128 while AWTweaks' settings arrive.
    if (frac(svPos.x / (DecodeSettings().on ? 256.0 : 64.0)) < 0.5)
        result = 0.0;
#endif
    return result;
}
