#pragma once

#include <sluice/measurement.hpp>
#include <sluice/result.hpp>

#include <cstddef>
#include <cstdint>
#include <span>

namespace sluice::experimental {

struct UringWriteResult {
    std::uint64_t submitted = 0;
    std::uint64_t completed = 0;
    std::uint64_t bytes_written = 0;
    std::uint64_t errors = 0;
};

class UringWriteBatch {
  public:
    explicit UringWriteBatch(unsigned queue_depth = 64);
    ~UringWriteBatch();

    UringWriteBatch(const UringWriteBatch&) = delete;
    UringWriteBatch& operator=(const UringWriteBatch&) = delete;
    UringWriteBatch(UringWriteBatch&&) = delete;
    UringWriteBatch& operator=(UringWriteBatch&&) = delete;

    Result<UringWriteResult> write_all(int fd, std::span<const std::byte> bytes,
                                       std::uint64_t file_offset);

    void set_stats(UringStats* stats) { stats_ = stats; }

  private:
    [[maybe_unused]] unsigned queue_depth_;
#if defined(SLUICE_HAS_LIBURING)
    void* ring_ = nullptr;
#else
    [[maybe_unused]] void* ring_ = nullptr;
#endif
    UringStats* stats_ = nullptr;
};

} // namespace sluice::experimental
