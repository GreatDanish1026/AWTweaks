// Rebuild of VolumetricLight.rfx "volumetric_light" pixel shader #15: the shadowed
// spotlight beam (Alan's flashlight among them), drawn as a full-screen pass at half
// resolution. Per pixel it intersects the view ray with the light's cone (a unit cone
// in "cone space", capped at g_fNearPlaneCap and 1), clips the segment against the
// scene depth and marches it in 128 steps. Each step does one point-sampled shadow
// map compare (no filtering) and attenuates by exp(-decay * distance from the light);
// the sum is scaled by the segment length, a Henyey-Greenstein phase function, light
// intensity and exposure. A 32x32 jitter texture offsets the steps (z) and the shadow
// lookups (xy) per pixel; the game's blur and temporal passes smooth that noise out.
// Bindings, constant layout and signatures match the original, so it drops into the
// game's pipeline unchanged. ENHANCED 1 adds a better march (see EnhancedMarch),
// switched on by AWTweaks through the low digits of "Volumetric Light:Jitter Z" (see
// DecodeSettings); without that it runs exactly the rebuilt code. DEBUG_STRIPES 1 adds
// a test pattern.

#ifndef DEBUG_STRIPES
#define DEBUG_STRIPES 0
#endif
#ifndef ENHANCED
#define ENHANCED 0
#endif

cbuffer Globals : register(b0)
{
    row_major float4x4 g_mViewToCone : packoffset(c41);
    row_major float4x4 g_mConeToLight : packoffset(c49);
    row_major float4x4 g_mConeToShadow : packoffset(c53);
    float g_fNearPlaneCap : packoffset(c57.x);
    float3 g_vLightIntensity : packoffset(c57.y);
    float g_fLightDecay : packoffset(c58.x);
    float3 g_vLightDirVS : packoffset(c58.y);
    float2 g_vOutputRes : packoffset(c59.x);
    float3 g_vHGConstants : packoffset(c60.x);
    float3 g_vJitter : packoffset(c61.x);
    float2 g_vVolumetricLightJitterOffset : packoffset(c62.x);
    float g_fTemporalBlur : packoffset(c64.x);
};

SamplerState g_sLinearDepthBuffer_HalfRes : register(s0);
SamplerState g_sExposureTexture : register(s1);
SamplerState g_sVolumetricLightShadowMap : register(s2);
SamplerState g_sVolumetricLightJitterMap : register(s3);
Texture2D<float> g_sLinearDepthBuffer_HalfResTex : register(t0);
Texture2D<float> g_sExposureTextureTex : register(t1);
Texture2D<float> g_sVolumetricLightShadowMapTex : register(t2);
Texture2D<float4> g_sVolumetricLightJitterMapTex : register(t3);

static const int kSteps = 128;

// Keeps the compiler from regrouping a product across this point, so multiplies happen in
// the original's order: (distance * -log2(e)) * decay, not distance * (-log2(e) * decay)
// hoisted out of the loop; intensity before exposure. max(x, -FLT_MAX) is x for every
// value the shader produces (only -inf and NaN would change). `precise` can't do this:
// DXC spreads it to everything the value depends on, the whole march included, and that
// changes other results.
float Barrier(float x) { return max(x, -asfloat(0x7F7FFFFF)); }
float3 Barrier(float3 x) { return max(x, -asfloat(0x7F7FFFFF)); }

// Ray against the cone x^2 + y^2 = z^2 between the planes z = zNear and z = zFar.
bool intersectRayCappedCone2(float3 o, float3 d, float zNear, float zFar, out float tNear, out float tFar)
{
    float a = d.x * d.x + d.y * d.y - d.z * d.z;
    float b = (d.x * o.x + d.y * o.y) * 2.0 - o.z * 2.0 * d.z;
    float c = o.x * o.x + o.y * o.y - o.z * o.z;
    float disc = b * b - c * 4.0 * a;
    float root = sqrt(disc);
    tNear = asfloat(0x7F7FFFFF);
    tFar = -asfloat(0x7F7FFFFF);
    float t = (-root - b) / (a * 2.0);
    float z = t * d.z + o.z;
    if (z > zNear && z < zFar)
    {
        tNear = min(tNear, t);
        tFar = max(tFar, t);
    }
    t = (root - b) / (a * 2.0);
    z = t * d.z + o.z;
    if (z > zNear && z < zFar)
    {
        tNear = min(tNear, t);
        tFar = max(tFar, t);
    }
    if (d.z != 0.0)
    {
        t = -(o.z - zNear) / d.z;
        float2 p = t * d.xy + o.xy;
        if (dot(p, p) < zNear * zNear)
        {
            tNear = min(tNear, t);
            tFar = max(tFar, t);
        }
        t = -(o.z - zFar) / d.z;
        p = t * d.xy + o.xy;
        if (dot(p, p) < zFar * zFar)
        {
            tNear = min(tNear, t);
            tFar = max(tFar, t);
        }
    }
    return disc > 0.0 && tNear < tFar;
}

