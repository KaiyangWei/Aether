#pragma once

#include <vulkan/vulkan.h>

#include <string>

namespace Aether
{
    class VulkanShader
    {
    public:
        VulkanShader() = default;
        ~VulkanShader();

        VulkanShader(const VulkanShader&) = delete;
        VulkanShader& operator=(const VulkanShader&) = delete;

        bool Initialize(VkDevice device, const std::string& filepath);
        void Shutdown();

        VkShaderModule GetShaderModule() const
        {
            return m_shaderModule;
        }

    private:
        VkDevice m_device = VK_NULL_HANDLE;
        VkShaderModule m_shaderModule = VK_NULL_HANDLE;
    };
}