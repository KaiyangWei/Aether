#include "VulkanAccelerationStructure.h"

#include <RHI/Vulkan/VulkanMemoryManager.h>

#include <iostream>

namespace Aether
{
    VulkanAccelerationStructure::~VulkanAccelerationStructure()
    {
        Shutdown();
    }

    VkResult VulkanAccelerationStructure::Initialize(
        VkDevice device,
        const VulkanMemoryManager& memory,
        const VulkanRayTracingCapabilities& capabilities,
        VkAccelerationStructureTypeKHR type,
        VkDeviceSize size)
    {
        return InitializeWithFaultInjection(
            device, memory, capabilities, type, size, false, false);
    }

    VkResult VulkanAccelerationStructure::InitializeWithFaultInjection(
        VkDevice device,
        const VulkanMemoryManager& memory,
        const VulkanRayTracingCapabilities& capabilities,
        VkAccelerationStructureTypeKHR type,
        VkDeviceSize size,
        bool failStorageAllocation,
        bool failAsCreate)
    {
        if (m_handle != VK_NULL_HANDLE || m_storageBuffer.GetBuffer() != VK_NULL_HANDLE)
        {
            std::cerr << "VulkanAccelerationStructure::Initialize() failed: already initialized.\n";
            return VK_ERROR_INITIALIZATION_FAILED;
        }
        if (device == VK_NULL_HANDLE || size == 0)
        {
            std::cerr << "VulkanAccelerationStructure::Initialize() failed: invalid device or zero size.\n";
            return VK_ERROR_INITIALIZATION_FAILED;
        }
        if (!capabilities.IsReady())
        {
            std::cerr << "VulkanAccelerationStructure::Initialize() failed: Ray Tracing capabilities are not ready.\n";
            return VK_ERROR_FEATURE_NOT_PRESENT;
        }
        if (type != VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR &&
            type != VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR)
        {
            std::cerr << "VulkanAccelerationStructure::Initialize() failed: unsupported acceleration structure type.\n";
            return VK_ERROR_INITIALIZATION_FAILED;
        }

        if (failStorageAllocation)
        {
            std::cerr << "VulkanAccelerationStructure::Initialize() failed: injected storage allocation failure.\n";
            return VK_ERROR_OUT_OF_DEVICE_MEMORY;
        }

        constexpr VkBufferUsageFlags storageUsage =
            VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR |
            VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
        VkResult result = m_storageBuffer.Initialize(
            device,
            memory,
            size,
            storageUsage,
            VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        if (result != VK_SUCCESS)
        {
            std::cerr << "VulkanAccelerationStructure::Initialize() failed: storage buffer allocation failed ("
                << result << ").\n";
            Shutdown();
            return result;
        }

        if (failAsCreate)
        {
            std::cerr << "VulkanAccelerationStructure::Initialize() failed: injected AS handle creation failure.\n";
            Shutdown();
            return VK_ERROR_UNKNOWN;
        }

        VkAccelerationStructureCreateInfoKHR createInfo{};
        createInfo.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR;
        createInfo.buffer = m_storageBuffer.GetBuffer();
        createInfo.offset = 0;
        createInfo.size = size;
        createInfo.type = type;

        VkAccelerationStructureKHR handle = VK_NULL_HANDLE;
        result = capabilities.functions.vkCreateAccelerationStructureKHR(
            device, &createInfo, nullptr, &handle);
        if (result != VK_SUCCESS || handle == VK_NULL_HANDLE)
        {
            std::cerr << "VulkanAccelerationStructure::Initialize() failed: vkCreateAccelerationStructureKHR failed ("
                << result << ").\n";
            Shutdown();
            return result != VK_SUCCESS ? result : VK_ERROR_INITIALIZATION_FAILED;
        }

        m_device = device;
        m_destroyFunc = capabilities.functions.vkDestroyAccelerationStructureKHR;
        m_handle = handle;
        m_type = type;
        m_size = size;

        VkAccelerationStructureDeviceAddressInfoKHR addressInfo{};
        addressInfo.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_DEVICE_ADDRESS_INFO_KHR;
        addressInfo.accelerationStructure = m_handle;
        m_deviceAddress = capabilities.functions.vkGetAccelerationStructureDeviceAddressKHR(
            device, &addressInfo);
        if (m_deviceAddress == 0)
        {
            std::cerr << "VulkanAccelerationStructure::Initialize() failed: "
                << "vkGetAccelerationStructureDeviceAddressKHR returned 0.\n";
            Shutdown();
            return VK_ERROR_INITIALIZATION_FAILED;
        }

        return VK_SUCCESS;
    }

    void VulkanAccelerationStructure::Shutdown() noexcept
    {
        // Destroy the AS handle strictly BEFORE releasing its backing storage VulkanBuffer.
        if (m_handle != VK_NULL_HANDLE && m_device != VK_NULL_HANDLE && m_destroyFunc != nullptr)
        {
            m_destroyFunc(m_device, m_handle, nullptr);
        }
        m_handle = VK_NULL_HANDLE;
        m_deviceAddress = 0;
        m_size = 0;
        m_type = VK_ACCELERATION_STRUCTURE_TYPE_GENERIC_KHR;
        m_device = VK_NULL_HANDLE;
        m_destroyFunc = nullptr;

        m_storageBuffer.Shutdown();
    }
}
