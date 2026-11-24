#include "TaaPass.h"

#include <algorithm>
#include <cmath>

namespace Aether
{
    float Halton(uint32_t index, uint32_t base)
    {
        if (base < 2u)
            return 0.0f;
        float fraction = 1.0f;
        float result = 0.0f;
        uint32_t i = index;
        while (i > 0u)
        {
            fraction /= static_cast<float>(base);
            result += fraction * static_cast<float>(i % base);
            i /= base;
        }
        return result;
    }

    TaaConfig SanitizeTaaConfig(const TaaConfig& config)
    {
        TaaConfig out = config;
        out.minHistoryWeight = std::isfinite(out.minHistoryWeight)
            ? std::clamp(out.minHistoryWeight, 0.0f, 0.9f) : 0.15f;
        out.maxHistoryWeight = std::clamp(std::isfinite(out.maxHistoryWeight) ? out.maxHistoryWeight : 0.94f,
            out.minHistoryWeight, 0.98f);
        out.historyWeight = std::clamp(std::isfinite(out.historyWeight) ? out.historyWeight : 0.88f,
            out.minHistoryWeight, out.maxHistoryWeight);
        out.motionRejectionScale = std::isfinite(out.motionRejectionScale)
            ? std::clamp(out.motionRejectionScale, 0.0f, 100.0f) : 12.0f;
        out.depthRejectionThreshold = std::isfinite(out.depthRejectionThreshold)
            ? std::clamp(out.depthRejectionThreshold, 1e-5f, 0.5f) : 0.005f;
        out.varianceClampGamma = std::isfinite(out.varianceClampGamma)
            ? std::clamp(out.varianceClampGamma, 0.5f, 3.0f) : 1.25f;
        out.jitterSequenceLength = std::clamp(out.jitterSequenceLength, 2u, 16u);
        return out;
    }

    TaaJitterSample ComputeTaaJitter(
        uint32_t frameIndex,
        uint32_t width,
        uint32_t height,
        uint32_t sequenceLength)
    {
        TaaJitterSample sample{};
        if (width == 0u || height == 0u)
            return sample;

        const uint32_t clampedLen = std::clamp(sequenceLength, 2u, 16u);
        const uint32_t seqIdx = frameIndex % clampedLen;
        // Use 1-based Halton indices [1 .. clampedLen] to avoid (0, 0) bias on index 0.
        const float hx = Halton(seqIdx + 1u, 2u) - 0.5f;
        const float hy = Halton(seqIdx + 1u, 3u) - 0.5f;

        sample.index = seqIdx;
        sample.pixelOffset = glm::vec2(hx, hy);
        sample.ndcOffset = glm::vec2(
            hx * 2.0f / static_cast<float>(width),
            hy * 2.0f / static_cast<float>(height)
        );
        return sample;
    }

    glm::mat4 ApplyProjectionJitter(const glm::mat4& projection, const glm::vec2& ndcOffset)
    {
        if (!std::isfinite(ndcOffset.x) || !std::isfinite(ndcOffset.y))
            return projection;
        glm::mat4 jittered = projection;
        // Right-handed projection has clip.w = -view.z, so subtracting ndcOffset from
        // column 2 shifts post-divide NDC by +ndcOffset in both X and Y.
        jittered[2][0] -= ndcOffset.x;
        jittered[2][1] -= ndcOffset.y;
        return jittered;
    }

    glm::vec2 ComputeMotionVector(
        const glm::mat4& currUnjitteredMvp,
        const glm::mat4& prevUnjitteredMvp,
        const glm::vec3& localPos)
    {
        const glm::vec4 currClip = currUnjitteredMvp * glm::vec4(localPos, 1.0f);
        const glm::vec4 prevClip = prevUnjitteredMvp * glm::vec4(localPos, 1.0f);
        if (!std::isfinite(currClip.w) || !std::isfinite(prevClip.w) ||
            std::abs(currClip.w) <= 1e-6f || std::abs(prevClip.w) <= 1e-6f)
            return glm::vec2(0.0f);

        const glm::vec2 currUv = (glm::vec2(currClip) / currClip.w) * 0.5f + 0.5f;
        const glm::vec2 prevUv = (glm::vec2(prevClip) / prevClip.w) * 0.5f + 0.5f;
        return prevUv - currUv;
    }

    float EvaluateDepthRejection(float currentDepth, float previousDepth, float threshold)
    {
        if (!std::isfinite(currentDepth) || !std::isfinite(previousDepth) ||
            !std::isfinite(threshold) || threshold <= 0.0f)
            return 0.0f;
        const float diff = std::abs(currentDepth - previousDepth);
        return diff <= threshold ? 1.0f : 0.0f;
    }
}
