// cppio::experimental::UringWriteBatch — narrow experimental io_uring write
// spike (CPPIO-CORE-013C). See docs/io-uring-spike.md.
//
// WITHOUT CPPIO_HAS_LIBURING this header still compiles and the class is an
// unsupported stub: construction/write_all report unsupported so the project
// builds with no liburing dependency. WITH CPPIO_HAS_LIBURING it wraps a real
// io_uring ring (synchronous-over-uring: submit, then block on completion).
#pragma once

#include <cppio/measurement.hpp>
#include <cppio/result.hpp>

#include <cstddef>
#include <cstdint>
#include <span>

namespace cppio::experimental {

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

    // Submit one or more write ops until all `bytes` are written or an error
    // occurs. Does NOT take ownership of `fd`, does NOT close it. The caller's
    // `bytes` buffer must outlive the call (completion is waited for before
    // return). Without CPPIO_HAS_LIBURING returns backend_error.
    Result<UringWriteResult> write_all(int fd, std::span<const std::byte> bytes,
                                       std::uint64_t file_offset);

    // Optional measurement (CPPIO-CORE-013E). Caller-owned; null = no counting.
    void set_stats(UringStats* stats) { stats_ = stats; }

private:
    [[maybe_unused]] unsigned queue_depth_;
#if defined(CPPIO_HAS_LIBURING)
    void* ring_ = nullptr;  // points to a struct io_uring owned by the .cpp
#else
    [[maybe_unused]] void* ring_ = nullptr;  // unused stub
#endif
    UringStats* stats_ = nullptr;
};

}  // namespace cppio::experimental
