#include <Assets/ObjImporter.h>

#include <glm/geometric.hpp>

#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>

namespace
{
    bool NearlyEqual(float a, float b, float eps = 1e-4f)
    {
        return std::abs(a - b) <= eps;
    }

    bool NearlyEqual(const glm::vec3& a, const glm::vec3& b, float eps = 1e-4f)
    {
        return glm::length(a - b) <= eps;
    }
}

int main()
{
    // 1. Minimal triangle with position-only faces (auto flat normal & tangent).
    {
        const auto result = Aether::ParseObj(
            "# minimal triangle\n"
            "v 0 0 0\n"
            "v 1 0 0\n"
            "v 0 1 0\n"
            "f 1 2 3\n"
        );
        if (!result.mesh || !result.mesh->IsValid() ||
            result.mesh->vertices.size() != 3 || result.mesh->indices.size() != 3)
            return 1;
        for (const auto& v : result.mesh->vertices)
        {
            if (!NearlyEqual(v.normal, glm::vec3(0, 0, 1)) ||
                !NearlyEqual(v.color, glm::vec3(1, 1, 1)) ||
                !NearlyEqual(glm::length(glm::vec3(v.tangent)), 1.0f) ||
                std::abs(glm::dot(v.normal, glm::vec3(v.tangent))) > 1e-4f)
                return 2;
        }
    }

    // 2. Quad with v/vt/vn, vertex deduplication, fan triangulation, and tangent calculation.
    {
        const auto result = Aether::ParseObj(
            "o Quad\r\n"
            "g Default\r\n"
            "mtllib dummy.mtl\r\n"
            "usemtl Mat0\r\n"
            "s 1\r\n"
            "v 0 0 0 1 0.5 0.25\r\n"
            "v 2 0 0 0.2 0.8 0.4\r\n"
            "v 2 2 0 0.1 0.2 0.9\r\n"
            "v 0 2 0 0.9 0.9 0.1\r\n"
            "vt 0 0\r\n"
            "vt 1 0\r\n"
            "vt 1 1\r\n"
            "vt 0 1\r\n"
            "vn 0 0 1\r\n"
            "f 1/1/1 2/2/1 3/3/1 4/4/1\r\n"
        );
        if (!result.mesh || !result.mesh->IsValid() ||
            result.mesh->vertices.size() != 4 || result.mesh->indices.size() != 6)
            return 3;
        const auto& v0 = result.mesh->vertices[0];
        if (!NearlyEqual(v0.color, glm::vec3(1.0f, 0.5f, 0.25f)) ||
            !NearlyEqual(glm::vec3(v0.tangent), glm::vec3(1, 0, 0)) ||
            !NearlyEqual(v0.tangent.w, 1.0f) ||
            v0.materialUv[0] != v0.uv || v0.materialUv[4] != v0.uv)
            return 4;
    }

    // 3. Negative relative indices and v//vn syntax.
    {
        const auto result = Aether::ParseObj(
            "v 0 0 0\n"
            "v 0 0 1\n"
            "v 0 1 0\n"
            "vn -1 0 0\n"
            "f -3//-1 -2//-1 -1//-1\n"
        );
        if (!result.mesh || !result.mesh->IsValid() || result.mesh->indices.size() != 3)
            return 5;
        if (!NearlyEqual(result.mesh->vertices[0].normal, glm::vec3(-1, 0, 0)))
            return 6;
    }

    // 4. Pentagon fan triangulation (5 vertices -> 3 triangles -> 9 indices) and v/vt syntax.
    {
        const auto result = Aether::ParseObj(
            "v 0 0 0\n"
            "v 1 0 0\n"
            "v 1.5 1 0\n"
            "v 0.5 1.5 0\n"
            "v -0.5 1 0\n"
            "vt 0 0\n"
            "vt 1 0\n"
            "vt 1 1\n"
            "vt 0.5 1\n"
            "vt 0 1\n"
            "f 1/1 2/2 3/3 4/4 5/5\n"
        );
        if (!result.mesh || !result.mesh->IsValid() || result.mesh->indices.size() != 9)
            return 7;
    }

    // 5. Invalid inputs must fail cleanly with diagnostic messages.
    {
        const char* invalidInputs[] = {
            "",
            "# only comments\nv 0 0 0\n",
            "v 0 0 0\nv 1 0 0\nf 1 2\n",
            "v 0 0 0\nv 1 0 0\nv 0 1 0\nf 0 1 2\n",
            "v 0 0 0\nv 1 0 0\nv 0 1 0\nf 1 2 4\n",
            "v 0 0 0\nv 1 0 0\nv 0 1 0\nf -4 -2 -1\n",
            "v nan 0 0\nv 1 0 0\nv 0 1 0\nf 1 2 3\n",
            "v 0 0 0\nv 1 0 0\nv 0 1 0\nf 1/1/1/1 2 3\n"
        };
        for (size_t i = 0; i < std::size(invalidInputs); ++i)
        {
            const auto result = Aether::ParseObj(invalidInputs[i]);
            if (result.mesh.has_value() || result.error.empty())
            {
                std::cerr << "Invalid OBJ case " << i << " unexpectedly succeeded\n";
                return 8;
            }
        }
    }

    // 6. File-based ImportObj round-trip and missing file error.
    {
        const auto tempPath = std::filesystem::temp_directory_path() / "aether_obj_importer_test.obj";
        {
            std::ofstream out(tempPath, std::ios::binary);
            out << "v 0 0 0\nv 1 0 0\nv 0 1 0\nf 1 2 3\n";
        }
        const auto loaded = Aether::ImportObj(tempPath);
        std::filesystem::remove(tempPath);
        if (!loaded.mesh || !loaded.mesh->IsValid() || loaded.mesh->indices.size() != 3)
            return 9;

        const auto missing = Aether::ImportObj(tempPath);
        if (missing.mesh.has_value() || missing.error.empty())
            return 10;
    }

    return 0;
}
