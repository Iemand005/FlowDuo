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
        static const int SAMPLE_COUNT = 28;
        static const float GOLDEN_ANGLE = 2.39996323; // radians (~137.5 degrees)

        float2 blurStep = texel * radiusPx;
        float sigma = 0.45; // 0.3 = tighter/sharper falloff, 0.6 = softer/more spread
        float4 sum = color;
        float weightSum = 1.0;

        [unroll]
        for (int tap = 0; tap < SAMPLE_COUNT; ++tap)
        {
            float t = (tap + 0.5) / SAMPLE_COUNT;
            float r = sqrt(t); // sqrt gives uniform area coverage across the disk
            float theta = tap * GOLDEN_ANGLE;
            float2 offset = float2(cos(theta), sin(theta)) * r;

            float weight = exp(-(r * r) / (2.0 * sigma * sigma));
            sum += displayTex.Sample(borderSamp, sampleUv + offset * blurStep) * weight;
            weightSum += weight;
        }

        color = sum / weightSum;
    }

    float fade = 1.0 - effectMetrics.x * smoothstep(0.2, 1.0, v);
    return float4(color.rgb * fade, color.a);
}