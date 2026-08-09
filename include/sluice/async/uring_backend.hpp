// sluice::async::UringAsyncBackend (sluice-CORE-020B, ADR §4 Option 4).
//
// The Linux io_uring backend. Phase D1 migrates it onto the bounded
// RequestArena / RequestSlot lifecycle with a PRIVATE io_uring ring per
// backend instance (ADR Decision 18 — Uring execution-ownership amendment):
//
//   RequestArena            = logical request lifecycle / generation / terminal
//   one private io_uring    = execution ownership domain
//   io_uring_submit()       = transport progress only (NO RequestState change)
//   original operation CQE  = execution retirement / terminal candidate
//   RequestArena::reap()    = sole Completion-ready publication authority
//
// GATED behind liburing (ADR §11 D4 — optional dep):
//   * SLUICE_HAS_LIBURING defined (liburing linked): real io_uring path.
//   * otherwise: UNSUPPORTED STUB. submit_* returns IoError::backend_error
//     synchronously; poll()/wait_one() reap nothing. The project builds with
//     no liburing dependency.
//
// Cancel (ADR Decision 11, layered): pending/enqueued cancel may win the
// canceled terminal directly (Scheme B) — its operation SQE was NEVER
// installed into the ring and cannot execute. running/ring-owned cancel
// records intent only and may append an IORING_OP_ASYNC_CANCEL whose CQE is
// CONTROL-INFORMATIONAL (res ∈ {0, -ENOENT, -EALREADY}); it MUST NOT publish
// a terminal or release the slot. The original operation CQE decides the
// terminal (success / ordinary error / -ECANCELED).
//
// Resource bounds (AC-7, ADR Decision 13) — DISTINCT resources:
//   request_capacity : arena slots == dispatch ring entries == CqeRouter slots
//   queue_depth      : io_uring SQ/CQ depth (kernel-owned)
//   request_capacity > queue_depth is LEGAL; excess accepted work stays
//   enqueued locally until an SQE is available.
//
// Shutdown (ADR Decision 15; AGENTS.md §14): destruction is quiescent. The
// destructor tears down the ring; it does NOT implicitly cancel, drain, wait
// for a CQE, publish, or discard accepted work. Non-quiescent destruction is
// a contract violation.
//
// See docs/architecture/phase-d1-uring-frozen-design.md (the frozen design /
// compliance gate). State is instance-owned (no globals).
#pragma once

#include <sluice/async/async_io_context.hpp>
#include <sluice/async/completion.hpp>
#include <sluice/async/detail/reference_ready_sink.hpp>
#include <sluice/async/detail/request_arena.hpp>
#include <sluice/error.hpp>
#include <sluice/result.hpp>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <vector>

#if defined(SLUICE_HAS_LIBURING) && defined(SLUICE_ASYNC_INTERNAL_TESTING)
struct io_uring;
#endif

namespace sluice::async {

#if defined(SLUICE_HAS_LIBURING)
// Opaque pimpl holding the io_uring instance + transport state. Defined in the
// .cpp so this header never needs <liburing.h> (the experimental gate defines
// SLUICE_HAS_LIBURING without requiring liburing headers in includers).
struct UringRingState;
#endif

#if defined(SLUICE_HAS_LIBURING)
// Phase D1 configuration (AC-7, ADR Decision 13). request_capacity MUST be in
// [1, UINT32_MAX] (the SlotIndex domain); queue_depth MUST be > 0. Validation
// completes before any backend-state allocation.
// request_capacity is independent of queue_depth (ADR Decision 13 / 18);
// request_capacity > queue_depth is legal.
struct UringConfig {
    std::size_t request_capacity = 64; // arena + dispatch ring + router capacity
    unsigned queue_depth = 64;         // io_uring SQ/CQ depth
};
#endif

#if defined(SLUICE_HAS_LIBURING) && defined(SLUICE_ASYNC_INTERNAL_TESTING)
// Non-installed transport submit/wait seams used by the dedicated
// real-liburing fault tests.
// Production targets never define SLUICE_ASYNC_INTERNAL_TESTING and therefore
// expose neither this type nor the constructor overload below.
struct UringBackendSubmitTestHooks {
    using SubmitFn = int (*)(void*, ::io_uring*) noexcept;
    using SubmitAndWaitFn = int (*)(void*, ::io_uring*, unsigned) noexcept;
    using BeforePoisonWaitFn = void (*)(void*) noexcept;

