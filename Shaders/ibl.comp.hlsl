[[vk::binding(0)]] TextureCube<float4> sky;
[[vk::binding(1)]] SamplerState linearClamp;
[[vk::binding(2)]] RWStructuredBuffer<float4> output;
struct Params { uint side, offset, mode; float roughness; };
[[vk::push_constant]] Params pc;
float3 FaceDirection(uint face,float2 uv)
{
    float u=uv.x,v=uv.y;
    if(face==0) return normalize(float3(1,-v,-u));
    if(face==1) return normalize(float3(-1,-v,u));
    if(face==2) return normalize(float3(u,1,v));
    if(face==3) return normalize(float3(u,-1,-v));
    if(face==4) return normalize(float3(u,-v,1));
    return normalize(float3(-u,-v,-1));
}
float SmithTerm(float x,float a2) {return 2*x/max(x+sqrt(a2+(1-a2)*x*x),1e-6);}
[numthreads(8,8,1)]
void main(uint3 id:SV_DispatchThreadID)
{
    if(any(id.xy>=pc.side)) return;
    float2 uv=(id.xy+0.5)/pc.side;
    float3 n=FaceDirection(id.z,uv*2-1);
    float3 t=normalize(cross(abs(n.y)<.99?float3(0,1,0):float3(1,0,0),n));
    float3 b=cross(n,t);
    float rough=pc.mode==2?uv.y:pc.roughness;
    float a2=pow(max(.045,rough),4);
    float nv=uv.x;
    float3 view=float3(sqrt(1-nv*nv),0,nv);
    float3 sum=0;
    float weight=0;
    const uint count=256;
    for(uint i=0;i<count;++i)
    {
        float phi=6.2831853*i/count;
        float xi=float(reversebits(i))*2.3283064365386963e-10;
        float z=pc.mode==0?sqrt(1-xi):sqrt((1-xi)/(1+(a2-1)*xi));
        float s=sqrt(saturate(1-z*z));
        float3 h=float3(cos(phi)*s,sin(phi)*s,z);
        if(pc.mode==2)
        {
            float vh=saturate(dot(view,h));
            float nl=2*vh*z-nv;
            if(nl>0)
            {
                float vis=SmithTerm(nv,a2)*SmithTerm(nl,a2)*vh/max(z*nv,1e-6);
                float fc=pow(1-vh,5);
                sum.xy+=float2(1-fc,fc)*vis;
            }
        }
        else
        {
            float3 l=pc.mode==0?h:2*z*h-float3(0,0,1);
            float w=pc.mode==0?1:saturate(l.z);
            sum+=sky.SampleLevel(linearClamp,t*l.x+b*l.y+n*l.z,0).rgb*w;
            weight+=w;
        }
    }
    output[pc.offset+(id.z*pc.side+id.y)*pc.side+id.x]=float4(sum/(pc.mode==2?count:max(weight,.00001)),1);
}
