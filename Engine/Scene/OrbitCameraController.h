#pragma once

namespace Aether
{
    class Camera;

    class OrbitCameraController
    {
    public:
        void Orbit(Camera& camera, float yawRadians, float pitchRadians) const;
        void Pan(Camera& camera, float right, float up) const;
        void Zoom(Camera& camera, float steps) const;
        void SetDistanceRange(float minimum, float maximum);

    private:
        float m_minimumDistance = 0.25f;
        float m_maximumDistance = 50.0f;
    };
}
