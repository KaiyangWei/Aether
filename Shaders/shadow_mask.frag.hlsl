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
    [[vk::location(0)]] float2 uv : TEXCOORD0;
    [[vk::location(1)]] float alpha : COLOR0;
};

void main(Input input)
{
    const float alpha = material.baseColor.a * input.alpha * baseColorMap.Sample(baseColorSampler, input.uv).a;
    clip(alpha - material.surface.w);
}
