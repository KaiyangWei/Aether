struct VSInput
{
    [[vk::location(0)]] float3 Position : POSITION0;
    [[vk::location(1)]] float3 Color : COLOR0;
};

struct VSOutput
{
    float4 Position : SV_Position;
    [[vk::location(0)]] float3 Color : COLOR0;
};

struct PushConstants
{
    column_major float4x4 MVP;
};

[[vk::push_constant]]
PushConstants g_PushConstants;

VSOutput main(VSInput input)
{
    VSOutput output;
    output.Position = mul(g_PushConstants.MVP, float4(input.Position, 1.0f));
    output.Color = input.Color;
    return output;
}