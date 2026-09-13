Texture2D displayTex : register(t0);
SamplerState borderSamp : register(s0);

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

float4 main(VSOut i) : SV_TARGET
{
    float3 ray = normalize(i.lidWorld - headPos.xyz);
    float denominator = dot(dispNormal.xyz, ray);
    if (abs(denominator) < 0.00001)
        return float4(0.0, 0.0, 0.0, 1.0);

    float tHit = dot(dispNormal.xyz, dispOrigin.xyz - headPos.xyz) / denominator;
    if (tHit < 0.0)
        return float4(0.0, 0.0, 0.0, 1.0);

    float3 sourcePoint = headPos.xyz + tHit * ray;
    float3 local = sourcePoint - dispOrigin.xyz;
    float u = dot(local, dispRightAxis.xyz) / displayMetrics.x;
    float v = dot(local, dispUpAxis.xyz) / displayMetrics.y;

    if (u < 0.0 || u > 1.0 || v < 0.0 || v > 1.0)
        return float4(0.0, 0.0, 0.0, 1.0);

    float sampleV = 1.0 - v;

    float blurT = saturate((tHit - blurMetrics.x) / max(blurMetrics.y - blurMetrics.x, 0.0001));
    float radiusPx = blurT * blurMetrics.z;
    uint textureWidth;
    uint textureHeight;
    displayTex.GetDimensions(textureWidth, textureHeight);
    float2 texel = radiusPx / float2(textureWidth, textureHeight);
    float4 color = displayTex.Sample(borderSamp, float2(u, sampleV));
    if (radiusPx > 0.5)
    {
        float4 sum = color;
        const int taps = 4;
        [unroll]
        for (int tap = 1; tap <= taps; ++tap)
        {
            float weight = (float)tap / (float)taps;
            sum += displayTex.Sample(borderSamp, float2(u, sampleV) + float2(texel.x, 0.0) * weight);
            sum += displayTex.Sample(borderSamp, float2(u, sampleV) - float2(texel.x, 0.0) * weight);
            sum += displayTex.Sample(borderSamp, float2(u, sampleV) + float2(0.0, texel.y) * weight);
            sum += displayTex.Sample(borderSamp, float2(u, sampleV) - float2(0.0, texel.y) * weight);
        }
        color = sum / (1.0 + 4.0 * taps);
    }

    float fade = 1.0 - effectMetrics.x * smoothstep(0.2, 1.0, sampleV);
    return float4(color.rgb * fade, color.a);
}