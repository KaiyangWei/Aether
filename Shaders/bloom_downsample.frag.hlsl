[[vk::binding(0, 0)]] Texture2D<float4> srcTex0;
[[vk::binding(2, 0)]] SamplerState linearSampler;

struct Push
{
    float2 invSrcExtent;
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

// 13-tap Jimenez tent/box downsample filter to suppress aliasing and fireflies.
float4 main(Input input) : SV_Target0
{
    const float2 t = params.invSrcExtent;
    const float2 uv = input.uv;

    const float3 a = SampleSafe(uv + float2(-2.0 * t.x,  2.0 * t.y));
    const float3 b = SampleSafe(uv + float2( 0.0,        2.0 * t.y));
    const float3 c = SampleSafe(uv + float2( 2.0 * t.x,  2.0 * t.y));

    const float3 d = SampleSafe(uv + float2(-2.0 * t.x,  0.0));
    const float3 e = SampleSafe(uv);
    const float3 f = SampleSafe(uv + float2( 2.0 * t.x,  0.0));

    const float3 g = SampleSafe(uv + float2(-2.0 * t.x, -2.0 * t.y));
    const float3 h = SampleSafe(uv + float2( 0.0,       -2.0 * t.y));
    const float3 i = SampleSafe(uv + float2( 2.0 * t.x, -2.0 * t.y));

    const float3 j = SampleSafe(uv + float2(-t.x,  t.y));
    const float3 k = SampleSafe(uv + float2( t.x,  t.y));
    const float3 l = SampleSafe(uv + float2(-t.x, -t.y));
    const float3 m = SampleSafe(uv + float2( t.x, -t.y));

    float3 result = e * 0.125;
    result += (a + c + g + i) * 0.03125;
    result += (b + d + f + h) * 0.0625;
    result += (j + k + l + m) * 0.125;
    return float4(result, 1.0);
}
