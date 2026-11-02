#include <RHI/Vulkan/Shader/VulkanShader.h>

#include <fstream>
#include <vector>
#include <iostream>
#include <cstdint>

namespace Aether
{
    VulkanShader::~VulkanShader()
    {
        Shutdown();
    }

    bool VulkanShader::Initialize(
        VkDevice device,
        const std::string& filepath)
    {
        if (device == VK_NULL_HANDLE)
        {
            std::cerr << "VulkanShader: Invalid VkDevice.\n";
            return false;
        }

        if (m_shaderModule != VK_NULL_HANDLE)
        {
            std::cerr << "VulkanShader: Already initialized.\n";
            return false;
        }

        // Read SPIR-V binary
        std::ifstream file(filepath, std::ios::ate | std::ios::binary);

        if (!file.is_open())
        {
            std::cerr << "VulkanShader: Failed to open file: "
                << filepath << '\n';
            return false;
        }

        const std::streamsize fileSize = file.tellg();

        if (fileSize <= 0 || fileSize % sizeof(uint32_t) != 0)
        {
            std::cerr << "VulkanShader: Invalid SPIR-V file size.\n";
            return false;
        }

        std::vector<uint32_t> shaderCode(
            static_cast<size_t>(fileSize) / sizeof(uint32_t));

        file.seekg(0);

        if (!file.read(
            reinterpret_cast<char*>(shaderCode.data()),
            fileSize))
        {
            std::cerr << "VulkanShader: Failed to read SPIR-V file.\n";
            return false;
        }

        VkShaderModuleCreateInfo createInfo{};
        createInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
        createInfo.codeSize = static_cast<size_t>(fileSize);
        createInfo.pCode = shaderCode.data();

        VkShaderModule shaderModule = VK_NULL_HANDLE;

        const VkResult result = vkCreateShaderModule(
            device,
            &createInfo,
            nullptr,
            &shaderModule
        );

        if (result != VK_SUCCESS)
        {
            std::cerr << "VulkanShader: Failed to create Shader Module. "
                << "VkResult: " << result << '\n';
            return false;
        }

        m_device = device;
        m_shaderModule = shaderModule;

        return true;
    }

    void VulkanShader::Shutdown()
    {
        if (m_shaderModule != VK_NULL_HANDLE)
        {
            vkDestroyShaderModule(
                m_device,
                m_shaderModule,
                nullptr
            );

            m_shaderModule = VK_NULL_HANDLE;
        }

        m_device = VK_NULL_HANDLE;
    }
}