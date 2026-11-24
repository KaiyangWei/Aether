#include "VulkanBuffer.h"

#include <RHI/Vulkan/VulkanMemoryManager.h>

#include <iostream>

namespace Aether {

    VulkanBuffer::~VulkanBuffer() {
        Shutdown();
    }

    VkResult VulkanBuffer::Initialize(
        VkDevice device,
        const VulkanMemoryManager& memory,
        VkDeviceSize size,
        VkBufferUsageFlags usage,
        VkMemoryPropertyFlags required,
        VkMemoryPropertyFlags preferred
    ) {
        if (device == VK_NULL_HANDLE || size == 0 || usage == 0 || m_buffer != VK_NULL_HANDLE)
            return VK_ERROR_INITIALIZATION_FAILED;

        if ((usage & VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT) != 0 &&
            !memory.IsBufferDeviceAddressEnabled()) {
            std::cerr << "VulkanBuffer::Initialize() failed: VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT "
                << "requested while Buffer Device Address is disabled on the allocator/device.\n";
            return VK_ERROR_FEATURE_NOT_PRESENT;
        }

        VkBufferCreateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        info.size = size;
        info.usage = usage;
        info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

        VkBuffer buffer = VK_NULL_HANDLE;
        VkResult result = vkCreateBuffer(device, &info, nullptr, &buffer);
        if (result != VK_SUCCESS)
            return result;

        VmaAllocation allocation = VK_NULL_HANDLE;
        result = memory.AllocateForBuffer(buffer, required, preferred, allocation);
        if (result != VK_SUCCESS) {
            vkDestroyBuffer(device, buffer, nullptr);
            return result;
        }

        result = memory.BindBuffer(buffer, allocation);
        if (result != VK_SUCCESS) {
            vkDestroyBuffer(device, buffer, nullptr);
            memory.Free(allocation);
            return result;
        }

        m_memory = &memory;
        m_device = device;
        m_buffer = buffer;
        m_allocation = allocation;
        m_size = size;
        m_usage = usage;
        return VK_SUCCESS;
    }

    VkDeviceAddress VulkanBuffer::GetDeviceAddress() const noexcept {
        if (m_device == VK_NULL_HANDLE || m_buffer == VK_NULL_HANDLE || m_memory == nullptr) {
            std::cerr << "VulkanBuffer::GetDeviceAddress() failed: buffer is not initialized.\n";
            return 0;
        }
        if (!m_memory->IsBufferDeviceAddressEnabled()) {
            std::cerr << "VulkanBuffer::GetDeviceAddress() failed: Buffer Device Address is not enabled.\n";
            return 0;
        }
        if ((m_usage & VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT) == 0) {
            std::cerr << "VulkanBuffer::GetDeviceAddress() failed: buffer was not created with "
                << "VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT.\n";
            return 0;
        }

        VkBufferDeviceAddressInfo addressInfo{};
        addressInfo.sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO;
        addressInfo.buffer = m_buffer;
        const VkDeviceAddress address = vkGetBufferDeviceAddress(m_device, &addressInfo);
        if (address == 0) {
            std::cerr << "VulkanBuffer::GetDeviceAddress() failed: vkGetBufferDeviceAddress returned 0.\n";
        }
        return address;
    }

    VkResult VulkanBuffer::UploadData(const void* data, VkDeviceSize size) const {
        if (data == nullptr || size == 0 || size > m_size || m_allocation == VK_NULL_HANDLE)
            return VK_ERROR_INITIALIZATION_FAILED;

        return m_memory->Write(m_allocation, data, size);
    }

    VkResult VulkanBuffer::ReadData(void* data, VkDeviceSize size) const
    {
        if (!data || !size || size > m_size || !m_allocation)
            return VK_ERROR_INITIALIZATION_FAILED;
        return m_memory->Read(m_allocation, data, size);
    }

    void VulkanBuffer::Shutdown() noexcept {
        if (m_buffer == VK_NULL_HANDLE)
            return;

        vkDestroyBuffer(m_device, m_buffer, nullptr);
        m_memory->Free(m_allocation);

        m_buffer = VK_NULL_HANDLE;
        m_allocation = VK_NULL_HANDLE;
        m_device = VK_NULL_HANDLE;
        m_memory = nullptr;
        m_size = 0;
        m_usage = 0;
    }

}
