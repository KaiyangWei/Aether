#include <RHI/Vulkan/Pipeline/VulkanGraphicsPipeline.h>

#include <iostream>

namespace Aether
{
    VulkanGraphicsPipeline::~VulkanGraphicsPipeline()
    {
        Shutdown();
    }

    bool VulkanGraphicsPipeline::Initialize(
        VkDevice device,
        VkFormat colorFormat,
        VkShaderModule vertexShader,
        VkShaderModule fragmentShader)
    {
        if (device == VK_NULL_HANDLE ||
            vertexShader == VK_NULL_HANDLE ||
            fragmentShader == VK_NULL_HANDLE ||
            colorFormat == VK_FORMAT_UNDEFINED)
        {
            std::cerr << "VulkanGraphicsPipeline: Invalid initialization parameters.\n";
            return false;
        }

        if (m_pipeline != VK_NULL_HANDLE ||
            m_pipelineLayout != VK_NULL_HANDLE)
        {
            std::cerr << "VulkanGraphicsPipeline: Already initialized.\n";
            return false;
        }

        // ============================================================
        // 1. Pipeline Layout
        // ============================================================

        VkPipelineLayoutCreateInfo pipelineLayoutInfo{};
        pipelineLayoutInfo.sType =
            VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;

        VkPipelineLayout pipelineLayout = VK_NULL_HANDLE;

        VkResult result = vkCreatePipelineLayout(
            device,
            &pipelineLayoutInfo,
            nullptr,
            &pipelineLayout
        );

        if (result != VK_SUCCESS)
        {
            std::cerr << "VulkanGraphicsPipeline: Failed to create Pipeline Layout. "
                << "VkResult: " << result << '\n';
            return false;
        }

        // ============================================================
        // 2. Shader Stages
        // ============================================================

        VkPipelineShaderStageCreateInfo shaderStages[2]{};

        shaderStages[0].sType =
            VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        shaderStages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
        shaderStages[0].module = vertexShader;
        shaderStages[0].pName = "main";

        shaderStages[1].sType =
            VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        shaderStages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
        shaderStages[1].module = fragmentShader;
        shaderStages[1].pName = "main";

        // ============================================================
        // 3. Vertex Input
        // ============================================================

        VkPipelineVertexInputStateCreateInfo vertexInputInfo{};
        vertexInputInfo.sType =
            VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;

        // No Vertex Buffer for now.
        vertexInputInfo.vertexBindingDescriptionCount = 0;
        vertexInputInfo.vertexAttributeDescriptionCount = 0;

        // ============================================================
        // 4. Input Assembly
        // ============================================================

        VkPipelineInputAssemblyStateCreateInfo inputAssemblyInfo{};
        inputAssemblyInfo.sType =
            VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
        inputAssemblyInfo.topology =
            VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        inputAssemblyInfo.primitiveRestartEnable = VK_FALSE;

        // ============================================================
        // 5. Viewport & Scissor
        // ============================================================

        VkPipelineViewportStateCreateInfo viewportInfo{};
        viewportInfo.sType =
            VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
        viewportInfo.viewportCount = 1;
        viewportInfo.scissorCount = 1;

        // Viewport and Scissor will be set dynamically.

        // ============================================================
        // 6. Rasterization
        // ============================================================

        VkPipelineRasterizationStateCreateInfo rasterizationInfo{};
        rasterizationInfo.sType =
            VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
        rasterizationInfo.depthClampEnable = VK_FALSE;
        rasterizationInfo.rasterizerDiscardEnable = VK_FALSE;
        rasterizationInfo.polygonMode = VK_POLYGON_MODE_FILL;
        rasterizationInfo.cullMode = VK_CULL_MODE_NONE;
        rasterizationInfo.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
        rasterizationInfo.depthBiasEnable = VK_FALSE;
        rasterizationInfo.lineWidth = 1.0f;

        // ============================================================
        // 7. Multisampling
        // ============================================================

        VkPipelineMultisampleStateCreateInfo multisampleInfo{};
        multisampleInfo.sType =
            VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
        multisampleInfo.rasterizationSamples =
            VK_SAMPLE_COUNT_1_BIT;
        multisampleInfo.sampleShadingEnable = VK_FALSE;

        // ============================================================
        // 8. Color Blending
        // ============================================================

        VkPipelineColorBlendAttachmentState colorBlendAttachment{};
        colorBlendAttachment.blendEnable = VK_FALSE;
        colorBlendAttachment.colorWriteMask =
            VK_COLOR_COMPONENT_R_BIT |
            VK_COLOR_COMPONENT_G_BIT |
            VK_COLOR_COMPONENT_B_BIT |
            VK_COLOR_COMPONENT_A_BIT;

        VkPipelineColorBlendStateCreateInfo colorBlendInfo{};
        colorBlendInfo.sType =
            VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
        colorBlendInfo.logicOpEnable = VK_FALSE;
        colorBlendInfo.attachmentCount = 1;
        colorBlendInfo.pAttachments = &colorBlendAttachment;

        // ============================================================
        // 9. Dynamic State
        // ============================================================

        VkDynamicState dynamicStates[] =
        {
            VK_DYNAMIC_STATE_VIEWPORT,
            VK_DYNAMIC_STATE_SCISSOR
        };

        VkPipelineDynamicStateCreateInfo dynamicStateInfo{};
        dynamicStateInfo.sType =
            VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
        dynamicStateInfo.dynamicStateCount = 2;
        dynamicStateInfo.pDynamicStates = dynamicStates;

        // ============================================================
        // 10. Dynamic Rendering
        // ============================================================

        VkPipelineRenderingCreateInfo renderingInfo{};
        renderingInfo.sType =
            VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
        renderingInfo.colorAttachmentCount = 1;
        renderingInfo.pColorAttachmentFormats = &colorFormat;

        // ============================================================
        // 11. Graphics Pipeline
        // ============================================================

        VkGraphicsPipelineCreateInfo pipelineInfo{};
        pipelineInfo.sType =
            VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
        pipelineInfo.pNext = &renderingInfo;

        pipelineInfo.stageCount = 2;
        pipelineInfo.pStages = shaderStages;

        pipelineInfo.pVertexInputState = &vertexInputInfo;
        pipelineInfo.pInputAssemblyState = &inputAssemblyInfo;
        pipelineInfo.pViewportState = &viewportInfo;
        pipelineInfo.pRasterizationState = &rasterizationInfo;
        pipelineInfo.pMultisampleState = &multisampleInfo;
        pipelineInfo.pColorBlendState = &colorBlendInfo;
        pipelineInfo.pDynamicState = &dynamicStateInfo;

        pipelineInfo.layout = pipelineLayout;
        pipelineInfo.renderPass = VK_NULL_HANDLE;
        pipelineInfo.subpass = 0;

        VkPipeline pipeline = VK_NULL_HANDLE;

        result = vkCreateGraphicsPipelines(
            device,
            VK_NULL_HANDLE,
            1,
            &pipelineInfo,
            nullptr,
            &pipeline
        );

        if (result != VK_SUCCESS)
        {
            std::cerr << "VulkanGraphicsPipeline: Failed to create Graphics Pipeline. "
                << "VkResult: " << result << '\n';

            vkDestroyPipelineLayout(
                device,
                pipelineLayout,
                nullptr
            );

            return false;
        }

        // ============================================================
        // 12. Commit Resources
        // ============================================================

        m_device = device;
        m_pipelineLayout = pipelineLayout;
        m_pipeline = pipeline;

        return true;
    }

    void VulkanGraphicsPipeline::Shutdown()
    {
        if (m_pipeline != VK_NULL_HANDLE)
        {
            vkDestroyPipeline(
                m_device,
                m_pipeline,
                nullptr
            );

            m_pipeline = VK_NULL_HANDLE;
        }

        if (m_pipelineLayout != VK_NULL_HANDLE)
        {
            vkDestroyPipelineLayout(
                m_device,
                m_pipelineLayout,
                nullptr
            );

            m_pipelineLayout = VK_NULL_HANDLE;
        }

        m_device = VK_NULL_HANDLE;
    }
}