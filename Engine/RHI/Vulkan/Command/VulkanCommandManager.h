#pragma once

#include <vulkan/vulkan.h>

#include <vector>
namespace Aether
{
	class VulkanCommandManager
	{
	public:
		VulkanCommandManager() = default;
		~VulkanCommandManager();

		VulkanCommandManager(const VulkanCommandManager&) = delete;
		VulkanCommandManager& operator=(const VulkanCommandManager&) = delete;
		bool initialize(
			VkDevice device,
			uint32_t graphicsQueueFamily,
			uint32_t commandBufferCount
		);

		VkCommandBuffer GetCommandBuffer(uint32_t index) const
		{
			return m_commandBuffers.at(index);
		}

		void Shutdown();

	private:
		VkDevice m_device = VK_NULL_HANDLE;

		VkCommandPool m_commandPool = VK_NULL_HANDLE;

		std::vector<VkCommandBuffer> m_commandBuffers;
	};
}