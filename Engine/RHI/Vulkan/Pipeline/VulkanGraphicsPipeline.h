#pragma once

#include <vulkan/vulkan.h>

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
            VkShaderModule fragmentShader
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