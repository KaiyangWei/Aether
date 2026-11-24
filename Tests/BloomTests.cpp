#include <Renderer/Passes/BloomPass.h>

#include <cmath>
#include <limits>

int main()
{
    // 1. Default config is valid and restrained.
    {
        const Aether::BloomConfig defaults{};
        const auto sanitized = Aether::SanitizeBloomConfig(defaults);
        if (!sanitized.enabled || sanitized.threshold != 1.0f ||
            sanitized.softKnee != 0.5f || sanitized.intensity != 0.25f ||
            sanitized.pyramidLevels != 5u || sanitized.scatter != 0.7f ||
            sanitized.blurRadius != 1.0f)
            return 1;
    }

    // 2. Invalid / non-finite / out-of-range parameters are safely clamped.
    {
        Aether::BloomConfig bad{};
        bad.threshold = -5.0f;
        bad.softKnee = std::numeric_limits<float>::quiet_NaN();
        bad.intensity = 99.0f;
        bad.pyramidLevels = 0;
        bad.scatter = -2.0f;
        bad.blurRadius = std::numeric_limits<float>::infinity();
        bad.debugMode = static_cast<Aether::BloomDebugMode>(99);
        bad.debugLevel = 20;

        const auto clean = Aether::SanitizeBloomConfig(bad);
        if (clean.threshold != 0.0f || clean.softKnee != 0.5f ||
            clean.intensity != 4.0f || clean.pyramidLevels != 1u ||
            clean.scatter != 0.0f || clean.blurRadius != 1.0f ||
            clean.debugMode != Aether::BloomDebugMode::Off ||
            clean.debugLevel != 0u)
            return 2;
    }

    // 3. Standard 1280x720 5-level pyramid.
    {
        const auto levels = Aether::ComputeBloomPyramidDimensions({ 1280, 720 }, 5);
        if (levels.size() != 5 ||
            levels[0] != Aether::BloomExtent{ 640, 360 } ||
            levels[1] != Aether::BloomExtent{ 320, 180 } ||
            levels[2] != Aether::BloomExtent{ 160, 90 } ||
            levels[3] != Aether::BloomExtent{ 80, 45 } ||
            levels[4] != Aether::BloomExtent{ 40, 22 })
            return 3;
    }

    // 4. Odd dimensions never produce zero-sized levels.
    {
        const auto levels = Aether::ComputeBloomPyramidDimensions({ 901, 503 }, 6);
        if (levels.size() != 6 ||
            levels[0] != Aether::BloomExtent{ 450, 251 } ||
            levels[5].width == 0 || levels[5].height == 0)
            return 4;
    }

    // 5. Very small window dimensions stop cleanly at 1x1 without duplicating.
    {
        const auto tiny = Aether::ComputeBloomPyramidDimensions({ 4, 3 }, 6);
        if (tiny.size() != 2 ||
            tiny[0] != Aether::BloomExtent{ 2, 1 } ||
            tiny[1] != Aether::BloomExtent{ 1, 1 })
            return 5;

        const auto one = Aether::ComputeBloomPyramidDimensions({ 1, 1 }, 5);
        if (one.size() != 1 || one[0] != Aether::BloomExtent{ 1, 1 })
            return 6;
    }

    // 6. Zero dimensions or zero levels return empty.
    {
        if (!Aether::ComputeBloomPyramidDimensions({ 0, 720 }, 5).empty() ||
            !Aether::ComputeBloomPyramidDimensions({ 1280, 0 }, 5).empty() ||
            !Aether::ComputeBloomPyramidDimensions({ 1280, 720 }, 0).empty())
            return 7;
    }

    // 7. Device maxImageDimension cap is respected.
    {
        const auto capped = Aether::ComputeBloomPyramidDimensions({ 8192, 4096 }, 4, 2048);
        if (capped.size() != 4 || capped[0] != Aether::BloomExtent{ 1024, 1024 })
            return 8;
    }

    return 0;
}
