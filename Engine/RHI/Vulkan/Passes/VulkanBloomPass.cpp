#include "VulkanBloomPass.h"

#include <RHI/Vulkan/Pipeline/VulkanShader.h>
#include <RHI/Vulkan/VulkanContext.h>
#include <RHI/Vulkan/VulkanMemoryManager.h>

#include <algorithm>
#include <array>
#include <iostream>
#include <string>

namespace Aether
{
    namespace
    {
        struct BrightPush
        {
            float invSrcW, invSrcH;
            float threshold, softKnee;
        };

        struct DownsamplePush
        {
            float invSrcW, invSrcH;
            float pad0, pad1;
        };

        struct BlurPush
        {
            float dirX, dirY;
            float pad0, pad1;
        };

        struct UpsamplePush
        {
            float invSmallW, invSmallH;
            float scatter, combineBase;
        };

        struct CompositePush
        {
            float intensity;
            uint32_t debugMode;
            float pad0, pad1;
        };
    }

    VulkanBloomPass::~VulkanBloomPass()
    {
        Shutdown();
    }

    bool VulkanBloomPass::Initialize(
        const VulkanContext& context,
        const VulkanMemoryManager& memory,
        uint32_t framesInFlight,
        VkExtent2D sceneExtent,
        VkImageView hdrSceneView,
        VkDescriptorSetLayout toneMapInputLayout,
        std::span<const VkImageView> taaResolvedViews,
        std::span<const VkImageView> hybridCompositeViews)
    {
        if (m_device != VK_NULL_HANDLE || framesInFlight == 0 ||
            sceneExtent.width == 0 || sceneExtent.height == 0 ||
            hdrSceneView == VK_NULL_HANDLE || toneMapInputLayout == VK_NULL_HANDLE)
            return false;

        m_physicalDevice = context.GetPhysicalDevice();
        m_device = context.GetDevice();
        m_memory = &memory;
        m_framesInFlight = framesInFlight;
        m_toneMapInputLayout = toneMapInputLayout;

        VkPhysicalDeviceProperties props{};
        vkGetPhysicalDeviceProperties(m_physicalDevice, &props);
        m_maxImageDimension = props.limits.maxImageDimension2D > 0 ? props.limits.maxImageDimension2D : 16384u;

        VkSamplerCreateInfo samplerInfo{};
        samplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
        samplerInfo.magFilter = VK_FILTER_LINEAR;
        samplerInfo.minFilter = VK_FILTER_LINEAR;
        samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
        samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        samplerInfo.maxLod = 0.0f;
        if (vkCreateSampler(m_device, &samplerInfo, nullptr, &m_linearSampler) != VK_SUCCESS)
        {
            Shutdown();
            return false;
        }

        const std::array<VkDescriptorSetLayoutBinding, 3> bindings = {{
            { 0, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr },
            { 1, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr },
            { 2, VK_DESCRIPTOR_TYPE_SAMPLER,       1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr }
        }};
        VkDescriptorSetLayoutCreateInfo layoutInfo{};
        layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        layoutInfo.bindingCount = static_cast<uint32_t>(bindings.size());
        layoutInfo.pBindings = bindings.data();
        if (vkCreateDescriptorSetLayout(m_device, &layoutInfo, nullptr, &m_passSetLayout) != VK_SUCCESS)
        {
            Shutdown();
            return false;
        }

        if (!CreatePipelines() || !BuildFrameResources(sceneExtent, hdrSceneView, false, taaResolvedViews, hybridCompositeViews))
        {
            Shutdown();
            return false;
        }

        // Optional GPU timestamp queries for measuring Bloom pass cost.
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

    bool VulkanBloomPass::CreatePipelines()
    {
        const std::string shaderDir = AETHER_SHADER_DIR;
        VulkanShader vert;
        if (!vert.Initialize(m_device, shaderDir + "/bloom_fullscreen.vert.spv"))
            return false;

        VkPipelineVertexInputStateCreateInfo emptyInput{};
        emptyInput.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;

        const auto createPass = [&](VulkanGraphicsPipeline& pipeline, const char* fragName) {
            VulkanShader frag;
            if (!frag.Initialize(m_device, shaderDir + "/" + fragName))
                return false;
            return pipeline.Initialize(
                m_device,
                kHdrFormat,
                vert.GetShaderModule(),
                frag.GetShaderModule(),
                emptyInput,
                16u,
                VK_FORMAT_UNDEFINED,
                m_passSetLayout,
                false,
                VK_NULL_HANDLE,
                kPushStages,
                false);
        };

        return createPass(m_brightPipeline, "bloom_bright.frag.spv") &&
               createPass(m_downsamplePipeline, "bloom_downsample.frag.spv") &&
               createPass(m_blurPipeline, "bloom_blur.frag.spv") &&
               createPass(m_upsamplePipeline, "bloom_upsample.frag.spv") &&
               createPass(m_compositePipeline, "bloom_composite.frag.spv");
    }

    bool VulkanBloomPass::BuildFrameResources(
        VkExtent2D sceneExtent,
        VkImageView hdrSceneView,
        bool injectFailure,
        std::span<const VkImageView> taaResolvedViews,
        std::span<const VkImageView> hybridCompositeViews)
    {
        const auto dims = ComputeBloomPyramidDimensions(
            { sceneExtent.width, sceneExtent.height }, 8u, m_maxImageDimension);
        if (dims.empty() || hdrSceneView == VK_NULL_HANDLE)
            return false;

        const uint32_t levelCount = static_cast<uint32_t>(dims.size());
        const uint32_t passSetsPerFrame = 7u + levelCount * 5u;
        const uint32_t totalPassSets = passSetsPerFrame * m_framesInFlight;
        const uint32_t totalToneSets = m_framesInFlight;

        VkDescriptorPool newPool = VK_NULL_HANDLE;
        const std::array<VkDescriptorPoolSize, 2> poolSizes = {{
            { VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, totalPassSets * 2u + totalToneSets },
            { VK_DESCRIPTOR_TYPE_SAMPLER,       totalPassSets }
        }};
        VkDescriptorPoolCreateInfo poolInfo{};
        poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        poolInfo.maxSets = totalPassSets + totalToneSets;
        poolInfo.poolSizeCount = static_cast<uint32_t>(poolSizes.size());
        poolInfo.pPoolSizes = poolSizes.data();
        if (vkCreateDescriptorPool(m_device, &poolInfo, nullptr, &newPool) != VK_SUCCESS)
            return false;

        std::vector<std::unique_ptr<FrameResources>> newFrames;
        newFrames.reserve(m_framesInFlight);

        const auto allocSet = [&](VkDescriptorSetLayout layout, VkDescriptorSet& out) {
            VkDescriptorSetAllocateInfo alloc{};
            alloc.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
            alloc.descriptorPool = newPool;
            alloc.descriptorSetCount = 1;
            alloc.pSetLayouts = &layout;
            return vkAllocateDescriptorSets(m_device, &alloc, &out) == VK_SUCCESS;
        };

        const auto writePassSet = [&](VkDescriptorSet set, VkImageView v0, VkImageView v1) {
            const VkDescriptorImageInfo img0{ VK_NULL_HANDLE, v0, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
            const VkDescriptorImageInfo img1{ VK_NULL_HANDLE, v1, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
            const VkDescriptorImageInfo samp{ m_linearSampler, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_UNDEFINED };
            const std::array<VkWriteDescriptorSet, 3> writes = {{
                { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, set, 0, 0, 1,
                  VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, &img0, nullptr, nullptr },
                { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, set, 1, 0, 1,
                  VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, &img1, nullptr, nullptr },
                { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, set, 2, 0, 1,
                  VK_DESCRIPTOR_TYPE_SAMPLER, &samp, nullptr, nullptr }
            }};
            vkUpdateDescriptorSets(m_device, static_cast<uint32_t>(writes.size()), writes.data(), 0, nullptr);
        };

        constexpr VkImageUsageFlags usage =
            VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;

        bool ok = true;
        for (uint32_t f = 0; f < m_framesInFlight && ok; ++f)
        {
            auto frame = std::make_unique<FrameResources>();
            frame->levels.reserve(levelCount);

            for (uint32_t k = 0; k < levelCount && ok; ++k)
            {
                auto lvl = std::make_unique<LevelTarget>();
                lvl->extent = { dims[k].width, dims[k].height };
                if (lvl->mainImage.Initialize(m_device, *m_memory, lvl->extent, kHdrFormat, usage, VK_IMAGE_ASPECT_COLOR_BIT) != VK_SUCCESS ||
                    lvl->tempImage.Initialize(m_device, *m_memory, lvl->extent, kHdrFormat, usage, VK_IMAGE_ASPECT_COLOR_BIT) != VK_SUCCESS)
                {
                    ok = false;
                    break;
                }
                if (!allocSet(m_passSetLayout, lvl->mainSampleSet) ||
                    !allocSet(m_passSetLayout, lvl->tempSampleSet) ||
                    !allocSet(m_passSetLayout, lvl->upsampleFromMainSet) ||
                    !allocSet(m_passSetLayout, lvl->upsampleFromTempSet) ||
                    !allocSet(m_passSetLayout, lvl->debugLevelSet))
                {
                    ok = false;
                    break;
                }
                writePassSet(lvl->mainSampleSet, lvl->mainImage.GetView(), lvl->mainImage.GetView());
                writePassSet(lvl->tempSampleSet, lvl->tempImage.GetView(), lvl->tempImage.GetView());
                writePassSet(lvl->debugLevelSet, hdrSceneView, lvl->mainImage.GetView());
                frame->levels.push_back(std::move(lvl));
            }

            if (ok && injectFailure)
                ok = false;

            if (ok)
            {
                for (uint32_t k = 0; k + 1 < levelCount; ++k)
                {
                    auto& curr = *frame->levels[k];
                    const auto& smaller = *frame->levels[k + 1];
                    writePassSet(curr.upsampleFromMainSet, smaller.mainImage.GetView(), curr.mainImage.GetView());
                    writePassSet(curr.upsampleFromTempSet, smaller.tempImage.GetView(), curr.mainImage.GetView());
                }
                if (frame->reconstructed.Initialize(m_device, *m_memory, sceneExtent, kHdrFormat, usage, VK_IMAGE_ASPECT_COLOR_BIT) != VK_SUCCESS ||
                    frame->composite.Initialize(m_device, *m_memory, sceneExtent, kHdrFormat, usage, VK_IMAGE_ASPECT_COLOR_BIT) != VK_SUCCESS)
                {
                    ok = false;
                }
            }

            if (ok)
            {
                if (!allocSet(m_passSetLayout, frame->brightInputSet) ||
                    !allocSet(m_passSetLayout, frame->compositeSet) ||
                    !allocSet(m_passSetLayout, frame->taaBrightInputSet) ||
                    !allocSet(m_passSetLayout, frame->taaCompositeSet) ||
                    !allocSet(m_passSetLayout, frame->hybridBrightInputSet) ||
                    !allocSet(m_passSetLayout, frame->hybridCompositeSet) ||
                    !allocSet(m_passSetLayout, frame->debugBrightSet) ||
                    !allocSet(m_toneMapInputLayout, frame->outputToneMapSet))
                {
                    ok = false;
                }
                else
                {
                    const VkImageView taaView = (f < taaResolvedViews.size() && taaResolvedViews[f] != VK_NULL_HANDLE)
                        ? taaResolvedViews[f]
                        : hdrSceneView;
                    const VkImageView hybridView = (f < hybridCompositeViews.size() && hybridCompositeViews[f] != VK_NULL_HANDLE)
                        ? hybridCompositeViews[f]
                        : hdrSceneView;
                    writePassSet(frame->brightInputSet, hdrSceneView, hdrSceneView);
                    writePassSet(frame->compositeSet, hdrSceneView, frame->reconstructed.GetView());
                    writePassSet(frame->taaBrightInputSet, taaView, taaView);
                    writePassSet(frame->taaCompositeSet, taaView, frame->reconstructed.GetView());
                    writePassSet(frame->hybridBrightInputSet, hybridView, hybridView);
                    writePassSet(frame->hybridCompositeSet, hybridView, frame->reconstructed.GetView());
                    writePassSet(frame->debugBrightSet, hdrSceneView, frame->levels[0]->mainImage.GetView());

                    const VkDescriptorImageInfo outInfo{
                        VK_NULL_HANDLE, frame->composite.GetView(), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
                    };
                    const VkWriteDescriptorSet outWrite{
                        VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, frame->outputToneMapSet,
                        0, 0, 1, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, &outInfo, nullptr, nullptr
                    };
                    vkUpdateDescriptorSets(m_device, 1, &outWrite, 0, nullptr);
                }
            }

            if (ok)
                newFrames.push_back(std::move(frame));
        }

        if (!ok)
        {
            newFrames.clear();
            vkDestroyDescriptorPool(m_device, newPool, nullptr);
            return false;
        }

        ReleaseFrameResources();
        m_descriptorPool = newPool;
        m_frames = std::move(newFrames);
        m_sceneExtent = sceneExtent;
        return true;
    }

    bool VulkanBloomPass::OnResize(
        VkExtent2D sceneExtent,
        VkImageView hdrSceneView,
        bool injectFailure,
        std::span<const VkImageView> taaResolvedViews,
        std::span<const VkImageView> hybridCompositeViews)
    {
        if (m_device == VK_NULL_HANDLE || sceneExtent.width == 0 || sceneExtent.height == 0)
            return false;
        return BuildFrameResources(sceneExtent, hdrSceneView, injectFailure, taaResolvedViews, hybridCompositeViews);
    }

    void VulkanBloomPass::ReleaseFrameResources() noexcept
    {
        m_frames.clear();
        if (m_descriptorPool != VK_NULL_HANDLE)
        {
            vkDestroyDescriptorPool(m_device, m_descriptorPool, nullptr);
            m_descriptorPool = VK_NULL_HANDLE;
        }
    }

    void VulkanBloomPass::TransitionColorImage(
        VkCommandBuffer cmd,
        VkImage image,
        VkImageLayout oldLayout,
        VkImageLayout newLayout,
        VkPipelineStageFlags2 srcStage,
        VkAccessFlags2 srcAccess,
        VkPipelineStageFlags2 dstStage,
        VkAccessFlags2 dstAccess)
    {
        VkImageMemoryBarrier2 barrier{};
        barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
        barrier.srcStageMask = srcStage;
        barrier.srcAccessMask = srcAccess;
        barrier.dstStageMask = dstStage;
        barrier.dstAccessMask = dstAccess;
        barrier.oldLayout = oldLayout;
        barrier.newLayout = newLayout;
        barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.image = image;
        barrier.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };

        VkDependencyInfo dep{};
        dep.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
        dep.imageMemoryBarrierCount = 1;
        dep.pImageMemoryBarriers = &barrier;
        vkCmdPipelineBarrier2(cmd, &dep);
    }

    void VulkanBloomPass::DrawFullscreenPass(
        VkCommandBuffer cmd,
        const VulkanGraphicsPipeline& pipeline,
        VkDescriptorSet set,
        VkImageView targetView,
        VkExtent2D targetExtent,
        const void* pushData,
        uint32_t pushSize)
    {
        VkRenderingAttachmentInfo color{};
        color.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
        color.imageView = targetView;
        color.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        color.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;

        VkRenderingInfo renderInfo{};
        renderInfo.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
        renderInfo.renderArea.offset = { 0, 0 };
        renderInfo.renderArea.extent = targetExtent;
        renderInfo.layerCount = 1;
        renderInfo.colorAttachmentCount = 1;
        renderInfo.pColorAttachments = &color;

        vkCmdBeginRendering(cmd, &renderInfo);

        const VkViewport viewport{
            0.0f, 0.0f,
            static_cast<float>(targetExtent.width), static_cast<float>(targetExtent.height),
            0.0f, 1.0f
        };
        const VkRect2D scissor{ { 0, 0 }, targetExtent };
        vkCmdSetViewport(cmd, 0, 1, &viewport);
        vkCmdSetScissor(cmd, 0, 1, &scissor);
        vkCmdSetCullMode(cmd, VK_CULL_MODE_NONE);
        vkCmdSetFrontFace(cmd, VK_FRONT_FACE_COUNTER_CLOCKWISE);

        const auto layout = pipeline.GetPipelineLayout();
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline.GetPipeline());
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 0, 1, &set, 0, nullptr);
        vkCmdPushConstants(cmd, layout, kPushStages, 0, pushSize, pushData);
        vkCmdDraw(cmd, 3, 1, 0, 0);

        vkCmdEndRendering(cmd);
    }

