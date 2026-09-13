Texture2D sceneTex : register(t0);
SamplerState samp : register(s0);
cbuffer BlurCB : register(b1)
{
    float4 texelDir;
    float4 ranges;
};

struct VSOut
{
    float4 pos : SV_POSITION;
    float2 uv : TEXCOORD0;
};

float4 main(VSOut i) : SV_TARGET
{
    float2 texel = float2(texelDir.x, texelDir.y);
    float2 dir = lerp(float2(texel.x, 0.0), float2(0.0, texel.y), texelDir.z);
    float radius = max(0.5, lerp(ranges.x, ranges.y, 1.0 - i.uv.y));
    float2 stepDir = dir * radius * 0.25;
    float4 c = sceneTex.Sample(samp, i.uv) * 0.227027;
    c += (sceneTex.Sample(samp, i.uv + stepDir) + sceneTex.Sample(samp, i.uv - stepDir)) * 0.1945946;
    c += (sceneTex.Sample(samp, i.uv + stepDir * 2.0) + sceneTex.Sample(samp, i.uv - stepDir * 2.0)) * 0.1216216;
    c += (sceneTex.Sample(samp, i.uv + stepDir * 3.0) + sceneTex.Sample(samp, i.uv - stepDir * 3.0)) * 0.054054;
    c += (sceneTex.Sample(samp, i.uv + stepDir * 4.0) + sceneTex.Sample(samp, i.uv - stepDir * 4.0)) * 0.016216;
    return c;
}
