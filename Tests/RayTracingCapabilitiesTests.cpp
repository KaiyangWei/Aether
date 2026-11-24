#include <RHI/Vulkan/VulkanRayTracingCapabilities.h>

#include <cstdlib>
#include <iostream>

namespace
{
    int Fail(const char* message)
    {
        std::cerr << "RayTracingCapabilitiesTests FAILED: " << message << '\n';
        return 1;
    }

    Aether::VulkanRayTracingCapabilities MakeSupportedHardwareCaps()
    {
        Aether::VulkanRayTracingCapabilities caps{};
        caps.deferredHostOperationsExtensionSupported = true;
        caps.accelerationStructureExtensionSupported = true;
        caps.rayQueryExtensionSupported = true;
        caps.bufferDeviceAddressFeatureSupported = true;
        caps.accelerationStructureFeatureSupported = true;
        caps.rayQueryFeatureSupported = true;
        caps.graphicsQueueSupportsCompute = true;
        caps.minScratchOffsetAlignment = 128;
        caps.maxGeometryCount = 16777215;
        caps.maxInstanceCount = 16777215;
        caps.maxPrimitiveCount = 536870911;
        caps.hardwareSupported = Aether::EvaluateRayTracingHardwareSupport(caps);
        return caps;
    }

    Aether::VulkanRayTracingFunctions MakeDummyCompleteFunctions()
    {
        Aether::VulkanRayTracingFunctions funcs{};
        funcs.vkCreateAccelerationStructureKHR =
            reinterpret_cast<PFN_vkCreateAccelerationStructureKHR>(uintptr_t{ 0x1001 });
        funcs.vkDestroyAccelerationStructureKHR =
            reinterpret_cast<PFN_vkDestroyAccelerationStructureKHR>(uintptr_t{ 0x1002 });
        funcs.vkGetAccelerationStructureBuildSizesKHR =
            reinterpret_cast<PFN_vkGetAccelerationStructureBuildSizesKHR>(uintptr_t{ 0x1003 });
        funcs.vkCmdBuildAccelerationStructuresKHR =
            reinterpret_cast<PFN_vkCmdBuildAccelerationStructuresKHR>(uintptr_t{ 0x1004 });
        funcs.vkGetAccelerationStructureDeviceAddressKHR =
            reinterpret_cast<PFN_vkGetAccelerationStructureDeviceAddressKHR>(uintptr_t{ 0x1005 });
        return funcs;
    }
}

