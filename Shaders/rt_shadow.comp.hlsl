struct GpuLight
{
    float4 posAndType;         // xyz = world position, w = float(RtLightType)
    float4 dirAndRange;        // xyz = emit/toLight dir, w = range
    float4 colorAndIntensity;  // rgb = color, w = intensity
    float4 areaRightAndHalfW;  // xyz = right axis, w = halfW or cosOuter
    float4 areaUpAndHalfH;     // xyz = up axis, w = halfH or cosInner
};

struct RtGpuVertex
{
    float4 posAndU;
    float4 normalAndV;
};

struct RtGpuInstanceData
{
    float4 baseColorAndAlphaCutoff;
    float4 emissiveAndMetallic;
    float4 surfaceParams; // x = roughness, y = ao, z = baseAlpha, w = flags
    float4 modelRows[3];
    float4 prevModelRows[3];
    float4 invModelRows[3];
    uint   firstVertex;
    uint   firstIndex;
    uint   alphaMaskTexIndex;
    uint   sceneObjectIndex;
};

struct ShadowParamsUbo
{
    float4 cameraPosAndHasTlas;    // xyz = cameraPos, w = hasTlas
    float4 sunDirAndAngularRadius; // xyz = normalized dir TO sun, w = sunAngularRadiusRad
    float4 sunColorAndEnabled;     // rgb = sun radiance, w = sunLightEnabled
    float4 biasAndFlags;           // x = normalBias, y = distanceBias, z = tMin, w = maxShadowT
    uint4  countsAndExtent;        // x = width, y = height, z = shadowSamples, w = frameIndex
    uint4  featureFlags;           // x = shadowsEnabled, y = areaLightsEnabled, z = lightCount, w = fillEnabled
    GpuLight lights[8];
};

[[vk::binding(0, 0)]] ConstantBuffer<ShadowParamsUbo> g_params;
[[vk::binding(1, 0)]] RaytracingAccelerationStructure g_tlas;
[[vk::binding(2, 0)]] Texture2D<float4> g_rt0AlbedoMetallic;
[[vk::binding(3, 0)]] Texture2D<float4> g_rt1NormalRoughness;
[[vk::binding(4, 0)]] Texture2D<float4> g_rt2EmissiveAo;
[[vk::binding(5, 0)]] Texture2D<float4> g_rt3WorldPosInst;
[[vk::binding(6, 0)]] StructuredBuffer<RtGpuInstanceData> g_instances;
[[vk::binding(7, 0)]] StructuredBuffer<RtGpuVertex> g_vertices;
[[vk::binding(8, 0)]] StructuredBuffer<uint> g_indices;
[[vk::binding(9, 0)]] Texture2D<float4> g_alphaMasks[8];
[[vk::binding(10, 0)]] SamplerState g_alphaSamplers[8];
[[vk::image_format("rgba16f")]] [[vk::binding(11, 0)]] RWTexture2D<float4> g_directLightOut;
[[vk::image_format("rg16f")]] [[vk::binding(12, 0)]] RWTexture2D<float2> g_shadowAuxOut;

static const float kPi = 3.14159265359;

float3 SafeNormal(float3 v)
{
    return v * rsqrt(max(dot(v, v), 1e-20));
}

float InterleavedGradientNoise(float2 pixel, uint frameIndex)
{
    float2 p = pixel + float(frameIndex & 63u) * float2(5.588238, 17.132741);
    return frac(52.9829189 * frac(dot(p, float2(0.06711056, 0.00583715))));
}

float2 R2Sequence(uint index)
{
    return frac(float2(0.7548776662, 0.5698402910) * float(index + 1u));
}

float4 SampleAlphaMaskConst(uint idx, float2 uv)
{
    switch (idx)
    {
    case 0u: return g_alphaMasks[0].SampleLevel(g_alphaSamplers[0], uv, 0.0);
    case 1u: return g_alphaMasks[1].SampleLevel(g_alphaSamplers[1], uv, 0.0);
    case 2u: return g_alphaMasks[2].SampleLevel(g_alphaSamplers[2], uv, 0.0);
    case 3u: return g_alphaMasks[3].SampleLevel(g_alphaSamplers[3], uv, 0.0);
    case 4u: return g_alphaMasks[4].SampleLevel(g_alphaSamplers[4], uv, 0.0);
    case 5u: return g_alphaMasks[5].SampleLevel(g_alphaSamplers[5], uv, 0.0);
    case 6u: return g_alphaMasks[6].SampleLevel(g_alphaSamplers[6], uv, 0.0);
    case 7u: return g_alphaMasks[7].SampleLevel(g_alphaSamplers[7], uv, 0.0);
    default: return float4(1.0, 1.0, 1.0, 1.0);
    }
}

