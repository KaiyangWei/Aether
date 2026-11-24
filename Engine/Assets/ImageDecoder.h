#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace Aether
{
    struct DecodedImage
    {
        uint32_t width = 0, height = 0;
        std::vector<std::byte> rgba;
    };

    // Throws a diagnostic exception when the encoded data cannot be decoded.
    DecodedImage DecodeImage(std::span<const std::byte> encoded);
}
