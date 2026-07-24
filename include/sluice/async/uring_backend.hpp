// sluice::async::UringAsyncBackend (sluice-CORE-020B, ADR §4 Option 4).
//
// The Linux io_uring backend: submits read/write/sync SQEs and reaps CQEs in
// poll()/wait_one(). This is the HIGH-CONCURRENCY path (one syscall batches many
// ops, no per-op thread). GATED behind liburing (ADR §11 D4 — optional dep):
//
//   * SLUICE_HAS_LIBURING defined (liburing linked): real io_uring path.
//     Submit batches SQEs without submitting (lazy); poll() flushes via
//     io_uring_submit and reaps io_uring_peek_batch_cqe; wait_one() uses
//     io_uring_submit_and_wait. CQE res<0 maps to IoError via from_errno_value
//     (ADR E3). SQE pressure (queue full) is flushed + retried and tallied as
//     queue_full_retries.
//   * otherwise: UNSUPPORTED STUB. submit_* returns IoError::backend_error
//     synchronously; poll()/wait_one() reap nothing. The project builds with no
//     liburing dependency (020B gate / 013B pattern); tests run in stub mode.
//
// Cancel (ADR §7 X2): best-effort via io_uring IORING_OP_ASYNC_CANCEL
// (io_uring_prep_cancel64) when liburing is present. The cancel-vs-in-flight-CQE
// race is resolved structurally: the original op's CQE is the ONLY thing that
// completes the Completion (exactly-once, X3); a cancel SQE only toggles intent
// for stat accounting and is dropped if the target already resolved. No-op in
// stub mode. As of 026 (B3) cancel is O(1) average via a Completion* -> op-id
// reverse index (was a linear scan of outstanding ops).
//
// Submit batching (026 B3): submit_* only acquires + preps an SQE (flushing on
// SQE pressure); the kernel is poked in poll()/wait_one(). The public L1 API is
// unchanged (one submit_* per op); the seam is internal so a future Batch (T4)
// can submit many SQEs per flush. This matches Zig Io/Uring.zig's enqueue/submit
// split.
//
// Submit failure policy: transient -EINTR/-EAGAIN/-EBUSY and one anomalous
// zero-progress result retain the userspace-owned SQE suffix for a later driver
// call. The first permanent negative error (or repeated zero progress) makes
// the backend terminal: the accepted prefix remains kernel-owned and completes
// from CQEs, the provably unsubmitted suffix completes once with that error,
// later submissions fail synchronously, poll() never resubmits the suffix, and
// wait_one() returns the stored error instead of blocking indefinitely.
//
// Feature gates (026 B3): SLUICE_URING_REGISTERED_BUFFERS and
// SLUICE_URING_REGISTERED_FILES are build options, both OFF by default (matching
// Zig upstream — Io/Uring.zig uses neither). A future job may implement them
// under a documented lifetime contract.
//
// State is instance-owned (no globals, gate item 6).
#pragma once

#include <sluice/async/async_io_context.hpp>
#include <sluice/async/completion.hpp>
#include <sluice/error.hpp>
#include <sluice/result.hpp>

#include <cstddef>
#include <cstdint>

#if defined(SLUICE_HAS_LIBURING) && defined(SLUICE_URING_INTERNAL_TESTING)
struct io_uring;
#endif

namespace sluice::async {

#if defined(SLUICE_HAS_LIBURING) && defined(SLUICE_URING_INTERNAL_TESTING)
// Non-installed submit seam used by the dedicated real-liburing fault tests.
// Production targets never define SLUICE_URING_INTERNAL_TESTING and therefore
// expose neither this type nor the constructor overload below.
struct UringBackendSubmitTestHooks {
    using SubmitFn = int (*)(void*, ::io_uring*) noexcept;

    void* context = nullptr;
    SubmitFn submit = nullptr;
};
#endif

class UringAsyncBackend : public AsyncBackend {
public:
    // Construct with a submit/completion queue depth (clamped to kernel limits
    // when liburing is present). Stub mode ignores depth.
    explicit UringAsyncBackend(unsigned queue_depth = 64);
#if defined(SLUICE_HAS_LIBURING) && defined(SLUICE_URING_INTERNAL_TESTING)
    UringAsyncBackend(unsigned queue_depth, UringBackendSubmitTestHooks hooks);
#endif
    ~UringAsyncBackend() override;

    UringAsyncBackend(const UringAsyncBackend&) = delete;
    UringAsyncBackend& operator=(const UringAsyncBackend&) = delete;

    Result<void> submit_read(ReadOp op, Completion<std::size_t>& c) override;
    Result<void> submit_write(WriteOp op, Completion<std::size_t>& c) override;
    Result<void> submit_sync_data(SyncDataOp op, Completion<void>& c) override;
    Result<void> submit_sync_all(SyncAllOp op, Completion<void>& c) override;

    std::size_t poll() override;
    Result<std::size_t> wait_one() override;

    void cancel(Completion<std::size_t>& c) override;
    void cancel(Completion<void>& c) override;

    std::size_t outstanding() const noexcept override;

    // Whether this instance initialized a real io_uring (false in stub mode).
    // This is a capability query, not a health query: a later terminal submit
    // error is observed through Completion results, wait_one(), and submit_*.
    bool available() const noexcept;

private:
#if defined(SLUICE_HAS_LIBURING)
    struct Impl;
    Impl* impl_ = nullptr;
#endif
    bool available_ = false;
};

}  // namespace sluice::async
