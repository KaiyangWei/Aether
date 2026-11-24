#include "OrbitCameraController.h"
#include "Camera.h"

#include <glm/gtc/matrix_transform.hpp>
#include <algorithm>
#include <cmath>

namespace Aether
{
    void OrbitCameraController::Orbit(Camera& camera, float yawRadians, float pitchRadians) const
    {
        const glm::vec3 offset = camera.GetPosition() - camera.GetTarget();
        const glm::vec3 up = glm::normalize(camera.GetUp());
        const glm::vec3 yawed = glm::vec3(glm::rotate(glm::mat4(1.0f), yawRadians, up) * glm::vec4(offset, 0.0f));
        const glm::vec3 right = glm::normalize(glm::cross(up, yawed));
        const float pitch = std::asin(std::clamp(glm::dot(glm::normalize(yawed), up), -1.0f, 1.0f));
        const float limit = glm::radians(89.0f);
        const float delta = std::clamp(pitch + pitchRadians, -limit, limit) - pitch;
        camera.SetPosition(camera.GetTarget() + glm::vec3(glm::rotate(glm::mat4(1.0f), -delta, right) * glm::vec4(yawed, 0.0f)));
    }

    void OrbitCameraController::Pan(Camera& camera, float horizontal, float vertical) const
    {
        const glm::vec3 forward = glm::normalize(camera.GetTarget() - camera.GetPosition());
        const glm::vec3 right = glm::normalize(glm::cross(forward, camera.GetUp()));
        const glm::vec3 up = glm::cross(right, forward);
        const glm::vec3 delta = horizontal * right + vertical * up;
        camera.LookAt(camera.GetPosition() + delta, camera.GetTarget() + delta, camera.GetUp());
    }

    void OrbitCameraController::SetDistanceRange(float minimum, float maximum)
    {
        if (minimum > 0 && maximum >= minimum && std::isfinite(maximum))
        {
            m_minimumDistance = minimum;
            m_maximumDistance = maximum;
        }
    }

    void OrbitCameraController::Zoom(Camera& camera, float steps) const
    {
        const glm::vec3 offset = camera.GetPosition() - camera.GetTarget();
        const float distance = glm::length(offset);
        const float newDistance = std::clamp(distance * std::exp(-0.15f * steps), m_minimumDistance, m_maximumDistance);
        camera.SetPosition(camera.GetTarget() + offset * (newDistance / distance));
    }
}
