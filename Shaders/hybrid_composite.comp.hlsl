struct CompositeParamsUbo
{
    column_major float4x4 clipToWorldDirection;
    float4 cameraPosAndMaxRough; // xyz = cameraPos, w = reflectionMaxRoughness
    uint4  extentAndDebug;       // x = width, y = height, z = debugView, w = reflectionsEnabled
    uint4  flags;                // x = shadowsEnabled, y = hasEnv, z = denoiserEnabled, w = reserved
};

[[vk::binding(0, 0)]] ConstantBuffer<CompositeParamsUbo> g_params;
[[vk::binding(1, 0)]] Texture2D<float4> g_rt0AlbedoMetallic;
[[vk::binding(2, 0)]] Texture2D<float4> g_rt1NormalRoughness;
[[vk::binding(3, 0)]] Texture2D<float4> g_rt2EmissiveAo;
[[vk::binding(4, 0)]] Texture2D<float4> g_rt3WorldPosInst;
[[vk::binding(5, 0)]] Texture2D<float4> g_denoisedDirect;
[[vk::binding(6, 0)]] Texture2D<float4> g_denoisedRefl;
[[vk::binding(7, 0)]] Texture2D<float2> g_shadowAux;
[[vk::binding(8, 0)]] Texture2D<float2> g_reflAux;
[[vk::binding(9, 0)]] Texture2D<float2> g_varConf;
[[vk::image_format("rgba16f")]] [[vk::binding(10, 0)]] RWTexture2D<float4> g_compositeOut;

// Shared VulkanEnvironment Set 1
[[vk::binding(0, 1)]] TextureCube<float4> g_irradianceMap;
[[vk::binding(1, 1)]] TextureCube<float4> g_prefilteredMap;
[[vk::binding(2, 1)]] Texture2D<float4> g_brdfLut;
[[vk::binding(3, 1)]] SamplerState g_envSampler;
[[vk::binding(4, 1)]] TextureCube<float4> g_envCube;

float3 SafeNormal(float3 v)
{
    return v * rsqrt(max(dot(v, v), 1e-20));
}

float3 HeatMap(float t)
{
    float x = saturate(t);
    float3 c0 = float3(0.05, 0.08, 0.25);
    float3 c1 = float3(0.08, 0.65, 0.85);
    float3 c2 = float3(0.95, 0.85, 0.15);
    float3 c3 = float3(0.95, 0.18, 0.08);
    if (x < 0.33)
        return lerp(c0, c1, x / 0.33);
    if (x < 0.66)
        return lerp(c1, c2, (x - 0.33) / 0.33);
    return lerp(c2, c3, (x - 0.66) / 0.34);
}

