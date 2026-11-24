struct Push
{
    column_major float4x4 mvp;
    float4 model0;
    float4 model1;
    float4 model2;
    float4 eyeAndLightAngle;
};

[[vk::push_constant]] Push draw;

struct Input
{
    [[vk::location(0)]] float3 position : POSITION;
    [[vk::location(1)]] float3 color : COLOR0;
    [[vk::location(2)]] float3 normal : NORMAL;
    [[vk::location(3)]] float4 tangent : TANGENT;
    [[vk::location(4)]] float alpha : COLOR1;
    [[vk::location(5)]] float4 colorAndMrUv : TEXCOORD0;
    [[vk::location(6)]] float4 normalAndAoUv : TEXCOORD1;
    [[vk::location(7)]] float2 emissiveUv : TEXCOORD2;
};

struct Output
{
    float4 clip : SV_Position;
    [[vk::location(0)]] float3 position : TEXCOORD0;
    [[vk::location(1)]] float3 normal : TEXCOORD1;
    [[vk::location(2)]] float4 tangent : TEXCOORD2;
    [[vk::location(3)]] float4 color : TEXCOORD3;
    [[vk::location(4)]] float4 colorAndMrUv : TEXCOORD4;
    [[vk::location(5)]] float4 normalAndAoUv : TEXCOORD5;
    [[vk::location(6)]] float2 emissiveUv : TEXCOORD6;
    [[vk::location(7)]] nointerpolation float4 eyeAndLightAngle : TEXCOORD7;
};

Output main(Input input)
{
    Output output;
    const float4 p = float4(input.position, 1);
    output.clip = mul(draw.mvp, p);
    output.position = float3(dot(draw.model0, p), dot(draw.model1, p), dot(draw.model2, p));

    const float3 x = float3(draw.model0.x, draw.model1.x, draw.model2.x);
    const float3 y = float3(draw.model0.y, draw.model1.y, draw.model2.y);
    const float3 z = float3(draw.model0.z, draw.model1.z, draw.model2.z);
    const float3 cx = cross(y, z);
    const float3 cy = cross(z, x);
    const float3 cz = cross(x, y);
    const float orientation = dot(x, cx) < 0 ? -1 : 1;
    // Cofactors yield the inverse transpose up to a scale, removed by normalization.
    output.normal = orientation * (cx * input.normal.x + cy * input.normal.y + cz * input.normal.z);
    output.tangent = float4(x * input.tangent.x + y * input.tangent.y + z * input.tangent.z,
                           input.tangent.w * orientation);
    output.color = float4(input.color, input.alpha);
    output.colorAndMrUv = input.colorAndMrUv;
    output.normalAndAoUv = input.normalAndAoUv;
    output.emissiveUv = input.emissiveUv;
    output.eyeAndLightAngle = draw.eyeAndLightAngle;
    return output;
}
