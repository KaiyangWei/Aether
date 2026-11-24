#include "VulkanRenderer.h"
#include "VulkanCommandManager.h"
#include "VulkanContext.h"
#include <RHI/Vulkan/Resources/VulkanBuffer.h>

#include <algorithm>
#include <bit>
#include <limits>

namespace Aether
{
    VkResult VulkanRenderer::UploadTexture(VulkanTexture& texture, VkExtent2D extent, bool srgb,
                                           std::span<const std::byte> rgba, const TextureSampling& sampling)
    {
        return UploadTexture(texture, extent, srgb, rgba, sampling, false);
    }

    VkResult VulkanRenderer::UploadTexture(VulkanTexture& texture, VkExtent2D extent, bool srgb,
                                           std::span<const std::byte> rgba, const TextureSampling& sampling,
                                           bool failAfterCreate)
    {
        if (!m_context || texture.GetImage().GetImage() || !extent.width || !extent.height ||
            static_cast<uint64_t>(extent.width) * extent.height > std::numeric_limits<size_t>::max() / 4 ||
            rgba.size() != static_cast<size_t>(extent.width) * extent.height * 4)
            return VK_ERROR_INITIALIZATION_FAILED;

        const VkDevice device = m_context->GetDevice();
        const auto format = srgb ? VK_FORMAT_R8G8B8A8_SRGB : VK_FORMAT_R8G8B8A8_UNORM;
        const uint32_t mips = sampling.mipmapped ? std::bit_width(std::max(extent.width, extent.height)) : 1;
        VkFormatProperties properties{};
        vkGetPhysicalDeviceFormatProperties(m_context->GetPhysicalDevice(), format, &properties);
        if (mips > 1)
        {
            constexpr auto required = VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT |
                VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT;
            if ((properties.optimalTilingFeatures & required) != required)
                return VK_ERROR_FORMAT_NOT_SUPPORTED;
        }

        VkImageFormatProperties imageProperties{};
        const auto usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
            (mips > 1 ? VK_IMAGE_USAGE_TRANSFER_SRC_BIT : 0);
        const auto supported = vkGetPhysicalDeviceImageFormatProperties(m_context->GetPhysicalDevice(),
            format, VK_IMAGE_TYPE_2D, VK_IMAGE_TILING_OPTIMAL, usage, 0, &imageProperties);
        if (supported != VK_SUCCESS)
            return supported;
        if (extent.width > imageProperties.maxExtent.width || extent.height > imageProperties.maxExtent.height ||
            mips > imageProperties.maxMipLevels)
            return VK_ERROR_FORMAT_NOT_SUPPORTED;

        VulkanBuffer staging;
        auto result = staging.Initialize(device, *m_memoryManager, rgba.size(), VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT, VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        if (result == VK_SUCCESS)
            result = staging.UploadData(rgba.data(), rgba.size());
        if (result == VK_SUCCESS)
            result = texture.Initialize(device, *m_memoryManager, extent, format, mips, sampling);
        if (result != VK_SUCCESS)
            return result;
        if (failAfterCreate)
        {
            texture.Shutdown();
            return VK_ERROR_UNKNOWN;
        }

        VkCommandBufferAllocateInfo allocate{};
        allocate.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        allocate.commandPool = m_commandManager->GetCommandPool();
        allocate.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        allocate.commandBufferCount = 1;
        VkCommandBuffer commands = VK_NULL_HANDLE;
        result = vkAllocateCommandBuffers(device, &allocate, &commands);
        if (result == VK_SUCCESS)
        {
            VkCommandBufferBeginInfo begin{};
            begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
            begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
            result = vkBeginCommandBuffer(commands, &begin);
        }
        if (result == VK_SUCCESS)
        {
            const auto image = texture.GetImage().GetImage();
            const auto transition = [&](uint32_t base, uint32_t count, VkImageLayout oldLayout, VkImageLayout newLayout,
                                        VkPipelineStageFlags src, VkPipelineStageFlags dst,
                                        VkAccessFlags srcAccess, VkAccessFlags dstAccess) {
                VkImageMemoryBarrier barrier{};
                barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
                barrier.srcAccessMask = srcAccess;
                barrier.dstAccessMask = dstAccess;
                barrier.oldLayout = oldLayout;
                barrier.newLayout = newLayout;
                barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                barrier.image = image;
                barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, base, count, 0, 1};
                vkCmdPipelineBarrier(commands, src, dst, 0, 0, nullptr, 0, nullptr, 1, &barrier);
            };

            transition(0, mips, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, VK_ACCESS_TRANSFER_WRITE_BIT);
            VkBufferImageCopy copy{};
            copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            copy.imageExtent = {extent.width, extent.height, 1};
            vkCmdCopyBufferToImage(commands, staging.GetBuffer(), image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);

            int32_t width = static_cast<int32_t>(extent.width);
            int32_t height = static_cast<int32_t>(extent.height);
            for (uint32_t mip = 1; mip < mips; ++mip)
            {
                transition(mip - 1, 1, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                    VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                    VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT);

                VkImageBlit blit{};
                blit.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, mip - 1, 0, 1};
                blit.srcOffsets[1] = {width, height, 1};
                width = std::max(width / 2, 1);
                height = std::max(height / 2, 1);
                blit.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, mip, 0, 1};
                blit.dstOffsets[1] = {width, height, 1};
                vkCmdBlitImage(commands, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, image,
                    VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, VK_FILTER_LINEAR);

                transition(mip - 1, 1, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                    VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                    VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT);
            }
            transition(mips - 1, 1, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT);
            result = vkEndCommandBuffer(commands);
        }

        if (result == VK_SUCCESS)
        {
            VkSubmitInfo submit{};
            submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
            submit.commandBufferCount = 1;
            submit.pCommandBuffers = &commands;
            const auto queue = m_context->GetGraphicsQueue();
            result = vkQueueSubmit(queue, 1, &submit, VK_NULL_HANDLE);
            if (result == VK_SUCCESS)
                result = vkQueueWaitIdle(queue);
        }
        if (commands)
            vkFreeCommandBuffers(device, allocate.commandPool, 1, &commands);
        if (result != VK_SUCCESS)
            texture.Shutdown();
        return result;
    }
}