    void* context = nullptr;
    SubmitFn submit = nullptr;
    SubmitAndWaitFn submit_and_wait = nullptr;
    BeforePoisonWaitFn before_poison_wait = nullptr;
};
#endif

class UringAsyncBackend : public AsyncBackend {
  public:
    // Legacy source-compatible constructor: maps to
    // UringConfig{request_capacity == queue_depth, queue_depth}. Stub mode
    // (no liburing) ignores depth and reports available()==false.
    explicit UringAsyncBackend(unsigned queue_depth = 64);

#if defined(SLUICE_HAS_LIBURING)
    // Phase D1 explicit bounded configuration. request_capacity MUST be in
    // [1, UINT32_MAX] and queue_depth MUST be > 0. Invalid configuration is
    // rejected with std::invalid_argument before backend-state allocation.
    explicit UringAsyncBackend(UringConfig config);
#endif
#if defined(SLUICE_HAS_LIBURING) && defined(SLUICE_ASYNC_INTERNAL_TESTING)
    UringAsyncBackend(UringConfig config, UringBackendSubmitTestHooks hooks);
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

    // Whether this instance initialized a real io_uring (false in stub mode
    // or when the host kernel cannot create a ring). This is a capability
    // query, not a health query.
    bool available() const noexcept;

#if defined(SLUICE_HAS_LIBURING)
    // Phase D1 does NOT override wait_source(): Uring is a single-driver
    // backend whose wait_one() blocks in the kernel (io_uring_submit_and_wait)
    // and reaps CQEs synchronously on the calling thread. There is no separate
    // worker thread to signal a ReadyWaitSource, so declaring split-wait
    // capability would make AsyncIoContext::wait_one park in wait_for_change
    // forever. A future shard/M:N topology with a dedicated CQE-reaper thread
    // may revisit this; D1 leaves wait_source() returning nullptr (the default).
    // close_admission() also remains the default (no-op) for D1; D4 owns the
    // full close/drain redesign.

    // Phase D1 resource introspection (method-only seams; no member data).
    std::size_t arena_capacity() const noexcept { return arena_.capacity(); }
    std::size_t arena_slot_in_use() const noexcept { return arena_.slot_in_use(); }
    std::size_t arena_accepted_outstanding() const noexcept {
        return arena_.accepted_outstanding();
    }
    std::size_t arena_capacity_rejections() const noexcept { return arena_.capacity_rejections(); }
    std::size_t configured_queue_depth() const noexcept { return queue_depth_; }
#endif

#if defined(SLUICE_HAS_LIBURING) && defined(SLUICE_ASYNC_INTERNAL_TESTING)
    // Test-only: number of io_uring_submit() transport flushes actually issued
    // (proves submit is transport progress, decoupled from lifecycle).
    std::uint64_t submit_flushes_for_test() const noexcept {
        return submit_flushes_.load(std::memory_order_relaxed);
    }
    // Test-only: live operation cookies in the CqeRouter (bounded by
    // request_capacity).
    std::size_t live_cookies_for_test() const noexcept {
        return live_cookies_.load(std::memory_order_relaxed);
    }
    // Test-only: route a synthetic CQE (cookie + res) through the same
    // handle_one_cqe path a real CQE takes. Used by the stale-cookie detector
    // to prove a retired cookie no longer matches any LIVE router entry and is
    // dropped (P0-B ABA fix). Does NOT touch the io_uring ring; it injects the
    // CQE directly into the routing/terminal layer.
    void inject_cqe_for_test(std::uint64_t cookie, int res) noexcept {
        handle_one_cqe(cookie, res);
    }
    // Test-only: read the next operation cookie that WILL be allocated by the
    // next dispatch_one_locked without advancing the counter. Lets a test
    // predict the cookie an in-flight op will carry so it can inject a stale
    // cookie distinct from it. (next_cookie_ is mutated only under
    // dispatch_mtx_; this snapshot is read single-driver.)
    std::uint64_t peek_next_cookie_for_test() const noexcept { return next_cookie_; }
    // Test-only, single-driver read-only observation of the live router. Used
    // to prove SQ-pressure enqueue dispatches the FIFO front rather than the
    // newly appended tail. Offsets are unique in that detector.
    std::optional<std::uint64_t>
    live_cookie_for_offset_for_test(std::uint64_t offset) const noexcept {
        for (const auto& entry : router_) {
            if (entry.in_use && prepared_ops_[entry.handle.slot.value].offset == offset)
                return entry.cookie;
        }
        return std::nullopt;
    }
    // Test-only: validate a WriteOp through the EXACT production descriptor-
    // validation logic, WITHOUT reserve/prepare/commit/enqueue/get_sqe/kernel.
    // A read-only static wrapper over validate_write; it touches no instance
    // state, performs no syscall, and never reaches the ring. Used by the
    // UINT_MAX length-boundary detector to prove the inclusive validation
    // boundary without driving a huge real I/O to completion (the unsafe
    // ring-owned-then-cancel evidence it replaces).
    static Result<void> validate_write_for_test(WriteOp op) noexcept {
        return validate_write(op);
    }
    // Phase D2 read-only bounded-state observations. These expose no mutation
    // authority and add no member data; their out-of-line definitions are
    // compiled only into internal-testing builds.
    std::size_t dispatch_size_for_test() const noexcept;
    std::size_t transport_ledger_size_for_test() const noexcept;
    std::size_t sq_ready_for_test() const noexcept;
    std::size_t live_control_entries_for_test() const noexcept;
    // Test-only: number of backend_ready slots not yet reaped.
    std::size_t backend_ready_count_for_test() const noexcept {
        return arena_.backend_ready_count();
    }
    // Test-only: live tagged control execution references (submitted
    // AsyncCancel SQEs not yet retired by their control CQE).
    std::size_t live_control_sqes_for_test() const noexcept {
        return live_control_sqes_.load(std::memory_order_relaxed);
    }

