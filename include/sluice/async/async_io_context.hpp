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
#include <mutex>

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

// --- Split-phase readiness wait (issue #67 / AGENTS.md §13.2) ---------------
// A backend MAY expose an observe-only readiness wait so a caller can block
// for progress WITHOUT holding AsyncIoContext::access_mtx_ (the pre-fix code
// held it across ThreadPoolBackend::wait_one, starving every other poll/reap
// path and deadlocking drain at shutdown). The wait is pure observation:
// wait_for_change() never reaps, never publishes a Completion, never touches
// request lifecycle or accounting state. It can be interrupted by the control
// plane (interrupt_all) so shutdown can wake parked waiters without
// fabricating readiness.
struct BackendWaitToken {
    // Epochs are monotonic and published under the wait source's own mutex
    // BEFORE any notification. A wait for an observed token returns when
    // either epoch advances; the predicate protocol closes the lost-wake
    // window between snapshot and park (AGENTS.md §13.2).
    std::uint64_t progress_generation = 0;  // real readiness published
    std::uint64_t control_generation = 0;   // control-plane wake (close/stop)
};

enum class BackendWakeReason {
    progress,     // a real readiness publication advanced the epoch
    interrupted,  // a control-plane wake (close_admission / runtime stop)
};

class BackendWaitSource {
public:
    BackendWaitSource() = default;  // keep the implicit default (copy ops are deleted)
    virtual ~BackendWaitSource() = default;
    BackendWaitSource(const BackendWaitSource&) = delete;
    BackendWaitSource& operator=(const BackendWaitSource&) = delete;

    // Snapshot both epochs. MUST be paired with wait_for_change under the
    // documented order: snapshot -> poll -> wait_for_change(observed).
    virtual BackendWaitToken snapshot() const noexcept = 0;

    // MAY block, but NEVER reaps, publishes, or mutates any request,
    // outstanding, or backend_ready state. Returns progress when real
    // readiness advanced, interrupted when the control plane woke the wait.
    // Spurious wakes only re-check the predicate (no state change).
    virtual BackendWakeReason wait_for_change(BackendWaitToken observed) noexcept = 0;

    // Control-plane wake: unblocks ALL parked waiters (notify_all). Must NOT
    // fabricate readiness, change request state, publish Completions, or
    // cancel real I/O. One-shot: future waits snapshot the advanced control
    // generation and park normally again.
    virtual void interrupt_all() noexcept = 0;
};

// The internal backend boundary (ADR §4) — and simultaneously a PUBLIC
// extension point: the header is installed and AsyncBackend may be subclassed
// by tests and applications (ADR-explicit-io-completion-authority §3). Any
// derived class is a TRUSTED backend-author: through the inherited protected
// helpers it can claim Completions and publish terminal results. This is the
// deliberate injection seam that decouples L1 from how completions are
// produced; it is NOT a capability-isolation boundary against deliberately
// subclassing code. Concrete backends (019/020A/020B) implement this.
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
    // claims the Completion via try_claim() (ADR-explicit-io-completion-authority:
    // the backend is the claiming authority). Returns Result<void>: submit-time
    // errors (queue full, invalid op, Completion not idle — L8) are synchronous
    // (ADR E5).
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

    // Issue #67 split-phase wait capability (optional, default absent). A
    // backend that provides it lets AsyncIoContext::wait_one park for
    // readiness WITHOUT holding access_mtx_ (pure observation, interruptible
    // by the control plane). Backends that return nullptr keep the legacy
    // serialized wait_one contract: the whole blocking wait runs under
    // access_mtx_ (safe for non-blocking wait_one implementations; the
    // ApplicationRuntime rejects such backends at build time so the
    // multi-participant production path never takes the blocking fallback).
    // Source-compatible: existing external backends do not override it.
    virtual BackendWaitSource* wait_source() noexcept { return nullptr; }

protected:
    AsyncBackend() = default;
    AsyncStats* stats_ = nullptr;

    // ADR-explicit-io-completion-authority §9/§10: protected publication
    // helpers. Derived backends (the trusted backend-author role) use these to
    // claim Completions, publish terminal results, and roll back a claim that
    // was never accepted into backend tracking. Ordinary non-backend code
    // cannot access them (protected + Completion friendship is granted to
    // AsyncBackend only, not inherited by non-backend code).
    template <class T>
    static bool try_claim(Completion<T>& c) noexcept {
        return c.try_claim_for_backend();
    }

    // Phase B/E (ADR-explicit-io-request-contract, Accepted, Decision 5): the
    // accepted request lifecycle splits the claim into a private two-stage
    // binding so the winning backend can install RequestKey/context/release
    // capability before the Completion becomes observable as outstanding.
    // Migrated backends (Fake, Sync in Phase B; ThreadPool in Phase E) use this
    // protocol; the legacy single-step try_claim above remains for the
    // not-yet-migrated backend (Uring), which is out of Phase B/E scope. The two
    // paths share the same private-access boundary (friend AsyncBackend); they
    // do not race because a given Completion is driven by exactly one backend.
    template <class T>
    static bool begin_binding(Completion<T>& c) noexcept {
        return c.begin_binding_for_backend();
    }
    template <class T>
    static void commit_binding(Completion<T>& c) noexcept {
        c.commit_binding_to_outstanding();
    }
    template <class T>
    static void rollback_binding_before_accept(Completion<T>& c) noexcept {
        c.rollback_binding_before_accept();
    }
    // Phase B (ADR Decision 7 / design §8): the binding CAS winner installs the
    // opaque slot-release capability (arena + slot handle) before the Completion
    // becomes observable as outstanding. reset()/ready-destruction use it to
    // return the slot with generation++ (completion_ready -> free handshake).
    template <class T>
    static void install_binding(Completion<T>& c, detail::RequestArena* arena,
                                detail::SlotHandle h) noexcept {
        c.install_binding_for_backend(arena, h);
    }
    template <class T>
    static void clear_binding(Completion<T>& c) noexcept {
        c.clear_binding_for_backend();
    }

    template <class T>
    static void publish(Completion<T>& c, Result<T>&& result) noexcept {
        c.publish_from_reap(std::move(result));
    }

    // Backend-only: undo a claim that won but was never accepted into backend
    // tracking (no register/enqueue/dispatch; submit has not returned success),
    // e.g. io_uring SQE acquisition failed after claim. Call ONLY immediately
    // after this backend's own successful try_claim(), before any tracking step.
    template <class T>
    static void rollback_claim_before_accept(Completion<T>& c) noexcept {
        c.rollback_claim_before_accept();
    }
};

