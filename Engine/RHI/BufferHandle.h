#pragma once

#include <cstdint>
#include <limits>

namespace Aether {

    struct BufferHandle {
        static constexpr uint32_t INVALID_INDEX = std::numeric_limits<uint32_t>::max();

        uint32_t index = INVALID_INDEX;
        uint32_t generation = 0;

        bool IsValid() const {
            return index != INVALID_INDEX;
        }

        bool operator==(const BufferHandle& other) const {
            return index == other.index && generation == other.generation;
        }

        bool operator!=(const BufferHandle& other) const {
            return !(*this == other);
        }
    };

}