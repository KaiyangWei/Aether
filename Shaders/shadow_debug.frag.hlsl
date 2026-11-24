[[vk::binding(1, 0)]] Texture2D<float> shadowMap;
[[vk::binding(3, 0)]] SamplerState debugSampler;

struct Input
{
    [[vk::location(0)]] float2 uv : TEXCOORD0;
};

float4 main(Input input) : SV_Target0
{
    const float depth = shadowMap.SampleLevel(debugSampler, input.uv, 0);
    return float4(depth.xxx, 1.0);
}
