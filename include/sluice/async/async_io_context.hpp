// sluice::async foundation (ADR §3/§4).
//
// The L1 async API surface: op descriptors + AsyncIoContext (the public
// foundation) + AsyncBackend (the internal interface decoupling L1 from how
// completions are produced). Per the ADR this lives in namespace sluice::async
// and is OPT-IN; BlockingIoContext and Reader/Writer are untouched (A6).
//
// The reference backend completes ops SYNCHRONOUSLY at poll() time (no
// threads, no kernel). Concrete backends:
//   - FakeAsyncBackend  (deterministic test vehicle, error/short injection)
//   - ThreadPoolBackend (portable, std::thread)
//   - UringAsyncBackend (gated, liburing)
//
// Reaping is poll() / wait_one() ONLY (A3): no drain/deadline, because a
// deadline would smuggle in a timer API the async foundation excludes.
#pragma once

#include <sluice/async/completion.hpp>
#include <sluice/async/detail/ready_sink.hpp>
#include <sluice/async/request_handle.hpp>
#include <sluice/error.hpp>
#include <sluice/measurement.hpp>
#include <sluice/result.hpp>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>

namespace sluice::async {

// --- Op descriptors (ADR §3). All read/write ops are POSITIONAL: they
// carry an explicit offset. Sync ops carry no buffer/offset. ---
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

// --- Split-phase readiness wait (AGENTS.md §13.2) ---------------------------
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

    // Bounded-park variant: identical epoch protocol, with the
    // physical park capped at `max_park` so the caller can guarantee a
    // re-drain before an active deadline (the timer pump) — the MW-S2
    // MIXED-WAKE participant parks in wait_one() for split-wait backends.
    // Bounded parking is a SEPARATE capability (supports_bounded_wait): the
    // base implementation does NOT honor the bound — it parks on the
    // unbounded one-argument contract — so a caller with a deadline
    // obligation MUST query supports_bounded_wait() first and MUST NOT
    // silently fall back to this overload expecting a bounded park (a
    // discarded timeout is a liveness hole, not a degraded mode). In-tree
    // split-wait sources (ReadyWaitSource: cv.wait_for; UringWaitSource:
    // poll timeout) override both the overload and the capability.
    // `max_park` must be finite (nanoseconds::max() is the unbounded
    // sentinel and MUST NOT be passed — call the one-argument form instead).
    virtual BackendWakeReason wait_for_change(BackendWaitToken observed,
                                              std::chrono::nanoseconds max_park) noexcept {
        (void)max_park;
        return wait_for_change(observed);
    }

    // Whether the bounded overload above
    // actually bounds the physical park. Default false — an external
    // BackendWaitSource that only implements the one-argument unbounded
    // contract truthfully reports no bounded support. Consumers with a
    // deadline-driven park cap (AsyncIoContext::wait_one(max_park), the
    // Scheduler's MW-S2 bounded park) must check this BEFORE relying on a
    // finite bound; the base-class overload never fabricates the capability.
    virtual bool supports_bounded_wait() const noexcept { return false; }

    // Control-plane wake: unblocks ALL parked waiters (notify_all). Must NOT
    // fabricate readiness, change request state, publish Completions, or
    // cancel real I/O. One-shot: future waits snapshot the advanced control
    // generation and park normally again.
    virtual void interrupt_all() noexcept = 0;

    // Commit-to-park handshake: arm a ONE-SHOT mandatory
    // control-observation baseline for the NEXT wait_one() invocation. Called
    // by the committing authority (the Scheduler's MW-S2 Phase-B commit,
    // under global_mtx_) BEFORE the participant is exposed as about-to-park.
    // A control-plane wake (close_admission / interrupt_backend_waiters)
    // published AFTER this call is then guaranteed observed by that wait_one()
    // invocation even when it lands in the commit-to-wait_one window, before
    // the invocation captured its own snapshot — without the registration the
    // wake is rebaselined as a past event and the participant can park
    // forever (the runtime shutdown race). The default (no registration)
    // behaves exactly like snapshot(); split-wait backends that must not lose
    // a pre-snapshot interrupt override both methods (ReadyWaitSource /
    // UringWaitSource).
    virtual BackendWaitToken arm_committed_wait() noexcept { return snapshot(); }
    // One-shot consume of a previously armed baseline; without one, behaves
    // like snapshot(). wait_one() calls this at invocation start INSTEAD of
    // snapshot() so the commit-to-park registration wins over the entry
    // snapshot (the control baseline is captured once per external
    // invocation, and the commit is the invocation-begin for a Scheduler
    // participant).
    virtual BackendWaitToken consume_committed_wait() noexcept { return snapshot(); }
};

