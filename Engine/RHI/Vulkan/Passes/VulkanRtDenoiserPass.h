#pragma once

#include <Renderer/RtLighting.h>
#include <RHI/Vulkan/Resources/VulkanBuffer.h>
#include <RHI/Vulkan/Resources/VulkanImage.h>

#include <glm/mat4x4.hpp>
#include <glm/vec3.hpp>
#include <glm/vec4.hpp>
#include <vulkan/vulkan.h>

#include <array>
#include <cstdint>
#include <memory>
#include <vector>

namespace Aether
{
    class VulkanContext;
    class VulkanMemoryManager;
    class VulkanHybridGBufferPass;
    class VulkanRtShadowPass;
    class VulkanRtReflectionPass;
    class VulkanRayTracingScene;

    class VulkanRtDenoiserPass
    {
    public:
        VulkanRtDenoiserPass() = default;
        ~VulkanRtDenoiserPass();

        VulkanRtDenoiserPass(const VulkanRtDenoiserPass&) = delete;
        VulkanRtDenoiserPass& operator=(const VulkanRtDenoiserPass&) = delete;

        bool Initialize(
            const VulkanContext& context,
            const VulkanMemoryManager& memory,
            uint32_t framesInFlight,
            VkExtent2D extent,
            VkDescriptorSetLayout toneMapSetLayout);

        bool OnResize(VkExtent2D newExtent);
        void ResetHistory() noexcept;
        void Shutdown() noexcept;

        void OnFrameFenceCompleted(uint32_t frameIndex);

        bool Execute(
            VkCommandBuffer cmd,
            uint32_t frameIndex,
            const VulkanRayTracingScene& rtScene,
            const VulkanHybridGBufferPass& gBufferPass,
            const VulkanRtShadowPass& rtShadowPass,
            const VulkanRtReflectionPass& rtReflectionPass,
            VkDescriptorSet environmentSet,
            const glm::mat4& currClipToWorldDirection,
            const glm::mat4& prevUnjitteredViewProj,
            const glm::vec3& cameraPosition,
            const HybridRtConfig& config,
            VkBuffer optionalVarConfReadbackBuffer = VK_NULL_HANDLE,
            VkBuffer optionalCompositeReadbackBuffer = VK_NULL_HANDLE);

        void OnFrameSubmitted(uint32_t frameIndex);

        bool IsInitialized() const noexcept { return m_initialized; }
        bool IsHistoryValid() const noexcept { return m_lastHistorySlot != UINT32_MAX; }
        VkExtent2D GetExtent() const noexcept { return m_extent; }
        VkImageView GetCompositeView(uint32_t frameIndex) const noexcept;
        VkDescriptorSet GetOutputToneMapSet(uint32_t frameIndex) const noexcept;
        VkImageView GetVarConfView(uint32_t frameIndex) const noexcept;

        double GetLastTemporalGpuTimeMs() const noexcept { return m_lastTemporalGpuTimeMs; }
        double GetLastAtrousGpuTimeMs() const noexcept { return m_lastAtrousGpuTimeMs; }
        double GetLastCompositeGpuTimeMs() const noexcept { return m_lastCompositeGpuTimeMs; }
        double GetLastTotalGpuTimeMs() const noexcept { return m_lastTemporalGpuTimeMs + m_lastAtrousGpuTimeMs + m_lastCompositeGpuTimeMs; }

    private:
        struct TemporalParamsUbo
        {
            glm::mat4 prevViewProj{ 1.0f };
            glm::vec4 cameraPosAndHistoryValid{ 0.0f };
            glm::vec4 alphaAndThresholds{ 0.06f, 0.25f, 0.05f, 0.90f };
            uint32_t  extentAndFlags[4]{ 0, 0, 1, 4 };
        };
        static_assert(sizeof(TemporalParamsUbo) == 112);

        struct AtrousPushConstants
        {
            uint32_t width = 0;
            uint32_t height = 0;
            int32_t  stepSize = 1;
            uint32_t iterIndex = 0;
            float    minMirrorRoughness = 0.05f;
            float    sigmaLuma = 2.0f;
            float    sigmaNormal = 64.0f;
            float    sigmaDepth = 0.03f;
        };
        static_assert(sizeof(AtrousPushConstants) == 32);

