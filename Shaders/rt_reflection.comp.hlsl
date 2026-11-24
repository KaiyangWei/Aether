struct GpuLight
{
    float4 posAndType;
    float4 dirAndRange;
    float4 colorAndIntensity;
    float4 areaRightAndHalfW;
    float4 areaUpAndHalfH;
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

struct ReflectionParamsUbo
{
    float4 cameraPosAndHasTlas;    // xyz = cameraPos, w = hasTlas
    float4 sunDirAndMaxDist;       // xyz = normalized dir TO sun, w = reflectionMaxDistance
    float4 sunColorAndEnabled;     // rgb = sun radiance, w = sunLightEnabled
    float4 roughnessParams;        // x = minMirrorRoughness, y = maxRoughness, z = normalBias, w = distBias
    uint4  countsAndExtent;        // x = width, y = height, z = frameIndex, w = reflectionsEnabled
    uint4  lightFlags;             // x = areaLightsEnabled, y = lightCount, z = secondaryShadow, w = fillEnabled
    GpuLight lights[8];
};

[[vk::binding(0, 0)]] ConstantBuffer<ReflectionParamsUbo> g_params;
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
[[vk::image_format("rgba16f")]] [[vk::binding(11, 0)]] RWTexture2D<float4> g_reflectionOut;
[[vk::image_format("rg16f")]] [[vk::binding(12, 0)]] RWTexture2D<float2> g_reflectionAuxOut;

// Shared VulkanEnvironment Set 1 (FRAGMENT_BIT | COMPUTE_BIT)
[[vk::binding(0, 1)]] TextureCube<float4> g_irradianceMap;
[[vk::binding(1, 1)]] TextureCube<float4> g_prefilteredMap;
[[vk::binding(2, 1)]] Texture2D<float4> g_brdfLut;
[[vk::binding(3, 1)]] SamplerState g_envSampler;
[[vk::binding(4, 1)]] TextureCube<float4> g_envCube;

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

void BuildTangentBasis(float3 n, out float3 t, out float3 b)
{
    float3 up = abs(n.z) < 0.999 ? float3(0.0, 0.0, 1.0) : float3(1.0, 0.0, 0.0);
    t = SafeNormal(cross(up, n));
    b = cross(n, t);
}

// Heitz (2018) GGX Visible Normal Distribution Function importance sampling.
float3 SampleGgxVndfWorld(float3 V, float3 N, float alpha, float2 xi)
{
    float3 T, B;
    BuildTangentBasis(N, T, B);
    float3 Vlocal = float3(dot(V, T), dot(V, B), max(dot(V, N), 1e-4));

    float a = clamp(alpha, 0.001, 1.0);
    float3 Vh = SafeNormal(float3(a * Vlocal.x, a * Vlocal.y, Vlocal.z));

    float lensq = Vh.x * Vh.x + Vh.y * Vh.y;
    float3 T1 = lensq > 1e-12 ? float3(-Vh.y, Vh.x, 0.0) * rsqrt(lensq) : float3(1.0, 0.0, 0.0);
    float3 T2 = cross(Vh, T1);

    float r = sqrt(saturate(xi.x));
    float phi = 2.0 * kPi * xi.y;
    float t1 = r * cos(phi);
    float t2 = r * sin(phi);
    float s = 0.5 * (1.0 + Vh.z);
    t2 = (1.0 - s) * sqrt(saturate(1.0 - t1 * t1)) + s * t2;

    float3 Nh = t1 * T1 + t2 * T2 + sqrt(saturate(1.0 - t1 * t1 - t2 * t2)) * Vh;
    float3 Hlocal = SafeNormal(float3(a * Nh.x, a * Nh.y, max(0.0, Nh.z)));
    return SafeNormal(T * Hlocal.x + B * Hlocal.y + N * Hlocal.z);
}

float SmithG1(float nDotX, float alpha2)
{
    return 2.0 * nDotX / max(nDotX + sqrt(alpha2 + (1.0 - alpha2) * nDotX * nDotX), 1e-5);
}

float3 SchlickFresnel(float cosTheta, float3 f0)
{
    return f0 + (1.0 - f0) * pow(saturate(1.0 - cosTheta), 5.0);
}

float3 TransformObjectNormalToWorld(RtGpuInstanceData inst, float3 objN)
{
    float3 worldN;
    worldN.x = inst.invModelRows[0].x * objN.x + inst.invModelRows[1].x * objN.y + inst.invModelRows[2].x * objN.z;
    worldN.y = inst.invModelRows[0].y * objN.x + inst.invModelRows[1].y * objN.y + inst.invModelRows[2].y * objN.z;
    worldN.z = inst.invModelRows[0].z * objN.x + inst.invModelRows[1].z * objN.y + inst.invModelRows[2].z * objN.z;
    return SafeNormal(worldN);
}

float TraceSecondaryShadow(float3 origin, float3 dir, float tMin, float tMax)
{
    if (g_params.lightFlags.z == 0u || tMax <= tMin)
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
            float alpha = SampleAlphaMaskConst(inst.alphaMaskTexIndex, hitUv).a * inst.surfaceParams.z;
            if (alpha >= inst.baseColorAndAlphaCutoff.w)
            {
                q.CommitNonOpaqueTriangleHit();
                break;
            }
        }
    }
    return (q.CommittedStatus() == COMMITTED_TRIANGLE_HIT) ? 0.0 : 1.0;
}

