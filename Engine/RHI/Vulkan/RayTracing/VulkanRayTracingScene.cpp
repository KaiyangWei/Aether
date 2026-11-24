#include "VulkanRayTracingScene.h"

#include <Renderer/Vertex.h>
#include <RHI/Vulkan/Resources/VulkanMesh.h>
#include <RHI/Vulkan/VulkanContext.h>
#include <RHI/Vulkan/VulkanMemoryManager.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <iostream>
#include <unordered_set>

namespace Aether
{
    namespace
    {
        static_assert(offsetof(Vertex, position) == 0,
            "VulkanRayTracingScene expects Vertex::position at byte offset 0");
        static_assert(sizeof(Vertex) == 104,
            "VulkanRayTracingScene expects Vertex stride of 104 bytes");

        constexpr uint32_t kQueriesPerFrame = 4; // 0: blasBegin, 1: blasEnd, 2: tlasBegin, 3: tlasEnd

        void EmitMemoryBarrier(
            VkCommandBuffer cmd,
            VkPipelineStageFlags2 srcStage,
            VkAccessFlags2 srcAccess,
            VkPipelineStageFlags2 dstStage,
            VkAccessFlags2 dstAccess)
        {
            VkMemoryBarrier2 barrier{};
            barrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2;
            barrier.srcStageMask = srcStage;
            barrier.srcAccessMask = srcAccess;
            barrier.dstStageMask = dstStage;
            barrier.dstAccessMask = dstAccess;

            VkDependencyInfo dep{};
            dep.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
            dep.memoryBarrierCount = 1;
            dep.pMemoryBarriers = &barrier;
            vkCmdPipelineBarrier2(cmd, &dep);
        }
    }

    VulkanRayTracingScene::~VulkanRayTracingScene()
    {
        Shutdown();
    }

    bool VulkanRayTracingScene::Initialize(
        const VulkanContext& context,
        const VulkanMemoryManager& memory,
        uint32_t framesInFlight)
    {
        if (m_initialized || framesInFlight == 0 || context.GetDevice() == VK_NULL_HANDLE)
            return false;

        const auto& caps = context.GetRayTracingCapabilities();
        if (!caps.IsReady() || !memory.IsBufferDeviceAddressEnabled())
            return false;

        m_physicalDevice = context.GetPhysicalDevice();
        m_device = context.GetDevice();
        m_memory = &memory;
        m_capabilities = caps;
        m_minScratchOffsetAlignment = std::max<VkDeviceSize>(1, caps.minScratchOffsetAlignment);

        m_frames.clear();
        m_frames.reserve(framesInFlight);
        for (uint32_t i = 0; i < framesInFlight; ++i)
            m_frames.push_back(std::make_unique<FrameResources>());
        m_blasCache.clear();
        m_deferredRetirements.clear();
        m_submittedSerial = 0;
        m_completedSerial = 0;
        m_timingStats = {};

        // Optional GPU timestamp query pool (same convention as ShadowPass / VulkanTaaPass).
        VkPhysicalDeviceProperties props{};
        vkGetPhysicalDeviceProperties(m_physicalDevice, &props);
        uint32_t queueFamilyCount = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(m_physicalDevice, &queueFamilyCount, nullptr);
        std::vector<VkQueueFamilyProperties> queueProps(queueFamilyCount);
        vkGetPhysicalDeviceQueueFamilyProperties(m_physicalDevice, &queueFamilyCount, queueProps.data());
        const uint32_t gfxFamily = context.GetGraphicsQueueFamily();
        if (props.limits.timestampPeriod > 0.0f &&
            gfxFamily < queueProps.size() &&
            queueProps[gfxFamily].timestampValidBits > 0)
        {
            VkQueryPoolCreateInfo queryInfo{};
            queryInfo.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
            queryInfo.queryType = VK_QUERY_TYPE_TIMESTAMP;
            queryInfo.queryCount = framesInFlight * kQueriesPerFrame;
            if (vkCreateQueryPool(m_device, &queryInfo, nullptr, &m_timestampPool) == VK_SUCCESS)
            {
                m_timestampPeriodNs = static_cast<double>(props.limits.timestampPeriod);
                m_timingStats.timestampsSupported = true;
            }
        }

        m_initialized = true;
        return true;
    }

    void VulkanRayTracingScene::Shutdown() noexcept
    {
        if (!m_initialized && m_frames.empty() && m_blasCache.empty() &&
            m_deferredRetirements.empty() && m_timestampPool == VK_NULL_HANDLE)
            return;

        // Destroy per-frame TLAS and buffers first (dropping TLAS references to BLASes).
        for (auto& framePtr : m_frames)
        {
            if (!framePtr)
                continue;
            auto& frame = *framePtr;
            frame.tlas.Shutdown();
            frame.instanceBuffer.Shutdown();
            frame.instanceDataBuffer.Shutdown();
            frame.tlasScratchBuffer.Shutdown();
            frame.blasScratchBuffer.Shutdown();
            frame.referencedBlas.clear();
            frame.previousTopology.clear();
            frame.previousTransforms.clear();
            frame.instanceMetadata.clear();
            frame.instanceData.clear();
        }
        m_frames.clear();

        // Destroy all cached and deferred BLAS/buffer resources.
        m_blasCache.clear();
        m_deferredRetirements.clear();
        m_deferredBufferRetirements.clear();
        m_vertexStorageBuffer.reset();
        m_indexStorageBuffer.reset();
        m_geometryBufferGeneration = 0;
        m_whiteImage.Shutdown();
        m_whiteImageReady = false;
        if (m_whiteSampler != VK_NULL_HANDLE && m_device != VK_NULL_HANDLE)
        {
            vkDestroySampler(m_device, m_whiteSampler, nullptr);
            m_whiteSampler = VK_NULL_HANDLE;
        }

        if (m_timestampPool != VK_NULL_HANDLE && m_device != VK_NULL_HANDLE)
        {
            vkDestroyQueryPool(m_device, m_timestampPool, nullptr);
            m_timestampPool = VK_NULL_HANDLE;
        }

        m_physicalDevice = VK_NULL_HANDLE;
        m_device = VK_NULL_HANDLE;
        m_memory = nullptr;
        m_capabilities = {};
        m_minScratchOffsetAlignment = 1;
        m_timestampPeriodNs = 0.0;
        m_timingStats = {};
        m_submittedSerial = 0;
        m_completedSerial = 0;
        m_faultInjection = {};
        m_initialized = false;
    }

