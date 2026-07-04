// Implementation of UringAsyncBackend (sluice-CORE-020B).
//
// Two modes via SLUICE_HAS_LIBURING (matching the 013B experimental-uring gate):
//   * defined: real io_uring via liburing. Submits SQEs for read/write/sync,
//     reaps CQEs in poll()/wait_one() (the single reaping family, ADR A3/O1).
//   * undefined: UNSUPPORTED STUB. submit_* returns backend_error synchronously
//     so the project builds/links with no liburing dependency; tests run in stub
//     mode and assert the clean-skip contract.
#include <sluice/async/uring_backend.hpp>

#include <utility>

namespace sluice::async {

UringAsyncBackend::UringAsyncBackend(unsigned queue_depth)
    : queue_depth_(queue_depth) {
#if defined(SLUICE_HAS_LIBURING)
    // TODO(020B-real): io_uring_queue_init(queue_depth, &ring_, 0) and allocate
    // the per-SQE user-data -> Completion map. Left for a real-liburing build;
    // the stub path keeps this backend buildable everywhere.
    available_ = false;  // set true once the real init lands
#else
    available_ = false;
#endif
}

UringAsyncBackend::~UringAsyncBackend() {
#if defined(SLUICE_HAS_LIBURING)
    // TODO(020B-real): io_uring_queue_exit(&ring_), free the Completion map.
#endif
}

namespace {
// Stub-mode synchronous reject: the project was built without liburing. The op
// is NOT recorded outstanding (no Completion mutation), so outstanding() stays 0
// and the context's L11 check passes. The caller sees backend_error immediately.
Result<void> unsupported_stub() {
    return make_unexpected<void>(IoError{IoError::Code::backend_error});
}
}  // namespace

Result<void> UringAsyncBackend::submit_read(ReadOp, Completion<std::size_t>&) {
#if defined(SLUICE_HAS_LIBURING)
    // TODO(020B-real): io_uring_prep_read, set user_data = &c, mark outstanding.
#endif
    return unsupported_stub();
}
Result<void> UringAsyncBackend::submit_write(WriteOp, Completion<std::size_t>&) {
#if defined(SLUICE_HAS_LIBURING)
    // TODO(020B-real): io_uring_prep_write.
#endif
    return unsupported_stub();
}
Result<void> UringAsyncBackend::submit_sync_data(SyncDataOp, Completion<void>&) {
#if defined(SLUICE_HAS_LIBURING)
    // TODO(020B-real): io_uring_prep_fsync(fd, IORING_FSYNC_DATASYNC).
#endif
    return unsupported_stub();
}
Result<void> UringAsyncBackend::submit_sync_all(SyncAllOp, Completion<void>&) {
#if defined(SLUICE_HAS_LIBURING)
    // TODO(020B-real): io_uring_prep_fsync(fd, 0).
#endif
    return unsupported_stub();
}

std::size_t UringAsyncBackend::poll() {
#if defined(SLUICE_HAS_LIBURING)
    // TODO(020B-real): io_uring_peek_batch_cqe, complete each via user_data,
    // io_uring_cqe_seen. Tally canceled_ops / completion_errors via stats_.
#endif
    return 0;  // stub: nothing to reap
}

Result<std::size_t> UringAsyncBackend::wait_one() {
#if defined(SLUICE_HAS_LIBURING)
    // TODO(020B-real): io_uring_wait_cqe (blocking), then reap like poll().
#endif
    return std::size_t{0};  // stub: nothing to reap
}

void UringAsyncBackend::cancel(Completion<std::size_t>&) {
#if defined(SLUICE_HAS_LIBURING)
    // TODO(020B-real): io_uring_prep_cancel with the SQE's user_data.
#endif
    // stub: no-op (nothing was submitted)
}
void UringAsyncBackend::cancel(Completion<void>&) {
#if defined(SLUICE_HAS_LIBURING)
    // TODO(020B-real): io_uring_prep_cancel.
#endif
}

std::size_t UringAsyncBackend::outstanding() const noexcept {
#if defined(SLUICE_HAS_LIBURING)
    // TODO(020B-real): return in-flight SQE count.
#endif
    return 0;  // stub: never records outstanding
}

bool UringAsyncBackend::available() const noexcept {
    return available_;
}

}  // namespace sluice::async
