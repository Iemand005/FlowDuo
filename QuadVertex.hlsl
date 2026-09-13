cbuffer SceneCB : register(b0)
{
    float4 headPos;
    float4 dispOrigin;
    float4 dispRightAxis;
    float4 dispUpAxis;
    float4 dispNormal;
    float4 lidBL;
    float4 lidBR;
    float4 lidTL;
    float4 lidTR;
    float4 displayMetrics;
    float4 blurMetrics;
    float4 effectMetrics;
};

struct VSOut
{
    float4 pos : SV_POSITION;
    float2 st : TEXCOORD0;
    float3 lidWorld : TEXCOORD1;
};

VSOut main(uint vertexId : SV_VertexID)
{
    VSOut o;
    float2 st = float2(vertexId & 1, vertexId >> 1);
    o.pos = float4(st * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
    float3 bottom = lerp(lidBL.xyz, lidBR.xyz, st.x);
    float3 top = lerp(lidTL.xyz, lidTR.xyz, st.x);
    o.st = st;
    o.lidWorld = lerp(bottom, top, st.y);
    return o;
}