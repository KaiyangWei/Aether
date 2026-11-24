#pragma once

#include <RHI/Vulkan/Pipeline/VulkanGraphicsPipeline.h>
#include <RHI/Vulkan/Resources/VulkanImage.h>

#include <glm/mat4x4.hpp>
#include <glm/vec3.hpp>
#include <glm/vec4.hpp>
#include <vulkan/vulkan.h>

#include <cstdint>
#include <memory>
#include <span>
#include <vector>

namespace Aether
{
    class VulkanContext;
    class VulkanMemoryManager;
    class VulkanMesh;

    struct HybridGBufferDrawItem
    {
        const VulkanMesh* mesh = nullptr;
        glm::mat4 mvp{ 1.0f };
        glm::vec4 modelRows[3]{
            { 1.0f, 0.0f, 0.0f, 0.0f },
            { 0.0f, 1.0f, 0.0f, 0.0f },
            { 0.0f, 0.0f, 1.0f, 0.0f }
        };
        glm::vec3 cameraPosition{ 0.0f };
        uint32_t rtInstanceIndex = UINT32_MAX;
        bool mirrored = false;
        bool doubleSided = false;
        bool alphaBlend = false;
        VkDescriptorSet material = VK_NULL_HANDLE;
    };

    class VulkanHybridGBufferPass
    {
    public:
        VulkanHybridGBufferPass() = default;
        ~VulkanHybridGBufferPass();

        VulkanHybridGBufferPass(const VulkanHybridGBufferPass&) = delete;
        VulkanHybridGBufferPass& operator=(const VulkanHybridGBufferPass&) = delete;

        bool Initialize(
            const VulkanContext& context,
            const VulkanMemoryManager& memory,
            uint32_t framesInFlight,
            VkExtent2D extent,
            VkFormat depthFormat);

        bool OnResize(VkExtent2D extent, VkFormat depthFormat);
        void Shutdown() noexcept;

        void OnFrameFenceCompleted(uint32_t frameIndex);

        // Records Synchronization2 transitions and 4-MRT dynamic rendering pass into cmd.
        // Caller must pass depthImage in VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL.
        // Postcondition: current frame's RT0..RT3 are in VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL.
        bool Execute(
            VkCommandBuffer cmd,
            uint32_t frameIndex,
            VkImageView depthView,
            VkImageLayout depthLayout,
            std::span<const HybridGBufferDrawItem> items);

        void OnFrameSubmitted(uint32_t frameIndex);

        bool IsInitialized() const noexcept { return m_initialized; }
        VkExtent2D GetExtent() const noexcept { return m_extent; }
        VkImageView GetRt0View(uint32_t frameIndex) const noexcept;
        VkImageView GetRt1View(uint32_t frameIndex) const noexcept;
        VkImageView GetRt2View(uint32_t frameIndex) const noexcept;
        VkImageView GetRt3View(uint32_t frameIndex) const noexcept;
        VkImage GetRt1Image(uint32_t frameIndex) const noexcept;
        VkImage GetRt3Image(uint32_t frameIndex) const noexcept;
        double GetLastGpuTimeMs() const noexcept { return m_lastGpuTimeMs; }

    private:
        struct PushConstants
        {
            glm::mat4 mvp;
            glm::vec4 modelRows[3];
            glm::vec4 eyeAndInstanceId;
        };
        static_assert(sizeof(PushConstants) == 128);

        struct FrameResources
        {
            VulkanImage rt0AlbedoMetallic;
            VulkanImage rt1NormalRoughness;
            VulkanImage rt2EmissiveAo;
            VulkanImage rt3WorldPosInst;
            bool layoutsInitialized = false;
            bool recordedPass = false;
            bool hasPendingTimestamp = false;
        };

        bool CreatePipelines(VkFormat depthFormat);
        bool BuildSizeDependentResources(VkExtent2D extent);

        VkPhysicalDevice m_physicalDevice = VK_NULL_HANDLE;
        VkDevice m_device = VK_NULL_HANDLE;
        const VulkanMemoryManager* m_memory = nullptr;
        VkExtent2D m_extent{};
        VkFormat m_depthFormat = VK_FORMAT_UNDEFINED;

        VkDescriptorSetLayout m_materialLayout = VK_NULL_HANDLE;
        VulkanGraphicsPipeline m_pbrPipeline;
        VulkanGraphicsPipeline m_vertexColorPipeline;

        std::vector<std::unique_ptr<FrameResources>> m_frames;
        VkQueryPool m_timestampPool = VK_NULL_HANDLE;
        double m_timestampPeriodNs = 0.0;
        double m_lastGpuTimeMs = 0.0;
        bool m_initialized = false;
    };
}
