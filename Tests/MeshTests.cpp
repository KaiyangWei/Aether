#include <Renderer/MeshData.h>
#include <glm/geometric.hpp>
#include <limits>

int main()
{
    auto mesh = Aether::CreateCubeMesh();
    if (!mesh.IsValid() || mesh.vertices.size() != 24 || mesh.indices.size() != 36)
        return 1;
    for (size_t i = 0; i < mesh.indices.size(); i += 3)
    {
        const auto a = mesh.vertices[mesh.indices[i]].position;
        const auto b = mesh.vertices[mesh.indices[i + 1]].position;
        const auto c = mesh.vertices[mesh.indices[i + 2]].position;
        if (glm::dot(glm::cross(b - a, c - a), a) <= 0.0f)
            return 2;
    }
    for (size_t base = 0; base < mesh.vertices.size(); base += 4)
    {
        const auto& origin = mesh.vertices[base];
        const auto u = mesh.vertices[base + 1].position - origin.position;
        const auto v = mesh.vertices[base + 3].position - origin.position;
        for (size_t i = 0; i < 4; ++i)
        {
            const auto& vertex = mesh.vertices[base + i];
            const glm::vec3 tangent(vertex.tangent);
            if (vertex.normal != glm::normalize(glm::cross(u, v)) ||
                tangent != glm::normalize(u) ||
                glm::cross(vertex.normal, tangent) * vertex.tangent.w != glm::normalize(v) ||
                vertex.position != origin.position + u * vertex.uv.x + v * vertex.uv.y)
                return 5;
        }
    }
    auto nonFinite = mesh;
    nonFinite.vertices.front().uv.x = std::numeric_limits<float>::quiet_NaN();
    if (nonFinite.IsValid())
        return 6;

    auto invalid = mesh;
    invalid.indices.front() = static_cast<uint32_t>(mesh.vertices.size());
    if (invalid.IsValid())
        return 3;
    mesh.indices.pop_back();
    if (mesh.IsValid() || Aether::MeshData{}.IsValid())
        return 4;
    const auto tetrahedron = Aether::CreateTetrahedronMesh();
    if (!tetrahedron.IsValid() || tetrahedron.indices.size() != 12)
        return 7;
    for (size_t i = 0; i < tetrahedron.indices.size(); i += 3)
    {
        const auto& a = tetrahedron.vertices[tetrahedron.indices[i]];
        const auto& b = tetrahedron.vertices[tetrahedron.indices[i + 1]];
        const auto& c = tetrahedron.vertices[tetrahedron.indices[i + 2]];
        if (glm::dot(glm::cross(b.position - a.position, c.position - a.position), a.position) <= 0.0f)
            return 8;
    }
    return 0;
}
