struct Output
{
    float4 position : SV_Position;
    [[vk::location(0)]] float2 uv : TEXCOORD0;
};

Output main(uint id : SV_VertexID)
{
    const float2 uv = float2((id << 1) & 2, id & 2);
    Output output;
    output.position = float4(uv * 2.0 - 1.0, 0.0, 1.0);
    output.uv = uv;
    return output;
}
