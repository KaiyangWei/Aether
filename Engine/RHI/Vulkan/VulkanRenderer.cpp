#include "VulkanRenderer.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <iterator>
#include <iostream>
#include <string>

#include <RHI/Vulkan/VulkanContext.h>
#include <RHI/Vulkan/Resources/VulkanMesh.h>
#include <RHI/Vulkan/VulkanCommandManager.h>
#include <RHI/Vulkan/VulkanSyncManager.h>
#include <RHI/Vulkan/Pipeline/VulkanGraphicsPipeline.h>
#include <RHI/Vulkan/Pipeline/VulkanShader.h>
#include <Renderer/Vertex.h>

Aether::VulkanRenderer::~VulkanRenderer()
{
    Shutdown();
}

bool Aether::VulkanRenderer::Initialize(
    VulkanContext& context,
    VulkanCommandManager& commandManager,
    VulkanSyncManager& syncManager,
    const VulkanMemoryManager& memoryManager
)
{
    if (m_context != nullptr)
    {
        std::cerr << "Vulkan Renderer has already been initialized!\n";
        return false;
    }

    if (context.GetDevice() == VK_NULL_HANDLE ||
        context.GetSwapchain() == VK_NULL_HANDLE)
    {
        std::cerr << "Vulkan Context is not ready!\n";
        return false;
    }

    if (context.GetSwapchainImages().empty())
    {
        std::cerr << "Swapchain Images are empty!\n";
        return false;
    }

    if (syncManager.GetFramesInFlight() == 0)
    {
        std::cerr << "Vulkan Sync Manager is not ready!\n";
        return false;
    }

    if (!CreateDepthAttachment(context, memoryManager))
        return false;
    if (!CreateHdrTarget(context, memoryManager) ||
        !m_taaPass.Initialize(
            context, memoryManager, syncManager.GetFramesInFlight(),
            context.GetSwapchainExtent(), m_depthAttachment.GetFormat(),
            m_hdrColor.GetView(), m_depthAttachment.GetView(), m_hdrLayout))
    {
        m_taaPass.Shutdown();
        ReleaseHdr(context.GetDevice());
        m_depthAttachment.Shutdown();
        return false;
    }

    if (context.GetRayTracingCapabilities().IsReady())
    {
        if (!m_rtScene.Initialize(context, memoryManager, syncManager.GetFramesInFlight()))
        {
            std::cerr << "Warning: VulkanRayTracingScene initialization failed; continuing in rasterization-only mode.\n";
        }
        else
        {
            if (!m_rayQueryPass.Initialize(
                    context, memoryManager, syncManager.GetFramesInFlight(),
                    context.GetSwapchainExtent(), m_hdrLayout))
            {
                std::cerr << "Warning: VulkanRayQueryPass initialization failed; Ray Query mode unavailable.\n";
            }
            const bool hybridOk =
                m_hybridGBufferPass.Initialize(
                    context, memoryManager, syncManager.GetFramesInFlight(),
                    context.GetSwapchainExtent(), m_depthAttachment.GetFormat()) &&
                m_rtShadowPass.Initialize(
                    context, memoryManager, syncManager.GetFramesInFlight(),
                    context.GetSwapchainExtent()) &&
                m_rtReflectionPass.Initialize(
                    context, memoryManager, syncManager.GetFramesInFlight(),
                    context.GetSwapchainExtent()) &&
                m_rtDenoiserPass.Initialize(
                    context, memoryManager, syncManager.GetFramesInFlight(),
                    context.GetSwapchainExtent(), m_hdrLayout);
            if (!hybridOk)
            {
                std::cerr << "Warning: Hybrid RT passes initialization failed; falling back cleanly.\n";
                m_rtDenoiserPass.Shutdown();
                m_rtReflectionPass.Shutdown();
                m_rtShadowPass.Shutdown();
                m_hybridGBufferPass.Shutdown();
            }
        }
    }

    std::vector<VkImageView> taaViews;
    taaViews.reserve(syncManager.GetFramesInFlight());
    std::vector<VkImageView> hybridViews;
    if (m_rtDenoiserPass.IsInitialized())
        hybridViews.reserve(syncManager.GetFramesInFlight());
    for (uint32_t f = 0; f < syncManager.GetFramesInFlight(); ++f)
    {
        taaViews.push_back(m_taaPass.GetHistoryView(f));
        if (m_rtDenoiserPass.IsInitialized())
            hybridViews.push_back(m_rtDenoiserPass.GetCompositeView(f));
    }

    if (!m_bloomPass.Initialize(
            context, memoryManager, syncManager.GetFramesInFlight(),
            context.GetSwapchainExtent(), m_hdrColor.GetView(), m_hdrLayout, taaViews, hybridViews) ||
        !m_shadowPass.Initialize(
            context, memoryManager, syncManager.GetFramesInFlight(),
            m_hdrColor.GetFormat(), m_depthAttachment.GetFormat()) ||
        !CreatePipeline(context))
    {
        m_shadowPass.Shutdown();
        m_bloomPass.Shutdown();
        m_rtDenoiserPass.Shutdown();
        m_rtReflectionPass.Shutdown();
        m_rtShadowPass.Shutdown();
        m_hybridGBufferPass.Shutdown();
        m_rayQueryPass.Shutdown();
        m_rtScene.Shutdown();
        m_taaPass.Shutdown();
        ReleaseHdr(context.GetDevice());
        m_depthAttachment.Shutdown();
        return false;
    }

    // Frame-level GPU timestamp query pool (covers complete Rasterization or Hybrid frame pass work).
    {
        VkPhysicalDeviceProperties props{};
        vkGetPhysicalDeviceProperties(context.GetPhysicalDevice(), &props);
        uint32_t qCount = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(context.GetPhysicalDevice(), &qCount, nullptr);
        std::vector<VkQueueFamilyProperties> qProps(qCount);
        vkGetPhysicalDeviceQueueFamilyProperties(context.GetPhysicalDevice(), &qCount, qProps.data());
        const uint32_t gfxFamily = context.GetGraphicsQueueFamily();
        if (props.limits.timestampPeriod > 0.0f &&
            gfxFamily < qProps.size() &&
            qProps[gfxFamily].timestampValidBits > 0)
        {
            VkQueryPoolCreateInfo qInfo{ VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO };
            qInfo.queryType = VK_QUERY_TYPE_TIMESTAMP;
            qInfo.queryCount = syncManager.GetFramesInFlight() * 2;
            if (vkCreateQueryPool(context.GetDevice(), &qInfo, nullptr, &m_frameTimestampPool) == VK_SUCCESS)
            {
                m_frameTimestampPeriodNs = static_cast<double>(props.limits.timestampPeriod);
                m_frameHasPendingTimestamp.assign(syncManager.GetFramesInFlight(), false);
            }
        }
    }

    m_memoryManager = &memoryManager;
    m_context = &context;
    m_commandManager = &commandManager;
    m_syncManager = &syncManager;

    m_imageLayouts.resize(
        context.GetSwapchainImages().size(),
        VK_IMAGE_LAYOUT_UNDEFINED
    );

    m_currentFrame = 0;

    std::cout << "Vulkan Renderer Initialized Successfully!\n";
    std::cout << "   Frames In Flight: "
        << m_syncManager->GetFramesInFlight() << '\n';
    std::cout << "   Swapchain Images: "
        << m_imageLayouts.size() << '\n';
    std::cout << "   Ray Tracing Ready: "
        << (context.GetRayTracingCapabilities().IsReady() ? "Yes" : "No") << '\n';

    return true;
}

