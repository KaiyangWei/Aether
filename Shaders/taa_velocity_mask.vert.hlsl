struct Push
{
    column_major float4x4 currMvpJittered;
    column_major float4x4 prevMvpJittered;
};

[[vk::push_constant]] Push draw;

struct Input
{
    [[vk::location(0)]] float3 position : POSITION;
    [[vk::location(1)]] float alpha : COLOR1;
    [[vk::location(2)]] float2 baseColorUv : TEXCOORD0;
};

struct Output
{
    float4 clip : SV_Position;
    [[vk::location(0)]] float4 currClip : TEXCOORD0;
    [[vk::location(1)]] float4 prevClip : TEXCOORD1;
    [[vk::location(2)]] float2 uv : TEXCOORD2;
    [[vk::location(3)]] float alpha : COLOR0;
};

Output main(Input input)
{
    Output output;
    const float4 p = float4(input.position, 1.0);
    const float4 currClip = mul(draw.currMvpJittered, p);
    output.clip = currClip;
    output.currClip = currClip;
    output.prevClip = mul(draw.prevMvpJittered, p);
    output.uv = input.baseColorUv;
    output.alpha = input.alpha;
    return output;
}
