#pragma once

#include <RHI/Vulkan/Resources/VulkanMesh.h>
#include <Scene/Scene.h>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace Aether
{
    struct ImportedAsset;

    struct UploadedAsset
    {
        std::vector<std::unique_ptr<VulkanMesh>> meshes;
        std::vector<SceneObject> objects;
        glm::vec3 minimum{0}, maximum{0};
    };

    struct AssetUploadResult
    {
        std::optional<UploadedAsset> asset;
        std::string error;
    };

    class VulkanAssetUploader
    {
    public:
        static AssetUploadResult Upload(VkDevice device, const VulkanMemoryManager& memory,
                                       const ImportedAsset& asset, MeshId firstId,
                                       bool enableRayTracingUsage = false);

    private:
        friend class AetherApplication;
        static AssetUploadResult Upload(VkDevice device, const VulkanMemoryManager& memory,
                                       const ImportedAsset& asset, MeshId firstId,
                                       std::optional<size_t> failPrimitive,
                                       bool enableRayTracingUsage = false);
    };
}
