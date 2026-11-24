#include "VulkanRenderer.h"
#include "VulkanContext.h"
#include "Pipeline/VulkanShader.h"

namespace Aether
{
void VulkanRenderer::ReleaseHdr(VkDevice device)
{
    m_skyboxPipeline.Shutdown();
    m_tonePipeline.Shutdown();
    vkDestroyDescriptorPool(device, m_hdrPool, nullptr);
    vkDestroyDescriptorSetLayout(device, m_hdrLayout, nullptr);
    m_hdrPool = {};
    m_hdrLayout = {};
    m_hdrSet = {};
    m_hdrColor.Shutdown();
}

bool VulkanRenderer::CreateSkyboxPipeline(const VulkanContext &context)
{
    const auto device = context.GetDevice();
    const std::string dir = AETHER_SHADER_DIR;
    VulkanShader vert, frag;
    if (!vert.Initialize(device, dir + "/skybox.vert.spv") ||
        !frag.Initialize(device, dir + "/skybox.frag.spv"))
        return false;

    VkDescriptorSetLayout environmentLayout{};
    if (VulkanEnvironment::CreateLayout(device, environmentLayout) != VK_SUCCESS)
        return false;

    VkPipelineVertexInputStateCreateInfo inputs{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    const bool created = m_skyboxPipeline.Initialize(
        device, m_hdrColor.GetFormat(), vert.GetShaderModule(), frag.GetShaderModule(), inputs,
        sizeof(glm::mat4), m_depthAttachment.GetFormat(), environmentLayout, false, VK_NULL_HANDLE,
        VK_SHADER_STAGE_VERTEX_BIT, false);
    vkDestroyDescriptorSetLayout(device, environmentLayout, nullptr);
    return created;
}

void VulkanRenderer::DrawSkybox(VkCommandBuffer cmd, VkDescriptorSet environment, const glm::mat4& clipToWorldDirection)
{
    const auto layout = m_skyboxPipeline.GetPipelineLayout();
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_skyboxPipeline.GetPipeline());
    vkCmdSetCullMode(cmd, VK_CULL_MODE_NONE);
    vkCmdSetFrontFace(cmd, VK_FRONT_FACE_COUNTER_CLOCKWISE);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 0, 1, &environment, 0, nullptr);
    vkCmdPushConstants(cmd, layout, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(clipToWorldDirection), &clipToWorldDirection);
    vkCmdDraw(cmd, 3, 1, 0, 0);
}

bool VulkanRenderer::CreateHdrTarget(const VulkanContext &context, const VulkanMemoryManager &memory, VkExtent2D extent)
{
    const auto device = context.GetDevice();
    const VkExtent2D targetExtent = (extent.width > 0 && extent.height > 0) ? extent : context.GetSwapchainExtent();
    m_hdrColor.Shutdown();
    if (m_hdrColor.Initialize(device, memory, targetExtent, VK_FORMAT_R16G16B16A16_SFLOAT,
                              VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                              VK_IMAGE_ASPECT_COLOR_BIT) != VK_SUCCESS)
        return false;
    if (!m_hdrSet)
    {
        VkDescriptorSetLayoutBinding binding{0, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, VK_SHADER_STAGE_FRAGMENT_BIT,
                                             nullptr};
        VkDescriptorSetLayoutCreateInfo layout{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        layout.bindingCount = 1;
        layout.pBindings = &binding;
        if (vkCreateDescriptorSetLayout(device, &layout, nullptr, &m_hdrLayout) != VK_SUCCESS)
            return false;
        VkDescriptorPoolSize size{VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1};
        VkDescriptorPoolCreateInfo pool{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        pool.maxSets = 1;
        pool.poolSizeCount = 1;
        pool.pPoolSizes = &size;
        if (vkCreateDescriptorPool(device, &pool, nullptr, &m_hdrPool) != VK_SUCCESS)
            return false;
        VkDescriptorSetAllocateInfo alloc{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        alloc.descriptorSetCount = 1;
        alloc.descriptorPool = m_hdrPool;
        alloc.pSetLayouts = &m_hdrLayout;
        if (vkAllocateDescriptorSets(device, &alloc, &m_hdrSet) != VK_SUCCESS)
            return false;
    }
    VkDescriptorImageInfo image{VK_NULL_HANDLE, m_hdrColor.GetView(), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, m_hdrSet, 0, 0, 1,
                               VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,       &image};
    vkUpdateDescriptorSets(device, 1, &write, 0, nullptr);
    return true;
}

bool VulkanRenderer::CreateTonePipeline(const VulkanContext &context)
{
    VulkanShader vert, frag;
    const std::string dir = AETHER_SHADER_DIR;
    const auto device = context.GetDevice();
    VkPipelineVertexInputStateCreateInfo inputs{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    return vert.Initialize(device, dir + "/tonemap.vert.spv") &&
           frag.Initialize(device, dir + "/tonemap.frag.spv") &&
           m_tonePipeline.Initialize(device, context.GetSwapchainImageFormat(), vert.GetShaderModule(),
                                     frag.GetShaderModule(), inputs, sizeof(float), VK_FORMAT_UNDEFINED,
                                     m_hdrLayout, false, VK_NULL_HANDLE, VK_SHADER_STAGE_FRAGMENT_BIT);
}

void VulkanRenderer::ToneMap(
    VkCommandBuffer cmd, VkImageView target, VkExtent2D extent, float exposure, VkDescriptorSet hdrInputSet)
{
    VkRenderingAttachmentInfo color{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    color.imageView = target;
    color.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    color.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    VkRenderingInfo render{VK_STRUCTURE_TYPE_RENDERING_INFO};
    render.layerCount = 1;
    render.renderArea.extent = extent;
    render.colorAttachmentCount = 1;
    render.pColorAttachments = &color;
    vkCmdBeginRendering(cmd, &render);
    const auto layout = m_tonePipeline.GetPipelineLayout();
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_tonePipeline.GetPipeline());
    vkCmdSetCullMode(cmd, VK_CULL_MODE_NONE);
    vkCmdSetFrontFace(cmd, VK_FRONT_FACE_COUNTER_CLOCKWISE);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 0, 1, &hdrInputSet, 0, nullptr);
    vkCmdPushConstants(cmd, layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(float), &exposure);
    vkCmdDraw(cmd, 3, 1, 0, 0);
    vkCmdEndRendering(cmd);
}
} // namespace Aether
