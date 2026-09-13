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
    float sigma = max(0.001, lerp(ranges.x, ranges.y, 1.0 - i.uv.y));
    int halfK = min(32, (int)(sigma * 2.5 + 0.5));
    float wsum = 0.0;
    float4 c = 0.0;
    [loop]
    for (int k = -halfK; k <= halfK; ++k)
    {
        float w = exp(-((float)k * (float)k) / (2.0 * sigma * sigma));
        c += sceneTex.Sample(samp, i.uv + dir * (float)k) * w;
        wsum += w;
    }
    return c / wsum;
}
