[[vk::binding(0)]] Texture2D<float4> hdr;
[[vk::constant_id(0)]] const uint encodeSrgb=0;
struct Exposure {float ev;};
[[vk::push_constant]] Exposure exposure;
float4 main(float4 position:SV_Position):SV_Target
{
    float3 x=min(65504.0,max(0,hdr.Load(int3(int2(position.xy),0)).rgb))*exp2(exposure.ev);
    float3 c=saturate(x*(2.51*x+.03)/(x*(2.43*x+.59)+.14));
    if(encodeSrgb)
        c=lerp(12.92*c,1.055*pow(c,1.0/2.4)-.055,step(.0031308,c));
    return float4(c,1);
}
