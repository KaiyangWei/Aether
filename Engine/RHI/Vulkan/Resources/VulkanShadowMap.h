#pragma once

#include <RHI/Vulkan/Resources/VulkanImage.h>
#include <cstdint>

namespace Aether
{
    class VulkanMemoryManager;

    class VulkanShadowMap
    {
    public:
        VulkanShadowMap() = default;
        ~VulkanShadowMap();

        VulkanShadowMap(const VulkanShadowMap&) = delete;
        VulkanShadowMap& operator=(const VulkanShadowMap&) = delete;

        static VkFormat SelectDepthFormat(VkPhysicalDevice physicalDevice, bool& supportsLinearFilter);

        VkResult Initialize(
            VkPhysicalDevice physicalDevice,
            VkDevice device,
            const VulkanMemoryManager& memory,
            uint32_t resolution
        );
        void Shutdown() noexcept;

        const VulkanImage& GetImage() const { return m_image; }
        VkImageView GetDepthView() const { return m_image.GetView(); }
        VkSampler GetCompareSampler() const { return m_compareSampler; }
        VkSampler GetDebugSampler() const { return m_debugSampler; }
        VkFormat GetFormat() const { return m_image.GetFormat(); }
        uint32_t GetResolution() const { return m_image.GetExtent().width; }
        bool SupportsLinearCompareFilter() const { return m_supportsLinearFilter; }

        VkImageLayout GetLayout() const { return m_layout; }
        void SetLayout(VkImageLayout layout) { m_layout = layout; }

    private:
        VkDevice m_device = VK_NULL_HANDLE;
        VulkanImage m_image;
        VkSampler m_compareSampler = VK_NULL_HANDLE;
        VkSampler m_debugSampler = VK_NULL_HANDLE;
        VkImageLayout m_layout = VK_IMAGE_LAYOUT_UNDEFINED;
        bool m_supportsLinearFilter = false;
    };
}