// The internal backend boundary (ADR §4) — and simultaneously a PUBLIC
// extension point: the header is installed and AsyncBackend may be subclassed
// by tests and applications (ADR-explicit-io-completion-authority §3). Any
// derived class is a TRUSTED backend-author: through the inherited protected
// helpers it can claim Completions and publish terminal results. This is the
// deliberate injection seam that decouples L1 from how completions are
// produced; it is NOT a capability-isolation boundary against deliberately
// subclassing code. The concrete backends implement this.
// Lifecycle: AsyncIoContext OWNS its backend (unique_ptr). State is
// instance-owned; no globals.
class AsyncBackend {
public:
    virtual ~AsyncBackend() = default;
    AsyncBackend(const AsyncBackend&) = delete;
    AsyncBackend& operator=(const AsyncBackend&) = delete;

    // Optional stats sink. The context attaches its caller-owned AsyncStats so
    // the backend can tally per-completion outcomes it knows directly — e.g.
    // canceled_ops and completion_errors — without the context having
    // to re-scan results after poll(). Null = no counting (default).
    void attach_stats(AsyncStats* s) { stats_ = s; }

    // Optional identity-bearing ReadySink attachment.
    // When set, every reap (poll/wait_one) delivers by-value ReadyEvents to
    // this sink instead of the backend's internal no-op ReferenceReadySink.
    // The sink runs with NO slot/backend/admission lock held (arena contract)
    // and MUST NOT acquire backend-progress locks; the Scheduler-owned sink
    // only marks routing records under its own leaf domain (design
    // docs/design/phase-f1-scheduler-ready-sink.md §5). Null restores the
    // no-op default. Mirrors attach_stats: a narrow, non-virtual, non-owning
    // setter. The Scheduler installs its sink at construction and detaches at
    // destruction; a standalone context (no Scheduler) keeps the no-op sink.
    void attach_ready_sink(detail::SynchronousReadySink* sink) noexcept {
        routing_sink_ = sink;
    }

    // Production waiter registration / cancellation (ADR Decision
    // 10). Registers ONE waiter (token + routing lease) on the slot bound to
    // the accepted Completion. A second registration returns invalid_state
    // without overwriting the first; an unresolvable Completion
    // (unbound / cross-context / stale) returns invalid_state (provenance
    // misuse, Decision 6). cancel_waiter removes ONLY the waiter — never the
    // I/O, never the borrow — and returns the moved-out lease on success
    // (not_found when reap already closed the registration). Default
    // implementations return not_supported for backends without the
    // RequestArena waiter machinery.
    virtual Result<void> register_waiter(Completion<std::size_t>& c,
                                         detail::WaiterToken token,
                                         detail::RoutingLease lease) {
        (void)c; (void)token; (void)lease;
        return make_unexpected<void>(IoError{IoError::Code::not_supported});
    }
    virtual Result<void> register_waiter(Completion<void>& c,
                                         detail::WaiterToken token,
                                         detail::RoutingLease lease) {
        (void)c; (void)token; (void)lease;
        return make_unexpected<void>(IoError{IoError::Code::not_supported});
    }
    virtual Result<detail::RoutingLease> cancel_waiter(Completion<std::size_t>& c) {
        (void)c;
        return make_unexpected<detail::RoutingLease>(
            IoError{IoError::Code::not_supported});
    }
    virtual Result<detail::RoutingLease> cancel_waiter(Completion<void>& c) {
        (void)c;
        return make_unexpected<detail::RoutingLease>(
            IoError{IoError::Code::not_supported});
    }

