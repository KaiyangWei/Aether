#pragma once

#include <Renderer/RayTracingMath.h>
#include <Renderer/RtLighting.h>
#include <RHI/Vulkan/Resources/VulkanAccelerationStructure.h>
#include <RHI/Vulkan/Resources/VulkanBuffer.h>
#include <RHI/Vulkan/Resources/VulkanImage.h>
#include <RHI/Vulkan/VulkanRayTracingCapabilities.h>

#include <glm/mat4x4.hpp>
#include <vulkan/vulkan.h>

#include <array>
#include <cstdint>
#include <memory>
#include <span>
#include <unordered_map>
#include <vector>

namespace Aether
{
    class VulkanContext;
    class VulkanMemoryManager;
    class VulkanMesh;

    struct RtSceneItem
    {
        const VulkanMesh* mesh = nullptr;
        glm::mat4 model{ 1.0f };
        uint32_t sceneObjectIndex = 0;
        bool mirrored = false;
        bool doubleSided = false;
        bool alphaMask = false;
        bool alphaBlend = false;
        glm::mat4 prevModel{ 1.0f };
        bool hasPrevModel = false;
        RtMaterialSummary material{};
        VkImageView alphaMaskView = VK_NULL_HANDLE;
        VkSampler alphaMaskSampler = VK_NULL_HANDLE;
    };

    // Dense per-TLAS instance metadata indexed by VkAccelerationStructureInstanceKHR::instanceCustomIndex.
    // Preserves SceneObject / Mesh / AlphaMode mapping for Phase 9C Ray Query consumption.
    struct RtInstanceMetadata
    {
        uint32_t sceneObjectIndex = 0;
        uint64_t meshUniqueId = 0;
        bool doubleSided = false;
        bool mirrored = false;
        bool alphaMask = false;
    };

    enum class TlasBuildAction : uint8_t
    {
        None = 0,
        Skipped,
        InitialBuild,
        Rebuild,
        Update
    };

    struct RtSceneTimingStats
    {
        double lastBlasBuildGpuTimeMs = 0.0;
        double lastTlasInitialBuildGpuTimeMs = 0.0;
        double lastTlasRebuildGpuTimeMs = 0.0;
        double lastTlasUpdateGpuTimeMs = 0.0;
        double lastTlasGpuTimeMs = 0.0;
        TlasBuildAction lastCompletedTlasAction = TlasBuildAction::None;
        uint32_t lastCompletedBlasBuildCount = 0;
        uint32_t lastCompletedTlasInstanceCount = 0;
        bool timestampsSupported = false;
    };

    class VulkanRayTracingScene
    {
    public:
        VulkanRayTracingScene() = default;
        ~VulkanRayTracingScene();

        VulkanRayTracingScene(const VulkanRayTracingScene&) = delete;
        VulkanRayTracingScene& operator=(const VulkanRayTracingScene&) = delete;

        bool Initialize(
            const VulkanContext& context,
            const VulkanMemoryManager& memory,
            uint32_t framesInFlight);

        void Shutdown() noexcept;

        // Must be called after the corresponding frame slot's in-flight fence has been waited on.
        void OnFrameFenceCompleted(uint32_t frameIndex);

        // Must be called when the device is confirmed idle (e.g. after swapchain recreation).
        void OnDeviceIdle();

        // Records any required BLAS builds and the frame slot's TLAS build/update into cmd.
        // The caller must have already waited on frameIndex's in-flight fence.
        bool RecordFrame(
            VkCommandBuffer cmd,
            uint32_t frameIndex,
            std::span<const RtSceneItem> items);

        // Must be called after vkQueueSubmit2 succeeds for frameIndex.
        void OnFrameSubmitted(uint32_t frameIndex);

        bool IsInitialized() const noexcept { return m_initialized; }
        uint32_t GetFramesInFlight() const noexcept { return static_cast<uint32_t>(m_frames.size()); }

        bool HasValidTlas(uint32_t frameIndex) const noexcept;
        VkAccelerationStructureKHR GetTlasHandle(uint32_t frameIndex) const noexcept;
        VkDeviceAddress GetTlasDeviceAddress(uint32_t frameIndex) const noexcept;
        uint32_t GetTlasInstanceCount(uint32_t frameIndex) const noexcept;
        TlasBuildAction GetLastRecordedTlasAction(uint32_t frameIndex) const noexcept;
        std::span<const RtInstanceMetadata> GetInstanceMetadata(uint32_t frameIndex) const noexcept;

        uint32_t GetActiveBlasCount() const noexcept { return static_cast<uint32_t>(m_blasCache.size()); }
        uint32_t GetDeferredBlasCount() const noexcept { return static_cast<uint32_t>(m_deferredRetirements.size()); }
        uint32_t GetFrameReferencedBlasCount(uint32_t frameIndex) const noexcept;
        VkDeviceAddress GetLastBlasScratchAddress(uint32_t frameIndex) const noexcept;
        VkDeviceAddress GetLastTlasScratchAddress(uint32_t frameIndex) const noexcept;
        VkDeviceSize GetMinScratchOffsetAlignment() const noexcept { return m_minScratchOffsetAlignment; }

        const RtSceneTimingStats& GetTimingStats() const noexcept { return m_timingStats; }

