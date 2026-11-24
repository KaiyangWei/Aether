#pragma once

#include <glm/mat4x4.hpp>
#include <glm/vec2.hpp>
#include <glm/vec3.hpp>
#include <cstdint>

namespace Aether
{
    struct TaaConfig
    {
        bool enabled = true;
        float historyWeight = 0.88f;
        float maxHistoryWeight = 0.94f;
        float minHistoryWeight = 0.15f;
        float motionRejectionScale = 12.0f;
        float depthRejectionThreshold = 0.005f;
        float varianceClampGamma = 1.25f;
        uint32_t jitterSequenceLength = 8;
        bool resetHistory = false;
    };

    struct TaaJitterSample
    {
        glm::vec2 pixelOffset{ 0.0f }; // In (-0.5, +0.5) pixels.
        glm::vec2 ndcOffset{ 0.0f };   // In (-1/W, +1/W) x (-1/H, +1/H) Vulkan NDC.
        uint32_t index = 0;
    };

    // Deterministic Halton radical inverse in [0, 1) for 1-based sequence index.
    float Halton(uint32_t index, uint32_t base);

    // Sanitizes and clamps TAA configuration values to safe finite ranges.
    TaaConfig SanitizeTaaConfig(const TaaConfig& config);

    // Computes Halton(2, 3) subpixel & Vulkan NDC jitter for a frame index and render resolution.
    TaaJitterSample ComputeTaaJitter(
        uint32_t frameIndex,
        uint32_t width,
        uint32_t height,
        uint32_t sequenceLength = 8
    );

    // Applies Vulkan NDC jitter to Aether's right-handed, [0, 1]-depth, Y-flipped projection matrix.
    // Does not mutate the input projection matrix.
    glm::mat4 ApplyProjectionJitter(const glm::mat4& projection, const glm::vec2& ndcOffset);

    // Computes screen-space motion vector using Aether's convention: velocity = prevUV - currUV.
    glm::vec2 ComputeMotionVector(
        const glm::mat4& currUnjitteredMvp,
        const glm::mat4& prevUnjitteredMvp,
        const glm::vec3& localPos
    );

    // Evaluates depth disocclusion rejection weight in [0, 1] (0 = rejected, 1 = accepted).
    float EvaluateDepthRejection(float currentDepth, float previousDepth, float threshold);
}
