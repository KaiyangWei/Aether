#pragma once

#include <glm/glm.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace Aether
{
    enum class RenderMode : uint8_t
    {
        Rasterization = 0,
        HybridRayTracing = 1,
        RayQuery = 2
    };

    enum class RayQueryDebugMode : uint32_t
    {
        DiagnosticShading = 0,
        HitMiss = 1,
        HitDistance = 2,
        PrimitiveAndInstance = 3,
        RawReadback = 4
    };

    constexpr uint32_t kRayQueryInteractiveDebugModeCount = 4;

    struct RayQueryConfig
    {
        RayQueryDebugMode debugMode = RayQueryDebugMode::DiagnosticShading;
        float tMin = 0.001f;
        float tMax = 100.0f;
        float maxDistanceVisualization = 14.0f;
    };

    struct RenderModeSwitchResult
    {
        RenderMode nextMode = RenderMode::Rasterization;
        bool switched = false;
        bool invalidateTemporalHistory = false;
        const char* message = "";
    };

    inline const char* RenderModeToString(RenderMode mode) noexcept
    {
        switch (mode)
        {
        case RenderMode::Rasterization:
            return "Rasterization";
        case RenderMode::HybridRayTracing:
            return "Hybrid Ray Tracing";
        case RenderMode::RayQuery:
            return "Ray Query";
        default:
            return "Unknown";
        }
    }

    inline const char* RayQueryDebugModeToString(RayQueryDebugMode mode) noexcept
    {
        switch (mode)
        {
        case RayQueryDebugMode::DiagnosticShading:
            return "Diagnostic Shading";
        case RayQueryDebugMode::HitMiss:
            return "Hit / Miss";
        case RayQueryDebugMode::HitDistance:
            return "Hit Distance";
        case RayQueryDebugMode::PrimitiveAndInstance:
            return "Primitive & Instance ID";
        case RayQueryDebugMode::RawReadback:
            return "Raw Numeric Readback";
        default:
            return "Unknown";
        }
    }

    inline RenderModeSwitchResult ToggleRenderMode(RenderMode current, bool rayTracingReady) noexcept
    {
        if (current == RenderMode::Rasterization)
        {
            if (!rayTracingReady)
            {
                return {
                    RenderMode::Rasterization,
                    false,
                    false,
                    "Hybrid Ray Tracing is unavailable on this device; staying in Rasterization mode."
                };
            }
            return {
                RenderMode::HybridRayTracing,
                true,
                true,
                "Switched render mode: Rasterization -> Hybrid Ray Tracing"
            };
        }
        return {
            RenderMode::Rasterization,
            true,
            true,
            "Switched render mode: Hybrid Ray Tracing -> Rasterization"
        };
    }

    inline RayQueryDebugMode CycleInteractiveRayQueryDebugMode(RayQueryDebugMode current) noexcept
    {
        const uint32_t idx = static_cast<uint32_t>(current);
        const uint32_t next = (idx + 1u) % kRayQueryInteractiveDebugModeCount;
        return static_cast<RayQueryDebugMode>(next);
    }

    inline RayQueryConfig SanitizeRayQueryConfig(const RayQueryConfig& input) noexcept
    {
        RayQueryConfig out = input;
        if (static_cast<uint32_t>(out.debugMode) > static_cast<uint32_t>(RayQueryDebugMode::RawReadback))
            out.debugMode = RayQueryDebugMode::DiagnosticShading;
        if (!std::isfinite(out.tMin) || out.tMin <= 0.0f)
            out.tMin = 0.001f;
        out.tMin = std::clamp(out.tMin, 1e-5f, 10.0f);
        if (!std::isfinite(out.tMax) || out.tMax <= out.tMin)
            out.tMax = std::max(100.0f, out.tMin + 1.0f);
        out.tMax = std::clamp(out.tMax, out.tMin + 0.01f, 10000.0f);
        if (!std::isfinite(out.maxDistanceVisualization) || out.maxDistanceVisualization <= 0.01f)
            out.maxDistanceVisualization = 14.0f;
        out.maxDistanceVisualization = std::clamp(out.maxDistanceVisualization, 0.1f, 1000.0f);
        return out;
    }

    // Exact CPU mirror of rayquery_debug.comp.hlsl primary camera ray generation.
    // clipToWorldDirection = inverse(unjitteredProjection * mat4(mat3(viewMatrix))).
    inline bool GenerateCameraPrimaryRay(
        const glm::vec3& cameraPosition,
        const glm::mat4& clipToWorldDirection,
        uint32_t pixelX,
        uint32_t pixelY,
        uint32_t width,
        uint32_t height,
        glm::vec3& outOrigin,
        glm::vec3& outDirection) noexcept
    {
        if (width == 0 || height == 0 || pixelX >= width || pixelY >= height)
            return false;

        const float u = (static_cast<float>(pixelX) + 0.5f) / static_cast<float>(width);
        const float v = (static_cast<float>(pixelY) + 0.5f) / static_cast<float>(height);
        const glm::vec2 ndc(u * 2.0f - 1.0f, v * 2.0f - 1.0f);
        const glm::vec4 worldH = clipToWorldDirection * glm::vec4(ndc.x, ndc.y, 1.0f, 1.0f);
        if (!std::isfinite(worldH.x) || !std::isfinite(worldH.y) ||
            !std::isfinite(worldH.z) || !std::isfinite(worldH.w) ||
            std::abs(worldH.w) <= 1e-12f)
            return false;

        const glm::vec3 unnormalized = glm::vec3(worldH) / worldH.w;
        const float len = glm::length(unnormalized);
        if (!std::isfinite(len) || len <= 1e-8f)
            return false;

        outOrigin = cameraPosition;
        outDirection = unnormalized / len;
        return true;
    }
}
