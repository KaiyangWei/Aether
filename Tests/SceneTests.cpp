#include <Scene/Scene.h>

#include <glm/geometric.hpp>

int main()
{
    Aether::Scene scene;
    Aether::Transform transform;
    transform.position = { 5, 6, 7 };
    transform.rotation = glm::angleAxis(glm::radians(90.0f), glm::vec3(0, 0, 1));
    transform.scale = { 2, 3, 4 };
    scene.AddObject({ transform, { 0 } });
    scene.AddObject({ {}, { 0 } });

    auto objects = scene.GetObjects();
    if (objects.size() != 2 || objects[0].mesh.meshId != objects[1].mesh.meshId)
        return 1;
    const auto world = glm::vec3(objects[0].transform.GetMatrix() * glm::vec4(1, 1, 1, 1));
    if (glm::length(world - glm::vec3(2, 8, 11)) > 1e-5f)
        return 2;
    objects[0].transform.position.x += 1.0f;
    const Aether::Scene& view = scene;
    if (view.GetObjects()[1].transform.GetMatrix() != glm::mat4(1.0f))
        return 3;
    if (scene.RemoveObject(2) || view.GetObject(2) || !scene.RemoveObject(0) ||
        scene.GetObjects().size() != 1 || !scene.GetObject(0) ||
        view.GetObject(0)->transform.GetMatrix() != glm::mat4(1.0f) ||
        view.GetObject(0)->mesh.meshId != 0)
        return 5;
    scene.Clear();
    return scene.GetObjects().empty() ? 0 : 4;
}