// The public L1 foundation (parallels the blocking IoContext). Owns a backend;
// routes submit_*/poll/wait_one/cancel to it; tallies AsyncStats.
// Move-only, non-copyable (L6).
//
// E15-P1-03 / E15-P2-06 / ADR §5 L11 — outstanding-Completion lifecycle:
//   * Publication of a terminal result is done by the BACKEND during reap
//     (poll/wait_one) through AsyncBackend::publish; the context routes and
//     reaps but does not itself publish. Destroying this context — OR
//     move-assigning another context over it — while Completions are still
//     outstanding is a CONTRACT VIOLATION and fails fast in BOTH Debug and
//     Release (detail::async_context_outstanding_fail_fast → std::terminate).
//     A destructor / move-assignment has no Result channel to surface
//     invalid_state, and silent abandonment would strand caller-owned,
//     address-stable Completions permanently outstanding.
//   * Move CONSTRUCTION from a source with outstanding work is SAFE: the
//     backend instance (and thus every outstanding Completion pointer it
//     holds) transfers to the new owner, so callers observing those
//     Completions via the new owner still see them resolve.
//   * Move ASSIGNMENT requires the DESTINATION to have zero outstanding work;
//     the SOURCE's outstanding work transfers with the backend.
class AsyncIoContext {
public:
    // Construct with a concrete backend (owned). stats may be null (no counting).
    explicit AsyncIoContext(std::unique_ptr<AsyncBackend> backend,
                            AsyncStats* stats = nullptr);
    ~AsyncIoContext();

    AsyncIoContext(const AsyncIoContext&) = delete;
    AsyncIoContext& operator=(const AsyncIoContext&) = delete;
    // Move ctor: safe even if the source has outstanding work (the backend
    // transfers with it). See the class-header L11 note.
    AsyncIoContext(AsyncIoContext&&) noexcept;
    // Move assign: requires the destination's backend (if any) to have zero
    // outstanding work, else fail-fast. Source-side outstanding work transfers.
    // Self-assignment is a no-op. See the class-header L11 note.
    AsyncIoContext& operator=(AsyncIoContext&&) noexcept;

    // A1/A2: submit does not block; records the op outstanding. Submit-time
    // errors are synchronous via Result<void> (E5). The Completion must be idle
    // (L8) or the call returns invalid_state.
    Result<void> submit_read(ReadOp op, Completion<std::size_t>& c);
    Result<void> submit_write(WriteOp op, Completion<std::size_t>& c);
    Result<void> submit_sync_data(SyncDataOp op, Completion<void>& c);
    Result<void> submit_sync_all(SyncAllOp op, Completion<void>& c);

    std::size_t poll();
    // Blocking reap. With a split-wait-capable backend (wait_source != null)
    // the wait NEVER runs under access_mtx_: the call repeatedly
    //   snapshot -> poll (serialized) -> park in the observe-only ready wait
    // and returns the count of Completions reaped (a plain >0 progress), or
    // 0 when the wait was interrupted by the control plane (close_admission /
    // interrupt_backend_waiters) with nothing reaped — 0 is NOT an error and
    // fabricates no completion (I8). A 0 with no outstanding work is also
    // returned (an empty wait is a no-progress boundary, never a park).
    // Without the capability the legacy serialized contract applies (the whole
    // call, including a backend-side block, runs under access_mtx_).
    Result<std::size_t> wait_one();

    // Control-plane wake for a split-phase wait in progress (issue #67): wakes
    // every participant parked in wait_one()'s observe phase so shutdown /
    // admission close can re-evaluate. No-op for backends without the wait
    // capability. Never fabricates readiness, never touches request state, and
    // never blocks on access_mtx_.
    void interrupt_backend_waiters() noexcept;

    void cancel(Completion<std::size_t>& c);
    void cancel(Completion<void>& c);

    std::size_t outstanding() const noexcept;
    const AsyncStats* stats() const noexcept { return stats_; }

private:
    std::unique_ptr<AsyncBackend> backend_;
    AsyncStats* stats_;
    // E7-C serialized backend access domain: at most one caller CONSUMES
    // AsyncBackend at a time (submit_*/cancel/poll/reap/outstanding all
    // acquire this). The split-phase wait's OBSERVE phase (wait_for_change)
    // intentionally runs outside it — it is a pure epoch wait that touches no
    // backend state, so it may run concurrently with serialized consuming
    // operations. No context-level lock is ever held across an unbounded
    // block (issue #67).
    mutable std::mutex access_mtx_;
};

}  // namespace sluice::async
