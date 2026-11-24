#pragma once

#include <vk_mem_alloc.h>

namespace Aether {

    class VulkanMemoryManager;

    class VulkanBuffer {
    public:
        VulkanBuffer() = default;
        ~VulkanBuffer();

        VulkanBuffer(const VulkanBuffer&) = delete;
        VulkanBuffer& operator=(const VulkanBuffer&) = delete;

        // The device and allocator must outlive the buffer. Wait for GPU use
        // to complete before uploading to or releasing an in-flight buffer.
        VkResult Initialize(
            VkDevice device,
            const VulkanMemoryManager& memory,
            VkDeviceSize size,
            VkBufferUsageFlags usage,
            VkMemoryPropertyFlags required,
            VkMemoryPropertyFlags preferred = 0
        );
        VkResult UploadData(const void* data, VkDeviceSize size) const;
        VkResult ReadData(void* data, VkDeviceSize size) const;
        void Shutdown() noexcept;

        VkBuffer GetBuffer() const { return m_buffer; }
        VkDeviceSize GetSize() const { return m_size; }
        VkBufferUsageFlags GetUsage() const noexcept { return m_usage; }
        bool HasDeviceAddressUsage() const noexcept
        {
            return (m_usage & VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT) != 0;
        }
        VkDeviceAddress GetDeviceAddress() const noexcept;

    private:
        const VulkanMemoryManager* m_memory = nullptr;
        VkDevice m_device = VK_NULL_HANDLE;
        VkBuffer m_buffer = VK_NULL_HANDLE;
        VmaAllocation m_allocation = VK_NULL_HANDLE;
        VkDeviceSize m_size = 0;
        VkBufferUsageFlags m_usage = 0;
    };

}
