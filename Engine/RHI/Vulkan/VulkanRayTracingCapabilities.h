#pragma once

#include <vulkan/vulkan.h>

#include <cstdint>

namespace Aether
{
    struct VulkanRayTracingFunctions
    {
        PFN_vkCreateAccelerationStructureKHR vkCreateAccelerationStructureKHR = nullptr;
        PFN_vkDestroyAccelerationStructureKHR vkDestroyAccelerationStructureKHR = nullptr;
        PFN_vkGetAccelerationStructureBuildSizesKHR vkGetAccelerationStructureBuildSizesKHR = nullptr;
        PFN_vkCmdBuildAccelerationStructuresKHR vkCmdBuildAccelerationStructuresKHR = nullptr;
        PFN_vkGetAccelerationStructureDeviceAddressKHR vkGetAccelerationStructureDeviceAddressKHR = nullptr;

        bool AreAllLoaded() const noexcept
        {
            return vkCreateAccelerationStructureKHR != nullptr &&
                   vkDestroyAccelerationStructureKHR != nullptr &&
                   vkGetAccelerationStructureBuildSizesKHR != nullptr &&
                   vkCmdBuildAccelerationStructuresKHR != nullptr &&
                   vkGetAccelerationStructureDeviceAddressKHR != nullptr;
        }
    };

    struct VulkanRayTracingCapabilities
    {
        // Hardware / physical device support (queried before logical device creation).
        bool deferredHostOperationsExtensionSupported = false;
        bool accelerationStructureExtensionSupported = false;
        bool rayQueryExtensionSupported = false;
        bool bufferDeviceAddressFeatureSupported = false;
        bool accelerationStructureFeatureSupported = false;
        bool rayQueryFeatureSupported = false;
        bool graphicsQueueSupportsCompute = false;
        bool hardwareSupported = false;

        // Device enablement state (true only when the full required RT capability set was atomically enabled).
        bool bufferDeviceAddressEnabled = false;
        bool accelerationStructureEnabled = false;
        bool rayQueryEnabled = false;
        bool deviceEnabled = false;

        // Runtime availability of the five required acceleration structure KHR functions.
        bool functionsLoaded = false;
        VulkanRayTracingFunctions functions{};

        // Queried acceleration structure hardware limits / alignment properties.
        VkDeviceSize minScratchOffsetAlignment = 0;
        uint64_t maxGeometryCount = 0;
        uint64_t maxInstanceCount = 0;
        uint64_t maxPrimitiveCount = 0;
        uint32_t maxPerStageDescriptorAccelerationStructures = 0;

        bool IsHardwareSupported() const noexcept
        {
            return hardwareSupported;
        }

        bool IsDeviceEnabled() const noexcept
        {
            return deviceEnabled;
        }

        bool IsBufferDeviceAddressEnabled() const noexcept
        {
            return bufferDeviceAddressEnabled;
        }

        bool AreFunctionsAvailable() const noexcept
        {
            return functionsLoaded && functions.AreAllLoaded();
        }

        bool IsReady() const noexcept
        {
            return hardwareSupported && deviceEnabled && AreFunctionsAvailable();
        }
    };

    inline bool EvaluateRayTracingHardwareSupport(const VulkanRayTracingCapabilities& caps) noexcept
    {
        return caps.deferredHostOperationsExtensionSupported &&
               caps.accelerationStructureExtensionSupported &&
               caps.rayQueryExtensionSupported &&
               caps.bufferDeviceAddressFeatureSupported &&
               caps.accelerationStructureFeatureSupported &&
               caps.rayQueryFeatureSupported &&
               caps.graphicsQueueSupportsCompute;
    }

    inline void FinalizeRayTracingDeviceCapabilities(
        VulkanRayTracingCapabilities& caps,
        bool deviceSetEnabled,
        const VulkanRayTracingFunctions& loadedFunctions,
        bool activeGraphicsQueueSupportsCompute) noexcept
    {
        caps.graphicsQueueSupportsCompute = activeGraphicsQueueSupportsCompute;
        caps.hardwareSupported = EvaluateRayTracingHardwareSupport(caps);

        if (!deviceSetEnabled || !caps.hardwareSupported)
        {
            caps.bufferDeviceAddressEnabled = false;
            caps.accelerationStructureEnabled = false;
            caps.rayQueryEnabled = false;
            caps.deviceEnabled = false;
            caps.functions = {};
            caps.functionsLoaded = false;
            return;
        }

        caps.bufferDeviceAddressEnabled = true;
        caps.accelerationStructureEnabled = true;
        caps.rayQueryEnabled = true;
        caps.deviceEnabled = true;

        if (loadedFunctions.AreAllLoaded())
        {
            caps.functions = loadedFunctions;
            caps.functionsLoaded = true;
        }
        else
        {
            // Never expose partially loaded function pointers.
            caps.functions = {};
            caps.functionsLoaded = false;
        }
    }
}
