#include <Scene/Camera.h>
#include <Scene/OrbitCameraController.h>

#include <cmath>
#include <iostream>

int main()
{
    Aether::Camera camera(45.0f, 2.0f, 0.1f, 100.0f);
    camera.LookAt({0, 0, 5}, {0, 0, 0});
    const auto projection = camera.GetProjectionMatrix();
    const auto near = projection * glm::vec4(0, 0, -0.1f, 1);
    const auto far = projection * glm::vec4(0, 0, -100.0f, 1);
    if (std::abs(near.z / near.w) > 1e-5f ||
        std::abs(far.z / far.w - 1.0f) > 1e-5f || projection[1][1] >= 0)
        return 1;

    const auto view = camera.GetViewMatrix();
    if (glm::length(glm::vec3(view * glm::vec4(camera.GetPosition(), 1))) > 1e-5f ||
        camera.GetViewProjectionMatrix() != projection * view)
        return 2;

    camera.SetAspectRatio(1.0f);
    if (std::abs(camera.GetProjectionMatrix()[0][0] - 2 * projection[0][0]) > 1e-5f ||
        camera.GetViewProjectionMatrix() != camera.GetProjectionMatrix() * view)
        return 3;

    Aether::OrbitCameraController controller;
    controller.Orbit(camera, 0.5f, 100.0f);
    if (std::abs(glm::distance(camera.GetPosition(), camera.GetTarget()) - 5.0f) > 1e-5f ||
        !std::isfinite(camera.GetViewMatrix()[0][0]) ||
        std::abs(camera.GetPosition().y) >= 5.0f)
        return 4;

    const auto offset = camera.GetPosition() - camera.GetTarget();
    controller.Pan(camera, 1.0f, 0.5f);
    if (glm::length((camera.GetPosition() - camera.GetTarget()) - offset) > 1e-5f ||
        glm::length(camera.GetTarget()) < 1.0f)
        return 5;

    controller.Zoom(camera, 1000.0f);
    if (std::abs(glm::distance(camera.GetPosition(), camera.GetTarget()) - 0.25f) > 1e-5f)
        return 6;

    controller.Zoom(camera, -1000.0f);
    if (std::abs(glm::distance(camera.GetPosition(), camera.GetTarget()) - 50.0f) > 1e-4f)
        return 7;

    const auto validView = camera.GetViewMatrix();
    const auto validProjection = camera.GetProjectionMatrix();
    camera.LookAt({0, 0, 0}, {0, 0, 0});
    camera.LookAt({0, 5, 0}, {0, 0, 0});
    camera.LookAt({0, 0, 5}, {0, 0, 0}, {0, 0, 0});
    camera.SetPosition(camera.GetTarget());
    camera.SetAspectRatio(0.0f);
    camera.SetFov(180.0f);
    camera.SetClipPlanes(1.0f, 0.1f);
    if (camera.GetViewMatrix() != validView || camera.GetProjectionMatrix() != validProjection)
        return 8;

    const auto forward = camera.GetViewMatrix() * glm::vec4(camera.GetForward(), 0.0f);
    if (glm::length(glm::vec3(forward) - glm::vec3(0, 0, -1)) > 1e-5f)
        return 9;

    return 0;
}
