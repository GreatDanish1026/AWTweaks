// Full-screen triangle for test/shader_compare: SV_Position plus a view ray in TEXCOORD0
// scaled so depth (0..1) * ray * g_fInvNear (5) gives view space with z = depth * 2000 m.
struct VSOut
{
    float4 pos : SV_Position;
    float4 viewRay : TEXCOORD0;
};

VSOut main(uint id : SV_VertexID)
{
    float2 uv = float2((id << 1) & 2, id & 2);
    float2 ndc = uv * float2(2, -2) + float2(-1, 1);
    VSOut o;
    o.pos = float4(ndc, 0, 1);
    o.viewRay = float4(ndc * 0.7 * 400.0, 400.0, 0);
    return o;
}
