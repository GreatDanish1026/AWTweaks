// Full-screen triangle for shader_compare --volumetric, with the outputs of the game's
// VolumetricLight vertex shader: uv in TEXCOORD0 and a view-space ray in TEXCOORD1
// (here a 90 x 90 degree frustum, z = 1).
struct VSOut
{
    float4 pos : SV_Position;
    float2 uv : TEXCOORD0;
    float3 viewRay : TEXCOORD1;
};

VSOut main(uint id : SV_VertexID)
{
    float2 uv = float2((id << 1) & 2, id & 2);
    float2 ndc = uv * float2(2, -2) + float2(-1, 1);
    VSOut o;
    o.pos = float4(ndc, 0, 1);
    o.uv = uv;
    o.viewRay = float3(ndc, 1.0);
    return o;
}
