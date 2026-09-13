Texture2D displayTex : register(t0);
SamplerState borderSamp : register(s0); // must be created with D3D11_FILTER_MIN_MAG_MIP_LINEAR (or equivalent trilinear filter) on the CPU side, and displayTex must have a full mip chain generated (GenerateMips) before this shader reads it

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
    float2 sampleUv = float2(u, sampleV);

    float lidDisplayDistance = abs(dot(i.lidWorld - dispOrigin.xyz, dispNormal.xyz));
    float radiusPx = min(max(lidDisplayDistance * blurMetrics.z, 0.0), 256.0);

    uint textureWidth, textureHeight, mipLevels;
    displayTex.GetDimensions(0, textureWidth, textureHeight, mipLevels);

    float lodScale = 0.9;
    float lod = log2(max(radiusPx, 1.0)) * lodScale;
    lod = clamp(lod, 0.0, float(mipLevels - 1));

    float4 color;
    if (radiusPx > 0.5)
        color = displayTex.SampleLevel(borderSamp, sampleUv, lod);
    else
        color = displayTex.SampleLevel(borderSamp, sampleUv, 0.0);

    float fade = 1.0 - effectMetrics.x * smoothstep(0.2, 1.0, v);
    return float4(color.rgb * fade, color.a);
}