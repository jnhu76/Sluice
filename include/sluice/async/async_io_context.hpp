// sluice::async foundation (sluice-CORE-017, ADR §3/§4).
//
// The L1 async API surface: op descriptors + AsyncIoContext (the public
// foundation) + AsyncBackend (the internal interface decoupling L1 from how
// completions are produced). Per the ADR this lives in namespace sluice::async
// and is OPT-IN; BlockingIoContext and Reader/Writer are untouched (A6).
//
// 017 ships the skeleton + a default backend that completes ops SYNCHRONOUSLY
// at poll() time (no threads, no kernel). Real backends land in later jobs:
//   - 019 FakeAsyncBackend  (deterministic test vehicle, error/short injection)
//   - 020A ThreadPoolBackend (portable, std::thread)
//   - 020B UringAsyncBackend (gated, liburing)
//
// Reaping is poll() / wait_one() ONLY (A3): no drain/deadline, because a
// deadline would smuggle in a timer API this job excludes (016B O2).
#pragma once

#include <sluice/async/completion.hpp>
#include <sluice/error.hpp>
#include <sluice/measurement.hpp>
#include <sluice/result.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>

namespace sluice::async {

// --- Op descriptors (ADR §3). All read/write ops are POSITIONAL (P1): they
// carry an explicit offset. Sync ops carry no buffer/offset (P3). ---
struct ReadOp {
    int fd = -1;
    std::byte* dst = nullptr;
    std::size_t len = 0;
    std::uint64_t offset = 0;
};
struct WriteOp {
    int fd = -1;
    const std::byte* src = nullptr;
    std::size_t len = 0;
    std::uint64_t offset = 0;
};
struct SyncDataOp {  // fdatasync (W4)
    int fd = -1;
};
struct SyncAllOp {   // fsync (W4)
    int fd = -1;
};

// The internal backend boundary (ADR §4). Not public-facing; AsyncIoContext
// delegates to it. Concrete backends (019/020A/020B) implement this.
// Lifecycle: AsyncIoContext OWNS its backend (unique_ptr). State is
// instance-owned; no globals (gate item 6).
class AsyncBackend {
public:
    virtual ~AsyncBackend() = default;
    AsyncBackend(const AsyncBackend&) = delete;
    AsyncBackend& operator=(const AsyncBackend&) = delete;

    // Optional stats sink. The context attaches its caller-owned AsyncStats so
    // the backend can tally per-completion outcomes it knows directly — e.g.
    // canceled_ops (job 021) and completion_errors — without the context having
    // to re-scan results after poll(). Null = no counting (default).
    void attach_stats(AsyncStats* s) { stats_ = s; }

    // Hand an op to the backend against the caller-owned Completion. The backend
    // records the op outstanding (marking the Completion via mark_outstanding()
    // is the context's job, not the backend's, so the state machine stays in one
    // place). Returns Result<void>: submit-time errors (queue full, invalid op,
    // Completion not idle — L8) are synchronous (ADR E5).
    virtual Result<void> submit_read(ReadOp op, Completion<std::size_t>& c) = 0;
    virtual Result<void> submit_write(WriteOp op, Completion<std::size_t>& c) = 0;
    virtual Result<void> submit_sync_data(SyncDataOp op, Completion<void>& c) = 0;
    virtual Result<void> submit_sync_all(SyncAllOp op, Completion<void>& c) = 0;

    // Non-blocking reap: complete only ops the backend can settle now. Returns
    // the count made ready. Never blocks.
    virtual std::size_t poll() = 0;
    // Blocking reap: wait until >=1 ready, then reap. Returns the count made
    // ready or a backend error. No deadline (016B O2).
    virtual Result<std::size_t> wait_one() = 0;

    // ADR §7 X2: best-effort cancel of one outstanding op. Minimal first; the
    // fuller model is job 021. The completion is marked ready in poll/wait_one
    // exactly once (X3).
    virtual void cancel(Completion<std::size_t>& c) { (void)c; }
    virtual void cancel(Completion<void>& c) { (void)c; }

    // Outstanding op count (for AsyncStats.max_outstanding + L11 checks).
    virtual std::size_t outstanding() const noexcept = 0;

protected:
    AsyncBackend() = default;
    AsyncStats* stats_ = nullptr;
};

// The public L1 foundation (parallels the blocking IoContext). Owns a backend;
// routes submit_*/poll/wait_one/cancel to it; tallies AsyncStats.
// Move-only, non-copyable (L6).
class AsyncIoContext {
public:
    // Construct with a concrete backend (owned). stats may be null (no counting).
    explicit AsyncIoContext(std::unique_ptr<AsyncBackend> backend,
                            AsyncStats* stats = nullptr);
    ~AsyncIoContext();

    AsyncIoContext(const AsyncIoContext&) = delete;
    AsyncIoContext& operator=(const AsyncIoContext&) = delete;
    AsyncIoContext(AsyncIoContext&&) noexcept;
    AsyncIoContext& operator=(AsyncIoContext&&) noexcept;

    // A1/A2: submit does not block; records the op outstanding. Submit-time
    // errors are synchronous via Result<void> (E5). The Completion must be idle
    // (L8) or the call returns invalid_state.
    Result<void> submit_read(ReadOp op, Completion<std::size_t>& c);
    Result<void> submit_write(WriteOp op, Completion<std::size_t>& c);
    Result<void> submit_sync_data(SyncDataOp op, Completion<void>& c);
    Result<void> submit_sync_all(SyncAllOp op, Completion<void>& c);

    std::size_t poll();
    Result<std::size_t> wait_one();

    void cancel(Completion<std::size_t>& c);
    void cancel(Completion<void>& c);

    std::size_t outstanding() const noexcept;
    const AsyncStats* stats() const noexcept { return stats_; }

private:
    std::unique_ptr<AsyncBackend> backend_;
    AsyncStats* stats_;
};

}  // namespace sluice::async
