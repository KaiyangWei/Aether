struct Push
{
    column_major float4x4 currClipToPrevClipRot;
};

[[vk::push_constant]] Push sky;

struct Input
{
    [[vk::location(0)]] float2 uv : TEXCOORD0;
};

float4 main(Input input) : SV_Target0
{
    const float2 currNdc = input.uv * 2.0 - 1.0;
    const float4 prevClip = mul(sky.currClipToPrevClipRot, float4(currNdc, 1.0, 1.0));
    if (prevClip.w <= 1e-5)
        return 0.0.xxxx;
    const float2 prevUv = (prevClip.xy / prevClip.w) * 0.5 + 0.5;
    return float4(prevUv - input.uv, 1.0, 2.0);
}
