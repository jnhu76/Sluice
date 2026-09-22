#pragma once

// io_uring submission-path lowering detail. File legality, validation
// precedence and error categories are not decided here; they live in
// `sluice/detail/file_semantics.hpp`.

#include <algorithm>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <limits>

namespace sluice::detail {

// Largest length a single io_uring read/write SQE can carry. This is a
// transfer limit of one execution, so a longer request yields a short count
// rather than a File-semantic rejection.
inline unsigned uring_chunk_length(std::size_t remaining) noexcept {
    constexpr auto native_max = static_cast<std::size_t>(std::numeric_limits<unsigned>::max());
    return static_cast<unsigned>(std::min(remaining, native_max));
}

enum class UringSubmitProgress : std::uint8_t {
    error,
    no_progress,
    partial,
    complete,
};

inline UringSubmitProgress classify_uring_submit(int submit_result,
                                                 unsigned pending_before) noexcept {
    if (submit_result < 0)
        return UringSubmitProgress::error;
    if (pending_before == 0 || static_cast<unsigned>(submit_result) >= pending_before) {
        return UringSubmitProgress::complete;
    }
    if (submit_result == 0)
        return UringSubmitProgress::no_progress;
    return UringSubmitProgress::partial;
}

template <class WaitFn> int retry_uring_wait_on_eintr(WaitFn&& wait_fn) {
    int result = 0;
    do {
        result = wait_fn();
    } while (result == -EINTR);
    return result;
}

} // namespace sluice::detail
