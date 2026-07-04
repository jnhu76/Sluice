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
// stub mode.
//
// State is instance-owned (no globals, gate item 6).
#pragma once

#include <sluice/async/async_io_context.hpp>
#include <sluice/async/completion.hpp>
#include <sluice/error.hpp>
#include <sluice/result.hpp>

#include <cstddef>
#include <cstdint>

namespace sluice::async {

class UringAsyncBackend : public AsyncBackend {
public:
    // Construct with a submit/completion queue depth (clamped to kernel limits
    // when liburing is present). Stub mode ignores depth.
    explicit UringAsyncBackend(unsigned queue_depth = 64);
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

    // Whether this instance is backed by a real io_uring (false in stub mode).
    bool available() const noexcept;

private:
#if defined(SLUICE_HAS_LIBURING)
    struct Impl;
    Impl* impl_ = nullptr;
#endif
    bool available_ = false;
};

}  // namespace sluice::async
