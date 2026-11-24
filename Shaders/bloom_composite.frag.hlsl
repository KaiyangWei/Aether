[[vk::binding(0, 0)]] Texture2D<float4> srcTex0; // Original linear HDR scene
[[vk::binding(1, 0)]] Texture2D<float4> srcTex1; // Reconstructed Bloom or debug intermediate
[[vk::binding(2, 0)]] SamplerState linearSampler;

struct Push
{
    float intensity;
    uint debugMode;
    float pad0;
    float pad1;
};

[[vk::push_constant]] Push params;

struct Input
{
    [[vk::location(0)]] float2 uv : TEXCOORD0;
};

float3 SafeHdr(float3 c)
{
    return min(max(select(isfinite(c), c, 0.0.xxx), 0.0.xxx), 65000.0.xxx);
}

float4 main(Input input) : SV_Target0
{
    const float3 bloom = SafeHdr(srcTex1.SampleLevel(linearSampler, input.uv, 0).rgb);
    if (params.debugMode >= 1u && params.debugMode <= 3u)
        return float4(bloom, 1.0);

    const float3 orig = SafeHdr(srcTex0.SampleLevel(linearSampler, input.uv, 0).rgb);
    return float4(min(orig + bloom * params.intensity, 65000.0.xxx), 1.0);
}
