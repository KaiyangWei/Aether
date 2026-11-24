#pragma once

#include <Renderer/RayTracingConfig.h>
#include <RHI/Vulkan/Resources/VulkanBuffer.h>
#include <RHI/Vulkan/Resources/VulkanImage.h>

#include <glm/mat4x4.hpp>
#include <glm/vec3.hpp>
#include <vulkan/vulkan.h>

#include <cstdint>
#include <memory>
#include <vector>

namespace Aether
{
    class VulkanContext;
    class VulkanMemoryManager;

    class VulkanRayQueryPass
    {
    public:
        VulkanRayQueryPass() = default;
        ~VulkanRayQueryPass();

        VulkanRayQueryPass(const VulkanRayQueryPass&) = delete;
        VulkanRayQueryPass& operator=(const VulkanRayQueryPass&) = delete;

        bool Initialize(
            const VulkanContext& context,
            const VulkanMemoryManager& memory,
            uint32_t framesInFlight,
            VkExtent2D extent,
            VkDescriptorSetLayout toneMapSetLayout);

        bool OnResize(VkExtent2D newExtent);
        void Shutdown() noexcept;

        // Must be called after the frame slot's in-flight fence has been waited on.
        void OnFrameFenceCompleted(uint32_t frameIndex);

        // Records Synchronization2 barriers, Ray Query compute dispatch, optional FP16 readback copy,
        // and final transition to SHADER_READ_ONLY_OPTIMAL for ToneMap sampling.
        bool Execute(
            VkCommandBuffer cmd,
            uint32_t frameIndex,
            VkAccelerationStructureKHR tlasHandle,
            bool tlasUpdatedThisFrame,
            VkDescriptorSet environmentSet,
            const glm::mat4& clipToWorldDirection,
            const glm::vec3& cameraPosition,
            const RayQueryConfig& config,
            VkBuffer optionalReadbackBuffer = VK_NULL_HANDLE);

        void OnFrameSubmitted(uint32_t frameIndex);

        bool IsInitialized() const noexcept { return m_initialized; }
        VkExtent2D GetExtent() const noexcept { return m_extent; }
        VkDescriptorSet GetOutputToneMapSet(uint32_t frameIndex) const noexcept;
        double GetLastGpuTimeMs() const noexcept { return m_lastGpuTimeMs; }
        bool AreTimestampsSupported() const noexcept { return m_timestampPool != VK_NULL_HANDLE; }

    private:
        struct RayQueryUniforms
        {
            glm::mat4 clipToWorldDirection{ 1.0f };
            glm::vec4 cameraOriginAndTMin{ 0.0f };
            glm::vec4 rayLimitsAndSize{ 0.0f };
            uint32_t modeAndFlags[4]{ 0, 0, 0, 0 };
        };
        static_assert(sizeof(RayQueryUniforms) == 112);

        struct FrameResources
        {
            VulkanImage outputImage;
            VkImageLayout outputLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            VulkanBuffer paramsBuffer;
            VkDescriptorSet computeSet = VK_NULL_HANDLE;
            VkDescriptorSet toneMapSet = VK_NULL_HANDLE;
            VkAccelerationStructureKHR boundTlas = VK_NULL_HANDLE;
            bool recordedDispatch = false;
            bool hasPendingTimestamps = false;
        };

        bool CreateOutputImages(VkExtent2D extent);
        void UpdateFrameStaticDescriptors(FrameResources& frame) const;

        VkPhysicalDevice m_physicalDevice = VK_NULL_HANDLE;
        VkDevice m_device = VK_NULL_HANDLE;
        const VulkanMemoryManager* m_memory = nullptr;
        VkExtent2D m_extent{};

        VkDescriptorSetLayout m_computeSetLayout = VK_NULL_HANDLE;
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
