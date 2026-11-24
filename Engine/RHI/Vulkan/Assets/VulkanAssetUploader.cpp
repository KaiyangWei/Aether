#include "VulkanAssetUploader.h"

#include <Assets/GltfImporter.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <limits>

namespace Aether
{
    AssetUploadResult VulkanAssetUploader::Upload(VkDevice device, const VulkanMemoryManager& memory,
                                                 const ImportedAsset& asset, MeshId firstId,
                                                 bool enableRayTracingUsage)
    {
        return Upload(device, memory, asset, firstId, std::nullopt, enableRayTracingUsage);
    }

    AssetUploadResult VulkanAssetUploader::Upload(VkDevice device, const VulkanMemoryManager& memory,
                                                 const ImportedAsset& asset, MeshId firstId,
                                                 std::optional<size_t> failPrimitive,
                                                 bool enableRayTracingUsage)
    {
        UploadedAsset batch;
        batch.minimum = glm::vec3(std::numeric_limits<float>::max());
        batch.maximum = -batch.minimum;

        // A mesh's primitive IDs are assigned the first time an active node references it.
        std::vector<std::optional<MeshId>> firstPrimitive(asset.meshes.size());
        std::vector<size_t> pending = asset.roots;
        for (size_t i = 0; i < pending.size(); ++i)
        {
            const auto& node = asset.nodes.at(pending[i]);
            pending.insert(pending.end(), node.children.begin(), node.children.end());
            if (!node.mesh)
                continue;

            const auto meshIndex = *node.mesh;
            const auto& primitives = asset.meshes.at(meshIndex);
            auto& base = firstPrimitive[meshIndex];
            if (!base)
            {
                base = firstId + batch.meshes.size();
                for (size_t primitive = 0; primitive < primitives.size(); ++primitive)
                {
                    auto mesh = std::make_unique<VulkanMesh>();
                    const auto& part = primitives[primitive];
                    const ImportedMaterial fallback;
                    const auto& material = part.material ? asset.materials.at(*part.material) : fallback;
                    const std::array slots{material.baseColorTexture, material.metallicRoughnessTexture,
                        material.normalTexture, material.occlusionTexture, material.emissiveTexture};
                    auto geometry = part.geometry;
                    for (size_t slot = 0; slot < slots.size(); ++slot)
                    {
                        const auto set = slots[slot] ? slots[slot]->uvSet : 0;
                        if (!set)
                        {
                            for (auto& vertex : geometry.vertices)
                                vertex.materialUv[slot] = vertex.uv;
                        }
                        else
                        {
                            const auto coordinates = std::ranges::find(part.extraTexCoords, set,
                                &std::pair<size_t, std::vector<glm::vec2>>::first);
                            if (coordinates == part.extraTexCoords.end())
                                return {{}, "Material references absent TEXCOORD_" + std::to_string(set)};
                            for (size_t v = 0; v < geometry.vertices.size(); ++v)
                                geometry.vertices[v].materialUv[slot] = coordinates->second[v];
                        }
                    }
                    const auto result = mesh->Initialize(device, memory, geometry,
                        failPrimitive == batch.meshes.size(), enableRayTracingUsage);
                    if (result != VK_SUCCESS)
                        return {{}, "mesh " + std::to_string(meshIndex) + " primitive " +
                            std::to_string(primitive) + ": GPU upload failed (" + std::to_string(result) + ")"};
                    batch.meshes.push_back(std::move(mesh));
                }
            }
            for (size_t p = 0; p < primitives.size(); ++p)
            {
                const auto& part = primitives[p];
                SceneObject instance;
                instance.mesh.meshId = *base + p;
                instance.modelMatrix = node.world;
                instance.material = part.material;
                instance.doubleSided = part.material && asset.materials.at(*part.material).doubleSided;
                batch.objects.push_back(instance);
                for (const auto& vertex : part.geometry.vertices)
                {
                    const auto point = glm::vec3(node.world * glm::vec4(vertex.position, 1));
                    if (!std::isfinite(point.x) || !std::isfinite(point.y) || !std::isfinite(point.z))
                        return {{}, "Mesh bounds are non-finite after transformation"};
                    batch.minimum = glm::min(batch.minimum, point);
                    batch.maximum = glm::max(batch.maximum, point);
                }
            }
        }
        if (batch.objects.empty())
            return {{}, "Selected glTF scene has no renderable primitives"};
        return {std::move(batch), {}};
    }
}