    void VulkanBloomPass::Execute(
        VkCommandBuffer cmd,
        uint32_t frameIndex,
        const BloomConfig& rawConfig,
        bool useTaaInput)
    {
        Execute(cmd, frameIndex, rawConfig,
            useTaaInput ? BloomInputSource::TaaResolved : BloomInputSource::HdrScene);
    }

    void VulkanBloomPass::Execute(
        VkCommandBuffer cmd,
        uint32_t frameIndex,
        const BloomConfig& rawConfig,
        BloomInputSource inputSource)
    {
        if (m_device == VK_NULL_HANDLE || frameIndex >= m_frames.size())
            return;

        auto& frame = *m_frames[frameIndex];
        const BloomConfig cfg = SanitizeBloomConfig(rawConfig);
        if (!cfg.enabled || frame.levels.empty())
            return;

        if (m_timestampPool != VK_NULL_HANDLE && frame.hasTimestamp)
        {
            uint64_t timestamps[2]{};
            if (vkGetQueryPoolResults(
                    m_device, m_timestampPool, frameIndex * 2, 2,
                    sizeof(timestamps), timestamps, sizeof(uint64_t), VK_QUERY_RESULT_64_BIT) == VK_SUCCESS &&
                timestamps[1] >= timestamps[0])
            {
                m_lastGpuTimeMs = static_cast<double>(timestamps[1] - timestamps[0]) * m_timestampPeriodNs * 1e-6;
            }
            frame.hasTimestamp = false;
        }

        if (m_timestampPool != VK_NULL_HANDLE)
        {
            vkCmdResetQueryPool(cmd, m_timestampPool, frameIndex * 2, 2);
            vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, m_timestampPool, frameIndex * 2);
        }

