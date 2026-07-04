// sluice::async::UringAsyncBackend (sluice-CORE-020B, ADR §4 Option 4).
//
// The Linux io_uring backend: submits read/write/sync SQEs and reaps CQEs in
// poll()/wait_one(). This is the HIGH-CONCURRENCY path (one syscall batches many
// ops, no per-op thread). GATED behind liburing (ADR §11 D4 — optional dep):
//
//   * SLUICE_HAS_LIBURING defined (liburing linked): real io_uring path.
//   * otherwise: UNSUPPORTED STUB. submit_* returns IoError::backend_error
//     synchronously; poll()/wait_one() reap nothing. The project builds with no
//     liburing dependency (020B gate / 013B pattern); tests run in stub mode.
//
// Cancel (ADR §7 X2): best-effort via io_uring IORING_OP_ASYNC_CANCEL (when
// liburing is present); no-op in stub mode.
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
    unsigned queue_depth_ = 0;
    bool available_ = false;
};

}  // namespace sluice::async
