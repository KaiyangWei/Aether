struct RayQueryParams
{
    column_major float4x4 clipToWorldDirection;
    float4 cameraOriginAndTMin; // xyz = cameraPosition, w = tMin
    float4 rayLimitsAndSize;    // x = tMax, y = maxDistVis, z = width, w = height
    uint4  modeAndFlags;        // x = debugMode, y = hasEnvironment, z = reserved, w = reserved
};

[[vk::binding(0, 0)]] RaytracingAccelerationStructure g_tlas;
[[vk::image_format("rgba16f")]] [[vk::binding(1, 0)]] RWTexture2D<float4> g_output;
[[vk::binding(2, 0)]] ConstantBuffer<RayQueryParams> g_params;

// Shared VulkanEnvironment Set 1 (same descriptor set as Skybox and PBR passes).
[[vk::binding(3, 1)]] SamplerState g_envSampler;
[[vk::binding(4, 1)]] TextureCube<float4> g_envCube;

float3 HashColor(uint seed)
{
    uint h = seed * 747796405u + 2891336453u;
    h = ((h >> ((h >> 28u) + 4u)) ^ h) * 277803737u;
    h = (h >> 22u) ^ h;

    float r = float((h >> 0u) & 255u) / 255.0;
    float g = float((h >> 8u) & 255u) / 255.0;
    float b = float((h >> 16u) & 255u) / 255.0;
    return lerp(float3(0.18, 0.22, 0.28), float3(0.96, 0.94, 0.92), float3(r, g, b));
}

float3 DistanceRamp(float normalizedDist)
{
    float d = saturate(normalizedDist);
    float3 nearColor = float3(1.40, 0.72, 0.16);
    float3 midColor  = float3(0.14, 0.42, 0.56);
    float3 farColor  = float3(0.015, 0.025, 0.09);
    float t = pow(d, 1.15);
    return t < 0.5
        ? lerp(nearColor, midColor, t * 2.0)
        : lerp(midColor, farColor, (t - 0.5) * 2.0);
}

[numthreads(8, 8, 1)]
void main(uint3 tid : SV_DispatchThreadID)
{
    const uint width = (uint)g_params.rayLimitsAndSize.z;
    const uint height = (uint)g_params.rayLimitsAndSize.w;
    if (tid.x >= width || tid.y >= height)
        return;

    const float2 uv = (float2(tid.xy) + 0.5) / float2(width, height);
    const float2 ndc = uv * 2.0 - 1.0;

    // Exact match to Vulkan positive-height viewport and Camera::GetProjectionMatrix() Y-flip.
    const float4 worldH = mul(g_params.clipToWorldDirection, float4(ndc, 1.0, 1.0));
    const float3 rayDir = normalize(worldH.xyz / worldH.w);

    RayDesc ray;
    ray.Origin = g_params.cameraOriginAndTMin.xyz;
    ray.Direction = rayDir;
    ray.TMin = g_params.cameraOriginAndTMin.w;
    ray.TMax = g_params.rayLimitsAndSize.x;

    RayQuery<RAY_FLAG_NONE> q;
    q.TraceRayInline(g_tlas, RAY_FLAG_NONE, 0xFF, ray);
    while (q.Proceed())
    {
        if (q.CandidateType() == CANDIDATE_NON_OPAQUE_TRIANGLE)
        {
            q.CommitNonOpaqueTriangleHit();
        }
    }

    const uint mode = g_params.modeAndFlags.x;
    const bool hit = (q.CommittedStatus() == COMMITTED_TRIANGLE_HIT);

    if (mode == 4u)
    {
        // Mode 4: Raw deterministic numeric readback (hitFlag, rayT, primitiveIndex, instanceCustomIndex).
        if (hit)
        {
            g_output[tid.xy] = float4(
                1.0,
                q.CommittedRayT(),
                float(q.CommittedPrimitiveIndex()),
                float(q.CommittedInstanceID()));
        }
        else
        {
            g_output[tid.xy] = float4(0.0, 0.0, -1.0, -1.0);
        }
        return;
    }

    float3 outColor = float3(0.0, 0.0, 0.0);

    if (!hit)
    {
        if (mode == 0u && g_params.modeAndFlags.y != 0u)
        {
            outColor = g_envCube.SampleLevel(g_envSampler, rayDir, 0.0).rgb;
        }
        else if (mode == 1u)
        {
            outColor = float3(0.04, 0.08, 0.22);
        }
        else if (mode == 2u)
        {
            outColor = float3(0.015, 0.025, 0.06);
        }
        else
        {
            outColor = float3(0.025, 0.035, 0.07);
        }
        g_output[tid.xy] = float4(outColor, 1.0);
        return;
    }

    const float rayT = q.CommittedRayT();
    const uint primIndex = q.CommittedPrimitiveIndex();
    const uint instId = q.CommittedInstanceID();
    const float2 bary2 = q.CommittedTriangleBarycentrics();
    const float3 bary = float3(1.0 - bary2.x - bary2.y, bary2.x, bary2.y);
    const bool frontFace = q.CommittedTriangleFrontFace();

    if (mode == 0u)
    {
        // Mode 0: Spatially readable Ray Query diagnostic shading
        // Instance palette + subtle triangle edge wireframe + depth attenuation + front/back cue.
        const float3 instColor = HashColor(instId + 17u);
        const float minBary = min(bary.x, min(bary.y, bary.z));
        const float edgeFactor = smoothstep(0.0, 0.035, minBary);
        const float depthAtten = 1.0 / (1.0 + 0.07 * rayT * rayT * 0.25 + 0.08 * rayT);
        const float faceCue = frontFace ? 1.0 : 0.65;
        const float baryShade = 0.72 + 0.28 * (bary.x * 1.15 + bary.y * 0.85 + bary.z * 0.65);
        const float3 baseDiag = instColor * baryShade * (0.38 + 0.92 * depthAtten) * faceCue;
        outColor = lerp(baseDiag * 0.35 + float3(0.06, 0.08, 0.11), baseDiag, edgeFactor);
    }
    else if (mode == 1u)
    {
        // Mode 1: Hit / Miss
        outColor = float3(0.14, 0.88, 0.34);
    }
    else if (mode == 2u)
    {
        // Mode 2: Normalized Hit Distance
        const float maxDistVis = max(g_params.rayLimitsAndSize.y, 0.1);
        outColor = DistanceRamp(rayT / maxDistVis);
    }
    else
    {
        // Mode 3: Primitive & Instance Index
        const float3 instColor = HashColor(instId * 131u + 7u);
        const float3 primColor = HashColor(primIndex * 977u + instId * 37u + 3u);
        const float minBary = min(bary.x, min(bary.y, bary.z));
        const float edgeFactor = smoothstep(0.0, 0.03, minBary);
        outColor = lerp(float3(0.04, 0.04, 0.06), lerp(instColor, primColor, 0.55), edgeFactor);
    }

    g_output[tid.xy] = float4(outColor, 1.0);
}
