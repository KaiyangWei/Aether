#include "VulkanContext.h"

#include <Platform/Window/window.h>

#include <GLFW/glfw3.h>

#include <iostream>

#include <vector>

namespace Aether
{
	VulkanContext::~VulkanContext()
	{
		Shutdown();
	}

	bool VulkanContext::Initialize(Window& window)
	{
		if (m_initialized)
		{
			std::cerr << "VulkanContext already initialized!\n";
			return false;
		}

		if (window.GetNativeHandle() == nullptr)
		{
			std::cerr << "Invalid GLFW window!\n";
			return false;
		}

		uint32_t extensionCount = 0;

		const char** extensions =
			glfwGetRequiredInstanceExtensions(&extensionCount);

		if (extensions == nullptr || extensionCount == 0)
		{
			std::cerr << "Failed to get GLFW Vulkan extensions!\n";
			return false;
		}

		vkb::InstanceBuilder builder;
		auto instanceResult = builder
			.set_app_name("Aether")
			.set_engine_name("Aether Engine")
			.require_api_version(1, 3, 0)
			.enable_extensions(extensionCount, extensions)
			.enable_validation_layers()
			.use_default_debug_messenger()
			.build();

		if (!instanceResult)
		{
			std::cerr << "Failed to create Vulkan Instance: "
				<< instanceResult.error().message()
				<< '\n';
			return false;
		}

		m_instance = instanceResult.value();

		m_initialized = true;

		std::cout << "Vulkan Instance created successfully!\n";

		return true;
	}

	bool VulkanContext::CreateSurface(Window& window)
	{
		if (!m_initialized)
		{
			std::cerr << "Invalid Vulkan Instance!\n";
			return false;
		}
		if (m_surface != VK_NULL_HANDLE)
		{
			std::cerr << "Vulkan Surface already exist!\n";
			return false;
		}
		if (glfwCreateWindowSurface(
			m_instance.instance,
			window.GetNativeHandle(),
			nullptr,
			&m_surface
		) != VK_SUCCESS)
		{
			std::cerr << "Failed to create Vulkan Surface!\n";
			return false;
		}

		std::cout << "Vulkan Surface created successfully!\n";

		return true;
	}

	bool VulkanContext::EnumeratePhysicalDevices()
	{
		if (!m_initialized || m_surface == VK_NULL_HANDLE)
		{
			std::cerr << "Vulkan Instance or Surface is not initialized!\n";
			return false;
		}

		auto devicesResult = vkb::PhysicalDeviceSelector{ m_instance }
			.set_surface(m_surface)
			.add_required_extension(VK_KHR_SWAPCHAIN_EXTENSION_NAME)
			.select_devices();

		if (!devicesResult)
		{
			std::cerr << "Failed to enumerate suitable Physical Devices: "
				<< devicesResult.error().message() << '\n';
			return false;
		}

		std::cout << "========== Suitable Vulkan Physical Devices ==========\n";

		uint32_t index = 0;

		for (const auto& device : devicesResult.value())
		{
			std::cout << "Device " << index++ << ":\n";
			std::cout << "   Name: " << device.properties.deviceName << '\n';
			std::cout << "   Type: " << device.properties.deviceType << '\n';
			std::cout << "   API Version: "
				<< VK_VERSION_MAJOR(device.properties.apiVersion) << "."
				<< VK_VERSION_MINOR(device.properties.apiVersion) << "."
				<< VK_VERSION_PATCH(device.properties.apiVersion) << '\n';
		}

		return true;
	}

	bool VulkanContext::SelectPhysicalDevice()
	{
		if (!m_initialized || m_surface == VK_NULL_HANDLE)
		{
			std::cerr << "Vulkan Instance or Surface is not initialized!\n";
			return false;
		}

		if (m_physicalDevice.physical_device != VK_NULL_HANDLE)
		{
			std::cerr << "Physical Device already selected!\n";
			return false;
		}

		std::cout << "========== Selecting Physical Device ==========\n";
		VkPhysicalDeviceVulkan13Features features13{};
		features13.sType =
			VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;

		features13.synchronization2 = VK_TRUE;
		features13.dynamicRendering = VK_TRUE;
		auto physicalDeviceResult = vkb::PhysicalDeviceSelector{ m_instance }
			.set_surface(m_surface)
			.set_minimum_version(1, 3)
			.set_required_features_13(features13)
			.add_required_extension(VK_KHR_SWAPCHAIN_EXTENSION_NAME)
			.select();

		if (!physicalDeviceResult)
		{
			std::cerr << "Failed to select Physical Device: "
				<< physicalDeviceResult.error().message() << '\n';
			return false;
		}

		m_physicalDevice = physicalDeviceResult.value();

		std::cout << "Selected GPU: "
			<< m_physicalDevice.properties.deviceName << '\n';

		return true;
	}

