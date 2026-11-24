#include "ShadowPass.h"

#include <Renderer/Vertex.h>
#include <RHI/Vulkan/Assets/VulkanMaterialSet.h>
#include <RHI/Vulkan/Pipeline/VulkanShader.h>
#include <RHI/Vulkan/Resources/VulkanMesh.h>
#include <RHI/Vulkan/VulkanContext.h>
#include <RHI/Vulkan/VulkanMemoryManager.h>

#include <array>
#include <cstddef>
#include <iostream>
#include <string>

namespace Aether
{
    ShadowPass::~ShadowPass()
    {
        Shutdown();
    }

    VkResult ShadowPass::CreateDescriptorSetLayout(VkDevice device, VkDescriptorSetLayout& layout)
    {
        const std::array<VkDescriptorSetLayoutBinding, 4> bindings = {{
            { 0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr },
            { 1, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,  1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr },
            { 2, VK_DESCRIPTOR_TYPE_SAMPLER,        1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr },
            { 3, VK_DESCRIPTOR_TYPE_SAMPLER,        1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr }
        }};
        VkDescriptorSetLayoutCreateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        info.bindingCount = static_cast<uint32_t>(bindings.size());
        info.pBindings = bindings.data();
        return vkCreateDescriptorSetLayout(device, &info, nullptr, &layout);
    }

    bool ShadowPass::Initialize(
        const VulkanContext& context,
        const VulkanMemoryManager& memory,
        uint32_t framesInFlight,
        VkFormat hdrColorFormat,
        VkFormat hdrDepthFormat,
        uint32_t resolution)
    {
        if (m_device != VK_NULL_HANDLE || framesInFlight == 0 || resolution == 0)
            return false;

        m_physicalDevice = context.GetPhysicalDevice();
        m_device = context.GetDevice();
        m_memory = &memory;
        m_framesInFlight = framesInFlight;
        m_resolution = resolution;

        if (CreateDescriptorSetLayout(m_device, m_descriptorSetLayout) != VK_SUCCESS)
        {
            Shutdown();
            return false;
        }

        const std::array<VkDescriptorPoolSize, 3> poolSizes = {{
            { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, framesInFlight },
            { VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,  framesInFlight },
            { VK_DESCRIPTOR_TYPE_SAMPLER,        framesInFlight * 2 }
        }};
        VkDescriptorPoolCreateInfo poolInfo{};
        poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        poolInfo.maxSets = framesInFlight;
        poolInfo.poolSizeCount = static_cast<uint32_t>(poolSizes.size());
        poolInfo.pPoolSizes = poolSizes.data();
        if (vkCreateDescriptorPool(m_device, &poolInfo, nullptr, &m_descriptorPool) != VK_SUCCESS)
        {
            Shutdown();
            return false;
        }

        if (!CreateFrameResources(resolution))
        {
            Shutdown();
            return false;
        }

        if (!CreatePipelines(hdrColorFormat, hdrDepthFormat))
        {
            Shutdown();
            return false;
        }

        // Optional GPU timestamp queries for measuring shadow pass cost.
        VkPhysicalDeviceProperties props{};
        vkGetPhysicalDeviceProperties(m_physicalDevice, &props);
        uint32_t queueFamilyCount = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(m_physicalDevice, &queueFamilyCount, nullptr);
        std::vector<VkQueueFamilyProperties> queueProps(queueFamilyCount);
        vkGetPhysicalDeviceQueueFamilyProperties(m_physicalDevice, &queueFamilyCount, queueProps.data());
        const uint32_t gfxFamily = context.GetGraphicsQueueFamily();
        if (props.limits.timestampPeriod > 0.0f &&
            gfxFamily < queueProps.size() &&
            queueProps[gfxFamily].timestampValidBits > 0)
        {
            VkQueryPoolCreateInfo queryInfo{};
            queryInfo.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
            queryInfo.queryType = VK_QUERY_TYPE_TIMESTAMP;
            queryInfo.queryCount = framesInFlight * 2;
            if (vkCreateQueryPool(m_device, &queryInfo, nullptr, &m_timestampPool) == VK_SUCCESS)
                m_timestampPeriodNs = props.limits.timestampPeriod;
        }

        return true;
    }

