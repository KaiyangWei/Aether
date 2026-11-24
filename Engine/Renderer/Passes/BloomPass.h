#pragma once

#include <cstdint>
#include <vector>

namespace Aether
{
    enum class BloomDebugMode : uint32_t
    {
        Off = 0,
        BrightPass = 1,
        DownsampleLevel = 2,
        ReconstructedBloom = 3,
        Composite = 4
    };

    struct BloomExtent
    {
        uint32_t width = 0;
        uint32_t height = 0;

        bool operator==(const BloomExtent&) const = default;
    };

    struct BloomConfig
    {
        bool enabled = true;
        float threshold = 1.0f;
        float softKnee = 0.5f;
        float intensity = 0.25f;
        uint32_t pyramidLevels = 5;
        float scatter = 0.7f;
        float blurRadius = 1.0f;
        BloomDebugMode debugMode = BloomDebugMode::Off;
        uint32_t debugLevel = 0;
    };

    // Clamps and sanitizes all Bloom parameters to safe finite ranges.
    BloomConfig SanitizeBloomConfig(const BloomConfig& config);

    // Computes multi-resolution Bloom pyramid dimensions starting at half scene resolution,
    // safely handling odd/small dimensions and device image limits.
    std::vector<BloomExtent> ComputeBloomPyramidDimensions(
        BloomExtent sceneExtent,
        uint32_t requestedLevels,
        uint32_t maxImageDimension = 16384u
    );
}
