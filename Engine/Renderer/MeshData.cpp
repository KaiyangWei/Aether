#include "MeshData.h"

#include <glm/geometric.hpp>
#include <algorithm>
#include <cmath>
#include <limits>

namespace Aether
{
    bool MeshData::IsValid() const
    {
        return !vertices.empty() && !indices.empty() && indices.size() % 3 == 0 &&
            indices.size() <= std::numeric_limits<uint32_t>::max() &&
            std::all_of(indices.begin(), indices.end(), [this](uint32_t index) {
                return index < vertices.size();
            }) &&
            std::all_of(vertices.begin(), vertices.end(), [](const Vertex& vertex) {
                const auto finite = [](const auto& attribute) {
                    for (glm::length_t i = 0; i < attribute.length(); ++i)
                        if (!std::isfinite(attribute[i]))
                            return false;
                    return true;
                };
                return finite(vertex.position) && finite(vertex.color) &&
                    finite(vertex.normal) && finite(vertex.uv) && finite(vertex.tangent) && std::isfinite(vertex.colorAlpha);
            });
    }

    MeshData CreateCubeMesh()
    {
        const Vertex corners[] = {
            // Front corners (z = +0.5)
            { { -0.5f, -0.5f,  0.5f }, { 1.0f, 0.2f, 0.2f } },
            { { 0.5f, -0.5f,  0.5f }, { 0.2f, 1.0f, 0.2f } },
            { { 0.5f,  0.5f,  0.5f }, { 0.2f, 0.4f, 1.0f } },
            { { -0.5f,  0.5f,  0.5f }, { 1.0f, 0.9f, 0.2f } },

            // Back corners (z = -0.5)
            { { -0.5f, -0.5f, -0.5f }, { 1.0f, 0.2f, 1.0f } },
            { { 0.5f, -0.5f, -0.5f }, { 0.2f, 1.0f, 1.0f } },
            { { 0.5f,  0.5f, -0.5f }, { 1.0f, 1.0f, 1.0f } },
            { { -0.5f,  0.5f, -0.5f }, { 0.5f, 0.2f, 1.0f } }
        };
        const uint32_t faces[][4] = {
            { 0, 1, 2, 3 }, { 5, 4, 7, 6 },
            { 1, 5, 6, 2 }, { 4, 0, 3, 7 },
            { 3, 2, 6, 7 }, { 4, 5, 1, 0 }
        };
        const glm::vec2 uvs[] = { {0, 0}, {1, 0}, {1, 1}, {0, 1} };

        MeshData mesh;
        mesh.vertices.reserve(24);
        mesh.indices.reserve(36);
        for (const auto& face : faces)
        {
            const auto base = static_cast<uint32_t>(mesh.vertices.size());
            const auto u = corners[face[1]].position - corners[face[0]].position;
            const auto v = corners[face[3]].position - corners[face[0]].position;
            const auto normal = glm::normalize(glm::cross(u, v));
            const glm::vec4 tangent(glm::normalize(u), 1.0f);
            for (uint32_t i = 0; i < 4; ++i)
            {
                auto vertex = corners[face[i]];
                vertex.normal = normal;
                vertex.uv = uvs[i];
                vertex.tangent = tangent;
                mesh.vertices.push_back(vertex);
            }
            mesh.indices.insert(mesh.indices.end(), {base, base + 1, base + 2, base + 2, base + 3, base});
        }
        return mesh;
    }

    MeshData CreateTetrahedronMesh()
    {
        const glm::vec3 positions[] = {
            { 0.5f, 0.5f, 0.5f }, { -0.5f, -0.5f, 0.5f },
            { -0.5f, 0.5f, -0.5f }, { 0.5f, -0.5f, -0.5f }
        };
        const glm::vec3 colors[] = {
            { 1.0f, 0.5f, 0.2f }, { 0.2f, 0.9f, 1.0f },
            { 0.8f, 0.3f, 1.0f }, { 1.0f, 0.9f, 0.3f }
        };
        const uint32_t faces[][3] = { {0, 1, 3}, {0, 2, 1}, {0, 3, 2}, {1, 2, 3} };
        const glm::vec2 uvs[] = { {0, 0}, {1, 0}, {0, 1} };

        MeshData mesh;
        mesh.vertices.reserve(12);
        mesh.indices.reserve(12);
        for (const auto& face : faces)
        {
            const auto u = positions[face[1]] - positions[face[0]];
            const auto v = positions[face[2]] - positions[face[0]];
            const auto normal = glm::normalize(glm::cross(u, v));
            const glm::vec4 tangent(glm::normalize(u), 1.0f);
            for (uint32_t i = 0; i < 3; ++i)
            {
                mesh.indices.push_back(static_cast<uint32_t>(mesh.vertices.size()));
                mesh.vertices.push_back({ positions[face[i]], colors[face[i]], normal, uvs[i], tangent });
            }
        }
        return mesh;
    }
}
