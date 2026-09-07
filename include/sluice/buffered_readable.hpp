










#pragma once

#include <sluice/result.hpp>

#include <cstddef>
#include <span>

namespace sluice {

class BufferedReadable {
  public:
    virtual ~BufferedReadable() = default;




    virtual std::span<const std::byte> peek_buffered() const = 0;




    virtual Result<void> consume_buffered(std::size_t n) = 0;
};

}
