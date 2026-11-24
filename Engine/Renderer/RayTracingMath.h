#pragma once

#include <vulkan/vulkan.h>
#include <glm/glm.hpp>

#include <cmath>
#include <cstdint>
#include <optional>

namespace Aether
{
    constexpr uint32_t kMaxRtInstanceCustomIndex = 0x00FFFFFFu;
    constexpr uint32_t kMaxRtSbtRecordOffset = 0x00FFFFFFu;

    inline uint64_t AlignUp(uint64_t value, uint64_t alignment) noexcept
    {
        if (alignment <= 1)
            return value;
        const uint64_t remainder = value % alignment;
        return remainder == 0 ? value : value + (alignment - remainder);
    }

    inline VkDeviceAddress AlignUpDeviceAddress(VkDeviceAddress address, VkDeviceSize alignment) noexcept
    {
        return static_cast<VkDeviceAddress>(AlignUp(static_cast<uint64_t>(address), static_cast<uint64_t>(alignment)));
    }

    inline VkDeviceSize ComputePaddedScratchSize(VkDeviceSize requiredSize, VkDeviceSize minAlignment) noexcept
    {
        if (requiredSize == 0)
            return 0;
        if (minAlignment <= 1)
            return requiredSize;
        return requiredSize + (minAlignment - 1);
    }

    inline bool IsAddressAligned(VkDeviceAddress address, VkDeviceSize alignment) noexcept
    {
        if (address == 0)
            return false;
        if (alignment <= 1)
            return true;
        return (address % alignment) == 0;
    }

    inline bool IsFiniteMatrix(const glm::mat4& matrix) noexcept
    {
        for (int col = 0; col < 4; ++col)
        {
            for (int row = 0; row < 4; ++row)
            {
                if (!std::isfinite(matrix[col][row]))
                    return false;
            }
        }
        return true;
    }

    // Converts a GLM column-major 4x4 affine matrix (model[col][row]) into
    // Vulkan's row-major 3x4 VkTransformMatrixKHR (matrix[row][col]).
    inline VkTransformMatrixKHR ToVkTransformMatrix(const glm::mat4& model) noexcept
    {
        VkTransformMatrixKHR out{};
        for (int row = 0; row < 3; ++row)
        {
            for (int col = 0; col < 4; ++col)
            {
                out.matrix[row][col] = model[col][row];
            }
        }
        return out;
    }

    inline glm::vec3 TransformPoint(const VkTransformMatrixKHR& m, const glm::vec3& p) noexcept
    {
        return glm::vec3(
            m.matrix[0][0] * p.x + m.matrix[0][1] * p.y + m.matrix[0][2] * p.z + m.matrix[0][3],
            m.matrix[1][0] * p.x + m.matrix[1][1] * p.y + m.matrix[1][2] * p.z + m.matrix[1][3],
            m.matrix[2][0] * p.x + m.matrix[2][1] * p.y + m.matrix[2][2] * p.z + m.matrix[2][3]
        );
    }

    inline bool AreVkTransformMatricesEqual(
        const VkTransformMatrixKHR& a,
        const VkTransformMatrixKHR& b) noexcept
    {
        for (int row = 0; row < 3; ++row)
        {
            for (int col = 0; col < 4; ++col)
            {
                if (a.matrix[row][col] != b.matrix[row][col])
                    return false;
            }
        }
        return true;
    }

    // Instance-level flags override BLAS opacity so Opaque and Alpha-MASK instances can
    // safely share a single BLAS without rebuilding it.
    inline VkGeometryInstanceFlagsKHR ComputeRtInstanceFlags(
        bool doubleSided,
        bool mirrored,
        bool alphaMask) noexcept
    {
        VkGeometryInstanceFlagsKHR flags = 0;
        if (doubleSided)
            flags |= VK_GEOMETRY_INSTANCE_TRIANGLE_FACING_CULL_DISABLE_BIT_KHR;
        if (mirrored)
            flags |= VK_GEOMETRY_INSTANCE_TRIANGLE_FLIP_FACING_BIT_KHR;
        if (alphaMask)
            flags |= VK_GEOMETRY_INSTANCE_FORCE_NO_OPAQUE_BIT_KHR;
        else
            flags |= VK_GEOMETRY_INSTANCE_FORCE_OPAQUE_BIT_KHR;
        return flags;
    }

    inline bool IsValidRtInstanceCustomIndex(uint32_t customIndex) noexcept
    {
        return customIndex <= kMaxRtInstanceCustomIndex;
    }

    inline bool IsValidRtSbtRecordOffset(uint32_t sbtOffset) noexcept
    {
        return sbtOffset <= kMaxRtSbtRecordOffset;
    }

    struct RtInstanceBuildParams
    {
        glm::mat4 model{ 1.0f };
        uint64_t blasDeviceAddress = 0;
        uint32_t customIndex = 0;
        uint8_t mask = 0xFFu;
        uint32_t sbtRecordOffset = 0;
        VkGeometryInstanceFlagsKHR flags = 0;
    };

    inline std::optional<VkAccelerationStructureInstanceKHR> BuildVkAccelerationStructureInstance(
        const RtInstanceBuildParams& params) noexcept
    {
        if (params.blasDeviceAddress == 0 ||
            params.mask == 0 ||
            !IsFiniteMatrix(params.model) ||
            !IsValidRtInstanceCustomIndex(params.customIndex) ||
            !IsValidRtSbtRecordOffset(params.sbtRecordOffset))
        {
            return std::nullopt;
        }

        VkAccelerationStructureInstanceKHR instance{};
        instance.transform = ToVkTransformMatrix(params.model);
        instance.instanceCustomIndex = params.customIndex & kMaxRtInstanceCustomIndex;
        instance.mask = params.mask;
        instance.instanceShaderBindingTableRecordOffset = params.sbtRecordOffset & kMaxRtSbtRecordOffset;
        instance.flags = static_cast<uint32_t>(params.flags) & 0xFFu;
        instance.accelerationStructureReference = params.blasDeviceAddress;
        return instance;
    }
}