float3 EvaluateSimpleHitBrdf(float3 n, float3 v, float3 l, float3 baseColor, float metallic, float roughness)
{
    float nl = saturate(dot(n, l));
    if (nl <= 0.0)
        return float3(0.0, 0.0, 0.0);

    float nv = max(saturate(dot(n, v)), 1e-4);
    float3 h = SafeNormal(v + l);
    float nh = saturate(dot(n, h));
    float vh = saturate(dot(v, h));

    float alpha = max(roughness * roughness, 0.002);
    float alpha2 = alpha * alpha;
    float term = nh * nh * (alpha2 - 1.0) + 1.0;
    float d = alpha2 / max(kPi * term * term, 1e-6);
    float g = SmithG1(nl, alpha2) * SmithG1(nv, alpha2);
    float3 f0 = lerp(0.04.xxx, baseColor, metallic);
    float3 f = SchlickFresnel(vh, f0);
    float3 spec = (d * g * f) / max(4.0 * nl * nv, 1e-5);
    float3 diff = (1.0 - f) * (1.0 - metallic) * baseColor / kPi;
    return (diff + spec) * nl;
}

[numthreads(8, 8, 1)]
void main(uint3 tid : SV_DispatchThreadID)
{
    const uint width = g_params.countsAndExtent.x;
    const uint height = g_params.countsAndExtent.y;
    if (tid.x >= width || tid.y >= height)
        return;

    const float4 rt3 = g_rt3WorldPosInst.Load(int3(tid.xy, 0));
    const uint enabled = g_params.countsAndExtent.w;
    if (rt3.w <= 0.5 || enabled == 0u || g_params.cameraPosAndHasTlas.w < 0.5)
    {
        g_reflectionOut[tid.xy] = float4(0.0, 0.0, 0.0, 0.0);
        g_reflectionAuxOut[tid.xy] = float2(0.0, 0.0);
        return;
    }

    const float4 rt0 = g_rt0AlbedoMetallic.Load(int3(tid.xy, 0));
    const float4 rt1 = g_rt1NormalRoughness.Load(int3(tid.xy, 0));

    const float3 albedo = rt0.rgb;
    const float metallic = saturate(rt0.a);
    const float3 N = SafeNormal(rt1.rgb);
    const float roughness = clamp(rt1.a, 0.02, 1.0);
    const float3 worldPos = rt3.xyz;

    const float minMirrorRough = g_params.roughnessParams.x;
    const float maxRough = g_params.roughnessParams.y;
    if (roughness > maxRough)
    {
        g_reflectionOut[tid.xy] = float4(0.0, 0.0, 0.0, 0.0);
        g_reflectionAuxOut[tid.xy] = float2(0.0, 0.0);
        return;
    }

    const float3 toEye = g_params.cameraPosAndHasTlas.xyz - worldPos;
    const float viewDist = length(toEye);
    const float3 V = toEye / max(viewDist, 1e-6);
    const float3 f0 = lerp(0.04.xxx, albedo, metallic);

    const bool isMirror = (roughness <= minMirrorRough);
    float3 L;
    float3 brdfWeight;

    if (isMirror)
    {
        L = SafeNormal(reflect(-V, N));
        float nv = max(dot(N, V), 1e-4);
        brdfWeight = SchlickFresnel(nv, f0);
    }
    else
    {
        const uint frameIndex = g_params.countsAndExtent.z;
        const float ign = InterleavedGradientNoise(float2(tid.xy), frameIndex);
        const float2 r2 = R2Sequence(frameIndex);
        const float2 xi = frac(float2(ign, frac(ign * 11.73 + r2.y)) + r2);
        const float alpha = max(roughness * roughness, 0.0025);
        float3 H = SampleGgxVndfWorld(V, N, alpha, xi);
        L = SafeNormal(reflect(-V, H));
        if (dot(N, L) <= 0.001)
            L = SafeNormal(reflect(-V, N));
        float nl = saturate(dot(N, L));
        float vh = saturate(dot(V, H));
        float alpha2 = alpha * alpha;
        brdfWeight = SchlickFresnel(vh, f0) * SmithG1(nl, alpha2);
    }

    if (dot(N, L) <= 0.0005)
    {
        g_reflectionOut[tid.xy] = float4(0.0, 0.0, 0.0, 0.0);
        g_reflectionAuxOut[tid.xy] = float2(0.0, 0.0);
        return;
    }

    const float normalBias = g_params.roughnessParams.z;
    const float distBias = g_params.roughnessParams.w;
    const float3 origin = worldPos +
        N * (normalBias + viewDist * distBias) +
        V * (normalBias * 0.5 + viewDist * distBias * 0.35);

    RayDesc ray;
    ray.Origin = origin;
    ray.Direction = L;
    ray.TMin = 0.002;
    ray.TMax = max(g_params.sunDirAndMaxDist.w, 1.0);

    RayQuery<RAY_FLAG_NONE> q;
    q.TraceRayInline(g_tlas, RAY_FLAG_NONE, 0xFF, ray);

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
            float alpha = SampleAlphaMaskConst(inst.alphaMaskTexIndex, hitUv).a * inst.surfaceParams.z;
            if (alpha >= inst.baseColorAndAlphaCutoff.w)
            {
                q.CommitNonOpaqueTriangleHit();
            }
        }
    }

    if (q.CommittedStatus() != COMMITTED_TRIANGLE_HIT)
    {
        float3 envRad = isMirror
            ? g_envCube.SampleLevel(g_envSampler, L, 0.0).rgb
            : g_prefilteredMap.SampleLevel(g_envSampler, L, roughness * 7.0).rgb;
        if (!isMirror)
        {
            float envLum = dot(envRad, float3(0.2126, 0.7152, 0.0722));
            if (envLum > 24.0)
                envRad *= (24.0 / envLum);
        }
        g_reflectionOut[tid.xy] = float4(envRad * brdfWeight, 0.0);
        g_reflectionAuxOut[tid.xy] = float2(0.0, 0.0);
        return;
    }

    const float hitT = q.CommittedRayT();
    const uint hitInstId = q.CommittedInstanceID();
    const uint hitPrimId = q.CommittedPrimitiveIndex();
    const float2 b2 = q.CommittedTriangleBarycentrics();
    const float3 bary = float3(1.0 - b2.x - b2.y, b2.x, b2.y);

    const RtGpuInstanceData hitInst = g_instances[hitInstId];
    const uint i0 = g_indices[hitInst.firstIndex + hitPrimId * 3u + 0u];
    const uint i1 = g_indices[hitInst.firstIndex + hitPrimId * 3u + 1u];
    const uint i2 = g_indices[hitInst.firstIndex + hitPrimId * 3u + 2u];

    const RtGpuVertex v0 = g_vertices[hitInst.firstVertex + i0];
    const RtGpuVertex v1 = g_vertices[hitInst.firstVertex + i1];
    const RtGpuVertex v2 = g_vertices[hitInst.firstVertex + i2];

    float3 objN = v0.normalAndV.xyz * bary.x + v1.normalAndV.xyz * bary.y + v2.normalAndV.xyz * bary.z;
    if (dot(objN, objN) < 1e-6)
        objN = cross(v1.posAndU.xyz - v0.posAndU.xyz, v2.posAndU.xyz - v0.posAndU.xyz);

    float3 hitN = TransformObjectNormalToWorld(hitInst, objN);
    const float3 hitV = -L;
    if (dot(hitN, hitV) < 0.0)
        hitN = -hitN;

    const float3 hitPos = origin + L * hitT;
    const float3 hitAlbedo = hitInst.baseColorAndAlphaCutoff.rgb;
    const float3 hitEmissive = hitInst.emissiveAndMetallic.rgb;
    const float hitMetallic = saturate(hitInst.emissiveAndMetallic.a);
    const float hitRoughness = clamp(hitInst.surfaceParams.x, 0.02, 1.0);
    const float hitAo = saturate(hitInst.surfaceParams.y);

    float3 hitRadiance = hitEmissive;

    if (g_params.sunColorAndEnabled.w > 0.5)
    {
        float3 sunDir = SafeNormal(g_params.sunDirAndMaxDist.xyz);
        if (dot(hitN, sunDir) > 0.0005)
        {
            float3 hitShadowOrigin = hitPos + hitN * 0.008;
            float hitSunVis = TraceSecondaryShadow(hitShadowOrigin, sunDir, 0.002, 150.0);
            hitRadiance += EvaluateSimpleHitBrdf(hitN, hitV, sunDir, hitAlbedo, hitMetallic, hitRoughness) *
                           g_params.sunColorAndEnabled.rgb * hitSunVis;
        }
    }

    if (g_params.lightFlags.w != 0u)
    {
        float3 sunDir = SafeNormal(g_params.sunDirAndMaxDist.xyz);
        float3 fillDir = SafeNormal(float3(-sunDir.z, 0.4, -sunDir.x));
        hitRadiance += EvaluateSimpleHitBrdf(hitN, hitV, fillDir, hitAlbedo, hitMetallic, hitRoughness) *
                       float3(0.45, 0.62, 0.92);
    }

    if (g_params.lightFlags.x != 0u)
    {
        uint lightCount = min(g_params.lightFlags.y, 8u);
        for (uint li = 0u; li < lightCount; ++li)
        {
            GpuLight light = g_params.lights[li];
            uint lightType = (uint)(light.posAndType.w + 0.5);
            float3 toLight = light.posAndType.xyz - hitPos;
            float distSq = dot(toLight, toLight);
            float dist = sqrt(max(distSq, 1e-6));
            float range = max(light.dirAndRange.w, 0.5);
            if (dist < range)
            {
                float3 lDir = toLight / dist;
                float ratio = dist / range;
                float r2 = ratio * ratio;
                float window = pow(saturate(1.0 - r2 * r2), 2.0);
                if (lightType == 3u)
                {
                    float halfW = max(light.areaRightAndHalfW.w, 1e-4);
                    float halfH = max(light.areaUpAndHalfH.w, 1e-4);
                    float area = 4.0 * halfW * halfH;
                    float lightCos = max(0.0, dot(SafeNormal(light.dirAndRange.xyz), -lDir));
                    float geom = (lightCos * area) / max(distSq, 0.12);
                    hitRadiance += EvaluateSimpleHitBrdf(hitN, hitV, lDir, hitAlbedo, hitMetallic, hitRoughness) *
                                   (light.colorAndIntensity.rgb * (light.colorAndIntensity.w * geom * window));
                }
                else
                {
                    float invSq = 1.0 / max(distSq, 0.12);
                    hitRadiance += EvaluateSimpleHitBrdf(hitN, hitV, lDir, hitAlbedo, hitMetallic, hitRoughness) *
                                   (light.colorAndIntensity.rgb * (light.colorAndIntensity.w * invSq * window));
                }
            }
        }
    }

    {
        float hitNV = saturate(dot(hitN, hitV));
        float3 hitF0 = lerp(0.04.xxx, hitAlbedo, hitMetallic);
        float2 hitAb = g_brdfLut.SampleLevel(g_envSampler, float2(hitNV, hitRoughness), 0.0).rg;
        float3 hitSpecWeight = hitF0 * hitAb.x + hitAb.y;
        float3 hitDiffWeight = (1.0 - saturate(hitSpecWeight)) * (1.0 - hitMetallic);
        float3 hitIrrad = g_irradianceMap.SampleLevel(g_envSampler, hitN, 0.0).rgb * hitAlbedo * hitDiffWeight;
        float3 hitSpecEnv = g_prefilteredMap.SampleLevel(g_envSampler, reflect(-hitV, hitN), hitRoughness * 7.0).rgb * hitSpecWeight;
        hitRadiance += (hitIrrad + hitSpecEnv) * hitAo;
    }

    if (!isMirror)
    {
        float lum = dot(hitRadiance, float3(0.2126, 0.7152, 0.0722));
        if (lum > 24.0)
            hitRadiance *= (24.0 / lum);
    }

    g_reflectionOut[tid.xy] = float4(hitRadiance * brdfWeight, 1.0);
    g_reflectionAuxOut[tid.xy] = float2(1.0, hitT);
}