const Aether::VulkanRayTracingCapabilities& Aether::VulkanRenderer::GetRayTracingCapabilities() const
{
    static const VulkanRayTracingCapabilities kEmptyCapabilities{};
    return m_context ? m_context->GetRayTracingCapabilities() : kEmptyCapabilities;
}

bool Aether::VulkanRenderer::IsRayTracingReady() const
{
    return GetRayTracingCapabilities().IsReady();
}
bool Aether::VulkanRenderer::CreateDepthAttachment(
    const VulkanContext& context, const VulkanMemoryManager& memory, VkExtent2D extent)
{
    const VkExtent2D targetExtent = (extent.width > 0 && extent.height > 0) ? extent : context.GetSwapchainExtent();
    VkFormat depthFormat = VK_FORMAT_UNDEFINED;
    constexpr VkFormatFeatureFlags desiredDepthFeatures =
        VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT |
        VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT;
    for (VkFormat candidate : { VK_FORMAT_D32_SFLOAT, VK_FORMAT_D16_UNORM })
    {
        VkFormatProperties properties{};
        vkGetPhysicalDeviceFormatProperties(context.GetPhysicalDevice(), candidate, &properties);
        if ((properties.optimalTilingFeatures & desiredDepthFeatures) == desiredDepthFeatures)
        {
            depthFormat = candidate;
            break;
        }
    }

    const VkResult result = m_depthAttachment.Initialize(
        context.GetDevice(), memory, targetExtent, depthFormat,
        VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
        VK_IMAGE_ASPECT_DEPTH_BIT);
    if (result != VK_SUCCESS)
    {
        std::cerr << "Failed to create depth attachment: " << result << '\n';
        return false;
    }
    m_depthLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    return true;
}

bool Aether::VulkanRenderer::CreatePipeline(const VulkanContext& context)
{
    VulkanShader vertexShader;
    VulkanShader fragmentShader;

    const std::string shaderDirectory = AETHER_SHADER_DIR;

    if (!vertexShader.Initialize(
        context.GetDevice(),
        shaderDirectory + "/triangle.vert.spv"))
    {
        std::cerr << "Failed to load vertex shader!\n";
        return false;
    }

    if (!fragmentShader.Initialize(
        context.GetDevice(),
        shaderDirectory + "/triangle.frag.spv"))
    {
        std::cerr << "Failed to load fragment shader!\n";
        return false;
    }

    const VkVertexInputBindingDescription bindingDescription{ 0, sizeof(Vertex), VK_VERTEX_INPUT_RATE_VERTEX };
    const VkVertexInputAttributeDescription attributeDescriptions[] = {
        { 0, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(Vertex, position) },
        { 1, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(Vertex, color) }
    };

    VkPipelineVertexInputStateCreateInfo vertexInputInfo{};
    vertexInputInfo.sType =
        VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    vertexInputInfo.vertexBindingDescriptionCount = 1;
    vertexInputInfo.pVertexBindingDescriptions = &bindingDescription;
    vertexInputInfo.vertexAttributeDescriptionCount =
        static_cast<uint32_t>(std::size(attributeDescriptions));
    vertexInputInfo.pVertexAttributeDescriptions = attributeDescriptions;

    if (!m_graphicsPipeline.Initialize(
        context.GetDevice(),
        m_hdrColor.GetFormat(),
        vertexShader.GetShaderModule(),
        fragmentShader.GetShaderModule(),
        vertexInputInfo,
        MVP_SIZE,
        m_depthAttachment.GetFormat()))
    {
        std::cerr << "Failed to create Graphics Pipeline!\n";
        return false;
    }

    if (!CreatePbrPipelines(context) || !CreateSkyboxPipeline(context) || !CreateTonePipeline(context))
    {
        m_graphicsPipeline.Shutdown();
        m_pbrOpaque.Shutdown();
        m_pbrBlend.Shutdown();
        return false;
    }
    m_pipelineColorFormat = context.GetSwapchainImageFormat();
    return true;
}

bool Aether::VulkanRenderer::ResizeRenderTargetsForBenchmark(VkExtent2D extent)
{
    if (!m_context || !m_memoryManager || !m_syncManager || extent.width == 0 || extent.height == 0)
        return false;

    m_context->WaitIdle();
    m_rtScene.OnDeviceIdle();
    m_depthAttachment.Shutdown();
    m_frameHasPendingTimestamp.assign(m_syncManager->GetFramesInFlight(), false);
    if (!CreateHdrTarget(*m_context, *m_memoryManager, extent) ||
        !CreateDepthAttachment(*m_context, *m_memoryManager, extent) ||
        !m_taaPass.OnResize(extent, m_depthAttachment.GetFormat(), m_hdrColor.GetView(), m_depthAttachment.GetView()))
        return false;

    if (m_hybridGBufferPass.IsInitialized() && !m_hybridGBufferPass.OnResize(extent, m_depthAttachment.GetFormat()))
        return false;
    if (m_rtShadowPass.IsInitialized() && !m_rtShadowPass.OnResize(extent))
        return false;
    if (m_rtReflectionPass.IsInitialized() && !m_rtReflectionPass.OnResize(extent))
        return false;
    if (m_rtDenoiserPass.IsInitialized() && !m_rtDenoiserPass.OnResize(extent))
        return false;
    if (m_rayQueryPass.IsInitialized() && !m_rayQueryPass.OnResize(extent))
        return false;

    std::vector<VkImageView> taaViews;
    std::vector<VkImageView> hybridViews;
    taaViews.reserve(m_syncManager->GetFramesInFlight());
    if (m_rtDenoiserPass.IsInitialized())
        hybridViews.reserve(m_syncManager->GetFramesInFlight());
    for (uint32_t f = 0; f < m_syncManager->GetFramesInFlight(); ++f)
    {
        taaViews.push_back(m_taaPass.GetHistoryView(f));
        if (m_rtDenoiserPass.IsInitialized())
            hybridViews.push_back(m_rtDenoiserPass.GetCompositeView(f));
    }
    return m_bloomPass.OnResize(extent, m_hdrColor.GetView(), false, taaViews, hybridViews);
}

