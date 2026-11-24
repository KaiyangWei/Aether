struct SkyboxView
{
    column_major float4x4 clipToWorldDirection;
};

[[vk::push_constant]] SkyboxView skybox;

struct SkyboxVertex
{
    float4 position : SV_Position;
    [[vk::location(0)]] float3 direction : TEXCOORD0;
};

SkyboxVertex main(uint id : SV_VertexID)
{
    const float2 corner = float2((id << 1) & 2, id & 2) * 2.0 - 1.0;
    SkyboxVertex vertex;
    vertex.position = float4(corner, 0.0, 1.0);
    // Clip space already includes the camera's Vulkan Y convention.
    vertex.direction = mul(skybox.clipToWorldDirection, float4(corner, 1.0, 1.0)).xyz;
    return vertex;
}