    void VulkanRayTracingScene::OnFrameFenceCompleted(uint32_t frameIndex)
    {
        if (!m_initialized || frameIndex >= m_frames.size())
            return;

        auto& frame = *m_frames[frameIndex];
        if (frame.submitSerial > m_completedSerial)
            m_completedSerial = frame.submitSerial;

        if (frame.hasPendingTimestamps)
        {
            ReadFrameTimestamps(frameIndex);
            frame.hasPendingTimestamps = false;
        }

        CollectDeferredRetirements();
    }

    void VulkanRayTracingScene::OnDeviceIdle()
    {
        if (!m_initialized)
            return;

        m_completedSerial = m_submittedSerial;
        for (uint32_t i = 0; i < m_frames.size(); ++i)
        {
            if (m_frames[i]->hasPendingTimestamps)
            {
                ReadFrameTimestamps(i);
                m_frames[i]->hasPendingTimestamps = false;
            }
        }
        CollectDeferredRetirements();
    }

    void VulkanRayTracingScene::ReadFrameTimestamps(uint32_t frameIndex)
    {
        if (m_timestampPool == VK_NULL_HANDLE || m_timestampPeriodNs <= 0.0)
            return;

        auto& frame = *m_frames[frameIndex];
        std::array<uint64_t, kQueriesPerFrame> stamps{};
        const uint32_t baseQuery = frameIndex * kQueriesPerFrame;

        if (frame.recordedBlasBuild)
        {
            if (vkGetQueryPoolResults(
                    m_device, m_timestampPool, baseQuery, 2,
                    sizeof(uint64_t) * 2, stamps.data(), sizeof(uint64_t),
                    VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT) == VK_SUCCESS &&
                stamps[1] >= stamps[0])
            {
                m_timingStats.lastBlasBuildGpuTimeMs =
                    static_cast<double>(stamps[1] - stamps[0]) * m_timestampPeriodNs * 1e-6;
                m_timingStats.lastCompletedBlasBuildCount = frame.recordedBlasCount;
            }
        }

        if (frame.recordedTlasOp)
        {
            if (vkGetQueryPoolResults(
                    m_device, m_timestampPool, baseQuery + 2, 2,
                    sizeof(uint64_t) * 2, stamps.data() + 2, sizeof(uint64_t),
                    VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT) == VK_SUCCESS &&
                stamps[3] >= stamps[2])
            {
                const double tlasMs =
                    static_cast<double>(stamps[3] - stamps[2]) * m_timestampPeriodNs * 1e-6;
                m_timingStats.lastTlasGpuTimeMs = tlasMs;
                m_timingStats.lastCompletedTlasAction = frame.lastRecordedTlasAction;
                m_timingStats.lastCompletedTlasInstanceCount = frame.recordedTlasInstanceCount;
                switch (frame.lastRecordedTlasAction)
                {
                case TlasBuildAction::InitialBuild:
                    m_timingStats.lastTlasInitialBuildGpuTimeMs = tlasMs;
                    break;
                case TlasBuildAction::Rebuild:
                    m_timingStats.lastTlasRebuildGpuTimeMs = tlasMs;
                    break;
                case TlasBuildAction::Update:
                    m_timingStats.lastTlasUpdateGpuTimeMs = tlasMs;
                    break;
                default:
                    break;
                }
            }
        }
    }

    void VulkanRayTracingScene::CollectDeferredRetirements() noexcept
    {
        auto it = m_deferredRetirements.begin();
        while (it != m_deferredRetirements.end())
        {
            // A retired BLAS is destroyed ONLY when:
            // 1. It is no longer in m_blasCache (no current Scene instance references it),
            // 2. No frame-slot TLAS snapshot references it (use_count == 1), AND
            // 3. All GPU submissions that could access it have completed (retireAfterSerial <= m_completedSerial).
            if (it->retireAfterSerial <= m_completedSerial && it->blas.use_count() == 1)
            {
                it = m_deferredRetirements.erase(it);
            }
            else
            {
                ++it;
            }
        }

        auto bufIt = m_deferredBufferRetirements.begin();
        while (bufIt != m_deferredBufferRetirements.end())
        {
            if (bufIt->retireAfterSerial <= m_completedSerial)
                bufIt = m_deferredBufferRetirements.erase(bufIt);
            else
                ++bufIt;
        }
    }

    bool VulkanRayTracingScene::EnsureBufferCapacity(
        VulkanBuffer& buffer,
        VkDeviceSize requiredSize,
        VkBufferUsageFlags usage,
        VkMemoryPropertyFlags requiredMem,
        VkMemoryPropertyFlags preferredMem,
        bool injectFailure) const
    {
        if (requiredSize == 0)
            return true;
        if (buffer.GetBuffer() != VK_NULL_HANDLE && buffer.GetSize() >= requiredSize && !injectFailure)
            return true;
        if (injectFailure)
        {
            std::cerr << "VulkanRayTracingScene::EnsureBufferCapacity() failed: injected allocation failure.\n";
            return false;
        }

        // Frame slot fence was already waited before RecordFrame(), so releasing the old slot buffer is safe.
        const VkDeviceSize newSize = std::max(requiredSize, buffer.GetSize() * 2);
        buffer.Shutdown();
        const VkResult res = buffer.Initialize(
            m_device, *m_memory, newSize, usage, requiredMem, preferredMem);
        if (res != VK_SUCCESS)
        {
            std::cerr << "VulkanRayTracingScene::EnsureBufferCapacity() failed: " << res << '\n';
            return false;
        }
        return true;
    }