// ---------------------------------------------------------------- settings channel
// AWTweaks rounds "Volumetric Light:Jitter Z" (g_vJitter.z, stock 0.4) to 0.01 and adds
// code * 0.000001, code = beam quality in 0.1 steps (1..30). No code - the game's own
// value, or the mod toggled off with F10 - means stock.
struct Settings
{
    bool on;
    float jitterZ;  // the step jitter without the code
    float quality;  // 0.1 .. 3
};

Settings DecodeSettings()
{
    Settings st;
    float v = g_vJitter.z;
    float base = floor(v * 100.0 + 0.001) * 0.01;
    float scaled = (v - base) * 1000000.0;
    int code = (int)round(scaled);
    // Off unless the digits are a clean code: a value the engine scaled on its way here
    // would leave noise in them.
    st.on = code >= 1 && code <= 30 && abs(scaled - code) < 0.25;
    st.quality = code * 0.1;
    st.jitterZ = st.on ? base : v;
    return st;
}

#if ENHANCED
// Shadow test with bilinear filtering (percentage-closer): the 2x2 texels a bilinear
// fetch would blend, each compared, then blended. The stock march compares one point
// sample, so every step is fully lit or fully dark and shadow edges in the beam step
// and sparkle. The sampler (s2) is a plain one, so Gather does the comparisons' fetch.
float ShadowBilinear(float2 uv, float z, float2 res)
{
    float2 p = uv * res - 0.5;
    float2 f = frac(p);
    float4 d = g_sVolumetricLightShadowMapTex.Gather(g_sVolumetricLightShadowMap, (floor(p) + 1.0) / res);
    float4 lit = step(z, d);  // z <= d; x (0,1), y (1,1), z (1,0), w (0,0)
    return lerp(lerp(lit.w, lit.z, f.x), lerp(lit.x, lit.y, f.x), f.y);
}

// The beam march with
//  - filtered shadows (ShadowBilinear);
//  - a step count from how many shadow-map texels the segment crosses (0.75 steps per
//    texel), between 48 x quality and 32 + 128 x quality: a lamp's beam seen from the
//    side gets steps for its shadow detail, a short segment fewer than stock. Quality 1
//    averages about 140 steps in shader_compare's scenes (stock: always 128), 2 about 220.
//    Steps stay evenly spaced along the ray: spacing them evenly in shadow-map space
//    instead (more near the lamp, weighted by the Jacobian) flickered more;
//  - the falloff exp(-decay * distance) as a running product (distance is linear in t,
//    so each step multiplies by the same factor): one multiply per step for an exp2.
// Returns the same quantity as the stock loop: segment length / steps * sum.
float EnhancedMarch(float4 shadowStart, float4 shadowDelta, float distStart, float distDelta, float segment,
                    float jitterZ, float quality)
{
    float2 res;
    g_sVolumetricLightShadowMapTex.GetDimensions(res.x, res.y);
    float4 shadowEnd = shadowStart + shadowDelta;
    float2 uvStart = shadowStart.xy / max(shadowStart.w, 1e-6);
    float2 uvEnd = shadowEnd.xy / max(shadowEnd.w, 1e-6);
    float texels = length((uvEnd - uvStart) * res);
    float minSteps = round(48.0 * quality), maxSteps = round(32.0 + 128.0 * quality);
    int steps = (int)clamp(ceil(texels * 0.75), max(minSteps, 8.0), maxSteps);

    float dt = 1.0 / steps;
    float t = (jitterZ + 0.5) * dt;
    float k = asfloat(0xBFB8AA3B) * g_fLightDecay;  // -log2(e) * decay
    float weight = exp2(k * (t * distDelta + distStart));
    float ratio = exp2(k * distDelta * dt);
    float sum = 0.0;
    [loop] for (int i = 0; i < steps; ++i)
    {
        float4 s = shadowDelta * t + shadowStart;
        float invW = 1.0 / s.w;
        sum += ShadowBilinear(s.xy * invW, s.z * invW, res) * weight;
        weight *= ratio;
        t += dt;
    }
    return segment * dt * sum;
}
#endif