    // --- Phase D3 C2b/C2c seams (rows 3-8 / 11-14a): mirror the approved
    // ThreadPool observation style. Every seam delegates to REAL production
    // authority (RequestArena, ReferenceReadySink, the production cancel
    // core). No test-side state machine, no side-band identity/waiter map, no
    // second generation counter. Guarded; production builds carry nothing. ---

    // Resolve a Completion pointer to its current slot+generation (the same
    // bounded arena scan the public cancel path uses).
    std::optional<detail::SlotHandle> handle_for_completion_for_test(
        const void* completion) const noexcept {
        return arena_.resolve_completion(completion);
    }

    // Single-lock observation that validates generation, context, and non-free
    // state. Returns nullopt for a stale/released/unknown handle.
    std::optional<detail::RequestArena::RequestObservation> observe_for_test(
        detail::SlotHandle h) const noexcept {
        return arena_.observe_for_test(h);
    }

    // Identity-injection seam (C2b row 4): drive a CAPTURED SlotHandle through
    // the SAME production cancel core the public Completion-keyed cancel() uses
    // (dispatch remove_exact + arena_.cancel + terminal_won tally/signal).
    // Proves a stale-generation handle cannot act on a live N+1 occupant.
    detail::CancelDisposition cancel_handle_for_test(detail::SlotHandle h) noexcept {
        return cancel_handle_(h);
    }

    // Register one waiter on the slot bound to a real accepted Completion.
    // Forwards verbatim to the arena authority (not_found for an unbound/stale
    // Completion; invalid_state for a second registration or an
    // already-reaped slot — registration is orthogonal to execution state,
    // ADR Decision 10).
    Result<void> register_waiter_for_test(Completion<std::size_t>& c,
                                          detail::WaiterToken token,
                                          detail::RoutingLease lease) {
        auto h = arena_.resolve_completion(&c);
        if (!h.has_value()) {
            return make_unexpected<void>(IoError{IoError::Code::not_found});
        }
        return arena_.register_waiter(*h, token, std::move(lease));
    }
    Result<void> register_waiter_for_test(Completion<void>& c,
                                          detail::WaiterToken token,
                                          detail::RoutingLease lease) {
        auto h = arena_.resolve_completion(&c);
        if (!h.has_value()) {
            return make_unexpected<void>(IoError{IoError::Code::not_found});
        }
        return arena_.register_waiter(*h, token, std::move(lease));
    }