	bool VulkanContext::CreateLogicalDevice()
	{
		if (m_physicalDevice.physical_device == VK_NULL_HANDLE)
		{
			std::cerr << "Invalid Physical Device!\n";
			return false;
		}

		if (m_device.device != VK_NULL_HANDLE)
		{
			std::cerr << "Logical Device already exists!\n";
			return false;
		}

		auto deviceResult = vkb::DeviceBuilder{ m_physicalDevice }
		.build();

		if (!deviceResult)
		{
			std::cerr << "Failed to create Logical Device: "
				<< deviceResult.error().message() << '\n';
			return false;
		}

		m_device = deviceResult.value();

		auto graphicsQueueResult =
			m_device.get_queue(vkb::QueueType::graphics);

		auto graphicsQueueIndexResult =
			m_device.get_queue_index(vkb::QueueType::graphics);

		if (!graphicsQueueResult || !graphicsQueueIndexResult)
		{
			std::cerr << "Failed to retrieve Graphics Queue!\n";
			vkb::destroy_device(m_device);
			m_device = {};
			return false;
		}

		m_graphicsQueue = graphicsQueueResult.value();
		m_graphicsQueueFamily = graphicsQueueIndexResult.value();

		auto presentQueueResult =
			m_device.get_queue(vkb::QueueType::present);

		auto presentQueueIndexResult =
			m_device.get_queue_index(vkb::QueueType::present);

		if (!presentQueueResult || !presentQueueIndexResult)
		{
			std::cerr << "Failed to retrieve Present Queue!\n";
			vkb::destroy_device(m_device);
			m_device = {};

			m_graphicsQueue = VK_NULL_HANDLE;
			m_graphicsQueueFamily = UINT32_MAX;

			return false;
		}

		m_presentQueue = presentQueueResult.value();
		m_presentQueueFamily = presentQueueIndexResult.value();

		std::cout << "Logical Device created successfully!\n";
		std::cout << "Graphics Queue Family: "
			<< m_graphicsQueueFamily << '\n';
		std::cout << "Present Queue Family: "
			<< m_presentQueueFamily << '\n';

		return true;
	}

	bool VulkanContext::CreateSwapchain(Window& window)
	{
		if (m_device.device == VK_NULL_HANDLE)
		{
			std::cerr << "Invalid Logical Device!\n";
			return false;
		}

		if (m_swapchain.swapchain != VK_NULL_HANDLE)
		{
			std::cerr << "Swapchain already exists!\n";
			return false;
		}

		GLFWwindow* nativeWindow = window.GetNativeHandle();

		if (nativeWindow == nullptr)
		{
			std::cerr << "Invalid GLFW Window!\n";
			return false;
		}
		int width = 0;
		int height = 0;
		glfwGetFramebufferSize(nativeWindow, &width, &height);

		if (width <= 0 || height <= 0)
		{
			std::cerr << "Invalid Framebuffer Size!\n";
			return false;
		}
		auto swapchainResult = vkb::SwapchainBuilder{ m_device }
			.use_default_format_selection()
			.set_desired_present_mode(VK_PRESENT_MODE_FIFO_KHR)
			.set_desired_extent(
				static_cast<uint32_t>(width),
				static_cast<uint32_t>(height)
			)
			.set_image_usage_flags(VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT)
			.build();
		if (!swapchainResult)
		{
			std::cerr << "Failed to create Swapchain: "
				<< swapchainResult.error().message() << '\n';
			return false;
		}
		m_swapchain = swapchainResult.value();

		auto imagesAndViewsResult =
			m_swapchain.get_images_and_image_views();

		if (!imagesAndViewsResult)
		{
			std::cerr << "Failed to retrieve Swapchain Images and Image Views: "
				<< imagesAndViewsResult.error().message() << '\n';

			vkb::destroy_swapchain(m_swapchain);
			m_swapchain = {};

			return false;
		}

		m_swapchainImages = imagesAndViewsResult.value().first;
		m_swapchainImageViews = imagesAndViewsResult.value().second;

		m_swapchainImageFormat = m_swapchain.image_format;
		m_swapchainExtent = m_swapchain.extent;

		std::cout << "Swapchain created successfully!\n";
		std::cout << "   Image Count: "
			<< m_swapchainImages.size() << '\n';
		std::cout << "   Image Format: "
			<< m_swapchainImageFormat << '\n';
		std::cout << "   Extent: "
			<< m_swapchainExtent.width << " x "
			<< m_swapchainExtent.height << '\n';

		return true;
	}

	void VulkanContext::Shutdown()
	{
		if (m_swapchain.swapchain != VK_NULL_HANDLE)
		{
			if (!m_swapchainImageViews.empty())
			{
				m_swapchain.destroy_image_views(m_swapchainImageViews);
				m_swapchainImageViews.clear();
			}

			m_swapchainImages.clear();

			vkb::destroy_swapchain(m_swapchain);
			m_swapchain = {};

			m_swapchainImageFormat = VK_FORMAT_UNDEFINED;
			m_swapchainExtent = {};

			std::cout << "Swapchain destroyed!\n";
		}
		if (m_device.device != VK_NULL_HANDLE)
		{
			vkb::destroy_device(m_device);
			m_device = {};

			m_graphicsQueue = VK_NULL_HANDLE;
			m_presentQueue = VK_NULL_HANDLE;

			m_graphicsQueueFamily = UINT32_MAX;
			m_presentQueueFamily = UINT32_MAX;

			std::cout << "Logical Device destroyed!\n";
		}

		if (m_surface != VK_NULL_HANDLE)
		{
			vkb::destroy_surface(m_instance, m_surface);
			m_surface = VK_NULL_HANDLE;

			std::cout << "Vulkan Surface destroyed!\n";
		}

		m_physicalDevice = {};

		if (m_initialized)
		{
			vkb::destroy_instance(m_instance);
			m_instance = {};
			m_initialized = false;

			std::cout << "Vulkan Instance destroyed!\n";
		}
	}
}