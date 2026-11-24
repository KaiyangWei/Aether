struct Material
{
    float4 baseColor;
    float4 emissiveMetallic;
    float4 surface;
    float4 flags;
};

[[vk::binding(0, 0)]] ConstantBuffer<Material> material;
[[vk::binding(1, 0)]] Texture2D<float4> baseColorMap;
[[vk::binding(6, 0)]] SamplerState baseColorSampler;

struct Input
{
    [[vk::location(0)]] float4 currClip : TEXCOORD0;
    [[vk::location(1)]] float4 prevClip : TEXCOORD1;
    [[vk::location(2)]] float2 uv : TEXCOORD2;
    [[vk::location(3)]] float alpha : COLOR0;
};

float4 main(Input input) : SV_Target0
{
    const float alpha = material.baseColor.a * input.alpha * baseColorMap.Sample(baseColorSampler, input.uv).a;
    clip(alpha - material.surface.w);

    if (input.currClip.w <= 1e-6 || input.prevClip.w <= 1e-6)
        return 0.0.xxxx;
    const float2 currUv = (input.currClip.xy / input.currClip.w) * 0.5 + 0.5;
    const float2 prevUv = (input.prevClip.xy / input.prevClip.w) * 0.5 + 0.5;
    const float prevDepth = input.prevClip.z / input.prevClip.w;
    return float4(prevUv - currUv, prevDepth, 1.0);
}
