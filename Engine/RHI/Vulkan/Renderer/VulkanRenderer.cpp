#include "VulkanRenderer.h"

#include <iostream>
#include <string>
#include <RHI/Vulkan/Context/VulkanContext.h>
#include <RHI/Vulkan/Command/VulkanCommandManager.h>
#include <RHI/Vulkan/Sync/VulkanSyncManager.h>
#include <RHI/Vulkan/Pipeline/VulkanGraphicsPipeline.h>
#include <RHI/Vulkan/Shader/VulkanShader.h>
Aether::VulkanRenderer::~VulkanRenderer()
{
	Shutdown();
}

bool Aether::VulkanRenderer::Initialize(
    VulkanContext& context,
    VulkanCommandManager& commandManager,
    VulkanSyncManager& syncManager
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



    if (!m_graphicsPipeline.Initialize(
        context.GetDevice(),
        context.GetSwapchainImageFormat(),
        vertexShader.GetShaderModule(),
        fragmentShader.GetShaderModule()))
    {
        std::cerr << "Failed to create Graphics Pipeline!\n";
        return false;
    }

    m_context = &context;
    m_commandManager = &commandManager;
    m_syncManager = &syncManager;


    m_imagesInFlight.resize(
        context.GetSwapchainImages().size(),
        VK_NULL_HANDLE
    );

    m_imageLayouts.resize(
        context.GetSwapchainImages().size(),
        VK_IMAGE_LAYOUT_UNDEFINED
    );

    m_currentFrame = 0;

    std::cout << "Vulkan Renderer Initialized Successfully!\n";
    std::cout << "   Frames In Flight: "
        << MAX_FRAMES_IN_FLIGHT << '\n';
    std::cout << "   Swapchain Images: "
        << m_imagesInFlight.size() << '\n';

    return true;
}

bool Aether::VulkanRenderer::DrawFrame()
{
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
        return false;
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

    if (result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR)
    {
        std::cerr << "Failed to acquire swapchain image! "
            << result << '\n';
        return false;
    }

    if (m_imagesInFlight[imageIndex] != VK_NULL_HANDLE)
    {
        VkFence imageFence = m_imagesInFlight[imageIndex];

        result = vkWaitForFences(
            device,
            1,
            &imageFence,
            VK_TRUE,
            UINT64_MAX
        );

        if (result != VK_SUCCESS)
        {
            std::cerr << "Failed to wait for image fence!\n";
            return false;
        }
    }

    VkCommandBuffer commandBuffer =
        m_commandManager->GetCommandBuffer(imageIndex);

    result = vkResetCommandBuffer(commandBuffer, 0);

    if (result != VK_SUCCESS)
    {
        std::cerr << "Failed to reset command buffer!\n";
        return false;
    }

    VkCommandBufferBeginInfo beginInfo{};
    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;

    result = vkBeginCommandBuffer(commandBuffer, &beginInfo);

    if (result != VK_SUCCESS)
    {
        std::cerr << "Failed to begin command buffer!\n";
        return false;
    }

    VkImage image =
        m_context->GetSwapchainImages()[imageIndex];

    VkImageLayout oldLayout =
        m_imageLayouts[imageIndex];

    VkImageMemoryBarrier2 imageBarrier{};

    imageBarrier.sType =
        VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;

    imageBarrier.srcStageMask = VK_PIPELINE_STAGE_2_NONE;
    imageBarrier.srcAccessMask = 0;

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

    VkDependencyInfo dependencyInfo{};
    dependencyInfo.sType =
        VK_STRUCTURE_TYPE_DEPENDENCY_INFO;

    dependencyInfo.imageMemoryBarrierCount = 1;
    dependencyInfo.pImageMemoryBarriers = &imageBarrier;

    vkCmdPipelineBarrier2(commandBuffer, &dependencyInfo);


    VkRenderingAttachmentInfo colorAttachment{};
    colorAttachment.sType =
        VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;

    colorAttachment.imageView =
        m_context->GetSwapchainImageViews()[imageIndex];

    colorAttachment.imageLayout =
        VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

    colorAttachment.loadOp =
        VK_ATTACHMENT_LOAD_OP_CLEAR;

    colorAttachment.storeOp =
        VK_ATTACHMENT_STORE_OP_STORE;

    colorAttachment.clearValue.color = {
        { 0.02f, 0.04f, 0.10f, 1.0f }
    };

    VkRenderingInfo renderingInfo{};
    renderingInfo.sType =
        VK_STRUCTURE_TYPE_RENDERING_INFO;

    renderingInfo.renderArea.offset = { 0, 0 };
    renderingInfo.renderArea.extent =
        m_context->GetSwapchainExtent();

    renderingInfo.layerCount = 1;

    renderingInfo.colorAttachmentCount = 1;
    renderingInfo.pColorAttachments = &colorAttachment;

    vkCmdBeginRendering(commandBuffer, &renderingInfo);


    VkExtent2D extent = m_context->GetSwapchainExtent();

    VkViewport viewport{};
    viewport.x = 0.0f;
    viewport.y = 0.0f;
    viewport.width = static_cast<float>(extent.width);
    viewport.height = static_cast<float>(extent.height);
    viewport.minDepth = 0.0f;
    viewport.maxDepth = 1.0f;

    vkCmdSetViewport(
        commandBuffer,
        0,
        1,
        &viewport
    );


    VkRect2D scissor{};
    scissor.offset = { 0, 0 };
    scissor.extent = extent;

    vkCmdSetScissor(
        commandBuffer,
        0,
        1,
        &scissor
    );


    vkCmdBindPipeline(
        commandBuffer,
        VK_PIPELINE_BIND_POINT_GRAPHICS,
        m_graphicsPipeline.GetPipeline()
    );


    vkCmdDraw(
        commandBuffer,
        3,
        1,
        0,
        0 
    );

    vkCmdEndRendering(commandBuffer);


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

    vkCmdPipelineBarrier2(
        commandBuffer,
        &presentDependencyInfo
    );


    result = vkEndCommandBuffer(commandBuffer);

    if (result != VK_SUCCESS)
    {
        std::cerr << "Failed to submit command buffer! "
            << result << '\n';
        return false;
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
        return false;
    }


    result = vkQueueSubmit2(
        m_context->GetGraphicsQueue(),
        1,
        &submitInfo,
        currentFence
    );

    m_imagesInFlight[imageIndex] = currentFence;

    m_imageLayouts[imageIndex] =
        VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;


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
    {
        std::cerr << "Swapchain is out of date!\n";
        return false;
    }

    if (result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR)
    {
        std::cerr << "Failed to present swapchain image! "
            << result << '\n';
        return false;
    }

    if (result == VK_SUBOPTIMAL_KHR)
    {
        std::cout << "Swapchain is suboptimal.\n";
    }


    m_currentFrame =
        (m_currentFrame + 1) % MAX_FRAMES_IN_FLIGHT;

    if (result != VK_SUCCESS)
    {
        std::cerr << "Failed to end command buffer!\n";
        return false;
    }

    return true;
}

void Aether::VulkanRenderer::Shutdown()
{
    if (m_context == nullptr)
        return;

    vkDeviceWaitIdle(m_context->GetDevice());

    m_graphicsPipeline.Shutdown();

    m_imagesInFlight.clear();
    m_imageLayouts.clear();

    m_context = nullptr;
    m_commandManager = nullptr;
    m_syncManager = nullptr;

    m_currentFrame = 0;

    std::cout << "Vulkan Renderer Shutdown!\n";
}