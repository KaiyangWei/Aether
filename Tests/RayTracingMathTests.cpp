#include <Assets/CornellBoxScene.h>
#include <Renderer/RayTracingConfig.h>
#include <Renderer/RayTracingMath.h>
#include <Scene/Camera.h>
#include <Scene/Transform.h>

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>

#include <array>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <limits>
#include <vector>

namespace
{
    int Fail(const char* message)
    {
        std::cerr << "RayTracingMathTests FAILED: " << message << '\n';
        return 1;
    }

    bool ApproxEqual(const glm::vec3& a, const glm::vec3& b, float eps = 1e-4f)
    {
        return std::abs(a.x - b.x) <= eps &&
               std::abs(a.y - b.y) <= eps &&
               std::abs(a.z - b.z) <= eps;
    }

    bool VerifyMatrixAgainstPoints(const glm::mat4& model)
    {
        const VkTransformMatrixKHR vkMat = Aether::ToVkTransformMatrix(model);
        const std::array<glm::vec3, 6> points = {{
            { 0.0f, 0.0f, 0.0f },
            { 1.0f, 0.0f, 0.0f },
            { 0.0f, 1.0f, 0.0f },
            { 0.0f, 0.0f, 1.0f },
            { 1.3f, -2.7f, 4.1f },
            { -5.25f, 3.5f, -0.75f }
        }};

        for (const auto& p : points)
        {
            const glm::vec3 expected = glm::vec3(model * glm::vec4(p, 1.0f));
            const glm::vec3 actual = Aether::TransformPoint(vkMat, p);
            if (!ApproxEqual(expected, actual))
                return false;
        }
        return true;
    }
}