    bool VulkanRayTracingScene::RecordFrame(
        VkCommandBuffer cmd,
        uint32_t frameIndex,
        std::span<const RtSceneItem> items)
    {
        if (!m_initialized || cmd == VK_NULL_HANDLE || frameIndex >= m_frames.size())
            return false;

        auto& frame = *m_frames[frameIndex];
        frame.recordedBlasBuild = false;
        frame.recordedTlasOp = false;
        frame.lastRecordedTlasAction = TlasBuildAction::None;
        frame.recordedBlasCount = 0;
        frame.recordedTlasInstanceCount = 0;
        frame.lastBlasScratchAddress = 0;
        frame.lastTlasScratchAddress = 0;

        const uint64_t upcomingSerial = m_submittedSerial + 1;

        if (m_timestampPool != VK_NULL_HANDLE)
        {
            vkCmdResetQueryPool(cmd, m_timestampPool, frameIndex * kQueriesPerFrame, kQueriesPerFrame);
        }

        // Step 1: Identify unique valid meshes needed by the current scene and collect pending BLAS builds.
        struct PendingBlasBuild
        {
            const VulkanMesh* mesh = nullptr;
            uint64_t meshUniqueId = 0;
            uint32_t vertexCount = 0;
            uint32_t indexCount = 0;
            uint32_t primitiveCount = 0;
            VkAccelerationStructureGeometryKHR geometry{};
            VkAccelerationStructureBuildGeometryInfoKHR buildInfo{};
            VkAccelerationStructureBuildSizesInfoKHR sizeInfo{};
            std::shared_ptr<VulkanAccelerationStructure> blas;
        };

        std::unordered_set<uint64_t> activeMeshIds;
        activeMeshIds.reserve(items.size());
        std::vector<PendingBlasBuild> pendingBlas;

        for (const auto& item : items)
        {
            // Translucent Alpha-BLEND objects are excluded from the opaque/mask TLAS (consistent with ShadowPass & TAA).
            if (item.alphaBlend)
                continue;
            if (!item.mesh || !IsFiniteMatrix(item.model))
                continue;

            const VulkanMesh& mesh = *item.mesh;
            const uint64_t meshId = mesh.GetUniqueId();
            const uint32_t vertexCount = mesh.GetVertexCount();
            const uint32_t indexCount = mesh.GetIndexCount();
            if (meshId == 0 || !mesh.IsRayTracingUsageEnabled() ||
                vertexCount == 0 || indexCount < 3 || (indexCount % 3) != 0)
                continue;

            const uint32_t primitiveCount = indexCount / 3;
            if (primitiveCount > m_capabilities.maxPrimitiveCount)
                continue;

            const VkDeviceAddress vertexAddr = mesh.GetVertexBuffer().GetDeviceAddress();
            const VkDeviceAddress indexAddr = mesh.GetIndexBuffer().GetDeviceAddress();
            if (vertexAddr == 0 || indexAddr == 0)
                continue;

            const bool firstSeenInScene = activeMeshIds.insert(meshId).second;
            if (!firstSeenInScene)
                continue;

            auto cacheIt = m_blasCache.find(meshId);
            if (cacheIt != m_blasCache.end() && cacheIt->second.blas && cacheIt->second.blas->IsValid())
                continue;

            PendingBlasBuild build{};
            build.mesh = &mesh;
            build.meshUniqueId = meshId;
            build.vertexCount = vertexCount;
            build.indexCount = indexCount;
            build.primitiveCount = primitiveCount;

            build.geometry.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR;
            build.geometry.geometryType = VK_GEOMETRY_TYPE_TRIANGLES_KHR;
            // Keep geometry flags non-opaque so instance-level FORCE_OPAQUE / FORCE_NO_OPAQUE controls AlphaMode
            // without forcing duplicate BLAS builds for shared meshes.
            build.geometry.flags = 0;
            build.geometry.geometry.triangles.sType =
                VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR;
            build.geometry.geometry.triangles.vertexFormat = VK_FORMAT_R32G32B32_SFLOAT;
            build.geometry.geometry.triangles.vertexData.deviceAddress = vertexAddr;
            build.geometry.geometry.triangles.vertexStride = sizeof(Vertex);
            build.geometry.geometry.triangles.maxVertex = vertexCount - 1;
            build.geometry.geometry.triangles.indexType = VK_INDEX_TYPE_UINT32;
            build.geometry.geometry.triangles.indexData.deviceAddress = indexAddr;
            build.geometry.geometry.triangles.transformData.deviceAddress = 0;

            build.buildInfo.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR;
            build.buildInfo.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
            build.buildInfo.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR;
            build.buildInfo.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
            build.buildInfo.geometryCount = 1;
            build.buildInfo.pGeometries = &build.geometry;

            build.sizeInfo.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR;
            m_capabilities.functions.vkGetAccelerationStructureBuildSizesKHR(
                m_device,
                VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR,
                &build.buildInfo,
                &build.primitiveCount,
                &build.sizeInfo);

            if (build.sizeInfo.accelerationStructureSize == 0)
            {
                std::cerr << "VulkanRayTracingScene: BLAS build size query returned 0 for mesh " << meshId << '\n';
                return false;
            }

            pendingBlas.push_back(std::move(build));
        }

        // Step 2: Allocate and record any pending BLAS builds using aligned scratch memory.
        if (!pendingBlas.empty())
        {
            VkDeviceSize maxBlasScratch = 0;
            for (auto& build : pendingBlas)
            {
                build.blas = std::make_shared<VulkanAccelerationStructure>();
                const VkResult res = build.blas->InitializeWithFaultInjection(
                    m_device,
                    *m_memory,
                    m_capabilities,
                    VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR,
                    build.sizeInfo.accelerationStructureSize,
                    m_faultInjection.failBlasStorageAllocation,
                    m_faultInjection.failBlasAsCreate);
                if (res != VK_SUCCESS)
                {
                    return false;
                }
                maxBlasScratch = std::max(maxBlasScratch, build.sizeInfo.buildScratchSize);
            }

            const VkDeviceSize paddedBlasScratch =
                ComputePaddedScratchSize(maxBlasScratch, m_minScratchOffsetAlignment);
            if (!EnsureBufferCapacity(
                    frame.blasScratchBuffer,
                    paddedBlasScratch,
                    VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
                    VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
                    0,
                    m_faultInjection.failScratchAllocation))
            {
                return false;
            }

            const VkDeviceAddress rawScratchAddr = frame.blasScratchBuffer.GetDeviceAddress();
            const VkDeviceAddress alignedScratchAddr =
                AlignUpDeviceAddress(rawScratchAddr, m_minScratchOffsetAlignment);
            if (!IsAddressAligned(alignedScratchAddr, m_minScratchOffsetAlignment) ||
                (alignedScratchAddr - rawScratchAddr) + maxBlasScratch > frame.blasScratchBuffer.GetSize())
            {
                std::cerr << "VulkanRayTracingScene: BLAS scratch alignment validation failed.\n";
                return false;
            }
            frame.lastBlasScratchAddress = alignedScratchAddr;

            // Make host-written vertex/index buffers visible to acceleration structure builds.
            EmitMemoryBarrier(
                cmd,
                VK_PIPELINE_STAGE_2_HOST_BIT,
                VK_ACCESS_2_HOST_WRITE_BIT,
                VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
                VK_ACCESS_2_SHADER_READ_BIT);

            if (m_timestampPool != VK_NULL_HANDLE)
            {
                vkCmdWriteTimestamp2(
                    cmd,
                    VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
                    m_timestampPool,
                    frameIndex * kQueriesPerFrame + 0);
            }

            for (size_t i = 0; i < pendingBlas.size(); ++i)
            {
                auto& build = pendingBlas[i];
                if (i > 0)
                {
                    // Serialize sequential reuse of frame.blasScratchBuffer across multiple BLAS builds.
                    EmitMemoryBarrier(
                        cmd,
                        VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
                        VK_ACCESS_2_ACCELERATION_STRUCTURE_WRITE_BIT_KHR |
                            VK_ACCESS_2_ACCELERATION_STRUCTURE_READ_BIT_KHR,
                        VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
                        VK_ACCESS_2_ACCELERATION_STRUCTURE_WRITE_BIT_KHR |
                            VK_ACCESS_2_ACCELERATION_STRUCTURE_READ_BIT_KHR);
                }

                // Re-bind pointer to build.geometry (in case vector reallocated earlier).
                build.buildInfo.pGeometries = &build.geometry;
                build.buildInfo.dstAccelerationStructure = build.blas->GetHandle();
                build.buildInfo.scratchData.deviceAddress = alignedScratchAddr;

                VkAccelerationStructureBuildRangeInfoKHR rangeInfo{};
                rangeInfo.primitiveCount = build.primitiveCount;
                rangeInfo.primitiveOffset = 0;
                rangeInfo.firstVertex = 0;
                rangeInfo.transformOffset = 0;
                const VkAccelerationStructureBuildRangeInfoKHR* pRangeInfo = &rangeInfo;

                m_capabilities.functions.vkCmdBuildAccelerationStructuresKHR(
                    cmd, 1, &build.buildInfo, &pRangeInfo);

                BlasEntry entry{};
                entry.meshUniqueId = build.meshUniqueId;
                entry.blas = build.blas;
                entry.vertexCount = build.vertexCount;
                entry.indexCount = build.indexCount;
                entry.builtInSubmitSerial = upcomingSerial;
                entry.lastUsedSubmitSerial = upcomingSerial;
                if (build.mesh)
                {
                    std::vector<Vertex> rawVertices(build.vertexCount);
                    std::vector<uint32_t> rawIndices(build.indexCount);
                    if (build.mesh->GetVertexBuffer().ReadData(rawVertices.data(), VkDeviceSize(build.vertexCount) * sizeof(Vertex)) == VK_SUCCESS &&
                        build.mesh->GetIndexBuffer().ReadData(rawIndices.data(), VkDeviceSize(build.indexCount) * sizeof(uint32_t)) == VK_SUCCESS)
                    {
                        entry.vertices.resize(build.vertexCount);
                        for (uint32_t vi = 0; vi < build.vertexCount; ++vi)
                        {
                            const Vertex& v = rawVertices[vi];
                            entry.vertices[vi].posAndU = glm::vec4(v.position, v.uv.x);
                            entry.vertices[vi].normalAndV = glm::vec4(v.normal, v.uv.y);
                        }
                        entry.indices = std::move(rawIndices);
                    }
                }
                m_blasCache[build.meshUniqueId] = std::move(entry);
            }

            if (m_timestampPool != VK_NULL_HANDLE)
            {
                vkCmdWriteTimestamp2(
                    cmd,
                    VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
                    m_timestampPool,
                    frameIndex * kQueriesPerFrame + 1);
            }

            frame.recordedBlasBuild = true;
            frame.recordedBlasCount = static_cast<uint32_t>(pendingBlas.size());
        }

        // Step 3: Evict unused BLASes from m_blasCache into m_deferredRetirements (never destroying in-use BLASes).
        bool evictedAnyBlas = false;
        for (auto it = m_blasCache.begin(); it != m_blasCache.end(); )
        {
            if (activeMeshIds.find(it->first) == activeMeshIds.end())
            {
                if (it->second.blas)
                {
                    m_deferredRetirements.push_back({
                        std::move(it->second.blas),
                        it->second.lastUsedSubmitSerial
                    });
                }
                it = m_blasCache.erase(it);
                evictedAnyBlas = true;
            }
            else
            {
                ++it;
            }
        }
        const bool geometrySetChanged = !pendingBlas.empty() || evictedAnyBlas;

        // Step 4: Build TLAS instance list, topology keys, transforms, and metadata.
        std::vector<VkAccelerationStructureInstanceKHR> instances;
        std::vector<InstanceTopologyKey> topology;
        std::vector<VkTransformMatrixKHR> transforms;
        std::vector<RtInstanceMetadata> metadata;
        std::vector<const RtSceneItem*> activeInstances;
        std::vector<std::shared_ptr<VulkanAccelerationStructure>> referencedBlas;
        std::unordered_set<uint64_t> referencedBlasIds;
        bool anyReferencedBlasInFlightBuild = !pendingBlas.empty();

        instances.reserve(items.size());
        topology.reserve(items.size());
        transforms.reserve(items.size());
        metadata.reserve(items.size());
        activeInstances.reserve(items.size());

        for (const auto& item : items)
        {
            if (item.alphaBlend || !item.mesh || !IsFiniteMatrix(item.model))
                continue;

            const uint64_t meshId = item.mesh->GetUniqueId();
            auto cacheIt = m_blasCache.find(meshId);
            if (cacheIt == m_blasCache.end() || !cacheIt->second.blas || !cacheIt->second.blas->IsValid())
                continue;

            const uint32_t customIndex = static_cast<uint32_t>(instances.size());
            if (!IsValidRtInstanceCustomIndex(customIndex) ||
                instances.size() >= m_capabilities.maxInstanceCount)
                break;

            const VkGeometryInstanceFlagsKHR instFlags =
                ComputeRtInstanceFlags(item.doubleSided, item.mirrored, item.alphaMask);
            constexpr uint8_t kOpaqueOrMaskInstanceMask = 0xFFu;
            const uint64_t blasAddress = cacheIt->second.blas->GetDeviceAddress();

            const auto builtInstance = BuildVkAccelerationStructureInstance({
                item.model,
                blasAddress,
                customIndex,
                kOpaqueOrMaskInstanceMask,
                0u,
                instFlags
            });
            if (!builtInstance)
                continue;

            cacheIt->second.lastUsedSubmitSerial = upcomingSerial;
            if (cacheIt->second.builtInSubmitSerial > m_completedSerial)
                anyReferencedBlasInFlightBuild = true;

            if (referencedBlasIds.insert(meshId).second)
                referencedBlas.push_back(cacheIt->second.blas);

            instances.push_back(*builtInstance);
            activeInstances.push_back(&item);
            topology.push_back({
                meshId,
                blasAddress,
                customIndex,
                kOpaqueOrMaskInstanceMask,
                instFlags
            });
            transforms.push_back(builtInstance->transform);
            metadata.push_back({
                item.sceneObjectIndex,
                meshId,
                item.doubleSided,
                item.mirrored,
                item.alphaMask
            });
        }

        // Step 5: Handle empty scene safely (no zero-primitive TLAS build, no stale TLAS handle).
        if (instances.empty())
        {
            frame.tlas.Shutdown();
            frame.hasValidTlas = false;
            frame.builtWithAllowUpdate = false;
            frame.instanceCount = 0;
            frame.tlasBuildSize = 0;
            frame.tlasUpdateScratchSize = 0;
            frame.referencedBlas.clear();
            frame.previousTopology.clear();
            frame.previousTransforms.clear();
            frame.instanceMetadata.clear();
            frame.lastRecordedTlasAction = TlasBuildAction::None;
            if (!SyncSceneDataBuffers(cmd, frameIndex, geometrySetChanged, activeInstances))
                return false;
            CollectDeferredRetirements();
            return true;
        }

        // Step 6: Decide between Skip, In-Place Update, and InitialBuild / Rebuild for this frame slot.
        const bool topologyMatches =
            frame.hasValidTlas &&
            frame.tlas.IsValid() &&
            topology.size() == frame.previousTopology.size() &&
            topology == frame.previousTopology;

        bool transformsMatch = topologyMatches && transforms.size() == frame.previousTransforms.size();
        if (transformsMatch)
        {
            for (size_t i = 0; i < transforms.size(); ++i)
            {
                if (!AreVkTransformMatricesEqual(transforms[i], frame.previousTransforms[i]))
                {
                    transformsMatch = false;
                    break;
                }
            }
        }

        if (topologyMatches && transformsMatch && !anyReferencedBlasInFlightBuild)
        {
            frame.instanceMetadata = std::move(metadata);
            frame.lastRecordedTlasAction = TlasBuildAction::Skipped;
            if (!SyncSceneDataBuffers(cmd, frameIndex, geometrySetChanged, activeInstances))
                return false;
            CollectDeferredRetirements();
            return true;
        }

        const bool canUpdateInPlace =
            topologyMatches &&
            !transformsMatch &&
            frame.builtWithAllowUpdate &&
            frame.tlasUpdateScratchSize > 0 &&
            frame.instanceCount == static_cast<uint32_t>(instances.size());

        const VkDeviceSize instanceBufferBytes =
            static_cast<VkDeviceSize>(instances.size()) * sizeof(VkAccelerationStructureInstanceKHR);
        if (!EnsureBufferCapacity(
                frame.instanceBuffer,
                instanceBufferBytes,
                VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR |
                    VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
                VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT,
                VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                false))
        {
            return false;
        }
        frame.instanceCapacity = static_cast<uint32_t>(
            frame.instanceBuffer.GetSize() / sizeof(VkAccelerationStructureInstanceKHR));

        if (frame.instanceBuffer.UploadData(instances.data(), instanceBufferBytes) != VK_SUCCESS)
        {
            std::cerr << "VulkanRayTracingScene: failed to upload TLAS instance buffer.\n";
            return false;
        }

        const VkDeviceAddress instanceBufferAddr = frame.instanceBuffer.GetDeviceAddress();
        if (instanceBufferAddr == 0)
        {
            std::cerr << "VulkanRayTracingScene: TLAS instance buffer device address is 0.\n";
            return false;
        }

        VkAccelerationStructureGeometryKHR tlasGeometry{};
        tlasGeometry.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR;
        tlasGeometry.geometryType = VK_GEOMETRY_TYPE_INSTANCES_KHR;
        tlasGeometry.flags = 0;
        tlasGeometry.geometry.instances.sType =
            VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_INSTANCES_DATA_KHR;
        tlasGeometry.geometry.instances.arrayOfPointers = VK_FALSE;
        tlasGeometry.geometry.instances.data.deviceAddress = instanceBufferAddr;

        const uint32_t instanceCount = static_cast<uint32_t>(instances.size());
        constexpr VkBuildAccelerationStructureFlagsKHR kTlasBuildFlags =
            VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR |
            VK_BUILD_ACCELERATION_STRUCTURE_ALLOW_UPDATE_BIT_KHR;

        VkAccelerationStructureBuildGeometryInfoKHR tlasBuildInfo{};
        tlasBuildInfo.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR;
        tlasBuildInfo.type = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR;
        tlasBuildInfo.flags = kTlasBuildFlags;
        tlasBuildInfo.mode = canUpdateInPlace
            ? VK_BUILD_ACCELERATION_STRUCTURE_MODE_UPDATE_KHR
            : VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
        tlasBuildInfo.geometryCount = 1;
        tlasBuildInfo.pGeometries = &tlasGeometry;

        VkAccelerationStructureBuildSizesInfoKHR tlasSizes{};
        tlasSizes.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR;
        m_capabilities.functions.vkGetAccelerationStructureBuildSizesKHR(
            m_device,
            VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR,
            &tlasBuildInfo,
            &instanceCount,
            &tlasSizes);

        if (tlasSizes.accelerationStructureSize == 0)
        {
            std::cerr << "VulkanRayTracingScene: TLAS build size query returned 0.\n";
            return false;
        }

        const TlasBuildAction action = canUpdateInPlace
            ? TlasBuildAction::Update
            : (frame.hasValidTlas ? TlasBuildAction::Rebuild : TlasBuildAction::InitialBuild);

        if (!canUpdateInPlace)
        {
            const bool needNewTlasObject =
                !frame.tlas.IsValid() ||
                frame.tlas.GetSize() < tlasSizes.accelerationStructureSize ||
                m_faultInjection.failTlasStorageAllocation ||
                m_faultInjection.failTlasAsCreate;
            if (needNewTlasObject)
            {
                // Allocate and verify the replacement before replacing the slot's TLAS,
                // or release first if not fault-injecting so peak memory stays minimal.
                if (m_faultInjection.failTlasStorageAllocation || m_faultInjection.failTlasAsCreate)
                {
                    VulkanAccelerationStructure probe;
                    const VkResult probeRes = probe.InitializeWithFaultInjection(
                        m_device,
                        *m_memory,
                        m_capabilities,
                        VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR,
                        tlasSizes.accelerationStructureSize,
                        m_faultInjection.failTlasStorageAllocation,
                        m_faultInjection.failTlasAsCreate);
                    if (probeRes != VK_SUCCESS)
                        return false;
                }

                frame.tlas.Shutdown();
                const VkResult res = frame.tlas.Initialize(
                    m_device,
                    *m_memory,
                    m_capabilities,
                    VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR,
                    tlasSizes.accelerationStructureSize);
                if (res != VK_SUCCESS)
                {
                    frame.hasValidTlas = false;
                    return false;
                }
            }
            frame.tlasBuildSize = tlasSizes.accelerationStructureSize;
            frame.tlasUpdateScratchSize = tlasSizes.updateScratchSize;
        }

        const VkDeviceSize requiredTlasScratch = canUpdateInPlace
            ? tlasSizes.updateScratchSize
            : std::max(tlasSizes.buildScratchSize, tlasSizes.updateScratchSize);
        const VkDeviceSize paddedTlasScratch =
            ComputePaddedScratchSize(requiredTlasScratch, m_minScratchOffsetAlignment);
        if (!EnsureBufferCapacity(
                frame.tlasScratchBuffer,
                paddedTlasScratch,
                VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
                VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
                0,
                m_faultInjection.failScratchAllocation))
        {
            return false;
        }

        const VkDeviceAddress rawTlasScratchAddr = frame.tlasScratchBuffer.GetDeviceAddress();
        const VkDeviceAddress alignedTlasScratchAddr =
            AlignUpDeviceAddress(rawTlasScratchAddr, m_minScratchOffsetAlignment);
        if (!IsAddressAligned(alignedTlasScratchAddr, m_minScratchOffsetAlignment) ||
            (alignedTlasScratchAddr - rawTlasScratchAddr) + requiredTlasScratch > frame.tlasScratchBuffer.GetSize())
        {
            std::cerr << "VulkanRayTracingScene: TLAS scratch alignment validation failed.\n";
            return false;
        }
        frame.lastTlasScratchAddress = alignedTlasScratchAddr;

        // Step 7: Emit Synchronization2 barriers before TLAS build/update.
        // 7a. Host writes to instanceBuffer[f] -> AS build read.
        EmitMemoryBarrier(
            cmd,
            VK_PIPELINE_STAGE_2_HOST_BIT,
            VK_ACCESS_2_HOST_WRITE_BIT,
            VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
            VK_ACCESS_2_SHADER_READ_BIT);

        // 7b. BLAS build writes (same or prior in-flight submission) -> TLAS build read.
        if (anyReferencedBlasInFlightBuild)
        {
            EmitMemoryBarrier(
                cmd,
                VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
                VK_ACCESS_2_ACCELERATION_STRUCTURE_WRITE_BIT_KHR,
                VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
                VK_ACCESS_2_ACCELERATION_STRUCTURE_READ_BIT_KHR);
        }

        tlasBuildInfo.srcAccelerationStructure =
            canUpdateInPlace ? frame.tlas.GetHandle() : VK_NULL_HANDLE;
        tlasBuildInfo.dstAccelerationStructure = frame.tlas.GetHandle();
        tlasBuildInfo.scratchData.deviceAddress = alignedTlasScratchAddr;

        VkAccelerationStructureBuildRangeInfoKHR tlasRange{};
        tlasRange.primitiveCount = instanceCount;
        tlasRange.primitiveOffset = 0;
        tlasRange.firstVertex = 0;
        tlasRange.transformOffset = 0;
        const VkAccelerationStructureBuildRangeInfoKHR* pTlasRange = &tlasRange;

        if (m_timestampPool != VK_NULL_HANDLE)
        {
            vkCmdWriteTimestamp2(
                cmd,
                VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
                m_timestampPool,
                frameIndex * kQueriesPerFrame + 2);
        }

        m_capabilities.functions.vkCmdBuildAccelerationStructuresKHR(
            cmd, 1, &tlasBuildInfo, &pTlasRange);

        if (m_timestampPool != VK_NULL_HANDLE)
        {
            vkCmdWriteTimestamp2(
                cmd,
                VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
                m_timestampPool,
                frameIndex * kQueriesPerFrame + 3);
        }

        frame.hasValidTlas = true;
        frame.builtWithAllowUpdate = true;
        frame.instanceCount = instanceCount;
        frame.referencedBlas = std::move(referencedBlas);
        frame.previousTopology = std::move(topology);
        frame.previousTransforms = std::move(transforms);
        frame.instanceMetadata = std::move(metadata);
        frame.recordedTlasOp = true;
        frame.lastRecordedTlasAction = action;
        frame.recordedTlasInstanceCount = instanceCount;

        if (!SyncSceneDataBuffers(cmd, frameIndex, geometrySetChanged, activeInstances))
            return false;

        // Dropping previous frame.referencedBlas may allow already-completed retired BLASes to be freed now.
        CollectDeferredRetirements();
        return true;
    }

