#pragma once

#include <vulkan/vulkan.h>
#include <cstdint>
#include <span>

namespace Aether
{
    class VulkanGraphicsPipeline
    {
    public:
        VulkanGraphicsPipeline() = default;
        ~VulkanGraphicsPipeline();

        VulkanGraphicsPipeline(const VulkanGraphicsPipeline&) = delete;
        VulkanGraphicsPipeline& operator=(const VulkanGraphicsPipeline&) = delete;

        bool Initialize(
            VkDevice device,
            VkFormat colorFormat,
            VkShaderModule vertexShader,
            VkShaderModule fragmentShader,
            const VkPipelineVertexInputStateCreateInfo& vertexInputInfo,
            uint32_t pushConstantSize = 0,
            VkFormat depthFormat = VK_FORMAT_UNDEFINED,
            VkDescriptorSetLayout materialLayout = VK_NULL_HANDLE,
            bool alphaBlend = false,
            VkDescriptorSetLayout environmentLayout = VK_NULL_HANDLE,
            VkShaderStageFlags pushStages = VK_SHADER_STAGE_VERTEX_BIT,
            bool depthTest = true,
            VkDescriptorSetLayout shadowLayout = VK_NULL_HANDLE,
            bool depthBias = false,
            bool depthWrite = true,
            VkCompareOp depthCompareOp = VK_COMPARE_OP_LESS
        );

        bool InitializeMrt(
            VkDevice device,
            std::span<const VkFormat> colorFormats,
            VkShaderModule vertexShader,
            VkShaderModule fragmentShader,
            const VkPipelineVertexInputStateCreateInfo& vertexInputInfo,
            uint32_t pushConstantSize = 0,
            VkFormat depthFormat = VK_FORMAT_UNDEFINED,
            std::span<const VkDescriptorSetLayout> setLayouts = {},
            VkShaderStageFlags pushStages = VK_SHADER_STAGE_VERTEX_BIT,
            bool depthTest = true,
            bool depthWrite = true,
            VkCompareOp depthCompareOp = VK_COMPARE_OP_LESS
        );

        void Shutdown();

        VkPipeline GetPipeline() const
        {
            return m_pipeline;
        }

        VkPipelineLayout GetPipelineLayout() const
        {
            return m_pipelineLayout;
        }

    private:
        VkDevice m_device = VK_NULL_HANDLE;

        VkPipelineLayout m_pipelineLayout = VK_NULL_HANDLE;
        VkPipeline m_pipeline = VK_NULL_HANDLE;
    };
}
