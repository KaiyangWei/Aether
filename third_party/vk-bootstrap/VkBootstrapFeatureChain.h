#pragma once

#include <vector>
#include <string>

#define GLFW_INCLUDE_VULKAN
#include <GLFW/glfw3.h>
#define GLM_FORCE_DEPTH_ZERO_TO_ONE
#include <glm/glm.hpp>
#include <vk_mem_alloc.h>
#include <VkBootstrap.h>
#include <imgui.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_vulkan.h>

struct Buffer {
    VkBuffer buf;
    VmaAllocation alloc;
    VkDeviceAddress address;
};

struct Vertex {
    glm::vec4 pos;
    glm::vec4 normal;
    glm::vec4 color;
    glm::vec4 emission;
};

// 🌟 极其严苛的 16 字节对齐法则（防止显卡读取变量错位）
struct PushConstants {
    uint64_t vertexAddr;  // 8 字节
    uint64_t indexAddr;   // 8 字节
    uint32_t frameCount;  // 4 字节
    float lightIntensity; // 4 字节
    float pad[2];         // 💥 填补 8 字节，把接下来的 vec4 完美推到 32 字节对齐边界！
    glm::vec4 leftWallColor;
    glm::vec4 rightWallColor;
};

class RayTracingEngine {
    public:
    void run();

    private:
    void initWindow();
    void initVulkan();
    void initImGui();
    void loadRTFunctions();
    void createSwapchainAndImages();
    void buildAccelerationStructures();
    void createDescriptors();
    void createRayTracingPipeline();
    void createShaderBindingTable();
    void mainLoop();
    void cleanup();

    Buffer createBuffer(VkDeviceSize size, VkBufferUsageFlags usage, VmaMemoryUsage memUsage);
    VkShaderModule createShaderModule(const std::string& path);

    GLFWwindow* window = nullptr;
    vkb::Instance vkb_inst;
    vkb::Device vkb_device;
    VkDevice device = VK_NULL_HANDLE;
    VkPhysicalDevice physical_device = VK_NULL_HANDLE;
    VkSurfaceKHR surface = VK_NULL_HANDLE;
    VmaAllocator allocator = VK_NULL_HANDLE;

    vkb::Swapchain vkb_swapchain;
    std::vector<VkImage> swapchain_images;
    std::vector<VkImageView> swapchainImageViews;

    // --- 高清画质核心：HDR 与 SDR 双画布 ---
    VkImage accumImage = VK_NULL_HANDLE;
    VmaAllocation accumImageAlloc = VK_NULL_HANDLE;
    VkImageView accumImageView = VK_NULL_HANDLE;

    VkImage storageImage = VK_NULL_HANDLE;
    VmaAllocation storageImageAlloc = VK_NULL_HANDLE;
    VkImageView storageImageView = VK_NULL_HANDLE;

    VkCommandPool cmdPool = VK_NULL_HANDLE;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    VkQueue graphicsQueue = VK_NULL_HANDLE;

    VkAccelerationStructureKHR blas = VK_NULL_HANDLE;
    VkAccelerationStructureKHR tlas = VK_NULL_HANDLE;
    Buffer vertexBuffer;
    Buffer indexBuffer;
    Buffer instanceBuffer;
    Buffer blasBuffer;
    Buffer tlasBuffer;

    VkDescriptorSetLayout descSetLayout = VK_NULL_HANDLE;
    VkDescriptorPool descPool = VK_NULL_HANDLE;
    VkDescriptorPool imguiPool = VK_NULL_HANDLE;
    VkDescriptorSet descSet = VK_NULL_HANDLE;
    VkPipelineLayout pipeLayout = VK_NULL_HANDLE;
    VkPipeline rtPipeline = VK_NULL_HANDLE;
    Buffer sbtBuffer;

    VkStridedDeviceAddressRegionKHR rgenRegion{};
    VkStridedDeviceAddressRegionKHR missRegion{};
    VkStridedDeviceAddressRegionKHR hitRegion{};
    VkStridedDeviceAddressRegionKHR callRegion{};

    VkSemaphore imageAvailable = VK_NULL_HANDLE;
    VkSemaphore renderFinished = VK_NULL_HANDLE;
    VkFence inFlightFence = VK_NULL_HANDLE;

    uint32_t frameCount = 0;
    float ui_lightIntensity = 15.0f;
    glm::vec4 ui_leftWallColor = glm::vec4(0.8f, 0.1f, 0.1f, 1.0f);
    glm::vec4 ui_rightWallColor = glm::vec4(0.1f, 0.8f, 0.1f, 1.0f);
};