bool Aether::VulkanRenderer::OnSwapchainRecreated()
{
    // The context waits for device idle before replacing the swapchain.
    const VkFormat oldDepthFormat = m_depthAttachment.GetFormat();
    m_depthAttachment.Shutdown();
    if (!CreateHdrTarget(*m_context, *m_memoryManager) ||
        !CreateDepthAttachment(*m_context, *m_memoryManager) ||
        !m_taaPass.OnResize(
            m_context->GetSwapchainExtent(), m_depthAttachment.GetFormat(),
            m_hdrColor.GetView(), m_depthAttachment.GetView()))
        return false;

    if (m_hybridGBufferPass.IsInitialized() &&
        !m_hybridGBufferPass.OnResize(m_context->GetSwapchainExtent(), m_depthAttachment.GetFormat()))
        return false;
    if (m_rtShadowPass.IsInitialized() && !m_rtShadowPass.OnResize(m_context->GetSwapchainExtent()))
        return false;
    if (m_rtReflectionPass.IsInitialized() && !m_rtReflectionPass.OnResize(m_context->GetSwapchainExtent()))
        return false;
    if (m_rtDenoiserPass.IsInitialized() && !m_rtDenoiserPass.OnResize(m_context->GetSwapchainExtent()))
        return false;

    std::vector<VkImageView> taaViews;
    std::vector<VkImageView> hybridViews;
    taaViews.reserve(m_syncManager->GetFramesInFlight());
    if (m_rtDenoiserPass.IsInitialized())
        hybridViews.reserve(m_syncManager->GetFramesInFlight());
    for (uint32_t f = 0; f < m_syncManager->GetFramesInFlight(); ++f)
    {
        taaViews.push_back(m_taaPass.GetHistoryView(f));
        if (m_rtDenoiserPass.IsInitialized())
            hybridViews.push_back(m_rtDenoiserPass.GetCompositeView(f));
    }
    if (!m_bloomPass.OnResize(m_context->GetSwapchainExtent(), m_hdrColor.GetView(), false, taaViews, hybridViews))
        return false;

    if (m_context->GetSwapchainImageFormat() != m_pipelineColorFormat ||
        m_depthAttachment.GetFormat() != oldDepthFormat)
    {
        m_graphicsPipeline.Shutdown();
        m_pbrOpaque.Shutdown();
        m_pbrBlend.Shutdown();
        m_skyboxPipeline.Shutdown();
        m_tonePipeline.Shutdown();
        if (!CreatePipeline(*m_context))
            return false;
    }

    m_currentFrame = 0;
    m_rtScene.OnDeviceIdle();
    if (m_rayQueryPass.IsInitialized() && !m_rayQueryPass.OnResize(m_context->GetSwapchainExtent()))
        return false;
    m_imageLayouts.assign(
        m_context->GetSwapchainImages().size(),
        VK_IMAGE_LAYOUT_UNDEFINED
    );
    return true;
}

bool Aether::VulkanRenderer::SetShadowResolution(uint32_t resolution)
{
    if (!m_context)
        return false;
    m_context->WaitIdle();
    return m_shadowPass.SetResolution(resolution);
}

