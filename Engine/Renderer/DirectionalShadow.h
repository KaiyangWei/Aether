#pragma once

#include <glm/mat4x4.hpp>
#include <glm/vec3.hpp>
#include <cstdint>

namespace Aether
{
    struct ShadowConfig
    {
        bool enabled = true;
        bool debugView = false;
        uint32_t resolution = 2048;
        float strength = 1.0f;
        float constantDepthBias = 1.25f;
        float slopeDepthBias = 2.5f;
        float receiverDepthBias = 0.0008f;
        float normalBias = 0.015f;
        float pcfRadiusTexels = 1.0f;
        bool pcfEnabled = true;
        float maxShadowDistance = 40.0f;
        float casterMargin = 10.0f;
        bool stabilizeTexels = true;
    };

    struct ShadowLightSpace
    {
        glm::vec3 lightDirection{ 0.0f, 1.0f, 0.0f }; // Unit vector pointing from surface toward light.
        glm::mat4 view{ 1.0f };
        glm::mat4 projection{ 1.0f };
        glm::mat4 viewProjection{ 1.0f };
        float orthoWidth = 0.0f;
        float orthoHeight = 0.0f;
        float nearPlane = 0.0f;
        float farPlane = 0.0f;
        bool valid = false;
    };

    // Matches the directional key light convention in pbr.frag.hlsl: normalize(cos(angle), 0.8, sin(angle)).
    glm::vec3 ComputeDirectionalLightDirection(float lightAngleRadians);

    // Sanitizes and clamps ShadowConfig values to safe finite ranges.
    ShadowConfig SanitizeShadowConfig(const ShadowConfig& config);

    // Fits a right-handed Vulkan [0, 1] Y-flipped orthographic light-space transform to the camera frustum
    // (optionally clamped by scene bounds and maxShadowDistance) with optional texel snapping.
    ShadowLightSpace ComputeDirectionalLightSpace(
        const glm::vec3& lightDirection,
        const glm::mat4& cameraView,
        const glm::mat4& cameraProjection,
        uint32_t shadowResolution,
        float maxShadowDistance = 40.0f,
        float casterMargin = 10.0f,
        bool stabilizeTexels = true,
        const glm::vec3* sceneMin = nullptr,
        const glm::vec3* sceneMax = nullptr
    );

    // Projects a world-space point into shadow map UV [0, 1]^2 and Vulkan depth [0, 1].
    glm::vec3 ProjectToShadowCoords(const glm::mat4& lightViewProjection, const glm::vec3& worldPos);
}
