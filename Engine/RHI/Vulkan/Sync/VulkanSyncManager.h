#pragma once

#include <vulkan/vulkan.h>
#include <vector>
#include <cstdint>


namespace Aether
{
	class VulkanSyncManager
	{
	public:
		VulkanSyncManager() = default;
		~VulkanSyncManager();

		VulkanSyncManager(const VulkanSyncManager&) = delete;
		VulkanSyncManager& operator=(const VulkanSyncManager&) = delete;

		bool Initialize(
			VkDevice device,
			uint32_t framesInFlight,
			uint32_t swapchainImageCount
		);

		void Shutdown();

		VkSemaphore GetImageAvailableSemaphore(uint32_t frameIndex) const;
		VkSemaphore GetRenderFinishedSemaphore(uint32_t imageIndex) const;
		VkFence GetInFlightFence(uint32_t frameIndex) const;


	private:
		VkDevice m_device = VK_NULL_HANDLE;

		std::vector<VkSemaphore> m_imageAvailableSemaphores;
		std::vector<VkFence> m_inFlightFences;

		std::vector<VkSemaphore> m_renderFinishedSemaphores;
	};
}