    bool ShadowPass::CreateFrameResources(uint32_t resolution)
    {
        m_frames.clear();
        m_frames.reserve(m_framesInFlight);

        std::vector<VkDescriptorSetLayout> layouts(m_framesInFlight, m_descriptorSetLayout);
        std::vector<VkDescriptorSet> sets(m_framesInFlight, VK_NULL_HANDLE);
        VkDescriptorSetAllocateInfo allocInfo{};
        allocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        allocInfo.descriptorPool = m_descriptorPool;
        allocInfo.descriptorSetCount = m_framesInFlight;
        allocInfo.pSetLayouts = layouts.data();
        if (vkAllocateDescriptorSets(m_device, &allocInfo, sets.data()) != VK_SUCCESS)
            return false;

        for (uint32_t i = 0; i < m_framesInFlight; ++i)
        {
            auto frame = std::make_unique<FrameResources>();
            frame->descriptorSet = sets[i];
            if (frame->shadowMap.Initialize(m_physicalDevice, m_device, *m_memory, resolution) != VK_SUCCESS)
                return false;
            if (frame->uniformBuffer.Initialize(
                    m_device, *m_memory, sizeof(ShadowUniforms),
                    VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                    VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT,
                    VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) != VK_SUCCESS)
                return false;

            const ShadowUniforms initial{};
            if (frame->uniformBuffer.UploadData(&initial, sizeof(initial)) != VK_SUCCESS)
                return false;

            UpdateFrameDescriptor(*frame);
            m_depthFormat = frame->shadowMap.GetFormat();
            m_frames.push_back(std::move(frame));
        }
        m_resolution = resolution;
        return true;
    }

    void ShadowPass::UpdateFrameDescriptor(FrameResources& frame) const
    {
        const VkDescriptorBufferInfo uboInfo{ frame.uniformBuffer.GetBuffer(), 0, sizeof(ShadowUniforms) };
        const VkDescriptorImageInfo imageInfo{
            VK_NULL_HANDLE, frame.shadowMap.GetDepthView(), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
        };
        const VkDescriptorImageInfo compareSamplerInfo{
            frame.shadowMap.GetCompareSampler(), VK_NULL_HANDLE, VK_IMAGE_LAYOUT_UNDEFINED
        };
        const VkDescriptorImageInfo debugSamplerInfo{
            frame.shadowMap.GetDebugSampler(), VK_NULL_HANDLE, VK_IMAGE_LAYOUT_UNDEFINED
        };

        const std::array<VkWriteDescriptorSet, 4> writes = {{
            { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, frame.descriptorSet, 0, 0, 1,
              VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, nullptr, &uboInfo, nullptr },
            { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, frame.descriptorSet, 1, 0, 1,
              VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, &imageInfo, nullptr, nullptr },
            { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, frame.descriptorSet, 2, 0, 1,
              VK_DESCRIPTOR_TYPE_SAMPLER, &compareSamplerInfo, nullptr, nullptr },
            { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, frame.descriptorSet, 3, 0, 1,
              VK_DESCRIPTOR_TYPE_SAMPLER, &debugSamplerInfo, nullptr, nullptr }
        }};
        vkUpdateDescriptorSets(m_device, static_cast<uint32_t>(writes.size()), writes.data(), 0, nullptr);
    }

