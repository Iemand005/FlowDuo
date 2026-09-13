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

    float sampleV = 1.0 - v;

    float lidDisplayDistance = abs(dot(i.lidWorld - dispOrigin.xyz, dispNormal.xyz));
    float radiusPx = min(max(lidDisplayDistance * blurMetrics.z, 0.0), 256.0);
    uint textureWidth;
    uint textureHeight;
    displayTex.GetDimensions(textureWidth, textureHeight);
    float2 texel = 1.0 / float2(textureWidth, textureHeight);
    float2 sampleUv = float2(u, sampleV);
    float4 color = displayTex.Sample(borderSamp, sampleUv);
    if (radiusPx > 0.5)
    {
        const int kernelRadius = 4;
        float sigma = max(radiusPx * 0.5, 0.75);
        float inverseTwoSigmaSquared = 0.5 / (sigma * sigma);
        float2 kernelStep = texel * (radiusPx / (float)kernelRadius);
        float4 sum = float4(0.0, 0.0, 0.0, 0.0);
        float weightSum = 0.0;
        [unroll]
        for (int y = -kernelRadius; y <= kernelRadius; ++y)
        {
            [unroll]
            for (int x = -kernelRadius; x <= kernelRadius; ++x)
            {
                float2 offset = float2((float)x, (float)y) * kernelStep;
                float distanceSquared = (float)(x * x + y * y) * (radiusPx / (float)kernelRadius) * (radiusPx / (float)kernelRadius);
                float weight = exp(-distanceSquared * inverseTwoSigmaSquared);
                sum += displayTex.Sample(borderSamp, sampleUv + offset) * weight;
                weightSum += weight;
            }
        }
        color = sum / weightSum;
    }

    float fade = 1.0 - effectMetrics.x * smoothstep(0.2, 1.0, v);
    return float4(color.rgb * fade, color.a);
}