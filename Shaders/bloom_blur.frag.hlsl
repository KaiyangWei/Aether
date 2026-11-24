[[vk::binding(0, 0)]] Texture2D<float4> srcTex0;
[[vk::binding(2, 0)]] SamplerState linearSampler;

struct Push
{
    float2 directionStep;
    float pad0;
    float pad1;
};

[[vk::push_constant]] Push params;

struct Input
{
    [[vk::location(0)]] float2 uv : TEXCOORD0;
};

float3 SampleSafe(float2 uv)
{
    const float3 c = srcTex0.SampleLevel(linearSampler, uv, 0).rgb;
    return min(max(select(isfinite(c), c, 0.0.xxx), 0.0.xxx), 65000.0.xxx);
}

// Separable 9-tap Gaussian blur using 5 bilinearly-paired taps.
float4 main(Input input) : SV_Target0
{
    const float2 step1 = params.directionStep * 1.3846153846;
    const float2 step2 = params.directionStep * 3.2307692308;

    float3 sum = SampleSafe(input.uv) * 0.2270270270;
    sum += SampleSafe(input.uv + step1) * 0.3162162162;
    sum += SampleSafe(input.uv - step1) * 0.3162162162;
    sum += SampleSafe(input.uv + step2) * 0.0702702703;
    sum += SampleSafe(input.uv - step2) * 0.0702702703;
    return float4(sum, 1.0);
}