    bool ShadowPass::CreatePipelines(VkFormat hdrColorFormat, VkFormat hdrDepthFormat)
    {
        const std::string shaderDir = AETHER_SHADER_DIR;

        // 1. Opaque depth-only shadow pipeline (no fragment shader).
        {
            VulkanShader vert;
            if (!vert.Initialize(m_device, shaderDir + "/shadow.vert.spv"))
                return false;

            const VkVertexInputBindingDescription binding{ 0, sizeof(Vertex), VK_VERTEX_INPUT_RATE_VERTEX };
            const VkVertexInputAttributeDescription positionAttr{
                0, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(Vertex, position)
            };
            VkPipelineVertexInputStateCreateInfo vertexInput{};
            vertexInput.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
            vertexInput.vertexBindingDescriptionCount = 1;
            vertexInput.pVertexBindingDescriptions = &binding;
            vertexInput.vertexAttributeDescriptionCount = 1;
            vertexInput.pVertexAttributeDescriptions = &positionAttr;

            if (!m_opaquePipeline.Initialize(
                    m_device,
                    VK_FORMAT_UNDEFINED,
                    vert.GetShaderModule(),
                    VK_NULL_HANDLE,
                    vertexInput,
                    sizeof(glm::mat4),
                    m_depthFormat,
                    VK_NULL_HANDLE,
                    false,
                    VK_NULL_HANDLE,
                    VK_SHADER_STAGE_VERTEX_BIT,
                    true,
                    VK_NULL_HANDLE,
                    true))
                return false;
        }

        // 2. Alpha-MASK shadow cutout pipeline (reuses VulkanMaterialSet descriptor set 0).
        {
            VulkanShader vert, frag;
            if (!vert.Initialize(m_device, shaderDir + "/shadow_mask.vert.spv") ||
                !frag.Initialize(m_device, shaderDir + "/shadow_mask.frag.spv"))
                return false;

            VkDescriptorSetLayout materialLayout = VK_NULL_HANDLE;
            if (VulkanMaterialSet::CreateLayout(m_device, materialLayout) != VK_SUCCESS)
                return false;

            const VkVertexInputBindingDescription binding{ 0, sizeof(Vertex), VK_VERTEX_INPUT_RATE_VERTEX };
            const std::array<VkVertexInputAttributeDescription, 3> attrs = {{
                { 0, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(Vertex, position) },
                { 1, 0, VK_FORMAT_R32_SFLOAT,       offsetof(Vertex, colorAlpha) },
                { 2, 0, VK_FORMAT_R32G32_SFLOAT,    offsetof(Vertex, materialUv) }
            }};
            VkPipelineVertexInputStateCreateInfo vertexInput{};
            vertexInput.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
            vertexInput.vertexBindingDescriptionCount = 1;
            vertexInput.pVertexBindingDescriptions = &binding;
            vertexInput.vertexAttributeDescriptionCount = static_cast<uint32_t>(attrs.size());
            vertexInput.pVertexAttributeDescriptions = attrs.data();

            const bool ok = m_maskPipeline.Initialize(
                m_device,
                VK_FORMAT_UNDEFINED,
                vert.GetShaderModule(),
                frag.GetShaderModule(),
                vertexInput,
                sizeof(glm::mat4),
                m_depthFormat,
                materialLayout,
                false,
                VK_NULL_HANDLE,
                VK_SHADER_STAGE_VERTEX_BIT,
                true,
                VK_NULL_HANDLE,
                true);
            vkDestroyDescriptorSetLayout(m_device, materialLayout, nullptr);
            if (!ok)
                return false;
        }

        // 3. Shadow map debug visualization pipeline (renders raw depth into HDR color target).
        {
            VulkanShader vert, frag;
            if (!vert.Initialize(m_device, shaderDir + "/shadow_debug.vert.spv") ||
                !frag.Initialize(m_device, shaderDir + "/shadow_debug.frag.spv"))
                return false;

            VkPipelineVertexInputStateCreateInfo emptyInput{};
            emptyInput.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;

            if (!m_debugPipeline.Initialize(
                    m_device,
                    hdrColorFormat,
                    vert.GetShaderModule(),
                    frag.GetShaderModule(),
                    emptyInput,
                    0,
                    hdrDepthFormat,
                    m_descriptorSetLayout,
                    false,
                    VK_NULL_HANDLE,
                    VK_SHADER_STAGE_VERTEX_BIT,
                    false))
                return false;
        }

        return true;
    }

    bool ShadowPass::SetResolution(uint32_t resolution)
    {
        if (m_device == VK_NULL_HANDLE || resolution == 0)
            return false;
        if (resolution == m_resolution)
            return true;

        for (auto& frame : m_frames)
        {
            frame->shadowMap.Shutdown();
            if (frame->shadowMap.Initialize(m_physicalDevice, m_device, *m_memory, resolution) != VK_SUCCESS)
                return false;
            UpdateFrameDescriptor(*frame);
        }
        m_resolution = resolution;
        return true;
    }

