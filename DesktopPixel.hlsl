Texture2D desktopTex : register(t0);
SamplerState samp : register(s0);
cbuffer SceneCB : register(b1)
{
    float4 headPosition;
    float4 virtualBottomLeft;
    float4 virtualNormal;
    float4 virtualXAndWidth;
    float4 virtualYAndHeight;
    float4 fadeParams;
};

struct VSOut
{
    float4 pos : SV_POSITION;
    float3 worldPosition : TEXCOORD1;
};

float4 main(VSOut i) : SV_TARGET
{
    float3 ray = i.worldPosition - headPosition.xyz;
    float denominator = dot(ray, virtualNormal.xyz);
    if (abs(denominator) < 0.00001)
        return float4(0.0, 0.0, 0.0, 0.0);
    float rayDistance = dot(virtualBottomLeft.xyz - headPosition.xyz, virtualNormal.xyz) / denominator;
    float3 sourcePoint = headPosition.xyz + ray * rayDistance;
    float2 uv;
    uv.x = dot(sourcePoint - virtualBottomLeft.xyz, virtualXAndWidth.xyz) / virtualXAndWidth.w;
    uv.y = dot(sourcePoint - virtualBottomLeft.xyz, virtualYAndHeight.xyz) / virtualYAndHeight.w;

    float4 c = desktopTex.Sample(samp, uv);
    float fade = 1.0 - fadeParams.z * smoothstep(fadeParams.x, fadeParams.y, 1.0 - uv.y);
    return float4(c.rgb * fade, c.a);
}
