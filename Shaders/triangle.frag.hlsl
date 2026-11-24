struct PSInput
{
    [[vk::location(0)]] float3 Color : COLOR0;
};

struct PSOutput
{
    [[vk::location(0)]] float4 Color : SV_Target0;
};

PSOutput main(PSInput input)
{
    PSOutput output;
    output.Color = float4(input.Color, 1.0f);
    return output;
}