    // Wait-cancel through the REAL arena authority: removes ONLY the waiter,
    // never the I/O. Returns the moved-out RoutingLease, or not_found when no
    // registered waiter remains.
    Result<detail::RoutingLease> cancel_waiter_for_test(Completion<std::size_t>& c) {
        auto h = arena_.resolve_completion(&c);
        if (!h.has_value()) {
            return make_unexpected<detail::RoutingLease>(
                IoError{IoError::Code::not_found});
        }
        return arena_.cancel_waiter(*h);
    }
    Result<detail::RoutingLease> cancel_waiter_for_test(Completion<void>& c) {
        auto h = arena_.resolve_completion(&c);
        if (!h.has_value()) {
            return make_unexpected<detail::RoutingLease>(
                IoError{IoError::Code::not_found});
        }
        return arena_.cancel_waiter(*h);
    }

    // Stale-generation waiter injection (C2c row 14a): drive a CAPTURED
    // SlotHandle through the REAL arena register/cancel_waiter authorities.
    Result<void> register_waiter_handle_for_test(detail::SlotHandle h,
                                                 detail::WaiterToken token,
                                                 detail::RoutingLease lease) {
        return arena_.register_waiter(h, token, std::move(lease));
    }
    Result<detail::RoutingLease> cancel_waiter_handle_for_test(detail::SlotHandle h) {
        return arena_.cancel_waiter(h);
    }

    // Generation-validated by-value borrow snapshot for a captured SlotHandle.
    std::optional<detail::RequestArena::BorrowSnapshot> borrow_for_test(
        detail::SlotHandle h) const noexcept {
        return arena_.borrow_for_test(h);
    }

    // Generation-validated by-value single-waiter registration observation.
    std::optional<detail::RequestArena::WaiterObservation> waiter_for_test(
        detail::SlotHandle h) const noexcept {
        return arena_.waiter_for_test(h);
    }

    // C2c sink observation (fixed-size, allocation-free, test-only): the last
    // delivered ReadyEvent's waiter payload + total delivery count.
    std::size_t sink_deliveries() const noexcept { return sink_.deliveries(); }
    bool sink_last_has_waiter() const noexcept { return sink_.last_has_waiter(); }
    detail::WaiterToken sink_last_token() const noexcept { return sink_.last_token(); }
    std::uint64_t sink_last_lease_id() const noexcept { return sink_.last_lease_id(); }

    // Deterministic pause gates for the Uring race tests (mirror the
    // ThreadPool pause-gate discipline; AGENTS.md §13.3 / §15). Each gate is
    // a paused/resume atomic handshake; the production path spins on `paused`
    // and waits on `resume`. Compiled out of production sluice_async; the
    // layout cost in the internal-testing target is accepted and documented.
    struct AfterCommitBeforeEnqueuePauseGate {
        std::atomic<bool> paused{false};
        std::atomic<bool> resume{false};
        std::atomic<bool> exited{false};
    };
    struct BeforeDispatchTransferPauseGate {
        std::atomic<bool> paused{false};
        std::atomic<bool> resume{false};
        std::atomic<bool> exited{false};
        // true iff the gate fired with dispatch_mtx_ RELEASED (mirrors the
        // ThreadPool Gate-B discipline: the request stays enqueued while the
        // test drives cancel() against it).
        std::atomic<bool> dispatch_domain_released{false};
    };
    void set_after_commit_before_enqueue_pause_gate(AfterCommitBeforeEnqueuePauseGate* gate) noexcept {
        after_commit_before_enqueue_gate_.store(gate, std::memory_order_release);
    }
    void set_before_dispatch_transfer_pause_gate(BeforeDispatchTransferPauseGate* gate) noexcept {
        before_dispatch_transfer_gate_.store(gate, std::memory_order_release);
    }
#endif

  private:
#if defined(SLUICE_HAS_LIBURING)
    struct ValidatedConfigTag {};
    static UringConfig validate_config_(UringConfig config);
    UringAsyncBackend(UringConfig config, ValidatedConfigTag);

    // ---- fixed tagged operation payload (per-slot scratch, Scheme B) -------
    // Sized to request_capacity at construction, indexed by SlotIndex (1:1 with
    // arena slots). Carries the SQE descriptor; filled in prepare(), read by
    // dispatch after the slot is current-generation enqueued.
    struct PreparedUringOp {
        detail::OperationKind kind = detail::OperationKind::read;
        int fd = -1;
        const std::byte* buffer = nullptr; // dst (read) / src (write) / null (sync)
        std::size_t length = 0;            // requested length (for short-completion tally)
        unsigned native_length = 0;        // liburing nbytes (validated <= UINT_MAX at prepare)
        std::uint64_t offset = 0;
    };

