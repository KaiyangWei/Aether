[[vk::binding(0,1)]] TextureCube<float4> irradiance;
[[vk::binding(1,1)]] TextureCube<float4> prefiltered;
[[vk::binding(2,1)]] Texture2D<float4> brdf;
[[vk::binding(3,1)]] SamplerState environmentSampler;

struct ShadowData
{
    column_major float4x4 lightViewProj;
    float4 params0; // x: enabled, y: strength, z: receiverBias, w: texelSize
    float4 params1; // x: pcfRadiusTexels, y: normalBias, z: pcfEnabled, w: debugMode
};

[[vk::binding(0, 2)]] ConstantBuffer<ShadowData> shadow;
[[vk::binding(1, 2)]] Texture2D<float> shadowMap;
[[vk::binding(2, 2)]] SamplerComparisonState shadowSampler;

struct Material
{
    float4 baseColor;
    float4 emissiveMetallic;
    float4 surface;
    float4 flags;
};

[[vk::binding(0, 0)]] ConstantBuffer<Material> material;
[[vk::binding(1, 0)]] Texture2D<float4> baseColorMap;
[[vk::binding(2, 0)]] Texture2D<float4> metallicRoughnessMap;
[[vk::binding(3, 0)]] Texture2D<float4> normalMap;
[[vk::binding(4, 0)]] Texture2D<float4> occlusionMap;
[[vk::binding(5, 0)]] Texture2D<float4> emissiveMap;
[[vk::binding(6, 0)]] SamplerState baseColorSampler;
[[vk::binding(7, 0)]] SamplerState metallicRoughnessSampler;
[[vk::binding(8, 0)]] SamplerState normalSampler;
[[vk::binding(9, 0)]] SamplerState occlusionSampler;
[[vk::binding(10, 0)]] SamplerState emissiveSampler;


struct Input
{
    [[vk::location(0)]] float3 position : TEXCOORD0;
    [[vk::location(1)]] float3 normal : TEXCOORD1;
    [[vk::location(2)]] float4 tangent : TEXCOORD2;
    [[vk::location(3)]] float4 color : TEXCOORD3;
    [[vk::location(4)]] float4 colorAndMrUv : TEXCOORD4;
    [[vk::location(5)]] float4 normalAndAoUv : TEXCOORD5;
    [[vk::location(6)]] float2 emissiveUv : TEXCOORD6;
    [[vk::location(7)]] nointerpolation float4 eyeAndLightAngle : TEXCOORD7;
    bool frontFace : SV_IsFrontFace;
};

float3 SafeNormal(float3 v)
{
    return v * rsqrt(max(dot(v, v), 1e-20));
}

float3 SurfaceNormal(Input input, float3 view, float3 px, float3 py, float2 ux, float2 uy)
{
    const float3 geometric = SafeNormal(cross(px, py));
    float3 n = SafeNormal(input.normal);
    if (dot(n, n) < 0.5)
    {
        n = dot(geometric, view) < 0 ? -geometric : geometric;
        if (!input.frontFace)
            n = -n;
    }

    if (material.flags.y > 0.5)
    {
        float3 t = input.tangent.xyz;
        float handedness = input.tangent.w;
        if (dot(t, t) < 1e-16)
        {
            const float det = ux.x * uy.y - ux.y * uy.x;
            if (abs(det) > 1e-12)
            {
                t = (px * uy.y - py * ux.y) / det;
                const float3 b = (py * ux.x - px * uy.x) / det;
                handedness = dot(cross(n, t), b) < 0 ? -1 : 1;
            }
        }
        t = SafeNormal(t - n * dot(n, t));
        if (dot(t, t) > 0.5)
        {
            const float3 b = cross(n, t) * handedness;
            float3 sampled = normalMap.SampleGrad(normalSampler, input.normalAndAoUv.xy, ux, uy).xyz * 2 - 1;
            sampled.xy *= material.surface.y;
            n = SafeNormal(t * sampled.x + b * sampled.y + n * sampled.z);
        }
    }
    return input.frontFace ? n : -n;
}

float Smith(float nDotX, float alpha2)
{
    return 2 * nDotX / max(nDotX + sqrt(alpha2 + (1 - alpha2) * nDotX * nDotX), 1e-5);
}

float3 Light(float3 n, float3 view, float3 l, float3 radiance, float3 base, float metallic, float roughness)
{
    const float pi = 3.14159265;
    const float nl = saturate(dot(n, l));
    const float nv = saturate(dot(n, view));
    const float3 h = SafeNormal(view + l);
    const float nh = saturate(dot(n, h));
    const float vh = saturate(dot(view, h));
    const float alpha = roughness * roughness;
    const float alpha2 = alpha * alpha;
    const float term = nh * nh * (alpha2 - 1) + 1;
    const float d = alpha2 / (pi * term * term);
    const float g = Smith(nl, alpha2) * Smith(nv, alpha2);
    const float3 f0 = lerp(0.04.xxx, base, metallic);
    const float3 f = f0 + (1 - f0) * pow(1 - vh, 5);
    const float3 specular = d * g * f / max(4 * nl * nv, 1e-5);
    const float3 diffuse = (1 - f) * (1 - metallic) * base / pi;
    return (diffuse + specular) * radiance * nl;
}

