struct Push
{
    column_major float4x4 lightMvp;
};

[[vk::push_constant]] Push draw;

struct Input
{
    [[vk::location(0)]] float3 position : POSITION;
};

float4 main(Input input) : SV_Position
{
    return mul(draw.lightMvp, float4(input.position, 1.0));
}
