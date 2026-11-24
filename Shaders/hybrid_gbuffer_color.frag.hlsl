struct Input
{
    [[vk::location(0)]] float3 position : TEXCOORD0;
    [[vk::location(1)]] float3 normal : TEXCOORD1;
    [[vk::location(2)]] float4 tangent : TEXCOORD2;
    [[vk::location(3)]] float4 color : TEXCOORD3;
    [[vk::location(4)]] float4 colorAndMrUv : TEXCOORD4;
    [[vk::location(5)]] float4 normalAndAoUv : TEXCOORD5;
    [[vk::location(6)]] float2 emissiveUv : TEXCOORD6;
    [[vk::location(7)]] nointerpolation float4 eyeAndInstanceId : TEXCOORD7;
    bool frontFace : SV_IsFrontFace;
};

struct GBufferOutput
{
    float4 rt0AlbedoMetallic : SV_Target0;
    float4 rt1NormalRoughness : SV_Target1;
    float4 rt2EmissiveAo : SV_Target2;
    float4 rt3WorldPosInst : SV_Target3;
};

float3 SafeNormal(float3 v)
{
    return v * rsqrt(max(dot(v, v), 1e-20));
}

GBufferOutput main(Input input)
{
    const float3 px = ddx(input.position);
    const float3 py = ddy(input.position);
    const float3 geometric = SafeNormal(cross(px, py));
    float3 n = SafeNormal(input.normal);
    if (dot(n, n) < 0.5)
        n = geometric;
    if (!input.frontFace)
        n = -n;

    GBufferOutput output;
    output.rt0AlbedoMetallic = float4(input.color.rgb, 0.0);
    output.rt1NormalRoughness = float4(n, 0.55);
    output.rt2EmissiveAo = float4(0.0, 0.0, 0.0, 1.0);
    output.rt3WorldPosInst = float4(input.position, input.eyeAndInstanceId.w);
    return output;
}
