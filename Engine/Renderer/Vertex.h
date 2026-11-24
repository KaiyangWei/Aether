#pragma once

#include <glm/vec2.hpp>
#include <glm/vec3.hpp>
#include <glm/vec4.hpp>

namespace Aether
{
    struct Vertex
    {
        glm::vec3 position;
        glm::vec3 color;
        glm::vec3 normal{ 0.0f };
        glm::vec2 uv{ 0.0f };
        // XYZ is the tangent direction. W is the bitangent handedness.
        glm::vec4 tangent{ 0.0f, 0.0f, 0.0f, 1.0f };
        float colorAlpha = 1.0f;
        // Base color, metallic-roughness, normal, occlusion and emissive.
        glm::vec2 materialUv[5]{};
    };
}
