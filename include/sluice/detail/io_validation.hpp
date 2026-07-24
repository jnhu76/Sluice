// Checked conversions shared by POSIX and io_uring I/O backends.
//
// These helpers are intentionally internal: public operations continue to use
// size_t/uint64_t, while each native backend rejects values it cannot represent
// before issuing a syscall or marking caller-owned completion state.
#pragma once

#include <sluice/error.hpp>
#include <sluice/result.hpp>

#include <algorithm>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <sys/types.h>

namespace sluice::detail {

static_assert(std::numeric_limits<off_t>::is_integer &&
                  std::numeric_limits<off_t>::is_signed,
              "sluice positional I/O requires a signed integral off_t");
static_assert(std::numeric_limits<off_t>::digits >= 63,
              "sluice positional I/O requires 64-bit large-file support "
              "(_FILE_OFFSET_BITS=64 / LFS)");

inline Result<off_t> checked_posix_offset(std::uint64_t offset) {
    constexpr auto native_max =
        static_cast<std::uint64_t>(std::numeric_limits<off_t>::max());
    if (offset > native_max) {
        return make_unexpected<off_t>(IoError{.code = IoError::Code::invalid_state});
    }
    return static_cast<off_t>(offset);
}

inline Result<unsigned> checked_uring_length(std::size_t length) {
    constexpr auto native_max =
        static_cast<std::size_t>(std::numeric_limits<unsigned>::max());
    if (length > native_max) {
        return make_unexpected<unsigned>(
            IoError{.code = IoError::Code::invalid_state});
    }
    return static_cast<unsigned>(length);
}

inline unsigned uring_chunk_length(std::size_t remaining) noexcept {
    constexpr auto native_max =
        static_cast<std::size_t>(std::numeric_limits<unsigned>::max());
    return static_cast<unsigned>(std::min(remaining, native_max));
}

enum class UringSubmitProgress : std::uint8_t {
    error,
    no_progress,
    partial,
    complete,
};

// Classify io_uring_submit() against the number of SQEs that were pending
// before the call. A short positive return is not an operation failure: the
// unconsumed SQEs remain in the shared SQ and must be retained for a later
// submit. Negative and zero-progress results likewise leave caller-owned
// Completion state outstanding.
inline UringSubmitProgress classify_uring_submit(int submit_result,
                                                 unsigned pending_before) noexcept {
    if (submit_result < 0) return UringSubmitProgress::error;
    if (pending_before == 0 ||
        static_cast<unsigned>(submit_result) >= pending_before) {
        return UringSubmitProgress::complete;
    }
    if (submit_result == 0) return UringSubmitProgress::no_progress;
    return UringSubmitProgress::partial;
}

// liburing wait functions return negative errno values directly. Retry only
// -EINTR; other negative results remain available to the caller for mapping.
template <class WaitFn>
int retry_uring_wait_on_eintr(WaitFn&& wait_fn) {
    int result = 0;
    do {
        result = wait_fn();
    } while (result == -EINTR);
    return result;
}

} // namespace sluice::detail
