#include "DirectionalShadow.h"

#include <glm/geometric.hpp>
#include <glm/gtc/matrix_inverse.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>

namespace Aether
{
    namespace
    {
        bool IsFiniteVec3(const glm::vec3& v)
        {
            return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
        }

        bool IsFiniteMat4(const glm::mat4& m)
        {
            for (int c = 0; c < 4; ++c)
                for (int r = 0; r < 4; ++r)
                    if (!std::isfinite(m[c][r]))
                        return false;
            return true;
        }

        glm::vec3 ChooseSafeUp(const glm::vec3& forward)
        {
            const glm::vec3 worldUp(0.0f, 1.0f, 0.0f);
            if (std::abs(glm::dot(forward, worldUp)) > 0.99f)
                return glm::vec3(0.0f, 0.0f, 1.0f);
            return worldUp;
        }

        glm::mat4 MakeVulkanOrtho(float left, float right, float bottom, float top, float nearZ, float farZ)
        {
            // Swap bottom/top to flip Y once (both scale and asymmetric offset) for Vulkan's positive-height viewport.
            return glm::orthoRH_ZO(left, right, top, bottom, nearZ, farZ);
        }
    }

    glm::vec3 ComputeDirectionalLightDirection(float lightAngleRadians)
    {
        if (!std::isfinite(lightAngleRadians))
            lightAngleRadians = 0.0f;
        return glm::normalize(glm::vec3(std::cos(lightAngleRadians), 0.8f, std::sin(lightAngleRadians)));
    }

    ShadowConfig SanitizeShadowConfig(const ShadowConfig& config)
    {
        ShadowConfig out = config;
        out.resolution = std::clamp(out.resolution, 256u, 8192u);
        out.strength = std::isfinite(out.strength) ? std::clamp(out.strength, 0.0f, 1.0f) : 1.0f;
        out.constantDepthBias = std::isfinite(out.constantDepthBias) ? std::clamp(out.constantDepthBias, 0.0f, 32.0f) : 1.25f;
        out.slopeDepthBias = std::isfinite(out.slopeDepthBias) ? std::clamp(out.slopeDepthBias, 0.0f, 32.0f) : 2.5f;
        out.receiverDepthBias = std::isfinite(out.receiverDepthBias) ? std::clamp(out.receiverDepthBias, 0.0f, 0.05f) : 0.0008f;
        out.normalBias = std::isfinite(out.normalBias) ? std::clamp(out.normalBias, 0.0f, 0.5f) : 0.015f;
        out.pcfRadiusTexels = std::isfinite(out.pcfRadiusTexels) ? std::clamp(out.pcfRadiusTexels, 0.25f, 4.0f) : 1.0f;
        out.maxShadowDistance = std::isfinite(out.maxShadowDistance) && out.maxShadowDistance > 0.1f
            ? std::clamp(out.maxShadowDistance, 0.5f, 1000.0f) : 40.0f;
        out.casterMargin = std::isfinite(out.casterMargin) && out.casterMargin >= 0.0f
            ? std::clamp(out.casterMargin, 0.0f, 500.0f) : 10.0f;
        return out;
    }

