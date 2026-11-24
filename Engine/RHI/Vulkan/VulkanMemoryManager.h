#pragma once

#include <vk_mem_alloc.h>

namespace Aether {

    class VulkanContext;

    class VulkanMemoryManager {
    public:
        VulkanMemoryManager() = default;
        ~VulkanMemoryManager();

        VulkanMemoryManager(const VulkanMemoryManager&) = delete;
        VulkanMemoryManager& operator=(const VulkanMemoryManager&) = delete;

        bool Initialize(const VulkanContext& context);
        void Shutdown();
        bool IsBufferDeviceAddressEnabled() const noexcept { return m_bufferDeviceAddressEnabled; }

        VkResult AllocateForBuffer(
            VkBuffer buffer,
            VkMemoryPropertyFlags required,
            VkMemoryPropertyFlags preferred,
            VmaAllocation& allocation
        ) const;
        VkResult AllocateForImage(VkImage image, VkMemoryPropertyFlags preferred, VmaAllocation& allocation) const;
        VkResult BindImage(VkImage image, VmaAllocation allocation) const;
        VkResult BindBuffer(VkBuffer buffer, VmaAllocation allocation) const;
        VkResult Write(VmaAllocation allocation, const void* data, VkDeviceSize size) const;
        VkResult Read(VmaAllocation allocation, void* data, VkDeviceSize size) const;
        void Free(VmaAllocation allocation) const noexcept;

    private:
        friend class AetherApplication;
        uint32_t GetAllocationCountForTest() const
        {
            VmaTotalStatistics stats{};
            vmaCalculateStatistics(m_allocator, &stats);
            return stats.total.statistics.allocationCount;
        }

        VmaAllocator m_allocator = VK_NULL_HANDLE;
        bool m_bufferDeviceAddressEnabled = false;
    };

}
