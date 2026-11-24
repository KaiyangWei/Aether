struct Push
{
    column_major float4x4 lightMvp;
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
    [[vk::location(0)]] float2 uv : TEXCOORD0;
    [[vk::location(1)]] float alpha : COLOR0;
};

Output main(Input input)
{
    Output output;
    output.clip = mul(draw.lightMvp, float4(input.position, 1.0));
    output.uv = input.baseColorUv;
    output.alpha = input.alpha;
    return output;
}