    // Bounded op_cookie -> full SlotHandle router (frozen design §7.2).
    // Construction-time capacity == request_capacity. NOT a request store: it
    // is transport routing metadata (ADR Decision 3 backend scratch). The arena
    // re-validates the full handle before any mutation.
    //
    // Kernel-visible identity discipline (P0-B): the SQE user_data carries the
    // COOKIE VALUE, not a router array index. The cookie is allocated from a
    // no-wrap 64-bit counter and is NEVER reused within backend lifetime (mirors
    // RequestArena generation no-wrap). The router ARRAY slot is recycled via a
    // free-list, but because routing keys on the cookie value, a stale CQE
    // (whose cookie belongs to a retired entry) no longer matches any live
    // entry and is dropped — the ABA window that existed when user_data carried
    // router_slot+1 is closed. The arena still re-validates the full generation
    // as a second layer of defense. The high bit is reserved for tagged cancel-
    // control user_data, so operation cookies occupy [1, 2^63-1].
    struct RouterEntry {
        enum class ControlState : std::uint8_t { none, prepared, submitted };

        std::uint64_t cookie = 0; // 0 = not a live operation cookie
        detail::SlotHandle handle{};
        detail::TerminalResult deferred_terminal{};
        ControlState control_state = ControlState::none;
        bool deferred_terminal_stored = false;
        bool in_use = false;
    };

    // Bounded local dispatch ring (capacity == request_capacity). Stores
    // SlotHandle only; push_back/front/remove_exact are noexcept. Mirrors the
    // ThreadPool BoundedDispatchQueue discipline.
    class BoundedDispatchQueue;

    // Bounded prepared-but-not-confirmed-consumed physical SQ ledger. Its
    // capacity is the ACTUAL initialized ring.sq.ring_entries (not configured
    // queue_depth); defined in the .cpp to keep liburing out of this header.
    // It is transport evidence only and never owns RequestState/terminal/
    // Completion authority.
    class TransportLedger;

    // Process-wide monotonic id for ContextIdentity provenance (distinct per
    // UringAsyncBackend instance — ADR Decision 2).
    static std::uint64_t next_backend_id() noexcept {
        static std::atomic<std::uint64_t> id{0x55720000u}; // 'Ur' provenance tag
        return ++id;
    }

    // Descriptor validation for a REAL syscall backend (ADR Decision 6;
    // AGENTS.md §9.1 — no fcntl(F_GETFD) preflight TOCTOU).
    static Result<void> validate_read(ReadOp op);
    static Result<void> validate_write(WriteOp op);
    static Result<void> validate_sync(SyncDataOp op);
    static Result<void> validate_sync(SyncAllOp op);
    template <class Op> static Result<void> validate_op(const Op& op) noexcept;

    // Five-stage admission mirroring ThreadPoolBackend (ADR Decision 5).
    template <class Op>
    Result<void> submit_size(Op op, Completion<std::size_t>& c, detail::OperationKind kind,
                             std::size_t len);
    template <class Op>
    Result<void> submit_void(Op op, Completion<void>& c, detail::OperationKind kind);

    template <class Op> static detail::BorrowMetadata borrow_of(const Op& op) noexcept {
        if constexpr (std::is_same_v<Op, ReadOp>) {
            return {op.fd, op.dst, op.len};
        } else {
            return {op.fd, op.src, op.len};
        }
    }

    // Publication thunks (ADR Decision 9): convert the arena's TerminalResult
    // to a Result<T> and publish Completion-ready through the protected helper.
    // Static + type-erased; the arena calls them inside the leaf domain at reap.
    static void publish_size_ready(void* completion, const detail::TerminalResult& t) noexcept;
    static void publish_void_ready(void* completion, const detail::TerminalResult& t) noexcept;
    static Result<std::size_t> terminal_to_size(const detail::TerminalResult& t) noexcept;
    static Result<void> terminal_to_void(const detail::TerminalResult& t) noexcept;