int main()
{
    using namespace Aether;

    // 1. Identity transform
    {
        const glm::mat4 identity(1.0f);
        const VkTransformMatrixKHR vkMat = ToVkTransformMatrix(identity);
        if (vkMat.matrix[0][0] != 1.0f || vkMat.matrix[1][1] != 1.0f || vkMat.matrix[2][2] != 1.0f ||
            vkMat.matrix[0][3] != 0.0f || vkMat.matrix[1][3] != 0.0f || vkMat.matrix[2][3] != 0.0f ||
            !VerifyMatrixAgainstPoints(identity))
            return Fail("identity transform conversion failed");
    }

    // 2. Pure translation
    {
        const glm::mat4 trans = glm::translate(glm::mat4(1.0f), glm::vec3(3.5f, -7.25f, 11.0f));
        const VkTransformMatrixKHR vkMat = ToVkTransformMatrix(trans);
        if (vkMat.matrix[0][3] != 3.5f || vkMat.matrix[1][3] != -7.25f || vkMat.matrix[2][3] != 11.0f ||
            vkMat.matrix[0][0] != 1.0f || vkMat.matrix[1][1] != 1.0f || vkMat.matrix[2][2] != 1.0f ||
            !VerifyMatrixAgainstPoints(trans))
            return Fail("pure translation conversion failed");
    }

    // 3. Rotation (arbitrary axis & angle)
    {
        const glm::quat q = glm::angleAxis(glm::radians(53.0f), glm::normalize(glm::vec3(0.4f, -0.8f, 0.5f)));
        const glm::mat4 rot = glm::mat4_cast(q);
        if (!VerifyMatrixAgainstPoints(rot))
            return Fail("rotation conversion failed");
    }

    // 4. Non-uniform scaling (including negative mirrored axis)
    {
        const glm::mat4 scale = glm::scale(glm::mat4(1.0f), glm::vec3(2.0f, -0.5f, 3.25f));
        const VkTransformMatrixKHR vkMat = ToVkTransformMatrix(scale);
        if (vkMat.matrix[0][0] != 2.0f || vkMat.matrix[1][1] != -0.5f || vkMat.matrix[2][2] != 3.25f ||
            !VerifyMatrixAgainstPoints(scale))
            return Fail("non-uniform scaling conversion failed");
    }

    // 5. Combined TRS using Aether::Transform::GetMatrix()
    {
        Transform trs{};
        trs.position = glm::vec3(-4.2f, 2.75f, 6.1f);
        trs.rotation = glm::angleAxis(glm::radians(-117.5f), glm::normalize(glm::vec3(1.0f, 2.0f, -1.5f)));
        trs.scale = glm::vec3(0.75f, 1.8f, 2.4f);
        const glm::mat4 combined = trs.GetMatrix();
        if (!VerifyMatrixAgainstPoints(combined))
            return Fail("combined TRS conversion failed");
    }

    // 6. Invalid instance metadata rejection + valid 24-bit bounds
    {
        const glm::mat4 validModel = glm::translate(glm::mat4(1.0f), glm::vec3(1.0f, 2.0f, 3.0f));
        const uint64_t validBlasAddr = 0x100000ULL;

        if (BuildVkAccelerationStructureInstance({ validModel, 0ULL, 0u, 0xFFu, 0u, 0 }).has_value())
            return Fail("zero BLAS device address must be rejected");

        if (BuildVkAccelerationStructureInstance({ validModel, validBlasAddr, 0u, 0x00u, 0u, 0 }).has_value())
            return Fail("zero instance mask must be rejected");

        if (BuildVkAccelerationStructureInstance({ validModel, validBlasAddr, kMaxRtInstanceCustomIndex + 1u, 0xFFu, 0u, 0 }).has_value())
            return Fail("24-bit customIndex overflow must be rejected");

        if (BuildVkAccelerationStructureInstance({ validModel, validBlasAddr, 0u, 0xFFu, kMaxRtSbtRecordOffset + 1u, 0 }).has_value())
            return Fail("24-bit SBT offset overflow must be rejected");

        glm::mat4 nanModel(1.0f);
        nanModel[3][0] = std::numeric_limits<float>::quiet_NaN();
        if (BuildVkAccelerationStructureInstance({ nanModel, validBlasAddr, 0u, 0xFFu, 0u, 0 }).has_value())
            return Fail("NaN transform must be rejected");

        glm::mat4 infModel(1.0f);
        infModel[1][2] = std::numeric_limits<float>::infinity();
        if (BuildVkAccelerationStructureInstance({ infModel, validBlasAddr, 0u, 0xFFu, 0u, 0 }).has_value())
            return Fail("Inf transform must be rejected");

        const auto maxValid = BuildVkAccelerationStructureInstance({
            validModel,
            validBlasAddr,
            kMaxRtInstanceCustomIndex,
            0xABu,
            kMaxRtSbtRecordOffset,
            ComputeRtInstanceFlags(true, true, true)
        });
        if (!maxValid.has_value() ||
            maxValid->instanceCustomIndex != kMaxRtInstanceCustomIndex ||
            maxValid->instanceShaderBindingTableRecordOffset != kMaxRtSbtRecordOffset ||
            maxValid->mask != 0xABu ||
            maxValid->accelerationStructureReference != validBlasAddr)
            return Fail("valid max 24-bit instance metadata failed");

        const VkGeometryInstanceFlagsKHR expectedFlags =
            VK_GEOMETRY_INSTANCE_TRIANGLE_FACING_CULL_DISABLE_BIT_KHR |
            VK_GEOMETRY_INSTANCE_TRIANGLE_FLIP_FACING_BIT_KHR |
            VK_GEOMETRY_INSTANCE_FORCE_NO_OPAQUE_BIT_KHR;
        if (static_cast<VkGeometryInstanceFlagsKHR>(maxValid->flags) != expectedFlags)
            return Fail("alphaMask + doubleSided + mirrored instance flags mismatch");

        const VkGeometryInstanceFlagsKHR opaqueFlags = ComputeRtInstanceFlags(false, false, false);
        if (opaqueFlags != VK_GEOMETRY_INSTANCE_FORCE_OPAQUE_BIT_KHR)
            return Fail("opaque instance flags mismatch");
    }

    // 7. Empty instance list handling
    {
        std::vector<RtInstanceBuildParams> emptyInput;
        std::vector<VkAccelerationStructureInstanceKHR> built;
        for (const auto& p : emptyInput)
        {
            if (auto inst = BuildVkAccelerationStructureInstance(p))
                built.push_back(*inst);
        }
        if (!built.empty())
            return Fail("empty instance input must yield empty output");
    }

    // 8. Scratch address alignment calculations (powers of 2 and non-powers of 2)
    {
        if (AlignUp(0, 128) != 0 || AlignUp(1, 128) != 128 || AlignUp(128, 128) != 128 ||
            AlignUp(129, 128) != 256 || AlignUp(0x1001, 256) != 0x1100 ||
            AlignUp(100, 1) != 100 || AlignUp(100, 0) != 100 ||
            AlignUp(385, 384) != 768)
            return Fail("AlignUp calculation mismatch");

        if (ComputePaddedScratchSize(0, 128) != 0 ||
            ComputePaddedScratchSize(1024, 1) != 1024 ||
            ComputePaddedScratchSize(1024, 128) != 1024 + 127 ||
            ComputePaddedScratchSize(1024, 256) != 1024 + 255)
            return Fail("ComputePaddedScratchSize mismatch");

        for (VkDeviceSize align : { VkDeviceSize{ 1 }, VkDeviceSize{ 16 }, VkDeviceSize{ 128 }, VkDeviceSize{ 256 }, VkDeviceSize{ 384 } })
        {
            const VkDeviceSize required = 4096;
            const VkDeviceSize padded = ComputePaddedScratchSize(required, align);
            for (VkDeviceSize rem = 0; rem < align; ++rem)
            {
                const VkDeviceAddress raw = 0x100000ULL + rem;
                const VkDeviceAddress aligned = AlignUpDeviceAddress(raw, align);
                if (!IsAddressAligned(aligned, align) || aligned < raw ||
                    (aligned - raw) + required > padded)
                    return Fail("padded scratch overflowed for address remainder");
            }
        }
    }

    // 9. Camera primary ray generation math (exact correspondence with Camera::GetViewProjectionMatrix)
    {
        Camera cam(45.0f, 16.0f / 9.0f, 0.1f, 100.0f);
        cam.LookAt({ 0.0f, 2.2f, 6.15f }, { 0.0f, 1.85f, -0.55f }, { 0.0f, 1.0f, 0.0f });
        const glm::mat4 view = cam.GetViewMatrix();
        const glm::mat4 proj = cam.GetProjectionMatrix();
        const glm::mat4 vp = proj * view;
        const glm::mat4 clipToWorldDir = glm::inverse(proj * glm::mat4(glm::mat3(view)));

        constexpr uint32_t width = 1280;
        constexpr uint32_t height = 720;

        // Invalid pixel bounds must fail cleanly.
        glm::vec3 orig{}, dir{};
        if (GenerateCameraPrimaryRay(cam.GetPosition(), clipToWorldDir, width, 0, width, height, orig, dir) ||
            GenerateCameraPrimaryRay(cam.GetPosition(), clipToWorldDir, 0, height, width, height, orig, dir) ||
            GenerateCameraPrimaryRay(cam.GetPosition(), clipToWorldDir, 0, 0, 0, height, orig, dir))
            return Fail("invalid pixel bounds must be rejected by GenerateCameraPrimaryRay");

        // Project known 3D points inside the room to pixel coordinates, then reconstruct the primary ray
        // and verify it points directly from camera eye to the exact world point.
        const std::array<glm::vec3, 5> worldPoints = {{
            {  0.00f, 1.85f, -0.55f },
            { -1.48f, 1.22f, -1.42f },
            {  1.48f, 1.42f, -1.25f },
            { -2.16f, 1.38f,  0.58f },
            {  1.32f, 0.76f,  0.78f }
        }};
        for (const auto& pt : worldPoints)
        {
            const glm::vec4 clip = vp * glm::vec4(pt, 1.0f);
            const glm::vec3 ndc = glm::vec3(clip) / clip.w;
            const uint32_t px = static_cast<uint32_t>(std::floor((ndc.x * 0.5f + 0.5f) * static_cast<float>(width)));
            const uint32_t py = static_cast<uint32_t>(std::floor((ndc.y * 0.5f + 0.5f) * static_cast<float>(height)));
            if (!GenerateCameraPrimaryRay(cam.GetPosition(), clipToWorldDir, px, py, width, height, orig, dir))
                return Fail("GenerateCameraPrimaryRay failed for projected world point");
            if (!ApproxEqual(orig, cam.GetPosition(), 1e-5f))
                return Fail("primary ray origin must match camera position");
            const glm::vec3 expectedDir = glm::normalize(pt - cam.GetPosition());
            if (glm::dot(dir, expectedDir) < 0.9999f)
                return Fail("reconstructed primary ray direction diverged from raster projection");
        }
    }

    // 10. Render mode switching policy, RayQueryConfig & HybridRtConfig sanitization
    {
        const auto blocked = ToggleRenderMode(RenderMode::Rasterization, false);
        if (blocked.switched || blocked.nextMode != RenderMode::Rasterization || blocked.invalidateTemporalHistory)
            return Fail("ToggleRenderMode must remain in Rasterization when RT is unavailable");

        const auto toHybrid = ToggleRenderMode(RenderMode::Rasterization, true);
        if (!toHybrid.switched || toHybrid.nextMode != RenderMode::HybridRayTracing || !toHybrid.invalidateTemporalHistory)
            return Fail("ToggleRenderMode must switch to HybridRayTracing and invalidate temporal history when RT is ready");

        const auto toRaster = ToggleRenderMode(RenderMode::HybridRayTracing, true);
        if (!toRaster.switched || toRaster.nextMode != RenderMode::Rasterization || !toRaster.invalidateTemporalHistory)
            return Fail("ToggleRenderMode must switch back to Rasterization and invalidate temporal history");

        if (ToggleRenderMode(RenderMode::RayQuery, true).nextMode != RenderMode::Rasterization)
            return Fail("ToggleRenderMode from RayQuery must return to Rasterization");

        RayQueryConfig dirty{};
        dirty.debugMode = static_cast<RayQueryDebugMode>(99);
        dirty.tMin = -5.0f;
        dirty.tMax = 0.0f;
        dirty.maxDistanceVisualization = std::numeric_limits<float>::quiet_NaN();
        const RayQueryConfig clean = SanitizeRayQueryConfig(dirty);
        if (clean.debugMode != RayQueryDebugMode::DiagnosticShading ||
            clean.tMin <= 0.0f || clean.tMax <= clean.tMin ||
            clean.maxDistanceVisualization <= 0.0f)
            return Fail("SanitizeRayQueryConfig failed to clean invalid parameters");

        HybridRtConfig dirtyHybrid{};
        dirtyHybrid.shadowSamples = 99;
        dirtyHybrid.sunAngularRadiusDeg = -4.0f;
        dirtyHybrid.reflectionMaxRoughness = 3.0f;
        dirtyHybrid.atrousIterations = 25;
        dirtyHybrid.debugView = static_cast<HybridDebugView>(999);
        const HybridRtConfig cleanHybrid = SanitizeHybridRtConfig(dirtyHybrid);
        if (cleanHybrid.shadowSamples != 16u || cleanHybrid.sunAngularRadiusDeg != 0.0f ||
            cleanHybrid.reflectionMaxRoughness != 1.0f || cleanHybrid.atrousIterations != 5u ||
            cleanHybrid.debugView != HybridDebugView::FinalHybrid)
            return Fail("SanitizeHybridRtConfig failed to clamp invalid parameters");

        if (CycleInteractiveRayQueryDebugMode(RayQueryDebugMode::PrimitiveAndInstance) !=
            RayQueryDebugMode::DiagnosticShading)
            return Fail("CycleInteractiveRayQueryDebugMode wrap-around failed");

        if (CycleHybridDebugView(HybridDebugView::RayQueryDiagnostic) != HybridDebugView::FinalHybrid)
            return Fail("CycleHybridDebugView wrap-around failed");
    }

    // 11. Cornell Box default validation scene CPU structural & explicit light checks
    {
        const ImportedAsset cornell = MakeCornellBoxScene();
        const CornellBoxValidationSummary summary = InspectCornellBoxAsset(cornell);
        if (!summary.allMeshesValid ||
            summary.meshCount < 10 ||
            summary.nodeCount < 25 ||
            summary.emissiveMaterialCount < 3 ||
            summary.mirrorLikeMaterialCount < 1 ||
            summary.roughDielectricMaterialCount < 2 ||
            summary.midRoughMetallicMaterialCount < 2 ||
            summary.roughMetallicMaterialCount < 1)
            return Fail("Cornell Box default validation scene failed structural requirements");

        const auto lights = GetCornellBoxLights();
        if (lights.size() < 3)
            return Fail("Cornell Box must define explicit physical area/point lights");
    }

    // 12. Phase 10 shadow origin bias & Schlick Fresnel invariants
    {
        const glm::vec3 p(1.0f, 0.0f, 0.0f);
        const glm::vec3 n(0.0f, 1.0f, 0.0f);
        const glm::vec3 v(0.0f, 0.6f, 0.8f);
        const glm::vec3 nearOrigin = ComputeShadowRayOrigin(p, n, v, 1.0f, 0.004f, 0.0015f);
        const glm::vec3 farOrigin = ComputeShadowRayOrigin(p, n, v, 20.0f, 0.004f, 0.0015f);
        if (nearOrigin.y <= p.y || farOrigin.y <= nearOrigin.y)
            return Fail("Shadow ray origin bias must offset along normal and scale with distance");

        const glm::vec3 f0(0.04f);
        const glm::vec3 fNormal = EvaluateSchlickFresnel(1.0f, f0);
        const glm::vec3 fGrazing = EvaluateSchlickFresnel(0.0f, f0);
        if (!ApproxEqual(fNormal, f0, 1e-4f) || !ApproxEqual(fGrazing, glm::vec3(1.0f), 1e-4f))
            return Fail("Schlick Fresnel endpoint mismatch");
    }

    // 13. Phase 11 Heitz GGX VNDF importance sampling upper-hemisphere validity
    {
        const glm::vec3 vLocal = glm::normalize(glm::vec3(0.4f, 0.2f, 0.88f));
        for (float alpha : { 0.02f, 0.15f, 0.50f, 0.90f })
        {
            for (int k = 0; k < 8; ++k)
            {
                const float u1 = (static_cast<float>(k) + 0.37f) / 8.0f;
                const float u2 = (static_cast<float>((k * 5 + 3) % 8) + 0.5f) / 8.0f;
                const glm::vec3 h = SampleGgxVndf(vLocal, alpha, u1, u2);
                if (!std::isfinite(h.x) || !std::isfinite(h.y) || !std::isfinite(h.z) ||
                    std::abs(glm::length(h) - 1.0f) > 1e-3f || h.z < -1e-4f)
                    return Fail("SampleGgxVndf returned invalid micronormal");
            }
        }
    }

    // 14. Phase 10 rectangular area light solid-angle weighting invariance to sample count
    {
        RtGpuLight rect{};
        rect.posAndType = { 0.0f, 4.0f, 0.0f, static_cast<float>(RtLightType::RectArea) };
        rect.dirAndRange = { 0.0f, -1.0f, 0.0f, 20.0f };
        rect.colorAndIntensity = { 1.0f, 1.0f, 1.0f, 4.0f };
        rect.areaRightAndHalfW = { 1.0f, 0.0f, 0.0f, 1.0f };
        rect.areaUpAndHalfH = { 0.0f, 0.0f, 1.0f, 1.0f };

        auto meanIrradiance = [&](int gridN) -> glm::vec3 {
            glm::vec3 sum(0.0f);
            for (int y = 0; y < gridN; ++y)
            {
                for (int x = 0; x < gridN; ++x)
                {
                    glm::vec3 sp{}, dir{}, rad{};
                    float dist = 0.0f;
                    const float u1 = (static_cast<float>(x) + 0.5f) / static_cast<float>(gridN);
                    const float u2 = (static_cast<float>(y) + 0.5f) / static_cast<float>(gridN);
                    if (EvaluateRectAreaLightSample({ 0, 0, 0 }, { 0, 1, 0 }, rect, u1, u2, sp, dir, dist, rad))
                        sum += rad;
                }
            }
            return sum / static_cast<float>(gridN * gridN);
        };
        const glm::vec3 avg2x2 = meanIrradiance(2);
        const glm::vec3 avg8x8 = meanIrradiance(8);
        if (avg8x8.r <= 0.1f || std::abs(avg2x2.r - avg8x8.r) / avg8x8.r > 0.06f)
            return Fail("Rect area light estimator mean diverged across sample counts");
    }

    // 15. Phase 12 temporal moments & disocclusion rejection math
    {
        float m1 = 0.0f, m2 = 0.0f, var = 0.0f;
        for (int i = 0; i < 16; ++i)
        {
            const float sample = (i % 2 == 0) ? 1.2f : 0.8f;
            const float alpha = (i == 0) ? 1.0f : 0.1f;
            const auto res = UpdateTemporalMoments(m1, m2, sample, alpha);
            m1 = res.m1;
            m2 = res.m2;
            var = res.variance;
        }
        if (m1 < 0.85f || m1 > 1.15f || var <= 0.005f)
            return Fail("Temporal moments failed to track mean & non-zero variance");

        if (ShouldRejectTemporalHistory({ 0, 0, 0 }, { 0.01f, 0, 0 }, 5.0f, { 0, 1, 0 }, { 0, 1, 0 }, 3u, 3u) ||
            !ShouldRejectTemporalHistory({ 0, 0, 0 }, { 0.01f, 0, 0 }, 5.0f, { 0, 1, 0 }, { 0, 1, 0 }, 3u, 4u) ||
            !ShouldRejectTemporalHistory({ 0, 0, 0 }, { 0.01f, 0, 0 }, 5.0f, { 0, 1, 0 }, { 0, 0, 1 }, 3u, 3u) ||
            !ShouldRejectTemporalHistory({ 0, 0, 0 }, { 0.80f, 0, 0 }, 5.0f, { 0, 1, 0 }, { 0, 1, 0 }, 3u, 3u))
            return Fail("Disocclusion rejection predicate failed");
    }

    std::cout << "RayTracingMathTests passed (15/15)\n";
    return 0;
}
