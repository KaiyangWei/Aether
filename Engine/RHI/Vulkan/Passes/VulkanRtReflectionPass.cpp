#include "VulkanRtReflectionPass.h"

#include <RHI/Vulkan/Assets/VulkanEnvironment.h>
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
    VulkanRtReflectionPass::~VulkanRtReflectionPass()
    {
        Shutdown();
    }

    bool VulkanRtReflectionPass::Initialize(
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
            {  0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,             1,                       VK_SHADER_STAGE_COMPUTE_BIT, nullptr },
            {  1, VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, 1,                       VK_SHADER_STAGE_COMPUTE_BIT, nullptr },
            {  2, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,              1,                       VK_SHADER_STAGE_COMPUTE_BIT, nullptr },
            {  3, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,              1,                       VK_SHADER_STAGE_COMPUTE_BIT, nullptr },
            {  4, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,              1,                       VK_SHADER_STAGE_COMPUTE_BIT, nullptr },
            {  5, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,              1,                       VK_SHADER_STAGE_COMPUTE_BIT, nullptr },
            {  6, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,             1,                       VK_SHADER_STAGE_COMPUTE_BIT, nullptr },
            {  7, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,             1,                       VK_SHADER_STAGE_COMPUTE_BIT, nullptr },
            {  8, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,             1,                       VK_SHADER_STAGE_COMPUTE_BIT, nullptr },
            {  9, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,              kMaxRtAlphaMaskTextures, VK_SHADER_STAGE_COMPUTE_BIT, nullptr },
            { 10, VK_DESCRIPTOR_TYPE_SAMPLER,                    kMaxRtAlphaMaskTextures, VK_SHADER_STAGE_COMPUTE_BIT, nullptr },
            { 11, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,              1,                       VK_SHADER_STAGE_COMPUTE_BIT, nullptr },
            { 12, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,              1,                       VK_SHADER_STAGE_COMPUTE_BIT, nullptr }
        }};

        VkDescriptorSetLayoutCreateInfo layoutInfo{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
        layoutInfo.bindingCount = static_cast<uint32_t>(bindings.size());
        layoutInfo.pBindings = bindings.data();
        if (vkCreateDescriptorSetLayout(m_device, &layoutInfo, nullptr, &m_descriptorSetLayout) != VK_SUCCESS)
        {
            Shutdown();
            return false;
        }

        if (VulkanEnvironment::CreateLayout(m_device, m_environmentSetLayout) != VK_SUCCESS)
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

        const std::array<VkDescriptorSetLayout, 2> setLayouts = { m_descriptorSetLayout, m_environmentSetLayout };
        VkPipelineLayoutCreateInfo plInfo{ VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
        plInfo.setLayoutCount = static_cast<uint32_t>(setLayouts.size());
        plInfo.pSetLayouts = setLayouts.data();
        if (vkCreatePipelineLayout(m_device, &plInfo, nullptr, &m_pipelineLayout) != VK_SUCCESS)
        {
            Shutdown();
            return false;
        }

        VulkanShader shader;
        const std::string spvPath = std::string(AETHER_SHADER_DIR) + "/rt_reflection.comp.spv";
        if (!shader.Initialize(m_device, spvPath))
        {
            std::cerr << "VulkanRtReflectionPass: failed to load " << spvPath << '\n';
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
            std::cerr << "VulkanRtReflectionPass: failed to create compute pipeline.\n";
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
                    m_device, *m_memory, sizeof(ReflectionParamsUbo),
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

    bool VulkanRtReflectionPass::CreateOutputImages(VkExtent2D extent)
    {
        constexpr VkImageUsageFlags kOutUsage =
            VK_IMAGE_USAGE_STORAGE_BIT |
            VK_IMAGE_USAGE_SAMPLED_BIT |
            VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
            VK_IMAGE_USAGE_TRANSFER_DST_BIT;

        for (auto& framePtr : m_frames)
        {
            auto& f = *framePtr;
            f.reflectionImage.Shutdown();
            f.reflectionAuxImage.Shutdown();

            if (f.reflectionImage.Initialize(
                    m_device, *m_memory, extent, VK_FORMAT_R16G16B16A16_SFLOAT,
                    kOutUsage, VK_IMAGE_ASPECT_COLOR_BIT) != VK_SUCCESS ||
                f.reflectionAuxImage.Initialize(
                    m_device, *m_memory, extent, VK_FORMAT_R16G16_SFLOAT,
                    kOutUsage, VK_IMAGE_ASPECT_COLOR_BIT) != VK_SUCCESS)
                return false;
        }
        m_extent = extent;
        ++m_sizeVersion;
        return true;
    }

    bool VulkanRtReflectionPass::OnResize(VkExtent2D newExtent)
    {
        if (!m_initialized || newExtent.width == 0 || newExtent.height == 0)
            return false;
        return CreateOutputImages(newExtent);
    }

    void VulkanRtReflectionPass::Shutdown() noexcept
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
        if (m_environmentSetLayout != VK_NULL_HANDLE && m_device != VK_NULL_HANDLE)
        {
            vkDestroyDescriptorSetLayout(m_device, m_environmentSetLayout, nullptr);
            m_environmentSetLayout = VK_NULL_HANDLE;
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

    void VulkanRtReflectionPass::OnFrameFenceCompleted(uint32_t frameIndex)
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

    void VulkanRtReflectionPass::OnFrameSubmitted(uint32_t frameIndex)
    {
        if (!m_initialized || frameIndex >= m_frames.size())
            return;
        auto& f = *m_frames[frameIndex];
        f.hasPendingTimestamp = (m_timestampPool != VK_NULL_HANDLE && f.recordedDispatch);
    }

    void VulkanRtReflectionPass::RefreshDescriptorSet(
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

        const VkDescriptorBufferInfo uboInfo{ frame.paramsBuffer.GetBuffer(), 0, sizeof(ReflectionParamsUbo) };
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

        const VkDescriptorImageInfo outReflInfo{
            VK_NULL_HANDLE, frame.reflectionImage.GetView(), VK_IMAGE_LAYOUT_GENERAL
        };
        const VkDescriptorImageInfo outAuxInfo{
            VK_NULL_HANDLE, frame.reflectionAuxImage.GetView(), VK_IMAGE_LAYOUT_GENERAL
        };
        writes.push_back({ VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, frame.descriptorSet,
            11, 0, 1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, &outReflInfo, nullptr, nullptr });
        writes.push_back({ VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, frame.descriptorSet,
            12, 0, 1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, &outAuxInfo, nullptr, nullptr });

        vkUpdateDescriptorSets(m_device, static_cast<uint32_t>(writes.size()), writes.data(), 0, nullptr);

        frame.boundTlas = tlasHandle;
        frame.boundGeomGeneration = geomGen;
        frame.boundInstGeneration = instGen;
        frame.boundMaskGeneration = maskGen;
        frame.sizeVersion = m_sizeVersion;
    }

    bool VulkanRtReflectionPass::Execute(
        VkCommandBuffer cmd,
        uint32_t frameIndex,
        const VulkanRayTracingScene& rtScene,
        bool tlasWrittenThisFrame,
        const VulkanHybridGBufferPass& gBufferPass,
        VkDescriptorSet environmentSet,
        const glm::vec3& cameraPosition,
        const glm::vec3& sunDirectionToLight,
        const glm::vec3& sunRadiance,
        bool sunEnabled,
        bool fillEnabled,
        const HybridRtConfig& rawConfig,
        uint32_t globalFrameCounter,
        std::span<const RtGpuLight> lights,
        VkBuffer optionalReflectionReadbackBuffer,
        VkBuffer optionalAuxReadbackBuffer)
    {
        if (!m_initialized || cmd == VK_NULL_HANDLE || frameIndex >= m_frames.size() ||
            environmentSet == VK_NULL_HANDLE)
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
                    frame.reflectionImage.GetImage(), { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 }
                },
                {
                    VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2, nullptr,
                    VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_TRANSFER_BIT, 0,
                    VK_PIPELINE_STAGE_2_CLEAR_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                    VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                    VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED,
                    frame.reflectionAuxImage.GetImage(), { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 }
                }
            }};
            VkDependencyInfo depClear{ VK_STRUCTURE_TYPE_DEPENDENCY_INFO };
            depClear.imageMemoryBarrierCount = 2;
            depClear.pImageMemoryBarriers = toClear.data();
            vkCmdPipelineBarrier2(cmd, &depClear);

            const VkClearColorValue zeroClear{ { 0.0f, 0.0f, 0.0f, 0.0f } };
            const VkImageSubresourceRange range{ VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
            vkCmdClearColorImage(cmd, frame.reflectionImage.GetImage(), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &zeroClear, 1, &range);
            vkCmdClearColorImage(cmd, frame.reflectionAuxImage.GetImage(), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &zeroClear, 1, &range);

            const std::array<VkImageMemoryBarrier2, 2> toRead = {{
                {
                    VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2, nullptr,
                    VK_PIPELINE_STAGE_2_CLEAR_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                    VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
                    VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                    VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED,
                    frame.reflectionImage.GetImage(), { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 }
                },
                {
                    VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2, nullptr,
                    VK_PIPELINE_STAGE_2_CLEAR_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                    VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
                    VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                    VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED,
                    frame.reflectionAuxImage.GetImage(), { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 }
                }
            }};
            VkDependencyInfo depRead{ VK_STRUCTURE_TYPE_DEPENDENCY_INFO };
            depRead.imageMemoryBarrierCount = 2;
            depRead.pImageMemoryBarriers = toRead.data();
            vkCmdPipelineBarrier2(cmd, &depRead);
            return true;
        }

        ReflectionParamsUbo ubo{};
        ubo.cameraPosAndHasTlas = glm::vec4(cameraPosition, 1.0f);
        const float dirLenSq = glm::dot(sunDirectionToLight, sunDirectionToLight);
        const glm::vec3 sunDir = dirLenSq > 1e-12f
            ? sunDirectionToLight / std::sqrt(dirLenSq)
            : glm::vec3(0.0f, 1.0f, 0.0f);
        ubo.sunDirAndMaxDist = glm::vec4(sunDir, cfg.reflectionMaxDistance);
        ubo.sunColorAndEnabled = glm::vec4(sunRadiance, sunEnabled ? 1.0f : 0.0f);
        ubo.roughnessParams = glm::vec4(
            cfg.reflectionMinRoughnessMirror,
            cfg.reflectionMaxRoughness,
            cfg.shadowNormalBias,
            cfg.shadowDistanceBias);
        ubo.countsAndExtent[0] = m_extent.width;
        ubo.countsAndExtent[1] = m_extent.height;
        ubo.countsAndExtent[2] = globalFrameCounter;
        ubo.countsAndExtent[3] = cfg.reflectionsEnabled ? 1u : 0u;
        ubo.lightFlags[0] = cfg.areaLightsEnabled ? 1u : 0u;
        ubo.lightFlags[1] = static_cast<uint32_t>(std::min<size_t>(lights.size(), kMaxRtSceneLights));
        ubo.lightFlags[2] = cfg.shadowsEnabled ? 1u : 0u;
        ubo.lightFlags[3] = fillEnabled ? 1u : 0u;
        for (uint32_t i = 0; i < ubo.lightFlags[1]; ++i)
            ubo.lights[i] = lights[i];

        if (frame.paramsBuffer.UploadData(&ubo, sizeof(ubo)) != VK_SUCCESS)
            return false;

        RefreshDescriptorSet(frame, frameIndex, rtScene, gBufferPass);

        if (m_timestampPool != VK_NULL_HANDLE)
        {
            vkCmdResetQueryPool(cmd, m_timestampPool, frameIndex * 2, 2);
            vkCmdWriteTimestamp2(cmd, VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, m_timestampPool, frameIndex * 2);
        }

        std::vector<VkMemoryBarrier2> memBarriers;
        memBarriers.push_back({
            VK_STRUCTURE_TYPE_MEMORY_BARRIER_2, nullptr,
            VK_PIPELINE_STAGE_2_HOST_BIT, VK_ACCESS_2_HOST_WRITE_BIT,
            VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_UNIFORM_READ_BIT
        });
        if (tlasWrittenThisFrame)
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
                frame.reflectionImage.GetImage(), { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 }
            },
            {
                VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2, nullptr,
                VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_TRANSFER_BIT, 0,
                VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL,
                VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED,
                frame.reflectionAuxImage.GetImage(), { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 }
            }
        }};

        VkDependencyInfo preDep{ VK_STRUCTURE_TYPE_DEPENDENCY_INFO };
        preDep.memoryBarrierCount = static_cast<uint32_t>(memBarriers.size());
        preDep.pMemoryBarriers = memBarriers.data();
        preDep.imageMemoryBarrierCount = 2;
        preDep.pImageMemoryBarriers = toGeneral.data();
        vkCmdPipelineBarrier2(cmd, &preDep);

        const std::array<VkDescriptorSet, 2> sets = { frame.descriptorSet, environmentSet };
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_pipeline);
        vkCmdBindDescriptorSets(
            cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_pipelineLayout,
            0, static_cast<uint32_t>(sets.size()), sets.data(), 0, nullptr);
        const uint32_t groupsX = (m_extent.width + 7u) / 8u;
        const uint32_t groupsY = (m_extent.height + 7u) / 8u;
        vkCmdDispatch(cmd, groupsX, groupsY, 1u);

        auto finalizeImage = [&](VkImage img, VkBuffer readbackBuf) {
            if (readbackBuf != VK_NULL_HANDLE)
            {
                const VkImageMemoryBarrier2 toCopy{
                    VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2, nullptr,
                    VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                    VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT,
                    VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                    VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED,
                    img, { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 }
                };
                VkDependencyInfo depCopy{ VK_STRUCTURE_TYPE_DEPENDENCY_INFO };
                depCopy.imageMemoryBarrierCount = 1;
                depCopy.pImageMemoryBarriers = &toCopy;
                vkCmdPipelineBarrier2(cmd, &depCopy);

                VkBufferImageCopy region{};
                region.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
                region.imageExtent = { m_extent.width, m_extent.height, 1 };
                vkCmdCopyImageToBuffer(cmd, img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, readbackBuf, 1, &region);

                const VkBufferMemoryBarrier2 hostBarrier{
                    VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2, nullptr,
                    VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                    VK_PIPELINE_STAGE_2_HOST_BIT, VK_ACCESS_2_HOST_READ_BIT,
                    VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED,
                    readbackBuf, 0, VK_WHOLE_SIZE
                };
                const VkImageMemoryBarrier2 toRead{
                    VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2, nullptr,
                    VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT,
                    VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
                    VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                    VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED,
                    img, { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 }
                };
                VkDependencyInfo depAfter{ VK_STRUCTURE_TYPE_DEPENDENCY_INFO };
                depAfter.bufferMemoryBarrierCount = 1;
                depAfter.pBufferMemoryBarriers = &hostBarrier;
                depAfter.imageMemoryBarrierCount = 1;
                depAfter.pImageMemoryBarriers = &toRead;
                vkCmdPipelineBarrier2(cmd, &depAfter);
            }
            else
            {
                const VkImageMemoryBarrier2 toRead{
                    VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2, nullptr,
                    VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                    VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
                    VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                    VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED,
                    img, { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 }
                };
                VkDependencyInfo depRead{ VK_STRUCTURE_TYPE_DEPENDENCY_INFO };
                depRead.imageMemoryBarrierCount = 1;
                depRead.pImageMemoryBarriers = &toRead;
                vkCmdPipelineBarrier2(cmd, &depRead);
            }
        };

        finalizeImage(frame.reflectionImage.GetImage(), optionalReflectionReadbackBuffer);
        finalizeImage(frame.reflectionAuxImage.GetImage(), optionalAuxReadbackBuffer);

        if (m_timestampPool != VK_NULL_HANDLE)
            vkCmdWriteTimestamp2(cmd, VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT, m_timestampPool, frameIndex * 2 + 1);

        frame.recordedDispatch = true;
        return true;
    }

    VkImageView VulkanRtReflectionPass::GetReflectionView(uint32_t frameIndex) const noexcept
    {
        return (m_initialized && frameIndex < m_frames.size()) ? m_frames[frameIndex]->reflectionImage.GetView() : VK_NULL_HANDLE;
    }

    VkImageView VulkanRtReflectionPass::GetReflectionAuxView(uint32_t frameIndex) const noexcept
    {
        return (m_initialized && frameIndex < m_frames.size()) ? m_frames[frameIndex]->reflectionAuxImage.GetView() : VK_NULL_HANDLE;
    }
}
