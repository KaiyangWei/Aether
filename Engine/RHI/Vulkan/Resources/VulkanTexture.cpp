#include "VulkanTexture.h"

namespace Aether
{
    VulkanTexture::~VulkanTexture()
    {
        Shutdown();
    }

    VkResult VulkanTexture::Initialize(VkDevice device, const VulkanMemoryManager& memory, VkExtent2D extent,
                                      VkFormat format, uint32_t mipLevels, const TextureSampling& sampling)
    {
        auto result = m_image.Initialize(device, memory, extent, format,
            VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
            (mipLevels > 1 ? VK_IMAGE_USAGE_TRANSFER_SRC_BIT : 0), VK_IMAGE_ASPECT_COLOR_BIT, mipLevels);
        if (result != VK_SUCCESS)
            return result;

        VkSamplerCreateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
        info.magFilter = sampling.magFilter;
        info.minFilter = sampling.minFilter;
        info.mipmapMode = sampling.mipMode;
        info.addressModeU = sampling.wrapU;
        info.addressModeV = sampling.wrapV;
        info.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
        info.maxLod = static_cast<float>(mipLevels - 1);
        result = vkCreateSampler(device, &info, nullptr, &m_sampler);
        if (result != VK_SUCCESS)
            m_image.Shutdown();
        else
            m_device = device;
        return result;
    }

    void VulkanTexture::Shutdown() noexcept
    {
        if (m_sampler)
            vkDestroySampler(m_device, m_sampler, nullptr);
        m_sampler = VK_NULL_HANDLE;
        m_device = VK_NULL_HANDLE;
        m_image.Shutdown();
    }
}
