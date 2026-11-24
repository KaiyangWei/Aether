#pragma once

#include <vulkan/vulkan.h>
#include <VkBootstrap.h>
#include <RHI/Vulkan/Resources/VulkanImage.h>
#include <RHI/Vulkan/VulkanRayTracingCapabilities.h>

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

		// 重建交换链接口
		bool RecreateSwapchain(Window& window);

		// 销毁当前交换链及其 ImageViews
		void DestroySwapchain();

		void WaitIdle() const;

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

		VkInstance GetInstance() const
		{
			return m_instance.instance;
		}

		VkPhysicalDevice GetPhysicalDevice() const
		{
			return m_physicalDevice.physical_device;
		}

		const VulkanRayTracingCapabilities& GetRayTracingCapabilities() const
		{
			return m_rayTracingCapabilities;
		}

		void Shutdown();

	private:
        friend class AetherApplication;
        struct SwapchainTestOptions
        {
            uint32_t minImageCount = 0;
            VkSurfaceFormatKHR format{};
            bool failCreate = false;
            uint32_t failView = 0; // One-based
        };
        SwapchainTestOptions m_swapchainTestOptions;
        bool m_disableRayTracingForTest = false;
        void ProbeAndConfigureOptionalRayTracing();
        bool BuildSwapchain(Window& window, bool recreate);
		vkb::Instance m_instance{};
		vkb::PhysicalDevice m_physicalDevice{};
		vkb::Device m_device{};
		vkb::Swapchain m_swapchain{};

		VulkanRayTracingCapabilities m_rayTracingCapabilities{};
		bool m_rayTracingRequested = false;

		std::vector<VulkanImage> m_swapchainResources;
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