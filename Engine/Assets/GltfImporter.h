#pragma once

#include <Renderer/MeshData.h>
#include <glm/glm.hpp>
#include <cstddef>
#include <filesystem>
#include <optional>
#include <string>

namespace Aether
{
    struct ImportedTextureSlot
    {
        size_t texture;
        size_t uvSet;
        float strength;
    };

    struct ImportedMaterial
    {
        glm::vec4 baseColor{1};
        glm::vec3 emissive{0};
        float metallic = 1, roughness = 1, alphaCutoff = 0.5f;
        enum class AlphaMode { Opaque, Mask, Blend } alphaMode = AlphaMode::Opaque;
        bool doubleSided = false;
        std::optional<ImportedTextureSlot> baseColorTexture, metallicRoughnessTexture,
            normalTexture, occlusionTexture, emissiveTexture;
    };

    struct ImportedImage
    {
        std::vector<std::byte> bytes;
        std::string mimeType;
    };

    struct ImportedSampler
    {
        std::optional<int> minFilter, magFilter;
        int wrapS, wrapT;
    };

    struct ImportedTexture
    {
        size_t image;
        std::optional<size_t> sampler;
    };

    struct ImportedPrimitive
    {
        MeshData geometry;
        std::optional<size_t> material;
        std::vector<std::pair<size_t, std::vector<glm::vec2>>> extraTexCoords;
    };

    struct ImportedNode
    {
        std::string name;
        std::optional<size_t> mesh, parent;
        std::vector<size_t> children;
        glm::mat4 local{1}, world{1};
    };

    struct ImportedAsset
    {
        std::vector<std::vector<ImportedPrimitive>> meshes;
        std::vector<ImportedNode> nodes;
        std::vector<size_t> roots;
        std::vector<ImportedMaterial> materials;
        std::vector<ImportedImage> images;
        std::vector<ImportedTexture> textures;
        std::vector<ImportedSampler> samplers;
    };

    struct GltfImportResult
    {
        std::optional<ImportedAsset> asset;
        std::string error;
    };

    GltfImportResult ImportGltf(const std::filesystem::path& path);
}
