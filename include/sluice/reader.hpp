#pragma once

#include <sluice/iovec.hpp>
#include <sluice/limit.hpp>
#include <sluice/measurement.hpp>
#include <sluice/result.hpp>

#include <cstddef>
#include <cstdint>
#include <span>

namespace sluice {

class Writer;

class Reader {
  public:
    virtual ~Reader() = default;

    virtual Result<std::size_t> read_some(std::span<std::byte> dst) = 0;

    Result<void> read_exact(std::span<std::byte> dst);

    Result<std::size_t> stream_to(Writer& writer);

    Result<std::uint64_t> stream_to(Writer& writer, std::span<std::byte> scratch, CopyLimit limit,
                                    CopyStats* stats = nullptr);

    Result<std::uint64_t> stream_to(Writer& writer, CopyLimit limit);

    virtual Result<std::size_t> read_vec(std::span<IoSlice> dsts);

    Result<void> read_vec_all(std::span<IoSlice> dsts);
};

} // namespace sluice
