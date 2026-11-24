#include "VulkanShadowMap.h"

#include <RHI/Vulkan/VulkanMemoryManager.h>

namespace Aether
{
    VulkanShadowMap::~VulkanShadowMap()
    {
        Shutdown();
    }

    VkFormat VulkanShadowMap::SelectDepthFormat(VkPhysicalDevice physicalDevice, bool& supportsLinearFilter)
    {
        supportsLinearFilter = false;
        if (physicalDevice == VK_NULL_HANDLE)
            return VK_FORMAT_UNDEFINED;

        constexpr VkFormat candidates[] = {
            VK_FORMAT_D32_SFLOAT,
            VK_FORMAT_D24_UNORM_S8_UINT,
            VK_FORMAT_D16_UNORM
        };
        constexpr VkFormatFeatureFlags requiredFeatures =
            VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT;

        for (VkFormat candidate : candidates)
        {
            VkFormatProperties props{};
            vkGetPhysicalDeviceFormatProperties(physicalDevice, candidate, &props);
            if ((props.optimalTilingFeatures & requiredFeatures) == requiredFeatures)
            {
                supportsLinearFilter =
                    (props.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT) != 0;
                return candidate;
            }
        }
        return VK_FORMAT_UNDEFINED;
    }

    VkResult VulkanShadowMap::Initialize(
        VkPhysicalDevice physicalDevice,
        VkDevice device,
        const VulkanMemoryManager& memory,
        uint32_t resolution)
    {
        if (m_device != VK_NULL_HANDLE || physicalDevice == VK_NULL_HANDLE ||
            device == VK_NULL_HANDLE || resolution == 0)
            return VK_ERROR_INITIALIZATION_FAILED;

        bool supportsLinear = false;
        const VkFormat format = SelectDepthFormat(physicalDevice, supportsLinear);
        if (format == VK_FORMAT_UNDEFINED)
            return VK_ERROR_FORMAT_NOT_SUPPORTED;

        constexpr VkImageUsageFlags usage =
            VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
        VkImageFormatProperties imageProps{};
        VkResult result = vkGetPhysicalDeviceImageFormatProperties(
            physicalDevice, format, VK_IMAGE_TYPE_2D, VK_IMAGE_TILING_OPTIMAL, usage, 0, &imageProps);
        if (result != VK_SUCCESS)
            return result;
        if (resolution > imageProps.maxExtent.width || resolution > imageProps.maxExtent.height)
            return VK_ERROR_FORMAT_NOT_SUPPORTED;

        result = m_image.Initialize(
            device, memory, {resolution, resolution}, format, usage, VK_IMAGE_ASPECT_DEPTH_BIT);
        if (result != VK_SUCCESS)
            return result;

        m_device = device;
        m_supportsLinearFilter = supportsLinear;

        VkSamplerCreateInfo compareInfo{};
        compareInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
        compareInfo.magFilter = supportsLinear ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
        compareInfo.minFilter = supportsLinear ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
        compareInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
        compareInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
        compareInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
        compareInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
        compareInfo.borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE;
        compareInfo.compareEnable = VK_TRUE;
        compareInfo.compareOp = VK_COMPARE_OP_LESS_OR_EQUAL;
        compareInfo.maxLod = 0.0f;

        result = vkCreateSampler(device, &compareInfo, nullptr, &m_compareSampler);
        if (result != VK_SUCCESS)
        {
            Shutdown();
            return result;
        }

        VkSamplerCreateInfo debugInfo{};
        debugInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
        debugInfo.magFilter = VK_FILTER_NEAREST;
        debugInfo.minFilter = VK_FILTER_NEAREST;
        debugInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
        debugInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        debugInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        debugInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        debugInfo.compareEnable = VK_FALSE;
        debugInfo.maxLod = 0.0f;

        result = vkCreateSampler(device, &debugInfo, nullptr, &m_debugSampler);
        if (result != VK_SUCCESS)
        {
            Shutdown();
            return result;
        }

        m_layout = VK_IMAGE_LAYOUT_UNDEFINED;
        return VK_SUCCESS;
    }

    void VulkanShadowMap::Shutdown() noexcept
    {
        if (m_debugSampler != VK_NULL_HANDLE)
        {
            vkDestroySampler(m_device, m_debugSampler, nullptr);
            m_debugSampler = VK_NULL_HANDLE;
        }
        if (m_compareSampler != VK_NULL_HANDLE)
        {
            vkDestroySampler(m_device, m_compareSampler, nullptr);
            m_compareSampler = VK_NULL_HANDLE;
        }
        m_image.Shutdown();
        m_device = VK_NULL_HANDLE;
        m_layout = VK_IMAGE_LAYOUT_UNDEFINED;
        m_supportsLinearFilter = false;
    }
}
