[[vk::binding(3, 0)]] SamplerState environmentSampler;
[[vk::binding(4, 0)]] TextureCube<float4> environmentCube;

float4 main([[vk::location(0)]] float3 direction : TEXCOORD0) : SV_Target
{
    return float4(environmentCube.SampleLevel(environmentSampler, normalize(direction), 0.0).rgb, 1.0);
}