    bool VulkanRayTracingScene::SyncSceneDataBuffers(
        VkCommandBuffer cmd,
        uint32_t frameIndex,
        bool geometrySetChanged,
        std::span<const RtSceneItem* const> activeInstances)
    {
        auto& frame = *m_frames[frameIndex];

        if (m_whiteImage.GetImage() == VK_NULL_HANDLE)
        {
            if (m_whiteImage.Initialize(
                    m_device, *m_memory, { 1, 1 }, VK_FORMAT_R8G8B8A8_UNORM,
                    VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                    VK_IMAGE_ASPECT_COLOR_BIT) != VK_SUCCESS)
                return false;
            m_whiteImageReady = false;
        }
        if (m_whiteSampler == VK_NULL_HANDLE)
        {
            VkSamplerCreateInfo samplerInfo{ VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
            samplerInfo.magFilter = VK_FILTER_LINEAR;
            samplerInfo.minFilter = VK_FILTER_LINEAR;
            samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
            samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT;
            samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT;
            samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
            samplerInfo.maxLod = 1.0f;
            if (vkCreateSampler(m_device, &samplerInfo, nullptr, &m_whiteSampler) != VK_SUCCESS)
                return false;
        }
        if (!m_whiteImageReady)
        {
            VkImageMemoryBarrier2 toClear{ VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2 };
            toClear.srcStageMask = VK_PIPELINE_STAGE_2_NONE;
            toClear.srcAccessMask = 0;
            toClear.dstStageMask = VK_PIPELINE_STAGE_2_CLEAR_BIT;
            toClear.dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
            toClear.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            toClear.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            toClear.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            toClear.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            toClear.image = m_whiteImage.GetImage();
            toClear.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
            VkDependencyInfo dep0{ VK_STRUCTURE_TYPE_DEPENDENCY_INFO };
            dep0.imageMemoryBarrierCount = 1;
            dep0.pImageMemoryBarriers = &toClear;
            vkCmdPipelineBarrier2(cmd, &dep0);

            const VkClearColorValue white{ { 1.0f, 1.0f, 1.0f, 1.0f } };
            const VkImageSubresourceRange range{ VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
            vkCmdClearColorImage(cmd, m_whiteImage.GetImage(), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &white, 1, &range);

            VkImageMemoryBarrier2 toRead = toClear;
            toRead.srcStageMask = VK_PIPELINE_STAGE_2_CLEAR_BIT;
            toRead.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
            toRead.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
            toRead.dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
            toRead.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            toRead.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            VkDependencyInfo dep1{ VK_STRUCTURE_TYPE_DEPENDENCY_INFO };
            dep1.imageMemoryBarrierCount = 1;
            dep1.pImageMemoryBarriers = &toRead;
            vkCmdPipelineBarrier2(cmd, &dep1);

            m_whiteImageReady = true;
        }

        if (geometrySetChanged || !m_vertexStorageBuffer || !m_indexStorageBuffer)
        {
            std::vector<uint64_t> sortedIds;
            sortedIds.reserve(m_blasCache.size());
            for (const auto& [id, _] : m_blasCache)
                sortedIds.push_back(id);
            std::sort(sortedIds.begin(), sortedIds.end());

            std::vector<RtGpuVertex> packedVertices;
            std::vector<uint32_t> packedIndices;
            for (uint64_t id : sortedIds)
            {
                auto& entry = m_blasCache.at(id);
                entry.firstVertex = static_cast<uint32_t>(packedVertices.size());
                entry.firstIndex = static_cast<uint32_t>(packedIndices.size());
                packedVertices.insert(packedVertices.end(), entry.vertices.begin(), entry.vertices.end());
                packedIndices.insert(packedIndices.end(), entry.indices.begin(), entry.indices.end());
            }
            if (packedVertices.empty())
                packedVertices.push_back({});
            if (packedIndices.empty())
                packedIndices.push_back(0u);

            if (m_vertexStorageBuffer)
            {
                m_deferredBufferRetirements.push_back({ std::move(m_vertexStorageBuffer), m_submittedSerial });
            }
            if (m_indexStorageBuffer)
            {
                m_deferredBufferRetirements.push_back({ std::move(m_indexStorageBuffer), m_submittedSerial });
            }

            m_vertexStorageBuffer = std::make_unique<VulkanBuffer>();
            const VkDeviceSize vBytes = static_cast<VkDeviceSize>(packedVertices.size()) * sizeof(RtGpuVertex);
            if (m_vertexStorageBuffer->Initialize(
                    m_device, *m_memory, vBytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                    VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT, VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) != VK_SUCCESS ||
                m_vertexStorageBuffer->UploadData(packedVertices.data(), vBytes) != VK_SUCCESS)
                return false;

            m_indexStorageBuffer = std::make_unique<VulkanBuffer>();
            const VkDeviceSize iBytes = static_cast<VkDeviceSize>(packedIndices.size()) * sizeof(uint32_t);
            if (m_indexStorageBuffer->Initialize(
                    m_device, *m_memory, iBytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                    VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT, VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) != VK_SUCCESS ||
                m_indexStorageBuffer->UploadData(packedIndices.data(), iBytes) != VK_SUCCESS)
                return false;

            ++m_geometryBufferGeneration;
        }

        const VkDescriptorImageInfo defaultWhite{
            m_whiteSampler, m_whiteImage.GetView(), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
        };
        frame.alphaMaskInfos.fill(defaultWhite);
        uint32_t nextMaskSlot = 1;

        frame.instanceData.clear();
        frame.instanceData.reserve(std::max<size_t>(1, activeInstances.size()));
        for (const RtSceneItem* itemPtr : activeInstances)
        {
            const RtSceneItem& item = *itemPtr;
            RtGpuInstanceData gpuInst{};
            gpuInst.baseColorAndAlphaCutoff = glm::vec4(glm::vec3(item.material.baseColor), item.material.alphaCutoff);
            gpuInst.emissiveAndMetallic = glm::vec4(item.material.emissive, item.material.metallic);
            const uint32_t flagBits =
                (item.alphaMask ? 1u : 0u) |
                (item.doubleSided ? 2u : 0u) |
                (item.mirrored ? 4u : 0u);
            gpuInst.surfaceParams = glm::vec4(
                std::clamp(item.material.roughness, 0.02f, 1.0f),
                std::clamp(item.material.ao, 0.0f, 1.0f),
                item.material.baseColor.a,
                static_cast<float>(flagBits));

            const glm::mat4 modelT = glm::transpose(item.model);
            gpuInst.modelRows[0] = modelT[0];
            gpuInst.modelRows[1] = modelT[1];
            gpuInst.modelRows[2] = modelT[2];

            const glm::mat4 prevM = item.hasPrevModel ? item.prevModel : item.model;
            const glm::mat4 prevMT = glm::transpose(prevM);
            gpuInst.prevModelRows[0] = prevMT[0];
            gpuInst.prevModelRows[1] = prevMT[1];
            gpuInst.prevModelRows[2] = prevMT[2];

            const glm::mat4 invM = glm::inverse(item.model);
            const glm::mat4 invMT = glm::transpose(invM);
            gpuInst.invModelRows[0] = invMT[0];
            gpuInst.invModelRows[1] = invMT[1];
            gpuInst.invModelRows[2] = invMT[2];

            const auto blasIt = m_blasCache.find(item.mesh->GetUniqueId());
            if (blasIt != m_blasCache.end())
            {
                gpuInst.firstVertex = blasIt->second.firstVertex;
                gpuInst.firstIndex = blasIt->second.firstIndex;
            }

            uint32_t maskSlot = 0;
            if (item.alphaMask && item.alphaMaskView != VK_NULL_HANDLE && item.alphaMaskSampler != VK_NULL_HANDLE)
            {
                for (uint32_t s = 1; s < nextMaskSlot; ++s)
                {
                    if (frame.alphaMaskInfos[s].imageView == item.alphaMaskView &&
                        frame.alphaMaskInfos[s].sampler == item.alphaMaskSampler)
                    {
                        maskSlot = s;
                        break;
                    }
                }
                if (maskSlot == 0 && nextMaskSlot < kMaxRtAlphaMaskTextures)
                {
                    maskSlot = nextMaskSlot++;
                    frame.alphaMaskInfos[maskSlot] = {
                        item.alphaMaskSampler, item.alphaMaskView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
                    };
                }
            }
            gpuInst.alphaMaskTexIndex = maskSlot;
            gpuInst.sceneObjectIndex = item.sceneObjectIndex;
            frame.instanceData.push_back(gpuInst);
        }

        bool alphaTableChanged = (frame.alphaMaskTableGeneration == 0);
        for (uint32_t s = 0; s < kMaxRtAlphaMaskTextures && !alphaTableChanged; ++s)
        {
            if (frame.alphaMaskInfos[s].imageView != frame.prevAlphaMaskInfos[s].imageView ||
                frame.alphaMaskInfos[s].sampler != frame.prevAlphaMaskInfos[s].sampler)
                alphaTableChanged = true;
        }
        if (alphaTableChanged)
        {
            frame.prevAlphaMaskInfos = frame.alphaMaskInfos;
            ++frame.alphaMaskTableGeneration;
        }

        if (frame.instanceData.empty())
            frame.instanceData.push_back({});

        const VkDeviceSize instDataBytes =
            static_cast<VkDeviceSize>(frame.instanceData.size()) * sizeof(RtGpuInstanceData);
        const VkBuffer prevInstDataHandle = frame.instanceDataBuffer.GetBuffer();
        if (!EnsureBufferCapacity(
                frame.instanceDataBuffer,
                instDataBytes,
                VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT,
                VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                false))
            return false;
        if (frame.instanceDataBuffer.GetBuffer() != prevInstDataHandle || frame.instanceDataGeneration == 0)
            ++frame.instanceDataGeneration;

        if (frame.instanceDataBuffer.UploadData(frame.instanceData.data(), instDataBytes) != VK_SUCCESS)
            return false;

        EmitMemoryBarrier(
            cmd,
            VK_PIPELINE_STAGE_2_HOST_BIT,
            VK_ACCESS_2_HOST_WRITE_BIT,
            VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
            VK_ACCESS_2_SHADER_STORAGE_READ_BIT);

        return true;
    }

    void VulkanRayTracingScene::OnFrameSubmitted(uint32_t frameIndex)
    {
        if (!m_initialized || frameIndex >= m_frames.size())
            return;

        ++m_submittedSerial;
        auto& frame = *m_frames[frameIndex];
        frame.submitSerial = m_submittedSerial;
        frame.hasPendingTimestamps =
            m_timestampPool != VK_NULL_HANDLE && (frame.recordedBlasBuild || frame.recordedTlasOp);
    }

    bool VulkanRayTracingScene::HasValidTlas(uint32_t frameIndex) const noexcept
    {
        return m_initialized && frameIndex < m_frames.size() &&
               m_frames[frameIndex]->hasValidTlas && m_frames[frameIndex]->tlas.IsValid();
    }

    VkAccelerationStructureKHR VulkanRayTracingScene::GetTlasHandle(uint32_t frameIndex) const noexcept
    {
        return HasValidTlas(frameIndex) ? m_frames[frameIndex]->tlas.GetHandle() : VK_NULL_HANDLE;
    }

    VkDeviceAddress VulkanRayTracingScene::GetTlasDeviceAddress(uint32_t frameIndex) const noexcept
    {
        return HasValidTlas(frameIndex) ? m_frames[frameIndex]->tlas.GetDeviceAddress() : 0;
    }

    uint32_t VulkanRayTracingScene::GetTlasInstanceCount(uint32_t frameIndex) const noexcept
    {
        return HasValidTlas(frameIndex) ? m_frames[frameIndex]->instanceCount : 0;
    }

    TlasBuildAction VulkanRayTracingScene::GetLastRecordedTlasAction(uint32_t frameIndex) const noexcept
    {
        return (m_initialized && frameIndex < m_frames.size())
            ? m_frames[frameIndex]->lastRecordedTlasAction
            : TlasBuildAction::None;
    }

    std::span<const RtInstanceMetadata> VulkanRayTracingScene::GetInstanceMetadata(uint32_t frameIndex) const noexcept
    {
        if (!m_initialized || frameIndex >= m_frames.size())
            return {};
        return m_frames[frameIndex]->instanceMetadata;
    }

    uint32_t VulkanRayTracingScene::GetFrameReferencedBlasCount(uint32_t frameIndex) const noexcept
    {
        if (!m_initialized || frameIndex >= m_frames.size())
            return 0;
        return static_cast<uint32_t>(m_frames[frameIndex]->referencedBlas.size());
    }

    VkDeviceAddress VulkanRayTracingScene::GetLastBlasScratchAddress(uint32_t frameIndex) const noexcept
    {
        if (!m_initialized || frameIndex >= m_frames.size())
            return 0;
        return m_frames[frameIndex]->lastBlasScratchAddress;
    }

    VkDeviceAddress VulkanRayTracingScene::GetLastTlasScratchAddress(uint32_t frameIndex) const noexcept
    {
        if (!m_initialized || frameIndex >= m_frames.size())
            return 0;
        return m_frames[frameIndex]->lastTlasScratchAddress;
    }

    VkBuffer VulkanRayTracingScene::GetVertexStorageBuffer() const noexcept
    {
        return (m_initialized && m_vertexStorageBuffer) ? m_vertexStorageBuffer->GetBuffer() : VK_NULL_HANDLE;
    }

    VkBuffer VulkanRayTracingScene::GetIndexStorageBuffer() const noexcept
    {
        return (m_initialized && m_indexStorageBuffer) ? m_indexStorageBuffer->GetBuffer() : VK_NULL_HANDLE;
    }

    VkBuffer VulkanRayTracingScene::GetInstanceDataBuffer(uint32_t frameIndex) const noexcept
    {
        if (!m_initialized || frameIndex >= m_frames.size())
            return VK_NULL_HANDLE;
        return m_frames[frameIndex]->instanceDataBuffer.GetBuffer();
    }

    uint64_t VulkanRayTracingScene::GetInstanceDataBufferGeneration(uint32_t frameIndex) const noexcept
    {
        if (!m_initialized || frameIndex >= m_frames.size())
            return 0;
        return m_frames[frameIndex]->instanceDataGeneration;
    }

    std::span<const VkDescriptorImageInfo> VulkanRayTracingScene::GetAlphaMaskImageInfos(uint32_t frameIndex) const noexcept
    {
        if (!m_initialized || frameIndex >= m_frames.size())
            return {};
        return m_frames[frameIndex]->alphaMaskInfos;
    }

    uint64_t VulkanRayTracingScene::GetAlphaMaskTableGeneration(uint32_t frameIndex) const noexcept
    {
        if (!m_initialized || frameIndex >= m_frames.size())
            return 0;
        return m_frames[frameIndex]->alphaMaskTableGeneration;
    }
}
