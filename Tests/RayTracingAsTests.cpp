#include <Core/AetherApplication.h>
#include <Renderer/MeshData.h>
#include <Renderer/RayTracingMath.h>
#include <RHI/Vulkan/Resources/VulkanAccelerationStructure.h>

#include <GLFW/glfw3.h>
#include <glm/gtc/quaternion.hpp>

#include <filesystem>
#include <iomanip>
#include <iostream>

namespace
{
    int FailAs(const char* step)
    {
        std::cerr << "rt-as FAILED at step: " << step << '\n';
        return 1;
    }
}

int Aether::AetherApplication::RunRayTracingAsTest()
{
    // Start with a single-cube scene so we can measure single-mesh / single-instance first.
    if (!Initialize(true))
        return FailAs("Initialize(true)");

    const auto& caps = m_vulkanContext.GetRayTracingCapabilities();
    if (!caps.IsReady() || !m_renderer.IsRayTracingSceneInitialized())
    {
        std::cout << "SKIP: Hardware Ray Tracing is not ready on this device\n";
        Shutdown();
        return 77;
    }

    auto& rtScene = m_renderer.m_rtScene;
    const VkDevice device = m_vulkanContext.GetDevice();
    const VkDeviceSize minScratchAlign = rtScene.GetMinScratchOffsetAlignment();
    const uint32_t preRtAllocations = m_memoryManager.GetAllocationCountForTest();

    // ------------------------------------------------------------------
    // 1. Direct VulkanAccelerationStructure lifecycle & fault injection
    // ------------------------------------------------------------------
    {
        VulkanAccelerationStructure probe;
        if (probe.InitializeWithFaultInjection(
                device, m_memoryManager, caps,
                VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR, 4096,
                true, false) != VK_ERROR_OUT_OF_DEVICE_MEMORY ||
            probe.IsValid() ||
            m_memoryManager.GetAllocationCountForTest() != preRtAllocations)
            return FailAs("AS fault injection: storage allocation");

        if (probe.InitializeWithFaultInjection(
                device, m_memoryManager, caps,
                VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR, 4096,
                false, true) != VK_ERROR_UNKNOWN ||
            probe.IsValid() ||
            probe.GetStorageBuffer().GetBuffer() != VK_NULL_HANDLE ||
            m_memoryManager.GetAllocationCountForTest() != preRtAllocations)
            return FailAs("AS fault injection: handle creation rollback");

        if (probe.Initialize(device, m_memoryManager, caps,
                VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR, 0) != VK_ERROR_INITIALIZATION_FAILED ||
            probe.Initialize(device, m_memoryManager, caps,
                VK_ACCELERATION_STRUCTURE_TYPE_GENERIC_KHR, 4096) != VK_ERROR_INITIALIZATION_FAILED ||
            m_memoryManager.GetAllocationCountForTest() != preRtAllocations)
            return FailAs("AS invalid argument rejection");

        VulkanAccelerationStructure directBlas;
        VulkanAccelerationStructure directTlas;
        if (directBlas.Initialize(device, m_memoryManager, caps,
                VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR, 4096) != VK_SUCCESS ||
            !directBlas.IsValid() || directBlas.GetHandle() == VK_NULL_HANDLE ||
            directBlas.GetDeviceAddress() == 0 ||
            directBlas.GetType() != VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR ||
            directBlas.GetSize() != 4096)
            return FailAs("direct BLAS AS create");

        if (directTlas.Initialize(device, m_memoryManager, caps,
                VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR, 4096) != VK_SUCCESS ||
            !directTlas.IsValid() || directTlas.GetHandle() == VK_NULL_HANDLE ||
            directTlas.GetDeviceAddress() == 0 ||
            directTlas.GetType() != VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR ||
            directTlas.GetSize() != 4096)
            return FailAs("direct TLAS AS create");

        if (m_memoryManager.GetAllocationCountForTest() != preRtAllocations + 2)
            return FailAs("direct AS allocation count");

        directTlas.Shutdown();
        directTlas.Shutdown(); // repeated cleanup must be safe
        directBlas.Shutdown();
        directBlas.Shutdown();
        if (directBlas.IsValid() || directTlas.IsValid() ||
            m_memoryManager.GetAllocationCountForTest() != preRtAllocations)
            return FailAs("direct AS shutdown leak check");
    }

    // ------------------------------------------------------------------
    // 2. Repeated VulkanRayTracingScene initialize / shutdown
    // ------------------------------------------------------------------
    for (int cycle = 0; cycle < 3; ++cycle)
    {
        rtScene.Shutdown();
        if (rtScene.IsInitialized() ||
            rtScene.GetActiveBlasCount() != 0 ||
            rtScene.GetDeferredBlasCount() != 0 ||
            rtScene.HasValidTlas(0) || rtScene.HasValidTlas(1) ||
            m_memoryManager.GetAllocationCountForTest() != preRtAllocations)
            return FailAs("repeated RTScene shutdown");

        if (!rtScene.Initialize(m_vulkanContext, m_memoryManager, MAX_FRAMES_IN_FLIGHT) ||
            !rtScene.IsInitialized())
            return FailAs("repeated RTScene re-initialize");
    }

    auto drawAndReturnSlot = [this]() -> int {
        const uint32_t slot = m_renderer.m_currentFrame;
        m_window.PollEvents();
        if (DrawScene() != VulkanRenderer::FrameResult::Rendered)
            return -1;
        return static_cast<int>(slot);
    };

    // Enable RT acceleration structure recording inside VulkanRenderer::DrawFrame.
    m_updateRayTracingScene = true;

    // ------------------------------------------------------------------
    // 3. Single mesh, single instance across both frame slots + timestamps
    // ------------------------------------------------------------------
    const int s0 = drawAndReturnSlot();
    if (s0 < 0)
        return FailAs("single-mesh frame 0 draw");
    if (rtScene.GetActiveBlasCount() != 1 ||
        !rtScene.HasValidTlas(static_cast<uint32_t>(s0)) ||
        rtScene.GetTlasInstanceCount(static_cast<uint32_t>(s0)) != 1 ||
        rtScene.GetLastRecordedTlasAction(static_cast<uint32_t>(s0)) != TlasBuildAction::InitialBuild ||
        !IsAddressAligned(rtScene.GetLastBlasScratchAddress(static_cast<uint32_t>(s0)), minScratchAlign) ||
        !IsAddressAligned(rtScene.GetLastTlasScratchAddress(static_cast<uint32_t>(s0)), minScratchAlign))
        return FailAs("single-mesh slot 0 assertions");

    const int s1 = drawAndReturnSlot();
    if (s1 < 0 || s1 == s0)
        return FailAs("single-mesh frame 1 draw");
    if (rtScene.GetActiveBlasCount() != 1 ||
        !rtScene.HasValidTlas(static_cast<uint32_t>(s1)) ||
        rtScene.GetTlasInstanceCount(static_cast<uint32_t>(s1)) != 1 ||
        rtScene.GetTlasHandle(0) == rtScene.GetTlasHandle(1) ||
        rtScene.GetLastRecordedTlasAction(static_cast<uint32_t>(s1)) != TlasBuildAction::InitialBuild ||
        !IsAddressAligned(rtScene.GetLastTlasScratchAddress(static_cast<uint32_t>(s1)), minScratchAlign))
        return FailAs("single-mesh slot 1 independent TLAS assertions");

    // Draw 2 more unchanged frames to flush timestamps and verify Skipped optimization.
    const int s2 = drawAndReturnSlot();
    const int s3 = drawAndReturnSlot();
    if (s2 < 0 || s3 < 0 ||
        rtScene.GetLastRecordedTlasAction(static_cast<uint32_t>(s2)) != TlasBuildAction::Skipped ||
        rtScene.GetLastRecordedTlasAction(static_cast<uint32_t>(s3)) != TlasBuildAction::Skipped)
        return FailAs("unchanged scene TLAS skip check");

    const double singleCubeBlasMs = rtScene.GetTimingStats().lastBlasBuildGpuTimeMs;
    const double smallTlasInitialMs = rtScene.GetTimingStats().lastTlasInitialBuildGpuTimeMs;

    // ------------------------------------------------------------------
    // 4. Single mesh, multiple instances (BLAS reuse, TLAS Rebuild)
    // ------------------------------------------------------------------
    {
        Transform t1{};
        t1.position = { -2.0f, 0.25f, 0.0f };
        t1.rotation = glm::angleAxis(glm::radians(35.0f), glm::vec3(0, 1, 0));
        t1.scale = { 0.8f, 1.2f, 0.9f };
        m_scene.AddObject({ t1, { 0 } });

        Transform t2{};
        t2.position = { 2.0f, -0.3f, 0.5f };
        t2.scale = { 1.1f, 0.7f, 1.3f };
        m_scene.AddObject({ t2, { 0 } });
    }
    const int mi0 = drawAndReturnSlot();
    const int mi1 = drawAndReturnSlot();
    if (mi0 < 0 || mi1 < 0 ||
        rtScene.GetActiveBlasCount() != 1 ||
        rtScene.GetTlasInstanceCount(0) != 3 ||
        rtScene.GetTlasInstanceCount(1) != 3 ||
        rtScene.GetLastRecordedTlasAction(static_cast<uint32_t>(mi0)) != TlasBuildAction::Rebuild ||
        rtScene.GetLastRecordedTlasAction(static_cast<uint32_t>(mi1)) != TlasBuildAction::Rebuild)
        return FailAs("single-mesh multi-instance BLAS reuse & TLAS rebuild");

    // ------------------------------------------------------------------
    // 5. Dynamic transform updates across both slots -> TLAS In-Place UPDATE
    // ------------------------------------------------------------------
    for (int f = 0; f < 4; ++f)
    {
        auto objs = m_scene.GetObjects();
        objs[0].transform.rotation = glm::angleAxis(glm::radians(15.0f * (f + 1)), glm::vec3(0, 1, 0));
        objs[1].transform.position.y = 0.25f + 0.1f * static_cast<float>(f + 1);
        const int slot = drawAndReturnSlot();
        if (slot < 0 ||
            rtScene.GetActiveBlasCount() != 1 ||
            rtScene.GetTlasInstanceCount(static_cast<uint32_t>(slot)) != 3 ||
            rtScene.GetLastRecordedTlasAction(static_cast<uint32_t>(slot)) != TlasBuildAction::Update)
            return FailAs("dynamic transform TLAS in-place update");
    }
    const double smallTlasRebuildMs = rtScene.GetTimingStats().lastTlasRebuildGpuTimeMs;
    const double smallTlasUpdateMs = rtScene.GetTimingStats().lastTlasUpdateGpuTimeMs;

    // ------------------------------------------------------------------
    // 6. Multiple unique meshes + sequential BLAS scratch reuse
    // ------------------------------------------------------------------
    const MeshId tetraId = m_meshes.size();
    {
        auto tetra = std::make_unique<VulkanMesh>();
        if (tetra->Initialize(device, m_memoryManager, CreateTetrahedronMesh(),
                VulkanMeshCreateOptions{ true }) != VK_SUCCESS)
            return FailAs("upload tetrahedron mesh");
        m_meshes.push_back(std::move(tetra));
    }
    const MeshId secondCubeId = m_meshes.size();
    {
        auto secondCube = std::make_unique<VulkanMesh>();
        if (secondCube->Initialize(device, m_memoryManager, CreateCubeMesh(),
                VulkanMeshCreateOptions{ true }) != VK_SUCCESS)
            return FailAs("upload second unique cube mesh");
        m_meshes.push_back(std::move(secondCube));
    }
    {
        Transform trA{};
        trA.position = { 0.0f, 1.8f, -0.5f };
        m_scene.AddObject({ trA, { tetraId } });

        Transform trB{};
        trB.position = { 0.0f, -1.8f, 0.5f };
        m_scene.AddObject({ trB, { secondCubeId } });
    }
    const int mm0 = drawAndReturnSlot();
    const int mm1 = drawAndReturnSlot();
    if (mm0 < 0 || mm1 < 0 ||
        rtScene.GetActiveBlasCount() != 3 ||
        rtScene.GetTlasInstanceCount(static_cast<uint32_t>(mm0)) != 5 ||
        rtScene.GetTlasInstanceCount(static_cast<uint32_t>(mm1)) != 5 ||
        !IsAddressAligned(rtScene.GetLastBlasScratchAddress(static_cast<uint32_t>(mm0)), minScratchAlign))
        return FailAs("multi-mesh sequential BLAS build & scratch reuse");

    // Flush slot mm0 timestamps by advancing another frame.
    if (drawAndReturnSlot() < 0)
        return FailAs("multi-mesh timestamp flush");
    const double multiMeshBlasMs = rtScene.GetTimingStats().lastBlasBuildGpuTimeMs;

    // ------------------------------------------------------------------
    // 7. Object removal & BLAS deferred destruction WITHOUT vkDeviceWaitIdle()
    // ------------------------------------------------------------------
    // Remove object referencing secondCubeId (index 4).
    if (!m_scene.RemoveObject(4))
        return FailAs("remove scene object");

    const int dr0 = drawAndReturnSlot();
    if (dr0 < 0 ||
        rtScene.GetActiveBlasCount() != 2 ||
        rtScene.GetDeferredBlasCount() < 1 ||
        rtScene.GetTlasInstanceCount(static_cast<uint32_t>(dr0)) != 4)
        return FailAs("deferred BLAS retention while other frame slot references it");

    // Advance across remaining frame slots so every prior TLAS snapshot referencing secondCubeId is replaced
    // and its fence has completed — without calling vkDeviceWaitIdle().
    if (drawAndReturnSlot() < 0 || drawAndReturnSlot() < 0)
        return FailAs("advance frames to complete deferred BLAS destruction");
    if (rtScene.GetActiveBlasCount() != 2 || rtScene.GetDeferredBlasCount() != 0)
        return FailAs("deferred BLAS must be destroyed after in-flight frames complete");

    // ------------------------------------------------------------------
    // 8. Empty Scene handling
    // ------------------------------------------------------------------
    m_scene.Clear();
    for (int f = 0; f < 3; ++f)
    {
        if (drawAndReturnSlot() < 0)
            return FailAs("empty scene draw");
    }
    if (rtScene.HasValidTlas(0) || rtScene.HasValidTlas(1) ||
        rtScene.GetTlasHandle(0) != VK_NULL_HANDLE ||
        rtScene.GetTlasHandle(1) != VK_NULL_HANDLE ||
        rtScene.GetTlasInstanceCount(0) != 0 ||
        rtScene.GetTlasInstanceCount(1) != 0 ||
        rtScene.GetActiveBlasCount() != 0 ||
        rtScene.GetDeferredBlasCount() != 0)
        return FailAs("empty scene TLAS invalidation & BLAS cleanup");

    // ------------------------------------------------------------------
    // 9. Asset replacement with embedded.glb (Opaque + Alpha MASK + Alpha BLEND)
    // ------------------------------------------------------------------
    {
        auto imported = ImportGltf(std::filesystem::path("asset-fixtures") / "embedded.glb");
        if (!imported.asset)
            return FailAs("import embedded.glb");
        auto secondMat = imported.asset->materials.front();
        secondMat.alphaMode = ImportedMaterial::AlphaMode::Blend;
        imported.asset->materials.push_back(secondMat);
        imported.asset->meshes[0][1].material = 1;
        imported.asset->meshes[0][1].extraTexCoords = imported.asset->meshes[0][0].extraTexCoords;

        if (!AddImportedAsset(std::move(*imported.asset)).empty())
            return FailAs("AddImportedAsset embedded.glb");

        for (int f = 0; f < 3; ++f)
        {
            if (drawAndReturnSlot() < 0)
                return FailAs("draw embedded.glb with RTScene");
        }
        // embedded.glb has 5 instances total: 2 Alpha-BLEND (excluded) + 3 Opaque/Mask (included).
        if (!rtScene.HasValidTlas(0) || !rtScene.HasValidTlas(1) ||
            rtScene.GetTlasInstanceCount(0) != 3 ||
            rtScene.GetTlasInstanceCount(1) != 3 ||
            rtScene.GetActiveBlasCount() != 2)
            return FailAs("embedded.glb BLAS/TLAS instance & alpha filtering");

        const auto meta = rtScene.GetInstanceMetadata(0);
        if (meta.size() != 3)
            return FailAs("embedded.glb metadata size");
        bool sawDoubleSided = false;
        bool sawMirrored = false;
        for (const auto& entry : meta)
        {
            sawDoubleSided = sawDoubleSided || entry.doubleSided;
            sawMirrored = sawMirrored || entry.mirrored;
        }
        if (!sawDoubleSided || !sawMirrored)
            return FailAs("embedded.glb doubleSided/mirrored metadata preservation");
    }

    // ------------------------------------------------------------------
    // 10. Optional high-poly DamagedHelmet.glb BLAS timing (if present)
    // ------------------------------------------------------------------
    double helmetBlasMs = -1.0;
    const std::filesystem::path helmetPath = "DamagedHelmet.glb";
    if (std::filesystem::exists(helmetPath))
    {
        auto helmet = ImportGltf(helmetPath);
        if (!helmet.asset)
            return FailAs("import DamagedHelmet.glb");
        if (!AddImportedAsset(std::move(*helmet.asset)).empty())
            return FailAs("AddImportedAsset DamagedHelmet.glb");
        for (int f = 0; f < 3; ++f)
        {
            if (drawAndReturnSlot() < 0)
                return FailAs("draw DamagedHelmet.glb with RTScene");
        }
        if (rtScene.GetActiveBlasCount() < 1 || !rtScene.HasValidTlas(0) || !rtScene.HasValidTlas(1))
            return FailAs("DamagedHelmet.glb BLAS/TLAS validity");
        helmetBlasMs = rtScene.GetTimingStats().lastBlasBuildGpuTimeMs;
    }

    // ------------------------------------------------------------------
    // 11. Multi-instance benchmark (1000 instances sharing 1 BLAS)
    // ------------------------------------------------------------------
    double tlas1000BuildMs = 0.0;
    double tlas1000UpdateMs = 0.0;
    {
        m_vulkanContext.WaitIdle();
        rtScene.OnDeviceIdle();
        m_drawItems.clear();
        m_scene.Clear();
        m_previewAsset.reset();
        m_previewMaterials.reset();
        m_meshes.clear();

        auto cube = std::make_unique<VulkanMesh>();
        if (cube->Initialize(device, m_memoryManager, CreateCubeMesh(),
                VulkanMeshCreateOptions{ true }) != VK_SUCCESS)
            return FailAs("upload cube for 1000-instance test");
        m_meshes.push_back(std::move(cube));

        for (int x = 0; x < 10; ++x)
        {
            for (int y = 0; y < 10; ++y)
            {
                for (int z = 0; z < 10; ++z)
                {
                    Transform t{};
                    t.position = {
                        static_cast<float>(x - 5) * 2.5f,
                        static_cast<float>(y - 5) * 2.5f,
                        static_cast<float>(z - 5) * 2.5f
                    };
                    t.scale = glm::vec3(0.45f);
                    m_scene.AddObject({ t, { 0 } });
                }
            }
        }

        // Initial build on both frame slots + flush.
        if (drawAndReturnSlot() < 0 || drawAndReturnSlot() < 0 || drawAndReturnSlot() < 0)
            return FailAs("1000-instance TLAS build");
        if (rtScene.GetActiveBlasCount() != 1 ||
            rtScene.GetTlasInstanceCount(0) != 1000 ||
            rtScene.GetTlasInstanceCount(1) != 1000)
            return FailAs("1000-instance BLAS reuse & TLAS count");
        tlas1000BuildMs = rtScene.GetTimingStats().lastTlasRebuildGpuTimeMs;
        if (tlas1000BuildMs <= 0.0)
            tlas1000BuildMs = rtScene.GetTimingStats().lastTlasGpuTimeMs;

        // Transform-only update on all 1000 instances across both slots + flush.
        for (int f = 0; f < 3; ++f)
        {
            auto objs = m_scene.GetObjects();
            const glm::quat rot = glm::angleAxis(glm::radians(5.0f * (f + 1)), glm::vec3(0, 1, 0));
            for (auto& o : objs)
                o.transform.rotation = rot;
            const int slot = drawAndReturnSlot();
            if (slot < 0 ||
                rtScene.GetLastRecordedTlasAction(static_cast<uint32_t>(slot)) != TlasBuildAction::Update)
                return FailAs("1000-instance TLAS update");
        }
        tlas1000UpdateMs = rtScene.GetTimingStats().lastTlasUpdateGpuTimeMs;
    }

    // ------------------------------------------------------------------
    // 12. Swapchain Resize while RT infrastructure exists
    // ------------------------------------------------------------------
    {
        glfwSetWindowSize(m_window.GetNativeHandle(), 900, 600);
        m_window.PollEvents();
        if (!RecreateSwapchain())
            return FailAs("RecreateSwapchain while RTScene exists");
        if (!m_renderer.IsRayTracingSceneInitialized() ||
            rtScene.GetActiveBlasCount() != 1 ||
            !rtScene.HasValidTlas(0) || !rtScene.HasValidTlas(1))
            return FailAs("RTScene state after swapchain resize");

        for (int f = 0; f < 4; ++f)
        {
            m_scene.GetObjects()[0].transform.position.y += 0.05f;
            if (drawAndReturnSlot() < 0)
                return FailAs("draw after swapchain resize with RTScene");
        }
    }

    // ------------------------------------------------------------------
    // 13. RecordFrame fault injection (BLAS/TLAS/Scratch) with zero VMA leaks
    // ------------------------------------------------------------------
    {
        m_vulkanContext.WaitIdle();
        rtScene.OnDeviceIdle();

        // Prepare a fresh mesh not yet in m_blasCache so BLAS build paths trigger cleanly.
        VulkanMesh faultMesh;
        if (faultMesh.Initialize(device, m_memoryManager, CreateTetrahedronMesh(),
                VulkanMeshCreateOptions{ true }) != VK_SUCCESS)
            return FailAs("fault-injection mesh init");

        VkCommandBuffer cmd = m_commandManager.GetCommandBuffer(0);
        auto beginProbeCmd = [&]() -> bool {
            if (vkResetCommandBuffer(cmd, 0) != VK_SUCCESS)
                return false;
            VkCommandBufferBeginInfo bi{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
            bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
            return vkBeginCommandBuffer(cmd, &bi) == VK_SUCCESS;
        };

        const RtSceneItem faultItem{ &faultMesh, glm::mat4(1.0f), 0, false, false, false, false };
        const std::array<RtSceneItem, 1> faultItems{ faultItem };

        const uint32_t faultBaseline = m_memoryManager.GetAllocationCountForTest();

        // 13a. BLAS storage allocation failure
        rtScene.m_faultInjection = {};
        rtScene.m_faultInjection.failBlasStorageAllocation = true;
        if (!beginProbeCmd() || rtScene.RecordFrame(cmd, 0, faultItems))
            return FailAs("fault injection failBlasStorageAllocation did not fail");
        vkEndCommandBuffer(cmd);
        if (m_memoryManager.GetAllocationCountForTest() != faultBaseline)
            return FailAs("fault injection failBlasStorageAllocation leaked VMA allocations");

        // 13b. BLAS handle creation failure
        rtScene.m_faultInjection = {};
        rtScene.m_faultInjection.failBlasAsCreate = true;
        if (!beginProbeCmd() || rtScene.RecordFrame(cmd, 0, faultItems))
            return FailAs("fault injection failBlasAsCreate did not fail");
        vkEndCommandBuffer(cmd);
        if (m_memoryManager.GetAllocationCountForTest() != faultBaseline)
            return FailAs("fault injection failBlasAsCreate leaked VMA allocations");

        // 13c. Scratch allocation failure
        rtScene.m_faultInjection = {};
        rtScene.m_faultInjection.failScratchAllocation = true;
        if (!beginProbeCmd() || rtScene.RecordFrame(cmd, 0, faultItems))
            return FailAs("fault injection failScratchAllocation did not fail");
        vkEndCommandBuffer(cmd);
        if (m_memoryManager.GetAllocationCountForTest() != faultBaseline)
            return FailAs("fault injection failScratchAllocation leaked VMA allocations");

        // 13d. TLAS storage allocation failure (using existing cached cube mesh)
        const RtSceneItem cubeItem{ m_meshes[0].get(), glm::mat4(1.0f), 0, false, false, false, false };
        const std::array<RtSceneItem, 1> cubeItems{ cubeItem };
        rtScene.m_faultInjection = {};
        rtScene.m_faultInjection.failTlasStorageAllocation = true;
        if (!beginProbeCmd() || rtScene.RecordFrame(cmd, 0, cubeItems))
            return FailAs("fault injection failTlasStorageAllocation did not fail");
        vkEndCommandBuffer(cmd);
        if (m_memoryManager.GetAllocationCountForTest() != faultBaseline)
            return FailAs("fault injection failTlasStorageAllocation leaked VMA allocations");

        // 13e. TLAS handle creation failure
        rtScene.m_faultInjection = {};
        rtScene.m_faultInjection.failTlasAsCreate = true;
        if (!beginProbeCmd() || rtScene.RecordFrame(cmd, 0, cubeItems))
            return FailAs("fault injection failTlasAsCreate did not fail");
        vkEndCommandBuffer(cmd);
        if (m_memoryManager.GetAllocationCountForTest() != faultBaseline)
            return FailAs("fault injection failTlasAsCreate leaked VMA allocations");

        rtScene.m_faultInjection = {};
    }

    // ------------------------------------------------------------------
    // 14. Final VMA zero-leak verification upon RTScene shutdown
    // ------------------------------------------------------------------
    m_vulkanContext.WaitIdle();
    const uint32_t activeAllocsBeforeRtShutdown = m_memoryManager.GetAllocationCountForTest();
    rtScene.Shutdown();
    const uint32_t allocsAfterRtShutdown = m_memoryManager.GetAllocationCountForTest();
    if (rtScene.IsInitialized() ||
        rtScene.GetActiveBlasCount() != 0 ||
        rtScene.GetDeferredBlasCount() != 0 ||
        rtScene.HasValidTlas(0) || rtScene.HasValidTlas(1) ||
        allocsAfterRtShutdown != preRtAllocations)
        return FailAs("final RTScene shutdown VMA allocation baseline check");

    std::cout << std::fixed << std::setprecision(4);
    std::cout << "=== Phase 9B RTX 5090 GPU Timestamp Summary (GPU-only execution) ===\n";
    std::cout << "   Min Scratch Offset Alignment: " << minScratchAlign << " bytes\n";
    std::cout << "   Single-Mesh BLAS Build (Cube):        " << singleCubeBlasMs << " ms\n";
    std::cout << "   Multi-Mesh BLAS Build (2 meshes):     " << multiMeshBlasMs << " ms\n";
    if (helmetBlasMs >= 0.0)
        std::cout << "   DamagedHelmet.glb BLAS Build:         " << helmetBlasMs << " ms\n";
    std::cout << "   TLAS Initial Build (1 instance):      " << smallTlasInitialMs << " ms\n";
    std::cout << "   TLAS Rebuild (3 instances):           " << smallTlasRebuildMs << " ms\n";
    std::cout << "   TLAS In-Place Update (3 instances):   " << smallTlasUpdateMs << " ms\n";
    std::cout << "   TLAS Rebuild (1000 instances):        " << tlas1000BuildMs << " ms\n";
    std::cout << "   TLAS In-Place Update (1000 instances):" << tlas1000UpdateMs << " ms\n";
    std::cout << "   VMA Allocations (Pre-RT / Active / Post-RT): "
              << preRtAllocations << " / " << activeAllocsBeforeRtShutdown
              << " / " << allocsAfterRtShutdown << '\n';
    std::cout << "Phase 9B BLAS/TLAS acceleration structure test suite verified\n";

    Shutdown();
    return 0;
}
