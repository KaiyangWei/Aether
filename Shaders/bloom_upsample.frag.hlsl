[[vk::binding(0, 0)]] Texture2D<float4> srcTex0; // Smaller pyramid level
[[vk::binding(1, 0)]] Texture2D<float4> srcTex1; // Current level (when combineBase > 0.5)
[[vk::binding(2, 0)]] SamplerState linearSampler;

struct Push
{
    float2 invSmallExtent;
    float scatter;
    float combineBase;
};

[[vk::push_constant]] Push params;

struct Input
{
    [[vk::location(0)]] float2 uv : TEXCOORD0;
};

float3 SampleSafe(Texture2D<float4> tex, float2 uv)
{
    const float3 c = tex.SampleLevel(linearSampler, uv, 0).rgb;
    return min(max(select(isfinite(c), c, 0.0.xxx), 0.0.xxx), 65000.0.xxx);
}

// 9-tap 3x3 tent upsample filter with controlled energy reconstruction.
float4 main(Input input) : SV_Target0
{
    const float2 d = params.invSmallExtent;
    const float2 uv = input.uv;

    float3 tent = SampleSafe(srcTex0, uv) * 4.0;
    tent += SampleSafe(srcTex0, uv + float2(-d.x,  0.0)) * 2.0;
    tent += SampleSafe(srcTex0, uv + float2( d.x,  0.0)) * 2.0;
    tent += SampleSafe(srcTex0, uv + float2( 0.0, -d.y)) * 2.0;
    tent += SampleSafe(srcTex0, uv + float2( 0.0,  d.y)) * 2.0;
    tent += SampleSafe(srcTex0, uv + float2(-d.x, -d.y));
    tent += SampleSafe(srcTex0, uv + float2( d.x, -d.y));
    tent += SampleSafe(srcTex0, uv + float2(-d.x,  d.y));
    tent += SampleSafe(srcTex0, uv + float2( d.x,  d.y));
    tent *= (1.0 / 16.0);

    if (params.combineBase > 0.5)
    {
        const float3 base = SampleSafe(srcTex1, uv);
        const float w = params.scatter;
        return float4((base + tent * w) / (1.0 + 0.5 * w), 1.0);
    }
    return float4(tent, 1.0);
}
