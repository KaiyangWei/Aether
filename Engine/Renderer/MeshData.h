#pragma once

#include <Renderer/Vertex.h>
#include <cstdint>
#include <vector>

namespace Aether
{
    struct MeshData
    {
        std::vector<Vertex> vertices;
        std::vector<uint32_t> indices;

        bool IsValid() const;
    };

    MeshData CreateCubeMesh();
    MeshData CreateTetrahedronMesh();
}