    // Dispatch one enqueued request toward ring ownership (frozen design §4).
    // Acquires dispatch_mtx_. Returns false if the request could not be
    // dispatched this pass (SQ full / fatal); true if it became ring-owned.
    bool dispatch_one(detail::SlotHandle h) noexcept;
    // P0-A peek protocol: assumes dispatch_mtx_ is held AND h == dispatch_
    // ->front(). The caller peeks the front and does NOT remove it before this
    // call; on a successful transfer this function removes h exactly once via
    // dispatch_->remove_exact(h) (a miss is an invariant violation -> fail-fast).
    // On a NULL SQE the function returns false WITHOUT mutating the queue (h
    // stays at the front). Used by the poll()/wait_one() peek drains and by
    // enqueue_after_commit's single-critical-section path.
    bool dispatch_one_locked(detail::SlotHandle h) noexcept;

    // Unified enqueue + FIFO-front dispatch drain under ONE dispatch_mtx_
    // critical section. noexcept; the caller has already committed. Holding
    // the lock across push_back -> front/dispatch_one_locked is load-bearing:
    // it closes the window in which cancel() could terminalize the front
    // between enqueue and dispatch (cancel takes the same lock). Therefore
    // mark_running(front)==false inside the transaction is an invariant
    // violation, not a cancel-won race.
    void enqueue_after_commit(detail::SlotHandle h) noexcept;

    // Transport progress under dispatch_mtx_. Positive results retire the
    // physical-ledger prefix as transport evidence only. A permanent negative
    // result invokes the separate proof-consuming recovery controller; the
    // syscall itself remains lifecycle-neutral.
    int submit_transport_locked() noexcept;

    // Classify a submit/submit-and-wait result while dispatch_mtx_ is held.
    // `had_pending_transport` distinguishes a submission failure (eligible for
    // the D1 zero-consumption theorem) from a pure wait error with to_submit=0.
    void account_transport_result_locked(int rc, bool had_pending_transport) noexcept;

    // Consume the proven-zero-consumption theorem after a permanent negative
    // submit: poison admission, locally retire the exact Class-A physical
    // ledger and the never-dispatched FIFO, but leave positively submitted
    // Class-C operation/control entries bound for their CQEs.
    void poison_and_recover_locked(IoError error) noexcept;

    // Poisoned wait progress. Direct enter with to_submit=0 is load-bearing:
    // it may wait/reap old Class-C CQEs but cannot submit the quarantined tail.
    int wait_cqe_without_submit() noexcept;

    // Reap all currently-ready CQEs: route op cookies to full SlotHandles,
    // validate generation, record_terminal ONLY (never publish). Control
    // cancel CQEs update only fixed cancel bookkeeping. Returns the count of
    // NON-CONTROL CQEs observed (a stale/unknown cookie is dropped without
    // recording a terminal but is still counted here). Production callers
    // discard this value; it exists for bounded diagnostics only.
    std::size_t reap_cqes() noexcept;

    // Decode one CQE: an operation terminal is recorded or deferred until its
    // tagged control retires; a control may release that deferred terminal.
    void handle_one_cqe(std::uint64_t user_data, int res) noexcept;

    // Publish a previously decoded operation terminal into RequestArena and
    // retire its router entry. Called immediately when no control reference is
    // live, or after the matching tagged control CQE/recovery retires it.
    void finalize_operation_terminal_(std::size_t router_index,
                                      const detail::TerminalResult& terminal) noexcept;

    // Allocate a unique operation cookie from the no-wrap counter. Domain is
    // [1, 2^63-1]; 0 is unused and the high bit is reserved for tagged control
    // identities. If the counter would reach the control tag, fail-fast
    // (mirrors RequestArena generation no-wrap discipline) — never wrap. The
    // cookie is NEVER reused within backend lifetime, so a stale CQE's cookie
    // cannot match a later LIVE entry.
    std::uint64_t allocate_cookie_() noexcept;

    // Find the router ARRAY index of the LIVE entry whose SlotHandle matches h
    // (slot + full generation). Returns the index, or request_capacity (==
    // router_.size()) if no live entry matches (h is not currently ring-owned).
    // Bounded O(request_capacity) scan, allocation-free.
    std::size_t find_live_router_index_(detail::SlotHandle h) const noexcept;
    std::size_t find_live_router_cookie_(std::uint64_t cookie) const noexcept;
    void retire_router_entry_(std::size_t router_index) noexcept;

