#include "VulkanHybridGBufferPass.h"

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
    namespace
    {
        constexpr VkFormat kRt0Format = VK_FORMAT_R16G16B16A16_SFLOAT;
        constexpr VkFormat kRt1Format = VK_FORMAT_R16G16B16A16_SFLOAT;
        constexpr VkFormat kRt2Format = VK_FORMAT_R16G16B16A16_SFLOAT;
        constexpr VkFormat kRt3Format = VK_FORMAT_R32G32B32A32_SFLOAT;
    }

    VulkanHybridGBufferPass::~VulkanHybridGBufferPass()
    {
        Shutdown();
    }

    bool VulkanHybridGBufferPass::Initialize(
        const VulkanContext& context,
        const VulkanMemoryManager& memory,
        uint32_t framesInFlight,
        VkExtent2D extent,
        VkFormat depthFormat)
    {
        if (m_initialized || framesInFlight == 0 || extent.width == 0 || extent.height == 0 ||
            context.GetDevice() == VK_NULL_HANDLE || depthFormat == VK_FORMAT_UNDEFINED)
            return false;

        m_physicalDevice = context.GetPhysicalDevice();
        m_device = context.GetDevice();
        m_memory = &memory;
        m_extent = extent;
        m_depthFormat = depthFormat;

        if (VulkanMaterialSet::CreateLayout(m_device, m_materialLayout) != VK_SUCCESS)
        {
            Shutdown();
            return false;
        }

        m_frames.clear();
        m_frames.reserve(framesInFlight);
        for (uint32_t i = 0; i < framesInFlight; ++i)
            m_frames.push_back(std::make_unique<FrameResources>());

        if (!CreatePipelines(depthFormat) || !BuildSizeDependentResources(extent))
        {
            Shutdown();
            return false;
        }

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
            VkQueryPoolCreateInfo qInfo{ VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO };
            qInfo.queryType = VK_QUERY_TYPE_TIMESTAMP;
            qInfo.queryCount = framesInFlight * 2;
            if (vkCreateQueryPool(m_device, &qInfo, nullptr, &m_timestampPool) == VK_SUCCESS)
                m_timestampPeriodNs = static_cast<double>(props.limits.timestampPeriod);
        }

        m_initialized = true;
        return true;
    }

    bool VulkanHybridGBufferPass::CreatePipelines(VkFormat depthFormat)
    {
        const std::string shaderDir = AETHER_SHADER_DIR;
        VulkanShader vert, pbrFrag, colorFrag;
        if (!vert.Initialize(m_device, shaderDir + "/hybrid_gbuffer.vert.spv") ||
            !pbrFrag.Initialize(m_device, shaderDir + "/hybrid_gbuffer.frag.spv") ||
            !colorFrag.Initialize(m_device, shaderDir + "/hybrid_gbuffer_color.frag.spv"))
        {
            std::cerr << "VulkanHybridGBufferPass: failed to load G-Buffer shaders.\n";
            return false;
        }

        const VkVertexInputBindingDescription binding{ 0, sizeof(Vertex), VK_VERTEX_INPUT_RATE_VERTEX };
        const std::array<VkVertexInputAttributeDescription, 8> attributes = {{
            { 0, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(Vertex, position) },
            { 1, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(Vertex, color) },
            { 2, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(Vertex, normal) },
            { 3, 0, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(Vertex, tangent) },
            { 4, 0, VK_FORMAT_R32_SFLOAT, offsetof(Vertex, colorAlpha) },
            { 5, 0, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(Vertex, materialUv) },
            { 6, 0, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(Vertex, materialUv) + 2 * sizeof(glm::vec2) },
            { 7, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(Vertex, materialUv) + 4 * sizeof(glm::vec2) }
        }};

        VkPipelineVertexInputStateCreateInfo vertexInput{ VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
        vertexInput.vertexBindingDescriptionCount = 1;
        vertexInput.pVertexBindingDescriptions = &binding;
        vertexInput.vertexAttributeDescriptionCount = static_cast<uint32_t>(attributes.size());
        vertexInput.pVertexAttributeDescriptions = attributes.data();

        const std::array<VkFormat, 4> mrtFormats = { kRt0Format, kRt1Format, kRt2Format, kRt3Format };
        const std::array<VkDescriptorSetLayout, 1> pbrSetLayouts = { m_materialLayout };

        m_pbrPipeline.Shutdown();
        m_vertexColorPipeline.Shutdown();

        if (!m_pbrPipeline.InitializeMrt(
                m_device, mrtFormats, vert.GetShaderModule(), pbrFrag.GetShaderModule(),
                vertexInput, sizeof(PushConstants), depthFormat, pbrSetLayouts,
                VK_SHADER_STAGE_VERTEX_BIT, true, true, VK_COMPARE_OP_LESS))
            return false;

        if (!m_vertexColorPipeline.InitializeMrt(
                m_device, mrtFormats, vert.GetShaderModule(), colorFrag.GetShaderModule(),
                vertexInput, sizeof(PushConstants), depthFormat, {},
                VK_SHADER_STAGE_VERTEX_BIT, true, true, VK_COMPARE_OP_LESS))
            return false;

        m_depthFormat = depthFormat;
        return true;
    }

    bool VulkanHybridGBufferPass::BuildSizeDependentResources(VkExtent2D extent)
    {
        constexpr VkImageUsageFlags kRtUsage =
            VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
            VK_IMAGE_USAGE_SAMPLED_BIT |
            VK_IMAGE_USAGE_TRANSFER_DST_BIT;

        for (auto& framePtr : m_frames)
        {
            auto& f = *framePtr;
            f.rt0AlbedoMetallic.Shutdown();
            f.rt1NormalRoughness.Shutdown();
            f.rt2EmissiveAo.Shutdown();
            f.rt3WorldPosInst.Shutdown();
            f.layoutsInitialized = false;

            if (f.rt0AlbedoMetallic.Initialize(m_device, *m_memory, extent, kRt0Format, kRtUsage, VK_IMAGE_ASPECT_COLOR_BIT) != VK_SUCCESS ||
                f.rt1NormalRoughness.Initialize(m_device, *m_memory, extent, kRt1Format, kRtUsage, VK_IMAGE_ASPECT_COLOR_BIT) != VK_SUCCESS ||
                f.rt2EmissiveAo.Initialize(m_device, *m_memory, extent, kRt2Format, kRtUsage, VK_IMAGE_ASPECT_COLOR_BIT) != VK_SUCCESS ||
                f.rt3WorldPosInst.Initialize(m_device, *m_memory, extent, kRt3Format, kRtUsage, VK_IMAGE_ASPECT_COLOR_BIT) != VK_SUCCESS)
                return false;
        }
        m_extent = extent;
        return true;
    }

    bool VulkanHybridGBufferPass::OnResize(VkExtent2D extent, VkFormat depthFormat)
    {
        if (!m_initialized || extent.width == 0 || extent.height == 0 || depthFormat == VK_FORMAT_UNDEFINED)
            return false;
        if (depthFormat != m_depthFormat && !CreatePipelines(depthFormat))
            return false;
        return BuildSizeDependentResources(extent);
    }

    void VulkanHybridGBufferPass::Shutdown() noexcept
    {
        m_pbrPipeline.Shutdown();
        m_vertexColorPipeline.Shutdown();
        m_frames.clear();

        if (m_timestampPool != VK_NULL_HANDLE && m_device != VK_NULL_HANDLE)
        {
            vkDestroyQueryPool(m_device, m_timestampPool, nullptr);
            m_timestampPool = VK_NULL_HANDLE;
        }
        if (m_materialLayout != VK_NULL_HANDLE && m_device != VK_NULL_HANDLE)
        {
            vkDestroyDescriptorSetLayout(m_device, m_materialLayout, nullptr);
            m_materialLayout = VK_NULL_HANDLE;
        }

        m_physicalDevice = VK_NULL_HANDLE;
        m_device = VK_NULL_HANDLE;
        m_memory = nullptr;
        m_extent = {};
        m_depthFormat = VK_FORMAT_UNDEFINED;
        m_timestampPeriodNs = 0.0;
        m_lastGpuTimeMs = 0.0;
        m_initialized = false;
    }

    void VulkanHybridGBufferPass::OnFrameFenceCompleted(uint32_t frameIndex)
    {
        if (!m_initialized || frameIndex >= m_frames.size())
            return;
        auto& f = *m_frames[frameIndex];
        if (m_timestampPool != VK_NULL_HANDLE && f.hasPendingTimestamp && f.recordedPass)
        {
            uint64_t stamps[2]{};
            if (vkGetQueryPoolResults(
                    m_device, m_timestampPool, frameIndex * 2, 2,
                    sizeof(stamps), stamps, sizeof(uint64_t), VK_QUERY_RESULT_64_BIT) == VK_SUCCESS &&
                stamps[1] >= stamps[0])
            {
                m_lastGpuTimeMs = static_cast<double>(stamps[1] - stamps[0]) * m_timestampPeriodNs * 1e-6;
            }
        }
        f.hasPendingTimestamp = false;
        f.recordedPass = false;
    }

    void VulkanHybridGBufferPass::OnFrameSubmitted(uint32_t frameIndex)
    {
        if (!m_initialized || frameIndex >= m_frames.size())
            return;
        auto& f = *m_frames[frameIndex];
        f.hasPendingTimestamp = (m_timestampPool != VK_NULL_HANDLE && f.recordedPass);
    }

    bool VulkanHybridGBufferPass::Execute(
        VkCommandBuffer cmd,
        uint32_t frameIndex,
        VkImageView depthView,
        VkImageLayout depthLayout,
        std::span<const HybridGBufferDrawItem> items)
    {
        if (!m_initialized || cmd == VK_NULL_HANDLE || frameIndex >= m_frames.size() ||
            depthView == VK_NULL_HANDLE)
            return false;

        auto& f = *m_frames[frameIndex];
        f.recordedPass = false;

        if (m_timestampPool != VK_NULL_HANDLE)
        {
            vkCmdResetQueryPool(cmd, m_timestampPool, frameIndex * 2, 2);
            vkCmdWriteTimestamp2(cmd, VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, m_timestampPool, frameIndex * 2);
        }

        // Ensure any other frame slot's RT1/RT3 are in SHADER_READ_ONLY_OPTIMAL before denoiser binds them as previous-frame history.
        for (size_t otherIdx = 0; otherIdx < m_frames.size(); ++otherIdx)
        {
            if (otherIdx == frameIndex || m_frames[otherIdx]->layoutsInitialized)
                continue;
            auto& other = *m_frames[otherIdx];
            const std::array<VkImage, 2> histImgs = {
                other.rt1NormalRoughness.GetImage(),
                other.rt3WorldPosInst.GetImage()
            };
            std::array<VkImageMemoryBarrier2, 2> initBarriers{};
            for (size_t j = 0; j < 2; ++j)
            {
                initBarriers[j].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
                initBarriers[j].srcStageMask = VK_PIPELINE_STAGE_2_NONE;
                initBarriers[j].srcAccessMask = 0;
                initBarriers[j].dstStageMask = VK_PIPELINE_STAGE_2_CLEAR_BIT;
                initBarriers[j].dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
                initBarriers[j].oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
                initBarriers[j].newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
                initBarriers[j].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                initBarriers[j].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                initBarriers[j].image = histImgs[j];
                initBarriers[j].subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
            }
            VkDependencyInfo initDep{ VK_STRUCTURE_TYPE_DEPENDENCY_INFO };
            initDep.imageMemoryBarrierCount = 2;
            initDep.pImageMemoryBarriers = initBarriers.data();
            vkCmdPipelineBarrier2(cmd, &initDep);

            const VkClearColorValue zeroClear{ { 0.0f, 0.0f, 0.0f, 0.0f } };
            const VkImageSubresourceRange subRange{ VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
            vkCmdClearColorImage(cmd, histImgs[0], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &zeroClear, 1, &subRange);
            vkCmdClearColorImage(cmd, histImgs[1], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &zeroClear, 1, &subRange);

            for (size_t j = 0; j < 2; ++j)
            {
                initBarriers[j].srcStageMask = VK_PIPELINE_STAGE_2_CLEAR_BIT;
                initBarriers[j].srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
                initBarriers[j].dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
                initBarriers[j].dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
                initBarriers[j].oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
                initBarriers[j].newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            }
            vkCmdPipelineBarrier2(cmd, &initDep);
            other.layoutsInitialized = true;
        }
        f.layoutsInitialized = true;

        const std::array<VkImage, 4> rtImages = {
            f.rt0AlbedoMetallic.GetImage(),
            f.rt1NormalRoughness.GetImage(),
            f.rt2EmissiveAo.GetImage(),
            f.rt3WorldPosInst.GetImage()
        };

        std::array<VkImageMemoryBarrier2, 4> toColor{};
        for (size_t i = 0; i < 4; ++i)
        {
            toColor[i].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
            toColor[i].srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
            toColor[i].srcAccessMask = 0;
            toColor[i].dstStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
            toColor[i].dstAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
            toColor[i].oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            toColor[i].newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            toColor[i].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            toColor[i].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            toColor[i].image = rtImages[i];
            toColor[i].subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
        }
        VkDependencyInfo depToColor{ VK_STRUCTURE_TYPE_DEPENDENCY_INFO };
        depToColor.imageMemoryBarrierCount = static_cast<uint32_t>(toColor.size());
        depToColor.pImageMemoryBarriers = toColor.data();
        vkCmdPipelineBarrier2(cmd, &depToColor);

        std::array<VkRenderingAttachmentInfo, 4> colorAttachments{};
        const std::array<VkImageView, 4> rtViews = {
            f.rt0AlbedoMetallic.GetView(),
            f.rt1NormalRoughness.GetView(),
            f.rt2EmissiveAo.GetView(),
            f.rt3WorldPosInst.GetView()
        };
        for (size_t i = 0; i < 4; ++i)
        {
            colorAttachments[i].sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
            colorAttachments[i].imageView = rtViews[i];
            colorAttachments[i].imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            colorAttachments[i].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
            colorAttachments[i].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
            colorAttachments[i].clearValue.color = { { 0.0f, 0.0f, 0.0f, 0.0f } };
        }

        VkRenderingAttachmentInfo depthAttachment{ VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO };
        depthAttachment.imageView = depthView;
        depthAttachment.imageLayout = depthLayout;
        depthAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        depthAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        depthAttachment.clearValue.depthStencil = { 1.0f, 0 };

        VkRenderingInfo renderingInfo{ VK_STRUCTURE_TYPE_RENDERING_INFO };
        renderingInfo.renderArea.offset = { 0, 0 };
        renderingInfo.renderArea.extent = m_extent;
        renderingInfo.layerCount = 1;
        renderingInfo.colorAttachmentCount = static_cast<uint32_t>(colorAttachments.size());
        renderingInfo.pColorAttachments = colorAttachments.data();
        renderingInfo.pDepthAttachment = &depthAttachment;

        vkCmdBeginRendering(cmd, &renderingInfo);

        const VkViewport viewport{
            0.0f, 0.0f,
            static_cast<float>(m_extent.width), static_cast<float>(m_extent.height),
            0.0f, 1.0f
        };
        const VkRect2D scissor{ { 0, 0 }, m_extent };
        vkCmdSetViewport(cmd, 0, 1, &viewport);
        vkCmdSetScissor(cmd, 0, 1, &scissor);

        VkPipeline boundPipeline = VK_NULL_HANDLE;
        const VulkanMesh* boundMesh = nullptr;

        for (const auto& item : items)
        {
            if (item.alphaBlend || !item.mesh)
                continue;

            const auto& pipeline = item.material ? m_pbrPipeline : m_vertexColorPipeline;
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

            const VkPipelineLayout layout = pipeline.GetPipelineLayout();
            if (item.material != VK_NULL_HANDLE)
                vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 0, 1, &item.material, 0, nullptr);

            PushConstants push{};
            push.mvp = item.mvp;
            push.modelRows[0] = item.modelRows[0];
            push.modelRows[1] = item.modelRows[1];
            push.modelRows[2] = item.modelRows[2];
            const float instIdPlusOne = item.rtInstanceIndex != UINT32_MAX
                ? static_cast<float>(item.rtInstanceIndex + 1u)
                : 0.0f;
            push.eyeAndInstanceId = glm::vec4(item.cameraPosition, instIdPlusOne);

            vkCmdPushConstants(cmd, layout, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(push), &push);
            vkCmdDrawIndexed(cmd, boundMesh->GetIndexCount(), 1, 0, 0, 0);
        }

        vkCmdEndRendering(cmd);

        std::array<VkImageMemoryBarrier2, 4> toShaderRead{};
        for (size_t i = 0; i < 4; ++i)
        {
            toShaderRead[i].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
            toShaderRead[i].srcStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
            toShaderRead[i].srcAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
            toShaderRead[i].dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
            toShaderRead[i].dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
            toShaderRead[i].oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            toShaderRead[i].newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            toShaderRead[i].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            toShaderRead[i].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            toShaderRead[i].image = rtImages[i];
            toShaderRead[i].subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
        }
        VkDependencyInfo depToRead{ VK_STRUCTURE_TYPE_DEPENDENCY_INFO };
        depToRead.imageMemoryBarrierCount = static_cast<uint32_t>(toShaderRead.size());
        depToRead.pImageMemoryBarriers = toShaderRead.data();
        vkCmdPipelineBarrier2(cmd, &depToRead);

        if (m_timestampPool != VK_NULL_HANDLE)
            vkCmdWriteTimestamp2(cmd, VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT, m_timestampPool, frameIndex * 2 + 1);

        f.recordedPass = true;
        return true;
    }

    VkImageView VulkanHybridGBufferPass::GetRt0View(uint32_t frameIndex) const noexcept
    {
        return (m_initialized && frameIndex < m_frames.size()) ? m_frames[frameIndex]->rt0AlbedoMetallic.GetView() : VK_NULL_HANDLE;
    }
    VkImageView VulkanHybridGBufferPass::GetRt1View(uint32_t frameIndex) const noexcept
    {
        return (m_initialized && frameIndex < m_frames.size()) ? m_frames[frameIndex]->rt1NormalRoughness.GetView() : VK_NULL_HANDLE;
    }
    VkImageView VulkanHybridGBufferPass::GetRt2View(uint32_t frameIndex) const noexcept
    {
        return (m_initialized && frameIndex < m_frames.size()) ? m_frames[frameIndex]->rt2EmissiveAo.GetView() : VK_NULL_HANDLE;
    }
    VkImageView VulkanHybridGBufferPass::GetRt3View(uint32_t frameIndex) const noexcept
    {
        return (m_initialized && frameIndex < m_frames.size()) ? m_frames[frameIndex]->rt3WorldPosInst.GetView() : VK_NULL_HANDLE;
    }
    VkImage VulkanHybridGBufferPass::GetRt1Image(uint32_t frameIndex) const noexcept
    {
        return (m_initialized && frameIndex < m_frames.size()) ? m_frames[frameIndex]->rt1NormalRoughness.GetImage() : VK_NULL_HANDLE;
    }
    VkImage VulkanHybridGBufferPass::GetRt3Image(uint32_t frameIndex) const noexcept
    {
        return (m_initialized && frameIndex < m_frames.size()) ? m_frames[frameIndex]->rt3WorldPosInst.GetImage() : VK_NULL_HANDLE;
    }
}
