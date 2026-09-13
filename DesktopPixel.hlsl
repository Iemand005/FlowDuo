Texture2D desktopTex : register(t0);
SamplerState samp : register(s0);
cbuffer SceneCB : register(b1)
{
    float4 fadeParams;
    float4 blurParams;
};

struct VSOut
{
    float4 pos : SV_POSITION;
    float2 uv : TEXCOORD0;
    float blur : TEXCOORD1;
};

float4 main(VSOut i) : SV_TARGET
{
    float2 uv = i.uv;
    float radius = i.blur * blurParams.x;

    const int lobes = 4;
    const int dirs = 8;
    float4 sum = desktopTex.Sample(samp, uv);
    float weight = 1.0;
    for (int l = 1; l <= lobes; ++l)
    {
        float d = (float)l / (float)lobes * radius;
        for (int k = 0; k < dirs; ++k)
        {
            float ang = 6.28318530718f / (float)dirs * (float)k;
            sum += desktopTex.Sample(samp, uv + float2(cos(ang), sin(ang)) * d);
            weight += 1.0;
        }
    }

    float4 c = sum / weight;
    float fade = 1.0 - fadeParams.z * smoothstep(fadeParams.x, fadeParams.y, uv.y);
    return float4(c.rgb * fade, c.a);
}