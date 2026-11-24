#pragma once

#include <glm/glm.hpp>
#include <glm/gtc/constants.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace Aether
{
    enum class RtLightType : uint32_t
    {
        Directional = 0,
        Point = 1,
        Spot = 2,
        RectArea = 3
    };

    constexpr uint32_t kMaxRtSceneLights = 8;
    constexpr uint32_t kMaxRtAlphaMaskTextures = 8;

    // 80-byte std140/std430 GPU light descriptor shared across RT Shadow and RT Reflection passes.
    struct RtGpuLight
    {
        glm::vec4 posAndType{ 0.0f };          // xyz = world position (Point/Spot/Area), w = float(RtLightType)
        glm::vec4 dirAndRange{ 0.0f, -1.0f, 0.0f, 10.0f }; // xyz = emission/toLight dir, w = range or angularRadius
        glm::vec4 colorAndIntensity{ 1.0f };   // rgb = linear color, w = luminous intensity / radiance
        glm::vec4 areaRightAndHalfW{ 1.0f, 0.0f, 0.0f, 0.5f }; // xyz = unit right axis, w = halfWidth or cosOuter
        glm::vec4 areaUpAndHalfH{ 0.0f, 1.0f, 0.0f, 0.5f };    // xyz = unit up axis, w = halfHeight or cosInner
    };
    static_assert(sizeof(RtGpuLight) == 80);

    // Compact CPU material description carried from Scene/ImportedAsset into RT SSBOs without extra VMA buffers.
    struct RtMaterialSummary
    {
        glm::vec4 baseColor{ 1.0f };
        glm::vec3 emissive{ 0.0f };
        float metallic = 0.0f;
        float roughness = 0.5f;
        float ao = 1.0f;
        float alphaCutoff = 0.5f;
        bool alphaMask = false;
        bool doubleSided = false;
    };

    // Consolidated vertex format packed once per BLAS mesh into VulkanRayTracingScene's SSBO.
    struct RtGpuVertex
    {
        glm::vec4 posAndU{ 0.0f };    // xyz = object position, w = baseColor/mr u
        glm::vec4 normalAndV{ 0.0f }; // xyz = object normal, w = baseColor/mr v
    };
    static_assert(sizeof(RtGpuVertex) == 32);

    // 208-byte std430 instance record indexed 1:1 by TLAS instanceCustomIndex and G-Buffer instanceId.
    struct RtGpuInstanceData
    {
        glm::vec4 baseColorAndAlphaCutoff{ 1.0f, 1.0f, 1.0f, 0.5f };
        glm::vec4 emissiveAndMetallic{ 0.0f }; // rgb = emissive, a = metallic
        glm::vec4 surfaceParams{ 0.5f, 1.0f, 1.0f, 0.0f }; // x = roughness, y = ao, z = baseAlpha, w = flags (bit0: alphaMask, bit1: doubleSided, bit2: mirrored)
        glm::vec4 modelRows[3]{
            { 1.0f, 0.0f, 0.0f, 0.0f },
            { 0.0f, 1.0f, 0.0f, 0.0f },
            { 0.0f, 0.0f, 1.0f, 0.0f }
        };
        glm::vec4 prevModelRows[3]{
            { 1.0f, 0.0f, 0.0f, 0.0f },
            { 0.0f, 1.0f, 0.0f, 0.0f },
            { 0.0f, 0.0f, 1.0f, 0.0f }
        };
        glm::vec4 invModelRows[3]{
            { 1.0f, 0.0f, 0.0f, 0.0f },
            { 0.0f, 1.0f, 0.0f, 0.0f },
            { 0.0f, 0.0f, 1.0f, 0.0f }
        };
        uint32_t firstVertex = 0;
        uint32_t firstIndex = 0;
        uint32_t alphaMaskTexIndex = 0;
        uint32_t sceneObjectIndex = 0;
    };
    static_assert(sizeof(RtGpuInstanceData) == 208);

    enum class HybridDebugView : uint32_t
    {
        FinalHybrid = 0,
        RtShadowsOnly = 1,
        RtReflectionsOnly = 2,
        ShadowVisibility = 3,
        ShadowSampleCount = 4,
        ReflectionContribution = 5,
        ReflectionHitMiss = 6,
        ReflectionHitDistance = 7,
        SurfaceRoughness = 8,
        TemporalConfidence = 9,
        DenoiserVariance = 10,
        RayQueryDiagnostic = 11
    };

    constexpr uint32_t kHybridDebugViewCount = 12;

    inline const char* HybridDebugViewToString(HybridDebugView view) noexcept
    {
        switch (view)
        {
        case HybridDebugView::FinalHybrid:            return "Final Hybrid Lighting";
        case HybridDebugView::RtShadowsOnly:          return "RT Shadows Only";
        case HybridDebugView::RtReflectionsOnly:      return "RT Reflections Only";
        case HybridDebugView::ShadowVisibility:       return "Shadow Visibility Mask";
        case HybridDebugView::ShadowSampleCount:      return "Shadow Sample Count";
        case HybridDebugView::ReflectionContribution: return "Reflection Radiance (Raw/Denoised)";
        case HybridDebugView::ReflectionHitMiss:      return "Reflection Hit / Miss";
        case HybridDebugView::ReflectionHitDistance:  return "Reflection Hit Distance";
        case HybridDebugView::SurfaceRoughness:       return "Surface Roughness";
        case HybridDebugView::TemporalConfidence:     return "Denoiser Temporal Confidence";
        case HybridDebugView::DenoiserVariance:       return "Denoiser Spatial Variance";
        case HybridDebugView::RayQueryDiagnostic:     return "Ray Query Primary Diagnostic";
        default:                                      return "Unknown";
        }
    }

    inline HybridDebugView CycleHybridDebugView(HybridDebugView current) noexcept
    {
        const uint32_t next = (static_cast<uint32_t>(current) + 1u) % kHybridDebugViewCount;
        return static_cast<HybridDebugView>(next);
    }

    struct HybridRtConfig
    {
        bool shadowsEnabled = true;
        bool reflectionsEnabled = true;
        bool denoiserEnabled = true;
        bool areaLightsEnabled = true;
        uint32_t shadowSamples = 4;
        float sunAngularRadiusDeg = 1.6f;
        float shadowNormalBias = 0.004f;
        float shadowDistanceBias = 0.0015f;
        float reflectionMaxRoughness = 0.72f;
        float reflectionMinRoughnessMirror = 0.05f;
        float reflectionMaxDistance = 40.0f;
        float temporalAlphaMin = 0.06f;
        float temporalAlphaMax = 0.25f;
        uint32_t atrousIterations = 3;
        HybridDebugView debugView = HybridDebugView::FinalHybrid;
        bool resetHistory = false;
    };

    inline HybridRtConfig SanitizeHybridRtConfig(const HybridRtConfig& input) noexcept
    {
        HybridRtConfig out = input;
        out.shadowSamples = std::clamp(out.shadowSamples, 1u, 16u);
        if (!std::isfinite(out.sunAngularRadiusDeg))
            out.sunAngularRadiusDeg = 1.6f;
        out.sunAngularRadiusDeg = std::clamp(out.sunAngularRadiusDeg, 0.0f, 15.0f);
        if (!std::isfinite(out.shadowNormalBias))
            out.shadowNormalBias = 0.004f;
        out.shadowNormalBias = std::clamp(out.shadowNormalBias, 0.0f, 0.2f);
        if (!std::isfinite(out.shadowDistanceBias))
            out.shadowDistanceBias = 0.0015f;
        out.shadowDistanceBias = std::clamp(out.shadowDistanceBias, 0.0f, 0.05f);
        if (!std::isfinite(out.reflectionMaxRoughness))
            out.reflectionMaxRoughness = 0.72f;
        out.reflectionMaxRoughness = std::clamp(out.reflectionMaxRoughness, 0.05f, 1.0f);
        if (!std::isfinite(out.reflectionMinRoughnessMirror))
            out.reflectionMinRoughnessMirror = 0.05f;
        out.reflectionMinRoughnessMirror = std::clamp(out.reflectionMinRoughnessMirror, 0.0f, 0.25f);
        if (!std::isfinite(out.reflectionMaxDistance) || out.reflectionMaxDistance <= 0.5f)
            out.reflectionMaxDistance = 40.0f;
        out.reflectionMaxDistance = std::clamp(out.reflectionMaxDistance, 1.0f, 1000.0f);
        if (!std::isfinite(out.temporalAlphaMin))
            out.temporalAlphaMin = 0.06f;
        out.temporalAlphaMin = std::clamp(out.temporalAlphaMin, 0.01f, 1.0f);
        if (!std::isfinite(out.temporalAlphaMax))
            out.temporalAlphaMax = 0.25f;
        out.temporalAlphaMax = std::clamp(out.temporalAlphaMax, out.temporalAlphaMin, 1.0f);
        out.atrousIterations = std::clamp(out.atrousIterations, 0u, 5u);
        if (static_cast<uint32_t>(out.debugView) >= kHybridDebugViewCount)
            out.debugView = HybridDebugView::FinalHybrid;
        return out;
    }

    // Normal + distance-scaled shadow ray origin bias (matches rt_shadow.comp.hlsl).
    inline glm::vec3 ComputeShadowRayOrigin(
        const glm::vec3& worldPos,
        const glm::vec3& shadingNormal,
        const glm::vec3& viewDir,
        float viewDistance,
        float normalBias = 0.004f,
        float distanceBias = 0.0015f) noexcept
    {
        const float dist = std::max(0.0f, viewDistance);
        const glm::vec3 n = glm::dot(shadingNormal, shadingNormal) > 1e-12f
            ? glm::normalize(shadingNormal)
            : glm::vec3(0.0f, 1.0f, 0.0f);
        const glm::vec3 v = glm::dot(viewDir, viewDir) > 1e-12f
            ? glm::normalize(viewDir)
            : n;
        const float nOffset = normalBias + dist * distanceBias;
        const float vOffset = normalBias * 0.75f + dist * distanceBias * 0.5f;
        return worldPos + n * nOffset + v * vOffset;
    }

    inline glm::vec3 EvaluateSchlickFresnel(float cosTheta, const glm::vec3& f0) noexcept
    {
        const float clamped = std::clamp(1.0f - std::clamp(cosTheta, 0.0f, 1.0f), 0.0f, 1.0f);
        const float f = std::pow(clamped, 5.0f);
        return f0 + (glm::vec3(1.0f) - f0) * f;
    }

    // Heitz (2018) GGX Visible Normal Distribution Function (VNDF) sampling in local Z-up tangent space.
    inline glm::vec3 SampleGgxVndf(
        const glm::vec3& viewLocal,
        float alpha,
        float u1,
        float u2) noexcept
    {
        const float a = std::clamp(alpha, 0.001f, 1.0f);
        const glm::vec3 vRaw(a * viewLocal.x, a * viewLocal.y, std::max(viewLocal.z, 1e-4f));
        const glm::vec3 Vh = glm::normalize(vRaw);

        const float lensq = Vh.x * Vh.x + Vh.y * Vh.y;
        const glm::vec3 T1 = lensq > 1e-12f
            ? glm::vec3(-Vh.y, Vh.x, 0.0f) / std::sqrt(lensq)
            : glm::vec3(1.0f, 0.0f, 0.0f);
        const glm::vec3 T2 = glm::cross(Vh, T1);

        const float r = std::sqrt(std::clamp(u1, 0.0f, 1.0f));
        const float phi = 2.0f * glm::pi<float>() * std::clamp(u2, 0.0f, 1.0f);
        const float t1 = r * std::cos(phi);
        float t2 = r * std::sin(phi);
        const float s = 0.5f * (1.0f + Vh.z);
        t2 = (1.0f - s) * std::sqrt(std::max(0.0f, 1.0f - t1 * t1)) + s * t2;

        const glm::vec3 Nh = t1 * T1 + t2 * T2 + std::sqrt(std::max(0.0f, 1.0f - t1 * t1 - t2 * t2)) * Vh;
        return glm::normalize(glm::vec3(a * Nh.x, a * Nh.y, std::max(0.0f, Nh.z)));
    }

    // Monte Carlo estimator weight (irradiance factor before BRDF) for a rectangular area light.
    // Dividing by light area PDF makes the expected mean invariant to stochastic sample count!
    inline bool EvaluateRectAreaLightSample(
        const glm::vec3& worldPos,
        const glm::vec3& normal,
        const RtGpuLight& light,
        float u1,
        float u2,
        glm::vec3& outSamplePos,
        glm::vec3& outDirToLight,
        float& outDistance,
        glm::vec3& outIncomingRadiance) noexcept
    {
        if (static_cast<uint32_t>(light.posAndType.w + 0.5f) != static_cast<uint32_t>(RtLightType::RectArea))
            return false;

        const glm::vec3 center(light.posAndType);
        const glm::vec3 rawEmitDir(light.dirAndRange);
        const glm::vec3 emitNormal = glm::dot(rawEmitDir, rawEmitDir) > 1e-12f
            ? glm::normalize(rawEmitDir)
            : glm::vec3(0.0f, -1.0f, 0.0f);
        const glm::vec3 right(light.areaRightAndHalfW);
        const glm::vec3 up(light.areaUpAndHalfH);
        const float halfW = std::max(1e-4f, light.areaRightAndHalfW.w);
        const float halfH = std::max(1e-4f, light.areaUpAndHalfH.w);
        const float area = 4.0f * halfW * halfH;

        const float sx = std::clamp(u1, 0.0f, 1.0f) * 2.0f - 1.0f;
        const float sy = std::clamp(u2, 0.0f, 1.0f) * 2.0f - 1.0f;
        outSamplePos = center + right * (sx * halfW) + up * (sy * halfH);

        const glm::vec3 toLight = outSamplePos - worldPos;
        const float distSq = glm::dot(toLight, toLight);
        if (distSq <= 1e-6f)
            return false;

        outDistance = std::sqrt(distSq);
        outDirToLight = toLight / outDistance;

        const float nDotL = std::max(0.0f, glm::dot(normal, outDirToLight));
        const float lightCos = std::max(0.0f, glm::dot(emitNormal, -outDirToLight));
        if (nDotL <= 0.0f || lightCos <= 0.0f)
        {
            outIncomingRadiance = glm::vec3(0.0f);
            return true;
        }

        // Solid angle factor dOmega = (cos_light * Area) / dist^2 (since pdf_area = 1/Area).
        const float geomTerm = (lightCos * area) / std::max(distSq, 0.04f);
        const float range = std::max(0.5f, light.dirAndRange.w);
        const float ratio = outDistance / range;
        const float smoothWindow = std::pow(std::clamp(1.0f - ratio * ratio * ratio * ratio, 0.0f, 1.0f), 2.0f);
        const glm::vec3 radiance = glm::vec3(light.colorAndIntensity) * light.colorAndIntensity.w;
        outIncomingRadiance = radiance * (geomTerm * smoothWindow);
        return true;
    }

    struct TemporalMomentResult
    {
        float m1 = 0.0f;
        float m2 = 0.0f;
        float variance = 0.0f;
    };

    inline TemporalMomentResult UpdateTemporalMoments(
        float prevM1,
        float prevM2,
        float currLuma,
        float alpha) noexcept
    {
        const float a = std::clamp(alpha, 0.0f, 1.0f);
        const float luma = std::max(0.0f, currLuma);
        const float m1 = (1.0f - a) * std::max(0.0f, prevM1) + a * luma;
        const float m2 = (1.0f - a) * std::max(0.0f, prevM2) + a * (luma * luma);
        const float variance = std::max(0.0f, m2 - m1 * m1);
        return { m1, m2, variance };
    }

    inline bool ShouldRejectTemporalHistory(
        const glm::vec3& currWorldPos,
        const glm::vec3& prevSampleWorldPos,
        float viewDistance,
        const glm::vec3& currNormal,
        const glm::vec3& prevNormal,
        uint32_t currInstanceId,
        uint32_t prevInstanceId,
        float normalDotThreshold = 0.90f) noexcept
    {
        if (currInstanceId == 0u || currInstanceId != prevInstanceId)
            return true;
        if (glm::dot(glm::normalize(currNormal), glm::normalize(prevNormal)) < normalDotThreshold)
            return true;
        const float maxPosDelta = std::max(0.08f, 0.04f * std::max(0.0f, viewDistance));
        return glm::distance(currWorldPos, prevSampleWorldPos) > maxPosDelta;
    }
}
