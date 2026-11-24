#pragma once
#include <filesystem>
#include <vector>
#include <glm/vec4.hpp>

namespace Aether
{
struct HdrImage
{
    unsigned width{}, height{};
    std::vector<glm::vec4> pixels;
};

HdrImage LoadRadiance(const std::filesystem::path &path);
HdrImage LoadStudioEnvironment(bool warm);
HdrImage PanoramaFace(const HdrImage &panorama, unsigned face, unsigned size);
} // namespace Aether
