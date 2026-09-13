Texture2D desktopTex : register(t0);
SamplerState samp : register(s0);
cbuffer SceneCB : register(b1)
{
    float4 fadeParams;
};

struct VSOut
{
    float4 pos : SV_POSITION;
    float2 uv : TEXCOORD0;
};

float4 main(VSOut i) : SV_TARGET
{
    float4 c = desktopTex.Sample(samp, i.uv);
    float fade = 1.0 - fadeParams.z * smoothstep(fadeParams.x, fadeParams.y, 1.0 - i.uv.y);
    return float4(c.rgb * fade, c.a);
}