        struct CompositeParamsUbo
        {
            glm::mat4 clipToWorldDirection{ 1.0f };
            glm::vec4 cameraPosAndMaxRough{ 0.0f };
            uint32_t  extentAndDebug[4]{ 0, 0, 0, 1 };
            uint32_t  flags[4]{ 1, 1, 1, 0 };
        };
        static_assert(sizeof(CompositeParamsUbo) == 112);

        struct FrameResources
        {
            VulkanImage shadowHistory;
            VulkanImage reflHistory;
            VulkanImage momentsHistory;
            VkImageLayout historyLayout = VK_IMAGE_LAYOUT_UNDEFINED;

            VulkanImage atrousShadow[2];
            VulkanImage atrousRefl[2];
            VulkanImage varConfImage;
            VulkanImage compositeImage;

            VulkanBuffer temporalParamsBuffer;
            VulkanBuffer compositeParamsBuffer;

            VkDescriptorSet temporalSet = VK_NULL_HANDLE;
            VkDescriptorSet atrousFromHistSet = VK_NULL_HANDLE;
            VkDescriptorSet atrous0To1Set = VK_NULL_HANDLE;
            VkDescriptorSet atrous1To0Set = VK_NULL_HANDLE;
            VkDescriptorSet compositeFromHistSet = VK_NULL_HANDLE;
            VkDescriptorSet compositeFromScratch0Set = VK_NULL_HANDLE;
            VkDescriptorSet compositeFromScratch1Set = VK_NULL_HANDLE;
            VkDescriptorSet toneMapSet = VK_NULL_HANDLE;

            uint32_t boundPrevSlot = UINT32_MAX;
            uint64_t boundInstGeneration = 0;
            uint64_t sizeVersion = 0;
            bool recordedPass = false;
            bool hasPendingTimestamps = false;
        };

        bool CreatePipelines();
        bool BuildSizeDependentResources(VkExtent2D extent);
        void UpdateStaticFrameDescriptors(FrameResources& frame, uint32_t frameIndex,
                                          const VulkanHybridGBufferPass& gBufferPass,
                                          const VulkanRtShadowPass& rtShadowPass,
                                          const VulkanRtReflectionPass& rtReflectionPass);
        void UpdateTemporalDynamicDescriptors(FrameResources& frame, uint32_t frameIndex, uint32_t prevSlot,
                                              const VulkanRayTracingScene& rtScene,
                                              const VulkanHybridGBufferPass& gBufferPass,
                                              const VulkanRtShadowPass& rtShadowPass,
                                              const VulkanRtReflectionPass& rtReflectionPass);

        VkPhysicalDevice m_physicalDevice = VK_NULL_HANDLE;
        VkDevice m_device = VK_NULL_HANDLE;
        const VulkanMemoryManager* m_memory = nullptr;
        VkExtent2D m_extent{};
        uint64_t m_sizeVersion = 1;

        VkDescriptorSetLayout m_toneMapLayout = VK_NULL_HANDLE;
        VkDescriptorSetLayout m_environmentLayout = VK_NULL_HANDLE;
        VkDescriptorSetLayout m_temporalLayout = VK_NULL_HANDLE;
        VkDescriptorSetLayout m_atrousLayout = VK_NULL_HANDLE;
        VkDescriptorSetLayout m_compositeLayout = VK_NULL_HANDLE;

        VkDescriptorPool m_descriptorPool = VK_NULL_HANDLE;

        VkPipelineLayout m_temporalPipelineLayout = VK_NULL_HANDLE;
        VkPipeline m_temporalPipeline = VK_NULL_HANDLE;
        VkPipelineLayout m_atrousPipelineLayout = VK_NULL_HANDLE;
        VkPipeline m_atrousPipeline = VK_NULL_HANDLE;
        VkPipelineLayout m_compositePipelineLayout = VK_NULL_HANDLE;
        VkPipeline m_compositePipeline = VK_NULL_HANDLE;

        std::vector<std::unique_ptr<FrameResources>> m_frames;
        uint32_t m_lastHistorySlot = UINT32_MAX;

        VkQueryPool m_timestampPool = VK_NULL_HANDLE;
        double m_timestampPeriodNs = 0.0;
        double m_lastTemporalGpuTimeMs = 0.0;
        double m_lastAtrousGpuTimeMs = 0.0;
        double m_lastCompositeGpuTimeMs = 0.0;
        bool m_initialized = false;
    };
}