[numthreads(8, 8, 1)]
void main(uint3 tid : SV_DispatchThreadID)
{
    const uint width = g_params.extentAndDebug.x;
    const uint height = g_params.extentAndDebug.y;
    if (tid.x >= width || tid.y >= height)
        return;

    const float2 uv = (float2(tid.xy) + 0.5) / float2(width, height);
    const float2 ndc = uv * 2.0 - 1.0;
    const float4 worldH = mul(g_params.clipToWorldDirection, float4(ndc, 1.0, 1.0));
    const float3 skyDir = SafeNormal(worldH.xyz / max(abs(worldH.w), 1e-6));

    const float4 rt3 = g_rt3WorldPosInst.Load(int3(tid.xy, 0));
    const uint debugView = g_params.extentAndDebug.z;

    if (rt3.w <= 0.5)
    {
        float3 skyColor = float3(0.015, 0.025, 0.05);
        if (debugView == 0u)
            skyColor = g_envCube.SampleLevel(g_envSampler, skyDir, 0.0).rgb;
        else if (debugView == 1u)
            skyColor = g_envCube.SampleLevel(g_envSampler, skyDir, 0.0).rgb * 0.15;
        g_compositeOut[tid.xy] = float4(skyColor, 1.0);
        return;
    }

    const float4 rt0 = g_rt0AlbedoMetallic.Load(int3(tid.xy, 0));
    const float4 rt1 = g_rt1NormalRoughness.Load(int3(tid.xy, 0));
    const float4 rt2 = g_rt2EmissiveAo.Load(int3(tid.xy, 0));
    const float4 directData = g_denoisedDirect.Load(int3(tid.xy, 0));
    const float4 reflData = g_denoisedRefl.Load(int3(tid.xy, 0));
    const float2 shadowAux = g_shadowAux.Load(int3(tid.xy, 0));
    const float2 reflAux = g_reflAux.Load(int3(tid.xy, 0));
    const float2 varConf = g_varConf.Load(int3(tid.xy, 0));

    const float3 albedo = rt0.rgb;
    const float metallic = saturate(rt0.a);
    const float3 N = SafeNormal(rt1.rgb);
    const float roughness = clamp(rt1.a, 0.02, 1.0);
    const float3 emissive = rt2.rgb;
    const float ao = saturate(rt2.a);
    const float3 worldPos = rt3.xyz;
    const float3 V = SafeNormal(g_params.cameraPosAndMaxRough.xyz - worldPos);

    const float nv = saturate(dot(N, V));
    const float3 f0 = lerp(0.04.xxx, albedo, metallic);
    const float2 ab = g_brdfLut.SampleLevel(g_envSampler, float2(nv, roughness), 0.0).rg;
    const float3 specWeight = f0 * ab.x + ab.y;
    const float3 diffWeight = (1.0 - saturate(specWeight)) * (1.0 - metallic);

    const float3 indirectDiffuse = g_irradianceMap.SampleLevel(g_envSampler, N, 0.0).rgb * albedo * diffWeight;
    const float3 indirectSpecIbl = g_prefilteredMap.SampleLevel(g_envSampler, reflect(-V, N), roughness * 7.0).rgb * specWeight;

    const bool reflectionsEnabled = (g_params.extentAndDebug.w != 0u);
    const float maxRough = max(g_params.cameraPosAndMaxRough.w, 0.05);
    const float rtReflBlend = reflectionsEnabled
        ? saturate(1.0 - smoothstep(maxRough * 0.78, maxRough, roughness))
        : 0.0;
    const float3 indirectSpecular = lerp(indirectSpecIbl, reflData.rgb, rtReflBlend);

    float3 outColor = float3(0.0, 0.0, 0.0);

    if (debugView == 0u) // FinalHybrid
    {
        outColor = directData.rgb + (indirectDiffuse + indirectSpecular) * ao + emissive;
    }
    else if (debugView == 1u) // RtShadowsOnly
    {
        outColor = directData.rgb + indirectDiffuse * (0.18 * ao) + emissive * 0.5;
    }
    else if (debugView == 2u) // RtReflectionsOnly
    {
        outColor = reflData.rgb * ao * rtReflBlend + emissive * 0.25;
    }
    else if (debugView == 3u) // ShadowVisibility
    {
        float v = saturate(directData.a);
        outColor = lerp(float3(0.04, 0.06, 0.14), float3(1.15, 1.15, 1.15), v);
    }
    else if (debugView == 4u) // ShadowSampleCount
    {
        outColor = HeatMap(shadowAux.y);
    }
    else if (debugView == 5u) // ReflectionContribution
    {
        outColor = reflData.rgb * rtReflBlend;
    }
    else if (debugView == 6u) // ReflectionHitMiss
    {
        if (roughness > maxRough)
            outColor = float3(0.08, 0.08, 0.10);
        else if (reflAux.x > 0.5)
            outColor = float3(0.10, 0.88, 0.34);
        else
            outColor = float3(0.10, 0.30, 0.90);
    }
    else if (debugView == 7u) // ReflectionHitDistance
    {
        if (reflAux.x > 0.5)
            outColor = HeatMap(1.0 - saturate(reflAux.y / 12.0));
        else
            outColor = float3(0.02, 0.03, 0.06);
    }
    else if (debugView == 8u) // SurfaceRoughness
    {
        outColor = roughness.xxx;
    }
    else if (debugView == 9u) // TemporalConfidence
    {
        float c = saturate(varConf.y);
        outColor = lerp(float3(0.95, 0.12, 0.10), float3(0.12, 0.92, 0.32), c);
    }
    else if (debugView == 10u) // DenoiserVariance
    {
        outColor = HeatMap(saturate(sqrt(max(varConf.x, 0.0)) * 0.65));
    }

    g_compositeOut[tid.xy] = float4(outColor, 1.0);
}
