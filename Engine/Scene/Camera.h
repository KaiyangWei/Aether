#pragma once

#include <glm/glm.hpp>

namespace Aether
{
    class Camera
    {
    public:
        Camera();
        Camera(float fovDegrees, float aspectRatio, float nearZ = 0.1f, float farZ = 100.0f);
        ~Camera() = default;

        void LookAt(
            const glm::vec3& eye,
            const glm::vec3& target,
            const glm::vec3& up = glm::vec3(0.0f, 1.0f, 0.0f)
        );

        void SetPosition(const glm::vec3& position);
        void SetTarget(const glm::vec3& target);
        void SetAspectRatio(float aspectRatio);
        void SetFov(float fovDegrees);
        void SetClipPlanes(float nearZ, float farZ);

        const glm::mat4& GetViewMatrix() const;
        const glm::mat4& GetProjectionMatrix() const;
        const glm::mat4& GetViewProjectionMatrix() const;

        const glm::vec3& GetPosition() const { return m_position; }
        const glm::vec3& GetTarget() const { return m_target; }
        glm::vec3 GetForward() const { return glm::normalize(m_target - m_position); }
        const glm::vec3& GetUp() const { return m_up; }

    private:
        void UpdateViewMatrix() const;
        void UpdateProjectionMatrix() const;

    private:
        glm::vec3 m_position{ 1.8f, 1.4f, 2.2f };
        glm::vec3 m_target{ 0.0f, 0.0f, 0.0f };
        glm::vec3 m_up{ 0.0f, 1.0f, 0.0f };

        float m_fov = 45.0f; // 角度制
        float m_aspectRatio = 16.0f / 9.0f;
        float m_nearZ = 0.1f;
        float m_farZ = 100.0f;

        mutable glm::mat4 m_viewMatrix{ 1.0f };
        mutable glm::mat4 m_projectionMatrix{ 1.0f };
        mutable glm::mat4 m_viewProjectionMatrix{ 1.0f };

        mutable bool m_viewDirty = true;
        mutable bool m_projectionDirty = true;
    };
}