    // Hand an op to the backend against the caller-owned Completion. The backend
    // claims the Completion through the two-stage binding (ADR-explicit-io-
    // completion-authority + ADR-explicit-io-request-contract Decision 5: the
    // backend is the claiming authority; `binding -> outstanding` is the
    // acceptance LP). Returns Result<void>: submit-time errors (queue full,
    // invalid op, Completion not idle — L8) are synchronous (ADR E5).
    virtual Result<void> submit_read(ReadOp op, Completion<std::size_t>& c) = 0;
    virtual Result<void> submit_write(WriteOp op, Completion<std::size_t>& c) = 0;
    virtual Result<void> submit_sync_data(SyncDataOp op, Completion<void>& c) = 0;
    virtual Result<void> submit_sync_all(SyncAllOp op, Completion<void>& c) = 0;

    // Non-blocking reap: complete only ops the backend can settle now. Returns
    // the count made ready. Never blocks.
    virtual std::size_t poll() = 0;
    // Blocking reap: wait until >=1 ready, then reap. Returns the count made
    // ready or a backend error. No deadline (timers are out of scope).
    virtual Result<std::size_t> wait_one() = 0;

    // ADR §7 X2: best-effort cancel of one outstanding op — the op may still
    // complete with its ordinary result. The completion is marked ready in
    // poll/wait_one exactly once (X3).
    virtual void cancel(Completion<std::size_t>& c) { (void)c; }
    virtual void cancel(Completion<void>& c) { (void)c; }

    // Outstanding op count (for AsyncStats.max_outstanding + L11 checks).
    virtual std::size_t outstanding() const noexcept = 0;

    // Split-phase wait capability (optional, default absent). A
    // backend that provides it lets AsyncIoContext::wait_one park for
    // readiness WITHOUT holding access_mtx_ (pure observation, interruptible
    // by the control plane). Backends that return nullptr keep the legacy
    // serialized wait_one contract: the whole blocking wait runs under
    // access_mtx_ — safe when the backend's wait_one never blocks (see
    // wait_one_is_nonblocking) or when the caller is the single documented
    // driver. Source-compatible: existing external backends do not override
    // it.
    virtual BackendWaitSource* wait_source() noexcept { return nullptr; }

    // Whether wait_one() is guaranteed NON-BLOCKING (returns
    // immediately with whatever is currently reaped — e.g. the reference
    // backends, whose readiness is produced synchronously inside poll).
    // ApplicationRuntime requires EITHER a split wait capability OR this
    // non-blocking contract: the multi-participant runtime path must never
    // take a BLOCKING legacy wait_one (a participant parked while holding
    // access_mtx_ starves every other poll/reap path and deadlocks drain).
    // The default (false) is the conservative choice for external backends.
    virtual bool wait_one_is_nonblocking() const noexcept { return false; }

    // ADR-public-request-handle: public accepted-request identity.
    // supports_request_identity() is the only PUBLIC part of the seam: whether
    // this backend can produce/resolve a RequestHandle. Default false —
    // external/legacy backends that do not use the RequestArena identity
    // contract truthfully opt out. The four production arena backends override
    // to true. submit_*_request checks this BEFORE accepting: false =>
    // not_supported with no side effect, so the Decision-4 contract
    // ("successful acceptance => exactly one valid handle") holds without
    // leaving an accepted-but-handleless operation.
    virtual bool supports_request_identity() const noexcept { return false; }

private:
    // Sealed identity seam (ADR-public-request-handle Decision 2 — non-forgeable
    // construction authority). AsyncIoContext is the ONLY consumer: its
    // submit_*_request mints the handle from the just-bound Completion and its
    // request_state resolves it. Ordinary code — even code holding a raw
    // AsyncBackend* (the backend is a public extension point) — must not be able
    // to mint a handle from a Completion (identity_of) or feed a raw
    // (context, slot, generation) tuple into the resolver (request_handle_state
    // / resolve_identity_state): either would bypass the submit_*_request
    // construction authority and expose the internal identity tuple.
    friend class AsyncIoContext;

    // Virtual hook for request_state(): resolve a public identity tuple
    // (context, slot, generation) to the slot's current state, or not_supported
    // for backends without the identity contract. PRIVATE virtual: derived
    // backends override it (override access is checked at the call site, so
    // their overrides are private too) and are reached only through
    // request_handle_state, never through a raw backend pointer. Takes PLAIN
    // scalars so derived backends need no friendship (friendship is not
    // inherited). Arena backends override with a one-line delegation to their
    // arena. not_found is a state, not an error, for valid handles whose slot
    // was released/reused or whose context does not match.
    virtual Result<RequestHandleState> resolve_identity_state(std::uint64_t context,
                                                              std::uint32_t slot,
                                                              std::uint64_t generation) const {
        (void)context; (void)slot; (void)generation;
        return make_unexpected<RequestHandleState>(IoError{IoError::Code::not_supported});
    }

