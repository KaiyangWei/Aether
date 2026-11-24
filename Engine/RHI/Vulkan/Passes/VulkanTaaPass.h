#pragma once

#include <Renderer/Passes/TaaPass.h>
#include <RHI/Vulkan/Pipeline/VulkanGraphicsPipeline.h>
#include <RHI/Vulkan/Resources/VulkanBuffer.h>
#include <RHI/Vulkan/Resources/VulkanImage.h>

#include <glm/mat4x4.hpp>
#include <glm/vec2.hpp>
#include <glm/vec4.hpp>
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
    class VulkanMesh;

    struct VelocityDrawItem
    {
        const VulkanMesh* mesh = nullptr;
        glm::mat4 currMvpJittered{ 1.0f };
        glm::mat4 prevMvpJittered{ 0.0f };
        bool mirrored = false;
        bool doubleSided = false;
        bool alphaMask = false;
        bool alphaBlend = false;
        VkDescriptorSet material = VK_NULL_HANDLE;
    };

    class VulkanTaaPass
    {
    public:
        VulkanTaaPass() = default;
        ~VulkanTaaPass();

        VulkanTaaPass(const VulkanTaaPass&) = delete;
        VulkanTaaPass& operator=(const VulkanTaaPass&) = delete;

        static VkFormat SelectVelocityFormat(VkPhysicalDevice physicalDevice);

        bool Initialize(
            const VulkanContext& context,
            const VulkanMemoryManager& memory,
            uint32_t framesInFlight,
            VkExtent2D extent,
            VkFormat depthFormat,
            VkImageView currentHdrView,
            VkImageView currentDepthView,
            VkDescriptorSetLayout toneMapInputLayout
        );

        bool OnResize(
            VkExtent2D extent,
            VkFormat depthFormat,
            VkImageView currentHdrView,
            VkImageView currentDepthView,
            bool injectFailure = false
        );

        void ResetHistory();
        void Shutdown() noexcept;

        // Executes Motion Vector Pass -> TAA Temporal Resolve -> Depth History Copy.
        // Preconditions:
        // - currentHdr is in VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
        // - currentDepth is in VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL (with stored scene depth)
        // Postconditions:
        // - m_history[frameIndex] contains resolved linear HDR in VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
        // - currentDepth is transitioned back to VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL
        // Commit the history only after Execute's command buffer has been submitted successfully.
        bool Execute(
            VkCommandBuffer cmd,
            uint32_t frameIndex,
            VkImage currentDepthImage,
            std::span<const VelocityDrawItem> items,
            const glm::mat4& currClipToPrevClipRot,
            const glm::vec2& jitterDeltaUv,
            const TaaConfig& config
        );
        void OnFrameSubmitted(uint32_t frameIndex);

        VkImageView GetHistoryView(uint32_t frameIndex) const { return m_history.at(frameIndex)->GetView(); }
        const VulkanImage& GetHistoryImage(uint32_t frameIndex) const { return *m_history.at(frameIndex); }
        const VulkanImage& GetVelocityImage() const { return *m_velocity; }
        VkDescriptorSet GetOutputToneMapSet(uint32_t frameIndex) const { return m_outputToneMapSets.at(frameIndex); }
        VkFormat GetVelocityFormat() const { return m_velocityFormat; }
        VkExtent2D GetExtent() const { return m_extent; }
        bool IsHistoryValid() const { return m_lastHistorySlot != UINT32_MAX; }
        double GetLastVelocityGpuTimeMs() const { return m_lastVelocityGpuMs; }
        double GetLastResolveGpuTimeMs() const { return m_lastResolveGpuMs; }
        double GetLastTotalGpuTimeMs() const { return m_lastVelocityGpuMs + m_lastResolveGpuMs; }
        bool IsInitialized() const { return m_device != VK_NULL_HANDLE; }

    private:
        struct FrameParams
        {
            glm::vec4 jitterAndInvExtent{ 0.0f };
            glm::vec4 weights{ 0.0f };
            glm::vec4 thresholds{ 0.0f };
        };
        static_assert(sizeof(FrameParams) == 48);

        struct VelocityPush
        {
            glm::mat4 currMvpJittered;
            glm::mat4 prevMvpJittered;
        };
        static_assert(sizeof(VelocityPush) == 128);

        bool CreatePipelines(VkFormat depthFormat);
        bool BuildSizeDependentResources(
            VkExtent2D extent,
            VkFormat depthFormat,
            VkImageView currentHdrView,
            VkImageView currentDepthView,
            bool injectFailure
        );
        void ReleaseSizeDependentResources() noexcept;

        static void TransitionImage(
            VkCommandBuffer cmd,
            VkImage image,
            VkImageAspectFlags aspect,
            VkImageLayout oldLayout,
            VkImageLayout newLayout,
            VkPipelineStageFlags2 srcStage,
            VkAccessFlags2 srcAccess,
            VkPipelineStageFlags2 dstStage,
            VkAccessFlags2 dstAccess
        );

        static constexpr VkFormat kHdrFormat = VK_FORMAT_R16G16B16A16_SFLOAT;
        static constexpr VkShaderStageFlags kVelPushStages = VK_SHADER_STAGE_VERTEX_BIT;
        static constexpr VkShaderStageFlags kSkyPushStages =
            VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;

        VkPhysicalDevice m_physicalDevice = VK_NULL_HANDLE;
        VkDevice m_device = VK_NULL_HANDLE;
        const VulkanMemoryManager* m_memory = nullptr;
        uint32_t m_framesInFlight = 0;
        VkExtent2D m_extent{};
        VkFormat m_depthFormat = VK_FORMAT_UNDEFINED;
        VkFormat m_velocityFormat = VK_FORMAT_UNDEFINED;

        VkSampler m_linearSampler = VK_NULL_HANDLE;
        VkSampler m_pointSampler = VK_NULL_HANDLE;

        VkDescriptorSetLayout m_resolveLayout = VK_NULL_HANDLE;
        VkDescriptorSetLayout m_toneMapInputLayout = VK_NULL_HANDLE;
        VkDescriptorPool m_descriptorPool = VK_NULL_HANDLE;

        std::unique_ptr<VulkanImage> m_velocity;
        VkImageLayout m_velocityLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        std::unique_ptr<VulkanImage> m_prevDepth;
        VkImageLayout m_prevDepthLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        VkImageView m_currentDepthView = VK_NULL_HANDLE;

        std::vector<std::unique_ptr<VulkanImage>> m_history;
        std::vector<VkImageLayout> m_historyLayouts;
        std::vector<std::unique_ptr<VulkanBuffer>> m_paramBuffers;
        std::vector<VkDescriptorSet> m_resolveSets;
        std::vector<VkDescriptorSet> m_outputToneMapSets;
        std::vector<bool> m_frameHasTimestamp;

        VulkanGraphicsPipeline m_skyVelocityPipeline;
        VulkanGraphicsPipeline m_opaqueVelocityPipeline;
        VulkanGraphicsPipeline m_maskVelocityPipeline;
        VulkanGraphicsPipeline m_resolvePipeline;

        uint32_t m_lastHistorySlot = UINT32_MAX;
        VkQueryPool m_timestampPool = VK_NULL_HANDLE;
        float m_timestampPeriodNs = 0.0f;
        double m_lastVelocityGpuMs = 0.0;
        double m_lastResolveGpuMs = 0.0;
    };
}