    // Per-slot backend cancel bookkeeping. The arena owns cancel_intent_
    // (intent authority); this struct only tracks whether an AsyncCancel SQE
    // has already been appended for this slot, so repeated cancel() calls do
    // not enqueue unbounded cancel SQEs (frozen design §8.2).
    struct CancelScratch {
        bool cancel_queued = false;
    };

    // Cancel bookkeeping + best-effort AsyncCancel SQE append for a running
    // request. Idempotent per-slot (one cancel_queued bit).
    void issue_running_cancel(detail::SlotHandle h) noexcept;

    // The production cancel core (ADR Decision 11 / Scheme B) shared by the
    // public Completion-keyed cancel() overloads and the guarded
    // cancel_handle_for_test seam: DISARM LOCAL EXECUTION FIRST (dispatch
    // remove_exact under dispatch_mtx_), TERMINAL WIN SECOND (arena_.cancel),
    // then tally + signal on terminal_won / append the bounded AsyncCancel on
    // intent_recorded. Behavior-preserving refactor of the inline cancel()
    // bodies; no test code reimplements state transitions.
    detail::CancelDisposition cancel_handle_(detail::SlotHandle h) noexcept;

#if defined(SLUICE_ASYNC_INTERNAL_TESTING)
    // Deterministic pause helpers (no-op when the matching gate is disarmed).
    void wait_after_commit_before_enqueue_pause_() noexcept;
    void wait_before_dispatch_transfer_pause_() noexcept;
#endif

    // Wake the ready domain. D1 is single-driver (wait_one blocks in the
    // kernel and reaps synchronously), so there is no separate worker to wake;
    // this is a no-op kept as a seam for a future shard/M:N topology with a
    // dedicated CQE-reaper thread.
    void signal_ready_progress() noexcept {}

    // ---- members -----------------------------------------------------------
    detail::RequestArena arena_;
    detail::ReferenceReadySink sink_;
    std::vector<PreparedUringOp> prepared_ops_;       // size == request_capacity
    std::vector<RouterEntry> router_;                 // size == request_capacity
    std::vector<CancelScratch> cancel_scratch_;       // size == request_capacity
    std::vector<detail::SlotIndex> cookie_free_list_; // free router slots
    std::uint64_t next_cookie_ = 1; // 0 reserved; high bit reserved for tagged control identity
    unsigned queue_depth_ = 64;

    // The private io_uring instance (opaque pimpl — owns the io_uring + the
    // internal-testing transport hooks). One ring per backend (ADR Decision 18).
    std::unique_ptr<UringRingState> ring_state_;
    std::unique_ptr<TransportLedger> transport_ledger_;
    bool have_ring_ = false;
    bool admission_closed_ = false;
    std::optional<IoError> fatal_error_; // permanent transport poison

    // Backend dispatch domain: local dispatch ring + dispatch/cancel
    // arbitration + cookie/router installation and cancel-side lookup/scratch
    // mutation. CQE lookup/retirement is serialized by D1's documented
    // AsyncIoContext single-driver call domain and intentionally does not take
    // this mutex before arena.record_terminal(). Lock order:
    // dispatch_mtx_ -> arena leaf only — never nested with the ready-wait
    // mutex. io_uring_submit() (syscall) is transport progress and may be
    // called under dispatch_mtx_ but NEVER under the arena mutex.
    mutable std::mutex dispatch_mtx_;
    std::unique_ptr<BoundedDispatchQueue> dispatch_;

    std::atomic<std::uint64_t> submit_flushes_{0};
    std::atomic<std::size_t> live_cookies_{0};
    std::atomic<std::size_t> live_control_sqes_{0};

#if defined(SLUICE_ASYNC_INTERNAL_TESTING)
    // D3 deterministic pause gates (see the guarded setters above). Compiled
    // out of production builds; the layout cost in the internal-testing target
    // is accepted and documented (AGENTS.md §15).
    std::atomic<AfterCommitBeforeEnqueuePauseGate*> after_commit_before_enqueue_gate_{nullptr};
    std::atomic<BeforeDispatchTransferPauseGate*> before_dispatch_transfer_gate_{nullptr};
#endif
#endif // SLUICE_HAS_LIBURING

    bool available_ = false;
};

} // namespace sluice::async
