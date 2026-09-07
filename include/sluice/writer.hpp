#pragma once

#include <sluice/iovec.hpp>
#include <sluice/result.hpp>

#include <cstddef>
#include <span>

namespace sluice {

class Writer {
  public:
    virtual ~Writer() = default;

    virtual Result<std::size_t> write_some(std::span<const std::byte> src) = 0;

    virtual Result<void> flush() = 0;

    Result<void> write_all(std::span<const std::byte> src);

    virtual Result<std::size_t> write_vec(std::span<const ConstIoSlice> srcs);

    Result<void> write_all_vec(std::span<const ConstIoSlice> srcs);
};

} // namespace sluice
