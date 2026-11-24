#include "VulkanRenderer.h"
#include "VulkanContext.h"
#include "VulkanCommandManager.h"
#include <RHI/Vulkan/Assets/VulkanEnvironment.h>
#include <RHI/Vulkan/Resources/VulkanBuffer.h>
#include <array>
#include <stdexcept>

namespace Aether
{
VulkanEnvironment::~VulkanEnvironment()
{
    if (pool)
        vkDestroyDescriptorPool(device, pool, nullptr);
    if (layout)
        vkDestroyDescriptorSetLayout(device, layout, nullptr);
    if (sampler)
        vkDestroySampler(device, sampler, nullptr);
}

VkResult VulkanRenderer::EnvironmentCommands(const std::function<void(VkCommandBuffer)> &record)
{
    const auto device = m_context->GetDevice();
    VkCommandBufferAllocateInfo alloc{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    alloc.commandPool = m_commandManager->GetCommandPool();
    alloc.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    alloc.commandBufferCount = 1;
    VkCommandBuffer cmd{};
    auto status = vkAllocateCommandBuffers(device, &alloc, &cmd);
    if (status != VK_SUCCESS)
        return status;
    VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    status = vkBeginCommandBuffer(cmd, &begin);
    if (status == VK_SUCCESS)
    {
        record(cmd);
        status = vkEndCommandBuffer(cmd);
    }
    if (status == VK_SUCCESS)
    {
        VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        submit.commandBufferCount = 1;
        submit.pCommandBuffers = &cmd;
        status = vkQueueSubmit(m_context->GetGraphicsQueue(), 1, &submit, VK_NULL_HANDLE);
        if (status == VK_SUCCESS)
            status = vkQueueWaitIdle(m_context->GetGraphicsQueue());
    }
    vkFreeCommandBuffers(device, alloc.commandPool, 1, &cmd);
    return status;
}

void VulkanRenderer::CopyEnvironmentMap(VkCommandBuffer cmd, const VulkanImage &image, VkBuffer buffer,
                                        uint32_t layers, VkDeviceSize offset)
{
    VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.image = image.GetImage();
    barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, image.GetMipLevels(), 0, layers};
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0,
                         nullptr, 1, &barrier);
    auto extent = image.GetExtent();
    for (uint32_t mip = 0; mip < image.GetMipLevels(); ++mip)
    {
        VkBufferImageCopy copy{};
        copy.bufferOffset = offset;
        copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, mip, 0, layers};
        copy.imageExtent = {extent.width, extent.height, 1};
        vkCmdCopyBufferToImage(cmd, buffer, image.GetImage(), barrier.newLayout, 1, &copy);
        offset += VkDeviceSize(extent.width) * extent.height * layers * sizeof(glm::vec4);
        extent.width = std::max(1u, extent.width / 2);
        extent.height = std::max(1u, extent.height / 2);
    }
    barrier.oldLayout = barrier.newLayout;
    barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0,
                         nullptr, 1, &barrier);
}

VkResult VulkanRenderer::UploadEnvironment(VulkanEnvironment &env, const HdrImage &src)
{
    if (env.device || !src.width || !src.height || src.pixels.size() != uint64_t(src.width) * src.height)
        return VK_ERROR_INITIALIZATION_FAILED;
    env.device = m_context->GetDevice();
    constexpr auto usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    constexpr auto format = VK_FORMAT_R32G32B32A32_SFLOAT;
    constexpr unsigned side = 128;
    VkFormatProperties props{};
    vkGetPhysicalDeviceFormatProperties(m_context->GetPhysicalDevice(), format, &props);
    if (!(props.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT))
        return VK_ERROR_FORMAT_NOT_SUPPORTED;
    VkImageFormatProperties supported{};
    auto result = vkGetPhysicalDeviceImageFormatProperties(
        m_context->GetPhysicalDevice(), format, VK_IMAGE_TYPE_2D, VK_IMAGE_TILING_OPTIMAL, usage, 0, &supported);
    if (result != VK_SUCCESS || src.width > supported.maxExtent.width || src.height > supported.maxExtent.height)
        return VK_ERROR_FORMAT_NOT_SUPPORTED;
    result = env.panorama.Initialize(env.device, *m_memoryManager, {src.width, src.height}, format, usage,
                                     VK_IMAGE_ASPECT_COLOR_BIT);
    if (result != VK_SUCCESS)
        return result;
    result = env.cube.Initialize(env.device, *m_memoryManager, {side, side}, format, usage,
                                 VK_IMAGE_ASPECT_COLOR_BIT, 1, true);
    if (result != VK_SUCCESS)
        return result;
    VkSamplerCreateInfo sampler{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    sampler.minFilter = sampler.magFilter = VK_FILTER_LINEAR;
    sampler.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    sampler.addressModeU = sampler.addressModeV = sampler.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sampler.maxLod = VK_LOD_CLAMP_NONE;
    result = vkCreateSampler(env.device, &sampler, nullptr, &env.sampler);
    if (result != VK_SUCCESS)
        return result;
    std::vector<glm::vec4> pixels = src.pixels;
    for (unsigned face = 0; face < 6; ++face)
    {
        auto data = PanoramaFace(src, face, side);
        pixels.insert(pixels.end(), data.pixels.begin(), data.pixels.end());
    }
    VulkanBuffer staging;
    const auto size = pixels.size() * sizeof(glm::vec4);
    result = staging.Initialize(env.device, *m_memoryManager, size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                                VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT);
    if (result == VK_SUCCESS)
        result = staging.UploadData(pixels.data(), size);
    if (result != VK_SUCCESS)
        return result;
    return EnvironmentCommands([&](VkCommandBuffer cmd) {
        CopyEnvironmentMap(cmd, env.panorama, staging.GetBuffer(), 1, 0);
        CopyEnvironmentMap(cmd, env.cube, staging.GetBuffer(), 6, src.pixels.size() * sizeof(glm::vec4));
    });
}
} // namespace Aether
