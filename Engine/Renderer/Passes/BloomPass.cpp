#include "BloomPass.h"

#include <algorithm>
#include <cmath>

namespace Aether
{
    BloomConfig SanitizeBloomConfig(const BloomConfig& config)
    {
        BloomConfig out = config;
        out.threshold = std::isfinite(out.threshold) ? std::clamp(out.threshold, 0.0f, 64.0f) : 1.0f;
        out.softKnee = std::isfinite(out.softKnee) ? std::clamp(out.softKnee, 0.0f, 1.0f) : 0.5f;
        out.intensity = std::isfinite(out.intensity) ? std::clamp(out.intensity, 0.0f, 4.0f) : 0.25f;
        out.pyramidLevels = std::clamp(out.pyramidLevels, 1u, 8u);
        out.scatter = std::isfinite(out.scatter) ? std::clamp(out.scatter, 0.0f, 1.5f) : 0.7f;
        out.blurRadius = std::isfinite(out.blurRadius) ? std::clamp(out.blurRadius, 0.25f, 3.0f) : 1.0f;
        if (static_cast<uint32_t>(out.debugMode) > static_cast<uint32_t>(BloomDebugMode::Composite))
            out.debugMode = BloomDebugMode::Off;
        out.debugLevel = std::min(out.debugLevel, out.pyramidLevels - 1u);
        return out;
    }

    std::vector<BloomExtent> ComputeBloomPyramidDimensions(
        BloomExtent sceneExtent,
        uint32_t requestedLevels,
        uint32_t maxImageDimension)
    {
        std::vector<BloomExtent> levels;
        if (sceneExtent.width == 0 || sceneExtent.height == 0 || requestedLevels == 0 || maxImageDimension == 0)
            return levels;

        const uint32_t clampedLevels = std::clamp(requestedLevels, 1u, 8u);
        const uint32_t clampedW = std::min(sceneExtent.width, maxImageDimension);
        const uint32_t clampedH = std::min(sceneExtent.height, maxImageDimension);

        uint32_t w = std::max(1u, clampedW / 2u);
        uint32_t h = std::max(1u, clampedH / 2u);
        levels.reserve(clampedLevels);

        for (uint32_t i = 0; i < clampedLevels; ++i)
        {
            levels.push_back({ w, h });
            if (w == 1u && h == 1u)
                break;
            w = std::max(1u, w / 2u);
            h = std::max(1u, h / 2u);
        }
        return levels;
    }
}
