#include "HdrImage.h"
#include <glm/glm.hpp>
#include <algorithm>
#include <array>
#include <cmath>
#include <fstream>
#include <sstream>
#include <stdexcept>

namespace Aether
{
HdrImage LoadRadiance(const std::filesystem::path &path)
{
    std::ifstream in(path, std::ios::binary);
    auto line = [&] {
        std::string s;
        if (!std::getline(in, s))
            throw std::runtime_error("Truncated Radiance header");
        if (!s.empty() && s.back() == '\r')
            s.pop_back();
        return s;
    };
    const auto magic = line();
    if (magic != "#?RADIANCE" && magic != "#?RGBE")
        throw std::runtime_error("Expected Radiance RGBE");
    bool rgbe = false;
    for (auto s = line(); !s.empty(); s = line())
        rgbe |= s == "FORMAT=32-bit_rle_rgbe";
    std::istringstream resolution(line());
    std::string yAxis, xAxis;
    HdrImage image;
    if (!rgbe || !(resolution >> yAxis >> image.height >> xAxis >> image.width) ||
        (yAxis != "-Y" && yAxis != "+Y") || (xAxis != "+X" && xAxis != "-X") || !image.width || !image.height ||
        uint64_t(image.width) * image.height > (1ull << 26))
        throw std::runtime_error("Unsupported Radiance format or dimensions");
    image.pixels.resize(size_t(image.width) * image.height);
    auto byte = [&]() -> unsigned char {
        int c = in.get();
        if (c < 0)
            throw std::runtime_error("Truncated RGBE pixels");
        return static_cast<unsigned char>(c);
    };
    std::vector<std::array<unsigned char, 4>> row(image.width);
    for (unsigned y = 0; y < image.height; ++y)
    {
        row[0] = {byte(), byte(), byte(), byte()};
        if (image.width >= 8 && image.width <= 32767 && row[0][0] == 2 && row[0][1] == 2 && row[0][2] < 128)
        {
            if (unsigned(row[0][2]) * 256 + row[0][3] != image.width)
                throw std::runtime_error("Invalid RGBE scanline");
            for (unsigned ch = 0; ch < 4; ++ch)
                for (unsigned x = 0; x < image.width;)
                {
                    const auto code = byte();
                    const unsigned n = code > 128 ? code - 128 : code;
                    if (!n || n > image.width - x)
                        throw std::runtime_error("Invalid RGBE run");
                    const unsigned char value = code > 128 ? byte() : static_cast<unsigned char>(0);
                    for (unsigned i = 0; i < n; ++i)
                        row[x++][ch] = code > 128 ? value : byte();
                }
        }
        else
        {
            auto pixel = row.front();
            unsigned x = 0, shift = 0;
            while (x < image.width)
            {
                if (pixel[0] == 1 && pixel[1] == 1 && pixel[2] == 1)
                {
                    if (!x || shift >= 32)
                        throw std::runtime_error("Invalid legacy RGBE repeat");
                    const uint64_t count = uint64_t(pixel[3]) << shift;
                    if (count > image.width - x)
                        throw std::runtime_error("RGBE repeat exceeds scanline");
                    const auto previous = row[x - 1];
                    for (uint64_t i = 0; i < count; ++i)
                        row[x++] = previous;
                    shift += 8;
                }
                else
                {
                    row[x++] = pixel;
                    shift = 0;
                }
                if (x < image.width)
                    pixel = {byte(), byte(), byte(), byte()};
            }
        }
        for (unsigned x = 0; x < image.width; ++x)
        {
            const auto &p = row[x];

            const float k = p[3] ? std::ldexp(1.0f, int(p[3]) - 136) : 0;
            image.pixels[size_t(yAxis == "-Y" ? y : image.height - 1 - y) * image.width +
                         (xAxis == "+X" ? x : image.width - 1 - x)] = {k * p[0], k * p[1], k * p[2], 1};
        }
    }
    return image;
}

HdrImage LoadStudioEnvironment(bool warm)
{
    return LoadRadiance(std::filesystem::path(AETHER_ENVIRONMENT_DIR) / (warm ? "sunset.hdr" : "studio.hdr"));
}

HdrImage PanoramaFace(const HdrImage &src, unsigned face, unsigned size)
{
    HdrImage out{size, size};
    for (unsigned y = 0; y < size; ++y)
        for (unsigned x = 0; x < size; ++x)
        {
            const float u = 2 * (x + .5f) / size - 1, v = 2 * (y + .5f) / size - 1;
            const glm::vec3 dirs[] = {{1, -v, -u}, {-1, -v, u}, {u, 1, v}, {u, -1, -v}, {u, -v, 1}, {-u, -v, -1}};
            const auto d = glm::normalize(dirs[face]);
            const float fx = (std::atan2(d.z, d.x) / 6.2831853f + .5f) * src.width - .5f;
            const float fy = std::acos(std::clamp(d.y, -1.f, 1.f)) / 3.14159265f * src.height - .5f;
            const int ix = int(std::floor(fx)), iy = int(std::floor(fy));
            auto sample = [&](int i, int j) {
                return src.pixels[size_t(std::clamp(j, 0, int(src.height) - 1)) * src.width +
                                  (i + int(src.width)) % int(src.width)];
            };
            out.pixels.push_back(glm::mix(glm::mix(sample(ix, iy), sample(ix + 1, iy), fx - ix),
                                          glm::mix(sample(ix, iy + 1), sample(ix + 1, iy + 1), fx - ix), fy - iy));
        }
    return out;
}
} // namespace Aether
