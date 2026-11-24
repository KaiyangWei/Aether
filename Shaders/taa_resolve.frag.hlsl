struct FrameParams
{
    float4 jitterAndInvExtent; // xy: previous minus current jitter in UV, zw: invExtent
    float4 weights;            // x: historyWeight, y: minWeight, z: maxWeight, w: historyValid
    float4 thresholds;         // x: motionRejectionScale, y: depthRejectionThreshold, z: varianceGamma, w: unused
};

[[vk::binding(0, 0)]] ConstantBuffer<FrameParams> params;
[[vk::binding(1, 0)]] Texture2D<float4> currentHdr;
[[vk::binding(2, 0)]] Texture2D<float4> historyHdr;
[[vk::binding(3, 0)]] Texture2D<float4> velocityTex;
[[vk::binding(4, 0)]] Texture2D<float> currentDepthTex;
[[vk::binding(5, 0)]] Texture2D<float> prevDepthTex;
[[vk::binding(6, 0)]] SamplerState linearSampler;
[[vk::binding(7, 0)]] SamplerState pointSampler;

struct Input
{
    [[vk::location(0)]] float2 uv : TEXCOORD0;
};

float3 SafeHdr(float3 c)
{
    return min(max(select(isfinite(c), c, 0.0.xxx), 0.0.xxx), 65000.0.xxx);
}

float Luminance(float3 c)
{
    return dot(c, float3(0.2126, 0.7152, 0.0722));
}

// Perceptual Reinhard-like HDR compression during temporal clamping to prevent highlight pumping.
float3 CompressHdr(float3 c)
{
    return c / (1.0 + Luminance(c));
}

float3 ExpandHdr(float3 c)
{
    return c / max(1.0 - Luminance(c), 1.0 / 65001.0);
}

float3 RgbToYCoCg(float3 rgb)
{
    const float y  =  0.25 * rgb.r + 0.5 * rgb.g + 0.25 * rgb.b;
    const float co =  0.50 * rgb.r               - 0.50 * rgb.b;
    const float cg = -0.25 * rgb.r + 0.5 * rgb.g - 0.25 * rgb.b;
    return float3(y, co, cg);
}

float3 YCoCgToRgb(float3 ycocg)
{
    const float y  = ycocg.x;
    const float co = ycocg.y;
    const float cg = ycocg.z;
    return float3(y + co - cg, y + cg, y - co - cg);
}

float4 main(Input input) : SV_Target0
{
    const float2 uv = input.uv;
    const float2 invExtent = params.jitterAndInvExtent.zw;
    const float3 currentRaw = SafeHdr(currentHdr.SampleLevel(pointSampler, uv, 0).rgb);

    if (params.weights.w < 0.5 || params.weights.z <= 0.0)
        return float4(currentRaw, 1.0);

    // Use the visible surface's motion and expected depth, including at sky silhouettes.
    const float4 motion = velocityTex.SampleLevel(pointSampler, uv, 0);
    if (motion.w < 0.5 || !all(isfinite(motion)) || motion.z < 0.0 || motion.z > 1.0)
        return float4(currentRaw, 1.0);

    const float2 prevUv = uv + motion.xy;
    if (any(prevUv < 0.0) || any(prevUv >= 1.0))
        return float4(currentRaw, 1.0);

    // Compare depths in the previous projection. Sky cannot reuse geometry, or vice versa.
    const float currDepth = currentDepthTex.SampleLevel(pointSampler, uv, 0);
    const float prevDepth = prevDepthTex.SampleLevel(pointSampler, prevUv, 0);
    const bool isGeometry = currDepth < 1.0;
    if (!isfinite(currDepth) || !isfinite(prevDepth) ||
        (isGeometry != (motion.w < 1.5)) || (isGeometry != (prevDepth < 1.0)) ||
        (isGeometry && abs(motion.z - prevDepth) > params.thresholds.y))
        return float4(currentRaw, 1.0);

    // 3x3 neighborhood YCoCg variance clipping in compressed HDR space.
    float3 m1 = 0.0.xxx;
    float3 m2 = 0.0.xxx;
    [unroll]
    for (int y = -1; y <= 1; ++y)
    {
        [unroll]
        for (int x = -1; x <= 1; ++x)
        {
            const float3 s = SafeHdr(currentHdr.SampleLevel(pointSampler, uv + float2(x, y) * invExtent, 0).rgb);
            const float3 ycocg = RgbToYCoCg(CompressHdr(s));
            m1 += ycocg;
            m2 += ycocg * ycocg;
        }
    }
    const float3 mean = m1 / 9.0;
    const float3 variance = max(m2 / 9.0 - mean * mean, 0.0.xxx);
    const float3 stddev = sqrt(variance);
    const float gamma = params.thresholds.z;
    const float3 boxMin = mean - gamma * stddev;
    const float3 boxMax = mean + gamma * stddev;

    const float3 historyRaw = SafeHdr(historyHdr.SampleLevel(linearSampler, prevUv, 0).rgb);
    const float3 historyYCoCg = RgbToYCoCg(CompressHdr(historyRaw));
    const float3 clampedHistoryYCoCg = clamp(historyYCoCg, boxMin, boxMax);

    // Motion-adaptive history weight: fast motion lowers history contribution to prevent ghosting.
    const float speed = length(motion.xy - params.jitterAndInvExtent.xy);
    const float motionFactor = exp(-speed * params.thresholds.x);
    const float weight = clamp(
        lerp(params.weights.y, params.weights.x, motionFactor),
        params.weights.y,
        params.weights.z);

    const float3 currentYCoCg = RgbToYCoCg(CompressHdr(currentRaw));
    const float3 resolvedCompressed = YCoCgToRgb(lerp(currentYCoCg, clampedHistoryYCoCg, weight));
    const float3 resolvedLinear = SafeHdr(ExpandHdr(max(resolvedCompressed, 0.0.xxx)));
    return float4(resolvedLinear, 1.0);
}