    // Non-virtual identity extraction (friend of Completion + RequestHandle).
    // Derives a handle from a Completion's private arena binding installed at
    // commit (release_arena_ + bound_slot_). Returns an invalid handle when the
    // Completion has no arena binding (legacy/external backend). Defined
    // out-of-line where RequestArena's full definition is visible.
    RequestHandle identity_of(Completion<std::size_t>& c) const noexcept;
    RequestHandle identity_of(Completion<void>& c) const noexcept;

    // Non-virtual entry for request_state() (AsyncIoContext, friend): extracts
    // the handle's private components (friend of RequestHandle) and delegates to
    // the virtual resolve_identity_state(). An invalid handle short-circuits to
    // not_found.
    Result<RequestHandleState> request_handle_state(const RequestHandle& h) const noexcept;

protected:
    AsyncBackend() = default;
    AsyncStats* stats_ = nullptr;
    // The identity-bearing ReadySink the backend delivers reap
    // events to, when a consumer (the Scheduler) attached one. Null = the
    // backend's internal no-op ReferenceReadySink. Read by the backend's reap
    // call sites; written only by attach_ready_sink (Scheduler construction /
    // destruction, never concurrent with a reap of the same backend).
    detail::SynchronousReadySink* routing_sink_ = nullptr;

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

    // Two-stage binding (ADR-explicit-io-request-contract, Accepted,
    // Decision 5): the accepted request lifecycle splits the claim into a
    // private two-stage binding so the winning backend can install
    // RequestKey/context/release capability before the Completion becomes
    // observable as outstanding. All production backends (Fake, Sync,
    // ThreadPool, Uring) use this protocol — the `binding -> outstanding`
    // release-store is the acceptance LP (ADR §"Commit / accept" Step 5). The
    // legacy single-step `try_claim` above is retained ONLY as a protected
    // helper for the external-backend-authority negative-compile probe
    // (tests/external_backend_authority_negative_probe.cpp), which exercises
    // the simplest claim shape to prove the protected-access boundary; no
    // production backend uses it for acceptance. The two helpers share the
    // same private-access boundary (friend AsyncBackend); they do not race
    // because a given Completion is driven by exactly one backend.
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
    // ADR Decision 7 / design §8: the binding CAS winner installs the
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
// ADR §5 L11 — outstanding-Completion lifecycle:
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

    // ADR-public-request-handle: additive submit variants that
    // return the accepted request's public identity. On success the returned
    // RequestHandle names the committed RequestKey (Decision 4: successful
    // acceptance => exactly one valid handle). On synchronous rejection the
    // Result carries the error and NO handle is produced; no accepted request,
    // no borrow. If the backend does not support request identity
    // (supports_request_identity() == false) the call returns not_supported
    // WITHOUT submitting (no side effect). Serialized under access_mtx_ like
    // submit_*; the handle is derived from the just-bound Completion before the
    // lock is released (no accept/identity TOCTOU).
    Result<RequestHandle> submit_read_request(ReadOp op, Completion<std::size_t>& c);
    Result<RequestHandle> submit_write_request(WriteOp op, Completion<std::size_t>& c);
    Result<RequestHandle> submit_sync_data_request(SyncDataOp op, Completion<void>& c);
    Result<RequestHandle> submit_sync_all_request(SyncAllOp op, Completion<void>& c);

    // Read-only identity consumer (ADR Decision 6): the request's current
    // lifecycle state, or not_found for a stale / cross-context / released /
    // invalid handle. Does not mutate the request, register a waiter, or cancel
    // I/O. Returns not_supported for a backend without the identity contract.
    Result<RequestHandleState> request_state(const RequestHandle& h) const;

