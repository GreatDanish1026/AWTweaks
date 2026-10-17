// Diagnostic stand-in for SSSSS.rfx "sss_blurPS" (Jimenez's separable subsurface
// scattering blur, run once per direction). Same bindings and constant layout as the
// original. Where the specular buffer's .w (view depth, written by the skin pass) marks
// skin, it draws 32-pixel stripes: green ones show where the blur would run, red ones
// grow with g_fSSSWidth (full red at 0.01 m). Everything else passes through unblurred.

cbuffer Globals : register(b0)
{
    float g_fInvNear : packoffset(c25.x);
    float g_fFar : packoffset(c25.y);
    float2 g_vOutputRes : packoffset(c28.z);
    float2 g_vBlurDir : packoffset(c29.x);
    float3 g_vNearFarFOV : packoffset(c30.x);
    float g_fSSSWidth : packoffset(c30.w);
    float g_fFollowSurfaceScale : packoffset(c31.x);
    float g_fFollowSurface : packoffset(c31.y);
};

Texture2D<float4> g_sSourceBufferPointTex : register(t1);
SamplerState g_sSourceBufferPoint : register(s1);
Texture2D<float4> g_sSpecularBufferPointTex : register(t3);
SamplerState g_sSpecularBufferPoint : register(s3);

float4 main(float4 svPos : SV_Position, float2 uv : TEXCOORD0) : SV_Target
{
    float3 color = g_sSourceBufferPointTex.Sample(g_sSourceBufferPoint, uv).rgb;
    float depth = g_sSpecularBufferPointTex.Sample(g_sSpecularBufferPoint, uv).w;
    if (depth > 0.0)
    {
        // Both passes run, so each tints half as much.
        if (frac(svPos.x / 64.0) < 0.5)
            color = lerp(color, float3(0.0, 1.0, 0.0), 0.3);
        else
            color = lerp(color, float3(1.0, 0.0, 0.0), 0.5 * saturate(g_fSSSWidth / 0.01));
    }
    return float4(color, 1.0);
}
