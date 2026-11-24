#include "VulkanImage.h"

#include <RHI/Vulkan/VulkanMemoryManager.h>

namespace Aether
{
    VulkanImage::~VulkanImage()
    {
        Shutdown();
    }

    VkResult VulkanImage::Initialize(
        VkDevice device, const VulkanMemoryManager& memory, VkExtent2D extent,
        VkFormat format, VkImageUsageFlags usage, VkImageAspectFlags aspect, uint32_t mipLevels, bool cube)
    {
        if (m_image || !device || !extent.width || !extent.height ||
            format == VK_FORMAT_UNDEFINED || !usage || !aspect || !mipLevels)
            return VK_ERROR_INITIALIZATION_FAILED;

        VkImageCreateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        info.imageType = VK_IMAGE_TYPE_2D;
        info.format = format;
        info.extent = { extent.width, extent.height, 1 };
        info.mipLevels = mipLevels;
        info.arrayLayers = cube ? 6 : 1;
        info.flags = cube ? VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT : 0;
        info.samples = VK_SAMPLE_COUNT_1_BIT;
        info.tiling = VK_IMAGE_TILING_OPTIMAL;
        info.usage = usage;
        info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

        VkImage image = VK_NULL_HANDLE;
        VkResult result = vkCreateImage(device, &info, nullptr, &image);
        if (result != VK_SUCCESS)
            return result;

        m_device = device;
        m_image = image;
        m_memory = &memory;
        m_mipLevels = mipLevels;
        m_cube = cube;
        m_extent = extent;
        m_format = format;

        VmaAllocation allocation = VK_NULL_HANDLE;
        result = memory.AllocateForImage(image, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, allocation);
        if (result == VK_SUCCESS)
        {
            m_allocation = allocation;
            result = memory.BindImage(image, allocation);
        }
        if (result == VK_SUCCESS)
            result = CreateView(aspect);
        if (result != VK_SUCCESS)
            Shutdown();
        return result;
    }

    VkResult VulkanImage::InitializeSwapchainView(
        VkDevice device, VkImage image, VkExtent2D extent, VkFormat format)
    {
        if (m_image || !device || !image || !extent.width || !extent.height ||
            format == VK_FORMAT_UNDEFINED)
            return VK_ERROR_INITIALIZATION_FAILED;

        m_device = device;
        m_image = image;
        m_extent = extent;
        m_format = format;
        const VkResult result = CreateView(VK_IMAGE_ASPECT_COLOR_BIT);
        if (result != VK_SUCCESS)
            Shutdown();
        return result;
    }

    VkResult VulkanImage::CreateView(VkImageAspectFlags aspect)
    {
        VkImageViewCreateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        info.image = m_image;
        info.viewType = m_cube ? VK_IMAGE_VIEW_TYPE_CUBE : VK_IMAGE_VIEW_TYPE_2D;
        info.format = m_format;
        info.subresourceRange = { aspect, 0, m_mipLevels, 0, m_cube ? 6u : 1u };
        VkImageView view = VK_NULL_HANDLE;
        const VkResult result = vkCreateImageView(m_device, &info, nullptr, &view);
        if (result == VK_SUCCESS)
            m_view = view;
        return result;
    }

    void VulkanImage::Shutdown() noexcept
    {
        if (m_view)
            vkDestroyImageView(m_device, m_view, nullptr);
        if (m_memory)
        {
            vkDestroyImage(m_device, m_image, nullptr);
            if (m_allocation)
                m_memory->Free(m_allocation);
        }
        m_device = VK_NULL_HANDLE;
        m_memory = nullptr;
        m_allocation = VK_NULL_HANDLE;
        m_image = VK_NULL_HANDLE;
        m_view = VK_NULL_HANDLE;
        m_extent = {};
        m_mipLevels = 1;
        m_cube = false;
        m_format = VK_FORMAT_UNDEFINED;
    }
}
