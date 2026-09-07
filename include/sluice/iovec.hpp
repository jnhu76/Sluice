









#pragma once

#include <cstddef>
#include <span>

namespace sluice {


struct IoSlice {
    std::span<std::byte> bytes;
};


struct ConstIoSlice {
    std::span<const std::byte> bytes;
};

}
