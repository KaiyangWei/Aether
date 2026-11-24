struct AtrousPush
{
    uint  width;
    uint  height;
    int   stepSize;
    uint  iterIndex;
    float minMirrorRoughness;
    float sigmaLuma;
    float sigmaNormal;
    float sigmaDepth;
};

[[vk::push_constant]] AtrousPush g_push;

[[vk::binding(0, 0)]] Texture2D<float4> g_rt1NormalRoughness;
[[vk::binding(1, 0)]] Texture2D<float4> g_rt3WorldPosInst;
[[vk::binding(2, 0)]] Texture2D<float4> g_shadowIn;
[[vk::binding(3, 0)]] Texture2D<float4> g_reflIn;
[[vk::binding(4, 0)]] Texture2D<float2> g_varConfIn;
[[vk::image_format("rgba16f")]] [[vk::binding(5, 0)]] RWTexture2D<float4> g_shadowOut;
[[vk::image_format("rgba16f")]] [[vk::binding(6, 0)]] RWTexture2D<float4> g_reflOut;

float3 SafeNormal(float3 v)
{
    return v * rsqrt(max(dot(v, v), 1e-20));
}

float Luminance(float3 c)
{
    return max(0.0, dot(c, float3(0.2126, 0.7152, 0.0722)));
}

[numthreads(8, 8, 1)]
void main(uint3 tid : SV_DispatchThreadID)
{
    if (tid.x >= g_push.width || tid.y >= g_push.height)
        return;

    const float4 cRt3 = g_rt3WorldPosInst.Load(int3(tid.xy, 0));
    const float4 cShadow = g_shadowIn.Load(int3(tid.xy, 0));
    const float4 cRefl = g_reflIn.Load(int3(tid.xy, 0));

    if (cRt3.w <= 0.5)
    {
        g_shadowOut[tid.xy] = float4(0.0, 0.0, 0.0, 0.0);
        g_reflOut[tid.xy] = float4(0.0, 0.0, 0.0, 0.0);
        return;
    }

    const float4 cRt1 = g_rt1NormalRoughness.Load(int3(tid.xy, 0));
    const float3 cN = SafeNormal(cRt1.rgb);
    const float cRoughness = cRt1.a;
    const float3 cPos = cRt3.xyz;
    const uint cInstId = (uint)max(0.0, cRt3.w - 0.5);
    const float2 varConf = g_varConfIn.Load(int3(tid.xy, 0));
    const float stdDev = sqrt(max(varConf.x, 1e-5));

    const float cShadowLum = Luminance(cShadow.rgb);
    const float cReflLum = Luminance(cRefl.rgb);
    const float cVis = cShadow.a;
    const bool filterReflection = (cRoughness > g_push.minMirrorRoughness);

    static const float kKernel1D[5] = { 0.0625, 0.25, 0.375, 0.25, 0.0625 };

    float4 accumShadow = float4(0.0, 0.0, 0.0, 0.0);
    float shadowWeightSum = 0.0;

    float3 accumRefl = float3(0.0, 0.0, 0.0);
    float reflWeightSum = 0.0;

    const float shadowPhi = max(0.18, g_push.sigmaLuma * stdDev);
    const float reflPhi = max(0.25, g_push.sigmaLuma * stdDev * (0.4 + cRoughness));

    [unroll]
    for (int dy = -2; dy <= 2; ++dy)
    {
        [unroll]
        for (int dx = -2; dx <= 2; ++dx)
        {
            int2 sp = int2(tid.xy) + int2(dx, dy) * g_push.stepSize;
            if (sp.x < 0 || sp.x >= int(g_push.width) || sp.y < 0 || sp.y >= int(g_push.height))
                continue;

            float kW = kKernel1D[dx + 2] * kKernel1D[dy + 2];
            if (dx == 0 && dy == 0)
            {
                accumShadow += cShadow * kW;
                shadowWeightSum += kW;
                if (filterReflection)
                {
                    accumRefl += cRefl.rgb * kW;
                    reflWeightSum += kW;
                }
                continue;
            }

            float4 nRt3 = g_rt3WorldPosInst.Load(int3(sp, 0));
            uint nInstId = (uint)max(0.0, nRt3.w - 0.5);
            if (nRt3.w <= 0.5 || nInstId != cInstId)
                continue;

            float4 nRt1 = g_rt1NormalRoughness.Load(int3(sp, 0));
            float3 nN = SafeNormal(nRt1.rgb);
            float wNormal = pow(saturate(dot(cN, nN)), g_push.sigmaNormal);
            if (wNormal <= 1e-4)
                continue;

            float planeDist = abs(dot(nRt3.xyz - cPos, cN));
            float wDepth = exp(-planeDist / max(g_push.sigmaDepth * max(1.0, float(g_push.stepSize)), 1e-4));

            float4 nShadow = g_shadowIn.Load(int3(sp, 0));
            float nShadowLum = Luminance(nShadow.rgb);
            float wShadowLum = exp(-abs(cShadowLum - nShadowLum) / shadowPhi - abs(cVis - nShadow.a) * 3.5);
            float wS = kW * wNormal * wDepth * wShadowLum;
            accumShadow += nShadow * wS;
            shadowWeightSum += wS;

            if (filterReflection)
            {
                float3 nRefl = g_reflIn.Load(int3(sp, 0)).rgb;
                float nReflLum = Luminance(nRefl);
                float roughDiff = abs(cRoughness - nRt1.a);
                float wRough = exp(-roughDiff * 16.0);
                float wReflLum = exp(-abs(cReflLum - nReflLum) / reflPhi);
                float wR = kW * wNormal * wDepth * wRough * wReflLum;
                accumRefl += nRefl * wR;
                reflWeightSum += wR;
            }
        }
    }

    g_shadowOut[tid.xy] = shadowWeightSum > 1e-6 ? (accumShadow / shadowWeightSum) : cShadow;
    g_reflOut[tid.xy] = (filterReflection && reflWeightSum > 1e-6)
        ? float4(accumRefl / reflWeightSum, cRefl.a)
        : cRefl;
}