int main()
{
    using namespace Aether;

    // 1. Default state is cleanly unavailable.
    {
        const VulkanRayTracingCapabilities empty{};
        if (empty.IsHardwareSupported() || empty.IsDeviceEnabled() ||
            empty.IsBufferDeviceAddressEnabled() || empty.AreFunctionsAvailable() ||
            empty.IsReady() || EvaluateRayTracingHardwareSupport(empty))
            return Fail("default capabilities must report unavailable");
    }

    // 2. Missing any single extension, feature, or compute queue bit disqualifies hardware support.
    {
        const auto base = MakeSupportedHardwareCaps();
        if (!base.IsHardwareSupported())
            return Fail("complete hardware caps must report supported");

        auto missingExt = base;
        missingExt.deferredHostOperationsExtensionSupported = false;
        if (EvaluateRayTracingHardwareSupport(missingExt))
            return Fail("missing deferred_host_operations must fail hardware support");

        missingExt = base;
        missingExt.accelerationStructureExtensionSupported = false;
        if (EvaluateRayTracingHardwareSupport(missingExt))
            return Fail("missing acceleration_structure must fail hardware support");

        missingExt = base;
        missingExt.rayQueryExtensionSupported = false;
        if (EvaluateRayTracingHardwareSupport(missingExt))
            return Fail("missing ray_query must fail hardware support");

        auto missingFeat = base;
        missingFeat.bufferDeviceAddressFeatureSupported = false;
        if (EvaluateRayTracingHardwareSupport(missingFeat))
            return Fail("missing bufferDeviceAddress feature must fail hardware support");

        missingFeat = base;
        missingFeat.accelerationStructureFeatureSupported = false;
        if (EvaluateRayTracingHardwareSupport(missingFeat))
            return Fail("missing accelerationStructure feature must fail hardware support");

        missingFeat = base;
        missingFeat.rayQueryFeatureSupported = false;
        if (EvaluateRayTracingHardwareSupport(missingFeat))
            return Fail("missing rayQuery feature must fail hardware support");

        auto noCompute = base;
        noCompute.graphicsQueueSupportsCompute = false;
        if (EvaluateRayTracingHardwareSupport(noCompute))
            return Fail("graphics queue without COMPUTE bit must fail hardware support");
    }

    // 3. Hardware supported, but device RT set disabled (fallback path): never enabled or ready.
    {
        auto caps = MakeSupportedHardwareCaps();
        const auto dummyFuncs = MakeDummyCompleteFunctions();
        FinalizeRayTracingDeviceCapabilities(caps, false, dummyFuncs, true);
        if (!caps.IsHardwareSupported() || caps.IsDeviceEnabled() ||
            caps.IsBufferDeviceAddressEnabled() || caps.AreFunctionsAvailable() ||
            caps.IsReady() || caps.functions.vkCreateAccelerationStructureKHR != nullptr)
            return Fail("disabled device RT must remain non-enabled with null dispatch pointers");
    }

    // 4. Device enabled, but missing ANY single KHR function pointer: atomically clears all pointers, not ready.
    {
        auto caps = MakeSupportedHardwareCaps();
        auto partialFuncs = MakeDummyCompleteFunctions();
        partialFuncs.vkGetAccelerationStructureDeviceAddressKHR = nullptr;
        FinalizeRayTracingDeviceCapabilities(caps, true, partialFuncs, true);
        if (!caps.IsHardwareSupported() || !caps.IsDeviceEnabled() ||
            !caps.IsBufferDeviceAddressEnabled() || caps.AreFunctionsAvailable() ||
            caps.IsReady() ||
            caps.functions.vkCreateAccelerationStructureKHR != nullptr ||
            caps.functions.vkDestroyAccelerationStructureKHR != nullptr ||
            caps.functions.vkGetAccelerationStructureBuildSizesKHR != nullptr ||
            caps.functions.vkCmdBuildAccelerationStructuresKHR != nullptr ||
            caps.functions.vkGetAccelerationStructureDeviceAddressKHR != nullptr)
            return Fail("partial function loading must atomically clear all KHR pointers and report not ready");
    }

    // 5. Device enabled + all 5 KHR functions loaded + compute queue supported => Ready.
    {
        auto caps = MakeSupportedHardwareCaps();
        const auto completeFuncs = MakeDummyCompleteFunctions();
        FinalizeRayTracingDeviceCapabilities(caps, true, completeFuncs, true);
        if (!caps.IsHardwareSupported() || !caps.IsDeviceEnabled() ||
            !caps.IsBufferDeviceAddressEnabled() || !caps.AreFunctionsAvailable() ||
            !caps.IsReady())
            return Fail("complete enabled RT capabilities must report ready");
    }

    // 6. Active graphics queue lacking compute bit overrides readiness to unavailable.
    {
        auto caps = MakeSupportedHardwareCaps();
        const auto completeFuncs = MakeDummyCompleteFunctions();
        FinalizeRayTracingDeviceCapabilities(caps, true, completeFuncs, false);
        if (caps.IsHardwareSupported() || caps.IsDeviceEnabled() ||
            caps.AreFunctionsAvailable() || caps.IsReady())
            return Fail("active graphics queue without compute must disable RT atomically");
    }

    std::cout << "RayTracingCapabilitiesTests passed (6/6)\n";
    return 0;
}
