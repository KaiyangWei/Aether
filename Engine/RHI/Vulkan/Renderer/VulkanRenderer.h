#pragma once

#include <vulkan/vulkan.h>
#include <vector>
#include <cstdint>

#include <RHI/Vulkan/Pipeline/VulkanGraphicsPipeline.h>

namespace Aether
{
	class VulkanContext;
	class VulkanCommandManager;
	class VulkanSyncManager;

    class VulkanRenderer
    {
    public:
        VulkanRenderer() = default;
        ~VulkanRenderer();

        VulkanRenderer(const VulkanRenderer&) = delete;
        VulkanRenderer& operator=(const VulkanRenderer&) = delete;

        bool Initialize(
            VulkanContext& context,
            VulkanCommandManager& commandManager,
            VulkanSyncManager& syncManager
        );

        bool DrawFrame();

        void Shutdown();

    private:
        static constexpr uint32_t MAX_FRAMES_IN_FLIGHT = 2;

        VulkanContext* m_context = nullptr;
        VulkanCommandManager* m_commandManager = nullptr;
        VulkanSyncManager* m_syncManager = nullptr;

        uint32_t m_currentFrame = 0;

        std::vector<VkImageLayout> m_imageLayouts;

        std::vector<VkFence> m_imagesInFlight;

        VulkanGraphicsPipeline m_graphicsPipeline;
    };
}