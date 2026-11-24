#pragma once

#include <Renderer/DirectionalShadow.h>
#include <RHI/Vulkan/Pipeline/VulkanGraphicsPipeline.h>
#include <RHI/Vulkan/Resources/VulkanBuffer.h>
#include <RHI/Vulkan/Resources/VulkanShadowMap.h>

#include <glm/mat4x4.hpp>
#include <glm/vec4.hpp>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>

namespace Aether
{
    class VulkanContext;
    class VulkanMemoryManager;
    class VulkanMesh;

    struct ShadowCasterItem
    {
        const VulkanMesh* mesh = nullptr;
        glm::mat4 model{ 1.0f };
        bool mirrored = false;
        bool doubleSided = false;
        bool alphaMask = false;
        bool alphaBlend = false;
        VkDescriptorSet material = VK_NULL_HANDLE;
    };

    class ShadowPass
    {
    public:
        ShadowPass() = default;
        ~ShadowPass();

        ShadowPass(const ShadowPass&) = delete;
        ShadowPass& operator=(const ShadowPass&) = delete;

        static VkResult CreateDescriptorSetLayout(VkDevice device, VkDescriptorSetLayout& layout);

        bool Initialize(
            const VulkanContext& context,
            const VulkanMemoryManager& memory,
            uint32_t framesInFlight,
            VkFormat hdrColorFormat,
            VkFormat hdrDepthFormat,
            uint32_t resolution = 2048
        );
        void Shutdown() noexcept;

        // Rebuilds per-frame shadow maps at a new resolution. Caller must ensure GPU is idle.
        bool SetResolution(uint32_t resolution);

        // Executes depth-only shadow rendering and transitions the shadow map to SHADER_READ_ONLY_OPTIMAL.
        void Execute(
            VkCommandBuffer cmd,
            uint32_t frameIndex,
            std::span<const ShadowCasterItem> casters,
            const ShadowLightSpace& lightSpace,
            const ShadowConfig& config
        );

        // Renders the raw shadow depth map into the currently active HDR color rendering pass.
        void DrawDebugOverlay(VkCommandBuffer cmd, uint32_t frameIndex) const;

        VkDescriptorSetLayout GetDescriptorSetLayout() const { return m_descriptorSetLayout; }
        VkDescriptorSet GetDescriptorSet(uint32_t frameIndex) const { return m_frames.at(frameIndex)->descriptorSet; }
        VkFormat GetDepthFormat() const { return m_depthFormat; }
        uint32_t GetResolution() const { return m_resolution; }
        double GetLastGpuTimeMs() const { return m_lastGpuTimeMs; }
        bool IsInitialized() const { return m_device != VK_NULL_HANDLE; }

    private:
        struct ShadowUniforms
        {
            glm::mat4 lightViewProj{ 1.0f };
            glm::vec4 params0{ 0.0f }; // x: enabled, y: strength, z: receiverBias, w: texelSize
            glm::vec4 params1{ 0.0f }; // x: pcfRadiusTexels, y: normalBias, z: pcfEnabled, w: debugMode
        };
        static_assert(sizeof(ShadowUniforms) == 96);

        struct FrameResources
        {
            VulkanShadowMap shadowMap;
            VulkanBuffer uniformBuffer;
            VkDescriptorSet descriptorSet = VK_NULL_HANDLE;
            bool hasTimestamp = false;
        };

        bool CreatePipelines(VkFormat hdrColorFormat, VkFormat hdrDepthFormat);
        bool CreateFrameResources(uint32_t resolution);
        void UpdateFrameDescriptor(FrameResources& frame) const;

        VkPhysicalDevice m_physicalDevice = VK_NULL_HANDLE;
        VkDevice m_device = VK_NULL_HANDLE;
        const VulkanMemoryManager* m_memory = nullptr;
        uint32_t m_framesInFlight = 0;
        uint32_t m_resolution = 2048;
        VkFormat m_depthFormat = VK_FORMAT_UNDEFINED;

        VkDescriptorSetLayout m_descriptorSetLayout = VK_NULL_HANDLE;
        VkDescriptorPool m_descriptorPool = VK_NULL_HANDLE;
        std::vector<std::unique_ptr<FrameResources>> m_frames;

        VulkanGraphicsPipeline m_opaquePipeline;
        VulkanGraphicsPipeline m_maskPipeline;
        VulkanGraphicsPipeline m_debugPipeline;

        VkQueryPool m_timestampPool = VK_NULL_HANDLE;
        float m_timestampPeriodNs = 0.0f;
        double m_lastGpuTimeMs = 0.0;
    };
}
