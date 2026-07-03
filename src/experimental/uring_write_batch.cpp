// UringWriteBatch implementation (CPPIO-CORE-013C).
//
// Two compile modes:
//   * CPPIO_HAS_LIBURING defined: real io_uring path via liburing.
//   * otherwise: unsupported stub (construction ok, write_all -> backend_error).
// The stub keeps the project buildable with no liburing dependency (013B gate).
#include <cppio/experimental/uring_write_batch.hpp>

#include <cppio/error.hpp>

#if defined(CPPIO_HAS_LIBURING)
#include <liburing.h>
#endif

namespace cppio::experimental {

UringWriteBatch::UringWriteBatch(unsigned queue_depth) : queue_depth_(queue_depth) {
#if defined(CPPIO_HAS_LIBURING)
    auto* ring = new io_uring{};
    if (::io_uring_queue_init(queue_depth, ring, 0) == 0) {
        ring_ = ring;
    } else {
        delete ring;
        ring_ = nullptr;
    }
#else
    (void)queue_depth;  // stub: no ring to size
#endif
}

UringWriteBatch::~UringWriteBatch() {
#if defined(CPPIO_HAS_LIBURING)
    if (ring_) {
        ::io_uring_queue_exit(static_cast<io_uring*>(ring_));
        delete static_cast<io_uring*>(ring_);
        ring_ = nullptr;
    }
#endif
}

Result<UringWriteResult> UringWriteBatch::write_all(int fd, std::span<const std::byte> bytes,
                                                    std::uint64_t file_offset) {
#if !defined(CPPIO_HAS_LIBURING)
    (void)fd; (void)bytes; (void)file_offset;
    // Unsupported stub: the project was built without liburing. Return
    // backend_error so callers/tests can skip cleanly.
    return make_unexpected<UringWriteResult>(IoError{IoError::Code::backend_error});
#else
    UringWriteResult result{};
    if (ring_ == nullptr) {
        return make_unexpected<UringWriteResult>(IoError{IoError::Code::invalid_state});
    }
    if (fd < 0) {
        return make_unexpected<UringWriteResult>(IoError{IoError::Code::permission_denied});
    }
    auto* ring = static_cast<io_uring*>(ring_);
    std::uint64_t off = file_offset;
    std::size_t remaining = bytes.size();
    const std::byte* p = bytes.data();
    while (remaining > 0) {
        io_uring_sqe* sqe = ::io_uring_get_sqe(ring);
        if (sqe == nullptr) {
            // Submission queue full: flush pending, then retry.
            ::io_uring_submit(ring);
            sqe = ::io_uring_get_sqe(ring);
            if (sqe == nullptr) {
                ++result.errors;
                return make_unexpected<UringWriteResult>(IoError{IoError::Code::invalid_state});
            }
        }
        ::io_uring_prep_write(sqe, fd, p, remaining, static_cast<__off_t>(off));
        ++result.submitted;
        if (::io_uring_submit(ring) < 0) {
            ++result.errors;
            return make_unexpected<UringWriteResult>(IoError{IoError::Code::backend_error});
        }
        io_uring_cqe* cqe = nullptr;
        int wait = ::io_uring_wait_cqe(ring, &cqe);
        if (wait < 0 || cqe == nullptr) {
            ++result.errors;
            return make_unexpected<UringWriteResult>(IoError{IoError::Code::backend_error});
        }
        int res = cqe->res;
        ::io_uring_cqe_seen(ring, cqe);
        ++result.completed;
        if (res < 0) {
            ++result.errors;
            return make_unexpected<UringWriteResult>(from_errno_value(-res));
        }
        std::size_t wrote = static_cast<std::size_t>(res);
        result.bytes_written += wrote;
        if (wrote == 0) {
            // Zero progress: stop rather than spin.
            ++result.errors;
            return make_unexpected<UringWriteResult>(IoError{IoError::Code::invalid_state});
        }
        remaining -= std::min(wrote, remaining);
        p += wrote;
        off += wrote;
    }
    return result;
#endif
}

}  // namespace cppio::experimental
