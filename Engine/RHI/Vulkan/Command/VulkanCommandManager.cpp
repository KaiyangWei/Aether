#include "VulkanCommandManager.h"

#include <iostream>

bool Aether::VulkanCommandManager::initialize(
	VkDevice device,
	uint32_t graphicsQueueFamily,
	uint32_t commandBufferCount
)
{
	if (device == VK_NULL_HANDLE)
	{
		std::cerr << "Invalid Vulkan Device!\n";
		return false;
	}

	if (commandBufferCount == 0)
	{
		std::cerr << "Invalid Command Buffer Count!\n";
		return false;
	}

	m_device = device;

	VkCommandPoolCreateInfo poolInfo{};
	poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
	poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
	poolInfo.queueFamilyIndex = graphicsQueueFamily;

	VkResult result = vkCreateCommandPool(
		m_device,
		&poolInfo,
		nullptr,
		&m_commandPool
	);

	if (result != VK_SUCCESS)
	{
		std::cerr << "Failed to Create Command Pool! VkResult: "
			<< result << '\n';

		m_device = VK_NULL_HANDLE;
		return false;
	}

	std::cout << "Command Pool Created Successfully!\n";

	VkCommandBufferAllocateInfo allocInfo{};
	allocInfo.sType =
		VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;

	allocInfo.commandPool = m_commandPool;
	allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
	allocInfo.commandBufferCount = commandBufferCount;
	m_commandBuffers.resize(commandBufferCount);

	result = vkAllocateCommandBuffers(
		m_device,
		&allocInfo,
		m_commandBuffers.data()
	);

	if (result != VK_SUCCESS)
	{
		std::cerr << "Failed to Allocate Command Buffers! VkResult: "
			<< result << '\n';

		m_commandBuffers.clear();

		vkDestroyCommandPool(m_device, m_commandPool, nullptr);

		m_commandPool = VK_NULL_HANDLE;
		m_device = VK_NULL_HANDLE;

		return false;
	}

	std::cout << "Command Buffers Allocated Successfully!\n";
	std::cout << "   Command Buffer Count: "
		<< m_commandBuffers.size() << '\n';
	return true;
}

void Aether::VulkanCommandManager::Shutdown()
{
	if (m_commandPool != VK_NULL_HANDLE)
	{
		vkDestroyCommandPool(
			m_device,
			m_commandPool,
			nullptr
		);

		m_commandPool = VK_NULL_HANDLE;
		m_commandBuffers.clear();

		std::cout << "Command Pool Destroyed!\n";
	}

	m_device = VK_NULL_HANDLE;
}

Aether::VulkanCommandManager::~VulkanCommandManager()
{
	Shutdown();
}