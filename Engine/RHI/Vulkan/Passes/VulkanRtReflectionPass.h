#pragma once

#include <Renderer/RtLighting.h>
#include <RHI/Vulkan/Resources/VulkanBuffer.h>
#include <RHI/Vulkan/Resources/VulkanImage.h>

#include <glm/vec3.hpp>
#include <vulkan/vulkan.h>

#include <array>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>

namespace Aether
{
    class VulkanContext;
    class VulkanMemoryManager;
    class VulkanHybridGBufferPass;
    class VulkanRayTracingScene;

    class VulkanRtReflectionPass
    {
    public:
        VulkanRtReflectionPass() = default;
        ~VulkanRtReflectionPass();

        VulkanRtReflectionPass(const VulkanRtReflectionPass&) = delete;
        VulkanRtReflectionPass& operator=(const VulkanRtReflectionPass&) = delete;

        bool Initialize(
            const VulkanContext& context,
            const VulkanMemoryManager& memory,
            uint32_t framesInFlight,
            VkExtent2D extent);

        bool OnResize(VkExtent2D newExtent);
        void Shutdown() noexcept;

        void OnFrameFenceCompleted(uint32_t frameIndex);

        bool Execute(
            VkCommandBuffer cmd,
            uint32_t frameIndex,
            const VulkanRayTracingScene& rtScene,
            bool tlasWrittenThisFrame,
            const VulkanHybridGBufferPass& gBufferPass,
            VkDescriptorSet environmentSet,
            const glm::vec3& cameraPosition,
            const glm::vec3& sunDirectionToLight,
            const glm::vec3& sunRadiance,
            bool sunEnabled,
            bool fillEnabled,
            const HybridRtConfig& config,
            uint32_t globalFrameCounter,
            std::span<const RtGpuLight> lights,
            VkBuffer optionalReflectionReadbackBuffer = VK_NULL_HANDLE,
            VkBuffer optionalAuxReadbackBuffer = VK_NULL_HANDLE);

        void OnFrameSubmitted(uint32_t frameIndex);

        bool IsInitialized() const noexcept { return m_initialized; }
        VkExtent2D GetExtent() const noexcept { return m_extent; }
        VkImageView GetReflectionView(uint32_t frameIndex) const noexcept;
        VkImageView GetReflectionAuxView(uint32_t frameIndex) const noexcept;
        double GetLastGpuTimeMs() const noexcept { return m_lastGpuTimeMs; }

    private:
        struct ReflectionParamsUbo
        {
            glm::vec4 cameraPosAndHasTlas{ 0.0f };
            glm::vec4 sunDirAndMaxDist{ 0.0f };
            glm::vec4 sunColorAndEnabled{ 0.0f };
            glm::vec4 roughnessParams{ 0.05f, 0.72f, 0.004f, 0.0015f };
            uint32_t  countsAndExtent[4]{ 0, 0, 0, 1 };
            uint32_t  lightFlags[4]{ 1, 0, 1, 1 };
            RtGpuLight lights[kMaxRtSceneLights]{};
        };
        static_assert(sizeof(ReflectionParamsUbo) == 736);

        struct FrameResources
        {
            VulkanImage reflectionImage;
            VulkanImage reflectionAuxImage;
            VulkanBuffer paramsBuffer;
            VkDescriptorSet descriptorSet = VK_NULL_HANDLE;
            VkAccelerationStructureKHR boundTlas = VK_NULL_HANDLE;
            uint64_t boundGeomGeneration = 0;
            uint64_t boundInstGeneration = 0;
            uint64_t boundMaskGeneration = 0;
            uint64_t sizeVersion = 0;
            bool recordedDispatch = false;
            bool hasPendingTimestamp = false;
        };

        bool CreateOutputImages(VkExtent2D extent);
        void RefreshDescriptorSet(
            FrameResources& frame,
            uint32_t frameIndex,
            const VulkanRayTracingScene& rtScene,
            const VulkanHybridGBufferPass& gBufferPass);

        VkPhysicalDevice m_physicalDevice = VK_NULL_HANDLE;
        VkDevice m_device = VK_NULL_HANDLE;
        const VulkanMemoryManager* m_memory = nullptr;
        VkExtent2D m_extent{};
        uint64_t m_sizeVersion = 1;

        VkDescriptorSetLayout m_descriptorSetLayout = VK_NULL_HANDLE;
        VkDescriptorSetLayout m_environmentSetLayout = VK_NULL_HANDLE;
        VkDescriptorPool m_descriptorPool = VK_NULL_HANDLE;
        VkPipelineLayout m_pipelineLayout = VK_NULL_HANDLE;
        VkPipeline m_pipeline = VK_NULL_HANDLE;

        std::vector<std::unique_ptr<FrameResources>> m_frames;
        VkQueryPool m_timestampPool = VK_NULL_HANDLE;
        double m_timestampPeriodNs = 0.0;
        double m_lastGpuTimeMs = 0.0;
        bool m_initialized = false;
    };
}