Aether::VulkanRenderer::FrameResult Aether::VulkanRenderer::DrawFrame(
    std::span<const DrawItem> items, VkDescriptorSet environment,
    const glm::mat4& clipToWorldDirection, float exposure,
    const ShadowLightSpace& lightSpace, const ShadowConfig& shadowConfig,
    const BloomConfig& bloomConfig, const TaaConfig& taaConfig,
    const glm::mat4& currClipToPrevClipRot, const glm::vec2& jitterDeltaUv,
    bool updateRayTracingScene, RenderMode renderMode,
    const glm::vec3& cameraPosition, const RayQueryConfig& rayQueryConfig,
    const glm::mat4& unjitteredClipToWorldDirection,
    const HybridRtConfig& hybridConfig,
    std::span<const RtGpuLight> sceneLights,
    const glm::mat4& prevUnjitteredViewProj,
    float lightAngle)
{
    static_assert(sizeof(glm::mat4) == MVP_SIZE);
    if (!m_context || !environment)
        return FrameResult::FatalError;

    for (const auto& item : items)
    {
        if (!item.mesh || !item.mesh->GetVertexBuffer().GetBuffer() ||
            !item.mesh->GetIndexBuffer().GetBuffer() || !item.mesh->GetIndexCount())
        {
            std::cerr << "Invalid mesh in DrawFrame!\n";
            return FrameResult::FatalError;
        }
    }

    VkDevice device = m_context->GetDevice();

    VkFence currentFence =
        m_syncManager->GetInFlightFence(m_currentFrame);

    VkResult result = vkWaitForFences(
        device,
        1,
        &currentFence,
        VK_TRUE,
        UINT64_MAX
    );

    if (result != VK_SUCCESS)
    {
        std::cerr << "Failed to wait for frame fence!\n";
        return FrameResult::FatalError;
    }

    if (m_rtScene.IsInitialized())
        m_rtScene.OnFrameFenceCompleted(m_currentFrame);
    if (m_rayQueryPass.IsInitialized())
        m_rayQueryPass.OnFrameFenceCompleted(m_currentFrame);
    if (m_hybridGBufferPass.IsInitialized())
        m_hybridGBufferPass.OnFrameFenceCompleted(m_currentFrame);
    if (m_rtShadowPass.IsInitialized())
        m_rtShadowPass.OnFrameFenceCompleted(m_currentFrame);
    if (m_rtReflectionPass.IsInitialized())
        m_rtReflectionPass.OnFrameFenceCompleted(m_currentFrame);
    if (m_rtDenoiserPass.IsInitialized())
        m_rtDenoiserPass.OnFrameFenceCompleted(m_currentFrame);
    if (m_frameTimestampPool != VK_NULL_HANDLE &&
        m_currentFrame < m_frameHasPendingTimestamp.size() &&
        m_frameHasPendingTimestamp[m_currentFrame])
    {
        uint64_t stamps[2]{};
        if (vkGetQueryPoolResults(
                device, m_frameTimestampPool, m_currentFrame * 2, 2,
                sizeof(stamps), stamps, sizeof(uint64_t), VK_QUERY_RESULT_64_BIT) == VK_SUCCESS &&
            stamps[1] >= stamps[0])
        {
            m_lastFrameGpuTimeMs = static_cast<double>(stamps[1] - stamps[0]) * m_frameTimestampPeriodNs * 1e-6;
        }
        m_frameHasPendingTimestamp[m_currentFrame] = false;
    }

    uint32_t imageIndex = 0;

    VkSemaphore imageAvailable =
        m_syncManager->GetImageAvailableSemaphore(m_currentFrame);

    result = vkAcquireNextImageKHR(
        device,
        m_context->GetSwapchain(),
        UINT64_MAX,
        imageAvailable,
        VK_NULL_HANDLE,
        &imageIndex
    );

    if (result == VK_ERROR_OUT_OF_DATE_KHR)
        return FrameResult::RecreateSwapchain;

    const bool acquireSuboptimal = result == VK_SUBOPTIMAL_KHR;
    if (result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR)
    {
        std::cerr << "Failed to acquire swapchain image! "
            << result << '\n';
        return FrameResult::FatalError;
    }

    if (imageIndex >= m_imageLayouts.size() ||
        imageIndex >= m_context->GetSwapchainImages().size() ||
        imageIndex >= m_context->GetSwapchainImageViews().size() ||
        m_syncManager->GetRenderFinishedSemaphore(imageIndex) == VK_NULL_HANDLE)
        return FrameResult::FatalError;

    VkCommandBuffer commandBuffer =
        m_commandManager->GetCommandBuffer(m_currentFrame);

    result = vkResetCommandBuffer(commandBuffer, 0);

    if (result != VK_SUCCESS)
    {
        std::cerr << "Failed to reset command buffer!\n";
        return FrameResult::FatalError;
    }

    VkCommandBufferBeginInfo beginInfo{};
    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;

    result = vkBeginCommandBuffer(commandBuffer, &beginInfo);

    if (result != VK_SUCCESS)
    {
        std::cerr << "Failed to begin command buffer!\n";
        return FrameResult::FatalError;
    }

    const bool runRayQuery =
        ((renderMode == RenderMode::RayQuery) ||
         (renderMode == RenderMode::HybridRayTracing && hybridConfig.debugView == HybridDebugView::RayQueryDiagnostic)) &&
        m_rtScene.IsInitialized() &&
        m_rayQueryPass.IsInitialized();
    const bool runHybridRt =
        renderMode == RenderMode::HybridRayTracing &&
        hybridConfig.debugView != HybridDebugView::RayQueryDiagnostic &&
        CanUseHybridRtMode();
    const bool recordedRtScene = (updateRayTracingScene || runRayQuery || runHybridRt) && m_rtScene.IsInitialized();

    m_itemRtInstanceIndices.assign(items.size(), UINT32_MAX);
    if (recordedRtScene)
    {
        const auto& caps = m_context->GetRayTracingCapabilities();
        m_rtItems.clear();
        m_rtItems.reserve(items.size());
        for (size_t i = 0; i < items.size(); ++i)
        {
            const auto& item = items[i];
            if (!item.alphaBlend && item.mesh && IsFiniteMatrix(item.model) &&
                item.mesh->GetUniqueId() != 0 && item.mesh->IsRayTracingUsageEnabled() &&
                item.mesh->GetVertexCount() > 0 && item.mesh->GetIndexCount() >= 3 &&
                (item.mesh->GetIndexCount() % 3) == 0 &&
                (item.mesh->GetIndexCount() / 3) <= caps.maxPrimitiveCount &&
                item.mesh->GetVertexBuffer().GetDeviceAddress() != 0 &&
                item.mesh->GetIndexBuffer().GetDeviceAddress() != 0 &&
                IsValidRtInstanceCustomIndex(static_cast<uint32_t>(m_rtItems.size())) &&
                m_rtItems.size() < caps.maxInstanceCount)
            {
                m_itemRtInstanceIndices[i] = static_cast<uint32_t>(m_rtItems.size());
            }

            m_rtItems.push_back({
                item.mesh,
                item.model,
                item.sceneObjectIndex,
                item.mirrored,
                item.doubleSided,
                item.alphaMask,
                item.alphaBlend,
                item.prevModel,
                item.hasPrevModel,
                item.rtMaterial,
                item.alphaMaskView,
                item.alphaMaskSampler
            });
        }
        if (!m_rtScene.RecordFrame(commandBuffer, m_currentFrame, m_rtItems))
        {
            std::cerr << "Failed to record Ray Tracing acceleration structure updates!\n";
            vkEndCommandBuffer(commandBuffer);
            return FrameResult::FatalError;
        }
    }

    if (m_frameTimestampPool != VK_NULL_HANDLE)
    {
        vkCmdResetQueryPool(commandBuffer, m_frameTimestampPool, m_currentFrame * 2, 2);
        vkCmdWriteTimestamp2(commandBuffer, VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, m_frameTimestampPool, m_currentFrame * 2);
    }

    VkImage image =
        m_context->GetSwapchainImages()[imageIndex];
    const VkExtent2D swapExtent = m_context->GetSwapchainExtent();
    const VkExtent2D renderExtent = m_hdrColor.GetExtent();
    const VkExtent2D extent = swapExtent;

    VkViewport viewport{};
    viewport.x = 0.0f;
    viewport.y = 0.0f;
    viewport.width = static_cast<float>(swapExtent.width);
    viewport.height = static_cast<float>(swapExtent.height);
    viewport.minDepth = 0.0f;
    viewport.maxDepth = 1.0f;

    VkRect2D scissor{};
    scissor.offset = { 0, 0 };
    scissor.extent = swapExtent;

    const VkViewport hdrViewport{
        0.0f, 0.0f,
        static_cast<float>(renderExtent.width), static_cast<float>(renderExtent.height),
        0.0f, 1.0f
    };
    const VkRect2D hdrScissor{ { 0, 0 }, renderExtent };

    VkImageLayout oldLayout =
        m_imageLayouts[imageIndex];

    VkImageMemoryBarrier2 imageBarrier{};
    imageBarrier.sType =
        VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
    imageBarrier.srcStageMask =
        VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
    imageBarrier.srcAccessMask = 0;
    imageBarrier.dstStageMask =
        VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
    imageBarrier.dstAccessMask =
        VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
    imageBarrier.oldLayout = oldLayout;
    imageBarrier.newLayout =
        VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    imageBarrier.srcQueueFamilyIndex =
        VK_QUEUE_FAMILY_IGNORED;
    imageBarrier.dstQueueFamilyIndex =
        VK_QUEUE_FAMILY_IGNORED;
    imageBarrier.image = image;
    imageBarrier.subresourceRange.aspectMask =
        VK_IMAGE_ASPECT_COLOR_BIT;
    imageBarrier.subresourceRange.baseMipLevel = 0;
    imageBarrier.subresourceRange.levelCount = 1;
    imageBarrier.subresourceRange.baseArrayLayer = 0;
    imageBarrier.subresourceRange.layerCount = 1;

    VkDescriptorSet toneMapInput = m_hdrSet;
    bool runTaa = false;
    bool depthTransitioned = false;
    VkImageLayout nextDepthLayout = m_depthLayout;

    if (runRayQuery)
    {
        const TlasBuildAction tlasAction = m_rtScene.GetLastRecordedTlasAction(m_currentFrame);
        const bool tlasWrittenThisFrame =
            tlasAction == TlasBuildAction::InitialBuild ||
            tlasAction == TlasBuildAction::Rebuild ||
            tlasAction == TlasBuildAction::Update;

        if (!m_rayQueryPass.Execute(
                commandBuffer,
                m_currentFrame,
                m_rtScene.GetTlasHandle(m_currentFrame),
                tlasWrittenThisFrame,
                environment,
                unjitteredClipToWorldDirection,
                cameraPosition,
                rayQueryConfig,
                m_rayQueryReadbackTarget))
        {
            std::cerr << "Failed to record Ray Query compute pass!\n";
            vkEndCommandBuffer(commandBuffer);
            return FrameResult::FatalError;
        }

        VkDependencyInfo swapchainDep{ VK_STRUCTURE_TYPE_DEPENDENCY_INFO };
        swapchainDep.imageMemoryBarrierCount = 1;
        swapchainDep.pImageMemoryBarriers = &imageBarrier;
        vkCmdPipelineBarrier2(commandBuffer, &swapchainDep);

        vkCmdSetViewport(commandBuffer, 0, 1, &viewport);
        vkCmdSetScissor(commandBuffer, 0, 1, &scissor);

        // Post-processing policy in Ray Query diagnostic mode:
        // Bypass TAA and Bloom to preserve crisp, unblurred diagnostic visualization,
        // and apply Exposure + ACES Filmic Tone Mapping consistently for swapchain output.
        toneMapInput = m_rayQueryPass.GetOutputToneMapSet(m_currentFrame);
    }
    else if (runHybridRt)
    {
        constexpr VkPipelineStageFlags2 depthStages =
            VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT;
        VkImageMemoryBarrier2 depthBarrier{ VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2 };
        depthBarrier.srcStageMask = m_depthLayout == VK_IMAGE_LAYOUT_UNDEFINED ? VK_PIPELINE_STAGE_2_NONE : depthStages;
        depthBarrier.srcAccessMask = m_depthLayout == VK_IMAGE_LAYOUT_UNDEFINED ? 0 : VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
        depthBarrier.dstStageMask = depthStages;
        depthBarrier.dstAccessMask = VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
        depthBarrier.oldLayout = m_depthLayout;
        depthBarrier.newLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        depthBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        depthBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        depthBarrier.image = m_depthAttachment.GetImage();
        depthBarrier.subresourceRange = { VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1 };

        const std::array<VkImageMemoryBarrier2, 2> hybridPreBarriers = { imageBarrier, depthBarrier };
        VkDependencyInfo hybridPreDep{ VK_STRUCTURE_TYPE_DEPENDENCY_INFO };
        hybridPreDep.imageMemoryBarrierCount = static_cast<uint32_t>(hybridPreBarriers.size());
        hybridPreDep.pImageMemoryBarriers = hybridPreBarriers.data();
        vkCmdPipelineBarrier2(commandBuffer, &hybridPreDep);

        depthTransitioned = true;
        nextDepthLayout = depthBarrier.newLayout;

        m_gbufferItems.clear();
        m_gbufferItems.reserve(items.size());
        for (size_t i = 0; i < items.size(); ++i)
        {
            const auto& item = items[i];
            m_gbufferItems.push_back({
                item.mesh,
                item.pbr.mvp,
                { item.pbr.modelRows[0], item.pbr.modelRows[1], item.pbr.modelRows[2] },
                cameraPosition,
                m_itemRtInstanceIndices[i],
                item.mirrored,
                item.doubleSided,
                item.alphaBlend,
                item.material
            });
        }

        if (!m_hybridGBufferPass.Execute(
                commandBuffer, m_currentFrame, m_depthAttachment.GetView(), nextDepthLayout, m_gbufferItems))
        {
            std::cerr << "Failed to record Hybrid G-Buffer pass!\n";
            vkEndCommandBuffer(commandBuffer);
            return FrameResult::FatalError;
        }

        const TlasBuildAction tlasAction = m_rtScene.GetLastRecordedTlasAction(m_currentFrame);
        const bool tlasWrittenThisFrame =
            tlasAction == TlasBuildAction::InitialBuild ||
            tlasAction == TlasBuildAction::Rebuild ||
            tlasAction == TlasBuildAction::Update;

        const glm::vec3 sunDir = glm::normalize(glm::vec3(std::cos(lightAngle), 0.8f, std::sin(lightAngle)));
        const glm::vec3 sunRadiance(4.8f, 4.3f, 3.9f);

        if (!m_rtShadowPass.Execute(
                commandBuffer, m_currentFrame, m_rtScene, tlasWrittenThisFrame, m_hybridGBufferPass,
                cameraPosition, sunDir, sunRadiance, true, true, hybridConfig,
                m_hybridFrameCounter, sceneLights, m_rtShadowReadbackTarget) ||
            !m_rtReflectionPass.Execute(
                commandBuffer, m_currentFrame, m_rtScene, tlasWrittenThisFrame, m_hybridGBufferPass,
                environment, cameraPosition, sunDir, sunRadiance, true, true, hybridConfig,
                m_hybridFrameCounter, sceneLights, m_rtReflectionReadbackTarget, m_rtReflectionAuxReadbackTarget) ||
            !m_rtDenoiserPass.Execute(
                commandBuffer, m_currentFrame, m_rtScene, m_hybridGBufferPass, m_rtShadowPass,
                m_rtReflectionPass, environment, unjitteredClipToWorldDirection,
                prevUnjitteredViewProj, cameraPosition, hybridConfig,
                m_rtVarConfReadbackTarget, m_rtCompositeReadbackTarget))
        {
            std::cerr << "Failed to record Hybrid Ray Tracing passes!\n";
            vkEndCommandBuffer(commandBuffer);
            return FrameResult::FatalError;
        }

        const HybridRtConfig cleanHybrid = SanitizeHybridRtConfig(hybridConfig);
        const bool allowHybridBloom = bloomConfig.enabled &&
            (cleanHybrid.debugView == HybridDebugView::FinalHybrid ||
             cleanHybrid.debugView == HybridDebugView::RtShadowsOnly ||
             cleanHybrid.debugView == HybridDebugView::RtReflectionsOnly);

        if (allowHybridBloom)
        {
            m_bloomPass.Execute(commandBuffer, m_currentFrame, bloomConfig, BloomInputSource::HybridComposite);
            toneMapInput = m_bloomPass.GetOutputDescriptorSet(m_currentFrame);
        }
        else
        {
            toneMapInput = m_rtDenoiserPass.GetOutputToneMapSet(m_currentFrame);
        }

        vkCmdSetViewport(commandBuffer, 0, 1, &viewport);
        vkCmdSetScissor(commandBuffer, 0, 1, &scissor);
    }
    else
    {
        m_shadowCasters.clear();
        m_shadowCasters.reserve(items.size());
        for (const auto& item : items)
        {
            m_shadowCasters.push_back({
                item.mesh,
                item.model,
                item.mirrored,
                item.doubleSided,
                item.alphaMask,
                item.alphaBlend,
                item.material
            });
        }
        m_shadowPass.Execute(commandBuffer, m_currentFrame, m_shadowCasters, lightSpace, shadowConfig);

        VkDependencyInfo dependencyInfo{};
        dependencyInfo.sType =
            VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
        // Both depth tests can access the shared attachment on the graphics queue.
        constexpr VkPipelineStageFlags2 depthStages =
            VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT;
        VkImageMemoryBarrier2 depthBarrier{};
        depthBarrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
        depthBarrier.srcStageMask = m_depthLayout == VK_IMAGE_LAYOUT_UNDEFINED ? VK_PIPELINE_STAGE_2_NONE : depthStages;
        depthBarrier.srcAccessMask = m_depthLayout == VK_IMAGE_LAYOUT_UNDEFINED ? 0 : VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
        depthBarrier.dstStageMask = depthStages;
        depthBarrier.dstAccessMask =
            VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
        depthBarrier.oldLayout = m_depthLayout;
        depthBarrier.newLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        depthBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        depthBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        depthBarrier.image = m_depthAttachment.GetImage();
        depthBarrier.subresourceRange = { VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1 };

        auto hdrBarrier = imageBarrier;
        hdrBarrier.image = m_hdrColor.GetImage();
        hdrBarrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        hdrBarrier.srcStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
        hdrBarrier.srcAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
        const VkImageMemoryBarrier2 barriers[] = { imageBarrier, depthBarrier, hdrBarrier };
        dependencyInfo.imageMemoryBarrierCount = 3;
        dependencyInfo.pImageMemoryBarriers = barriers;

        vkCmdPipelineBarrier2(commandBuffer, &dependencyInfo);

        depthTransitioned = true;
        nextDepthLayout = depthBarrier.newLayout;

        VkRenderingAttachmentInfo colorAttachment{};
        colorAttachment.sType =
            VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
        colorAttachment.imageView = m_hdrColor.GetView();
        colorAttachment.imageLayout =
            VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        colorAttachment.loadOp =
            VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        colorAttachment.storeOp =
            VK_ATTACHMENT_STORE_OP_STORE;

        VkRenderingAttachmentInfo depthAttachment{};
        depthAttachment.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
        depthAttachment.imageView = m_depthAttachment.GetView();
        depthAttachment.imageLayout = depthBarrier.newLayout;
        depthAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        depthAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        depthAttachment.clearValue.depthStencil = { 1.0f, 0 };

        VkRenderingInfo renderingInfo{};
        renderingInfo.sType =
            VK_STRUCTURE_TYPE_RENDERING_INFO;
        renderingInfo.renderArea.offset = { 0, 0 };
        renderingInfo.renderArea.extent = renderExtent;
        renderingInfo.layerCount = 1;
        renderingInfo.colorAttachmentCount = 1;
        renderingInfo.pColorAttachments = &colorAttachment;
        renderingInfo.pDepthAttachment = &depthAttachment;

        vkCmdBeginRendering(commandBuffer, &renderingInfo);

        vkCmdSetViewport(commandBuffer, 0, 1, &hdrViewport);
        vkCmdSetScissor(commandBuffer, 0, 1, &hdrScissor);

        if (shadowConfig.debugView && shadowConfig.enabled)
        {
            m_shadowPass.DrawDebugOverlay(commandBuffer, m_currentFrame);
        }
        else
        {
            DrawSkybox(commandBuffer, environment, clipToWorldDirection);

            VkPipeline boundPipeline = VK_NULL_HANDLE;
            const VulkanMesh* boundMesh = nullptr;
            const VkDescriptorSet shadowSet = m_shadowPass.GetDescriptorSet(m_currentFrame);
            for (const auto& item : items)
            {
                const auto& pipeline = item.material ? (item.alphaBlend ? m_pbrBlend : m_pbrOpaque) : m_graphicsPipeline;
                if (pipeline.GetPipeline() != boundPipeline)
                {
                    boundPipeline = pipeline.GetPipeline();
                    vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, boundPipeline);
                }
                if (item.mesh != boundMesh)
                {
                    boundMesh = item.mesh;
                    const VkBuffer vertexBuffer = boundMesh->GetVertexBuffer().GetBuffer();
                    const VkDeviceSize vertexOffset = 0;
                    vkCmdBindVertexBuffers(commandBuffer, 0, 1, &vertexBuffer, &vertexOffset);
                    vkCmdBindIndexBuffer(commandBuffer, boundMesh->GetIndexBuffer().GetBuffer(), 0, VK_INDEX_TYPE_UINT32);
                }
                vkCmdSetFrontFace(commandBuffer, item.mirrored ? VK_FRONT_FACE_CLOCKWISE : VK_FRONT_FACE_COUNTER_CLOCKWISE);
                vkCmdSetCullMode(commandBuffer, item.doubleSided ? VK_CULL_MODE_NONE : VK_CULL_MODE_BACK_BIT);
                const auto layout = pipeline.GetPipelineLayout();
                if (item.material)
                {
                    const VkDescriptorSet sets[] = { item.material, environment, shadowSet };
                    vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 0, 3, sets, 0, nullptr);
                    vkCmdPushConstants(commandBuffer, layout, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(item.pbr), &item.pbr);
                }
                else
                {
                    vkCmdPushConstants(commandBuffer, layout, VK_SHADER_STAGE_VERTEX_BIT, 0, MVP_SIZE, &item.mvp);
                }
                vkCmdDrawIndexed(commandBuffer, boundMesh->GetIndexCount(), 1, 0, 0, 0);
            }
        }

        vkCmdEndRendering(commandBuffer);

        // Transition HDR scene target once for Bloom sampling and/or ToneMap sampling.
        {
            VkImageMemoryBarrier2 hdrReadBarrier{ VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2 };
            hdrReadBarrier.srcStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
            hdrReadBarrier.srcAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
            hdrReadBarrier.dstStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
            hdrReadBarrier.dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
            hdrReadBarrier.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            hdrReadBarrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            hdrReadBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            hdrReadBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            hdrReadBarrier.image = m_hdrColor.GetImage();
            hdrReadBarrier.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
            VkDependencyInfo hdrReadDep{ VK_STRUCTURE_TYPE_DEPENDENCY_INFO };
            hdrReadDep.imageMemoryBarrierCount = 1;
            hdrReadDep.pImageMemoryBarriers = &hdrReadBarrier;
            vkCmdPipelineBarrier2(commandBuffer, &hdrReadDep);
        }

        const bool shadowDebugActive = shadowConfig.debugView && shadowConfig.enabled;
        runTaa = taaConfig.enabled && !shadowDebugActive;
        if (runTaa)
        {
            m_velocityItems.clear();
            m_velocityItems.reserve(items.size());
            for (const auto& item : items)
            {
                m_velocityItems.push_back({
                    item.mesh,
                    item.mvp,
                    item.prevMvpJittered,
                    item.mirrored,
                    item.doubleSided,
                    item.alphaMask,
                    item.alphaBlend,
                    item.material
                });
            }
            if (!m_taaPass.Execute(
                commandBuffer, m_currentFrame, m_depthAttachment.GetImage(),
                m_velocityItems, currClipToPrevClipRot, jitterDeltaUv, taaConfig))
            {
                std::cerr << "Failed to record TAA pass\n";
                return FrameResult::FatalError;
            }
            toneMapInput = m_taaPass.GetOutputToneMapSet(m_currentFrame);
            vkCmdSetViewport(commandBuffer, 0, 1, &viewport);
            vkCmdSetScissor(commandBuffer, 0, 1, &scissor);
        }

        if (bloomConfig.enabled && !shadowDebugActive)
        {
            m_bloomPass.Execute(commandBuffer, m_currentFrame, bloomConfig, runTaa);
            toneMapInput = m_bloomPass.GetOutputDescriptorSet(m_currentFrame);
        }
        vkCmdSetViewport(commandBuffer, 0, 1, &viewport);
        vkCmdSetScissor(commandBuffer, 0, 1, &scissor);
    }

    ToneMap(commandBuffer, m_context->GetSwapchainImageViews()[imageIndex], extent, exposure, toneMapInput);

    if (m_frameTimestampPool != VK_NULL_HANDLE)
    {
        vkCmdWriteTimestamp2(commandBuffer, VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT,
                             m_frameTimestampPool, m_currentFrame * 2 + 1);
    }

    VkImageMemoryBarrier2 presentBarrier{};
    presentBarrier.sType =
        VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;

    presentBarrier.srcStageMask =
        VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
    presentBarrier.srcAccessMask =
        VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;

    presentBarrier.dstStageMask =
        VK_PIPELINE_STAGE_2_NONE;
    presentBarrier.dstAccessMask = 0;

    presentBarrier.oldLayout =
        VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    presentBarrier.newLayout =
        VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;

    presentBarrier.srcQueueFamilyIndex =
        VK_QUEUE_FAMILY_IGNORED;
    presentBarrier.dstQueueFamilyIndex =
        VK_QUEUE_FAMILY_IGNORED;

    presentBarrier.image = image;

    presentBarrier.subresourceRange.aspectMask =
        VK_IMAGE_ASPECT_COLOR_BIT;
    presentBarrier.subresourceRange.baseMipLevel = 0;
    presentBarrier.subresourceRange.levelCount = 1;
    presentBarrier.subresourceRange.baseArrayLayer = 0;
    presentBarrier.subresourceRange.layerCount = 1;

    VkDependencyInfo presentDependencyInfo{};
    presentDependencyInfo.sType =
        VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
    presentDependencyInfo.imageMemoryBarrierCount = 1;
    presentDependencyInfo.pImageMemoryBarriers = &presentBarrier;

    if (m_captureTarget)
    {
        presentBarrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        presentBarrier.dstStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
        presentBarrier.dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT;
        vkCmdPipelineBarrier2(commandBuffer, &presentDependencyInfo);

        VkBufferImageCopy region{};
        region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        region.imageExtent = {extent.width, extent.height, 1};
        vkCmdCopyImageToBuffer(commandBuffer, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            m_captureTarget, 1, &region);
        VkBufferMemoryBarrier2 readback{};
        readback.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2;
        readback.srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
        readback.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
        readback.dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT;
        readback.dstAccessMask = VK_ACCESS_2_HOST_READ_BIT;
        readback.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        readback.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        readback.buffer = m_captureTarget;
        readback.size = VK_WHOLE_SIZE;
        VkDependencyInfo host{};
        host.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
        host.bufferMemoryBarrierCount = 1;
        host.pBufferMemoryBarriers = &readback;
        vkCmdPipelineBarrier2(commandBuffer, &host);

        presentBarrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        presentBarrier.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
        presentBarrier.srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
        presentBarrier.srcAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT;
        presentBarrier.dstStageMask = VK_PIPELINE_STAGE_2_NONE;
        presentBarrier.dstAccessMask = 0;
    }

    vkCmdPipelineBarrier2(
        commandBuffer,
        &presentDependencyInfo
    );

    result = vkEndCommandBuffer(commandBuffer);

    if (result != VK_SUCCESS)
    {
        std::cerr << "Failed to end command buffer! "
            << result << '\n';
        return FrameResult::FatalError;
    }

    VkSemaphore renderFinished =
        m_syncManager->GetRenderFinishedSemaphore(imageIndex);

    VkSemaphoreSubmitInfo waitInfo{};
    waitInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO;
    waitInfo.semaphore = imageAvailable;
    waitInfo.stageMask =
        VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
    waitInfo.deviceIndex = 0;

    VkCommandBufferSubmitInfo commandBufferInfo{};
    commandBufferInfo.sType =
        VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO;
    commandBufferInfo.commandBuffer = commandBuffer;
    commandBufferInfo.deviceMask = 0;

    VkSemaphoreSubmitInfo signalInfo{};
    signalInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO;
    signalInfo.semaphore = renderFinished;
    signalInfo.stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    signalInfo.deviceIndex = 0;

    VkSubmitInfo2 submitInfo{};
    submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2;

    submitInfo.waitSemaphoreInfoCount = 1;
    submitInfo.pWaitSemaphoreInfos = &waitInfo;

    submitInfo.commandBufferInfoCount = 1;
    submitInfo.pCommandBufferInfos = &commandBufferInfo;

    submitInfo.signalSemaphoreInfoCount = 1;
    submitInfo.pSignalSemaphoreInfos = &signalInfo;

    result = vkResetFences(
        device,
        1,
        &currentFence
    );

    if (result != VK_SUCCESS)
    {
        std::cerr << "Failed to reset fence!\n";
        return FrameResult::FatalError;
    }

    result = vkQueueSubmit2(
        m_context->GetGraphicsQueue(),
        1,
        &submitInfo,
        currentFence
    );

    if (result != VK_SUCCESS)
    {
        std::cerr << "Failed to submit command buffer! "
            << result << '\n';
        return FrameResult::FatalError;
    }

    m_imageLayouts[imageIndex] =
        VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    if (depthTransitioned)
        m_depthLayout = nextDepthLayout;
    if (runTaa)
        m_taaPass.OnFrameSubmitted(m_currentFrame);
    else
        m_taaPass.ResetHistory();
    if (recordedRtScene)
        m_rtScene.OnFrameSubmitted(m_currentFrame);
    if (runRayQuery)
        m_rayQueryPass.OnFrameSubmitted(m_currentFrame);
    if (runHybridRt)
    {
        ++m_hybridFrameCounter;
        m_hybridGBufferPass.OnFrameSubmitted(m_currentFrame);
        m_rtShadowPass.OnFrameSubmitted(m_currentFrame);
        m_rtReflectionPass.OnFrameSubmitted(m_currentFrame);
        m_rtDenoiserPass.OnFrameSubmitted(m_currentFrame);
    }
    else
    {
        m_rtDenoiserPass.ResetHistory();
    }
    if (m_frameTimestampPool != VK_NULL_HANDLE && m_currentFrame < m_frameHasPendingTimestamp.size())
        m_frameHasPendingTimestamp[m_currentFrame] = true;

    VkPresentInfoKHR presentInfo{};
    presentInfo.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;

    presentInfo.waitSemaphoreCount = 1;
    presentInfo.pWaitSemaphores = &renderFinished;

    VkSwapchainKHR swapchain = m_context->GetSwapchain();

    presentInfo.swapchainCount = 1;
    presentInfo.pSwapchains = &swapchain;
    presentInfo.pImageIndices = &imageIndex;

    result = vkQueuePresentKHR(
        m_context->GetPresentQueue(),
        &presentInfo
    );

    if (result == VK_ERROR_OUT_OF_DATE_KHR)
        return FrameResult::RecreateSwapchain;

    if (result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR)
    {
        std::cerr << "Failed to present swapchain image! "
            << result << '\n';
        return FrameResult::FatalError;
    }

    m_currentFrame =
        (m_currentFrame + 1) % m_syncManager->GetFramesInFlight();

    if (acquireSuboptimal || result == VK_SUBOPTIMAL_KHR)
        return FrameResult::RecreateSwapchain;

    return FrameResult::Rendered;
}