    void ShadowPass::Execute(
        VkCommandBuffer cmd,
        uint32_t frameIndex,
        std::span<const ShadowCasterItem> casters,
        const ShadowLightSpace& lightSpace,
        const ShadowConfig& rawConfig)
    {
        if (m_device == VK_NULL_HANDLE || frameIndex >= m_frames.size())
            return;

        auto& frame = *m_frames[frameIndex];
        const ShadowConfig cfg = SanitizeShadowConfig(rawConfig);
        const bool active = cfg.enabled && lightSpace.valid;

        // Read back previous GPU timestamp for this frame slot (its fence was just waited on).
        if (m_timestampPool != VK_NULL_HANDLE && frame.hasTimestamp)
        {
            uint64_t timestamps[2]{};
            const VkResult qRes = vkGetQueryPoolResults(
                m_device, m_timestampPool, frameIndex * 2, 2,
                sizeof(timestamps), timestamps, sizeof(uint64_t), VK_QUERY_RESULT_64_BIT);
            if (qRes == VK_SUCCESS && timestamps[1] >= timestamps[0])
                m_lastGpuTimeMs = static_cast<double>(timestamps[1] - timestamps[0]) * m_timestampPeriodNs * 1e-6;
            frame.hasTimestamp = false;
        }

        ShadowUniforms uniforms{};
        uniforms.lightViewProj = active ? lightSpace.viewProjection : glm::mat4(1.0f);
        uniforms.params0 = glm::vec4(
            active ? 1.0f : 0.0f,
            cfg.strength,
            cfg.receiverDepthBias,
            1.0f / static_cast<float>(m_resolution)
        );
        uniforms.params1 = glm::vec4(
            cfg.pcfRadiusTexels,
            cfg.normalBias,
            cfg.pcfEnabled ? 1.0f : 0.0f,
            cfg.debugView ? 1.0f : 0.0f
        );
        frame.uniformBuffer.UploadData(&uniforms, sizeof(uniforms));

        const VkImageLayout oldLayout = frame.shadowMap.GetLayout();
        if (!active && oldLayout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL)
            return;

        if (m_timestampPool != VK_NULL_HANDLE)
        {
            vkCmdResetQueryPool(cmd, m_timestampPool, frameIndex * 2, 2);
            vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, m_timestampPool, frameIndex * 2);
        }

        constexpr VkPipelineStageFlags2 depthStages =
            VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT;

        VkImageMemoryBarrier2 toDepth{};
        toDepth.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
        toDepth.srcStageMask = oldLayout == VK_IMAGE_LAYOUT_UNDEFINED
            ? VK_PIPELINE_STAGE_2_NONE
            : VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
        toDepth.srcAccessMask = oldLayout == VK_IMAGE_LAYOUT_UNDEFINED
            ? 0
            : VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
        toDepth.dstStageMask = depthStages;
        toDepth.dstAccessMask =
            VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
        toDepth.oldLayout = oldLayout;
        toDepth.newLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        toDepth.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        toDepth.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        toDepth.image = frame.shadowMap.GetImage().GetImage();
        toDepth.subresourceRange = { VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1 };

        VkDependencyInfo depToDepth{};
        depToDepth.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
        depToDepth.imageMemoryBarrierCount = 1;
        depToDepth.pImageMemoryBarriers = &toDepth;
        vkCmdPipelineBarrier2(cmd, &depToDepth);

        VkRenderingAttachmentInfo depthAttachment{};
        depthAttachment.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
        depthAttachment.imageView = frame.shadowMap.GetDepthView();
        depthAttachment.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        depthAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        depthAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        depthAttachment.clearValue.depthStencil = { 1.0f, 0 };

        VkRenderingInfo renderInfo{};
        renderInfo.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
        renderInfo.renderArea.offset = { 0, 0 };
        renderInfo.renderArea.extent = { m_resolution, m_resolution };
        renderInfo.layerCount = 1;
        renderInfo.colorAttachmentCount = 0;
        renderInfo.pColorAttachments = nullptr;
        renderInfo.pDepthAttachment = &depthAttachment;

        vkCmdBeginRendering(cmd, &renderInfo);

