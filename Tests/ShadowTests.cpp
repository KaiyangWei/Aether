#include <Renderer/DirectionalShadow.h>
#include <Scene/Camera.h>

#include <cmath>
#include <iostream>
#include <limits>

int main()
{
    Aether::Camera camera(45.0f, 16.0f / 9.0f, 0.1f, 100.0f);
    camera.LookAt({0.0f, 2.5f, 6.0f}, {0.0f, 0.0f, 0.0f});

    // 1. Directional light direction convention matches pbr.frag.hlsl.
    {
        const auto dir = Aether::ComputeDirectionalLightDirection(0.0f);
        const auto expected = glm::normalize(glm::vec3(1.0f, 0.8f, 0.0f));
        if (glm::length(dir - expected) > 1e-5f)
            return 1;
    }

    // 2. Standard directional light space: Vulkan [0,1] depth, Y-flipped ortho, origin inside frustum.
    {
        const glm::vec3 sceneMin(-2.0f, -1.0f, -2.0f);
        const glm::vec3 sceneMax(2.0f, 2.0f, 2.0f);
        const auto ls = Aether::ComputeDirectionalLightSpace(
            Aether::ComputeDirectionalLightDirection(0.6f),
            camera.GetViewMatrix(),
            camera.GetProjectionMatrix(),
            2048, 40.0f, 10.0f, true, &sceneMin, &sceneMax);
        if (!ls.valid || ls.projection[1][1] >= 0.0f || ls.nearPlane <= 0.0f || ls.farPlane <= ls.nearPlane)
            return 2;

        // Verify all scene AABB corners map inside [0, 1] UV and [0, 1] Vulkan depth.
        for (int ix = 0; ix < 2; ++ix)
            for (int iy = 0; iy < 2; ++iy)
                for (int iz = 0; iz < 2; ++iz)
                {
                    const glm::vec3 corner(
                        ix ? sceneMax.x : sceneMin.x,
                        iy ? sceneMax.y : sceneMin.y,
                        iz ? sceneMax.z : sceneMin.z);
                    const auto coords = Aether::ProjectToShadowCoords(ls.viewProjection, corner);
                    if (coords.x < 0.0f || coords.x > 1.0f ||
                        coords.y < 0.0f || coords.y > 1.0f ||
                        coords.z < 0.0f || coords.z > 1.0f)
                        return 3;
                }

        // Verify point closer to light has smaller Vulkan depth than point further from light.
        const glm::vec3 closer = ls.lightDirection * 1.0f;
        const glm::vec3 farther = -ls.lightDirection * 1.0f;
        const auto cCloser = Aether::ProjectToShadowCoords(ls.viewProjection, closer);
        const auto cFarther = Aether::ProjectToShadowCoords(ls.viewProjection, farther);
        if (!(cCloser.z < cFarther.z))
            return 4;
    }

    // 3. Degenerate vertical light directions (+Y and -Y) must not produce NaN or invalid lookAt.
    {
        for (const glm::vec3 vertical : {glm::vec3(0.0f, 1.0f, 0.0f), glm::vec3(0.0f, -1.0f, 0.0f)})
        {
            const auto ls = Aether::ComputeDirectionalLightSpace(
                vertical, camera.GetViewMatrix(), camera.GetProjectionMatrix(), 1024);
            if (!ls.valid)
                return 5;
            const auto coords = Aether::ProjectToShadowCoords(ls.viewProjection, glm::vec3(0.0f));
            if (!std::isfinite(coords.x) || !std::isfinite(coords.y) || !std::isfinite(coords.z))
                return 6;
        }
    }

    // 4. Invalid inputs must fail cleanly (valid == false).
    {
        const auto zeroLight = Aether::ComputeDirectionalLightSpace(
            glm::vec3(0.0f), camera.GetViewMatrix(), camera.GetProjectionMatrix(), 2048);
        const auto zeroRes = Aether::ComputeDirectionalLightSpace(
            glm::vec3(0.0f, 1.0f, 0.0f), camera.GetViewMatrix(), camera.GetProjectionMatrix(), 0);
        const auto badDist = Aether::ComputeDirectionalLightSpace(
            glm::vec3(0.0f, 1.0f, 0.0f), camera.GetViewMatrix(), camera.GetProjectionMatrix(), 2048, -5.0f);
        const auto singularProj = Aether::ComputeDirectionalLightSpace(
            glm::vec3(0.0f, 1.0f, 0.0f), camera.GetViewMatrix(), glm::mat4(0.0f), 2048);
        const auto nanLight = Aether::ComputeDirectionalLightSpace(
            glm::vec3(std::numeric_limits<float>::quiet_NaN(), 1.0f, 0.0f),
            camera.GetViewMatrix(), camera.GetProjectionMatrix(), 2048);
        if (zeroLight.valid || zeroRes.valid || badDist.valid || singularProj.valid || nanLight.valid)
            return 7;
    }

    // 5. Texel stabilization: small translation along light-space plane keeps world-origin snapped to texel centers.
    {
        const glm::vec3 dir = Aether::ComputeDirectionalLightDirection(0.4f);
        const auto ls1 = Aether::ComputeDirectionalLightSpace(
            dir, camera.GetViewMatrix(), camera.GetProjectionMatrix(), 1024, 25.0f, 10.0f, true);
        Aether::Camera shifted = camera;
        shifted.LookAt({0.03f, 2.5f, 6.02f}, {0.03f, 0.0f, 0.02f});
        const auto ls2 = Aether::ComputeDirectionalLightSpace(
            dir, shifted.GetViewMatrix(), shifted.GetProjectionMatrix(), 1024, 25.0f, 10.0f, true);
        if (!ls1.valid || !ls2.valid)
            return 8;
        const auto p1 = Aether::ProjectToShadowCoords(ls1.viewProjection, glm::vec3(0.0f));
        const auto p2 = Aether::ProjectToShadowCoords(ls2.viewProjection, glm::vec3(0.0f));
        const float uTexelDelta = (p2.x - p1.x) * 1024.0f;
        const float vTexelDelta = (p2.y - p1.y) * 1024.0f;
        if (std::abs(uTexelDelta - std::round(uTexelDelta)) > 1e-3f ||
            std::abs(vTexelDelta - std::round(vTexelDelta)) > 1e-3f)
            return 9;
    }

    // 6. ShadowConfig sanitization / bias validation.
    {
        Aether::ShadowConfig cfg{};
        cfg.resolution = 16;
        cfg.strength = 3.5f;
        cfg.constantDepthBias = -2.0f;
        cfg.slopeDepthBias = std::numeric_limits<float>::infinity();
        cfg.receiverDepthBias = 1.0f;
        cfg.normalBias = -1.0f;
        cfg.pcfRadiusTexels = 0.0f;
        const auto clean = Aether::SanitizeShadowConfig(cfg);
        if (clean.resolution != 256u || clean.strength != 1.0f ||
            clean.constantDepthBias != 0.0f || !std::isfinite(clean.slopeDepthBias) ||
            clean.receiverDepthBias > 0.05f || clean.normalBias != 0.0f ||
            clean.pcfRadiusTexels < 0.25f)
            return 10;
    }

    return 0;
}