void BuildOrthonormalBasis(float3 n, out float3 t, out float3 b)
{
    float3 up = abs(n.z) < 0.999 ? float3(0.0, 0.0, 1.0) : float3(1.0, 0.0, 0.0);
    t = SafeNormal(cross(up, n));
    b = cross(n, t);
}

float SmithG(float nDotX, float alpha2)
{
    return 2.0 * nDotX / max(nDotX + sqrt(alpha2 + (1.0 - alpha2) * nDotX * nDotX), 1e-5);
}

float3 EvaluateDirectBrdf(float3 n, float3 v, float3 l, float3 baseColor, float metallic, float roughness)
{
    float nl = saturate(dot(n, l));
    if (nl <= 0.0)
        return float3(0.0, 0.0, 0.0);

    float nv = max(saturate(dot(n, v)), 1e-4);
    float3 h = SafeNormal(v + l);
    float nh = saturate(dot(n, h));
    float vh = saturate(dot(v, h));

    float alpha = max(roughness * roughness, 0.0009);
    float alpha2 = alpha * alpha;
    float term = nh * nh * (alpha2 - 1.0) + 1.0;
    float d = alpha2 / max(kPi * term * term, 1e-6);
    float g = SmithG(nl, alpha2) * SmithG(nv, alpha2);

    float3 f0 = lerp(0.04.xxx, baseColor, metallic);
    float3 f = f0 + (1.0 - f0) * pow(1.0 - vh, 5.0);
    float3 spec = (d * g * f) / max(4.0 * nl * nv, 1e-5);
    float3 diff = (1.0 - f) * (1.0 - metallic) * baseColor / kPi;
    return (diff + spec) * nl;
}

float TraceShadowRay(float3 origin, float3 dir, float tMin, float tMax)
{
    if (g_params.cameraPosAndHasTlas.w < 0.5 || tMax <= tMin)
        return 1.0;

    RayDesc ray;
    ray.Origin = origin;
    ray.Direction = dir;
    ray.TMin = tMin;
    ray.TMax = tMax;

    const uint flags = RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH | RAY_FLAG_SKIP_CLOSEST_HIT_SHADER;
    RayQuery<RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH | RAY_FLAG_SKIP_CLOSEST_HIT_SHADER> q;
    q.TraceRayInline(g_tlas, flags, 0xFF, ray);

    while (q.Proceed())
    {
        if (q.CandidateType() == CANDIDATE_NON_OPAQUE_TRIANGLE)
        {
            uint instId = q.CandidateInstanceID();
            uint primId = q.CandidatePrimitiveIndex();
            RtGpuInstanceData inst = g_instances[instId];

            uint i0 = g_indices[inst.firstIndex + primId * 3u + 0u];
            uint i1 = g_indices[inst.firstIndex + primId * 3u + 1u];
            uint i2 = g_indices[inst.firstIndex + primId * 3u + 2u];

            float2 uv0 = float2(g_vertices[inst.firstVertex + i0].posAndU.w,
                                g_vertices[inst.firstVertex + i0].normalAndV.w);
            float2 uv1 = float2(g_vertices[inst.firstVertex + i1].posAndU.w,
                                g_vertices[inst.firstVertex + i1].normalAndV.w);
            float2 uv2 = float2(g_vertices[inst.firstVertex + i2].posAndU.w,
                                g_vertices[inst.firstVertex + i2].normalAndV.w);

            float2 b2 = q.CandidateTriangleBarycentrics();
            float2 hitUv = (1.0 - b2.x - b2.y) * uv0 + b2.x * uv1 + b2.y * uv2;
            float texAlpha = SampleAlphaMaskConst(inst.alphaMaskTexIndex, hitUv).a;
            float alpha = texAlpha * inst.surfaceParams.z;
            if (alpha >= inst.baseColorAndAlphaCutoff.w)
            {
                q.CommitNonOpaqueTriangleHit();
                break;
            }
        }
    }

    return (q.CommittedStatus() == COMMITTED_TRIANGLE_HIT) ? 0.0 : 1.0;
}

