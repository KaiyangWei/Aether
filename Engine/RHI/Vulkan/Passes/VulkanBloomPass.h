#pragma once

#include <Renderer/Passes/BloomPass.h>
#include <RHI/Vulkan/Pipeline/VulkanGraphicsPipeline.h>
#include <RHI/Vulkan/Resources/VulkanImage.h>

#include <vulkan/vulkan.h>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>

namespace Aether
{
    class VulkanContext;
    class VulkanMemoryManager;

    enum class BloomInputSource : uint8_t
    {
        HdrScene = 0,
        TaaResolved = 1,
        HybridComposite = 2
    };

    class VulkanBloomPass
    {
    public:
        VulkanBloomPass() = default;
        ~VulkanBloomPass();

        VulkanBloomPass(const VulkanBloomPass&) = delete;
        VulkanBloomPass& operator=(const VulkanBloomPass&) = delete;

        bool Initialize(
            const VulkanContext& context,
            const VulkanMemoryManager& memory,
            uint32_t framesInFlight,
            VkExtent2D sceneExtent,
            VkImageView hdrSceneView,
            VkDescriptorSetLayout toneMapInputLayout,
            std::span<const VkImageView> taaResolvedViews = {},
            std::span<const VkImageView> hybridCompositeViews = {}
        );

        // Recreates size-dependent pyramid, reconstructed, and composite targets after GPU idle.
        bool OnResize(
            VkExtent2D sceneExtent,
            VkImageView hdrSceneView,
            bool injectFailure = false,
            std::span<const VkImageView> taaResolvedViews = {},
            std::span<const VkImageView> hybridCompositeViews = {}
        );

        void Shutdown() noexcept;

        // Executes BrightPass -> Downsample -> Separable Gaussian Blur -> Upsample -> HDR Composite.
        // Caller must ensure the HDR scene / TAA / Hybrid composite image is in VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL.
        void Execute(
            VkCommandBuffer cmd,
            uint32_t frameIndex,
            const BloomConfig& config,
            bool useTaaInput = false
        );

        void Execute(
            VkCommandBuffer cmd,
            uint32_t frameIndex,
            const BloomConfig& config,
            BloomInputSource inputSource
        );

        VkDescriptorSet GetOutputDescriptorSet(uint32_t frameIndex) const
        {
            return m_frames.at(frameIndex)->outputToneMapSet;
        }
        const VulkanImage& GetCompositeImage(uint32_t frameIndex) const
        {
            return m_frames.at(frameIndex)->composite;
        }
        const VulkanImage& GetReconstructedImage(uint32_t frameIndex) const
        {
            return m_frames.at(frameIndex)->reconstructed;
        }
        const VulkanImage& GetLevelImage(uint32_t frameIndex, uint32_t level) const
        {
            return m_frames.at(frameIndex)->levels.at(level)->mainImage;
        }
        uint32_t GetLevelCount() const
        {
            return m_frames.empty() ? 0u : static_cast<uint32_t>(m_frames.front()->levels.size());
        }
        VkExtent2D GetSceneExtent() const { return m_sceneExtent; }
        double GetLastGpuTimeMs() const { return m_lastGpuTimeMs; }
        bool IsInitialized() const { return m_device != VK_NULL_HANDLE; }

    private:
        struct LevelTarget
        {
            VulkanImage mainImage;
            VulkanImage tempImage;
            VkExtent2D extent{};
            VkDescriptorSet mainSampleSet = VK_NULL_HANDLE;
            VkDescriptorSet tempSampleSet = VK_NULL_HANDLE;
            VkDescriptorSet upsampleFromMainSet = VK_NULL_HANDLE;
            VkDescriptorSet upsampleFromTempSet = VK_NULL_HANDLE;
            VkDescriptorSet debugLevelSet = VK_NULL_HANDLE;
        };

        struct FrameResources
        {
            std::vector<std::unique_ptr<LevelTarget>> levels;
            VulkanImage reconstructed;
            VulkanImage composite;
            VkDescriptorSet brightInputSet = VK_NULL_HANDLE;
            VkDescriptorSet compositeSet = VK_NULL_HANDLE;
            VkDescriptorSet taaBrightInputSet = VK_NULL_HANDLE;
            VkDescriptorSet taaCompositeSet = VK_NULL_HANDLE;
            VkDescriptorSet hybridBrightInputSet = VK_NULL_HANDLE;
            VkDescriptorSet hybridCompositeSet = VK_NULL_HANDLE;
            VkDescriptorSet debugBrightSet = VK_NULL_HANDLE;
            VkDescriptorSet outputToneMapSet = VK_NULL_HANDLE;
            bool hasTimestamp = false;
        };

        bool CreatePipelines();
        bool BuildFrameResources(
            VkExtent2D sceneExtent,
            VkImageView hdrSceneView,
            bool injectFailure,
            std::span<const VkImageView> taaResolvedViews,
            std::span<const VkImageView> hybridCompositeViews
        );
        void ReleaseFrameResources() noexcept;

        static void TransitionColorImage(
            VkCommandBuffer cmd,
            VkImage image,
            VkImageLayout oldLayout,
            VkImageLayout newLayout,
            VkPipelineStageFlags2 srcStage,
            VkAccessFlags2 srcAccess,
            VkPipelineStageFlags2 dstStage,
            VkAccessFlags2 dstAccess
        );

        static void DrawFullscreenPass(
            VkCommandBuffer cmd,
            const VulkanGraphicsPipeline& pipeline,
            VkDescriptorSet set,
            VkImageView targetView,
            VkExtent2D targetExtent,
            const void* pushData,
            uint32_t pushSize
        );

        static constexpr VkFormat kHdrFormat = VK_FORMAT_R16G16B16A16_SFLOAT;
        static constexpr VkShaderStageFlags kPushStages =
            VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;

        VkPhysicalDevice m_physicalDevice = VK_NULL_HANDLE;
        VkDevice m_device = VK_NULL_HANDLE;
        const VulkanMemoryManager* m_memory = nullptr;
        uint32_t m_framesInFlight = 0;
        uint32_t m_maxImageDimension = 16384;
        VkExtent2D m_sceneExtent{};

        VkSampler m_linearSampler = VK_NULL_HANDLE;
        VkDescriptorSetLayout m_passSetLayout = VK_NULL_HANDLE;
        VkDescriptorSetLayout m_toneMapInputLayout = VK_NULL_HANDLE;
        VkDescriptorPool m_descriptorPool = VK_NULL_HANDLE;
        std::vector<std::unique_ptr<FrameResources>> m_frames;

        VulkanGraphicsPipeline m_brightPipeline;
        VulkanGraphicsPipeline m_downsamplePipeline;
        VulkanGraphicsPipeline m_blurPipeline;
        VulkanGraphicsPipeline m_upsamplePipeline;
        VulkanGraphicsPipeline m_compositePipeline;

        VkQueryPool m_timestampPool = VK_NULL_HANDLE;
        float m_timestampPeriodNs = 0.0f;
        double m_lastGpuTimeMs = 0.0;
    };
}
