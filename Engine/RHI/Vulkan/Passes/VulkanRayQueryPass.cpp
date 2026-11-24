#include "VulkanRayQueryPass.h"

#include <RHI/Vulkan/Assets/VulkanEnvironment.h>
#include <RHI/Vulkan/Pipeline/VulkanShader.h>
#include <RHI/Vulkan/VulkanContext.h>
#include <RHI/Vulkan/VulkanMemoryManager.h>

#include <array>
#include <iostream>
#include <string>

namespace Aether
{
    VulkanRayQueryPass::~VulkanRayQueryPass()
    {
        Shutdown();
    }

    bool VulkanRayQueryPass::Initialize(
        const VulkanContext& context,
        const VulkanMemoryManager& memory,
        uint32_t framesInFlight,
        VkExtent2D extent,
        VkDescriptorSetLayout toneMapSetLayout)
    {
        if (m_initialized || framesInFlight == 0 || extent.width == 0 || extent.height == 0 ||
            context.GetDevice() == VK_NULL_HANDLE || toneMapSetLayout == VK_NULL_HANDLE)
            return false;

        if (!context.GetRayTracingCapabilities().IsReady())
            return false;

        m_physicalDevice = context.GetPhysicalDevice();
        m_device = context.GetDevice();
        m_memory = &memory;
        m_extent = extent;

        // Set 0: binding 0 = TLAS, binding 1 = output storage image, binding 2 = params UBO.
        const std::array<VkDescriptorSetLayoutBinding, 3> computeBindings = {{
            { 0, VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr },
            { 1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,              1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr },
            { 2, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,             1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr }
        }};
        VkDescriptorSetLayoutCreateInfo computeLayoutInfo{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
        computeLayoutInfo.bindingCount = static_cast<uint32_t>(computeBindings.size());
        computeLayoutInfo.pBindings = computeBindings.data();
        if (vkCreateDescriptorSetLayout(m_device, &computeLayoutInfo, nullptr, &m_computeSetLayout) != VK_SUCCESS)
        {
            Shutdown();
            return false;
        }

        if (VulkanEnvironment::CreateLayout(m_device, m_environmentSetLayout) != VK_SUCCESS)
        {
            Shutdown();
            return false;
        }

        const std::array<VkDescriptorPoolSize, 4> poolSizes = {{
            { VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, framesInFlight },
            { VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,              framesInFlight },
            { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,             framesInFlight },
            { VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,              framesInFlight }
        }};
        VkDescriptorPoolCreateInfo poolInfo{ VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
        poolInfo.maxSets = framesInFlight * 2;
        poolInfo.poolSizeCount = static_cast<uint32_t>(poolSizes.size());
        poolInfo.pPoolSizes = poolSizes.data();
        if (vkCreateDescriptorPool(m_device, &poolInfo, nullptr, &m_descriptorPool) != VK_SUCCESS)
        {
            Shutdown();
            return false;
        }

        const std::array<VkDescriptorSetLayout, 2> setLayouts = {
            m_computeSetLayout,
            m_environmentSetLayout
        };
        VkPipelineLayoutCreateInfo pipelineLayoutInfo{ VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
        pipelineLayoutInfo.setLayoutCount = static_cast<uint32_t>(setLayouts.size());
        pipelineLayoutInfo.pSetLayouts = setLayouts.data();
        if (vkCreatePipelineLayout(m_device, &pipelineLayoutInfo, nullptr, &m_pipelineLayout) != VK_SUCCESS)
        {
            Shutdown();
            return false;
        }

        VulkanShader computeShader;
        const std::string shaderPath = std::string(AETHER_SHADER_DIR) + "/rayquery_debug.comp.spv";
        if (!computeShader.Initialize(m_device, shaderPath))
        {
            std::cerr << "VulkanRayQueryPass: failed to load " << shaderPath << '\n';
            Shutdown();
            return false;
        }

        VkComputePipelineCreateInfo pipelineInfo{ VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO };
        pipelineInfo.layout = m_pipelineLayout;
        pipelineInfo.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        pipelineInfo.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
        pipelineInfo.stage.module = computeShader.GetShaderModule();
        pipelineInfo.stage.pName = "main";
        if (vkCreateComputePipelines(m_device, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &m_pipeline) != VK_SUCCESS)
        {
            std::cerr << "VulkanRayQueryPass: failed to create Ray Query compute pipeline.\n";
            Shutdown();
            return false;
        }

        m_frames.clear();
        m_frames.reserve(framesInFlight);
        for (uint32_t i = 0; i < framesInFlight; ++i)
        {
            auto frame = std::make_unique<FrameResources>();

            VkDescriptorSetAllocateInfo computeAlloc{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
            computeAlloc.descriptorPool = m_descriptorPool;
            computeAlloc.descriptorSetCount = 1;
            computeAlloc.pSetLayouts = &m_computeSetLayout;
            if (vkAllocateDescriptorSets(m_device, &computeAlloc, &frame->computeSet) != VK_SUCCESS)
            {
                Shutdown();
                return false;
            }

            VkDescriptorSetAllocateInfo toneAlloc{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
            toneAlloc.descriptorPool = m_descriptorPool;
            toneAlloc.descriptorSetCount = 1;
            toneAlloc.pSetLayouts = &toneMapSetLayout;
            if (vkAllocateDescriptorSets(m_device, &toneAlloc, &frame->toneMapSet) != VK_SUCCESS)
            {
                Shutdown();
                return false;
            }

            if (frame->paramsBuffer.Initialize(
                    m_device, *m_memory, sizeof(RayQueryUniforms),
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

        // Optional GPU timestamp query pool (2 timestamps per frame slot: begin & end of compute dispatch).
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
            VkQueryPoolCreateInfo queryInfo{ VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO };
            queryInfo.queryType = VK_QUERY_TYPE_TIMESTAMP;
            queryInfo.queryCount = framesInFlight * 2;
            if (vkCreateQueryPool(m_device, &queryInfo, nullptr, &m_timestampPool) == VK_SUCCESS)
                m_timestampPeriodNs = static_cast<double>(props.limits.timestampPeriod);
        }

        m_initialized = true;
        return true;
    }

    bool VulkanRayQueryPass::CreateOutputImages(VkExtent2D extent)
    {
        constexpr VkImageUsageFlags kOutputUsage =
            VK_IMAGE_USAGE_STORAGE_BIT |
            VK_IMAGE_USAGE_SAMPLED_BIT |
            VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
            VK_IMAGE_USAGE_TRANSFER_DST_BIT;

        for (auto& framePtr : m_frames)
        {
            auto& frame = *framePtr;
            frame.outputImage.Shutdown();
            frame.outputLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            const VkResult res = frame.outputImage.Initialize(
                m_device,
                *m_memory,
                extent,
                VK_FORMAT_R16G16B16A16_SFLOAT,
                kOutputUsage,
                VK_IMAGE_ASPECT_COLOR_BIT);
            if (res != VK_SUCCESS)
            {
                std::cerr << "VulkanRayQueryPass: failed to create output storage image (" << res << ").\n";
                return false;
            }
            UpdateFrameStaticDescriptors(frame);
        }
        m_extent = extent;
        return true;
    }

    void VulkanRayQueryPass::UpdateFrameStaticDescriptors(FrameResources& frame) const
    {
        const VkDescriptorImageInfo storageInfo{
            VK_NULL_HANDLE,
            frame.outputImage.GetView(),
            VK_IMAGE_LAYOUT_GENERAL
        };
        const VkDescriptorBufferInfo uboInfo{
            frame.paramsBuffer.GetBuffer(),
            0,
            sizeof(RayQueryUniforms)
        };
        const VkDescriptorImageInfo sampledInfo{
            VK_NULL_HANDLE,
            frame.outputImage.GetView(),
            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
        };

        std::array<VkWriteDescriptorSet, 3> writes{};
        writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[0].dstSet = frame.computeSet;
        writes[0].dstBinding = 1;
        writes[0].descriptorCount = 1;
        writes[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        writes[0].pImageInfo = &storageInfo;

        writes[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[1].dstSet = frame.computeSet;
        writes[1].dstBinding = 2;
        writes[1].descriptorCount = 1;
        writes[1].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        writes[1].pBufferInfo = &uboInfo;

        writes[2].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[2].dstSet = frame.toneMapSet;
        writes[2].dstBinding = 0;
        writes[2].descriptorCount = 1;
        writes[2].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
        writes[2].pImageInfo = &sampledInfo;

        vkUpdateDescriptorSets(m_device, static_cast<uint32_t>(writes.size()), writes.data(), 0, nullptr);
    }

    bool VulkanRayQueryPass::OnResize(VkExtent2D newExtent)
    {
        if (!m_initialized || newExtent.width == 0 || newExtent.height == 0)
            return false;
        return CreateOutputImages(newExtent);
    }

    void VulkanRayQueryPass::OnFrameFenceCompleted(uint32_t frameIndex)
    {
        if (!m_initialized || frameIndex >= m_frames.size())
            return;

        auto& frame = *m_frames[frameIndex];
        if (frame.hasPendingTimestamps && frame.recordedDispatch &&
            m_timestampPool != VK_NULL_HANDLE && m_timestampPeriodNs > 0.0)
        {
            std::array<uint64_t, 2> stamps{};
            if (vkGetQueryPoolResults(
                    m_device, m_timestampPool, frameIndex * 2, 2,
                    sizeof(stamps), stamps.data(), sizeof(uint64_t),
                    VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT) == VK_SUCCESS &&
                stamps[1] >= stamps[0])
            {
                m_lastGpuTimeMs =
                    static_cast<double>(stamps[1] - stamps[0]) * m_timestampPeriodNs * 1e-6;
            }
        }
        frame.hasPendingTimestamps = false;
    }

    bool VulkanRayQueryPass::Execute(
        VkCommandBuffer cmd,
        uint32_t frameIndex,
        VkAccelerationStructureKHR tlasHandle,
        bool tlasUpdatedThisFrame,
        VkDescriptorSet environmentSet,
        const glm::mat4& clipToWorldDirection,
        const glm::vec3& cameraPosition,
        const RayQueryConfig& config,
        VkBuffer optionalReadbackBuffer)
    {
        if (!m_initialized || cmd == VK_NULL_HANDLE || frameIndex >= m_frames.size() ||
            environmentSet == VK_NULL_HANDLE)
            return false;

        auto& frame = *m_frames[frameIndex];
        frame.recordedDispatch = false;

        const RayQueryConfig clean = SanitizeRayQueryConfig(config);

        // Empty scene (no valid TLAS): clear output cleanly without writing a null AS descriptor.
        if (tlasHandle == VK_NULL_HANDLE)
        {
            frame.boundTlas = VK_NULL_HANDLE;
            VkImageMemoryBarrier2 toClear{ VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2 };
            toClear.srcStageMask = frame.outputLayout == VK_IMAGE_LAYOUT_UNDEFINED
                ? VK_PIPELINE_STAGE_2_NONE
                : (VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_COPY_BIT);
            toClear.srcAccessMask = frame.outputLayout == VK_IMAGE_LAYOUT_UNDEFINED
                ? 0
                : (VK_ACCESS_2_SHADER_SAMPLED_READ_BIT | VK_ACCESS_2_TRANSFER_READ_BIT);
            toClear.dstStageMask = VK_PIPELINE_STAGE_2_CLEAR_BIT;
            toClear.dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
            toClear.oldLayout = frame.outputLayout;
            toClear.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            toClear.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            toClear.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            toClear.image = frame.outputImage.GetImage();
            toClear.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };

            VkDependencyInfo depClear{ VK_STRUCTURE_TYPE_DEPENDENCY_INFO };
            depClear.imageMemoryBarrierCount = 1;
            depClear.pImageMemoryBarriers = &toClear;
            vkCmdPipelineBarrier2(cmd, &depClear);

            VkClearColorValue clearVal{};
            if (clean.debugMode == RayQueryDebugMode::RawReadback)
            {
                clearVal.float32[0] = 0.0f;
                clearVal.float32[1] = 0.0f;
                clearVal.float32[2] = -1.0f;
                clearVal.float32[3] = -1.0f;
            }
            else
            {
                clearVal.float32[0] = 0.04f;
                clearVal.float32[1] = 0.08f;
                clearVal.float32[2] = 0.22f;
                clearVal.float32[3] = 1.0f;
            }
            const VkImageSubresourceRange range{ VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
            vkCmdClearColorImage(
                cmd, frame.outputImage.GetImage(), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                &clearVal, 1, &range);

            VkImageLayout currLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            VkPipelineStageFlags2 currStage = VK_PIPELINE_STAGE_2_CLEAR_BIT;
            VkAccessFlags2 currAccess = VK_ACCESS_2_TRANSFER_WRITE_BIT;

            if (optionalReadbackBuffer != VK_NULL_HANDLE)
            {
                VkImageMemoryBarrier2 toCopy{ VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2 };
                toCopy.srcStageMask = currStage;
                toCopy.srcAccessMask = currAccess;
                toCopy.dstStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
                toCopy.dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT;
                toCopy.oldLayout = currLayout;
                toCopy.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
                toCopy.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                toCopy.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                toCopy.image = frame.outputImage.GetImage();
                toCopy.subresourceRange = range;
                VkDependencyInfo depCopy{ VK_STRUCTURE_TYPE_DEPENDENCY_INFO };
                depCopy.imageMemoryBarrierCount = 1;
                depCopy.pImageMemoryBarriers = &toCopy;
                vkCmdPipelineBarrier2(cmd, &depCopy);

                VkBufferImageCopy region{};
                region.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
                region.imageExtent = { m_extent.width, m_extent.height, 1 };
                vkCmdCopyImageToBuffer(
                    cmd, frame.outputImage.GetImage(), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                    optionalReadbackBuffer, 1, &region);

                VkBufferMemoryBarrier2 hostBarrier{ VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2 };
                hostBarrier.srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
                hostBarrier.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
                hostBarrier.dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT;
                hostBarrier.dstAccessMask = VK_ACCESS_2_HOST_READ_BIT;
                hostBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                hostBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                hostBarrier.buffer = optionalReadbackBuffer;
                hostBarrier.size = VK_WHOLE_SIZE;
                VkDependencyInfo depHost{ VK_STRUCTURE_TYPE_DEPENDENCY_INFO };
                depHost.bufferMemoryBarrierCount = 1;
                depHost.pBufferMemoryBarriers = &hostBarrier;
                vkCmdPipelineBarrier2(cmd, &depHost);

                currLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
                currStage = VK_PIPELINE_STAGE_2_COPY_BIT;
                currAccess = VK_ACCESS_2_TRANSFER_READ_BIT;
            }

            VkImageMemoryBarrier2 toRead{ VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2 };
            toRead.srcStageMask = currStage;
            toRead.srcAccessMask = currAccess;
            toRead.dstStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
            toRead.dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
            toRead.oldLayout = currLayout;
            toRead.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            toRead.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            toRead.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            toRead.image = frame.outputImage.GetImage();
            toRead.subresourceRange = range;
            VkDependencyInfo depRead{ VK_STRUCTURE_TYPE_DEPENDENCY_INFO };
            depRead.imageMemoryBarrierCount = 1;
            depRead.pImageMemoryBarriers = &toRead;
            vkCmdPipelineBarrier2(cmd, &depRead);

            frame.outputLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            return true;
        }

        // Update TLAS descriptor binding whenever the TLAS handle changes or was rebuilt this frame.
        if (frame.boundTlas != tlasHandle || tlasUpdatedThisFrame)
        {
            VkWriteDescriptorSetAccelerationStructureKHR asWrite{
                VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_ACCELERATION_STRUCTURE_KHR
            };
            asWrite.accelerationStructureCount = 1;
            asWrite.pAccelerationStructures = &tlasHandle;

            VkWriteDescriptorSet write{ VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
            write.pNext = &asWrite;
            write.dstSet = frame.computeSet;
            write.dstBinding = 0;
            write.descriptorCount = 1;
            write.descriptorType = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;
            vkUpdateDescriptorSets(m_device, 1, &write, 0, nullptr);
            frame.boundTlas = tlasHandle;
        }

        RayQueryUniforms uniforms{};
        uniforms.clipToWorldDirection = clipToWorldDirection;
        uniforms.cameraOriginAndTMin = glm::vec4(cameraPosition, clean.tMin);
        uniforms.rayLimitsAndSize = glm::vec4(
            clean.tMax,
            clean.maxDistanceVisualization,
            static_cast<float>(m_extent.width),
            static_cast<float>(m_extent.height));
        uniforms.modeAndFlags[0] = static_cast<uint32_t>(clean.debugMode);
        uniforms.modeAndFlags[1] = 1u;
        if (frame.paramsBuffer.UploadData(&uniforms, sizeof(uniforms)) != VK_SUCCESS)
            return false;

        // Synchronization2:
        // 1. TLAS build/update write -> Compute Shader AS read (when TLAS was written this frame)
        // 2. Host UBO write -> Compute Shader uniform read
        // 3. outputImage -> GENERAL for storage write
        std::array<VkMemoryBarrier2, 2> memBarriers{};
        uint32_t memBarrierCount = 0;

        memBarriers[memBarrierCount].sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2;
        memBarriers[memBarrierCount].srcStageMask = VK_PIPELINE_STAGE_2_HOST_BIT;
        memBarriers[memBarrierCount].srcAccessMask = VK_ACCESS_2_HOST_WRITE_BIT;
        memBarriers[memBarrierCount].dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
        memBarriers[memBarrierCount].dstAccessMask = VK_ACCESS_2_UNIFORM_READ_BIT;
        ++memBarrierCount;

        if (tlasUpdatedThisFrame)
        {
            memBarriers[memBarrierCount].sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2;
            memBarriers[memBarrierCount].srcStageMask =
                VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR;
            memBarriers[memBarrierCount].srcAccessMask =
                VK_ACCESS_2_ACCELERATION_STRUCTURE_WRITE_BIT_KHR;
            memBarriers[memBarrierCount].dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
            memBarriers[memBarrierCount].dstAccessMask =
                VK_ACCESS_2_ACCELERATION_STRUCTURE_READ_BIT_KHR;
            ++memBarrierCount;
        }

        VkImageMemoryBarrier2 toGeneral{ VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2 };
        toGeneral.srcStageMask = frame.outputLayout == VK_IMAGE_LAYOUT_UNDEFINED
            ? VK_PIPELINE_STAGE_2_NONE
            : (VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_COPY_BIT);
        toGeneral.srcAccessMask = frame.outputLayout == VK_IMAGE_LAYOUT_UNDEFINED
            ? 0
            : (VK_ACCESS_2_SHADER_SAMPLED_READ_BIT | VK_ACCESS_2_TRANSFER_READ_BIT);
        toGeneral.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
        toGeneral.dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
        toGeneral.oldLayout = frame.outputLayout;
        toGeneral.newLayout = VK_IMAGE_LAYOUT_GENERAL;
        toGeneral.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        toGeneral.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        toGeneral.image = frame.outputImage.GetImage();
        toGeneral.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };

        VkDependencyInfo preDispatchDep{ VK_STRUCTURE_TYPE_DEPENDENCY_INFO };
        preDispatchDep.memoryBarrierCount = memBarrierCount;
        preDispatchDep.pMemoryBarriers = memBarriers.data();
        preDispatchDep.imageMemoryBarrierCount = 1;
        preDispatchDep.pImageMemoryBarriers = &toGeneral;
        vkCmdPipelineBarrier2(cmd, &preDispatchDep);

        if (m_timestampPool != VK_NULL_HANDLE)
        {
            vkCmdResetQueryPool(cmd, m_timestampPool, frameIndex * 2, 2);
            vkCmdWriteTimestamp2(
                cmd, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, m_timestampPool, frameIndex * 2);
        }

        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_pipeline);
        const std::array<VkDescriptorSet, 2> sets = { frame.computeSet, environmentSet };
        vkCmdBindDescriptorSets(
            cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_pipelineLayout,
            0, static_cast<uint32_t>(sets.size()), sets.data(), 0, nullptr);

        const uint32_t groupX = (m_extent.width + 7u) / 8u;
        const uint32_t groupY = (m_extent.height + 7u) / 8u;
        vkCmdDispatch(cmd, groupX, groupY, 1);

        if (m_timestampPool != VK_NULL_HANDLE)
        {
            vkCmdWriteTimestamp2(
                cmd, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, m_timestampPool, frameIndex * 2 + 1);
        }

        frame.recordedDispatch = true;

        VkImageLayout currLayout = VK_IMAGE_LAYOUT_GENERAL;
        VkPipelineStageFlags2 currStage = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
        VkAccessFlags2 currAccess = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;

        if (optionalReadbackBuffer != VK_NULL_HANDLE)
        {
            VkImageMemoryBarrier2 toCopy{ VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2 };
            toCopy.srcStageMask = currStage;
            toCopy.srcAccessMask = currAccess;
            toCopy.dstStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
            toCopy.dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT;
            toCopy.oldLayout = currLayout;
            toCopy.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
            toCopy.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            toCopy.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            toCopy.image = frame.outputImage.GetImage();
            toCopy.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
            VkDependencyInfo depCopy{ VK_STRUCTURE_TYPE_DEPENDENCY_INFO };
            depCopy.imageMemoryBarrierCount = 1;
            depCopy.pImageMemoryBarriers = &toCopy;
            vkCmdPipelineBarrier2(cmd, &depCopy);

            VkBufferImageCopy region{};
            region.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
            region.imageExtent = { m_extent.width, m_extent.height, 1 };
            vkCmdCopyImageToBuffer(
                cmd, frame.outputImage.GetImage(), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                optionalReadbackBuffer, 1, &region);

            VkBufferMemoryBarrier2 hostBarrier{ VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2 };
            hostBarrier.srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
            hostBarrier.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
            hostBarrier.dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT;
            hostBarrier.dstAccessMask = VK_ACCESS_2_HOST_READ_BIT;
            hostBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            hostBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            hostBarrier.buffer = optionalReadbackBuffer;
            hostBarrier.size = VK_WHOLE_SIZE;
            VkDependencyInfo depHost{ VK_STRUCTURE_TYPE_DEPENDENCY_INFO };
            depHost.bufferMemoryBarrierCount = 1;
            depHost.pBufferMemoryBarriers = &hostBarrier;
            vkCmdPipelineBarrier2(cmd, &depHost);

            currLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
            currStage = VK_PIPELINE_STAGE_2_COPY_BIT;
            currAccess = VK_ACCESS_2_TRANSFER_READ_BIT;
        }

        VkImageMemoryBarrier2 toSampled{ VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2 };
        toSampled.srcStageMask = currStage;
        toSampled.srcAccessMask = currAccess;
        toSampled.dstStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
        toSampled.dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
        toSampled.oldLayout = currLayout;
        toSampled.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        toSampled.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        toSampled.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        toSampled.image = frame.outputImage.GetImage();
        toSampled.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };

        VkDependencyInfo depSampled{ VK_STRUCTURE_TYPE_DEPENDENCY_INFO };
        depSampled.imageMemoryBarrierCount = 1;
        depSampled.pImageMemoryBarriers = &toSampled;
        vkCmdPipelineBarrier2(cmd, &depSampled);

        frame.outputLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        return true;
    }

    void VulkanRayQueryPass::OnFrameSubmitted(uint32_t frameIndex)
    {
        if (!m_initialized || frameIndex >= m_frames.size())
            return;
        m_frames[frameIndex]->hasPendingTimestamps =
            m_timestampPool != VK_NULL_HANDLE && m_frames[frameIndex]->recordedDispatch;
    }

    VkDescriptorSet VulkanRayQueryPass::GetOutputToneMapSet(uint32_t frameIndex) const noexcept
    {
        if (!m_initialized || frameIndex >= m_frames.size())
            return VK_NULL_HANDLE;
        return m_frames[frameIndex]->toneMapSet;
    }

    void VulkanRayQueryPass::Shutdown() noexcept
    {
        if (m_device != VK_NULL_HANDLE)
        {
            if (m_pipeline != VK_NULL_HANDLE)
            {
                vkDestroyPipeline(m_device, m_pipeline, nullptr);
                m_pipeline = VK_NULL_HANDLE;
            }
            if (m_pipelineLayout != VK_NULL_HANDLE)
            {
                vkDestroyPipelineLayout(m_device, m_pipelineLayout, nullptr);
                m_pipelineLayout = VK_NULL_HANDLE;
            }
            if (m_descriptorPool != VK_NULL_HANDLE)
            {
                vkDestroyDescriptorPool(m_device, m_descriptorPool, nullptr);
                m_descriptorPool = VK_NULL_HANDLE;
            }
            if (m_computeSetLayout != VK_NULL_HANDLE)
            {
                vkDestroyDescriptorSetLayout(m_device, m_computeSetLayout, nullptr);
                m_computeSetLayout = VK_NULL_HANDLE;
            }
            if (m_environmentSetLayout != VK_NULL_HANDLE)
            {
                vkDestroyDescriptorSetLayout(m_device, m_environmentSetLayout, nullptr);
                m_environmentSetLayout = VK_NULL_HANDLE;
            }
            if (m_timestampPool != VK_NULL_HANDLE)
            {
                vkDestroyQueryPool(m_device, m_timestampPool, nullptr);
                m_timestampPool = VK_NULL_HANDLE;
            }
        }

        for (auto& framePtr : m_frames)
        {
            if (!framePtr)
                continue;
            framePtr->outputImage.Shutdown();
            framePtr->paramsBuffer.Shutdown();
            framePtr->outputLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            framePtr->computeSet = VK_NULL_HANDLE;
            framePtr->toneMapSet = VK_NULL_HANDLE;
            framePtr->boundTlas = VK_NULL_HANDLE;
        }
        m_frames.clear();

        m_physicalDevice = VK_NULL_HANDLE;
        m_device = VK_NULL_HANDLE;
        m_memory = nullptr;
        m_extent = {};
        m_timestampPeriodNs = 0.0;
        m_lastGpuTimeMs = 0.0;
        m_initialized = false;
    }
}