[numthreads(8, 8, 1)]
void main(uint3 tid : SV_DispatchThreadID)
{
    const uint width = g_params.countsAndExtent.x;
    const uint height = g_params.countsAndExtent.y;
    if (tid.x >= width || tid.y >= height)
        return;

    const float4 rt3 = g_rt3WorldPosInst.Load(int3(tid.xy, 0));
    if (rt3.w <= 0.5)
    {
        g_directLightOut[tid.xy] = float4(0.0, 0.0, 0.0, 0.0);
        g_shadowAuxOut[tid.xy] = float2(0.0, 0.0);
        return;
    }

    const float4 rt0 = g_rt0AlbedoMetallic.Load(int3(tid.xy, 0));
    const float4 rt1 = g_rt1NormalRoughness.Load(int3(tid.xy, 0));

    const float3 albedo = rt0.rgb;
    const float metallic = saturate(rt0.a);
    const float3 N = SafeNormal(rt1.rgb);
    const float roughness = clamp(rt1.a, 0.02, 1.0);
    const float3 worldPos = rt3.xyz;

    const float3 toEye = g_params.cameraPosAndHasTlas.xyz - worldPos;
    const float viewDist = length(toEye);
    const float3 V = toEye / max(viewDist, 1e-6);

    const float normalBias = g_params.biasAndFlags.x;
    const float distBias = g_params.biasAndFlags.y;
    const float tMin = g_params.biasAndFlags.z;
    const float maxShadowT = g_params.biasAndFlags.w;

    const float3 rayOrigin = worldPos +
        N * (normalBias + viewDist * distBias) +
        V * (normalBias * 0.75 + viewDist * distBias * 0.5);

    const uint shadowSamples = clamp(g_params.countsAndExtent.z, 1u, 16u);
    const uint frameIndex = g_params.countsAndExtent.w;
    const bool shadowsEnabled = (g_params.featureFlags.x != 0u);
    const bool areaLightsEnabled = (g_params.featureFlags.y != 0u);
    const uint lightCount = min(g_params.featureFlags.z, 8u);
    const bool fillEnabled = (g_params.featureFlags.w != 0u);

    const float noise = InterleavedGradientNoise(float2(tid.xy), frameIndex);

    float3 totalDirect = float3(0.0, 0.0, 0.0);
    uint raysTraced = 0u;
    float dirVis = 1.0;
    float weightedVisSum = 0.0;
    float weightSum = 0.0;

    // 1. Directional Sun Light (soft disk stochastic visibility + Cook-Torrance PBR)
    if (g_params.sunColorAndEnabled.w > 0.5)
    {
        const float3 sunDir = SafeNormal(g_params.sunDirAndAngularRadius.xyz);
        const float sunNL = dot(N, sunDir);
        const float sunTanRadius = tan(clamp(g_params.sunDirAndAngularRadius.w, 0.0, 0.25));

        if (!shadowsEnabled)
        {
            dirVis = 1.0;
        }
        else if (sunNL <= 0.0005)
        {
            dirVis = 0.0;
        }
        else if (shadowSamples == 1u && sunTanRadius <= 1e-6)
        {
            dirVis = TraceShadowRay(rayOrigin, sunDir, tMin, maxShadowT);
            raysTraced += 1u;
        }
        else
        {
            float3 sunT, sunB;
            BuildOrthonormalBasis(sunDir, sunT, sunB);
            float visSum = 0.0;
            for (uint s = 0u; s < shadowSamples; ++s)
            {
                float2 xi = frac(R2Sequence(s + frameIndex * shadowSamples) + float2(noise, frac(noise * 13.37)));
                float r = sqrt(xi.x) * sunTanRadius;
                float phi = 2.0 * kPi * xi.y;
                float3 sampleDir = SafeNormal(sunDir + sunT * (r * cos(phi)) + sunB * (r * sin(phi)));
                visSum += TraceShadowRay(rayOrigin, sampleDir, tMin, maxShadowT);
                raysTraced += 1u;
            }
            dirVis = visSum / float(shadowSamples);
        }

        float3 sunBrdf = EvaluateDirectBrdf(N, V, sunDir, albedo, metallic, roughness);
        float3 sunContrib = sunBrdf * g_params.sunColorAndEnabled.rgb;
        totalDirect += sunContrib * dirVis;
        float w = max(dot(sunContrib, float3(0.2126, 0.7152, 0.0722)), 0.05);
        weightedVisSum += dirVis * w;
        weightSum += w;
    }

    // 2. Explicit Local Lights (RectArea / Point / Spot) with stochastic area-light soft shadows
    if (areaLightsEnabled && lightCount > 0u)
    {
        for (uint li = 0u; li < lightCount; ++li)
        {
            GpuLight light = g_params.lights[li];
            uint lightType = (uint)(light.posAndType.w + 0.5);

            if (lightType == 3u) // RectArea
            {
                float3 emitNormal = SafeNormal(light.dirAndRange.xyz);
                float3 right = light.areaRightAndHalfW.xyz;
                float3 up = light.areaUpAndHalfH.xyz;
                float halfW = max(light.areaRightAndHalfW.w, 1e-4);
                float halfH = max(light.areaUpAndHalfH.w, 1e-4);
                float area = 4.0 * halfW * halfH;
                float range = max(light.dirAndRange.w, 0.5);

                uint areaSamples = clamp(shadowSamples, 1u, 4u);
                float3 accum = float3(0.0, 0.0, 0.0);
                float areaVisSum = 0.0;
                float areaSampleWeightSum = 0.0;

                for (uint s = 0u; s < areaSamples; ++s)
                {
                    float2 xi = frac(R2Sequence(s + li * 7u + frameIndex * 11u) + float2(noise, frac(noise * 7.13)));
                    // Stratify first 2x2 grid when areaSamples >= 4
                    if (areaSamples == 4u)
                    {
                        float2 cell = float2(s & 1u, (s >> 1u) & 1u);
                        xi = (cell + xi) * 0.5;
                    }
                    float2 signedUv = xi * 2.0 - 1.0;
                    float3 samplePos = light.posAndType.xyz + right * (signedUv.x * halfW) + up * (signedUv.y * halfH);
                    float3 toLight = samplePos - worldPos;
                    float distSq = dot(toLight, toLight);
                    float dist = sqrt(max(distSq, 1e-6));
                    float3 L = toLight / dist;

                    float nl = dot(N, L);
                    float lightCos = dot(emitNormal, -L);
                    if (nl > 0.0005 && lightCos > 0.0005 && dist < range)
                    {
                        float ratio = dist / range;
                        float r2 = ratio * ratio;
                        float window = pow(saturate(1.0 - r2 * r2), 2.0);
                        float geomTerm = (lightCos * area) / max(distSq, 0.04);
                        float vis = 1.0;
                        if (shadowsEnabled)
                        {
                            float shadowTMax = max(tMin + 1e-4, dist - 0.02);
                            vis = TraceShadowRay(rayOrigin, L, tMin, shadowTMax);
                            raysTraced += 1u;
                        }
                        float3 brdf = EvaluateDirectBrdf(N, V, L, albedo, metallic, roughness);
                        float3 rad = light.colorAndIntensity.rgb * (light.colorAndIntensity.w * geomTerm * window);
                        accum += brdf * rad * vis;
                        areaVisSum += vis;
                        areaSampleWeightSum += 1.0;
                    }
                }

                totalDirect += accum / float(areaSamples);
                if (areaSampleWeightSum > 0.0)
                {
                    float avgAreaVis = areaVisSum / areaSampleWeightSum;
                    float lW = 0.25;
                    weightedVisSum += avgAreaVis * lW;
                    weightSum += lW;
                }
            }
            else if (lightType == 1u || lightType == 2u) // Point or Spot
            {
                float3 toLightCenter = light.posAndType.xyz - worldPos;
                float centerDist = length(toLightCenter);
                float range = max(light.dirAndRange.w, 0.5);
                if (centerDist < range && centerDist > 1e-4)
                {
                    float3 centerDir = toLightCenter / centerDist;
                    float coneAtten = 1.0;
                    if (lightType == 2u)
                    {
                        float spotCos = dot(SafeNormal(light.dirAndRange.xyz), -centerDir);
                        float cosOuter = light.areaRightAndHalfW.w;
                        float cosInner = max(light.areaUpAndHalfH.w, cosOuter + 1e-4);
                        coneAtten = smoothstep(cosOuter, cosInner, spotCos);
                    }
                    if (coneAtten > 0.001)
                    {
                        float ratio = centerDist / range;
                        float r2 = ratio * ratio;
                        float window = pow(saturate(1.0 - r2 * r2), 2.0);
                        float invSq = 1.0 / max(centerDist * centerDist, 0.06);

                        float3 L = centerDir;
                        float vis = 1.0;
                        if (shadowsEnabled && dot(N, L) > 0.0005)
                        {
                            float shadowTMax = max(tMin + 1e-4, centerDist - 0.02);
                            vis = TraceShadowRay(rayOrigin, L, tMin, shadowTMax);
                            raysTraced += 1u;
                        }
                        float3 brdf = EvaluateDirectBrdf(N, V, L, albedo, metallic, roughness);
                        float3 rad = light.colorAndIntensity.rgb * (light.colorAndIntensity.w * invSq * window * coneAtten);
                        totalDirect += brdf * rad * vis;
                    }
                }
            }
        }
    }

    // 3. Subtle secondary fill light matching existing PBR directional fill (unshadowed soft bounce)
    if (fillEnabled)
    {
        float3 sunDir = SafeNormal(g_params.sunDirAndAngularRadius.xyz);
        float3 fillDir = SafeNormal(float3(-sunDir.z, 0.4, -sunDir.x));
        float3 fillBrdf = EvaluateDirectBrdf(N, V, fillDir, albedo, metallic, roughness);
        totalDirect += fillBrdf * float3(0.45, 0.62, 0.92);
    }

    const float combinedVis = weightSum > 0.0 ? (weightedVisSum / weightSum) : dirVis;
    g_directLightOut[tid.xy] = float4(totalDirect, saturate(combinedVis));
    g_shadowAuxOut[tid.xy] = float2(saturate(dirVis), saturate(float(raysTraced) / 16.0));
}
