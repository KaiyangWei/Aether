#include "VulkanRenderer.h"
#include "VulkanContext.h"
#include <RHI/Vulkan/Assets/VulkanMaterialSet.h>
#include <RHI/Vulkan/Pipeline/VulkanShader.h>
#include <Renderer/Vertex.h>

namespace Aether
{
    bool VulkanRenderer::CreatePbrPipelines(const VulkanContext& context)
    {
        const auto device = context.GetDevice();
        const std::string folder = AETHER_SHADER_DIR;
        VulkanShader vertex, pixel;
        if (!vertex.Initialize(device, folder + "/pbr.vert.spv") ||
            !pixel.Initialize(device, folder + "/pbr.frag.spv"))
            return false;

        VkDescriptorSetLayout layout = VK_NULL_HANDLE;
        if (VulkanMaterialSet::CreateLayout(device, layout) != VK_SUCCESS)
            return false;

        const VkVertexInputBindingDescription binding{0, sizeof(Vertex), VK_VERTEX_INPUT_RATE_VERTEX};
        const VkVertexInputAttributeDescription attributes[] = {
            {0, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(Vertex, position)},
            {1, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(Vertex, color)},
            {2, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(Vertex, normal)},
            {3, 0, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(Vertex, tangent)},
            {4, 0, VK_FORMAT_R32_SFLOAT, offsetof(Vertex, colorAlpha)},
            {5, 0, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(Vertex, materialUv)},
            {6, 0, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(Vertex, materialUv) + 2 * sizeof(glm::vec2)},
            {7, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(Vertex, materialUv) + 4 * sizeof(glm::vec2)}
        };
        VkPipelineVertexInputStateCreateInfo inputs{};
        inputs.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
        inputs.vertexBindingDescriptionCount = 1;
        inputs.pVertexBindingDescriptions = &binding;
        inputs.vertexAttributeDescriptionCount = static_cast<uint32_t>(std::size(attributes));
        inputs.pVertexAttributeDescriptions = attributes;
        VkDescriptorSetLayout environmentLayout{};
        if (VulkanEnvironment::CreateLayout(device,environmentLayout)!=VK_SUCCESS)
        {
            vkDestroyDescriptorSetLayout(device,layout,nullptr);
            return false;
        }
        const auto create = [&](VulkanGraphicsPipeline& pipeline, bool blend) {
            return pipeline.Initialize(device, VK_FORMAT_R16G16B16A16_SFLOAT, vertex.GetShaderModule(),
                pixel.GetShaderModule(), inputs, sizeof(PbrDrawConstants), m_depthAttachment.GetFormat(),
                layout, blend, environmentLayout, VK_SHADER_STAGE_VERTEX_BIT, true,
                m_shadowPass.GetDescriptorSetLayout());
        };
        const bool success = create(m_pbrOpaque, false) && create(m_pbrBlend, true);
        vkDestroyDescriptorSetLayout(device, layout, nullptr);
        vkDestroyDescriptorSetLayout(device,environmentLayout,nullptr);
        return success;
    }
}
