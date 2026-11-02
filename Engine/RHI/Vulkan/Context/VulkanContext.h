#pragma once

#include <vulkan/vulkan.h>

#include <VkBootstrap.h>

#include <vector>
#include <cstdint>
namespace Aether
{
	class Window;

	class VulkanContext
	{
	public:
		VulkanContext() = default;
		~VulkanContext();

		VulkanContext(const VulkanContext&) = delete;
		VulkanContext& operator=(const VulkanContext&) = delete;

		bool Initialize(Window& window);
		bool CreateSurface(Window& window);

		bool EnumeratePhysicalDevices();
		bool SelectPhysicalDevice();
		bool CreateLogicalDevice();
		bool CreateSwapchain(Window& window);

		VkDevice GetDevice() const
		{
			return m_device.device;
		}

		uint32_t GetGraphicsQueueFamily() const
		{
			return m_graphicsQueueFamily;
		}

		uint32_t GetPresentQueueFamily() const
		{
			return m_presentQueueFamily;
		}

		VkSwapchainKHR GetSwapchain() const
		{
			return m_swapchain.swapchain;
		}

		VkExtent2D GetSwapchainExtent() const
		{
			return m_swapchainExtent;
		}

		const std::vector<VkImage>& GetSwapchainImages() const
		{
			return m_swapchainImages;
		}

		const std::vector<VkImageView>& GetSwapchainImageViews() const
		{
			return m_swapchainImageViews;
		}

		VkFormat GetSwapchainImageFormat() const
		{
			return m_swapchainImageFormat;
		}

		VkQueue GetGraphicsQueue() const
		{
			return m_graphicsQueue;
		}

		VkQueue GetPresentQueue() const
		{
			return m_presentQueue;
		}

		void Shutdown();

	private:

		vkb::Instance m_instance{};
		vkb::PhysicalDevice m_physicalDevice{};
		vkb::Device m_device{};
		vkb::Swapchain m_swapchain{};

		std::vector<VkImage> m_swapchainImages;
		std::vector<VkImageView> m_swapchainImageViews;

		VkFormat m_swapchainImageFormat = VK_FORMAT_UNDEFINED;
		VkExtent2D m_swapchainExtent{};

		VkSurfaceKHR m_surface = VK_NULL_HANDLE;

		VkQueue m_graphicsQueue = VK_NULL_HANDLE;
		VkQueue m_presentQueue = VK_NULL_HANDLE;

		uint32_t m_graphicsQueueFamily = UINT32_MAX;
		uint32_t m_presentQueueFamily = UINT32_MAX;

		bool m_initialized = false;
	};
}