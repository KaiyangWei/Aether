#include "VulkanTaaPass.h"

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
    VulkanTaaPass::~VulkanTaaPass()
    {
        Shutdown();
    }

    VkFormat VulkanTaaPass::SelectVelocityFormat(VkPhysicalDevice physicalDevice)
    {
        if (physicalDevice == VK_NULL_HANDLE)
            return VK_FORMAT_UNDEFINED;

        constexpr VkFormat candidates[] = {
            VK_FORMAT_R16G16B16A16_SFLOAT,
            VK_FORMAT_R32G32B32A32_SFLOAT
        };
        constexpr VkFormatFeatureFlags required =
            VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT |
            VK_FORMAT_FEATURE_TRANSFER_SRC_BIT;

        for (VkFormat candidate : candidates)
        {
            VkFormatProperties props{};
            vkGetPhysicalDeviceFormatProperties(physicalDevice, candidate, &props);
            if ((props.optimalTilingFeatures & required) == required)
                return candidate;
        }
        return VK_FORMAT_UNDEFINED;
    }

    bool VulkanTaaPass::Initialize(
        const VulkanContext& context,
        const VulkanMemoryManager& memory,
        uint32_t framesInFlight,
        VkExtent2D extent,
        VkFormat depthFormat,
        VkImageView currentHdrView,
        VkImageView currentDepthView,
        VkDescriptorSetLayout toneMapInputLayout)
    {
        if (m_device != VK_NULL_HANDLE || framesInFlight < 2u ||
            extent.width == 0 || extent.height == 0 ||
            depthFormat == VK_FORMAT_UNDEFINED ||
            currentHdrView == VK_NULL_HANDLE || currentDepthView == VK_NULL_HANDLE ||
            toneMapInputLayout == VK_NULL_HANDLE)
            return false;

        m_physicalDevice = context.GetPhysicalDevice();
        m_device = context.GetDevice();
        m_memory = &memory;
        m_framesInFlight = framesInFlight;
        m_toneMapInputLayout = toneMapInputLayout;
        m_velocityFormat = SelectVelocityFormat(m_physicalDevice);
        if (m_velocityFormat == VK_FORMAT_UNDEFINED)
        {
            Shutdown();
            return false;
        }

        // Linear & Point samplers
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
        samplerInfo.magFilter = VK_FILTER_NEAREST;
        samplerInfo.minFilter = VK_FILTER_NEAREST;
        if (vkCreateSampler(m_device, &samplerInfo, nullptr, &m_pointSampler) != VK_SUCCESS)
        {
            Shutdown();
            return false;
        }

        // TAA Resolve descriptor set layout (8 bindings)
        {
            const std::array<VkDescriptorSetLayoutBinding, 8> bindings = {{
                { 0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr },
                { 1, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,  1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr },
                { 2, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,  1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr },
                { 3, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,  1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr },
                { 4, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,  1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr },
                { 5, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,  1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr },
                { 6, VK_DESCRIPTOR_TYPE_SAMPLER,        1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr },
                { 7, VK_DESCRIPTOR_TYPE_SAMPLER,        1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr }
            }};
            VkDescriptorSetLayoutCreateInfo info{};
            info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
            info.bindingCount = static_cast<uint32_t>(bindings.size());
            info.pBindings = bindings.data();
            if (vkCreateDescriptorSetLayout(m_device, &info, nullptr, &m_resolveLayout) != VK_SUCCESS)
            {
                Shutdown();
                return false;
            }
        }

        if (!CreatePipelines(depthFormat) ||
            !BuildSizeDependentResources(extent, depthFormat, currentHdrView, currentDepthView, false))
        {
            Shutdown();
            return false;
        }

        // Optional GPU timestamp queries (3 per frame: start -> post-velocity -> post-resolve)
        VkPhysicalDeviceProperties props{};
        vkGetPhysicalDeviceProperties(m_physicalDevice, &props);
        uint32_t qfCount = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(m_physicalDevice, &qfCount, nullptr);
        std::vector<VkQueueFamilyProperties> qfProps(qfCount);
        vkGetPhysicalDeviceQueueFamilyProperties(m_physicalDevice, &qfCount, qfProps.data());
        const uint32_t gfxFamily = context.GetGraphicsQueueFamily();
        if (props.limits.timestampPeriod > 0.0f &&
            gfxFamily < qfProps.size() &&
            qfProps[gfxFamily].timestampValidBits > 0)
        {
            VkQueryPoolCreateInfo qInfo{};
            qInfo.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
            qInfo.queryType = VK_QUERY_TYPE_TIMESTAMP;
            qInfo.queryCount = framesInFlight * 3u;
            if (vkCreateQueryPool(m_device, &qInfo, nullptr, &m_timestampPool) == VK_SUCCESS)
                m_timestampPeriodNs = props.limits.timestampPeriod;
        }

        return true;
    }

    bool VulkanTaaPass::CreatePipelines(VkFormat depthFormat)
    {
        const std::string shaderDir = AETHER_SHADER_DIR;

        VulkanShader fullVert;
        if (!fullVert.Initialize(m_device, shaderDir + "/bloom_fullscreen.vert.spv"))
            return false;

        VkPipelineVertexInputStateCreateInfo emptyInput{};
        emptyInput.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;

        // 1. Skybox analytical velocity pipeline (depth test disabled, matches velocity pass depthFormat)
        {
            VulkanShader skyFrag;
            if (!skyFrag.Initialize(m_device, shaderDir + "/taa_velocity_sky.frag.spv"))
                return false;
            if (!m_skyVelocityPipeline.Initialize(
                    m_device, m_velocityFormat, fullVert.GetShaderModule(), skyFrag.GetShaderModule(),
                    emptyInput, sizeof(glm::mat4), depthFormat,
                    VK_NULL_HANDLE, false, VK_NULL_HANDLE, kSkyPushStages, false))
                return false;
        }

        // 2. Opaque geometry velocity pipeline (depth test LESS_OR_EQUAL, depthWrite = false)
        {
            VulkanShader velVert, velFrag;
            if (!velVert.Initialize(m_device, shaderDir + "/taa_velocity.vert.spv") ||
                !velFrag.Initialize(m_device, shaderDir + "/taa_velocity.frag.spv"))
                return false;

            const VkVertexInputBindingDescription binding{ 0, sizeof(Vertex), VK_VERTEX_INPUT_RATE_VERTEX };
            const VkVertexInputAttributeDescription posAttr{
                0, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(Vertex, position)
            };
            VkPipelineVertexInputStateCreateInfo vertexInput{};
            vertexInput.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
            vertexInput.vertexBindingDescriptionCount = 1;
            vertexInput.pVertexBindingDescriptions = &binding;
            vertexInput.vertexAttributeDescriptionCount = 1;
            vertexInput.pVertexAttributeDescriptions = &posAttr;

            if (!m_opaqueVelocityPipeline.Initialize(
                    m_device, m_velocityFormat, velVert.GetShaderModule(), velFrag.GetShaderModule(),
                    vertexInput, sizeof(VelocityPush), depthFormat,
                    VK_NULL_HANDLE, false, VK_NULL_HANDLE, kVelPushStages, true,
                    VK_NULL_HANDLE, false, false, VK_COMPARE_OP_LESS_OR_EQUAL))
                return false;
        }

        // 3. Alpha-MASK cutout velocity pipeline (Set 0 = Material)
        {
            VulkanShader maskVert, maskFrag;
            if (!maskVert.Initialize(m_device, shaderDir + "/taa_velocity_mask.vert.spv") ||
                !maskFrag.Initialize(m_device, shaderDir + "/taa_velocity_mask.frag.spv"))
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

            const bool ok = m_maskVelocityPipeline.Initialize(
                m_device, m_velocityFormat, maskVert.GetShaderModule(), maskFrag.GetShaderModule(),
                vertexInput, sizeof(VelocityPush), depthFormat,
                materialLayout, false, VK_NULL_HANDLE, kVelPushStages, true,
                VK_NULL_HANDLE, false, false, VK_COMPARE_OP_LESS_OR_EQUAL);
            vkDestroyDescriptorSetLayout(m_device, materialLayout, nullptr);
            if (!ok)
                return false;
        }

        // 4. TAA Resolve fullscreen pipeline
        {
            VulkanShader resFrag;
            if (!resFrag.Initialize(m_device, shaderDir + "/taa_resolve.frag.spv"))
                return false;
            if (!m_resolvePipeline.Initialize(
                    m_device, kHdrFormat, fullVert.GetShaderModule(), resFrag.GetShaderModule(),
                    emptyInput, 0, VK_FORMAT_UNDEFINED,
                    m_resolveLayout, false, VK_NULL_HANDLE, VK_SHADER_STAGE_VERTEX_BIT, false))
                return false;
        }

        m_depthFormat = depthFormat;
        return true;
    }

    bool VulkanTaaPass::BuildSizeDependentResources(
        VkExtent2D extent,
        VkFormat depthFormat,
        VkImageView currentHdrView,
        VkImageView currentDepthView,
        bool injectFailure)
    {
        if (extent.width == 0 || extent.height == 0 ||
            currentHdrView == VK_NULL_HANDLE || currentDepthView == VK_NULL_HANDLE)
            return false;

        VkDescriptorPool newPool = VK_NULL_HANDLE;
        const std::array<VkDescriptorPoolSize, 3> poolSizes = {{
            { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, m_framesInFlight },
            { VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,  m_framesInFlight * 6u },
            { VK_DESCRIPTOR_TYPE_SAMPLER,        m_framesInFlight * 2u }
        }};
        VkDescriptorPoolCreateInfo poolInfo{};
        poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        poolInfo.maxSets = m_framesInFlight * 2u;
        poolInfo.poolSizeCount = static_cast<uint32_t>(poolSizes.size());
        poolInfo.pPoolSizes = poolSizes.data();
        if (vkCreateDescriptorPool(m_device, &poolInfo, nullptr, &newPool) != VK_SUCCESS)
            return false;

        auto allocSet = [&](VkDescriptorSetLayout layout, VkDescriptorSet& out) {
            VkDescriptorSetAllocateInfo alloc{};
            alloc.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
            alloc.descriptorPool = newPool;
            alloc.descriptorSetCount = 1;
            alloc.pSetLayouts = &layout;
            return vkAllocateDescriptorSets(m_device, &alloc, &out) == VK_SUCCESS;
        };

        constexpr VkImageUsageFlags colorUsage =
            VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
        constexpr VkImageUsageFlags prevDepthUsage =
            VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;

        auto newVelocity = std::make_unique<VulkanImage>();
        auto newPrevDepth = std::make_unique<VulkanImage>();
        std::vector<std::unique_ptr<VulkanImage>> newHistory;
        std::vector<std::unique_ptr<VulkanBuffer>> newParamBuffers;
        std::vector<VkDescriptorSet> newResolveSets(m_framesInFlight, VK_NULL_HANDLE);
        std::vector<VkDescriptorSet> newOutputSets(m_framesInFlight, VK_NULL_HANDLE);

        bool ok =
            newVelocity->Initialize(m_device, *m_memory, extent, m_velocityFormat, colorUsage, VK_IMAGE_ASPECT_COLOR_BIT) == VK_SUCCESS &&
            newPrevDepth->Initialize(m_device, *m_memory, extent, depthFormat, prevDepthUsage, VK_IMAGE_ASPECT_DEPTH_BIT) == VK_SUCCESS;

        if (ok && injectFailure)
            ok = false;

        for (uint32_t f = 0; f < m_framesInFlight && ok; ++f)
        {
            auto hist = std::make_unique<VulkanImage>();
            auto ubo = std::make_unique<VulkanBuffer>();
            if (hist->Initialize(m_device, *m_memory, extent, kHdrFormat, colorUsage, VK_IMAGE_ASPECT_COLOR_BIT) != VK_SUCCESS ||
                ubo->Initialize(m_device, *m_memory, sizeof(FrameParams), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                    VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT, VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) != VK_SUCCESS ||
                !allocSet(m_resolveLayout, newResolveSets[f]) ||
                !allocSet(m_toneMapInputLayout, newOutputSets[f]))
            {
                ok = false;
                break;
            }
            const FrameParams initial{};
            if (ubo->UploadData(&initial, sizeof(initial)) != VK_SUCCESS)
            {
                ok = false;
                break;
            }
            newHistory.push_back(std::move(hist));
            newParamBuffers.push_back(std::move(ubo));
        }

        if (!ok)
        {
            newHistory.clear();
            newParamBuffers.clear();
            newPrevDepth.reset();
            newVelocity.reset();
            vkDestroyDescriptorPool(m_device, newPool, nullptr);
            return false;
        }

        for (uint32_t f = 0; f < m_framesInFlight; ++f)
        {
            const uint32_t prevSlot = (f + m_framesInFlight - 1u) % m_framesInFlight;
            const VkDescriptorBufferInfo uboInfo{ newParamBuffers[f]->GetBuffer(), 0, sizeof(FrameParams) };

            const VkDescriptorImageInfo currHdrInfo{ VK_NULL_HANDLE, currentHdrView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
            const VkDescriptorImageInfo histHdrInfo{ VK_NULL_HANDLE, newHistory[prevSlot]->GetView(), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
            const VkDescriptorImageInfo velInfo{ VK_NULL_HANDLE, newVelocity->GetView(), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
            const VkDescriptorImageInfo currDepthInfo{ VK_NULL_HANDLE, currentDepthView, VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL };
            const VkDescriptorImageInfo prevDepthInfo{ VK_NULL_HANDLE, newPrevDepth->GetView(), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
            const VkDescriptorImageInfo linSamp{ m_linearSampler, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_UNDEFINED };
            const VkDescriptorImageInfo ptSamp{ m_pointSampler, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_UNDEFINED };

            const std::array<VkWriteDescriptorSet, 8> resolveWrites = {{
                { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, newResolveSets[f], 0, 0, 1, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, nullptr, &uboInfo, nullptr },
                { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, newResolveSets[f], 1, 0, 1, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, &currHdrInfo, nullptr, nullptr },
                { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, newResolveSets[f], 2, 0, 1, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, &histHdrInfo, nullptr, nullptr },
                { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, newResolveSets[f], 3, 0, 1, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, &velInfo, nullptr, nullptr },
                { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, newResolveSets[f], 4, 0, 1, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, &currDepthInfo, nullptr, nullptr },
                { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, newResolveSets[f], 5, 0, 1, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, &prevDepthInfo, nullptr, nullptr },
                { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, newResolveSets[f], 6, 0, 1, VK_DESCRIPTOR_TYPE_SAMPLER, &linSamp, nullptr, nullptr },
                { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, newResolveSets[f], 7, 0, 1, VK_DESCRIPTOR_TYPE_SAMPLER, &ptSamp, nullptr, nullptr }
            }};
            vkUpdateDescriptorSets(m_device, static_cast<uint32_t>(resolveWrites.size()), resolveWrites.data(), 0, nullptr);

            const VkDescriptorImageInfo outInfo{ VK_NULL_HANDLE, newHistory[f]->GetView(), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
            const VkWriteDescriptorSet outWrite{
                VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, newOutputSets[f],
                0, 0, 1, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, &outInfo, nullptr, nullptr
            };
            vkUpdateDescriptorSets(m_device, 1, &outWrite, 0, nullptr);
        }

        ReleaseSizeDependentResources();
        m_descriptorPool = newPool;
        m_velocity = std::move(newVelocity);
        m_prevDepth = std::move(newPrevDepth);
        m_currentDepthView = currentDepthView;
        m_history = std::move(newHistory);
        m_paramBuffers = std::move(newParamBuffers);
        m_resolveSets = std::move(newResolveSets);
        m_outputToneMapSets = std::move(newOutputSets);
        m_historyLayouts.assign(m_framesInFlight, VK_IMAGE_LAYOUT_UNDEFINED);
        m_frameHasTimestamp.assign(m_framesInFlight, false);
        m_velocityLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        m_prevDepthLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        m_extent = extent;
        ResetHistory();
        return true;
    }

    bool VulkanTaaPass::OnResize(
        VkExtent2D extent,
        VkFormat depthFormat,
        VkImageView currentHdrView,
        VkImageView currentDepthView,
        bool injectFailure)
    {
        if (m_device == VK_NULL_HANDLE)
            return false;

        if (depthFormat != m_depthFormat)
        {
            m_skyVelocityPipeline.Shutdown();
            m_opaqueVelocityPipeline.Shutdown();
            m_maskVelocityPipeline.Shutdown();
            m_resolvePipeline.Shutdown();
            if (!CreatePipelines(depthFormat))
                return false;
        }
        return BuildSizeDependentResources(extent, depthFormat, currentHdrView, currentDepthView, injectFailure);
    }

    void VulkanTaaPass::ResetHistory()
    {
        m_lastHistorySlot = UINT32_MAX;
    }

    void VulkanTaaPass::OnFrameSubmitted(uint32_t frameIndex)
    {
        m_lastHistorySlot = frameIndex;
        m_frameHasTimestamp[frameIndex] = m_timestampPool != VK_NULL_HANDLE;
    }

    void VulkanTaaPass::ReleaseSizeDependentResources() noexcept
    {
        m_history.clear();
        m_paramBuffers.clear();
        m_resolveSets.clear();
        m_outputToneMapSets.clear();
        m_historyLayouts.clear();
        m_frameHasTimestamp.clear();
        m_prevDepth.reset();
        m_velocity.reset();
        m_currentDepthView = VK_NULL_HANDLE;
        m_velocityLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        m_prevDepthLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        if (m_descriptorPool != VK_NULL_HANDLE)
        {
            vkDestroyDescriptorPool(m_device, m_descriptorPool, nullptr);
            m_descriptorPool = VK_NULL_HANDLE;
        }
    }

    void VulkanTaaPass::TransitionImage(
        VkCommandBuffer cmd,
        VkImage image,
        VkImageAspectFlags aspect,
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
        barrier.subresourceRange = { aspect, 0, 1, 0, 1 };

        VkDependencyInfo dep{};
        dep.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
        dep.imageMemoryBarrierCount = 1;
        dep.pImageMemoryBarriers = &barrier;
        vkCmdPipelineBarrier2(cmd, &dep);
    }

    bool VulkanTaaPass::Execute(
        VkCommandBuffer cmd,
        uint32_t frameIndex,
        VkImage currentDepthImage,
        std::span<const VelocityDrawItem> items,
        const glm::mat4& currClipToPrevClipRot,
        const glm::vec2& jitterDeltaUv,
        const TaaConfig& rawConfig)
    {
        if (m_device == VK_NULL_HANDLE || frameIndex >= m_framesInFlight || !m_velocity || !m_prevDepth)
            return false;

        const TaaConfig cfg = SanitizeTaaConfig(rawConfig);
        const uint32_t prevSlot = (frameIndex + m_framesInFlight - 1u) % m_framesInFlight;
        const bool useHistory = m_lastHistorySlot == prevSlot && !cfg.resetHistory;

        if (m_timestampPool != VK_NULL_HANDLE && m_frameHasTimestamp[frameIndex])
        {
            uint64_t ts[3]{};
            if (vkGetQueryPoolResults(
                    m_device, m_timestampPool, frameIndex * 3u, 3u,
                    sizeof(ts), ts, sizeof(uint64_t), VK_QUERY_RESULT_64_BIT) == VK_SUCCESS &&
                ts[2] >= ts[1] && ts[1] >= ts[0])
            {
                m_lastVelocityGpuMs = static_cast<double>(ts[1] - ts[0]) * m_timestampPeriodNs * 1e-6;
                m_lastResolveGpuMs = static_cast<double>(ts[2] - ts[1]) * m_timestampPeriodNs * 1e-6;
            }
            m_frameHasTimestamp[frameIndex] = false;
        }

        const FrameParams ubo{
            glm::vec4(jitterDeltaUv.x, jitterDeltaUv.y,
                1.0f / static_cast<float>(m_extent.width), 1.0f / static_cast<float>(m_extent.height)),
            glm::vec4(cfg.historyWeight, cfg.minHistoryWeight, cfg.maxHistoryWeight, useHistory ? 1.0f : 0.0f),
            glm::vec4(cfg.motionRejectionScale, cfg.depthRejectionThreshold, cfg.varianceClampGamma, 0.0f)
        };
        if (m_paramBuffers[frameIndex]->UploadData(&ubo, sizeof(ubo)) != VK_SUCCESS)
            return false;

        if (m_timestampPool != VK_NULL_HANDLE)
        {
            vkCmdResetQueryPool(cmd, m_timestampPool, frameIndex * 3u, 3u);
            vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, m_timestampPool, frameIndex * 3u);
        }

        constexpr VkPipelineStageFlags2 depthStages =
            VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT;

        // ============================================================
        // 1. Motion Vector Pass (Skybox analytical + Opaque/Mask geometry)
        // ============================================================
        TransitionImage(
            cmd, m_velocity->GetImage(), VK_IMAGE_ASPECT_COLOR_BIT,
            m_velocityLayout, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
            m_velocityLayout == VK_IMAGE_LAYOUT_UNDEFINED ? VK_PIPELINE_STAGE_2_NONE :
                VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
            m_velocityLayout == VK_IMAGE_LAYOUT_UNDEFINED ? 0 :
                VK_ACCESS_2_SHADER_SAMPLED_READ_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
            VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
            VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);

        TransitionImage(
            cmd, currentDepthImage, VK_IMAGE_ASPECT_DEPTH_BIT,
            VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
            VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL,
            depthStages, VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
            depthStages | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
            VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);

        {
            VkRenderingAttachmentInfo colorAtt{};
            colorAtt.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
            colorAtt.imageView = m_velocity->GetView();
            colorAtt.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            colorAtt.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
            colorAtt.storeOp = VK_ATTACHMENT_STORE_OP_STORE;

            VkRenderingAttachmentInfo depthAtt{};
            depthAtt.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
            depthAtt.imageView = m_currentDepthView;
            depthAtt.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
            depthAtt.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
            depthAtt.storeOp = VK_ATTACHMENT_STORE_OP_NONE;

            VkRenderingInfo renderInfo{};
            renderInfo.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
            renderInfo.renderArea.extent = m_extent;
            renderInfo.layerCount = 1;
            renderInfo.colorAttachmentCount = 1;
            renderInfo.pColorAttachments = &colorAtt;
            renderInfo.pDepthAttachment = &depthAtt;

            vkCmdBeginRendering(cmd, &renderInfo);

            const VkViewport vp{ 0.0f, 0.0f, static_cast<float>(m_extent.width), static_cast<float>(m_extent.height), 0.0f, 1.0f };
            const VkRect2D sc{ { 0, 0 }, m_extent };
            vkCmdSetViewport(cmd, 0, 1, &vp);
            vkCmdSetScissor(cmd, 0, 1, &sc);

            // 1a. Skybox analytical rotation-only velocity
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_skyVelocityPipeline.GetPipeline());
            vkCmdSetCullMode(cmd, VK_CULL_MODE_NONE);
            vkCmdSetFrontFace(cmd, VK_FRONT_FACE_COUNTER_CLOCKWISE);
            vkCmdPushConstants(cmd, m_skyVelocityPipeline.GetPipelineLayout(), kSkyPushStages,
                0, sizeof(currClipToPrevClipRot), &currClipToPrevClipRot);
            vkCmdDraw(cmd, 3, 1, 0, 0);

            // 1b. Opaque & Alpha-MASK scene geometry velocity (AlphaMode::Blend skipped to avoid corrupting opaque depth/velocity)
            VkPipeline boundPipeline = VK_NULL_HANDLE;
            const VulkanMesh* boundMesh = nullptr;

            for (const auto& item : items)
            {
                if (!item.mesh || item.alphaBlend)
                    continue;

                const bool useMask = item.alphaMask && item.material != VK_NULL_HANDLE;
                const auto& pipeline = useMask ? m_maskVelocityPipeline : m_opaqueVelocityPipeline;
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

                const auto layout = pipeline.GetPipelineLayout();
                if (useMask)
                {
                    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 0, 1, &item.material, 0, nullptr);
                }

                const VelocityPush push{ item.currMvpJittered, item.prevMvpJittered };
                vkCmdPushConstants(cmd, layout, kVelPushStages, 0, sizeof(push), &push);
                vkCmdDrawIndexed(cmd, boundMesh->GetIndexCount(), 1, 0, 0, 0);
            }

            vkCmdEndRendering(cmd);
        }

        TransitionImage(
            cmd, m_velocity->GetImage(), VK_IMAGE_ASPECT_COLOR_BIT,
            VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
            VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
        m_velocityLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

        if (m_timestampPool != VK_NULL_HANDLE)
            vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, m_timestampPool, frameIndex * 3u + 1u);

        // ============================================================
        // 2. TAA Temporal Resolve Pass -> m_history[frameIndex]
        // ============================================================
        if (m_historyLayouts[prevSlot] == VK_IMAGE_LAYOUT_UNDEFINED)
        {
            TransitionImage(
                cmd, m_history[prevSlot]->GetImage(), VK_IMAGE_ASPECT_COLOR_BIT,
                VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                VK_PIPELINE_STAGE_2_NONE, 0,
                VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
            m_historyLayouts[prevSlot] = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        }
        if (m_prevDepthLayout == VK_IMAGE_LAYOUT_UNDEFINED)
        {
            TransitionImage(
                cmd, m_prevDepth->GetImage(), VK_IMAGE_ASPECT_DEPTH_BIT,
                VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                VK_PIPELINE_STAGE_2_NONE, 0,
                VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
            m_prevDepthLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        }

        TransitionImage(
            cmd, m_history[frameIndex]->GetImage(), VK_IMAGE_ASPECT_COLOR_BIT,
            m_historyLayouts[frameIndex], VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
            m_historyLayouts[frameIndex] == VK_IMAGE_LAYOUT_UNDEFINED ? VK_PIPELINE_STAGE_2_NONE :
                VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
            m_historyLayouts[frameIndex] == VK_IMAGE_LAYOUT_UNDEFINED ? 0 :
                VK_ACCESS_2_SHADER_SAMPLED_READ_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
            VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);

        {
            VkRenderingAttachmentInfo colorAtt{};
            colorAtt.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
            colorAtt.imageView = m_history[frameIndex]->GetView();
            colorAtt.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            colorAtt.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
            colorAtt.storeOp = VK_ATTACHMENT_STORE_OP_STORE;

            VkRenderingInfo renderInfo{};
            renderInfo.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
            renderInfo.renderArea.extent = m_extent;
            renderInfo.layerCount = 1;
            renderInfo.colorAttachmentCount = 1;
            renderInfo.pColorAttachments = &colorAtt;

            vkCmdBeginRendering(cmd, &renderInfo);
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_resolvePipeline.GetPipeline());
            vkCmdSetCullMode(cmd, VK_CULL_MODE_NONE);
            vkCmdSetFrontFace(cmd, VK_FRONT_FACE_COUNTER_CLOCKWISE);
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                m_resolvePipeline.GetPipelineLayout(), 0, 1, &m_resolveSets[frameIndex], 0, nullptr);
            vkCmdDraw(cmd, 3, 1, 0, 0);
            vkCmdEndRendering(cmd);
        }

        TransitionImage(
            cmd, m_history[frameIndex]->GetImage(), VK_IMAGE_ASPECT_COLOR_BIT,
            VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
            VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
        m_historyLayouts[frameIndex] = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

        // ============================================================
        // 3. Copy Current Depth -> m_prevDepth & restore currentDepth layout
        // ============================================================
        TransitionImage(
            cmd, currentDepthImage, VK_IMAGE_ASPECT_DEPTH_BIT,
            VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            depthStages | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
            VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
            VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);

        TransitionImage(
            cmd, m_prevDepth->GetImage(), VK_IMAGE_ASPECT_DEPTH_BIT,
            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_COPY_BIT,
            VK_ACCESS_2_SHADER_SAMPLED_READ_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT,
            VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);

        VkImageCopy copyRegion{};
        copyRegion.srcSubresource = { VK_IMAGE_ASPECT_DEPTH_BIT, 0, 0, 1 };
        copyRegion.dstSubresource = { VK_IMAGE_ASPECT_DEPTH_BIT, 0, 0, 1 };
        copyRegion.extent = { m_extent.width, m_extent.height, 1 };
        vkCmdCopyImage(
            cmd,
            currentDepthImage, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            m_prevDepth->GetImage(), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            1, &copyRegion);

        TransitionImage(
            cmd, m_prevDepth->GetImage(), VK_IMAGE_ASPECT_DEPTH_BIT,
            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
            VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
        m_prevDepthLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

        TransitionImage(
            cmd, currentDepthImage, VK_IMAGE_ASPECT_DEPTH_BIT,
            VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
            VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT,
            depthStages, VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT);

        if (m_timestampPool != VK_NULL_HANDLE)
        {
            vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, m_timestampPool, frameIndex * 3u + 2u);
        }

        return true;
    }

    void VulkanTaaPass::Shutdown() noexcept
    {
        if (m_device == VK_NULL_HANDLE)
            return;

        m_resolvePipeline.Shutdown();
        m_maskVelocityPipeline.Shutdown();
        m_opaqueVelocityPipeline.Shutdown();
        m_skyVelocityPipeline.Shutdown();

        if (m_timestampPool != VK_NULL_HANDLE)
        {
            vkDestroyQueryPool(m_device, m_timestampPool, nullptr);
            m_timestampPool = VK_NULL_HANDLE;
        }

        ReleaseSizeDependentResources();

        if (m_resolveLayout != VK_NULL_HANDLE)
        {
            vkDestroyDescriptorSetLayout(m_device, m_resolveLayout, nullptr);
            m_resolveLayout = VK_NULL_HANDLE;
        }
        if (m_pointSampler != VK_NULL_HANDLE)
        {
            vkDestroySampler(m_device, m_pointSampler, nullptr);
            m_pointSampler = VK_NULL_HANDLE;
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
        m_depthFormat = VK_FORMAT_UNDEFINED;
        m_velocityFormat = VK_FORMAT_UNDEFINED;
        m_extent = {};
        ResetHistory();
        m_timestampPeriodNs = 0.0f;
        m_lastVelocityGpuMs = 0.0;
        m_lastResolveGpuMs = 0.0;
    }
}
