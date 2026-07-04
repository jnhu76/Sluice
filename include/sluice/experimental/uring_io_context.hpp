// sluice::experimental::UringIoContext — minimal standalone wrapper around
// UringWriteBatch that owns file open/close (RAII) for one write_file_all call.
// NOT a subclass of sluice::IoContext and NOT plugged into BlockingIoContext
// (the spike must not touch default backend selection). See docs/io-uring-spike.md.
#pragma once

#include <sluice/experimental/uring_write_batch.hpp>
#include <sluice/result.hpp>

#include <cstddef>
#include <span>
#include <string_view>

namespace sluice::experimental {

class UringIoContext {
  public:
    explicit UringIoContext(unsigned queue_depth = 64);
    // Open(path, O_WRONLY|O_CREAT|O_TRUNC) -> UringWriteBatch::write_all -> close.
    // Returns backend_error when liburing is unavailable (clean skip).
    Result<UringWriteResult> write_file_all(std::string_view path,
                                            std::span<const std::byte> bytes);

    // Optional measurement (CPPIO-CORE-013E); forwarded to the batch.
    void set_stats(UringStats* stats) { batch_.set_stats(stats); }

  private:
    UringWriteBatch batch_;
};

} // namespace sluice::experimental
