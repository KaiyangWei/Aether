#pragma once

#include <RHI/Vulkan/Resources/VulkanBuffer.h>

#include <cstdint>

namespace Aether
{
    struct MeshData;

    struct VulkanMeshCreateOptions
    {
        bool enableRayTracingUsage = false;
    };

    class VulkanMesh
    {
    public:
        VkResult Initialize(VkDevice device, const VulkanMemoryManager& memory, const MeshData& data);
        VkResult Initialize(VkDevice device, const VulkanMemoryManager& memory, const MeshData& data,
                            const VulkanMeshCreateOptions& options);
        void Shutdown() noexcept;

        const VulkanBuffer& GetVertexBuffer() const { return m_vertexBuffer; }
        const VulkanBuffer& GetIndexBuffer() const { return m_indexBuffer; }
        uint32_t GetVertexCount() const { return m_vertexCount; }
        uint32_t GetIndexCount() const { return m_indexCount; }
        uint64_t GetUniqueId() const noexcept { return m_uniqueId; }
        bool IsRayTracingUsageEnabled() const noexcept
        {
            return m_vertexBuffer.HasDeviceAddressUsage() && m_indexBuffer.HasDeviceAddressUsage();
        }

    private:
        friend class AetherApplication;
        friend class VulkanAssetUploader;
        VkResult Initialize(VkDevice device, const VulkanMemoryManager& memory,
                            const MeshData& data, bool failIndexUpload,
                            bool enableRayTracingUsage = false);
        VulkanBuffer m_vertexBuffer;
        VulkanBuffer m_indexBuffer;
        uint32_t m_vertexCount = 0;
        uint32_t m_indexCount = 0;
        uint64_t m_uniqueId = 0;
    };
}
