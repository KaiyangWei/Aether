#include <RHI/Vulkan/Pipeline/VulkanGraphicsPipeline.h>
#include <iostream>
#include <vector>

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
        VkShaderModule fragmentShader,
        const VkPipelineVertexInputStateCreateInfo& vertexInputInfo,
        uint32_t pushConstantSize,
        VkFormat depthFormat, VkDescriptorSetLayout materialLayout, bool alphaBlend,
        VkDescriptorSetLayout environmentLayout, VkShaderStageFlags pushStages, bool depthTest,
        VkDescriptorSetLayout shadowLayout, bool depthBias,
        bool depthWrite, VkCompareOp depthCompareOp)
    {
        const bool depthOnly = colorFormat == VK_FORMAT_UNDEFINED;
        if (device == VK_NULL_HANDLE ||
            vertexShader == VK_NULL_HANDLE ||
            (!depthOnly && fragmentShader == VK_NULL_HANDLE) ||
            (depthOnly && depthFormat == VK_FORMAT_UNDEFINED))
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
        // 1. Pipeline Layout (按需绑定 Push Constant Range)
        // ============================================================

        VkPushConstantRange pushConstantRange{};
        pushConstantRange.stageFlags = pushStages;
        pushConstantRange.offset = 0;
        pushConstantRange.size = pushConstantSize;

        VkPipelineLayoutCreateInfo pipelineLayoutInfo{};
        pipelineLayoutInfo.sType =
            VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;

        const VkDescriptorSetLayout layouts[] = { materialLayout, environmentLayout, shadowLayout };
        uint32_t setLayoutCount = 0;
        if (materialLayout)
        {
            setLayoutCount = 1;
            if (environmentLayout)
            {
                setLayoutCount = 2;
                if (shadowLayout)
                    setLayoutCount = 3;
            }
        }
        if (setLayoutCount > 0)
        {
            pipelineLayoutInfo.setLayoutCount = setLayoutCount;
            pipelineLayoutInfo.pSetLayouts = layouts;
        }
        if (pushConstantSize > 0)
        {
            pipelineLayoutInfo.pushConstantRangeCount = 1;
            pipelineLayoutInfo.pPushConstantRanges = &pushConstantRange;
        }

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
        const uint32_t encodeSrgb = colorFormat == VK_FORMAT_B8G8R8A8_SRGB ||
            colorFormat == VK_FORMAT_R8G8B8A8_SRGB ? 0u : 1u;
        const VkSpecializationMapEntry gammaConstant{0, 0, sizeof(encodeSrgb)};
        const VkSpecializationInfo gammaInfo{1, &gammaConstant, sizeof(encodeSrgb), &encodeSrgb};
        if (pushStages == VK_SHADER_STAGE_FRAGMENT_BIT)
            shaderStages[1].pSpecializationInfo = &gammaInfo;

        // ============================================================
        // 3. Input Assembly
        // ============================================================

        VkPipelineInputAssemblyStateCreateInfo inputAssemblyInfo{};
        inputAssemblyInfo.sType =
            VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
        inputAssemblyInfo.topology =
            VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        inputAssemblyInfo.primitiveRestartEnable = VK_FALSE;

        // ============================================================
        // 4. Viewport & Scissor
        // ============================================================

        VkPipelineViewportStateCreateInfo viewportInfo{};
        viewportInfo.sType =
            VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
        viewportInfo.viewportCount = 1;
        viewportInfo.scissorCount = 1;

        // ============================================================
        // 5. Rasterization (开启背面剔除)
        // ============================================================

        VkPipelineRasterizationStateCreateInfo rasterizationInfo{};
        rasterizationInfo.sType =
            VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
        rasterizationInfo.depthClampEnable = VK_FALSE;
        rasterizationInfo.rasterizerDiscardEnable = VK_FALSE;
        rasterizationInfo.polygonMode = VK_POLYGON_MODE_FILL;
        rasterizationInfo.cullMode = VK_CULL_MODE_BACK_BIT;
        rasterizationInfo.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
        rasterizationInfo.depthBiasEnable = depthBias ? VK_TRUE : VK_FALSE;
        rasterizationInfo.lineWidth = 1.0f;

        // ============================================================
        // 6. Multisampling
        // ============================================================

        VkPipelineMultisampleStateCreateInfo multisampleInfo{};
        multisampleInfo.sType =
            VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
        multisampleInfo.rasterizationSamples =
            VK_SAMPLE_COUNT_1_BIT;
        multisampleInfo.sampleShadingEnable = VK_FALSE;

        // ============================================================
        // 7. Color Blending
        // ============================================================

        VkPipelineColorBlendAttachmentState colorBlendAttachment{};
        colorBlendAttachment.blendEnable = alphaBlend ? VK_TRUE : VK_FALSE;
        colorBlendAttachment.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
        colorBlendAttachment.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        colorBlendAttachment.colorBlendOp = VK_BLEND_OP_ADD;
        colorBlendAttachment.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
        colorBlendAttachment.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        colorBlendAttachment.alphaBlendOp = VK_BLEND_OP_ADD;
        colorBlendAttachment.colorWriteMask =
            VK_COLOR_COMPONENT_R_BIT |
            VK_COLOR_COMPONENT_G_BIT |
            VK_COLOR_COMPONENT_B_BIT |
            VK_COLOR_COMPONENT_A_BIT;

        VkPipelineColorBlendStateCreateInfo colorBlendInfo{};
        colorBlendInfo.sType =
            VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
        colorBlendInfo.logicOpEnable = VK_FALSE;
        colorBlendInfo.attachmentCount = depthOnly ? 0u : 1u;
        colorBlendInfo.pAttachments = depthOnly ? nullptr : &colorBlendAttachment;

        // ============================================================
        // 8. Dynamic State
        // ============================================================

        VkDynamicState dynamicStates[] =
        {
            VK_DYNAMIC_STATE_VIEWPORT,
            VK_DYNAMIC_STATE_SCISSOR,
            VK_DYNAMIC_STATE_FRONT_FACE,
            VK_DYNAMIC_STATE_CULL_MODE,
            VK_DYNAMIC_STATE_DEPTH_BIAS
        };

        VkPipelineDynamicStateCreateInfo dynamicStateInfo{};
        dynamicStateInfo.sType =
            VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
        dynamicStateInfo.dynamicStateCount = depthBias ? 5u : 4u;
        dynamicStateInfo.pDynamicStates = dynamicStates;

        // ============================================================
        // 9. Dynamic Rendering
        // ============================================================

        VkPipelineDepthStencilStateCreateInfo depthState{};
        depthState.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
        depthState.depthTestEnable = depthTest && depthFormat != VK_FORMAT_UNDEFINED;
        depthState.depthWriteEnable = depthState.depthTestEnable && depthWrite && !alphaBlend;
        depthState.depthCompareOp = depthCompareOp;

        VkPipelineRenderingCreateInfo renderingInfo{};
        renderingInfo.depthAttachmentFormat = depthFormat;
        renderingInfo.sType =
            VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
        renderingInfo.colorAttachmentCount = depthOnly ? 0u : 1u;
        renderingInfo.pColorAttachmentFormats = depthOnly ? nullptr : &colorFormat;

        // ============================================================
        // 10. Graphics Pipeline
        // ============================================================

        VkGraphicsPipelineCreateInfo pipelineInfo{};
        pipelineInfo.sType =
            VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
        pipelineInfo.pNext = &renderingInfo;

        pipelineInfo.stageCount = fragmentShader != VK_NULL_HANDLE ? 2u : 1u;
        pipelineInfo.pStages = shaderStages;

        pipelineInfo.pVertexInputState = &vertexInputInfo;
        pipelineInfo.pInputAssemblyState = &inputAssemblyInfo;
        pipelineInfo.pViewportState = &viewportInfo;
        pipelineInfo.pRasterizationState = &rasterizationInfo;
        pipelineInfo.pMultisampleState = &multisampleInfo;
        pipelineInfo.pDepthStencilState = depthFormat != VK_FORMAT_UNDEFINED ? &depthState : nullptr;
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
        // 11. Commit Resources
        // ============================================================

        m_device = device;
        m_pipelineLayout = pipelineLayout;
        m_pipeline = pipeline;

        return true;
    }

    bool VulkanGraphicsPipeline::InitializeMrt(
        VkDevice device,
        std::span<const VkFormat> colorFormats,
        VkShaderModule vertexShader,
        VkShaderModule fragmentShader,
        const VkPipelineVertexInputStateCreateInfo& vertexInputInfo,
        uint32_t pushConstantSize,
        VkFormat depthFormat,
        std::span<const VkDescriptorSetLayout> setLayouts,
        VkShaderStageFlags pushStages,
        bool depthTest,
        bool depthWrite,
        VkCompareOp depthCompareOp)
    {
        if (device == VK_NULL_HANDLE || colorFormats.empty() ||
            vertexShader == VK_NULL_HANDLE || fragmentShader == VK_NULL_HANDLE)
        {
            std::cerr << "VulkanGraphicsPipeline::InitializeMrt: Invalid initialization parameters.\n";
            return false;
        }
        if (m_pipeline != VK_NULL_HANDLE || m_pipelineLayout != VK_NULL_HANDLE)
        {
            std::cerr << "VulkanGraphicsPipeline::InitializeMrt: Already initialized.\n";
            return false;
        }

        VkPushConstantRange pushConstantRange{};
        pushConstantRange.stageFlags = pushStages;
        pushConstantRange.offset = 0;
        pushConstantRange.size = pushConstantSize;

        VkPipelineLayoutCreateInfo pipelineLayoutInfo{};
        pipelineLayoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        if (!setLayouts.empty())
        {
            pipelineLayoutInfo.setLayoutCount = static_cast<uint32_t>(setLayouts.size());
            pipelineLayoutInfo.pSetLayouts = setLayouts.data();
        }
        if (pushConstantSize > 0)
        {
            pipelineLayoutInfo.pushConstantRangeCount = 1;
            pipelineLayoutInfo.pPushConstantRanges = &pushConstantRange;
        }

        VkPipelineLayout pipelineLayout = VK_NULL_HANDLE;
        VkResult result = vkCreatePipelineLayout(device, &pipelineLayoutInfo, nullptr, &pipelineLayout);
        if (result != VK_SUCCESS)
        {
            std::cerr << "VulkanGraphicsPipeline::InitializeMrt: Failed to create Pipeline Layout. VkResult: "
                      << result << '\n';
            return false;
        }

        VkPipelineShaderStageCreateInfo shaderStages[2]{};
        shaderStages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        shaderStages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
        shaderStages[0].module = vertexShader;
        shaderStages[0].pName = "main";
        shaderStages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        shaderStages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
        shaderStages[1].module = fragmentShader;
        shaderStages[1].pName = "main";

        VkPipelineInputAssemblyStateCreateInfo inputAssemblyInfo{};
        inputAssemblyInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
        inputAssemblyInfo.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        inputAssemblyInfo.primitiveRestartEnable = VK_FALSE;

        VkPipelineViewportStateCreateInfo viewportInfo{};
        viewportInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
        viewportInfo.viewportCount = 1;
        viewportInfo.scissorCount = 1;

        VkPipelineRasterizationStateCreateInfo rasterizationInfo{};
        rasterizationInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
        rasterizationInfo.depthClampEnable = VK_FALSE;
        rasterizationInfo.rasterizerDiscardEnable = VK_FALSE;
        rasterizationInfo.polygonMode = VK_POLYGON_MODE_FILL;
        rasterizationInfo.cullMode = VK_CULL_MODE_BACK_BIT;
        rasterizationInfo.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
        rasterizationInfo.depthBiasEnable = VK_FALSE;
        rasterizationInfo.lineWidth = 1.0f;

        VkPipelineMultisampleStateCreateInfo multisampleInfo{};
        multisampleInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
        multisampleInfo.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
        multisampleInfo.sampleShadingEnable = VK_FALSE;

        VkPipelineColorBlendAttachmentState defaultBlend{};
        defaultBlend.blendEnable = VK_FALSE;
        defaultBlend.colorWriteMask =
            VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
            VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
        std::vector<VkPipelineColorBlendAttachmentState> colorBlendAttachments(colorFormats.size(), defaultBlend);

        VkPipelineColorBlendStateCreateInfo colorBlendInfo{};
        colorBlendInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
        colorBlendInfo.logicOpEnable = VK_FALSE;
        colorBlendInfo.attachmentCount = static_cast<uint32_t>(colorBlendAttachments.size());
        colorBlendInfo.pAttachments = colorBlendAttachments.data();

        const VkDynamicState dynamicStates[] = {
            VK_DYNAMIC_STATE_VIEWPORT,
            VK_DYNAMIC_STATE_SCISSOR,
            VK_DYNAMIC_STATE_FRONT_FACE,
            VK_DYNAMIC_STATE_CULL_MODE
        };
        VkPipelineDynamicStateCreateInfo dynamicStateInfo{};
        dynamicStateInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
        dynamicStateInfo.dynamicStateCount = static_cast<uint32_t>(std::size(dynamicStates));
        dynamicStateInfo.pDynamicStates = dynamicStates;

        VkPipelineDepthStencilStateCreateInfo depthState{};
        depthState.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
        depthState.depthTestEnable = depthTest && depthFormat != VK_FORMAT_UNDEFINED;
        depthState.depthWriteEnable = depthState.depthTestEnable && depthWrite;
        depthState.depthCompareOp = depthCompareOp;

        VkPipelineRenderingCreateInfo renderingInfo{};
        renderingInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
        renderingInfo.depthAttachmentFormat = depthFormat;
        renderingInfo.colorAttachmentCount = static_cast<uint32_t>(colorFormats.size());
        renderingInfo.pColorAttachmentFormats = colorFormats.data();

        VkGraphicsPipelineCreateInfo pipelineInfo{};
        pipelineInfo.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
        pipelineInfo.pNext = &renderingInfo;
        pipelineInfo.stageCount = 2;
        pipelineInfo.pStages = shaderStages;
        pipelineInfo.pVertexInputState = &vertexInputInfo;
        pipelineInfo.pInputAssemblyState = &inputAssemblyInfo;
        pipelineInfo.pViewportState = &viewportInfo;
        pipelineInfo.pRasterizationState = &rasterizationInfo;
        pipelineInfo.pMultisampleState = &multisampleInfo;
        pipelineInfo.pDepthStencilState = depthFormat != VK_FORMAT_UNDEFINED ? &depthState : nullptr;
        pipelineInfo.pColorBlendState = &colorBlendInfo;
        pipelineInfo.pDynamicState = &dynamicStateInfo;
        pipelineInfo.layout = pipelineLayout;
        pipelineInfo.renderPass = VK_NULL_HANDLE;
        pipelineInfo.subpass = 0;

        VkPipeline pipeline = VK_NULL_HANDLE;
        result = vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &pipeline);
        if (result != VK_SUCCESS)
        {
            std::cerr << "VulkanGraphicsPipeline::InitializeMrt: Failed to create Graphics Pipeline. VkResult: "
                      << result << '\n';
            vkDestroyPipelineLayout(device, pipelineLayout, nullptr);
            return false;
        }

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
