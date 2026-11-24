#include "Camera.h"

#include <glm/gtc/matrix_transform.hpp>
#include <cmath>

namespace Aether
{
    Camera::Camera()
    {
        UpdateViewMatrix();
        UpdateProjectionMatrix();
    }

    Camera::Camera(float fovDegrees, float aspectRatio, float nearZ, float farZ)
    {
        SetFov(fovDegrees);
        SetAspectRatio(aspectRatio);
        SetClipPlanes(nearZ, farZ);
        UpdateViewMatrix();
        UpdateProjectionMatrix();
    }

    void Camera::LookAt(const glm::vec3& eye, const glm::vec3& target, const glm::vec3& up)
    {
        const glm::vec3 direction = target - eye;
        const float distance = glm::length(direction);
        const float upLength = glm::length(up);
        if (!std::isfinite(distance) || distance <= 1e-6f ||
            !std::isfinite(upLength) || upLength <= 1e-6f)
            return;

        const glm::vec3 normalizedUp = up / upLength;
        if (glm::length(glm::cross(direction / distance, normalizedUp)) <= 1e-4f)
            return;

        m_position = eye;
        m_target = target;
        m_up = normalizedUp;
        m_viewDirty = true;
    }

    void Camera::SetPosition(const glm::vec3& position)
    {
        LookAt(position, m_target, m_up);
    }

    void Camera::SetTarget(const glm::vec3& target)
    {
        LookAt(m_position, target, m_up);
    }

    void Camera::SetAspectRatio(float aspectRatio)
    {
        if (m_aspectRatio != aspectRatio && std::isfinite(aspectRatio) && aspectRatio >= 1e-6f)
        {
            m_aspectRatio = aspectRatio;
            m_projectionDirty = true;
        }
    }

    void Camera::SetFov(float fovDegrees)
    {
        if (m_fov != fovDegrees && fovDegrees >= 1.0f && fovDegrees <= 179.0f)
        {
            m_fov = fovDegrees;
            m_projectionDirty = true;
        }
    }

    void Camera::SetClipPlanes(float nearZ, float farZ)
    {
        if (nearZ > 0.0f && farZ > nearZ && std::isfinite(farZ))
        {
            m_nearZ = nearZ;
            m_farZ = farZ;
            m_projectionDirty = true;
        }
    }

    const glm::mat4& Camera::GetViewMatrix() const
    {
        UpdateViewMatrix();
        return m_viewMatrix;
    }

    const glm::mat4& Camera::GetProjectionMatrix() const
    {
        UpdateProjectionMatrix();
        return m_projectionMatrix;
    }

    const glm::mat4& Camera::GetViewProjectionMatrix() const
    {
        m_viewProjectionMatrix = GetProjectionMatrix() * GetViewMatrix();
        return m_viewProjectionMatrix;
    }

    void Camera::UpdateViewMatrix() const
    {
        if (!m_viewDirty)
            return;

        m_viewMatrix = glm::lookAtRH(m_position, m_target, m_up);
        m_viewDirty = false;
    }

    void Camera::UpdateProjectionMatrix() const
    {
        if (!m_projectionDirty)
            return;

        m_projectionMatrix = glm::perspectiveRH_ZO(
            glm::radians(m_fov),
            m_aspectRatio,
            m_nearZ,
            m_farZ
        );

        // Renderer uses a positive-height Vulkan viewport. Flip Y here once.
        m_projectionMatrix[1][1] *= -1.0f;

        m_projectionDirty = false;
    }
}