float4 main(float4 svPos : SV_Position, float2 uv : TEXCOORD0, float3 viewRay : TEXCOORD1) : SV_Target
{
    float depth = g_sLinearDepthBuffer_HalfResTex.Sample(g_sLinearDepthBuffer_HalfRes, uv);
    float3 dir = rsqrt(dot(viewRay, viewRay)) * viewRay;
    float3 o = float3(g_mViewToCone._14, g_mViewToCone._24, g_mViewToCone._34);
    float3 d = mul((float3x3)g_mViewToCone, dir);
    float tNear, tFar;
    bool hit = intersectRayCappedCone2(o, d, g_fNearPlaneCap, 1.0, tNear, tFar);
    float sceneT = depth / dir.z;

    float4 result = 0.0;
    if (hit && tFar >= 0.0 && tNear <= sceneT)
    {
        float tStart = max(tNear, 0.0);
        float tEnd = min(tFar, sceneT);
        float3 start = tStart * d + o;
        float3 end = tEnd * d + o;
        float distStart = mul(g_mConeToLight, float4(start, 1.0)).z;
        float distDelta = mul(g_mConeToLight, float4(end, 1.0)).z - distStart;

        float3 jitterTex = g_sVolumetricLightJitterMapTex.Sample(g_sVolumetricLightJitterMap,
            (g_vOutputRes * uv + g_vVolumetricLightJitterOffset) * (1.0 / 32.0)).xyz * 2.0 - 1.0;
        float3 jitter = jitterTex * g_vJitter;

        float4 shadowStart = mul(g_mConeToShadow, float4(start, 1.0));
        float4 shadowDelta = mul(g_mConeToShadow, float4(end, 1.0)) - shadowStart;
        shadowStart.xy += jitter.xy * shadowStart.w;

        float scatter;
#if ENHANCED
        Settings st = DecodeSettings();
        [branch] if (st.on)
            scatter = EnhancedMarch(shadowStart, shadowDelta, distStart, distDelta, tEnd - tStart,
                                    jitterTex.z * st.jitterZ, st.quality);
        else
#endif
        {
        float sum = 0.0;
        [unroll] for (int i = 0; i < kSteps; ++i)
        {
            float t = (jitter.z + (i + 0.5)) * (1.0 / kSteps);
            float dist = t * distDelta + distStart;
            float4 s = shadowDelta * t + shadowStart;
            float3 sp = s.xyz / s.w;
            float lit = sp.z <= g_sVolumetricLightShadowMapTex.SampleLevel(g_sVolumetricLightShadowMap, sp.xy, 0.0)
                            ? 1.0 : 0.0;
            sum += lit * exp2(Barrier(dist * asfloat(0xBFB8AA3B)) * g_fLightDecay);
        }
        scatter = (tEnd - tStart) * (1.0 / kSteps) * sum;
        }

        float cosTheta = dot(-dir, g_vLightDirVS);
        float phase = g_vHGConstants.x / exp2(log2(abs(g_vHGConstants.z * cosTheta + g_vHGConstants.y)) * 1.5);
        float3 light = Barrier(g_vLightIntensity * (scatter * phase));  // intensity, then exposure
        float exposure = g_sExposureTextureTex.Sample(g_sExposureTexture, float2(0.0, 0.0));
        result = float4(min(light * exposure, 32.0) * asfloat(0x3E19999A), g_fTemporalBlur);
    }
#if DEBUG_STRIPES
    // Stripes with no beam: 16 half-res pixels (32 screen pixels) wide, 128 screen pixels
    // while AWTweaks' settings arrive. Alpha (the temporal blend weight) stays, so the
    // later passes treat the stripes normally.
    if (frac(svPos.x / (DecodeSettings().on ? 128.0 : 32.0)) < 0.5)
        result.rgb = 0.0;
#endif
    return result;
}
