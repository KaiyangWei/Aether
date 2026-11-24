#pragma once

#include <vk_mem_alloc.h>

namespace Aether
{
    class VulkanMemoryManager;

    class VulkanImage
    {
    public:
        VulkanImage() = default;
        ~VulkanImage();

        VulkanImage(const VulkanImage&) = delete;
        VulkanImage& operator=(const VulkanImage&) = delete;

        VkResult Initialize(
            VkDevice device, const VulkanMemoryManager& memory, VkExtent2D extent,
            VkFormat format, VkImageUsageFlags usage, VkImageAspectFlags aspect, uint32_t mipLevels = 1, bool cube = false);
        VkResult InitializeSwapchainView(
            VkDevice device, VkImage image, VkExtent2D extent, VkFormat format);
        void Shutdown() noexcept;

        VkImage GetImage() const { return m_image; }
        VkImageView GetView() const { return m_view; }
        VkExtent2D GetExtent() const { return m_extent; }
        uint32_t GetMipLevels() const { return m_mipLevels; }
        VkFormat GetFormat() const { return m_format; }

    private:
        VkResult CreateView(VkImageAspectFlags aspect);

        VkDevice m_device = VK_NULL_HANDLE;
        const VulkanMemoryManager* m_memory = nullptr;
        VmaAllocation m_allocation = VK_NULL_HANDLE;
        VkImage m_image = VK_NULL_HANDLE;
        VkImageView m_view = VK_NULL_HANDLE;
        VkExtent2D m_extent{};
        uint32_t m_mipLevels = 1;
        bool m_cube = false;
        VkFormat m_format = VK_FORMAT_UNDEFINED;
    };
}