    std::size_t poll();
    // Blocking reap. With a split-wait-capable backend (wait_source != null)
    // the wait NEVER runs under access_mtx_: the call repeatedly
    //   snapshot -> poll (serialized) -> park in the observe-only ready wait
    // and returns the count of Completions reaped (a plain >0 progress), or
    // 0 when the wait was interrupted by the control plane (close_admission /
    // interrupt_backend_waiters) with nothing reaped — 0 is NOT an error and
    // fabricates no completion. A 0 with no outstanding work is also
    // returned (an empty wait is a no-progress boundary, never a park).
    // Without the capability the legacy serialized contract applies (the whole
    // call, including a backend-side block, runs under access_mtx_).
    //
    // Control-wake theorem: the CONTROL baseline is captured ONCE
    // at the start of this external invocation and held fixed for its whole
    // duration — a control-plane wake landing any time after the call began
    // (including the inter-iteration window between wait_for_change() returning
    // `progress` and the next internal snapshot) is observed as interrupted
    // and the call terminates with its final poll. The PROGRESS baseline may
    // refresh per internal loop. A FUTURE wait_one() captures a fresh baseline,
    // so the interrupt stays one-shot — never a sticky shutdown flag, never a
    // busy-spin (the one-shot control-generation contract).
    //
    // Commit-to-park handshake: the invocation baseline comes from
    // consume_committed_wait() — a Scheduler MW-S2 participant that armed a
    // committed-wait registration at its Phase-B commit (under global_mtx_,
    // before this call) uses the ARMED control generation as its baseline, so
    // a control wake published after the commit is observed even if it landed
    // before this call entered. Without a registration the behavior is
    // unchanged (a fresh snapshot at entry).
    Result<std::size_t> wait_one();

    // Bounded-park variant: identical split-phase semantics to
    // wait_one(), with each physical park inside the observe phase capped at
    // `max_park` so the caller can guarantee a re-drain before an active
    // deadline (the timer pump drives the Scheduler's deadline set; the
    // MIXED-WAKE progress participant parks in wait_one() and must still
    // return in time for pump_deadlines_locked). `max_park` must be finite
    // (pass nanoseconds::max() only through the no-argument form, which is
    // unbounded). Backends without the split wait capability ignore the bound
    // (legacy serialized contract, unchanged). A backend WITH a wait source
    // but WITHOUT the bounded transport (BackendWaitSource::
    // supports_bounded_wait() == false) returns not_supported SYNCHRONOUSLY:
    // a silently discarded deadline cap is a liveness hole, never a degraded
    // mode. Callers with a deadline obligation check
    // has_bounded_split_wait_capability() first.
    Result<std::size_t> wait_one(std::chrono::nanoseconds max_park);

    // Does the backend expose the split-phase readiness wait? The
    // Scheduler selects the MW-S2 park domain with this: a split-wait backend
    // parks the progress participant in wait_one() for BOTH backend-only and
    // MIXED-WAKE (external wake publications reach the park through
    // interrupt_backend_waiters); a non-split-wait backend keeps the
    // Scheduler-domain bounded observation park in MIXED-WAKE (its readiness
    // is poll-driven and cannot self-notify).
    bool has_split_wait_capability() const noexcept;

    // Split wait AND a bounded physical park
    // (BackendWaitSource::supports_bounded_wait()). The Scheduler's MW-S2
    // participant may park in the BACKEND domain with a finite cap only when
    // this holds; otherwise a deadline-bound park uses the Scheduler wake
    // domain, whose transport the Scheduler itself bounds. Lock-free and
    // construction-stable, like has_split_wait_capability().
    bool has_bounded_split_wait_capability() const noexcept;

    // Control-plane wake for a split-phase wait in progress: wakes
    // every participant parked in wait_one()'s observe phase so shutdown /
    // admission close can re-evaluate. Per the control-wake theorem
    // the wake is observed by ANY wait_one() invocation in flight at the time
    // of the interrupt (control baseline is per-invocation), including one
    // that has just returned from a progress wake and is between internal
    // iterations. No-op for backends without the wait capability. Never
    // fabricates readiness, never touches request state, and never blocks on
    // access_mtx_.
    void interrupt_backend_waiters() noexcept;

    // Commit-to-park handshake: register the mandatory
    // control-observation baseline with the backend wait source for the NEXT
    // wait_one() invocation. Called by the Scheduler's MW-S2 participant at
    // its Phase-B commit, under global_mtx_ and BEFORE releasing the
    // admission authority — so a runtime stop (request_stop ->
    // interrupt_backend_waiters) landing between the commit and the
    // participant's wait_one() entry is observed by that invocation instead
    // of being rebaselined as a past event. One-shot: consumed by the next
    // wait_one(); a future invocation captures a fresh baseline (the
    // interrupt stays one-shot). No-op for backends without the split wait
    // capability; never blocks on access_mtx_. Internal-use counterpart of
    // interrupt_backend_waiters().
    void arm_backend_wait_commit() noexcept;

