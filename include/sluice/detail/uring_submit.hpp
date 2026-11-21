#pragma once

#include <algorithm>
#include <cstddef>
#include <limits>

namespace sluice::detail {

inline unsigned uring_chunk_length(std::size_t remaining) noexcept {
    constexpr auto native_max = static_cast<std::size_t>(std::numeric_limits<unsigned>::max());
    return static_cast<unsigned>(std::min(remaining, native_max));
}

}
