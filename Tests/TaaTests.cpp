#include <Renderer/Passes/TaaPass.h>
#include <Scene/Camera.h>

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/matrix_inverse.hpp>
#include <cmath>
#include <limits>

int main()
{
    // 1. Halton sequence correctness (base 2 and base 3).
    {
        const float expectedBase2[] = { 0.5f, 0.25f, 0.75f, 0.125f, 0.625f, 0.375f, 0.875f, 0.0625f };
        const float expectedBase3[] = { 1.0f / 3.0f, 2.0f / 3.0f, 1.0f / 9.0f, 4.0f / 9.0f };
        for (uint32_t i = 0; i < 8; ++i)
            if (std::abs(Aether::Halton(i + 1, 2) - expectedBase2[i]) > 1e-6f)
                return 1;
        for (uint32_t i = 0; i < 4; ++i)
            if (std::abs(Aether::Halton(i + 1, 3) - expectedBase3[i]) > 1e-6f)
                return 2;
        if (Aether::Halton(1, 0) != 0.0f || Aether::Halton(1, 1) != 0.0f)
            return 3;
    }

    // 2. Pixel-to-NDC conversion, jitter bounds, and sequence wrapping.
    {
        const uint32_t w = 1280, h = 720;
        const auto s0 = Aether::ComputeTaaJitter(0, w, h, 8);
        const auto s8 = Aether::ComputeTaaJitter(8, w, h, 8);
        if (s0.index != 0 || s8.index != 0 || s0.ndcOffset != s8.ndcOffset)
            return 4;

        for (uint32_t f = 0; f < 8; ++f)
        {
            const auto s = Aether::ComputeTaaJitter(f, w, h, 8);
            if (std::abs(s.pixelOffset.x) > 0.5f || std::abs(s.pixelOffset.y) > 0.5f ||
                std::abs(s.ndcOffset.x) > 1.0f / float(w) + 1e-7f ||
                std::abs(s.ndcOffset.y) > 1.0f / float(h) + 1e-7f)
                return 5;
        }

        // Zero resolution must fail safely to zero jitter.
        const auto zero = Aether::ComputeTaaJitter(3, 0, 720, 8);
        if (zero.ndcOffset != glm::vec2(0.0f))
            return 6;
    }

    // 3. Projection jitter sign & Vulkan right-handed Y-flipped convention check.
    {
        Aether::Camera camera(45.0f, 16.0f / 9.0f, 0.1f, 100.0f);
        camera.LookAt({ 0.0f, 0.0f, 5.0f }, { 0.0f, 0.0f, 0.0f });
        const glm::mat4 view = camera.GetViewMatrix();
        const glm::mat4 proj = camera.GetProjectionMatrix();
        const glm::vec2 offset(0.0015f, -0.0025f);
        const glm::mat4 jittered = Aether::ApplyProjectionJitter(proj, offset);

        const glm::vec4 pBase = proj * view * glm::vec4(0.0f, 0.0f, 0.0f, 1.0f);
        const glm::vec4 pJit = jittered * view * glm::vec4(0.0f, 0.0f, 0.0f, 1.0f);
        const glm::vec2 ndcBase = glm::vec2(pBase) / pBase.w;
        const glm::vec2 ndcJit = glm::vec2(pJit) / pJit.w;
        if (glm::length((ndcJit - ndcBase) - offset) > 1e-5f)
            return 7;

        // NaN jitter must leave projection untouched.
        const glm::mat4 nanJit = Aether::ApplyProjectionJitter(proj, { std::numeric_limits<float>::quiet_NaN(), 0.0f });
        if (nanJit != proj)
            return 8;
    }

    // 4. Motion vector convention: velocity = prevUV - currUV.
    {
        Aether::Camera camera(45.0f, 16.0f / 9.0f, 0.1f, 100.0f);
        camera.LookAt({ 0.0f, 0.0f, 5.0f }, { 0.0f, 0.0f, 0.0f });
        const glm::mat4 vp = camera.GetViewProjectionMatrix();
        const glm::mat4 prevModel = glm::mat4(1.0f);
        const glm::mat4 currModel = glm::translate(glm::mat4(1.0f), glm::vec3(1.0f, 0.0f, 0.0f));

        // Static object -> zero velocity.
        const glm::vec2 vStatic = Aether::ComputeMotionVector(vp * prevModel, vp * prevModel, glm::vec3(0.0f));
        if (glm::length(vStatic) > 1e-6f)
            return 9;

        // Object moved +X (right on screen): currUV.x > prevUV.x, so velocity.x = prevUV.x - currUV.x < 0.
        const glm::vec2 vRight = Aether::ComputeMotionVector(vp * currModel, vp * prevModel, glm::vec3(0.0f));
        if (!(vRight.x < -0.05f) || std::abs(vRight.y) > 1e-5f)
            return 10;

        // Reprojection: currUV + velocity == prevUV.
        const glm::vec4 cCurr = vp * currModel * glm::vec4(0.0f, 0.0f, 0.0f, 1.0f);
        const glm::vec4 cPrev = vp * prevModel * glm::vec4(0.0f, 0.0f, 0.0f, 1.0f);
        const glm::vec2 currUv = (glm::vec2(cCurr) / cCurr.w) * 0.5f + 0.5f;
        const glm::vec2 prevUv = (glm::vec2(cPrev) / cPrev.w) * 0.5f + 0.5f;
        if (glm::length((currUv + vRight) - prevUv) > 1e-5f)
            return 11;
    }

    // 5. Depth rejection & config sanitization.
    {
        if (Aether::EvaluateDepthRejection(0.5f, 0.502f, 0.005f) != 1.0f ||
            Aether::EvaluateDepthRejection(0.5f, 0.52f, 0.005f) != 0.0f ||
            Aether::EvaluateDepthRejection(std::numeric_limits<float>::quiet_NaN(), 0.5f, 0.005f) != 0.0f)
            return 12;

        Aether::TaaConfig bad{};
        bad.minHistoryWeight = -1.0f;
        bad.maxHistoryWeight = 2.0f;
        bad.historyWeight = std::numeric_limits<float>::infinity();
        bad.depthRejectionThreshold = -0.1f;
        bad.varianceClampGamma = 0.0f;
        bad.jitterSequenceLength = 1;
        bad.resetHistory = true;
        const auto clean = Aether::SanitizeTaaConfig(bad);
        if (clean.minHistoryWeight != 0.0f || clean.maxHistoryWeight != 0.98f ||
            clean.historyWeight < clean.minHistoryWeight || clean.historyWeight > clean.maxHistoryWeight ||
            clean.depthRejectionThreshold < 1e-5f || clean.varianceClampGamma != 0.5f ||
            clean.jitterSequenceLength != 2u || !clean.resetHistory)
            return 13;

        bad.minHistoryWeight = 0.9f;
        bad.maxHistoryWeight = 0.91f;
        if (Aether::SanitizeTaaConfig(bad).historyWeight != 0.9f)
            return 14;
    }

    // Reproject the same point over all Halton phases, including the wrap.
    {
        Aether::Camera previous(45.f, 16.f / 9.f, 0.1f, 100.f);
        previous.LookAt({0, 0, 5}, {0, 0, 0});
        const glm::vec4 point(0.19f, -0.15f, 0.31f, 1.f);
        const auto uv = [](const glm::vec4& clip) {
            return glm::vec2(clip) / clip.w * 0.5f + 0.5f;
        };
        for (uint32_t phase = 0; phase < 8; ++phase)
        {
            const auto oldJitter = Aether::ComputeTaaJitter(phase, 1280, 720).ndcOffset;
            const auto newJitter = Aether::ComputeTaaJitter(phase + 1, 1280, 720).ndcOffset;
            const auto oldProjection = Aether::ApplyProjectionJitter(previous.GetProjectionMatrix(), oldJitter);
            for (unsigned motion = 0; motion < 5; ++motion)
            {
                auto current = previous;
                if (motion == 2 || motion == 3)
                    current.LookAt({0.45f, 0.1f, 5}, {0.45f, 0.1f, 0});
                if (motion == 4)
                    current.LookAt(previous.GetPosition(), {0.3f, 0.15f, 0});
                const auto model = glm::translate(glm::mat4(1), motion == 1 || motion == 3
                    ? glm::vec3(0.2f, 0.35f, -0.75f) : glm::vec3(0));
                const auto newProjection = Aether::ApplyProjectionJitter(current.GetProjectionMatrix(), newJitter);
                const auto oldUv = uv(oldProjection * previous.GetViewMatrix() * point);
                const auto newUv = uv(newProjection * current.GetViewMatrix() * model * point);
                const auto geom = Aether::ComputeMotionVector(
                    current.GetProjectionMatrix() * current.GetViewMatrix() * model,
                    previous.GetProjectionMatrix() * previous.GetViewMatrix(), glm::vec3(point));
                if (glm::length(oldUv - newUv - geom - (oldJitter - newJitter) * 0.5f) > 2e-6f)
                    return 15;

                const glm::vec4 worldRay(0.1f, 0.08f, -1.0f, 0.f);
                const glm::mat4 oldRot{glm::mat3(previous.GetViewMatrix())};
                const glm::mat4 newRot{glm::mat3(current.GetViewMatrix())};
                const auto skyUv = uv(newProjection * newRot * worldRay);
                const auto skyTransform = oldProjection * oldRot * glm::inverse(newProjection * newRot);
                const auto oldSkyUv = uv(oldProjection * oldRot * worldRay);
                if (glm::length(uv(skyTransform * glm::vec4(skyUv * 2.f - 1.f, 1.f, 1.f)) - oldSkyUv) > 2e-6f)
                    return 16;
                if (motion != 4 && glm::length(oldSkyUv - skyUv - (oldJitter - newJitter) * 0.5f) > 2e-6f)
                    return 17;
            }
        }
    }
    return 0;
}
