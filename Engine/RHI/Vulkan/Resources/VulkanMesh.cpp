#include "VulkanMesh.h"

#include <Renderer/MeshData.h>

#include <atomic>

namespace Aether
{
    namespace
    {
        std::atomic<uint64_t> g_nextVulkanMeshUniqueId{ 1 };
    }
    VkResult VulkanMesh::Initialize(VkDevice device, const VulkanMemoryManager& memory, const MeshData& data)
    {
        return Initialize(device, memory, data, false, false);
    }

    VkResult VulkanMesh::Initialize(VkDevice device, const VulkanMemoryManager& memory,
                                    const MeshData& data, const VulkanMeshCreateOptions& options)
    {
        return Initialize(device, memory, data, false, options.enableRayTracingUsage);
    }

    VkResult VulkanMesh::Initialize(VkDevice device, const VulkanMemoryManager& memory,
                                    const MeshData& data, bool failIndexUpload,
                                    bool enableRayTracingUsage)
    {
        if (m_indexCount || m_vertexCount || !data.IsValid())
            return VK_ERROR_INITIALIZATION_FAILED;

        const auto upload = [&](VulkanBuffer& buffer, VkBufferUsageFlags usage,
                                const void* bytes, VkDeviceSize size) {
            const VkResult result = buffer.Initialize(device, memory, size, usage,
                VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT, VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
            if (result != VK_SUCCESS)
                return result;
            if (failIndexUpload && (usage & VK_BUFFER_USAGE_INDEX_BUFFER_BIT) != 0)
                return VK_ERROR_UNKNOWN;
            return buffer.UploadData(bytes, size);
        };

        VkBufferUsageFlags extraUsage = 0;
        if (enableRayTracingUsage)
        {
            extraUsage = VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT |
                         VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR;
        }

        VkResult result = upload(m_vertexBuffer, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | extraUsage,
            data.vertices.data(), data.vertices.size() * sizeof(Vertex));
        if (result == VK_SUCCESS)
            result = upload(m_indexBuffer, VK_BUFFER_USAGE_INDEX_BUFFER_BIT | extraUsage,
                data.indices.data(), data.indices.size() * sizeof(uint32_t));

        if (result == VK_SUCCESS)
        {
            m_vertexCount = static_cast<uint32_t>(data.vertices.size());
            m_indexCount = static_cast<uint32_t>(data.indices.size());
            m_uniqueId = g_nextVulkanMeshUniqueId.fetch_add(1, std::memory_order_relaxed);
        }
        else
            Shutdown();
        return result;
    }

    void VulkanMesh::Shutdown() noexcept
    {
        m_indexBuffer.Shutdown();
        m_vertexBuffer.Shutdown();
        m_vertexCount = 0;
        m_indexCount = 0;
        m_uniqueId = 0;
    }
}
