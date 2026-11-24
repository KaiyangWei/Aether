struct RtGpuInstanceData
{
    float4 baseColorAndAlphaCutoff;
    float4 emissiveAndMetallic;
    float4 surfaceParams;
    float4 modelRows[3];
    float4 prevModelRows[3];
    float4 invModelRows[3];
    uint   firstVertex;
    uint   firstIndex;
    uint   alphaMaskTexIndex;
    uint   sceneObjectIndex;
};

struct TemporalParamsUbo
{
    column_major float4x4 prevViewProj;
    float4 cameraPosAndHistoryValid; // xyz = cameraPos, w = historyValid (0 or 1)
    float4 alphaAndThresholds;       // x = alphaMin, y = alphaMax, z = minMirrorRoughness, w = normalDotThreshold
    uint4  extentAndFlags;           // x = width, y = height, z = denoiserEnabled, w = shadowSamples
};

[[vk::binding(0, 0)]] ConstantBuffer<TemporalParamsUbo> g_params;
[[vk::binding(1, 0)]] Texture2D<float4> g_currRt1;
[[vk::binding(2, 0)]] Texture2D<float4> g_currRt3;
[[vk::binding(3, 0)]] Texture2D<float4> g_prevRt1;
[[vk::binding(4, 0)]] Texture2D<float4> g_prevRt3;
[[vk::binding(5, 0)]] Texture2D<float4> g_rawDirect;
[[vk::binding(6, 0)]] Texture2D<float4> g_rawRefl;
[[vk::binding(7, 0)]] Texture2D<float4> g_prevShadowAccum;
[[vk::binding(8, 0)]] Texture2D<float4> g_prevReflAccum;
[[vk::binding(9, 0)]] Texture2D<float4> g_prevMoments;
[[vk::binding(10, 0)]] StructuredBuffer<RtGpuInstanceData> g_instances;
[[vk::image_format("rgba16f")]] [[vk::binding(11, 0)]] RWTexture2D<float4> g_shadowAccumOut;
[[vk::image_format("rgba16f")]] [[vk::binding(12, 0)]] RWTexture2D<float4> g_reflAccumOut;
[[vk::image_format("rgba16f")]] [[vk::binding(13, 0)]] RWTexture2D<float4> g_momentsOut;
[[vk::image_format("rg16f")]] [[vk::binding(14, 0)]] RWTexture2D<float2> g_varConfOut;

float3 SafeNormal(float3 v)
{
    return v * rsqrt(max(dot(v, v), 1e-20));
}

float Luminance(float3 c)
{
    return max(0.0, dot(c, float3(0.2126, 0.7152, 0.0722)));
}

float ComputeSpatialFallbackVariance(int2 pix, int width, int height)
{
    float m1 = 0.0;
    float m2 = 0.0;
    float wSum = 0.0;
    [unroll]
    for (int dy = -1; dy <= 1; ++dy)
    {
        [unroll]
        for (int dx = -1; dx <= 1; ++dx)
        {
            int2 sp = clamp(pix + int2(dx, dy), int2(0, 0), int2(width - 1, height - 1));
            float l = Luminance(g_rawDirect.Load(int3(sp, 0)).rgb + g_rawRefl.Load(int3(sp, 0)).rgb);
            m1 += l;
            m2 += l * l;
            wSum += 1.0;
        }
    }
    m1 /= max(wSum, 1.0);
    m2 /= max(wSum, 1.0);
    return max(0.0, m2 - m1 * m1);
}

