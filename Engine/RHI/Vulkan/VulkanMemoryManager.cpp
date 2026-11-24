#define VMA_IMPLEMENTATION
#include <vk_mem_alloc.h>

#include "VulkanMemoryManager.h"

#include <iostream>

#include <RHI/Vulkan/VulkanContext.h>

namespace Aether {

    VulkanMemoryManager::~VulkanMemoryManager() {
        Shutdown();
    }

    bool VulkanMemoryManager::Initialize(
        const VulkanContext& context
    ) {
        if (m_allocator != VK_NULL_HANDLE) {
            return false;
        }

        if (context.GetInstance() == VK_NULL_HANDLE ||
            context.GetPhysicalDevice() == VK_NULL_HANDLE ||
            context.GetDevice() == VK_NULL_HANDLE) {
            return false;
        }

        const bool enableBufferDeviceAddress =
            context.GetRayTracingCapabilities().IsBufferDeviceAddressEnabled();

        VmaAllocatorCreateInfo allocatorCreateInfo{};
        allocatorCreateInfo.instance = context.GetInstance();
        allocatorCreateInfo.physicalDevice = context.GetPhysicalDevice();
        allocatorCreateInfo.device = context.GetDevice();
        allocatorCreateInfo.vulkanApiVersion = VK_API_VERSION_1_3;
        if (enableBufferDeviceAddress) {
            allocatorCreateInfo.flags |= VMA_ALLOCATOR_CREATE_BUFFER_DEVICE_ADDRESS_BIT;
        }

        VkResult result = vmaCreateAllocator(
            &allocatorCreateInfo,
            &m_allocator
        );

        if (result != VK_SUCCESS) {
            m_allocator = VK_NULL_HANDLE;
            m_bufferDeviceAddressEnabled = false;
            std::cerr << "Failed to create VMA Allocator!\n";
            return false;
        }

        m_bufferDeviceAddressEnabled = enableBufferDeviceAddress;
        std::cout << "Vulkan Memory Allocator created successfully! (BDA: "
            << (m_bufferDeviceAddressEnabled ? "Enabled" : "Disabled") << ")\n";
        return true;
    }

    VkResult VulkanMemoryManager::AllocateForBuffer(
        VkBuffer buffer,
        VkMemoryPropertyFlags required,
        VkMemoryPropertyFlags preferred,
        VmaAllocation& allocation
    ) const {
        if (m_allocator == VK_NULL_HANDLE)
            return VK_ERROR_INITIALIZATION_FAILED;

        VmaAllocationCreateInfo info{};
        info.requiredFlags = required;
        info.preferredFlags = preferred;
        return vmaAllocateMemoryForBuffer(m_allocator, buffer, &info, &allocation, nullptr);
    }

    VkResult VulkanMemoryManager::AllocateForImage(
        VkImage image, VkMemoryPropertyFlags preferred, VmaAllocation& allocation
    ) const {
        if (m_allocator == VK_NULL_HANDLE)
            return VK_ERROR_INITIALIZATION_FAILED;

        VmaAllocationCreateInfo info{};
        info.preferredFlags = preferred;
        return vmaAllocateMemoryForImage(m_allocator, image, &info, &allocation, nullptr);
    }

    VkResult VulkanMemoryManager::BindImage(VkImage image, VmaAllocation allocation) const {
        return vmaBindImageMemory(m_allocator, allocation, image);
    }

    VkResult VulkanMemoryManager::BindBuffer(VkBuffer buffer, VmaAllocation allocation) const {
        return vmaBindBufferMemory(m_allocator, allocation, buffer);
    }

    VkResult VulkanMemoryManager::Write(
        VmaAllocation allocation,
        const void* data,
        VkDeviceSize size
    ) const {
        return vmaCopyMemoryToAllocation(m_allocator, data, allocation, 0, size);
    }

    VkResult VulkanMemoryManager::Read(VmaAllocation allocation, void* data, VkDeviceSize size) const
    {
        return vmaCopyAllocationToMemory(m_allocator, allocation, 0, data, size);
    }

    void VulkanMemoryManager::Free(VmaAllocation allocation) const noexcept {
        vmaFreeMemory(m_allocator, allocation);
    }

    void VulkanMemoryManager::Shutdown() {
        if (m_allocator == VK_NULL_HANDLE)
            return;

        vmaDestroyAllocator(m_allocator);
        m_allocator = VK_NULL_HANDLE;
        m_bufferDeviceAddressEnabled = false;
    }

}