        const uint32_t activeLevels = std::min(cfg.pyramidLevels, static_cast<uint32_t>(frame.levels.size()));

        auto toColor = [&](VkImage img, VkImageLayout oldLayout) {
            TransitionColorImage(
                cmd, img, oldLayout, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                oldLayout == VK_IMAGE_LAYOUT_UNDEFINED ? VK_PIPELINE_STAGE_2_NONE : VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                oldLayout == VK_IMAGE_LAYOUT_UNDEFINED ? 0 : VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
                VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
        };
        auto toRead = [&](VkImage img) {
            TransitionColorImage(
                cmd, img, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
        };

        // 1. Bright Pass Extraction: HDR Scene -> levels[0].mainImage
        {
            auto& lvl0 = *frame.levels[0];
            toColor(lvl0.mainImage.GetImage(), VK_IMAGE_LAYOUT_UNDEFINED);
            const BrightPush push{
                1.0f / static_cast<float>(m_sceneExtent.width),
                1.0f / static_cast<float>(m_sceneExtent.height),
                cfg.threshold,
                cfg.softKnee
            };
            const VkDescriptorSet brightSet = (inputSource == BloomInputSource::HybridComposite)
                ? frame.hybridBrightInputSet
                : (inputSource == BloomInputSource::TaaResolved ? frame.taaBrightInputSet : frame.brightInputSet);
            DrawFullscreenPass(cmd, m_brightPipeline, brightSet,
                lvl0.mainImage.GetView(), lvl0.extent, &push, sizeof(push));
            toRead(lvl0.mainImage.GetImage());
        }

        // 2. Multi-Resolution Downsample Pyramid: levels[k-1].main -> levels[k].main
        for (uint32_t k = 1; k < activeLevels; ++k)
        {
            const auto& prev = *frame.levels[k - 1];
            auto& curr = *frame.levels[k];
            toColor(curr.mainImage.GetImage(), VK_IMAGE_LAYOUT_UNDEFINED);
            const DownsamplePush push{
                1.0f / static_cast<float>(prev.extent.width),
                1.0f / static_cast<float>(prev.extent.height),
                0.0f, 0.0f
            };
            DrawFullscreenPass(cmd, m_downsamplePipeline, prev.mainSampleSet,
                curr.mainImage.GetView(), curr.extent, &push, sizeof(push));
            toRead(curr.mainImage.GetImage());
        }

        // 3. Separable Gaussian Blur (Ping-Pong) at each active level (skip if inspecting raw BrightPass).
        if (cfg.debugMode != BloomDebugMode::BrightPass)
        {
            for (uint32_t k = 0; k < activeLevels; ++k)
            {
                auto& lvl = *frame.levels[k];
                const float invW = cfg.blurRadius / static_cast<float>(lvl.extent.width);
                const float invH = cfg.blurRadius / static_cast<float>(lvl.extent.height);

                // Horizontal: main -> temp
                toColor(lvl.tempImage.GetImage(), VK_IMAGE_LAYOUT_UNDEFINED);
                const BlurPush hPush{ invW, 0.0f, 0.0f, 0.0f };
                DrawFullscreenPass(cmd, m_blurPipeline, lvl.mainSampleSet,
                    lvl.tempImage.GetView(), lvl.extent, &hPush, sizeof(hPush));
                toRead(lvl.tempImage.GetImage());

                // Vertical: temp -> main
                toColor(lvl.mainImage.GetImage(), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
                const BlurPush vPush{ 0.0f, invH, 0.0f, 0.0f };
                DrawFullscreenPass(cmd, m_blurPipeline, lvl.tempSampleSet,
                    lvl.mainImage.GetView(), lvl.extent, &vPush, sizeof(vPush));
                toRead(lvl.mainImage.GetImage());
            }
        }

        // 4. Progressive Upsample & Full-Resolution Reconstruction
        if (activeLevels == 1)
        {
            const auto& lvl0 = *frame.levels[0];
            toColor(frame.reconstructed.GetImage(), VK_IMAGE_LAYOUT_UNDEFINED);
            const UpsamplePush push{
                1.0f / static_cast<float>(lvl0.extent.width),
                1.0f / static_cast<float>(lvl0.extent.height),
                cfg.scatter, 0.0f
            };
            DrawFullscreenPass(cmd, m_upsamplePipeline, lvl0.mainSampleSet,
                frame.reconstructed.GetView(), m_sceneExtent, &push, sizeof(push));
            toRead(frame.reconstructed.GetImage());
        }
        else
        {
            for (int k = static_cast<int>(activeLevels) - 2; k >= 0; --k)
            {
                auto& curr = *frame.levels[static_cast<size_t>(k)];
                const auto& smaller = *frame.levels[static_cast<size_t>(k + 1)];
                toColor(curr.tempImage.GetImage(), VK_IMAGE_LAYOUT_UNDEFINED);
                const UpsamplePush push{
                    1.0f / static_cast<float>(smaller.extent.width),
                    1.0f / static_cast<float>(smaller.extent.height),
                    cfg.scatter, 1.0f
                };
                const VkDescriptorSet upSet = (k == static_cast<int>(activeLevels) - 2)
                    ? curr.upsampleFromMainSet
                    : curr.upsampleFromTempSet;
                DrawFullscreenPass(cmd, m_upsamplePipeline, upSet,
                    curr.tempImage.GetView(), curr.extent, &push, sizeof(push));
                toRead(curr.tempImage.GetImage());
            }

            const auto& lvl0 = *frame.levels[0];
            toColor(frame.reconstructed.GetImage(), VK_IMAGE_LAYOUT_UNDEFINED);
            const UpsamplePush finalPush{
                1.0f / static_cast<float>(lvl0.extent.width),
                1.0f / static_cast<float>(lvl0.extent.height),
                cfg.scatter, 0.0f
            };
            DrawFullscreenPass(cmd, m_upsamplePipeline, lvl0.tempSampleSet,
                frame.reconstructed.GetView(), m_sceneExtent, &finalPush, sizeof(finalPush));
            toRead(frame.reconstructed.GetImage());
        }

        // 5. HDR Composite (or Debug View) -> frame.composite
        {
            VkDescriptorSet compSet = (inputSource == BloomInputSource::HybridComposite)
                ? frame.hybridCompositeSet
                : (inputSource == BloomInputSource::TaaResolved ? frame.taaCompositeSet : frame.compositeSet);
            if (cfg.debugMode == BloomDebugMode::BrightPass)
                compSet = frame.debugBrightSet;
            else if (cfg.debugMode == BloomDebugMode::DownsampleLevel)
                compSet = frame.levels[std::min(cfg.debugLevel, activeLevels - 1u)]->debugLevelSet;

            toColor(frame.composite.GetImage(), VK_IMAGE_LAYOUT_UNDEFINED);
            const CompositePush push{
                cfg.intensity,
                static_cast<uint32_t>(cfg.debugMode),
                0.0f, 0.0f
            };
            DrawFullscreenPass(cmd, m_compositePipeline, compSet,
                frame.composite.GetView(), m_sceneExtent, &push, sizeof(push));
            toRead(frame.composite.GetImage());
        }

        if (m_timestampPool != VK_NULL_HANDLE)
        {
            vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, m_timestampPool, frameIndex * 2 + 1);
            frame.hasTimestamp = true;
        }
    }

    void VulkanBloomPass::Shutdown() noexcept
    {
        if (m_device == VK_NULL_HANDLE)
            return;

        m_compositePipeline.Shutdown();
        m_upsamplePipeline.Shutdown();
        m_blurPipeline.Shutdown();
        m_downsamplePipeline.Shutdown();
        m_brightPipeline.Shutdown();

        if (m_timestampPool != VK_NULL_HANDLE)
        {
            vkDestroyQueryPool(m_device, m_timestampPool, nullptr);
            m_timestampPool = VK_NULL_HANDLE;
        }

        ReleaseFrameResources();

        if (m_passSetLayout != VK_NULL_HANDLE)
        {
            vkDestroyDescriptorSetLayout(m_device, m_passSetLayout, nullptr);
            m_passSetLayout = VK_NULL_HANDLE;
        }
        if (m_linearSampler != VK_NULL_HANDLE)
        {
            vkDestroySampler(m_device, m_linearSampler, nullptr);
            m_linearSampler = VK_NULL_HANDLE;
        }

        m_physicalDevice = VK_NULL_HANDLE;
        m_device = VK_NULL_HANDLE;
        m_memory = nullptr;
        m_framesInFlight = 0;
        m_toneMapInputLayout = VK_NULL_HANDLE;
        m_sceneExtent = {};
        m_timestampPeriodNs = 0.0f;
        m_lastGpuTimeMs = 0.0;
    }
}