        // Consolidated scene geometry/instance buffers and alpha-mask texture table for Hybrid RT passes.
        VkBuffer GetVertexStorageBuffer() const noexcept;
        VkBuffer GetIndexStorageBuffer() const noexcept;
        uint64_t GetGeometryBufferGeneration() const noexcept { return m_geometryBufferGeneration; }
        VkBuffer GetInstanceDataBuffer(uint32_t frameIndex) const noexcept;
        uint64_t GetInstanceDataBufferGeneration(uint32_t frameIndex) const noexcept;
        std::span<const VkDescriptorImageInfo> GetAlphaMaskImageInfos(uint32_t frameIndex) const noexcept;
        uint64_t GetAlphaMaskTableGeneration(uint32_t frameIndex) const noexcept;

    private:
        friend class AetherApplication;

        struct FaultInjectionOptions
        {
            bool failBlasStorageAllocation = false;
            bool failBlasAsCreate = false;
            bool failTlasStorageAllocation = false;
            bool failTlasAsCreate = false;
            bool failScratchAllocation = false;
        };

        struct BlasEntry
        {
            uint64_t meshUniqueId = 0;
            std::shared_ptr<VulkanAccelerationStructure> blas;
            uint32_t vertexCount = 0;
            uint32_t indexCount = 0;
            uint32_t firstVertex = 0;
            uint32_t firstIndex = 0;
            std::vector<RtGpuVertex> vertices;
            std::vector<uint32_t> indices;
            uint64_t builtInSubmitSerial = 0;
            uint64_t lastUsedSubmitSerial = 0;
        };

        struct DeferredBlasRetirement
        {
            std::shared_ptr<VulkanAccelerationStructure> blas;
            uint64_t retireAfterSerial = 0;
        };

        struct DeferredBufferRetirement
        {
            std::unique_ptr<VulkanBuffer> buffer;
            uint64_t retireAfterSerial = 0;
        };

        struct InstanceTopologyKey
        {
            uint64_t meshUniqueId = 0;
            uint64_t blasDeviceAddress = 0;
            uint32_t customIndex = 0;
            uint8_t mask = 0;
            VkGeometryInstanceFlagsKHR flags = 0;

            bool operator==(const InstanceTopologyKey&) const noexcept = default;
        };

        struct FrameResources
        {
            VulkanAccelerationStructure tlas;
            VulkanBuffer instanceBuffer;
            VulkanBuffer instanceDataBuffer;
            VulkanBuffer tlasScratchBuffer;
            VulkanBuffer blasScratchBuffer;
            uint32_t instanceCapacity = 0;
            uint32_t instanceCount = 0;
            bool hasValidTlas = false;
            bool builtWithAllowUpdate = false;
            VkDeviceSize tlasBuildSize = 0;
            VkDeviceSize tlasUpdateScratchSize = 0;
            std::vector<std::shared_ptr<VulkanAccelerationStructure>> referencedBlas;
            std::vector<InstanceTopologyKey> previousTopology;
            std::vector<VkTransformMatrixKHR> previousTransforms;
            std::vector<RtInstanceMetadata> instanceMetadata;
            std::vector<RtGpuInstanceData> instanceData;
            std::array<VkDescriptorImageInfo, kMaxRtAlphaMaskTextures> alphaMaskInfos{};
            std::array<VkDescriptorImageInfo, kMaxRtAlphaMaskTextures> prevAlphaMaskInfos{};
            uint64_t instanceDataGeneration = 0;
            uint64_t alphaMaskTableGeneration = 0;
            uint64_t submitSerial = 0;
            bool hasPendingTimestamps = false;
            bool recordedBlasBuild = false;
            bool recordedTlasOp = false;
            TlasBuildAction lastRecordedTlasAction = TlasBuildAction::None;
            uint32_t recordedBlasCount = 0;
            uint32_t recordedTlasInstanceCount = 0;
            VkDeviceAddress lastBlasScratchAddress = 0;
            VkDeviceAddress lastTlasScratchAddress = 0;
        };

        bool EnsureBufferCapacity(
            VulkanBuffer& buffer,
            VkDeviceSize requiredSize,
            VkBufferUsageFlags usage,
            VkMemoryPropertyFlags requiredMem,
            VkMemoryPropertyFlags preferredMem,
            bool injectFailure) const;

        void CollectDeferredRetirements() noexcept;
        void ReadFrameTimestamps(uint32_t frameIndex);
        bool SyncSceneDataBuffers(
            VkCommandBuffer cmd,
            uint32_t frameIndex,
            bool geometrySetChanged,
            std::span<const RtSceneItem* const> activeInstances);

        VkPhysicalDevice m_physicalDevice = VK_NULL_HANDLE;
        VkDevice m_device = VK_NULL_HANDLE;
        const VulkanMemoryManager* m_memory = nullptr;
        VulkanRayTracingCapabilities m_capabilities{};
        VkDeviceSize m_minScratchOffsetAlignment = 1;

        std::unordered_map<uint64_t, BlasEntry> m_blasCache;
        std::vector<DeferredBlasRetirement> m_deferredRetirements;
        std::vector<DeferredBufferRetirement> m_deferredBufferRetirements;
        std::vector<std::unique_ptr<FrameResources>> m_frames;

        std::unique_ptr<VulkanBuffer> m_vertexStorageBuffer;
        std::unique_ptr<VulkanBuffer> m_indexStorageBuffer;
        uint64_t m_geometryBufferGeneration = 0;

        VulkanImage m_whiteImage;
        VkSampler m_whiteSampler = VK_NULL_HANDLE;
        bool m_whiteImageReady = false;

        VkQueryPool m_timestampPool = VK_NULL_HANDLE;
        double m_timestampPeriodNs = 0.0;
        RtSceneTimingStats m_timingStats{};

        uint64_t m_submittedSerial = 0;
        uint64_t m_completedSerial = 0;

        FaultInjectionOptions m_faultInjection{};
        bool m_initialized = false;
    };
}
