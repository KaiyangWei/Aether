#include "MaterialStudy.h"
#include <cmath>
#include <glm/gtc/matrix_transform.hpp>

namespace Aether
{
ImportedAsset MakeMaterialStudy(ImportedAsset asset)
{
    // Parent the optional sample model to a display transform, preserving its hierarchy.
    if (!asset.nodes.empty())
    {
        const auto display =
            glm::translate(glm::mat4(1), glm::vec3(3.6f, 0, 0)) * glm::scale(glm::mat4(1), glm::vec3(1.45f));
        for (auto &node : asset.nodes)
            node.world = display * node.world;
        ImportedNode parent;
        parent.local = parent.world = display;
        parent.children = asset.roots;
        for (auto root : asset.roots)
            asset.nodes[root].parent = asset.nodes.size();
        asset.roots = {asset.nodes.size()};
        asset.nodes.push_back(std::move(parent));
    }
    MeshData sphere;
    constexpr unsigned rings = 32, segments = 64;
    for (unsigned j = 0; j <= rings; ++j)
        for (unsigned i = 0; i <= segments; ++i)
        {
            const float polar = j * 3.14159265f / rings, azimuth = i * 6.2831853f / segments;
            const glm::vec3 normal(std::sin(polar) * std::cos(azimuth), std::cos(polar),
                                   std::sin(polar) * std::sin(azimuth));
            Vertex v{};
            v.normal = normal;
            v.position = normal * .43f;
            v.color = glm::vec3(1);
            sphere.vertices.push_back(v);
            if (j < rings && i < segments)
            {
                unsigned a = j * (segments + 1) + i, b = a + segments + 1;
                sphere.indices.insert(sphere.indices.end(), {a, a + 1, b, a + 1, b + 1, b});
            }
        }
    for (unsigned row = 0; row < 5; ++row)
        for (unsigned col = 0; col < 5; ++col)
        {
            ImportedMaterial material;
            material.baseColor = {.72f, .36f, .14f, 1};
            material.metallic = row * .25f;
            material.roughness = col * .25f;
            ImportedNode node;
            node.mesh = asset.meshes.size();
            node.world = node.local =
                glm::translate(glm::mat4(1), glm::vec3(float(col) - 2.5f, 2.f - float(row), 0));
            asset.meshes.push_back({ImportedPrimitive{sphere, asset.materials.size(), {}}});
            asset.materials.push_back(material);
            asset.roots.push_back(asset.nodes.size());
            asset.nodes.push_back(std::move(node));
        }
    return asset;
}
} // namespace Aether
