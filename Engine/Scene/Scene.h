#pragma once

#include <Scene/Transform.h>

#include <cstddef>
#include <optional>
#include <span>
#include <vector>

namespace Aether
{
    // Index into the mesh resources supplied by the application.
    using MeshId = std::size_t;

    struct MeshInstance
    {
        MeshId meshId = 0;
    };

    struct SceneObject
    {
        Transform transform;
        MeshInstance mesh;
        std::optional<glm::mat4> modelMatrix{};
        std::optional<glm::mat4> previousModelMatrix{};
        bool doubleSided = false;
        std::optional<size_t> material{};

        glm::mat4 GetModelMatrix() const { return modelMatrix ? *modelMatrix : transform.GetMatrix(); }
    };

    class Scene
    {
    public:
        void AddObject(SceneObject object) { m_objects.push_back(object); }
        void AddObjects(std::span<const SceneObject> objects)
        {
            m_objects.insert(m_objects.end(), objects.begin(), objects.end());
        }

        // Removal shifts later indices and invalidates spans and object pointers.
        bool RemoveObject(std::size_t index)
        {
            if (index >= m_objects.size())
                return false;
            m_objects.erase(m_objects.begin() + static_cast<std::ptrdiff_t>(index));
            return true;
        }

        // Lookups are borrowed. Reacquire them after structural changes.
        SceneObject* GetObject(std::size_t index)
        {
            return index < m_objects.size() ? &m_objects[index] : nullptr;
        }

        const SceneObject* GetObject(std::size_t index) const
        {
            return index < m_objects.size() ? &m_objects[index] : nullptr;
        }

        std::span<SceneObject> GetObjects() { return m_objects; }
        std::span<const SceneObject> GetObjects() const { return m_objects; }
        void Clear() { m_objects.clear(); }

    private:
        std::vector<SceneObject> m_objects;
    };
}
