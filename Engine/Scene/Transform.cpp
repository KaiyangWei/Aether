#include "Transform.h"

#include <glm/gtc/matrix_transform.hpp>

glm::mat4 Aether::Transform::GetMatrix() const
{
    return glm::translate(glm::mat4(1.0f), position) *
        glm::mat4_cast(rotation) * glm::scale(glm::mat4(1.0f), scale);
}
