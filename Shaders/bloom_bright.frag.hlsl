[[vk::binding(0, 0)]] Texture2D<float4> srcTex0;
[[vk::binding(2, 0)]] SamplerState linearSampler;

struct Push
{
    float2 invSrcExtent;
    float threshold;
    float softKnee;
};

[[vk::push_constant]] Push params;

struct Input
{
    [[vk::location(0)]] float2 uv : TEXCOORD0;
};

float3 SafeHdr(float3 c)
{
    return min(max(select(isfinite(c), c, 0.0.xxx), 0.0.xxx), 65000.0.xxx);
}

float4 main(Input input) : SV_Target0
{
    const float2 d = params.invSrcExtent * 0.5;
    float3 c = 0.0.xxx;
    c += SafeHdr(srcTex0.SampleLevel(linearSampler, input.uv + float2(-d.x, -d.y), 0).rgb);
    c += SafeHdr(srcTex0.SampleLevel(linearSampler, input.uv + float2( d.x, -d.y), 0).rgb);
    c += SafeHdr(srcTex0.SampleLevel(linearSampler, input.uv + float2(-d.x,  d.y), 0).rgb);
    c += SafeHdr(srcTex0.SampleLevel(linearSampler, input.uv + float2( d.x,  d.y), 0).rgb);
    c *= 0.25;

    const float lum = dot(c, float3(0.2126, 0.7152, 0.0722));
    const float knee = max(params.threshold * params.softKnee, 1e-5);
    float soft = clamp(lum - params.threshold + knee, 0.0, 2.0 * knee);
    soft = (soft * soft) / (4.0 * knee);
    const float contribution = max(soft, lum - params.threshold) / max(lum, 1e-5);
    return float4(c * max(contribution, 0.0), 1.0);
}
