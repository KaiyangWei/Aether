struct Input
{
    [[vk::location(0)]] float4 currClip : TEXCOORD0;
    [[vk::location(1)]] float4 prevClip : TEXCOORD1;
};

float4 main(Input input) : SV_Target0
{
    if (input.currClip.w <= 1e-6 || input.prevClip.w <= 1e-6)
        return 0.0.xxxx;
    const float2 currUv = (input.currClip.xy / input.currClip.w) * 0.5 + 0.5;
    const float2 prevUv = (input.prevClip.xy / input.prevClip.w) * 0.5 + 0.5;
    const float prevDepth = input.prevClip.z / input.prevClip.w;
    return float4(prevUv - currUv, prevDepth, 1.0);
}