void Aether::VulkanRenderer::Shutdown()
{
    if (m_context == nullptr)
        return;

    m_context->WaitIdle();
    if (m_frameTimestampPool != VK_NULL_HANDLE)
    {
        vkDestroyQueryPool(m_context->GetDevice(), m_frameTimestampPool, nullptr);
        m_frameTimestampPool = VK_NULL_HANDLE;
    }
    m_frameHasPendingTimestamp.clear();
    m_frameTimestampPeriodNs = 0.0;
    m_lastFrameGpuTimeMs = 0.0;
    m_hybridFrameCounter = 0;

    m_rtDenoiserPass.Shutdown();
    m_rtReflectionPass.Shutdown();
    m_rtShadowPass.Shutdown();
    m_hybridGBufferPass.Shutdown();
    m_rayQueryPass.Shutdown();
    m_rtScene.Shutdown();
    m_shadowPass.Shutdown();
    m_bloomPass.Shutdown();
    m_taaPass.Shutdown();
    ReleaseHdr(m_context->GetDevice());

    m_graphicsPipeline.Shutdown();
    m_pbrOpaque.Shutdown();
    m_pbrBlend.Shutdown();
    m_depthAttachment.Shutdown();
    m_depthLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    m_memoryManager = nullptr;

    m_imageLayouts.clear();

    m_context = nullptr;
    m_commandManager = nullptr;
    m_syncManager = nullptr;

    m_currentFrame = 0;

    std::cout << "Vulkan Renderer Shutdown!\n";
}
