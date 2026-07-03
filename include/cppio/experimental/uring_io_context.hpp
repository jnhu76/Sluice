// cppio::experimental::UringIoContext — minimal standalone wrapper around
// UringWriteBatch that owns file open/close (RAII) for one write_file_all call.
// NOT a subclass of cppio::IoContext and NOT plugged into BlockingIoContext
// (the spike must not touch default backend selection). See docs/io-uring-spike.md.
#pragma once

#include <cppio/experimental/uring_write_batch.hpp>
#include <cppio/result.hpp>

#include <cstddef>
#include <span>
#include <string_view>

namespace cppio::experimental {

class UringIoContext {
public:
    explicit UringIoContext(unsigned queue_depth = 64);
    // Open(path, O_WRONLY|O_CREAT|O_TRUNC) -> UringWriteBatch::write_all -> close.
    // Returns backend_error when liburing is unavailable (clean skip).
    Result<UringWriteResult> write_file_all(std::string_view path,
                                            std::span<const std::byte> bytes);

private:
    UringWriteBatch batch_;
};

}  // namespace cppio::experimental
