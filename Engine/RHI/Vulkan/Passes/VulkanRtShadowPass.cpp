#include "VulkanRtShadowPass.h"

#include <RHI/Vulkan/Passes/VulkanHybridGBufferPass.h>
#include <RHI/Vulkan/Pipeline/VulkanShader.h>
#include <RHI/Vulkan/RayTracing/VulkanRayTracingScene.h>
#include <RHI/Vulkan/VulkanContext.h>
#include <RHI/Vulkan/VulkanMemoryManager.h>

#include <algorithm>
#include <cmath>
#include <iostream>
#include <string>

namespace Aether
{
    VulkanRtShadowPass::~VulkanRtShadowPass()
    {
        Shutdown();
    }

    bool VulkanRtShadowPass::Initialize(
        const VulkanContext& context,
        const VulkanMemoryManager& memory,
        uint32_t framesInFlight,
        VkExtent2D extent)
    {
        if (m_initialized || framesInFlight == 0 || extent.width == 0 || extent.height == 0 ||
            context.GetDevice() == VK_NULL_HANDLE || !context.GetRayTracingCapabilities().IsReady())
            return false;

        m_physicalDevice = context.GetPhysicalDevice();
        m_device = context.GetDevice();
        m_memory = &memory;
        m_extent = extent;
        m_sizeVersion = 1;

        const std::array<VkDescriptorSetLayoutBinding, 13> bindings = {{
            {  0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,         1,                       VK_SHADER_STAGE_COMPUTE_BIT, nullptr },
            {  1, VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, 1,                   VK_SHADER_STAGE_COMPUTE_BIT, nullptr },
            {  2, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,          1,                       VK_SHADER_STAGE_COMPUTE_BIT, nullptr },
            {  3, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,          1,                       VK_SHADER_STAGE_COMPUTE_BIT, nullptr },
            {  4, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,          1,                       VK_SHADER_STAGE_COMPUTE_BIT, nullptr },
            {  5, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,          1,                       VK_SHADER_STAGE_COMPUTE_BIT, nullptr },
            {  6, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,         1,                       VK_SHADER_STAGE_COMPUTE_BIT, nullptr },
            {  7, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,         1,                       VK_SHADER_STAGE_COMPUTE_BIT, nullptr },
            {  8, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,         1,                       VK_SHADER_STAGE_COMPUTE_BIT, nullptr },
            {  9, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,          kMaxRtAlphaMaskTextures, VK_SHADER_STAGE_COMPUTE_BIT, nullptr },
            { 10, VK_DESCRIPTOR_TYPE_SAMPLER,                kMaxRtAlphaMaskTextures, VK_SHADER_STAGE_COMPUTE_BIT, nullptr },
            { 11, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,          1,                       VK_SHADER_STAGE_COMPUTE_BIT, nullptr },
            { 12, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,          1,                       VK_SHADER_STAGE_COMPUTE_BIT, nullptr }
        }};

        VkDescriptorSetLayoutCreateInfo layoutInfo{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
        layoutInfo.bindingCount = static_cast<uint32_t>(bindings.size());
        layoutInfo.pBindings = bindings.data();
        if (vkCreateDescriptorSetLayout(m_device, &layoutInfo, nullptr, &m_descriptorSetLayout) != VK_SUCCESS)
        {
            Shutdown();
            return false;
        }

        const std::array<VkDescriptorPoolSize, 6> poolSizes = {{
            { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,             framesInFlight },
            { VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, framesInFlight },
            { VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,              framesInFlight * (4 + kMaxRtAlphaMaskTextures) },
            { VK_DESCRIPTOR_TYPE_SAMPLER,                    framesInFlight * kMaxRtAlphaMaskTextures },
            { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,             framesInFlight * 3 },
            { VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,              framesInFlight * 2 }
        }};
        VkDescriptorPoolCreateInfo poolInfo{ VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
        poolInfo.maxSets = framesInFlight;
        poolInfo.poolSizeCount = static_cast<uint32_t>(poolSizes.size());
        poolInfo.pPoolSizes = poolSizes.data();
        if (vkCreateDescriptorPool(m_device, &poolInfo, nullptr, &m_descriptorPool) != VK_SUCCESS)
        {
            Shutdown();
            return false;
        }

        VkPipelineLayoutCreateInfo plInfo{ VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
        plInfo.setLayoutCount = 1;
        plInfo.pSetLayouts = &m_descriptorSetLayout;
        if (vkCreatePipelineLayout(m_device, &plInfo, nullptr, &m_pipelineLayout) != VK_SUCCESS)
        {
            Shutdown();
            return false;
        }

        VulkanShader shader;
        const std::string spvPath = std::string(AETHER_SHADER_DIR) + "/rt_shadow.comp.spv";
        if (!shader.Initialize(m_device, spvPath))
        {
            std::cerr << "VulkanRtShadowPass: failed to load " << spvPath << '\n';
            Shutdown();
            return false;
        }

        VkComputePipelineCreateInfo pipeInfo{ VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO };
        pipeInfo.layout = m_pipelineLayout;
        pipeInfo.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        pipeInfo.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
        pipeInfo.stage.module = shader.GetShaderModule();
        pipeInfo.stage.pName = "main";
        if (vkCreateComputePipelines(m_device, VK_NULL_HANDLE, 1, &pipeInfo, nullptr, &m_pipeline) != VK_SUCCESS)
        {
            std::cerr << "VulkanRtShadowPass: failed to create compute pipeline.\n";
            Shutdown();
            return false;
        }

        m_frames.clear();
        m_frames.reserve(framesInFlight);
        for (uint32_t i = 0; i < framesInFlight; ++i)
        {
            auto frame = std::make_unique<FrameResources>();
            VkDescriptorSetAllocateInfo allocInfo{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
            allocInfo.descriptorPool = m_descriptorPool;
            allocInfo.descriptorSetCount = 1;
            allocInfo.pSetLayouts = &m_descriptorSetLayout;
            if (vkAllocateDescriptorSets(m_device, &allocInfo, &frame->descriptorSet) != VK_SUCCESS)
            {
                Shutdown();
                return false;
            }
            if (frame->paramsBuffer.Initialize(
                    m_device, *m_memory, sizeof(ShadowParamsUbo),
                    VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                    VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT,
                    VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) != VK_SUCCESS)
            {
                Shutdown();
                return false;
            }
            m_frames.push_back(std::move(frame));
        }

        if (!CreateOutputImages(extent))
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
            qInfo.queryCount = framesInFlight * 2;
            if (vkCreateQueryPool(m_device, &qInfo, nullptr, &m_timestampPool) == VK_SUCCESS)
                m_timestampPeriodNs = static_cast<double>(props.limits.timestampPeriod);
        }

        m_initialized = true;
        return true;
    }

    bool VulkanRtShadowPass::CreateOutputImages(VkExtent2D extent)
    {
        constexpr VkImageUsageFlags kOutUsage =
            VK_IMAGE_USAGE_STORAGE_BIT |
            VK_IMAGE_USAGE_SAMPLED_BIT |
            VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
            VK_IMAGE_USAGE_TRANSFER_DST_BIT;

        for (auto& framePtr : m_frames)
        {
            auto& f = *framePtr;
            f.directLightImage.Shutdown();
            f.shadowAuxImage.Shutdown();

            if (f.directLightImage.Initialize(
                    m_device, *m_memory, extent, VK_FORMAT_R16G16B16A16_SFLOAT,
                    kOutUsage, VK_IMAGE_ASPECT_COLOR_BIT) != VK_SUCCESS ||
                f.shadowAuxImage.Initialize(
                    m_device, *m_memory, extent, VK_FORMAT_R16G16_SFLOAT,
                    kOutUsage, VK_IMAGE_ASPECT_COLOR_BIT) != VK_SUCCESS)
                return false;
        }
        m_extent = extent;
        ++m_sizeVersion;
        return true;
    }

    bool VulkanRtShadowPass::OnResize(VkExtent2D newExtent)
    {
        if (!m_initialized || newExtent.width == 0 || newExtent.height == 0)
            return false;
        return CreateOutputImages(newExtent);
    }

    void VulkanRtShadowPass::Shutdown() noexcept
    {
        if (m_pipeline != VK_NULL_HANDLE && m_device != VK_NULL_HANDLE)
        {
            vkDestroyPipeline(m_device, m_pipeline, nullptr);
            m_pipeline = VK_NULL_HANDLE;
        }
        if (m_pipelineLayout != VK_NULL_HANDLE && m_device != VK_NULL_HANDLE)
        {
            vkDestroyPipelineLayout(m_device, m_pipelineLayout, nullptr);
            m_pipelineLayout = VK_NULL_HANDLE;
        }
        if (m_descriptorPool != VK_NULL_HANDLE && m_device != VK_NULL_HANDLE)
        {
            vkDestroyDescriptorPool(m_device, m_descriptorPool, nullptr);
            m_descriptorPool = VK_NULL_HANDLE;
        }
        if (m_descriptorSetLayout != VK_NULL_HANDLE && m_device != VK_NULL_HANDLE)
        {
            vkDestroyDescriptorSetLayout(m_device, m_descriptorSetLayout, nullptr);
            m_descriptorSetLayout = VK_NULL_HANDLE;
        }
        if (m_timestampPool != VK_NULL_HANDLE && m_device != VK_NULL_HANDLE)
        {
            vkDestroyQueryPool(m_device, m_timestampPool, nullptr);
            m_timestampPool = VK_NULL_HANDLE;
        }
        m_frames.clear();
        m_physicalDevice = VK_NULL_HANDLE;
        m_device = VK_NULL_HANDLE;
        m_memory = nullptr;
        m_extent = {};
        m_sizeVersion = 1;
        m_timestampPeriodNs = 0.0;
        m_lastGpuTimeMs = 0.0;
        m_initialized = false;
    }

    void VulkanRtShadowPass::OnFrameFenceCompleted(uint32_t frameIndex)
    {
        if (!m_initialized || frameIndex >= m_frames.size())
            return;
        auto& f = *m_frames[frameIndex];
        if (m_timestampPool != VK_NULL_HANDLE && f.hasPendingTimestamp && f.recordedDispatch)
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
        f.recordedDispatch = false;
    }

    void VulkanRtShadowPass::OnFrameSubmitted(uint32_t frameIndex)
    {
        if (!m_initialized || frameIndex >= m_frames.size())
            return;
        auto& f = *m_frames[frameIndex];
        f.hasPendingTimestamp = (m_timestampPool != VK_NULL_HANDLE && f.recordedDispatch);
    }

    void VulkanRtShadowPass::RefreshDescriptorSet(
        FrameResources& frame,
        uint32_t frameIndex,
        const VulkanRayTracingScene& rtScene,
        const VulkanHybridGBufferPass& gBufferPass)
    {
        const VkAccelerationStructureKHR tlasHandle = rtScene.GetTlasHandle(frameIndex);
        const VkBuffer vtxBuf = rtScene.GetVertexStorageBuffer();
        const VkBuffer idxBuf = rtScene.GetIndexStorageBuffer();
        const VkBuffer instBuf = rtScene.GetInstanceDataBuffer(frameIndex);
        const auto maskInfos = rtScene.GetAlphaMaskImageInfos(frameIndex);

        const uint64_t geomGen = rtScene.GetGeometryBufferGeneration();
        const uint64_t instGen = rtScene.GetInstanceDataBufferGeneration(frameIndex);
        const uint64_t maskGen = rtScene.GetAlphaMaskTableGeneration(frameIndex);

        if (frame.sizeVersion == m_sizeVersion &&
            frame.boundTlas == tlasHandle &&
            frame.boundGeomGeneration == geomGen &&
            frame.boundInstGeneration == instGen &&
            frame.boundMaskGeneration == maskGen)
            return;

        std::vector<VkWriteDescriptorSet> writes;
        writes.reserve(13);

        const VkDescriptorBufferInfo uboInfo{ frame.paramsBuffer.GetBuffer(), 0, sizeof(ShadowParamsUbo) };
        writes.push_back({ VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, frame.descriptorSet,
            0, 0, 1, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, nullptr, &uboInfo, nullptr });

        VkWriteDescriptorSetAccelerationStructureKHR asWrite{
            VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_ACCELERATION_STRUCTURE_KHR
        };
        if (tlasHandle != VK_NULL_HANDLE)
        {
            asWrite.accelerationStructureCount = 1;
            asWrite.pAccelerationStructures = &tlasHandle;
            writes.push_back({ VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, &asWrite, frame.descriptorSet,
                1, 0, 1, VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, nullptr, nullptr, nullptr });
        }

        const std::array<VkDescriptorImageInfo, 4> gbInfos = {{
            { VK_NULL_HANDLE, gBufferPass.GetRt0View(frameIndex), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL },
            { VK_NULL_HANDLE, gBufferPass.GetRt1View(frameIndex), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL },
            { VK_NULL_HANDLE, gBufferPass.GetRt2View(frameIndex), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL },
            { VK_NULL_HANDLE, gBufferPass.GetRt3View(frameIndex), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL }
        }};
        for (uint32_t i = 0; i < 4; ++i)
        {
            writes.push_back({ VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, frame.descriptorSet,
                2u + i, 0, 1, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, &gbInfos[i], nullptr, nullptr });
        }

        const VkDescriptorBufferInfo instInfo{ instBuf, 0, VK_WHOLE_SIZE };
        const VkDescriptorBufferInfo vtxInfo{ vtxBuf, 0, VK_WHOLE_SIZE };
        const VkDescriptorBufferInfo idxInfo{ idxBuf, 0, VK_WHOLE_SIZE };
        if (instBuf != VK_NULL_HANDLE)
        {
            writes.push_back({ VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, frame.descriptorSet,
                6, 0, 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, nullptr, &instInfo, nullptr });
        }
        if (vtxBuf != VK_NULL_HANDLE)
        {
            writes.push_back({ VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, frame.descriptorSet,
                7, 0, 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, nullptr, &vtxInfo, nullptr });
        }
        if (idxBuf != VK_NULL_HANDLE)
        {
            writes.push_back({ VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, frame.descriptorSet,
                8, 0, 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, nullptr, &idxInfo, nullptr });
        }

        std::array<VkDescriptorImageInfo, kMaxRtAlphaMaskTextures> imgs{};
        std::array<VkDescriptorImageInfo, kMaxRtAlphaMaskTextures> smps{};
        if (maskInfos.size() == kMaxRtAlphaMaskTextures)
        {
            for (uint32_t s = 0; s < kMaxRtAlphaMaskTextures; ++s)
            {
                imgs[s] = { VK_NULL_HANDLE, maskInfos[s].imageView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
                smps[s] = { maskInfos[s].sampler, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_UNDEFINED };
            }
            writes.push_back({ VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, frame.descriptorSet,
                9, 0, kMaxRtAlphaMaskTextures, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, imgs.data(), nullptr, nullptr });
            writes.push_back({ VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, frame.descriptorSet,
                10, 0, kMaxRtAlphaMaskTextures, VK_DESCRIPTOR_TYPE_SAMPLER, smps.data(), nullptr, nullptr });
        }

        const VkDescriptorImageInfo outDirectInfo{
            VK_NULL_HANDLE, frame.directLightImage.GetView(), VK_IMAGE_LAYOUT_GENERAL
        };
        const VkDescriptorImageInfo outAuxInfo{
            VK_NULL_HANDLE, frame.shadowAuxImage.GetView(), VK_IMAGE_LAYOUT_GENERAL
        };
        writes.push_back({ VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, frame.descriptorSet,
            11, 0, 1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, &outDirectInfo, nullptr, nullptr });
        writes.push_back({ VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, frame.descriptorSet,
            12, 0, 1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, &outAuxInfo, nullptr, nullptr });

        vkUpdateDescriptorSets(m_device, static_cast<uint32_t>(writes.size()), writes.data(), 0, nullptr);

        frame.boundTlas = tlasHandle;
        frame.boundGeomGeneration = geomGen;
        frame.boundInstGeneration = instGen;
        frame.boundMaskGeneration = maskGen;
        frame.sizeVersion = m_sizeVersion;
    }

    bool VulkanRtShadowPass::Execute(
        VkCommandBuffer cmd,
        uint32_t frameIndex,
        const VulkanRayTracingScene& rtScene,
        bool tlasWrittenThisFrame,
        const VulkanHybridGBufferPass& gBufferPass,
        const glm::vec3& cameraPosition,
        const glm::vec3& sunDirectionToLight,
        const glm::vec3& sunRadiance,
        bool sunEnabled,
        bool fillEnabled,
        const HybridRtConfig& rawConfig,
        uint32_t globalFrameCounter,
        std::span<const RtGpuLight> lights,
        VkBuffer optionalDirectReadbackBuffer)
    {
        if (!m_initialized || cmd == VK_NULL_HANDLE || frameIndex >= m_frames.size())
            return false;

        auto& frame = *m_frames[frameIndex];
        frame.recordedDispatch = false;

        const HybridRtConfig cfg = SanitizeHybridRtConfig(rawConfig);
        const VkAccelerationStructureKHR tlasHandle = rtScene.GetTlasHandle(frameIndex);

        if (tlasHandle == VK_NULL_HANDLE)
        {
            frame.boundTlas = VK_NULL_HANDLE;
            const std::array<VkImageMemoryBarrier2, 2> toClear = {{
                {
                    VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2, nullptr,
                    VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_TRANSFER_BIT, 0,
                    VK_PIPELINE_STAGE_2_CLEAR_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                    VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                    VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED,
                    frame.directLightImage.GetImage(), { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 }
                },
                {
                    VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2, nullptr,
                    VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_TRANSFER_BIT, 0,
                    VK_PIPELINE_STAGE_2_CLEAR_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                    VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                    VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED,
                    frame.shadowAuxImage.GetImage(), { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 }
                }
            }};
            VkDependencyInfo depClear{ VK_STRUCTURE_TYPE_DEPENDENCY_INFO };
            depClear.imageMemoryBarrierCount = 2;
            depClear.pImageMemoryBarriers = toClear.data();
            vkCmdPipelineBarrier2(cmd, &depClear);

            const VkClearColorValue zeroClear{ { 0.0f, 0.0f, 0.0f, 0.0f } };
            const VkImageSubresourceRange range{ VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
            vkCmdClearColorImage(cmd, frame.directLightImage.GetImage(), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &zeroClear, 1, &range);
            vkCmdClearColorImage(cmd, frame.shadowAuxImage.GetImage(), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &zeroClear, 1, &range);

            const std::array<VkImageMemoryBarrier2, 2> toRead = {{
                {
                    VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2, nullptr,
                    VK_PIPELINE_STAGE_2_CLEAR_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                    VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
                    VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                    VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED,
                    frame.directLightImage.GetImage(), { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 }
                },
                {
                    VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2, nullptr,
                    VK_PIPELINE_STAGE_2_CLEAR_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                    VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
                    VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                    VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED,
                    frame.shadowAuxImage.GetImage(), { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 }
                }
            }};
            VkDependencyInfo depRead{ VK_STRUCTURE_TYPE_DEPENDENCY_INFO };
            depRead.imageMemoryBarrierCount = 2;
            depRead.pImageMemoryBarriers = toRead.data();
            vkCmdPipelineBarrier2(cmd, &depRead);
            return true;
        }

        ShadowParamsUbo ubo{};
        ubo.cameraPosAndHasTlas = glm::vec4(cameraPosition, tlasHandle != VK_NULL_HANDLE ? 1.0f : 0.0f);
        const float dirLenSq = glm::dot(sunDirectionToLight, sunDirectionToLight);
        const glm::vec3 sunDir = dirLenSq > 1e-12f
            ? sunDirectionToLight / std::sqrt(dirLenSq)
            : glm::vec3(0.0f, 1.0f, 0.0f);
        ubo.sunDirAndAngularRadius = glm::vec4(sunDir, glm::radians(cfg.sunAngularRadiusDeg));
        ubo.sunColorAndEnabled = glm::vec4(sunRadiance, sunEnabled ? 1.0f : 0.0f);
        ubo.biasAndFlags = glm::vec4(cfg.shadowNormalBias, cfg.shadowDistanceBias, 0.001f, 200.0f);
        ubo.countsAndExtent[0] = m_extent.width;
        ubo.countsAndExtent[1] = m_extent.height;
        ubo.countsAndExtent[2] = cfg.shadowSamples;
        ubo.countsAndExtent[3] = globalFrameCounter;
        ubo.featureFlags[0] = cfg.shadowsEnabled ? 1u : 0u;
        ubo.featureFlags[1] = cfg.areaLightsEnabled ? 1u : 0u;
        ubo.featureFlags[2] = static_cast<uint32_t>(std::min<size_t>(lights.size(), kMaxRtSceneLights));
        ubo.featureFlags[3] = fillEnabled ? 1u : 0u;
        for (uint32_t i = 0; i < ubo.featureFlags[2]; ++i)
            ubo.lights[i] = lights[i];

        if (frame.paramsBuffer.UploadData(&ubo, sizeof(ubo)) != VK_SUCCESS)
            return false;

        RefreshDescriptorSet(frame, frameIndex, rtScene, gBufferPass);

        if (m_timestampPool != VK_NULL_HANDLE)
        {
            vkCmdResetQueryPool(cmd, m_timestampPool, frameIndex * 2, 2);
            vkCmdWriteTimestamp2(cmd, VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, m_timestampPool, frameIndex * 2);
        }

        // Barriers before dispatch: TLAS (if built), Host UBO writes, and output storage images to GENERAL.
        std::vector<VkMemoryBarrier2> memBarriers;
        memBarriers.push_back({
            VK_STRUCTURE_TYPE_MEMORY_BARRIER_2, nullptr,
            VK_PIPELINE_STAGE_2_HOST_BIT, VK_ACCESS_2_HOST_WRITE_BIT,
            VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_UNIFORM_READ_BIT
        });
        if (tlasWrittenThisFrame && tlasHandle != VK_NULL_HANDLE)
        {
            memBarriers.push_back({
                VK_STRUCTURE_TYPE_MEMORY_BARRIER_2, nullptr,
                VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
                VK_ACCESS_2_ACCELERATION_STRUCTURE_WRITE_BIT_KHR,
                VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                VK_ACCESS_2_ACCELERATION_STRUCTURE_READ_BIT_KHR
            });
        }

        const std::array<VkImageMemoryBarrier2, 2> toGeneral = {{
            {
                VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2, nullptr,
                VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_TRANSFER_BIT, 0,
                VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL,
                VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED,
                frame.directLightImage.GetImage(), { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 }
            },
            {
                VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2, nullptr,
                VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_TRANSFER_BIT, 0,
                VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL,
                VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED,
                frame.shadowAuxImage.GetImage(), { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 }
            }
        }};

        VkDependencyInfo preDep{ VK_STRUCTURE_TYPE_DEPENDENCY_INFO };
        preDep.memoryBarrierCount = static_cast<uint32_t>(memBarriers.size());
        preDep.pMemoryBarriers = memBarriers.data();
        preDep.imageMemoryBarrierCount = static_cast<uint32_t>(toGeneral.size());
        preDep.pImageMemoryBarriers = toGeneral.data();
        vkCmdPipelineBarrier2(cmd, &preDep);

        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_pipeline);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_pipelineLayout, 0, 1, &frame.descriptorSet, 0, nullptr);
        const uint32_t groupsX = (m_extent.width + 7u) / 8u;
        const uint32_t groupsY = (m_extent.height + 7u) / 8u;
        vkCmdDispatch(cmd, groupsX, groupsY, 1u);

        if (optionalDirectReadbackBuffer != VK_NULL_HANDLE)
        {
            const std::array<VkImageMemoryBarrier2, 2> postReadbackBarriers = {{
                {
                    VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2, nullptr,
                    VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                    VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT,
                    VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                    VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED,
                    frame.directLightImage.GetImage(), { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 }
                },
                {
                    VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2, nullptr,
                    VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                    VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
                    VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                    VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED,
                    frame.shadowAuxImage.GetImage(), { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 }
                }
            }};
            VkDependencyInfo copyPrep{ VK_STRUCTURE_TYPE_DEPENDENCY_INFO };
            copyPrep.imageMemoryBarrierCount = 2;
            copyPrep.pImageMemoryBarriers = postReadbackBarriers.data();
            vkCmdPipelineBarrier2(cmd, &copyPrep);

            VkBufferImageCopy region{};
            region.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
            region.imageExtent = { m_extent.width, m_extent.height, 1 };
            vkCmdCopyImageToBuffer(
                cmd, frame.directLightImage.GetImage(),
                VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, optionalDirectReadbackBuffer, 1, &region);

            const VkBufferMemoryBarrier2 hostBarrier{
                VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2, nullptr,
                VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                VK_PIPELINE_STAGE_2_HOST_BIT, VK_ACCESS_2_HOST_READ_BIT,
                VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED,
                optionalDirectReadbackBuffer, 0, VK_WHOLE_SIZE
            };
            const VkImageMemoryBarrier2 toReadAfterCopy{
                VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2, nullptr,
                VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT,
                VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
                VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED,
                frame.directLightImage.GetImage(), { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 }
            };
            VkDependencyInfo afterCopy{ VK_STRUCTURE_TYPE_DEPENDENCY_INFO };
            afterCopy.bufferMemoryBarrierCount = 1;
            afterCopy.pBufferMemoryBarriers = &hostBarrier;
            afterCopy.imageMemoryBarrierCount = 1;
            afterCopy.pImageMemoryBarriers = &toReadAfterCopy;
            vkCmdPipelineBarrier2(cmd, &afterCopy);
        }
        else
        {
            const std::array<VkImageMemoryBarrier2, 2> toShaderRead = {{
                {
                    VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2, nullptr,
                    VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                    VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
                    VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                    VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED,
                    frame.directLightImage.GetImage(), { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 }
                },
                {
                    VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2, nullptr,
                    VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                    VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
                    VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                    VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED,
                    frame.shadowAuxImage.GetImage(), { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 }
                }
            }};
            VkDependencyInfo postDep{ VK_STRUCTURE_TYPE_DEPENDENCY_INFO };
            postDep.imageMemoryBarrierCount = 2;
            postDep.pImageMemoryBarriers = toShaderRead.data();
            vkCmdPipelineBarrier2(cmd, &postDep);
        }

        if (m_timestampPool != VK_NULL_HANDLE)
            vkCmdWriteTimestamp2(cmd, VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT, m_timestampPool, frameIndex * 2 + 1);

        frame.recordedDispatch = true;
        return true;
    }

    VkImageView VulkanRtShadowPass::GetDirectLightView(uint32_t frameIndex) const noexcept
    {
        return (m_initialized && frameIndex < m_frames.size()) ? m_frames[frameIndex]->directLightImage.GetView() : VK_NULL_HANDLE;
    }

    VkImageView VulkanRtShadowPass::GetShadowAuxView(uint32_t frameIndex) const noexcept
    {
        return (m_initialized && frameIndex < m_frames.size()) ? m_frames[frameIndex]->shadowAuxImage.GetView() : VK_NULL_HANDLE;
    }
}
