#pragma once

#include "VulkanImage.h"

namespace Aether
{
    struct TextureSampling
    {
        VkFilter minFilter = VK_FILTER_LINEAR;
        VkFilter magFilter = VK_FILTER_LINEAR;
        VkSamplerMipmapMode mipMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
        VkSamplerAddressMode wrapU = VK_SAMPLER_ADDRESS_MODE_REPEAT;
        VkSamplerAddressMode wrapV = VK_SAMPLER_ADDRESS_MODE_REPEAT;
        bool mipmapped = true;
    };

    class VulkanTexture
    {
    public:
        VulkanTexture() = default;
        ~VulkanTexture();
        VulkanTexture(const VulkanTexture&) = delete;
        VulkanTexture& operator=(const VulkanTexture&) = delete;

        const VulkanImage& GetImage() const { return m_image; }
        VkSampler GetSampler() const { return m_sampler; }
        void Shutdown() noexcept;

    private:
        friend class VulkanRenderer;
        VkResult Initialize(VkDevice device, const VulkanMemoryManager& memory, VkExtent2D extent,
                            VkFormat format, uint32_t mipLevels, const TextureSampling& sampling);
        VulkanImage m_image;
        VkDevice m_device = VK_NULL_HANDLE;
        VkSampler m_sampler = VK_NULL_HANDLE;
    };
}
