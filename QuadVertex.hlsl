cbuffer TransformCB : register(b0)
{
    float4x4 transform;
};

struct VSIn
{
    float3 pos : POSITION;
    float2 uv : TEXCOORD0;
    float blur : TEXCOORD1;
};

struct VSOut
{
    float4 pos : SV_POSITION;
    float2 uv : TEXCOORD0;
    float blur : TEXCOORD1;
};

VSOut main(VSIn i)
{
    VSOut o;
    o.pos = mul(float4(i.pos, 1.0), transform);
    o.uv = i.uv;
    o.blur = i.blur;
    return o;
}