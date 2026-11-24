#pragma once

#include <RHI/Vulkan/Resources/VulkanBuffer.h>
#include <RHI/Vulkan/VulkanRayTracingCapabilities.h>

#include <vulkan/vulkan.h>

#include <cstdint>

namespace Aether
{
    class VulkanMemoryManager;

    class VulkanAccelerationStructure
    {
    public:
        VulkanAccelerationStructure() = default;
        ~VulkanAccelerationStructure();

        VulkanAccelerationStructure(const VulkanAccelerationStructure&) = delete;
        VulkanAccelerationStructure& operator=(const VulkanAccelerationStructure&) = delete;

        VkResult Initialize(
            VkDevice device,
            const VulkanMemoryManager& memory,
            const VulkanRayTracingCapabilities& capabilities,
            VkAccelerationStructureTypeKHR type,
            VkDeviceSize size);

        void Shutdown() noexcept;

        bool IsValid() const noexcept
        {
            return m_handle != VK_NULL_HANDLE &&
                   m_storageBuffer.GetBuffer() != VK_NULL_HANDLE &&
                   m_deviceAddress != 0;
        }

        VkAccelerationStructureKHR GetHandle() const noexcept { return m_handle; }
        VkDeviceAddress GetDeviceAddress() const noexcept { return m_deviceAddress; }
        VkAccelerationStructureTypeKHR GetType() const noexcept { return m_type; }
        VkDeviceSize GetSize() const noexcept { return m_size; }
        const VulkanBuffer& GetStorageBuffer() const noexcept { return m_storageBuffer; }

    private:
        friend class AetherApplication;
        friend class VulkanRayTracingScene;

        VkResult InitializeWithFaultInjection(
            VkDevice device,
            const VulkanMemoryManager& memory,
            const VulkanRayTracingCapabilities& capabilities,
            VkAccelerationStructureTypeKHR type,
            VkDeviceSize size,
            bool failStorageAllocation,
            bool failAsCreate);

        VkDevice m_device = VK_NULL_HANDLE;
        PFN_vkDestroyAccelerationStructureKHR m_destroyFunc = nullptr;
        VulkanBuffer m_storageBuffer;
        VkAccelerationStructureKHR m_handle = VK_NULL_HANDLE;
        VkDeviceAddress m_deviceAddress = 0;
        VkAccelerationStructureTypeKHR m_type = VK_ACCELERATION_STRUCTURE_TYPE_GENERIC_KHR;
        VkDeviceSize m_size = 0;
    };
}