    ShadowLightSpace ComputeDirectionalLightSpace(
        const glm::vec3& lightDirection,
        const glm::mat4& cameraView,
        const glm::mat4& cameraProjection,
        uint32_t shadowResolution,
        float maxShadowDistance,
        float casterMargin,
        bool stabilizeTexels,
        const glm::vec3* sceneMin,
        const glm::vec3* sceneMax)
    {
        ShadowLightSpace result{};
        if (shadowResolution == 0 || !IsFiniteVec3(lightDirection) ||
            !IsFiniteMat4(cameraView) || !IsFiniteMat4(cameraProjection) ||
            !std::isfinite(maxShadowDistance) || maxShadowDistance <= 0.0f ||
            !std::isfinite(casterMargin) || casterMargin < 0.0f)
            return result;

        const float lightLen = glm::length(lightDirection);
        if (!std::isfinite(lightLen) || lightLen <= 1e-6f)
            return result;
        const glm::vec3 toLight = lightDirection / lightLen;

        const glm::mat4 vp = cameraProjection * cameraView;
        const float det = glm::determinant(vp);
        if (!std::isfinite(det) || std::abs(det) <= 1e-12f)
            return result;
        const glm::mat4 invVp = glm::inverse(vp);
        if (!IsFiniteMat4(invVp))
            return result;

        // Reconstruct 8 camera frustum corners in world space from Vulkan NDC [-1, 1] x [-1, 1] x [0, 1].
        std::array<glm::vec3, 8> corners{};
        constexpr std::array<glm::vec3, 8> ndcCorners = {{
            {-1.0f, -1.0f, 0.0f}, { 1.0f, -1.0f, 0.0f},
            { 1.0f,  1.0f, 0.0f}, {-1.0f,  1.0f, 0.0f},
            {-1.0f, -1.0f, 1.0f}, { 1.0f, -1.0f, 1.0f},
            { 1.0f,  1.0f, 1.0f}, {-1.0f,  1.0f, 1.0f}
        }};
        for (size_t i = 0; i < 8; ++i)
        {
            const glm::vec4 world = invVp * glm::vec4(ndcCorners[i], 1.0f);
            if (!std::isfinite(world.w) || std::abs(world.w) <= 1e-7f)
                return result;
            corners[i] = glm::vec3(world) / world.w;
            if (!IsFiniteVec3(corners[i]))
                return result;
        }

        // Clamp far corners to maxShadowDistance along each frustum ray to preserve texel density.
        for (size_t i = 0; i < 4; ++i)
        {
            const glm::vec3 ray = corners[i + 4] - corners[i];
            const float rayLen = glm::length(ray);
            if (rayLen > maxShadowDistance && rayLen > 1e-6f)
                corners[i + 4] = corners[i] + ray * (maxShadowDistance / rayLen);
        }

        // Compute frustum center and bounding sphere radius (rotation-invariant for texel snapping).
        glm::vec3 center(0.0f);
        for (const auto& c : corners)
            center += c;
        center *= (1.0f / 8.0f);

        float radius = 0.0f;
        for (const auto& c : corners)
            radius = std::max(radius, glm::length(c - center));

        // If valid scene bounds are provided, tighten the shadow volume to cover the scene
        // while keeping casters in range, avoiding wasted shadow resolution on empty far space.
        bool hasSceneBounds = false;
        glm::vec3 clampedMin(0.0f), clampedMax(0.0f);
        if (sceneMin && sceneMax && IsFiniteVec3(*sceneMin) && IsFiniteVec3(*sceneMax) &&
            sceneMin->x <= sceneMax->x && sceneMin->y <= sceneMax->y && sceneMin->z <= sceneMax->z)
        {
            hasSceneBounds = true;
            const glm::vec3 sceneCenter = 0.5f * (*sceneMin + *sceneMax);
            const float sceneRadius = std::max(glm::length(0.5f * (*sceneMax - *sceneMin)), 0.25f);
            // If the scene is smaller than the frustum slice, center on the scene to maximize resolution.
            if (sceneRadius < radius)
            {
                center = sceneCenter;
                radius = sceneRadius * 1.15f;
            }
            clampedMin = *sceneMin;
            clampedMax = *sceneMax;
        }

        radius = std::max(radius, 0.25f);
        if (stabilizeTexels)
            radius = std::ceil(radius * 16.0f) / 16.0f;
        const glm::vec3 forward = -toLight;
        const glm::vec3 up = ChooseSafeUp(forward);

        // Initial light view looking at center along -toLight.
        const float eyePullback = radius + casterMargin + 1.0f;
        glm::vec3 lightEye = center + toLight * eyePullback;
        glm::mat4 lightView = glm::lookAtRH(lightEye, center, up);

        float minX = -radius, maxX = radius;
        float minY = -radius, maxY = radius;
        float minZ = std::numeric_limits<float>::max();
        float maxZ = -std::numeric_limits<float>::max();

        auto includePointZ = [&](const glm::vec3& p) {
            const glm::vec4 lp = lightView * glm::vec4(p, 1.0f);
            // In right-handed view space, points in front of the light camera have negative Z.
            const float depth = -lp.z;
            minZ = std::min(minZ, depth);
            maxZ = std::max(maxZ, depth);
        };

        if (hasSceneBounds)
        {
            for (int ix = 0; ix < 2; ++ix)
                for (int iy = 0; iy < 2; ++iy)
                    for (int iz = 0; iz < 2; ++iz)
                        includePointZ(glm::vec3(
                            ix ? clampedMax.x : clampedMin.x,
                            iy ? clampedMax.y : clampedMin.y,
                            iz ? clampedMax.z : clampedMin.z));
        }
        else
        {
            for (const auto& c : corners)
                includePointZ(c);
        }

        if (!std::isfinite(minZ) || !std::isfinite(maxZ))
            return result;

        // Texel snapping in light view space to reduce shadow edge shimmering on camera translation.
        const float extent = 2.0f * radius;
        if (stabilizeTexels)
        {
            const float texelSize = extent / static_cast<float>(shadowResolution);
            if (texelSize > 0.0f && std::isfinite(texelSize))
            {
                // Snap world origin projected into light space to texel grid.
                const glm::vec4 originLs = lightView * glm::vec4(0.0f, 0.0f, 0.0f, 1.0f);
                const float snappedX = std::round(originLs.x / texelSize) * texelSize;
                const float snappedY = std::round(originLs.y / texelSize) * texelSize;
                const float dx = snappedX - originLs.x;
                const float dy = snappedY - originLs.y;
                minX -= dx;
                maxX -= dx;
                minY -= dy;
                maxY -= dy;
            }
        }

        const float nearPlane = std::max(0.1f, minZ - casterMargin);
        const float farPlane = std::max(nearPlane + 1.0f, maxZ + std::max(1.0f, casterMargin * 0.25f));

        const glm::mat4 lightProj = MakeVulkanOrtho(minX, maxX, minY, maxY, nearPlane, farPlane);
        const glm::mat4 lightVp = lightProj * lightView;
        if (!IsFiniteMat4(lightVp))
            return result;

        result.lightDirection = toLight;
        result.view = lightView;
        result.projection = lightProj;
        result.viewProjection = lightVp;
        result.orthoWidth = maxX - minX;
        result.orthoHeight = maxY - minY;
        result.nearPlane = nearPlane;
        result.farPlane = farPlane;
        result.valid = true;
        return result;
    }

    glm::vec3 ProjectToShadowCoords(const glm::mat4& lightViewProjection, const glm::vec3& worldPos)
    {
        const glm::vec4 clip = lightViewProjection * glm::vec4(worldPos, 1.0f);
        if (!std::isfinite(clip.w) || std::abs(clip.w) <= 1e-7f)
            return glm::vec3(-1.0f);
        const glm::vec3 ndc = glm::vec3(clip) / clip.w;
        return glm::vec3(ndc.x * 0.5f + 0.5f, ndc.y * 0.5f + 0.5f, ndc.z);
    }
}