        if (active)
        {
            const VkViewport viewport{
                0.0f, 0.0f,
                static_cast<float>(m_resolution), static_cast<float>(m_resolution),
                0.0f, 1.0f
            };
            const VkRect2D scissor{ { 0, 0 }, { m_resolution, m_resolution } };
            vkCmdSetViewport(cmd, 0, 1, &viewport);
            vkCmdSetScissor(cmd, 0, 1, &scissor);
            vkCmdSetDepthBias(cmd, cfg.constantDepthBias, 0.0f, cfg.slopeDepthBias);

            VkPipeline boundPipeline = VK_NULL_HANDLE;
            const VulkanMesh* boundMesh = nullptr;

            for (const auto& item : casters)
            {
                // Transparent BLEND materials are excluded from shadow casting in Phase 6.
                if (!item.mesh || item.alphaBlend)
                    continue;

                const bool useMask = item.alphaMask && item.material != VK_NULL_HANDLE;
                const auto& pipeline = useMask ? m_maskPipeline : m_opaquePipeline;
                if (pipeline.GetPipeline() != boundPipeline)
                {
                    boundPipeline = pipeline.GetPipeline();
                    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, boundPipeline);
                }

                if (item.mesh != boundMesh)
                {
                    boundMesh = item.mesh;
                    const VkBuffer vb = boundMesh->GetVertexBuffer().GetBuffer();
                    const VkDeviceSize offset = 0;
                    vkCmdBindVertexBuffers(cmd, 0, 1, &vb, &offset);
                    vkCmdBindIndexBuffer(cmd, boundMesh->GetIndexBuffer().GetBuffer(), 0, VK_INDEX_TYPE_UINT32);
                }

                vkCmdSetFrontFace(cmd, item.mirrored ? VK_FRONT_FACE_CLOCKWISE : VK_FRONT_FACE_COUNTER_CLOCKWISE);
                vkCmdSetCullMode(cmd, item.doubleSided ? VK_CULL_MODE_NONE : VK_CULL_MODE_BACK_BIT);

                if (useMask)
                {
                    vkCmdBindDescriptorSets(
                        cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline.GetPipelineLayout(),
                        0, 1, &item.material, 0, nullptr);
                }

                const glm::mat4 lightMvp = lightSpace.viewProjection * item.model;
                vkCmdPushConstants(
                    cmd, pipeline.GetPipelineLayout(), VK_SHADER_STAGE_VERTEX_BIT,
                    0, sizeof(lightMvp), &lightMvp);
                vkCmdDrawIndexed(cmd, boundMesh->GetIndexCount(), 1, 0, 0, 0);
            }
        }

        vkCmdEndRendering(cmd);

        VkImageMemoryBarrier2 toRead{};
        toRead.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
        toRead.srcStageMask = depthStages;
        toRead.srcAccessMask = VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
        toRead.dstStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
        toRead.dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
        toRead.oldLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        toRead.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        toRead.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        toRead.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        toRead.image = frame.shadowMap.GetImage().GetImage();
        toRead.subresourceRange = { VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1 };

        VkDependencyInfo depToRead{};
        depToRead.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
        depToRead.imageMemoryBarrierCount = 1;
        depToRead.pImageMemoryBarriers = &toRead;
        vkCmdPipelineBarrier2(cmd, &depToRead);

        frame.shadowMap.SetLayout(VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

        if (m_timestampPool != VK_NULL_HANDLE)
        {
            vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, m_timestampPool, frameIndex * 2 + 1);
            frame.hasTimestamp = true;
        }
    }

    void ShadowPass::DrawDebugOverlay(VkCommandBuffer cmd, uint32_t frameIndex) const
    {
        if (m_device == VK_NULL_HANDLE || frameIndex >= m_frames.size())
            return;

        const auto layout = m_debugPipeline.GetPipelineLayout();
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_debugPipeline.GetPipeline());
        vkCmdSetCullMode(cmd, VK_CULL_MODE_NONE);
        vkCmdSetFrontFace(cmd, VK_FRONT_FACE_COUNTER_CLOCKWISE);
        const VkDescriptorSet set = m_frames[frameIndex]->descriptorSet;
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 0, 1, &set, 0, nullptr);
        vkCmdDraw(cmd, 3, 1, 0, 0);
    }

    void ShadowPass::Shutdown() noexcept
    {
        if (m_device == VK_NULL_HANDLE)
            return;

        m_debugPipeline.Shutdown();
        m_maskPipeline.Shutdown();
        m_opaquePipeline.Shutdown();

        if (m_timestampPool != VK_NULL_HANDLE)
        {
            vkDestroyQueryPool(m_device, m_timestampPool, nullptr);
            m_timestampPool = VK_NULL_HANDLE;
        }

        m_frames.clear();

        if (m_descriptorPool != VK_NULL_HANDLE)
        {
            vkDestroyDescriptorPool(m_device, m_descriptorPool, nullptr);
            m_descriptorPool = VK_NULL_HANDLE;
        }
        if (m_descriptorSetLayout != VK_NULL_HANDLE)
        {
            vkDestroyDescriptorSetLayout(m_device, m_descriptorSetLayout, nullptr);
            m_descriptorSetLayout = VK_NULL_HANDLE;
        }

        m_physicalDevice = VK_NULL_HANDLE;
        m_device = VK_NULL_HANDLE;
        m_memory = nullptr;
        m_framesInFlight = 0;
        m_depthFormat = VK_FORMAT_UNDEFINED;
        m_timestampPeriodNs = 0.0f;
        m_lastGpuTimeMs = 0.0;
    }
}
