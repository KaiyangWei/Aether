struct Material
{
    float4 baseColor;
    float4 emissiveMetallic;
    float4 surface;
    float4 flags;
};

[[vk::binding(0, 0)]] ConstantBuffer<Material> material;
[[vk::binding(1, 0)]] Texture2D<float4> baseColorMap;
[[vk::binding(2, 0)]] Texture2D<float4> metallicRoughnessMap;
[[vk::binding(3, 0)]] Texture2D<float4> normalMap;
[[vk::binding(4, 0)]] Texture2D<float4> occlusionMap;
[[vk::binding(5, 0)]] Texture2D<float4> emissiveMap;
[[vk::binding(6, 0)]] SamplerState baseColorSampler;
[[vk::binding(7, 0)]] SamplerState metallicRoughnessSampler;
[[vk::binding(8, 0)]] SamplerState normalSampler;
[[vk::binding(9, 0)]] SamplerState occlusionSampler;
[[vk::binding(10, 0)]] SamplerState emissiveSampler;

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
    float4 rt0AlbedoMetallic : SV_Target0;   // rgb = base albedo, a = metallic
    float4 rt1NormalRoughness : SV_Target1;  // rgb = shading normal, a = roughness
    float4 rt2EmissiveAo : SV_Target2;       // rgb = emissive, a = ambient occlusion
    float4 rt3WorldPosInst : SV_Target3;     // rgb = world position, a = float(rtInstanceIndex + 1)
};

float3 SafeNormal(float3 v)
{
    return v * rsqrt(max(dot(v, v), 1e-20));
}

float3 SurfaceNormal(Input input, float3 view, float3 px, float3 py, float2 ux, float2 uy)
{
    const float3 geometric = SafeNormal(cross(px, py));
    float3 n = SafeNormal(input.normal);
    if (dot(n, n) < 0.5)
    {
        n = dot(geometric, view) < 0 ? -geometric : geometric;
        if (!input.frontFace)
            n = -n;
    }

    if (material.flags.y > 0.5)
    {
        float3 t = input.tangent.xyz;
        float handedness = input.tangent.w;
        if (dot(t, t) < 1e-16)
        {
            const float det = ux.x * uy.y - ux.y * uy.x;
            if (abs(det) > 1e-12)
            {
                t = (px * uy.y - py * ux.y) / det;
                const float3 b = (py * ux.x - px * uy.x) / det;
                handedness = dot(cross(n, t), b) < 0 ? -1.0 : 1.0;
            }
        }
        t = SafeNormal(t - n * dot(n, t));
        if (dot(t, t) > 0.5)
        {
            const float3 b = cross(n, t) * handedness;
            float3 sampled = normalMap.SampleGrad(normalSampler, input.normalAndAoUv.xy, ux, uy).xyz * 2.0 - 1.0;
            sampled.xy *= material.surface.y;
            n = SafeNormal(t * sampled.x + b * sampled.y + n * sampled.z);
        }
    }
    return input.frontFace ? n : -n;
}

GBufferOutput main(Input input)
{
    const float3 px = ddx(input.position);
    const float3 py = ddy(input.position);
    const float2 ux = ddx(input.normalAndAoUv.xy);
    const float2 uy = ddy(input.normalAndAoUv.xy);

    const float4 base = material.baseColor * input.color *
        baseColorMap.Sample(baseColorSampler, input.colorAndMrUv.xy);
    if (material.flags.x == 1.0)
        clip(base.a - material.surface.w);

    const float3 mr = metallicRoughnessMap.Sample(metallicRoughnessSampler, input.colorAndMrUv.zw).rgb;
    const float metallic = saturate(material.emissiveMetallic.w * mr.b);
    // Preserve mirror smoothness (down to 0.02) for Cornell Box mirror panels and chrome hero spheres.
    const float roughness = clamp(material.surface.x * mr.g, 0.02, 1.0);
    const float ao = lerp(1.0, occlusionMap.Sample(occlusionSampler, input.normalAndAoUv.zw).r, material.surface.z);
    const float3 emissive = material.emissiveMetallic.xyz * emissiveMap.Sample(emissiveSampler, input.emissiveUv).rgb;
    const float3 view = SafeNormal(input.eyeAndInstanceId.xyz - input.position);
    const float3 n = SurfaceNormal(input, view, px, py, ux, uy);

    GBufferOutput output;
    output.rt0AlbedoMetallic = float4(base.rgb, metallic);
    output.rt1NormalRoughness = float4(n, roughness);
    output.rt2EmissiveAo = float4(emissive, ao);
    output.rt3WorldPosInst = float4(input.position, input.eyeAndInstanceId.w);
    return output;
}