    void cancel(Completion<std::size_t>& c);
    void cancel(Completion<void>& c);

    // Attach the identity-bearing ReadySink the backend delivers
    // reap events to (null restores the no-op default). Serialized under
    // access_mtx_ so attachment never races an in-flight poll/reap of the same
    // backend. The Scheduler installs its routing sink here at construction
    // and detaches at destruction (ApplicationRuntime destroys sched_ before
    // io_ctx_; standalone contexts keep the no-op sink).
    void set_ready_sink(detail::SynchronousReadySink* sink);

    // Production waiter registration / cancellation (ADR Decision
    // 10). See AsyncBackend::register_waiter / cancel_waiter for semantics;
    // these forward under access_mtx_ (the serialized backend domain).
    Result<void> register_waiter(Completion<std::size_t>& c,
                                 detail::WaiterToken token,
                                 detail::RoutingLease lease);
    Result<void> register_waiter(Completion<void>& c,
                                 detail::WaiterToken token,
                                 detail::RoutingLease lease);
    Result<detail::RoutingLease> cancel_waiter(Completion<std::size_t>& c);
    Result<detail::RoutingLease> cancel_waiter(Completion<void>& c);

    std::size_t outstanding() const noexcept;
    const AsyncStats* stats() const noexcept { return stats_; }

#if defined(SLUICE_ASYNC_INTERNAL_TESTING)
    // Phase G park-window forensics: the backend wait source's current
    // (progress_generation, control_generation) pair — the backend
    // publication sequence and the control-interrupt sequence compared
    // against a park ledger baseline. Zeros when the backend has no wait
    // source (non-split-wait).
    BackendWaitToken backend_wait_token_for_test() const noexcept;

    // Deterministic context-level pause (D4-RM13 detector seam; C4 / issue
    // #135): the WaitSourceProgressPauseGate definition and the seam bodies
    // moved to the NON-INSTALLED seam header
    // src/async/async_io_context_test_seams.hpp (included at the bottom of
    // this file under this same guard). Compiled out of production builds;
    // the layout cost in the internal-testing target is accepted and
    // documented (AGENTS.md §15).
    struct WaitSourceProgressPauseGate;
    void set_wait_source_progress_pause_gate_for_test(
        WaitSourceProgressPauseGate* gate) noexcept;
    static void resume_wait_source_progress_gate_for_test(
        WaitSourceProgressPauseGate& gate) noexcept;
#endif

private:
    std::unique_ptr<AsyncBackend> backend_;
    AsyncStats* stats_;
    // Serialized backend access domain: at most one caller CONSUMES
    // AsyncBackend at a time (submit_*/cancel/poll/reap/outstanding all
    // acquire this). The split-phase wait's OBSERVE phase (wait_for_change)
    // intentionally runs outside it — it is a pure epoch wait that touches no
    // backend state, so it may run concurrently with serialized consuming
    // operations. No context-level lock is ever held across an unbounded
    // block.
    mutable std::mutex access_mtx_;

#if defined(SLUICE_ASYNC_INTERNAL_TESTING)
    // D4-RM13 detector seam state (see WaitSourceProgressPauseGate). Compiled
    // out of production builds; the layout cost in the internal-testing target
    // is accepted and documented (AGENTS.md §15).
    std::atomic<WaitSourceProgressPauseGate*> wait_source_progress_gate_{nullptr};
    void pause_after_wait_source_progress_() noexcept;
#endif
};

}  // namespace sluice::async

#if defined(SLUICE_ASYNC_INTERNAL_TESTING)
// C4 (issue #135): the complete internal-testing control plane for
// AsyncIoContext lives in the NON-INSTALLED seam header
// src/async/async_io_context_test_seams.hpp, resolved via the
// internal-testing-only include path. Production TUs never compile this
// include.
#include "async_io_context_test_seams.hpp"
#endif  // defined(SLUICE_ASYNC_INTERNAL_TESTING)
