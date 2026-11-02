#include "VulkanSyncManager.h"

#include <iostream>



Aether::VulkanSyncManager::~VulkanSyncManager()
{
	Shutdown();
}

bool Aether::VulkanSyncManager::Initialize(
	VkDevice device,
	uint32_t framesInFlight,
	uint32_t swapchainImageCount
)
{
	if (device == VK_NULL_HANDLE ||
		framesInFlight == 0 ||
		swapchainImageCount == 0)
	{
		std::cerr << "Invalid Vulkan Sync Manager initialization parameters!\n";
		return false;
	}
	if (m_device != VK_NULL_HANDLE)
	{
		std::cerr << "Vulkan Sync Manager has already been initialized!\n";
		return false;
	}
	m_device = device;

	m_imageAvailableSemaphores.resize(framesInFlight, VK_NULL_HANDLE);
	m_inFlightFences.resize(framesInFlight, VK_NULL_HANDLE);
	m_renderFinishedSemaphores.resize(swapchainImageCount, VK_NULL_HANDLE);

	VkSemaphoreCreateInfo semaphoreInfo{};
	semaphoreInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;

	VkFenceCreateInfo fenceInfo{};
	fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
	fenceInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT;

	for (uint32_t i = 0; i < framesInFlight; ++i)
	{
		if (vkCreateSemaphore(
			m_device,
			&semaphoreInfo,
			nullptr,
			&m_imageAvailableSemaphores[i]) != VK_SUCCESS)
		{
			std::cerr << "Failed to create Image Available Semaphore!\n";
			Shutdown();
			return false;
		}

		if (vkCreateFence(
			m_device,
			&fenceInfo,
			nullptr,
			&m_inFlightFences[i]) != VK_SUCCESS)
		{
			std::cerr << "Failed to create In-Flight Fence!\n";
			Shutdown();
			return false;
		}
	}
	for (uint32_t i = 0; i < swapchainImageCount; ++i)
	{
		if (vkCreateSemaphore(
			m_device,
			&semaphoreInfo,
			nullptr,
			&m_renderFinishedSemaphores[i]) != VK_SUCCESS)
		{
			std::cerr << "Failed to create Render Finished Semaphore!\n";
			Shutdown();
			return false;
		}
	}

	std::cout << "Vulkan Sync Objects Created Successfully!\n";
	std::cout << "   Frames In Flight: " << framesInFlight << '\n';
	std::cout << "   Swapchain Images: " << swapchainImageCount << '\n';

	return true;

}
void Aether::VulkanSyncManager::Shutdown()
{
	if (m_device == VK_NULL_HANDLE)
		return;

	for (VkSemaphore semaphore : m_renderFinishedSemaphores)
	{
		if (semaphore != VK_NULL_HANDLE)
			vkDestroySemaphore(m_device, semaphore, nullptr);
	}

	for (VkSemaphore semaphore : m_imageAvailableSemaphores)
	{
		if (semaphore != VK_NULL_HANDLE)
			vkDestroySemaphore(m_device, semaphore, nullptr);
	}


	for (VkFence fence : m_inFlightFences)
	{
		if (fence != VK_NULL_HANDLE)
			vkDestroyFence(m_device, fence, nullptr);
	}

	m_renderFinishedSemaphores.clear();
	m_imageAvailableSemaphores.clear();
	m_inFlightFences.clear();

	m_device = VK_NULL_HANDLE;

	std::cout << "Vulkan Sync Objects Destroyed!\n";
}
VkSemaphore Aether::VulkanSyncManager::GetImageAvailableSemaphore(uint32_t frameIndex) const
{
	return m_imageAvailableSemaphores.at(frameIndex);
}

VkSemaphore Aether::VulkanSyncManager::GetRenderFinishedSemaphore(uint32_t imageIndex) const
{
	return m_renderFinishedSemaphores.at(imageIndex);
}

VkFence Aether::VulkanSyncManager::GetInFlightFence(uint32_t frameIndex) const
{
	return m_inFlightFences.at(frameIndex);
}