[numthreads(8, 8, 1)]
void main(uint3 tid : SV_DispatchThreadID)
{
    const uint width = g_params.extentAndFlags.x;
    const uint height = g_params.extentAndFlags.y;
    if (tid.x >= width || tid.y >= height)
        return;

    const float4 rt3 = g_currRt3.Load(int3(tid.xy, 0));
    const float4 rawDirect = g_rawDirect.Load(int3(tid.xy, 0));
    const float4 rawRefl = g_rawRefl.Load(int3(tid.xy, 0));

    if (rt3.w <= 0.5)
    {
        g_shadowAccumOut[tid.xy] = float4(0.0, 0.0, 0.0, 0.0);
        g_reflAccumOut[tid.xy] = float4(0.0, 0.0, 0.0, 0.0);
        g_momentsOut[tid.xy] = float4(0.0, 0.0, 0.0, 0.0);
        g_varConfOut[tid.xy] = float2(0.0, 0.0);
        return;
    }

    const float currShadowLuma = Luminance(rawDirect.rgb);
    const float currReflLuma = Luminance(rawRefl.rgb);

    const bool denoiserEnabled = (g_params.extentAndFlags.z != 0u);
    if (!denoiserEnabled)
    {
        float spatialVar = ComputeSpatialFallbackVariance(int2(tid.xy), int(width), int(height));
        g_shadowAccumOut[tid.xy] = rawDirect;
        g_reflAccumOut[tid.xy] = float4(rawRefl.rgb, 1.0);
        g_momentsOut[tid.xy] = float4(currShadowLuma, currShadowLuma * currShadowLuma,
                                      currReflLuma, currReflLuma * currReflLuma);
        g_varConfOut[tid.xy] = float2(spatialVar, 0.0);
        return;
    }

    const float4 rt1 = g_currRt1.Load(int3(tid.xy, 0));
    const float3 currN = SafeNormal(rt1.rgb);
    const float roughness = rt1.a;
    const float3 worldPos = rt3.xyz;
    const uint instId = (uint)max(0.0, rt3.w - 0.5);
    const float viewDist = length(g_params.cameraPosAndHistoryValid.xyz - worldPos);

    bool validHistory = (g_params.cameraPosAndHistoryValid.w > 0.5);
    float4 prevShadow = float4(0.0, 0.0, 0.0, 0.0);
    float4 prevRefl = float4(0.0, 0.0, 0.0, 0.0);
    float4 prevMoments = float4(0.0, 0.0, 0.0, 0.0);

    if (validHistory)
    {
        RtGpuInstanceData inst = g_instances[instId];
        float4 wp = float4(worldPos, 1.0);
        float3 localPos = float3(
            dot(inst.invModelRows[0], wp),
            dot(inst.invModelRows[1], wp),
            dot(inst.invModelRows[2], wp));
        float4 lp = float4(localPos, 1.0);
        float3 prevWorldPos = float3(
            dot(inst.prevModelRows[0], lp),
            dot(inst.prevModelRows[1], lp),
            dot(inst.prevModelRows[2], lp));

        float4 prevClip = mul(g_params.prevViewProj, float4(prevWorldPos, 1.0));
        if (prevClip.w <= 1e-4)
        {
            validHistory = false;
        }
        else
        {
            float2 prevNdc = prevClip.xy / prevClip.w;
            float2 prevUv = prevNdc * 0.5 + 0.5;
            if (any(prevUv < float2(0.0, 0.0)) || any(prevUv > float2(1.0, 1.0)))
            {
                validHistory = false;
            }
            else
            {
                float2 fCoord = prevUv * float2(width, height) - 0.5;
                int2 baseTap = int2(floor(fCoord));
                float2 fracTap = frac(fCoord);
                const float bilinearW[4] = {
                    (1.0 - fracTap.x) * (1.0 - fracTap.y),
                    fracTap.x * (1.0 - fracTap.y),
                    (1.0 - fracTap.x) * fracTap.y,
                    fracTap.x * fracTap.y
                };
                const int2 offsets[4] = {
                    int2(0, 0), int2(1, 0), int2(0, 1), int2(1, 1)
                };

                float maxPosDelta = max(0.08, 0.04 * viewDist);
                float normalThreshold = g_params.alphaAndThresholds.w;

                float4 sumShadow = float4(0.0, 0.0, 0.0, 0.0);
                float4 sumRefl = float4(0.0, 0.0, 0.0, 0.0);
                float4 sumMoments = float4(0.0, 0.0, 0.0, 0.0);
                float totalW = 0.0;

                [unroll]
                for (int k = 0; k < 4; ++k)
                {
                    int2 sp = baseTap + offsets[k];
                    if (sp.x >= 0 && sp.x < int(width) && sp.y >= 0 && sp.y < int(height))
                    {
                        float4 pRt3 = g_prevRt3.Load(int3(sp, 0));
                        uint pInstId = (uint)max(0.0, pRt3.w - 0.5);
                        if (pRt3.w > 0.5 && pInstId == instId)
                        {
                            float3 pNormal = SafeNormal(g_prevRt1.Load(int3(sp, 0)).rgb);
                            if (dot(currN, pNormal) >= normalThreshold &&
                                length(pRt3.xyz - prevWorldPos) <= maxPosDelta)
                            {
                                float w = max(bilinearW[k], 1e-4);
                                sumShadow += g_prevShadowAccum.Load(int3(sp, 0)) * w;
                                sumRefl += g_prevReflAccum.Load(int3(sp, 0)) * w;
                                sumMoments += g_prevMoments.Load(int3(sp, 0)) * w;
                                totalW += w;
                            }
                        }
                    }
                }

                if (totalW > 1e-5)
                {
                    prevShadow = sumShadow / totalW;
                    prevRefl = sumRefl / totalW;
                    prevMoments = sumMoments / totalW;
                }
                else
                {
                    validHistory = false;
                }
            }
        }
    }

    float histLen = validHistory ? (max(prevRefl.a, 0.0) + 1.0) : 1.0;
    const float maxHist = 24.0;
    histLen = min(histLen, maxHist);

    float alphaShadow = validHistory
        ? clamp(1.0 / histLen, g_params.alphaAndThresholds.x, g_params.alphaAndThresholds.y)
        : 1.0;

    // Mirrors respond rapidly without lagging, while rough surfaces accumulate longer for noise suppression.
    const bool isMirror = (roughness <= g_params.alphaAndThresholds.z);
    float alphaRefl = validHistory
        ? (isMirror ? max(alphaShadow, 0.35) : alphaShadow)
        : 1.0;

    float4 shadowAccum = lerp(prevShadow, rawDirect, alphaShadow);
    float3 reflAccum = lerp(prevRefl.rgb, rawRefl.rgb, alphaRefl);

    float sM1 = lerp(prevMoments.x, currShadowLuma, alphaShadow);
    float sM2 = lerp(prevMoments.y, currShadowLuma * currShadowLuma, alphaShadow);
    float rM1 = lerp(prevMoments.z, currReflLuma, alphaRefl);
    float rM2 = lerp(prevMoments.w, currReflLuma * currReflLuma, alphaRefl);

    float sVar = max(0.0, sM2 - sM1 * sM1);
    float rVar = max(0.0, rM2 - rM1 * rM1);
    float temporalVar = sVar + rVar;
    if (histLen < 4.0)
    {
        float spatialVar = ComputeSpatialFallbackVariance(int2(tid.xy), int(width), int(height));
        temporalVar = lerp(spatialVar, temporalVar, histLen / 4.0);
    }

    float confidence = saturate(histLen / 16.0);

    g_shadowAccumOut[tid.xy] = shadowAccum;
    g_reflAccumOut[tid.xy] = float4(reflAccum, histLen);
    g_momentsOut[tid.xy] = float4(sM1, sM2, rM1, rM2);
    g_varConfOut[tid.xy] = float2(temporalVar, confidence);
}
