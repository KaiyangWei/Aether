#pragma once

#include <Assets/GltfImporter.h>
#include <Renderer/RtLighting.h>

#include <glm/vec3.hpp>

#include <cstddef>
#include <vector>

namespace Aether
{
    struct CornellBoxCameraPreset
    {
        glm::vec3 eye{ 0.0f, 2.20f, 6.15f };
        glm::vec3 target{ 0.0f, 1.85f, -0.55f };
        glm::vec3 up{ 0.0f, 1.0f, 0.0f };
        float fovDegrees = 45.0f;
        float nearZ = 0.1f;
        float farZ = 100.0f;
    };

    struct CornellBoxValidationSummary
    {
        size_t meshCount = 0;
        size_t nodeCount = 0;
        size_t materialCount = 0;
        size_t emissiveMaterialCount = 0;
        size_t mirrorLikeMaterialCount = 0;
        size_t roughDielectricMaterialCount = 0;
        size_t midRoughMetallicMaterialCount = 0;
        size_t roughMetallicMaterialCount = 0;
        bool allMeshesValid = false;
    };

    CornellBoxCameraPreset GetDefaultCornellBoxCamera();

    // Explicit physical area & accent lights associated with the Cornell Box emissive fixtures.
    std::vector<RtGpuLight> GetCornellBoxLights();

    // Builds Aether's standard built-in stylized Cornell Box validation room.
    // Self-contained procedural PBR geometry and materials (zero external file dependencies).
    ImportedAsset MakeCornellBoxScene();

    CornellBoxValidationSummary InspectCornellBoxAsset(const ImportedAsset& asset);
}