float ComputeDirectionalShadow(float3 worldPos, float3 geometricNormal, float3 lightDir)
{
    if (shadow.params0.x < 0.5)
        return 1.0;

    const float nDotL = saturate(dot(geometricNormal, lightDir));
    const float3 biasedPos = worldPos + geometricNormal * (shadow.params1.y * (1.0 - nDotL * 0.7));
    const float4 clip = mul(shadow.lightViewProj, float4(biasedPos, 1.0));
    if (abs(clip.w) <= 1e-6)
        return 1.0;

    const float3 ndc = clip.xyz / clip.w;
    const float2 uv = ndc.xy * 0.5 + 0.5;
    const float depth = ndc.z;
    if (uv.x < 0.0 || uv.x > 1.0 || uv.y < 0.0 || uv.y > 1.0 || depth <= 0.0 || depth >= 1.0)
        return 1.0;

    const float bias = shadow.params0.z * max(1.0 - nDotL, 0.25);
    const float compareDepth = saturate(depth - bias);

    float visibility = 1.0;
    if (shadow.params1.z > 0.5)
    {
        const float2 texelStep = shadow.params0.w * shadow.params1.x;
        float sum = 0.0;
        [unroll]
        for (int y = -1; y <= 1; ++y)
        {
            [unroll]
            for (int x = -1; x <= 1; ++x)
            {
                sum += shadowMap.SampleCmpLevelZero(shadowSampler, uv + float2(x, y) * texelStep, compareDepth);
            }
        }
        visibility = sum / 9.0;
    }
    else
    {
        visibility = shadowMap.SampleCmpLevelZero(shadowSampler, uv, compareDepth);
    }

    return lerp(1.0, visibility, saturate(shadow.params0.y));
}

float4 main(Input input) : SV_Target0
{
    const float3 px = ddx(input.position);
    const float3 py = ddy(input.position);
    const float2 ux = ddx(input.normalAndAoUv.xy);
    const float2 uy = ddy(input.normalAndAoUv.xy);
    const float4 base = material.baseColor * input.color *
        baseColorMap.Sample(baseColorSampler, input.colorAndMrUv.xy);
    const float3 mr = metallicRoughnessMap.Sample(metallicRoughnessSampler, input.colorAndMrUv.zw).rgb;
    const float metallic = saturate(material.emissiveMetallic.w * mr.b);
    const float roughness = clamp(material.surface.x * mr.g, 0.045, 1);
    const float ao = lerp(1.0, occlusionMap.Sample(occlusionSampler, input.normalAndAoUv.zw).r, material.surface.z);
    const float3 emissive = material.emissiveMetallic.xyz * emissiveMap.Sample(emissiveSampler, input.emissiveUv).rgb;
    const float3 toEye = input.eyeAndLightAngle.xyz - input.position;
    const float3 view = SafeNormal(toEye);
    const float3 n = SurfaceNormal(input, view, px, py, ux, uy);
    const float3 rawGeomN = SafeNormal(input.normal);
    const float3 shadowNormal = dot(rawGeomN, rawGeomN) > 0.5 ? (input.frontFace ? rawGeomN : -rawGeomN) : n;
    const float angle = input.eyeAndLightAngle.w;
    const float3 key = SafeNormal(float3(cos(angle), 0.8, sin(angle)));
    const float3 fill = SafeNormal(float3(-sin(angle), 0.4, -cos(angle)));
    const float attenuation = 6 / max(dot(toEye, toEye), 0.15);
    const float shadowVisibility = ComputeDirectionalShadow(input.position, shadowNormal, key);
    float3 color = Light(n, view, key, float3(4.8, 4.3, 3.9), base.rgb, metallic, roughness) * shadowVisibility;
    color += Light(n, view, fill, float3(0.9, 1.2, 1.8), base.rgb, metallic, roughness);
    color += Light(n, view, view, attenuation.xxx, base.rgb, metallic, roughness);
    const float nv=saturate(dot(n,view));
    const float3 f0=lerp(0.04.xxx,base.rgb,metallic);
    const float2 ab=brdf.SampleLevel(environmentSampler,float2(nv,roughness),0).rg;
    const float3 specularWeight=f0*ab.x+ab.y;
    const float3 diffuseWeight=(1-saturate(specularWeight))*(1-metallic);
    const float3 indirectDiffuse=irradiance.SampleLevel(environmentSampler,n,0).rgb*base.rgb*diffuseWeight;
    const float3 indirectSpecular=prefiltered.SampleLevel(environmentSampler,reflect(-view,n),roughness*7).rgb*specularWeight;
    color+=(indirectDiffuse+indirectSpecular)*ao+emissive;


    if (material.flags.x == 1)
        clip(base.a - material.surface.w);
    return float4(color, material.flags.x == 2 ? base.a : 1);
}
