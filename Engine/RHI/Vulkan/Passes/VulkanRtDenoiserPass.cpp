#include "VulkanRtDenoiserPass.h"

#include <RHI/Vulkan/Assets/VulkanEnvironment.h>
#include <RHI/Vulkan/Passes/VulkanHybridGBufferPass.h>
#include <RHI/Vulkan/Passes/VulkanRtReflectionPass.h>
#include <RHI/Vulkan/Passes/VulkanRtShadowPass.h>
#include <RHI/Vulkan/Pipeline/VulkanShader.h>
#include <RHI/Vulkan/RayTracing/VulkanRayTracingScene.h>
#include <RHI/Vulkan/VulkanContext.h>
#include <RHI/Vulkan/VulkanMemoryManager.h>

#include <algorithm>
#include <iostream>
#include <string>

namespace Aether
{
    namespace
    {
        constexpr uint32_t kDenoiserQueriesPerFrame = 4; // 0: temporalStart, 1: temporalEnd/atrousStart, 2: atrousEnd/compStart, 3: compEnd
    }

    VulkanRtDenoiserPass::~VulkanRtDenoiserPass()
    {
        Shutdown();
    }

    bool VulkanRtDenoiserPass::Initialize(
        const VulkanContext& context,
        const VulkanMemoryManager& memory,
        uint32_t framesInFlight,
        VkExtent2D extent,
        VkDescriptorSetLayout toneMapSetLayout)
    {
        if (m_initialized || framesInFlight == 0 || extent.width == 0 || extent.height == 0 ||
            context.GetDevice() == VK_NULL_HANDLE || toneMapSetLayout == VK_NULL_HANDLE)
            return false;

        m_physicalDevice = context.GetPhysicalDevice();
        m_device = context.GetDevice();
        m_memory = &memory;
        m_extent = extent;
        m_toneMapLayout = toneMapSetLayout;
        m_sizeVersion = 1;
        m_lastHistorySlot = UINT32_MAX;

        if (VulkanEnvironment::CreateLayout(m_device, m_environmentLayout) != VK_SUCCESS)
        {
            Shutdown();
            return false;
        }

        // Temporal layout: b0, t1..t9 (9 sampled), t10 (storage buffer), u11..u14 (4 storage images)
        {
            const std::array<VkDescriptorSetLayoutBinding, 15> b = {{
                {  0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr },
                {  1, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,  1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr },
                {  2, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,  1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr },
                {  3, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,  1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr },
                {  4, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,  1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr },
                {  5, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,  1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr },
                {  6, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,  1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr },
                {  7, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,  1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr },
                {  8, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,  1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr },
                {  9, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,  1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr },
                { 10, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr },
                { 11, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,  1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr },
                { 12, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,  1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr },
                { 13, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,  1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr },
                { 14, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,  1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr }
            }};
            VkDescriptorSetLayoutCreateInfo info{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
            info.bindingCount = static_cast<uint32_t>(b.size());
            info.pBindings = b.data();
            if (vkCreateDescriptorSetLayout(m_device, &info, nullptr, &m_temporalLayout) != VK_SUCCESS)
            {
                Shutdown();
                return false;
            }
        }

        // Atrous layout: t0..t4 (5 sampled), u5..u6 (2 storage)
        {
            const std::array<VkDescriptorSetLayoutBinding, 7> b = {{
                { 0, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr },
                { 1, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr },
                { 2, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr },
                { 3, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr },
                { 4, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr },
                { 5, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr },
                { 6, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr }
            }};
            VkDescriptorSetLayoutCreateInfo info{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
            info.bindingCount = static_cast<uint32_t>(b.size());
            info.pBindings = b.data();
            if (vkCreateDescriptorSetLayout(m_device, &info, nullptr, &m_atrousLayout) != VK_SUCCESS)
            {
                Shutdown();
                return false;
            }
        }

        // Composite layout: b0, t1..t9 (9 sampled), u10 (1 storage)
        {
            const std::array<VkDescriptorSetLayoutBinding, 11> b = {{
                {  0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr },
                {  1, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,  1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr },
                {  2, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,  1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr },
                {  3, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,  1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr },
                {  4, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,  1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr },
                {  5, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,  1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr },
                {  6, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,  1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr },
                {  7, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,  1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr },
                {  8, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,  1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr },
                {  9, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,  1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr },
                { 10, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,  1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr }
            }};
            VkDescriptorSetLayoutCreateInfo info{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
            info.bindingCount = static_cast<uint32_t>(b.size());
            info.pBindings = b.data();
            if (vkCreateDescriptorSetLayout(m_device, &info, nullptr, &m_compositeLayout) != VK_SUCCESS)
            {
                Shutdown();
                return false;
            }
        }

        const std::array<VkDescriptorPoolSize, 4> poolSizes = {{
            { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, framesInFlight * 4u },
            { VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,  framesInFlight * (9u + 15u + 27u + 1u) },
            { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, framesInFlight * 1u },
            { VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,  framesInFlight * (4u + 6u + 3u) }
        }};
        VkDescriptorPoolCreateInfo poolInfo{ VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
        poolInfo.maxSets = framesInFlight * 8u;
        poolInfo.poolSizeCount = static_cast<uint32_t>(poolSizes.size());
        poolInfo.pPoolSizes = poolSizes.data();
        if (vkCreateDescriptorPool(m_device, &poolInfo, nullptr, &m_descriptorPool) != VK_SUCCESS)
        {
            Shutdown();
            return false;
        }

        if (!CreatePipelines())
        {
            Shutdown();
            return false;
        }

        m_frames.clear();
        m_frames.reserve(framesInFlight);
        auto allocSet = [&](VkDescriptorSetLayout layout, VkDescriptorSet& out) -> bool {
            VkDescriptorSetAllocateInfo info{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
            info.descriptorPool = m_descriptorPool;
            info.descriptorSetCount = 1;
            info.pSetLayouts = &layout;
            return vkAllocateDescriptorSets(m_device, &info, &out) == VK_SUCCESS;
        };

        for (uint32_t i = 0; i < framesInFlight; ++i)
        {
            auto f = std::make_unique<FrameResources>();
            if (!allocSet(m_temporalLayout, f->temporalSet) ||
                !allocSet(m_atrousLayout, f->atrousFromHistSet) ||
                !allocSet(m_atrousLayout, f->atrous0To1Set) ||
                !allocSet(m_atrousLayout, f->atrous1To0Set) ||
                !allocSet(m_compositeLayout, f->compositeFromHistSet) ||
                !allocSet(m_compositeLayout, f->compositeFromScratch0Set) ||
                !allocSet(m_compositeLayout, f->compositeFromScratch1Set) ||
                !allocSet(m_toneMapLayout, f->toneMapSet) ||
                f->temporalParamsBuffer.Initialize(
                    m_device, *m_memory, sizeof(TemporalParamsUbo),
                    VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                    VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT,
                    VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) != VK_SUCCESS ||
                f->compositeParamsBuffer.Initialize(
                    m_device, *m_memory, sizeof(CompositeParamsUbo),
                    VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                    VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT,
                    VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) != VK_SUCCESS)
            {
                Shutdown();
                return false;
            }
            m_frames.push_back(std::move(f));
        }

        if (!BuildSizeDependentResources(extent))
        {
            Shutdown();
            return false;
        }

        VkPhysicalDeviceProperties props{};
        vkGetPhysicalDeviceProperties(m_physicalDevice, &props);
        uint32_t qCount = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(m_physicalDevice, &qCount, nullptr);
        std::vector<VkQueueFamilyProperties> qProps(qCount);
        vkGetPhysicalDeviceQueueFamilyProperties(m_physicalDevice, &qCount, qProps.data());
        const uint32_t gfxFamily = context.GetGraphicsQueueFamily();
        if (props.limits.timestampPeriod > 0.0f &&
            gfxFamily < qProps.size() &&
            qProps[gfxFamily].timestampValidBits > 0)
        {
            VkQueryPoolCreateInfo qInfo{ VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO };
            qInfo.queryType = VK_QUERY_TYPE_TIMESTAMP;
            qInfo.queryCount = framesInFlight * kDenoiserQueriesPerFrame;
            if (vkCreateQueryPool(m_device, &qInfo, nullptr, &m_timestampPool) == VK_SUCCESS)
                m_timestampPeriodNs = static_cast<double>(props.limits.timestampPeriod);
        }

        m_initialized = true;
        return true;
    }

    bool VulkanRtDenoiserPass::CreatePipelines()
    {
        const std::string shaderDir = AETHER_SHADER_DIR;
        VulkanShader temporalShader, atrousShader, compositeShader;
        if (!temporalShader.Initialize(m_device, shaderDir + "/rt_denoise_temporal.comp.spv") ||
            !atrousShader.Initialize(m_device, shaderDir + "/rt_denoise_atrous.comp.spv") ||
            !compositeShader.Initialize(m_device, shaderDir + "/hybrid_composite.comp.spv"))
        {
            std::cerr << "VulkanRtDenoiserPass: failed to load denoiser/composite compute shaders.\n";
            return false;
        }

        // Temporal pipeline
        {
            VkPipelineLayoutCreateInfo plInfo{ VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
            plInfo.setLayoutCount = 1;
            plInfo.pSetLayouts = &m_temporalLayout;
            if (vkCreatePipelineLayout(m_device, &plInfo, nullptr, &m_temporalPipelineLayout) != VK_SUCCESS)
                return false;

            VkComputePipelineCreateInfo pInfo{ VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO };
            pInfo.layout = m_temporalPipelineLayout;
            pInfo.stage = { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0,
                VK_SHADER_STAGE_COMPUTE_BIT, temporalShader.GetShaderModule(), "main", nullptr };
            if (vkCreateComputePipelines(m_device, VK_NULL_HANDLE, 1, &pInfo, nullptr, &m_temporalPipeline) != VK_SUCCESS)
                return false;
        }

        // À-Trous pipeline
        {
            const VkPushConstantRange pushRange{ VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(AtrousPushConstants) };
            VkPipelineLayoutCreateInfo plInfo{ VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
            plInfo.setLayoutCount = 1;
            plInfo.pSetLayouts = &m_atrousLayout;
            plInfo.pushConstantRangeCount = 1;
            plInfo.pPushConstantRanges = &pushRange;
            if (vkCreatePipelineLayout(m_device, &plInfo, nullptr, &m_atrousPipelineLayout) != VK_SUCCESS)
                return false;

            VkComputePipelineCreateInfo pInfo{ VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO };
            pInfo.layout = m_atrousPipelineLayout;
            pInfo.stage = { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0,
                VK_SHADER_STAGE_COMPUTE_BIT, atrousShader.GetShaderModule(), "main", nullptr };
            if (vkCreateComputePipelines(m_device, VK_NULL_HANDLE, 1, &pInfo, nullptr, &m_atrousPipeline) != VK_SUCCESS)
                return false;
        }

        // Composite pipeline
        {
            const std::array<VkDescriptorSetLayout, 2> compLayouts = { m_compositeLayout, m_environmentLayout };
            VkPipelineLayoutCreateInfo plInfo{ VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
            plInfo.setLayoutCount = static_cast<uint32_t>(compLayouts.size());
            plInfo.pSetLayouts = compLayouts.data();
            if (vkCreatePipelineLayout(m_device, &plInfo, nullptr, &m_compositePipelineLayout) != VK_SUCCESS)
                return false;

            VkComputePipelineCreateInfo pInfo{ VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO };
            pInfo.layout = m_compositePipelineLayout;
            pInfo.stage = { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0,
                VK_SHADER_STAGE_COMPUTE_BIT, compositeShader.GetShaderModule(), "main", nullptr };
            if (vkCreateComputePipelines(m_device, VK_NULL_HANDLE, 1, &pInfo, nullptr, &m_compositePipeline) != VK_SUCCESS)
                return false;
        }

        return true;
    }

    bool VulkanRtDenoiserPass::BuildSizeDependentResources(VkExtent2D extent)
    {
        constexpr VkImageUsageFlags kHistUsage =
            VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
            VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
        constexpr VkImageUsageFlags kScratchUsage =
            VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
            VK_IMAGE_USAGE_TRANSFER_SRC_BIT;

        for (auto& framePtr : m_frames)
        {
            auto& f = *framePtr;
            f.shadowHistory.Shutdown();
            f.reflHistory.Shutdown();
            f.momentsHistory.Shutdown();
            f.historyLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            for (int i = 0; i < 2; ++i)
            {
                f.atrousShadow[i].Shutdown();
                f.atrousRefl[i].Shutdown();
            }
            f.varConfImage.Shutdown();
            f.compositeImage.Shutdown();

            if (f.shadowHistory.Initialize(m_device, *m_memory, extent, VK_FORMAT_R16G16B16A16_SFLOAT, kHistUsage, VK_IMAGE_ASPECT_COLOR_BIT) != VK_SUCCESS ||
                f.reflHistory.Initialize(m_device, *m_memory, extent, VK_FORMAT_R16G16B16A16_SFLOAT, kHistUsage, VK_IMAGE_ASPECT_COLOR_BIT) != VK_SUCCESS ||
                f.momentsHistory.Initialize(m_device, *m_memory, extent, VK_FORMAT_R16G16B16A16_SFLOAT, kHistUsage, VK_IMAGE_ASPECT_COLOR_BIT) != VK_SUCCESS ||
                f.atrousShadow[0].Initialize(m_device, *m_memory, extent, VK_FORMAT_R16G16B16A16_SFLOAT, kScratchUsage, VK_IMAGE_ASPECT_COLOR_BIT) != VK_SUCCESS ||
                f.atrousShadow[1].Initialize(m_device, *m_memory, extent, VK_FORMAT_R16G16B16A16_SFLOAT, kScratchUsage, VK_IMAGE_ASPECT_COLOR_BIT) != VK_SUCCESS ||
                f.atrousRefl[0].Initialize(m_device, *m_memory, extent, VK_FORMAT_R16G16B16A16_SFLOAT, kScratchUsage, VK_IMAGE_ASPECT_COLOR_BIT) != VK_SUCCESS ||
                f.atrousRefl[1].Initialize(m_device, *m_memory, extent, VK_FORMAT_R16G16B16A16_SFLOAT, kScratchUsage, VK_IMAGE_ASPECT_COLOR_BIT) != VK_SUCCESS ||
                f.varConfImage.Initialize(m_device, *m_memory, extent, VK_FORMAT_R16G16_SFLOAT, kScratchUsage, VK_IMAGE_ASPECT_COLOR_BIT) != VK_SUCCESS ||
                f.compositeImage.Initialize(m_device, *m_memory, extent, VK_FORMAT_R16G16B16A16_SFLOAT, kScratchUsage, VK_IMAGE_ASPECT_COLOR_BIT) != VK_SUCCESS)
                return false;

            const VkDescriptorImageInfo toneMapInfo{
                VK_NULL_HANDLE, f.compositeImage.GetView(), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
            };
            const VkWriteDescriptorSet tmWrite{
                VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, f.toneMapSet,
                0, 0, 1, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, &toneMapInfo, nullptr, nullptr
            };
            vkUpdateDescriptorSets(m_device, 1, &tmWrite, 0, nullptr);
        }

        m_extent = extent;
        m_lastHistorySlot = UINT32_MAX;
        ++m_sizeVersion;
        return true;
    }

    bool VulkanRtDenoiserPass::OnResize(VkExtent2D newExtent)
    {
        if (!m_initialized || newExtent.width == 0 || newExtent.height == 0)
            return false;
        return BuildSizeDependentResources(newExtent);
    }

    void VulkanRtDenoiserPass::ResetHistory() noexcept
    {
        m_lastHistorySlot = UINT32_MAX;
    }

    void VulkanRtDenoiserPass::Shutdown() noexcept
    {
        auto destroyPipeline = [&](VkPipeline& p, VkPipelineLayout& pl) {
            if (p != VK_NULL_HANDLE && m_device != VK_NULL_HANDLE)
            {
                vkDestroyPipeline(m_device, p, nullptr);
                p = VK_NULL_HANDLE;
            }
            if (pl != VK_NULL_HANDLE && m_device != VK_NULL_HANDLE)
            {
                vkDestroyPipelineLayout(m_device, pl, nullptr);
                pl = VK_NULL_HANDLE;
            }
        };
        destroyPipeline(m_temporalPipeline, m_temporalPipelineLayout);
        destroyPipeline(m_atrousPipeline, m_atrousPipelineLayout);
        destroyPipeline(m_compositePipeline, m_compositePipelineLayout);

        if (m_descriptorPool != VK_NULL_HANDLE && m_device != VK_NULL_HANDLE)
        {
            vkDestroyDescriptorPool(m_device, m_descriptorPool, nullptr);
            m_descriptorPool = VK_NULL_HANDLE;
        }
        auto destroyLayout = [&](VkDescriptorSetLayout& l) {
            if (l != VK_NULL_HANDLE && m_device != VK_NULL_HANDLE)
            {
                vkDestroyDescriptorSetLayout(m_device, l, nullptr);
                l = VK_NULL_HANDLE;
            }
        };
        destroyLayout(m_temporalLayout);
        destroyLayout(m_atrousLayout);
        destroyLayout(m_compositeLayout);
        destroyLayout(m_environmentLayout);

        if (m_timestampPool != VK_NULL_HANDLE && m_device != VK_NULL_HANDLE)
        {
            vkDestroyQueryPool(m_device, m_timestampPool, nullptr);
            m_timestampPool = VK_NULL_HANDLE;
        }

        m_frames.clear();
        m_physicalDevice = VK_NULL_HANDLE;
        m_device = VK_NULL_HANDLE;
        m_memory = nullptr;
        m_toneMapLayout = VK_NULL_HANDLE;
        m_extent = {};
        m_sizeVersion = 1;
        m_lastHistorySlot = UINT32_MAX;
        m_timestampPeriodNs = 0.0;
        m_lastTemporalGpuTimeMs = 0.0;
        m_lastAtrousGpuTimeMs = 0.0;
        m_lastCompositeGpuTimeMs = 0.0;
        m_initialized = false;
    }

    void VulkanRtDenoiserPass::OnFrameFenceCompleted(uint32_t frameIndex)
    {
        if (!m_initialized || frameIndex >= m_frames.size())
            return;
        auto& f = *m_frames[frameIndex];
        if (m_timestampPool != VK_NULL_HANDLE && f.hasPendingTimestamps && f.recordedPass)
        {
            std::array<uint64_t, kDenoiserQueriesPerFrame> stamps{};
            if (vkGetQueryPoolResults(
                    m_device, m_timestampPool, frameIndex * kDenoiserQueriesPerFrame, kDenoiserQueriesPerFrame,
                    sizeof(stamps), stamps.data(), sizeof(uint64_t), VK_QUERY_RESULT_64_BIT) == VK_SUCCESS)
            {
                const double toMs = m_timestampPeriodNs * 1e-6;
                if (stamps[1] >= stamps[0])
                    m_lastTemporalGpuTimeMs = static_cast<double>(stamps[1] - stamps[0]) * toMs;
                if (stamps[2] >= stamps[1])
                    m_lastAtrousGpuTimeMs = static_cast<double>(stamps[2] - stamps[1]) * toMs;
                if (stamps[3] >= stamps[2])
                    m_lastCompositeGpuTimeMs = static_cast<double>(stamps[3] - stamps[2]) * toMs;
            }
        }
        f.hasPendingTimestamps = false;
        f.recordedPass = false;
    }

    void VulkanRtDenoiserPass::OnFrameSubmitted(uint32_t frameIndex)
    {
        if (!m_initialized || frameIndex >= m_frames.size())
            return;
        auto& f = *m_frames[frameIndex];
        if (f.recordedPass)
        {
            m_lastHistorySlot = frameIndex;
            f.hasPendingTimestamps = (m_timestampPool != VK_NULL_HANDLE);
        }
    }

    void VulkanRtDenoiserPass::UpdateStaticFrameDescriptors(
        FrameResources& frame,
        uint32_t frameIndex,
        const VulkanHybridGBufferPass& gBufferPass,
        const VulkanRtShadowPass& rtShadowPass,
        const VulkanRtReflectionPass& rtReflectionPass)
    {
        const VkDescriptorImageInfo rt0{ VK_NULL_HANDLE, gBufferPass.GetRt0View(frameIndex), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
        const VkDescriptorImageInfo rt1{ VK_NULL_HANDLE, gBufferPass.GetRt1View(frameIndex), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
        const VkDescriptorImageInfo rt2{ VK_NULL_HANDLE, gBufferPass.GetRt2View(frameIndex), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
        const VkDescriptorImageInfo rt3{ VK_NULL_HANDLE, gBufferPass.GetRt3View(frameIndex), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
        const VkDescriptorImageInfo varConf{ VK_NULL_HANDLE, frame.varConfImage.GetView(), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };

        const VkDescriptorImageInfo shHistRead{ VK_NULL_HANDLE, frame.shadowHistory.GetView(), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
        const VkDescriptorImageInfo rfHistRead{ VK_NULL_HANDLE, frame.reflHistory.GetView(), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
        const VkDescriptorImageInfo sh0Read{ VK_NULL_HANDLE, frame.atrousShadow[0].GetView(), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
        const VkDescriptorImageInfo rf0Read{ VK_NULL_HANDLE, frame.atrousRefl[0].GetView(), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
        const VkDescriptorImageInfo sh1Read{ VK_NULL_HANDLE, frame.atrousShadow[1].GetView(), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
        const VkDescriptorImageInfo rf1Read{ VK_NULL_HANDLE, frame.atrousRefl[1].GetView(), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };

        const VkDescriptorImageInfo sh0Write{ VK_NULL_HANDLE, frame.atrousShadow[0].GetView(), VK_IMAGE_LAYOUT_GENERAL };
        const VkDescriptorImageInfo rf0Write{ VK_NULL_HANDLE, frame.atrousRefl[0].GetView(), VK_IMAGE_LAYOUT_GENERAL };
        const VkDescriptorImageInfo sh1Write{ VK_NULL_HANDLE, frame.atrousShadow[1].GetView(), VK_IMAGE_LAYOUT_GENERAL };
        const VkDescriptorImageInfo rf1Write{ VK_NULL_HANDLE, frame.atrousRefl[1].GetView(), VK_IMAGE_LAYOUT_GENERAL };

        auto writeAtrous = [&](VkDescriptorSet dst, const VkDescriptorImageInfo& sIn, const VkDescriptorImageInfo& rIn,
                               const VkDescriptorImageInfo& sOut, const VkDescriptorImageInfo& rOut) {
            const std::array<VkWriteDescriptorSet, 7> w = {{
                { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, dst, 0, 0, 1, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, &rt1, nullptr, nullptr },
                { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, dst, 1, 0, 1, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, &rt3, nullptr, nullptr },
                { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, dst, 2, 0, 1, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, &sIn, nullptr, nullptr },
                { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, dst, 3, 0, 1, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, &rIn, nullptr, nullptr },
                { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, dst, 4, 0, 1, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, &varConf, nullptr, nullptr },
                { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, dst, 5, 0, 1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, &sOut, nullptr, nullptr },
                { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, dst, 6, 0, 1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, &rOut, nullptr, nullptr }
            }};
            vkUpdateDescriptorSets(m_device, static_cast<uint32_t>(w.size()), w.data(), 0, nullptr);
        };
        writeAtrous(frame.atrousFromHistSet, shHistRead, rfHistRead, sh0Write, rf0Write);
        writeAtrous(frame.atrous0To1Set, sh0Read, rf0Read, sh1Write, rf1Write);
        writeAtrous(frame.atrous1To0Set, sh1Read, rf1Read, sh0Write, rf0Write);

        const VkDescriptorBufferInfo compUbo{ frame.compositeParamsBuffer.GetBuffer(), 0, sizeof(CompositeParamsUbo) };
        const VkDescriptorImageInfo shAux{ VK_NULL_HANDLE, rtShadowPass.GetShadowAuxView(frameIndex), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
        const VkDescriptorImageInfo rfAux{ VK_NULL_HANDLE, rtReflectionPass.GetReflectionAuxView(frameIndex), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
        const VkDescriptorImageInfo compOut{ VK_NULL_HANDLE, frame.compositeImage.GetView(), VK_IMAGE_LAYOUT_GENERAL };

        auto writeComp = [&](VkDescriptorSet dst, const VkDescriptorImageInfo& directImg, const VkDescriptorImageInfo& reflImg) {
            const std::array<VkWriteDescriptorSet, 11> w = {{
                { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, dst,  0, 0, 1, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, nullptr, &compUbo, nullptr },
                { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, dst,  1, 0, 1, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, &rt0, nullptr, nullptr },
                { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, dst,  2, 0, 1, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, &rt1, nullptr, nullptr },
                { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, dst,  3, 0, 1, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, &rt2, nullptr, nullptr },
                { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, dst,  4, 0, 1, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, &rt3, nullptr, nullptr },
                { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, dst,  5, 0, 1, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, &directImg, nullptr, nullptr },
                { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, dst,  6, 0, 1, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, &reflImg, nullptr, nullptr },
                { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, dst,  7, 0, 1, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, &shAux, nullptr, nullptr },
                { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, dst,  8, 0, 1, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, &rfAux, nullptr, nullptr },
                { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, dst,  9, 0, 1, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, &varConf, nullptr, nullptr },
                { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, dst, 10, 0, 1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, &compOut, nullptr, nullptr }
            }};
            vkUpdateDescriptorSets(m_device, static_cast<uint32_t>(w.size()), w.data(), 0, nullptr);
        };
        writeComp(frame.compositeFromHistSet, shHistRead, rfHistRead);
        writeComp(frame.compositeFromScratch0Set, sh0Read, rf0Read);
        writeComp(frame.compositeFromScratch1Set, sh1Read, rf1Read);
    }

    void VulkanRtDenoiserPass::UpdateTemporalDynamicDescriptors(
        FrameResources& frame,
        uint32_t frameIndex,
        uint32_t prevSlot,
        const VulkanRayTracingScene& rtScene,
        const VulkanHybridGBufferPass& gBufferPass,
        const VulkanRtShadowPass& rtShadowPass,
        const VulkanRtReflectionPass& rtReflectionPass)
    {
        const uint64_t instGen = rtScene.GetInstanceDataBufferGeneration(frameIndex);
        if (frame.sizeVersion == m_sizeVersion &&
            frame.boundPrevSlot == prevSlot &&
            frame.boundInstGeneration == instGen)
            return;

        if (frame.sizeVersion != m_sizeVersion)
            UpdateStaticFrameDescriptors(frame, frameIndex, gBufferPass, rtShadowPass, rtReflectionPass);

        const auto& prevFrame = *m_frames[prevSlot];
        const VkBuffer instBuf = rtScene.GetInstanceDataBuffer(frameIndex);

        const VkDescriptorBufferInfo ubo{ frame.temporalParamsBuffer.GetBuffer(), 0, sizeof(TemporalParamsUbo) };
        const VkDescriptorImageInfo currRt1{ VK_NULL_HANDLE, gBufferPass.GetRt1View(frameIndex), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
        const VkDescriptorImageInfo currRt3{ VK_NULL_HANDLE, gBufferPass.GetRt3View(frameIndex), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
        const VkDescriptorImageInfo prevRt1{ VK_NULL_HANDLE, gBufferPass.GetRt1View(prevSlot), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
        const VkDescriptorImageInfo prevRt3{ VK_NULL_HANDLE, gBufferPass.GetRt3View(prevSlot), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
        const VkDescriptorImageInfo rawDirect{ VK_NULL_HANDLE, rtShadowPass.GetDirectLightView(frameIndex), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
        const VkDescriptorImageInfo rawRefl{ VK_NULL_HANDLE, rtReflectionPass.GetReflectionView(frameIndex), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
        const VkDescriptorImageInfo prevSh{ VK_NULL_HANDLE, prevFrame.shadowHistory.GetView(), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
        const VkDescriptorImageInfo prevRf{ VK_NULL_HANDLE, prevFrame.reflHistory.GetView(), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
        const VkDescriptorImageInfo prevMom{ VK_NULL_HANDLE, prevFrame.momentsHistory.GetView(), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
        const VkDescriptorBufferInfo instInfo{ instBuf, 0, VK_WHOLE_SIZE };

        const VkDescriptorImageInfo shOut{ VK_NULL_HANDLE, frame.shadowHistory.GetView(), VK_IMAGE_LAYOUT_GENERAL };
        const VkDescriptorImageInfo rfOut{ VK_NULL_HANDLE, frame.reflHistory.GetView(), VK_IMAGE_LAYOUT_GENERAL };
        const VkDescriptorImageInfo momOut{ VK_NULL_HANDLE, frame.momentsHistory.GetView(), VK_IMAGE_LAYOUT_GENERAL };
        const VkDescriptorImageInfo vcOut{ VK_NULL_HANDLE, frame.varConfImage.GetView(), VK_IMAGE_LAYOUT_GENERAL };

        std::vector<VkWriteDescriptorSet> writes;
        writes.reserve(15);
        writes.push_back({ VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, frame.temporalSet, 0, 0, 1, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, nullptr, &ubo, nullptr });
        writes.push_back({ VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, frame.temporalSet, 1, 0, 1, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, &currRt1, nullptr, nullptr });
        writes.push_back({ VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, frame.temporalSet, 2, 0, 1, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, &currRt3, nullptr, nullptr });
        writes.push_back({ VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, frame.temporalSet, 3, 0, 1, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, &prevRt1, nullptr, nullptr });
        writes.push_back({ VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, frame.temporalSet, 4, 0, 1, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, &prevRt3, nullptr, nullptr });
        writes.push_back({ VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, frame.temporalSet, 5, 0, 1, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, &rawDirect, nullptr, nullptr });
        writes.push_back({ VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, frame.temporalSet, 6, 0, 1, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, &rawRefl, nullptr, nullptr });
        writes.push_back({ VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, frame.temporalSet, 7, 0, 1, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, &prevSh, nullptr, nullptr });
        writes.push_back({ VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, frame.temporalSet, 8, 0, 1, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, &prevRf, nullptr, nullptr });
        writes.push_back({ VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, frame.temporalSet, 9, 0, 1, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, &prevMom, nullptr, nullptr });
        if (instBuf != VK_NULL_HANDLE)
        {
            writes.push_back({ VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, frame.temporalSet, 10, 0, 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, nullptr, &instInfo, nullptr });
        }
        writes.push_back({ VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, frame.temporalSet, 11, 0, 1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, &shOut, nullptr, nullptr });
        writes.push_back({ VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, frame.temporalSet, 12, 0, 1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, &rfOut, nullptr, nullptr });
        writes.push_back({ VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, frame.temporalSet, 13, 0, 1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, &momOut, nullptr, nullptr });
        writes.push_back({ VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, frame.temporalSet, 14, 0, 1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, &vcOut, nullptr, nullptr });

        vkUpdateDescriptorSets(m_device, static_cast<uint32_t>(writes.size()), writes.data(), 0, nullptr);

        frame.boundPrevSlot = prevSlot;
        frame.boundInstGeneration = instGen;
        frame.sizeVersion = m_sizeVersion;
    }

    bool VulkanRtDenoiserPass::Execute(
        VkCommandBuffer cmd,
        uint32_t frameIndex,
        const VulkanRayTracingScene& rtScene,
        const VulkanHybridGBufferPass& gBufferPass,
        const VulkanRtShadowPass& rtShadowPass,
        const VulkanRtReflectionPass& rtReflectionPass,
        VkDescriptorSet environmentSet,
        const glm::mat4& currClipToWorldDirection,
        const glm::mat4& prevUnjitteredViewProj,
        const glm::vec3& cameraPosition,
        const HybridRtConfig& rawConfig,
        VkBuffer optionalVarConfReadbackBuffer,
        VkBuffer optionalCompositeReadbackBuffer)
    {
        if (!m_initialized || cmd == VK_NULL_HANDLE || frameIndex >= m_frames.size() ||
            environmentSet == VK_NULL_HANDLE)
            return false;

        const HybridRtConfig cfg = SanitizeHybridRtConfig(rawConfig);
        if (cfg.resetHistory)
            m_lastHistorySlot = UINT32_MAX;

        const uint32_t framesCount = static_cast<uint32_t>(m_frames.size());
        const bool historyValid = (m_lastHistorySlot != UINT32_MAX && m_lastHistorySlot < framesCount && m_lastHistorySlot != frameIndex);
        const uint32_t prevSlot = historyValid
            ? m_lastHistorySlot
            : (frameIndex + framesCount - 1u) % framesCount;

        auto& frame = *m_frames[frameIndex];
        auto& prevFrame = *m_frames[prevSlot];
        frame.recordedPass = false;

        TemporalParamsUbo tUbo{};
        tUbo.prevViewProj = prevUnjitteredViewProj;
        tUbo.cameraPosAndHistoryValid = glm::vec4(cameraPosition, (historyValid && prevSlot != frameIndex) ? 1.0f : 0.0f);
        tUbo.alphaAndThresholds = glm::vec4(cfg.temporalAlphaMin, cfg.temporalAlphaMax, cfg.reflectionMinRoughnessMirror, 0.90f);
        tUbo.extentAndFlags[0] = m_extent.width;
        tUbo.extentAndFlags[1] = m_extent.height;
        tUbo.extentAndFlags[2] = cfg.denoiserEnabled ? 1u : 0u;
        tUbo.extentAndFlags[3] = cfg.shadowSamples;

        CompositeParamsUbo cUbo{};
        cUbo.clipToWorldDirection = currClipToWorldDirection;
        cUbo.cameraPosAndMaxRough = glm::vec4(cameraPosition, cfg.reflectionMaxRoughness);
        cUbo.extentAndDebug[0] = m_extent.width;
        cUbo.extentAndDebug[1] = m_extent.height;
        cUbo.extentAndDebug[2] = static_cast<uint32_t>(cfg.debugView);
        cUbo.extentAndDebug[3] = cfg.reflectionsEnabled ? 1u : 0u;
        cUbo.flags[0] = cfg.shadowsEnabled ? 1u : 0u;
        cUbo.flags[1] = 1u;
        cUbo.flags[2] = cfg.denoiserEnabled ? 1u : 0u;

        if (frame.temporalParamsBuffer.UploadData(&tUbo, sizeof(tUbo)) != VK_SUCCESS ||
            frame.compositeParamsBuffer.UploadData(&cUbo, sizeof(cUbo)) != VK_SUCCESS)
            return false;

        UpdateTemporalDynamicDescriptors(frame, frameIndex, prevSlot, rtScene, gBufferPass, rtShadowPass, rtReflectionPass);

        if (m_timestampPool != VK_NULL_HANDLE)
        {
            vkCmdResetQueryPool(cmd, m_timestampPool, frameIndex * kDenoiserQueriesPerFrame, kDenoiserQueriesPerFrame);
            vkCmdWriteTimestamp2(cmd, VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, m_timestampPool,
                                 frameIndex * kDenoiserQueriesPerFrame + 0);
        }

        // Ensure prevFrame history images are valid in SHADER_READ_ONLY_OPTIMAL on initial frames.
        if (prevFrame.historyLayout == VK_IMAGE_LAYOUT_UNDEFINED)
        {
            const std::array<VkImage, 3> prevImgs = {
                prevFrame.shadowHistory.GetImage(),
                prevFrame.reflHistory.GetImage(),
                prevFrame.momentsHistory.GetImage()
            };
            std::array<VkImageMemoryBarrier2, 3> toClear{};
            for (size_t i = 0; i < 3; ++i)
            {
                toClear[i] = {
                    VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2, nullptr,
                    VK_PIPELINE_STAGE_2_NONE, 0,
                    VK_PIPELINE_STAGE_2_CLEAR_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                    VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                    VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED,
                    prevImgs[i], { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 }
                };
            }
            VkDependencyInfo depClear{ VK_STRUCTURE_TYPE_DEPENDENCY_INFO };
            depClear.imageMemoryBarrierCount = 3;
            depClear.pImageMemoryBarriers = toClear.data();
            vkCmdPipelineBarrier2(cmd, &depClear);

            const VkClearColorValue zeroClear{ { 0.0f, 0.0f, 0.0f, 0.0f } };
            const VkImageSubresourceRange range{ VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
            for (VkImage img : prevImgs)
                vkCmdClearColorImage(cmd, img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &zeroClear, 1, &range);

            for (size_t i = 0; i < 3; ++i)
            {
                toClear[i].srcStageMask = VK_PIPELINE_STAGE_2_CLEAR_BIT;
                toClear[i].srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
                toClear[i].dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
                toClear[i].dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
                toClear[i].oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
                toClear[i].newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            }
            vkCmdPipelineBarrier2(cmd, &depClear);
            prevFrame.historyLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        }

        const uint32_t groupsX = (m_extent.width + 7u) / 8u;
        const uint32_t groupsY = (m_extent.height + 7u) / 8u;

        // ============================================================
        // 1. Temporal Reprojection, Moments & Variance Estimation
        // ============================================================
        {
            const VkMemoryBarrier2 uboBarrier{
                VK_STRUCTURE_TYPE_MEMORY_BARRIER_2, nullptr,
                VK_PIPELINE_STAGE_2_HOST_BIT, VK_ACCESS_2_HOST_WRITE_BIT,
                VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_UNIFORM_READ_BIT
            };
            const std::array<VkImageMemoryBarrier2, 4> toGen = {{
                {
                    VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2, nullptr,
                    VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_TRANSFER_BIT, 0,
                    VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                    VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL,
                    VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED,
                    frame.shadowHistory.GetImage(), { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 }
                },
                {
                    VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2, nullptr,
                    VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_TRANSFER_BIT, 0,
                    VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                    VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL,
                    VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED,
                    frame.reflHistory.GetImage(), { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 }
                },
                {
                    VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2, nullptr,
                    VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_TRANSFER_BIT, 0,
                    VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                    VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL,
                    VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED,
                    frame.momentsHistory.GetImage(), { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 }
                },
                {
                    VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2, nullptr,
                    VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_TRANSFER_BIT, 0,
                    VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                    VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL,
                    VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED,
                    frame.varConfImage.GetImage(), { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 }
                }
            }};
            VkDependencyInfo preDep{ VK_STRUCTURE_TYPE_DEPENDENCY_INFO };
            preDep.memoryBarrierCount = 1;
            preDep.pMemoryBarriers = &uboBarrier;
            preDep.imageMemoryBarrierCount = 4;
            preDep.pImageMemoryBarriers = toGen.data();
            vkCmdPipelineBarrier2(cmd, &preDep);

            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_temporalPipeline);
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_temporalPipelineLayout, 0, 1, &frame.temporalSet, 0, nullptr);
            vkCmdDispatch(cmd, groupsX, groupsY, 1u);

            std::array<VkImageMemoryBarrier2, 3> histToRead{};
            const std::array<VkImage, 3> histImgs = {
                frame.shadowHistory.GetImage(),
                frame.reflHistory.GetImage(),
                frame.momentsHistory.GetImage()
            };
            for (size_t i = 0; i < 3; ++i)
            {
                histToRead[i] = {
                    VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2, nullptr,
                    VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                    VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
                    VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                    VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED,
                    histImgs[i], { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 }
                };
            }
            VkDependencyInfo postHistDep{ VK_STRUCTURE_TYPE_DEPENDENCY_INFO };
            postHistDep.imageMemoryBarrierCount = 3;
            postHistDep.pImageMemoryBarriers = histToRead.data();
            vkCmdPipelineBarrier2(cmd, &postHistDep);
            frame.historyLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

            if (optionalVarConfReadbackBuffer != VK_NULL_HANDLE)
            {
                const VkImageMemoryBarrier2 toCopy{
                    VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2, nullptr,
                    VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                    VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT,
                    VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                    VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED,
                    frame.varConfImage.GetImage(), { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 }
                };
                VkDependencyInfo depC{ VK_STRUCTURE_TYPE_DEPENDENCY_INFO };
                depC.imageMemoryBarrierCount = 1;
                depC.pImageMemoryBarriers = &toCopy;
                vkCmdPipelineBarrier2(cmd, &depC);

                VkBufferImageCopy reg{};
                reg.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
                reg.imageExtent = { m_extent.width, m_extent.height, 1 };
                vkCmdCopyImageToBuffer(cmd, frame.varConfImage.GetImage(),
                    VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, optionalVarConfReadbackBuffer, 1, &reg);

                const VkBufferMemoryBarrier2 hostB{
                    VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2, nullptr,
                    VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                    VK_PIPELINE_STAGE_2_HOST_BIT, VK_ACCESS_2_HOST_READ_BIT,
                    VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED,
                    optionalVarConfReadbackBuffer, 0, VK_WHOLE_SIZE
                };
                const VkImageMemoryBarrier2 vcToRead{
                    VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2, nullptr,
                    VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT,
                    VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
                    VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                    VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED,
                    frame.varConfImage.GetImage(), { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 }
                };
                VkDependencyInfo depVcRead{ VK_STRUCTURE_TYPE_DEPENDENCY_INFO };
                depVcRead.bufferMemoryBarrierCount = 1;
                depVcRead.pBufferMemoryBarriers = &hostB;
                depVcRead.imageMemoryBarrierCount = 1;
                depVcRead.pImageMemoryBarriers = &vcToRead;
                vkCmdPipelineBarrier2(cmd, &depVcRead);
            }
            else
            {
                const VkImageMemoryBarrier2 vcToRead{
                    VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2, nullptr,
                    VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                    VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
                    VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                    VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED,
                    frame.varConfImage.GetImage(), { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 }
                };
                VkDependencyInfo depVc{ VK_STRUCTURE_TYPE_DEPENDENCY_INFO };
                depVc.imageMemoryBarrierCount = 1;
                depVc.pImageMemoryBarriers = &vcToRead;
                vkCmdPipelineBarrier2(cmd, &depVc);
            }
        }

        if (m_timestampPool != VK_NULL_HANDLE)
        {
            vkCmdWriteTimestamp2(cmd, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, m_timestampPool,
                                 frameIndex * kDenoiserQueriesPerFrame + 1);
        }

        // ============================================================
        // 2. Variance-Guided Roughness-Aware À-Trous Spatial Filtering
        // ============================================================
        const uint32_t atrousIters = cfg.denoiserEnabled ? cfg.atrousIterations : 0u;
        if (atrousIters > 0u)
        {
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_atrousPipeline);
            for (uint32_t iter = 0; iter < atrousIters; ++iter)
            {
                const uint32_t writeBuf = iter % 2u;
                const VkDescriptorSet iterSet = (iter == 0u)
                    ? frame.atrousFromHistSet
                    : ((iter % 2u == 1u) ? frame.atrous0To1Set : frame.atrous1To0Set);

                const std::array<VkImageMemoryBarrier2, 2> toGen = {{
                    {
                        VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2, nullptr,
                        VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, 0,
                        VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                        VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL,
                        VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED,
                        frame.atrousShadow[writeBuf].GetImage(), { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 }
                    },
                    {
                        VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2, nullptr,
                        VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, 0,
                        VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                        VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL,
                        VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED,
                        frame.atrousRefl[writeBuf].GetImage(), { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 }
                    }
                }};
                VkDependencyInfo depGen{ VK_STRUCTURE_TYPE_DEPENDENCY_INFO };
                depGen.imageMemoryBarrierCount = 2;
                depGen.pImageMemoryBarriers = toGen.data();
                vkCmdPipelineBarrier2(cmd, &depGen);

                AtrousPushConstants push{};
                push.width = m_extent.width;
                push.height = m_extent.height;
                push.stepSize = 1 << iter;
                push.iterIndex = iter;
                push.minMirrorRoughness = cfg.reflectionMinRoughnessMirror;
                push.sigmaLuma = 2.0f;
                push.sigmaNormal = 64.0f;
                push.sigmaDepth = 0.03f;

                vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_atrousPipelineLayout,
                    0, 1, &iterSet, 0, nullptr);
                vkCmdPushConstants(cmd, m_atrousPipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT,
                    0, sizeof(push), &push);
                vkCmdDispatch(cmd, groupsX, groupsY, 1u);

                const std::array<VkImageMemoryBarrier2, 2> toRead = {{
                    {
                        VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2, nullptr,
                        VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                        VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
                        VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                        VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED,
                        frame.atrousShadow[writeBuf].GetImage(), { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 }
                    },
                    {
                        VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2, nullptr,
                        VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                        VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
                        VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                        VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED,
                        frame.atrousRefl[writeBuf].GetImage(), { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 }
                    }
                }};
                VkDependencyInfo depRead{ VK_STRUCTURE_TYPE_DEPENDENCY_INFO };
                depRead.imageMemoryBarrierCount = 2;
                depRead.pImageMemoryBarriers = toRead.data();
                vkCmdPipelineBarrier2(cmd, &depRead);
            }
        }

        if (m_timestampPool != VK_NULL_HANDLE)
        {
            vkCmdWriteTimestamp2(cmd, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, m_timestampPool,
                                 frameIndex * kDenoiserQueriesPerFrame + 2);
        }

        // ============================================================
        // 3. Unified Hybrid HDR Composite + Debug Visualizations
        // ============================================================
        {
            const VkMemoryBarrier2 compUboBarrier{
                VK_STRUCTURE_TYPE_MEMORY_BARRIER_2, nullptr,
                VK_PIPELINE_STAGE_2_HOST_BIT, VK_ACCESS_2_HOST_WRITE_BIT,
                VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_UNIFORM_READ_BIT
            };
            const VkImageMemoryBarrier2 toGen{
                VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2, nullptr,
                VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_TRANSFER_BIT, 0,
                VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL,
                VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED,
                frame.compositeImage.GetImage(), { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 }
            };
            VkDependencyInfo depGen{ VK_STRUCTURE_TYPE_DEPENDENCY_INFO };
            depGen.memoryBarrierCount = 1;
            depGen.pMemoryBarriers = &compUboBarrier;
            depGen.imageMemoryBarrierCount = 1;
            depGen.pImageMemoryBarriers = &toGen;
            vkCmdPipelineBarrier2(cmd, &depGen);

            VkDescriptorSet compSet = frame.compositeFromHistSet;
            if (atrousIters > 0u)
                compSet = ((atrousIters - 1u) % 2u == 0u) ? frame.compositeFromScratch0Set : frame.compositeFromScratch1Set;

            const std::array<VkDescriptorSet, 2> sets = { compSet, environmentSet };
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_compositePipeline);
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_compositePipelineLayout,
                0, static_cast<uint32_t>(sets.size()), sets.data(), 0, nullptr);
            vkCmdDispatch(cmd, groupsX, groupsY, 1u);

            if (optionalCompositeReadbackBuffer != VK_NULL_HANDLE)
            {
                const VkImageMemoryBarrier2 toCopy{
                    VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2, nullptr,
                    VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                    VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT,
                    VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                    VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED,
                    frame.compositeImage.GetImage(), { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 }
                };
                VkDependencyInfo depC{ VK_STRUCTURE_TYPE_DEPENDENCY_INFO };
                depC.imageMemoryBarrierCount = 1;
                depC.pImageMemoryBarriers = &toCopy;
                vkCmdPipelineBarrier2(cmd, &depC);

                VkBufferImageCopy reg{};
                reg.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
                reg.imageExtent = { m_extent.width, m_extent.height, 1 };
                vkCmdCopyImageToBuffer(cmd, frame.compositeImage.GetImage(),
                    VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, optionalCompositeReadbackBuffer, 1, &reg);

                const VkBufferMemoryBarrier2 hostB{
                    VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2, nullptr,
                    VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                    VK_PIPELINE_STAGE_2_HOST_BIT, VK_ACCESS_2_HOST_READ_BIT,
                    VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED,
                    optionalCompositeReadbackBuffer, 0, VK_WHOLE_SIZE
                };
                const VkImageMemoryBarrier2 compToFrag{
                    VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2, nullptr,
                    VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT,
                    VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
                    VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                    VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED,
                    frame.compositeImage.GetImage(), { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 }
                };
                VkDependencyInfo depFinal{ VK_STRUCTURE_TYPE_DEPENDENCY_INFO };
                depFinal.bufferMemoryBarrierCount = 1;
                depFinal.pBufferMemoryBarriers = &hostB;
                depFinal.imageMemoryBarrierCount = 1;
                depFinal.pImageMemoryBarriers = &compToFrag;
                vkCmdPipelineBarrier2(cmd, &depFinal);
            }
            else
            {
                const VkImageMemoryBarrier2 compToFrag{
                    VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2, nullptr,
                    VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                    VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
                    VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                    VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED,
                    frame.compositeImage.GetImage(), { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 }
                };
                VkDependencyInfo depFinal{ VK_STRUCTURE_TYPE_DEPENDENCY_INFO };
                depFinal.imageMemoryBarrierCount = 1;
                depFinal.pImageMemoryBarriers = &compToFrag;
                vkCmdPipelineBarrier2(cmd, &depFinal);
            }
        }

        if (m_timestampPool != VK_NULL_HANDLE)
        {
            vkCmdWriteTimestamp2(cmd, VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT, m_timestampPool,
                                 frameIndex * kDenoiserQueriesPerFrame + 3);
        }

        frame.recordedPass = true;
        return true;
    }

    VkImageView VulkanRtDenoiserPass::GetCompositeView(uint32_t frameIndex) const noexcept
    {
        return (m_initialized && frameIndex < m_frames.size()) ? m_frames[frameIndex]->compositeImage.GetView() : VK_NULL_HANDLE;
    }

    VkDescriptorSet VulkanRtDenoiserPass::GetOutputToneMapSet(uint32_t frameIndex) const noexcept
    {
        return (m_initialized && frameIndex < m_frames.size()) ? m_frames[frameIndex]->toneMapSet : VK_NULL_HANDLE;
    }

    VkImageView VulkanRtDenoiserPass::GetVarConfView(uint32_t frameIndex) const noexcept
    {
        return (m_initialized && frameIndex < m_frames.size()) ? m_frames[frameIndex]->varConfImage.GetView() : VK_NULL_HANDLE;
    }
}
