#include <Platform/Window/Window.h>
#include <RHI/Vulkan/Context/VulkanContext.h>
#include <RHI/Vulkan/Command/VulkanCommandManager.h>
#include <RHI/Vulkan/Sync/VulkanSyncManager.h>
#include <RHI/Vulkan/Renderer/VulkanRenderer.h>
#include <iostream>
int main()
{
    Aether::Window window;
    Aether::VulkanContext vulkanContext;
    Aether::VulkanCommandManager commandManager;
    Aether::VulkanSyncManager syncManager;
    Aether::VulkanRenderer renderer;

    if (!window.Initialize(1280, 720, "Aether"))
        return -1;

    if (!vulkanContext.Initialize(window))
        return -1;

    if (!vulkanContext.CreateSurface(window))
        return -1;

    if (!vulkanContext.SelectPhysicalDevice())
        return -1;

    if (!vulkanContext.CreateLogicalDevice())
        return -1;

    if (!vulkanContext.CreateSwapchain(window))
        return -1;

    if (!commandManager.initialize(
        vulkanContext.GetDevice(),
        vulkanContext.GetGraphicsQueueFamily(),
        static_cast<uint32_t>(
            vulkanContext.GetSwapchainImages().size())))
        return -1;

    if (!syncManager.Initialize(
        vulkanContext.GetDevice(),
        2,
        static_cast<uint32_t>(
            vulkanContext.GetSwapchainImages().size())))
        return -1;

    if (!renderer.Initialize(
        vulkanContext,
        commandManager,
        syncManager))
        return -1;

    while (window.IsOpen())
    {
        window.PollEvents();
        if (!renderer.DrawFrame())
            break;
    }

    return 0;
}