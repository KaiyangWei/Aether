#include "VulkanContext.h"

#include <Platform/Window.h>
#include <GLFW/glfw3.h>

#include <array>
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
			std::cerr << "Vulkan Surface already exists!\n";
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

		ProbeAndConfigureOptionalRayTracing();
		return true;
	}

	void VulkanContext::ProbeAndConfigureOptionalRayTracing()
	{
		m_rayTracingCapabilities = {};
		m_rayTracingRequested = false;

		if (m_physicalDevice.physical_device == VK_NULL_HANDLE)
			return;

		auto& caps = m_rayTracingCapabilities;
		caps.deferredHostOperationsExtensionSupported =
			m_physicalDevice.is_extension_present(VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME);
		caps.accelerationStructureExtensionSupported =
			m_physicalDevice.is_extension_present(VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME);
		caps.rayQueryExtensionSupported =
			m_physicalDevice.is_extension_present(VK_KHR_RAY_QUERY_EXTENSION_NAME);

		VkPhysicalDeviceVulkan12Features supported12{};
		supported12.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;

		VkPhysicalDeviceAccelerationStructureFeaturesKHR supportedAs{};
		supportedAs.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR;

		VkPhysicalDeviceRayQueryFeaturesKHR supportedRq{};
		supportedRq.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_QUERY_FEATURES_KHR;

		VkPhysicalDeviceFeatures2 supportedFeatures2{};
		supportedFeatures2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;

		void** nextFeature = &supportedFeatures2.pNext;
		*nextFeature = &supported12;
		nextFeature = &supported12.pNext;
		if (caps.accelerationStructureExtensionSupported)
		{
			*nextFeature = &supportedAs;
			nextFeature = &supportedAs.pNext;
		}
		if (caps.rayQueryExtensionSupported)
		{
			*nextFeature = &supportedRq;
			nextFeature = &supportedRq.pNext;
		}
		vkGetPhysicalDeviceFeatures2(m_physicalDevice.physical_device, &supportedFeatures2);

		caps.bufferDeviceAddressFeatureSupported = supported12.bufferDeviceAddress == VK_TRUE;
		caps.accelerationStructureFeatureSupported =
			caps.accelerationStructureExtensionSupported && supportedAs.accelerationStructure == VK_TRUE;
		caps.rayQueryFeatureSupported =
			caps.rayQueryExtensionSupported && supportedRq.rayQuery == VK_TRUE;

		if (caps.accelerationStructureExtensionSupported)
		{
			VkPhysicalDeviceAccelerationStructurePropertiesKHR asProps{};
			asProps.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_PROPERTIES_KHR;
			VkPhysicalDeviceProperties2 props2{};
			props2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
			props2.pNext = &asProps;
			vkGetPhysicalDeviceProperties2(m_physicalDevice.physical_device, &props2);

			caps.minScratchOffsetAlignment = asProps.minAccelerationStructureScratchOffsetAlignment;
			caps.maxGeometryCount = asProps.maxGeometryCount;
			caps.maxInstanceCount = asProps.maxInstanceCount;
			caps.maxPrimitiveCount = asProps.maxPrimitiveCount;
			caps.maxPerStageDescriptorAccelerationStructures =
				asProps.maxPerStageDescriptorAccelerationStructures;
		}

		// Match vk-bootstrap's QueueType::graphics selection (first family with VK_QUEUE_GRAPHICS_BIT).
		const auto queueFamilies = m_physicalDevice.get_queue_families();
		for (const auto& family : queueFamilies)
		{
			if ((family.queueFlags & VK_QUEUE_GRAPHICS_BIT) != 0)
			{
				caps.graphicsQueueSupportsCompute = (family.queueFlags & VK_QUEUE_COMPUTE_BIT) != 0;
				break;
			}
		}

		caps.hardwareSupported = EvaluateRayTracingHardwareSupport(caps);

		if (!caps.hardwareSupported || m_disableRayTracingForTest)
			return;

		// Atomically enable the complete RT extension + feature set on a candidate copy.
		// Vulkan 1.2+ promotes VK_KHR_buffer_device_address to core, so only the three
		// non-core KHR extensions are added as device extensions.
		vkb::PhysicalDevice candidate = m_physicalDevice;
		constexpr std::array<const char*, 3> requiredExtensions = {
			VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME,
			VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME,
			VK_KHR_RAY_QUERY_EXTENSION_NAME
		};

		VkPhysicalDeviceVulkan12Features required12{};
		required12.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
		required12.bufferDeviceAddress = VK_TRUE;

		VkPhysicalDeviceAccelerationStructureFeaturesKHR requiredAs{};
		requiredAs.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR;
		requiredAs.accelerationStructure = VK_TRUE;

		VkPhysicalDeviceRayQueryFeaturesKHR requiredRq{};
		requiredRq.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_QUERY_FEATURES_KHR;
		requiredRq.rayQuery = VK_TRUE;

		const bool extensionsEnabled = candidate.enable_extensions_if_present(
			requiredExtensions.size(), requiredExtensions.data());
		const bool v12Enabled = extensionsEnabled && candidate.enable_extension_features_if_present(required12);
		const bool asEnabled = v12Enabled && candidate.enable_extension_features_if_present(requiredAs);
		const bool rqEnabled = asEnabled && candidate.enable_extension_features_if_present(requiredRq);

		if (rqEnabled)
		{
			m_physicalDevice = std::move(candidate);
			m_rayTracingRequested = true;
		}
		else
		{
			std::cerr << "Warning: Optional Ray Tracing feature/extension enablement failed atomically; "
				<< "falling back to rasterization-only mode.\n";
		}
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

		bool activeGraphicsQueueSupportsCompute = false;
		const auto queueFamilies = m_physicalDevice.get_queue_families();
		if (m_graphicsQueueFamily < queueFamilies.size())
		{
			activeGraphicsQueueSupportsCompute =
				(queueFamilies[m_graphicsQueueFamily].queueFlags & VK_QUEUE_COMPUTE_BIT) != 0;
		}

		VulkanRayTracingFunctions loadedFunctions{};
		if (m_rayTracingRequested)
		{
			auto loadDeviceProc = [this](const char* name) {
				return vkGetDeviceProcAddr(m_device.device, name);
			};
			loadedFunctions.vkCreateAccelerationStructureKHR =
				reinterpret_cast<PFN_vkCreateAccelerationStructureKHR>(
					loadDeviceProc("vkCreateAccelerationStructureKHR"));
			loadedFunctions.vkDestroyAccelerationStructureKHR =
				reinterpret_cast<PFN_vkDestroyAccelerationStructureKHR>(
					loadDeviceProc("vkDestroyAccelerationStructureKHR"));
			loadedFunctions.vkGetAccelerationStructureBuildSizesKHR =
				reinterpret_cast<PFN_vkGetAccelerationStructureBuildSizesKHR>(
					loadDeviceProc("vkGetAccelerationStructureBuildSizesKHR"));
			loadedFunctions.vkCmdBuildAccelerationStructuresKHR =
				reinterpret_cast<PFN_vkCmdBuildAccelerationStructuresKHR>(
					loadDeviceProc("vkCmdBuildAccelerationStructuresKHR"));
			loadedFunctions.vkGetAccelerationStructureDeviceAddressKHR =
				reinterpret_cast<PFN_vkGetAccelerationStructureDeviceAddressKHR>(
					loadDeviceProc("vkGetAccelerationStructureDeviceAddressKHR"));
		}

		FinalizeRayTracingDeviceCapabilities(
			m_rayTracingCapabilities,
			m_rayTracingRequested,
			loadedFunctions,
			activeGraphicsQueueSupportsCompute);

		if (m_rayTracingCapabilities.IsDeviceEnabled() && !m_rayTracingCapabilities.AreFunctionsAvailable())
		{
			std::cerr << "Warning: Ray Tracing device extensions were enabled, but one or more "
				<< "required KHR acceleration structure functions failed to load; RT remains unavailable.\n";
		}

		std::cout << "Logical Device created successfully!\n";
		std::cout << "Graphics Queue Family: "
			<< m_graphicsQueueFamily << '\n';
		std::cout << "Present Queue Family: "
			<< m_presentQueueFamily << '\n';
		std::cout << "Ray Tracing Capabilities - Hardware: "
			<< (m_rayTracingCapabilities.IsHardwareSupported() ? "Supported" : "Unsupported")
			<< ", Device Enabled: "
			<< (m_rayTracingCapabilities.IsDeviceEnabled() ? "Yes" : "No")
			<< ", Functions Loaded: "
			<< (m_rayTracingCapabilities.AreFunctionsAvailable() ? "Yes" : "No")
			<< ", Ready: "
			<< (m_rayTracingCapabilities.IsReady() ? "Yes" : "No") << '\n';
		if (m_rayTracingCapabilities.minScratchOffsetAlignment > 0)
		{
			std::cout << "   AS Min Scratch Alignment: "
				<< m_rayTracingCapabilities.minScratchOffsetAlignment << " bytes\n";
		}

		return true;
	}

	bool VulkanContext::CreateSwapchain(Window& window)
	{
		return BuildSwapchain(window, false);
	}

	bool VulkanContext::RecreateSwapchain(Window& window)
	{
		return BuildSwapchain(window, true);
	}

	bool VulkanContext::BuildSwapchain(Window& window, bool recreate)
	{
		if (!m_device.device || !window.GetNativeHandle() ||
			recreate != (m_swapchain.swapchain != VK_NULL_HANDLE))
			return false;

		int width = 0;
		int height = 0;
		glfwGetFramebufferSize(window.GetNativeHandle(), &width, &height);
		if (width <= 0 || height <= 0)
			return false;

		if (recreate && vkDeviceWaitIdle(m_device.device) != VK_SUCCESS)
			return false;

		auto builder = vkb::SwapchainBuilder{ m_device }
			.set_old_swapchain(m_swapchain)
			.use_default_format_selection()
			.set_desired_present_mode(VK_PRESENT_MODE_FIFO_KHR)
			.set_desired_extent(static_cast<uint32_t>(width), static_cast<uint32_t>(height))
			.set_image_usage_flags(VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT);

		VkSurfaceCapabilitiesKHR capabilities{};
		if (vkGetPhysicalDeviceSurfaceCapabilitiesKHR(GetPhysicalDevice(), m_surface, &capabilities) == VK_SUCCESS &&
			(capabilities.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_SRC_BIT))
			builder.add_image_usage_flags(VK_IMAGE_USAGE_TRANSFER_SRC_BIT);

		const auto options = m_swapchainTestOptions;
		if (options.minImageCount)
			builder.set_required_min_image_count(options.minImageCount);
		if (options.format.format != VK_FORMAT_UNDEFINED)
			builder.set_desired_format(options.format);

		auto result = options.failCreate
			? vkb::Result<vkb::Swapchain>{vkb::SwapchainError::failed_create_swapchain, VK_ERROR_INITIALIZATION_FAILED}
			: builder.build();

		// Passing oldSwapchain to Vulkan can retire it even on creation failure.
		DestroySwapchain();
		if (!result)
		{
			std::cerr << "Failed to create Swapchain: " << result.error().message() << '\n';
			return false;
		}

		m_swapchain = result.value();
		auto images = m_swapchain.get_images();
		if (!images || images.value().empty())
		{
			DestroySwapchain();
			return false;
		}
		m_swapchainImages = std::move(images.value());
		m_swapchainImageViews.reserve(m_swapchainImages.size());

		std::vector<VulkanImage>(m_swapchainImages.size()).swap(m_swapchainResources);
		for (size_t i = 0; i < m_swapchainImages.size(); ++i)
		{
			auto& image = m_swapchainResources[i];
			const VkResult viewResult = options.failView == i + 1
				? VK_ERROR_OUT_OF_HOST_MEMORY
				: image.InitializeSwapchainView(m_device.device, m_swapchainImages[i],
					m_swapchain.extent, m_swapchain.image_format);
			if (viewResult != VK_SUCCESS)
			{
				std::cerr << "Failed to create Swapchain ImageView "
					<< m_swapchainImageViews.size() + 1 << ": " << viewResult << '\n';
				DestroySwapchain();
				return false;
			}
			m_swapchainImageViews.push_back(image.GetView());
		}

		m_swapchainImageFormat = m_swapchain.image_format;
		m_swapchainExtent = m_swapchain.extent;
		std::cout << "Swapchain " << (recreate ? "recreated" : "created")
			<< " successfully. Images: " << m_swapchainImages.size()
			<< ", format: " << m_swapchainImageFormat << ", extent: "
			<< m_swapchainExtent.width << " x " << m_swapchainExtent.height << '\n';
		return true;
	}

	void VulkanContext::DestroySwapchain()
	{
		if (m_swapchain.swapchain != VK_NULL_HANDLE)
		{
			m_swapchainImageViews.clear();
			m_swapchainResources.clear();
			m_swapchainImages.clear();

			vkb::destroy_swapchain(m_swapchain);
			m_swapchain = {};

			m_swapchainImageFormat = VK_FORMAT_UNDEFINED;
			m_swapchainExtent = {};

			std::cout << "Swapchain destroyed!\n";
		}
	}

	void VulkanContext::WaitIdle() const
	{
		if (m_device.device != VK_NULL_HANDLE)
		{
			vkDeviceWaitIdle(m_device.device);
		}
	}

	void VulkanContext::Shutdown()
	{
		DestroySwapchain();

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
		m_rayTracingCapabilities = {};
		m_rayTracingRequested = false;

		if (m_initialized)
		{
			vkb::destroy_instance(m_instance);
			m_instance = {};
			m_initialized = false;

			std::cout << "Vulkan Instance destroyed!\n";
		}
	}
}
