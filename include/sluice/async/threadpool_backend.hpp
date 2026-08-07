// sluice::async::ThreadPoolBackend (sluice-CORE-020A, Phase E).
//
// The portable, always-buildable REAL async backend: a bounded set of persistent
// blocking-I/O workers that run pread/pwrite/fdatasync/fsync. This is the
// FALLBACK where io_uring is absent (ADR §4) and is the first PRODUCTION backend
// to drive real POSIX syscalls through the bounded RequestArena / RequestSlot
// lifecycle (ADR-explicit-io-request-contract, Accepted; Phase E).
//
// Phase E replaces the legacy "one std::thread per op + std::function +
// Completion* ready deque" model (DIV-03 / DIV-12) with:
//
//   fixed count of persistent blocking-I/O workers (worker_count)
// + construction-time bounded dispatch ring (capacity == request_capacity)
// + RequestArena / RequestSlot as the ONE request lifecycle authority
// + a fixed tagged PreparedBlockingOp payload (no std::function, no Completion*)
// + worker consumes a SlotHandle, runs the syscall, records backend-ready ONLY
// + reap (poll/wait_one) is the SOLE Completion-ready publication authority
//
// See docs/design/phase-e-bounded-threadpool-backend.md (the frozen design) and
// docs/architecture/phase-e-compliance-gate.md (the evidence ledger).
//
// Resource bounds (AC-7, ADR Decision 13) — these are DISTINCT resources:
//   request_capacity  : arena slots == dispatch ring entries (backend-owned)
//   worker_count      : persistent blocking-I/O worker threads (backend-owned)
//   (Scheduler worker count, io_uring queue depth, application pipeline depth,
//    and caller-owned Completion count are all separate and unchanged here.)
//
//   worker threads created after successful construction = 0
//   dispatch storage growth after construction          = 0
//   0 <= accepted_outstanding <= request_capacity
//   0 <= active_workers       <= worker_count
//
// Cancel (ADR Decision 11, layered — AC-9): the public API is Completion-keyed;
// it resolves to a SlotHandle via the arena's bounded scan and drives the shared
// state machine. pending/enqueued cancel may win the canceled terminal directly
// (Scheme B); running cancel records intent only (best-effort, DIV-10 — no
// pthread_kill/tgkill), and the syscall's REAL result wins verbatim. A confirmed
// interruption would record err(canceled) explicitly and win; Phase E does not
// implement one. cancel never publishes Completion-ready directly.
//
// Shutdown (ADR Decision 15; AGENTS.md §14): close_admission() rejects new
// reserve with invalid_state (Completion idle, no borrow) while existing accepted
// requests continue; cancel/poll/wait_one/reap remain legal. close_admission()
// ALSO wakes any participant parked in the ready wait (issue #67): a one-shot
// control wake that re-evaluates — it never fabricates readiness or changes
// request state, and future waits park normally again (no shutdown busy-spin).
// Destruction is quiescent and fail-fast: the destructor verifies
// arena/dispatch/worker quiescence via quiescence_snapshot() before setting
// stopping_; it does NOT implicitly cancel, drain, wait for a running syscall,
// publish, or discard the queue; it only tears down the already-idle persistent
// workers. Non-quiescent destruction fail-fasts in Debug AND Release.
//
// No new dependency (std::thread/mutex/condition_vector only — ADR §11 D4).
// State is instance-owned (no globals).
#pragma once

#include <sluice/async/async_io_context.hpp>
#include <sluice/async/completion.hpp>
#include <sluice/async/detail/ready_wait_source.hpp>
#include <sluice/async/detail/reference_ready_sink.hpp>
#include <sluice/async/detail/request_arena.hpp>
#include <sluice/detail/posix_retry.hpp>
#include <sluice/error.hpp>
#include <sluice/result.hpp>

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

namespace sluice::async {

// Phase E configuration (AC-7, ADR Decision 13). Both MUST be > 0. The default
// constructor ThreadPoolBackend() uses ThreadPoolConfig{} below.
struct ThreadPoolConfig {
    std::size_t request_capacity = 64;  // arena capacity == dispatch ring capacity
    std::size_t worker_count = 4;       // persistent blocking-I/O workers
};

class ThreadPoolBackend : public AsyncBackend {
  public:
    // Default configuration (see ThreadPoolConfig).
    ThreadPoolBackend() : ThreadPoolBackend(ThreadPoolConfig{}) {}

    // Explicit bounded configuration. request_capacity and worker_count MUST be
    // > 0. Callers may derive worker_count from hardware_concurrency() and
    // clamp it to >= 1 themselves; the constructor rejects a 0 value with
    // std::invalid_argument.
    explicit ThreadPoolBackend(ThreadPoolConfig config);

    // Quiescent destruction: joins the persistent workers. MUST be preceded by
    // close_admission + drain + reset to accepted_outstanding == 0 /
    // slot_in_use == 0. Non-quiescent destruction fail-fasts (Debug AND Release).
    ~ThreadPoolBackend() override;

    ThreadPoolBackend(const ThreadPoolBackend&) = delete;
    ThreadPoolBackend& operator=(const ThreadPoolBackend&) = delete;

    Result<void> submit_read(ReadOp op, Completion<std::size_t>& c) override;
    Result<void> submit_write(WriteOp op, Completion<std::size_t>& c) override;
    Result<void> submit_sync_data(SyncDataOp op, Completion<void>& c) override;
    Result<void> submit_sync_all(SyncAllOp op, Completion<void>& c) override;

    std::size_t poll() override;
    Result<std::size_t> wait_one() override;

    void cancel(Completion<std::size_t>& c) override;
    void cancel(Completion<void>& c) override;

    std::size_t outstanding() const noexcept override;

    // Issue #67 split-phase wait capability: the backend's ready wait is a
    // pure observe-only epoch wait (see detail::ReadyWaitSource). AsyncIoContext
    // uses it to park for readiness WITHOUT holding access_mtx_, so a second
    // participant's poll/reap path always stays reachable (I7).
    BackendWaitSource* wait_source() noexcept override { return &ready_wait_; }

    // Production admission close (ADR Decision 15). New reserve() returns
    // invalid_state (Completion idle, no borrow); existing accepted requests
    // continue; cancel/poll/wait_one/reap remain legal. Wakes any participant
    // parked in the ready wait (issue #67) as a one-shot re-evaluation signal.
    // Idempotent.
    void close_admission();

    // Phase E resource introspection (method-only seams; no member data exposed).
    std::size_t arena_capacity() const noexcept { return arena_.capacity(); }
    std::size_t arena_slot_in_use() const noexcept { return arena_.slot_in_use(); }
    std::size_t arena_capacity_rejections() const noexcept {
        return arena_.capacity_rejections();
    }
    std::size_t configured_worker_count() const noexcept { return workers_.size(); }

#if defined(SLUICE_ASYNC_INTERNAL_TESTING)
    // Test-only: persistent workers spawned (== worker_count for the backend's
    // whole life; never grows). Replaces the legacy unjoined_workers_for_test
    // (the per-op thread model is gone).
    std::size_t workers_spawned_for_test() const noexcept { return workers_.size(); }
    // Test-only: active workers currently between mark_running and
    // record_terminal. Bounded by worker_count.
    std::size_t active_workers_for_test() const {
        std::lock_guard<std::mutex> lk(work_mtx_);
        return active_workers_;
    }
    // Test-only: current dispatch ring occupancy and high-water mark.
    std::size_t dispatch_size_for_test() const {
        std::lock_guard<std::mutex> lk(work_mtx_);
        return dispatch_.size();
    }
    std::size_t dispatch_high_water_for_test() const {
        std::lock_guard<std::mutex> lk(work_mtx_);
        return dispatch_.high_water();
    }
    // Test-only: number of real syscalls the workers have executed (for
    // cancel/no-execute assertions). Monotonic; not a public contract.
    std::uint64_t syscall_count_for_test() const noexcept { return syscall_count_.load(); }

    // Test-only: number of backend_ready slots not yet reaped.
    std::size_t backend_ready_count_for_test() const noexcept {
        return arena_.backend_ready_count();
    }

    // Test-only: wait-phase entry flag (issue #67 drain-starvation regression).
    // The ready wait domain stores `true` into the pointed-to atomic
    // immediately before it blocks in the ready-cv wait, so a test can
    // deterministically observe "a participant has completed its empty reap
    // and is now parked in the backend ready wait" (the exact state from
    // which the old code held access_mtx_ across the block and starved every
    // other poll/reap path). Disarm by passing nullptr. Compiled out of
    // production sluice_async.
    void set_wait_phase_flag_for_test(std::atomic<bool>* flag) noexcept {
        ready_wait_.set_wait_phase_flag(flag);
    }

    // Test-only: resolve a Completion pointer to its current slot+generation.
    // Returns nullopt if the Completion is not bound to any slot.
    std::optional<detail::SlotHandle> handle_for_completion_for_test(
        const void* completion) const noexcept {
        return arena_.resolve_completion(completion);
    }

    // Test-only: single-lock observation that validates generation, context, and
    // non-free state. Returns nullopt for a stale/released/unknown handle.
    std::optional<detail::RequestArena::RequestObservation> observe_for_test(
        detail::SlotHandle h) const noexcept {
        return arena_.observe_for_test(h);
    }

    // Test-only identity-injection seam (Phase C2b row 4): drive a CAPTURED
    // SlotHandle (typically a stale-generation handle from a released occupant)
    // through the REAL cancel authority path — remove_exact + arena_.cancel
    // under work_mtx_, then tally+signal on terminal_won — instead of the
    // pointer-keyed public cancel(Completion&). This proves a stale-generation
    // event cannot act on a live N+1 occupant of the same physical slot: handle
    // validation rejects it with not_found and no side effect (no dispatch
    // removal, no tally). Returns the disposition so the test can assert
    // not_found/already_terminal. Mirrors observe_for_test (test-only,
    // guarded; production builds carry nothing).
    detail::CancelDisposition cancel_handle_for_test(detail::SlotHandle h) noexcept {
        detail::CancelDisposition disp;
        {
            std::lock_guard<std::mutex> lk(work_mtx_);
            (void)dispatch_.remove_exact(h);  // matches the real cancel() sequence
            disp = arena_.cancel(h);
        }
        if (disp == detail::CancelDisposition::terminal_won) {
            tally_canceled();
            signal_ready_progress();
        }
        return disp;
    }

    // Deterministic pause gates for the ThreadPoolBackend race tests. Each gate
    // is armed by the test, the production path spins on `paused` and waits on
    // `resume`, giving the test an exact observation window. These are compiled
    // out of production sluice_async; the layout cost in the internal-testing
    // target is accepted and documented (AGENTS.md §15).
    struct AfterArenaEnqueueBeforeDispatchPushPauseGate {
        std::atomic<bool> paused{false};
        std::atomic<bool> resume{false};
        // Set by the production path: true iff the gate fired INSIDE work_mtx_.
        std::atomic<bool> work_domain_held{false};
        // Set false before the pause, true after dispatch_.push_back(h) completes.
        // Lets the test observe "not yet pushed" without taking work_mtx_.
        std::atomic<bool> dispatch_push_completed{false};
        // Set false when the production path enters the pause, true after it
        // leaves (the spin loop exits). The test waits on this before unbinding
        // the gate pointer from the backend so the gate object always outlives
        // every production-path access.
        std::atomic<bool> exited{true};
    };
    struct BeforeWorkerDequeuePauseGate {
        std::atomic<bool> paused{false};
        std::atomic<bool> resume{false};
        std::atomic<bool> exited{true};
    };
    struct WorkerRunningPauseGate {
        std::atomic<bool> paused{false};
        std::atomic<bool> resume{false};
        std::atomic<bool> exited{true};
    };
    struct TerminalPublicationPauseGate {
        std::atomic<bool> paused{false};
        std::atomic<bool> resume{false};
        std::atomic<bool> exited{true};
    };
    // C2d (ADR Gate 4): deterministic commit/enqueue pause. The submit path
    // pauses AFTER commit (Completion outstanding, slot `pending`, enqueue pin
    // set) and BEFORE taking work_mtx_ — the exact state from which a pending
    // cancellation wins the canceled terminal (Scheme B) and the resumed
    // enqueue observes backend_ready and acknowledges the pin as a terminal
    // no-op with no dispatch linkage. See
    // `tp_c2d_cancel_wins_before_enqueue_injection_armed`.
    struct BeforeEnqueueLockPauseGate {
        std::atomic<bool> paused{false};
        std::atomic<bool> resume{false};
        std::atomic<bool> exited{true};
    };

    void set_after_enqueue_before_push_pause_gate(
        AfterArenaEnqueueBeforeDispatchPushPauseGate* gate) noexcept {
        after_enqueue_before_push_gate_.store(gate, std::memory_order_release);
    }
    void set_before_dequeue_pause_gate(BeforeWorkerDequeuePauseGate* gate) noexcept {
        before_dequeue_gate_.store(gate, std::memory_order_release);
    }
    void set_running_pause_gate(WorkerRunningPauseGate* gate) noexcept {
        running_gate_.store(gate, std::memory_order_release);
    }
    void set_terminal_publication_pause_gate(
        TerminalPublicationPauseGate* gate) noexcept {
        terminal_publication_gate_.store(gate, std::memory_order_release);
    }
    void set_before_enqueue_lock_pause_gate(
        BeforeEnqueueLockPauseGate* gate) noexcept {
        before_enqueue_lock_gate_.store(gate, std::memory_order_release);
    }

    // --- Phase C2d seams (rows 9-10): failure injection. Compiled out of
    // production sluice_async; the layout cost in the internal-testing target
    // is accepted and documented (AGENTS.md §15). ---

    // Post-commit dispatch-failure injection. When `armed` and the submit
    // path's enqueue won (outcome == enqueued), the submit path records the
    // defined `backend_error` terminal through the arena's terminal-winner
    // authority INSTEAD of pushing the handle onto the dispatch ring — the
    // ADR Decision-12 "post-commit dispatch failure after execution ownership
    // is proven absent" winner candidate (AGENTS.md §10.5). The handle was
    // never visible to any worker (workers dequeue only under work_mtx_, which
    // the injection holds), so no worker, ring, kernel, or other executor
    // holds execution ownership; submit still returns success; reap publishes
    // the defined terminal exactly once. `fired` increments exactly once per
    // injected submit; the test reads it to distinguish "injection fired" from
    // "a cancel won first". The control object must be declared before the
    // backend and outlive it (same lifetime rule as the pause gates).
    struct DispatchFailureInjection {
        std::atomic<bool> armed{false};
        std::atomic<std::size_t> fired{0};
    };
    void set_dispatch_failure_injection(DispatchFailureInjection* injection) noexcept {
        dispatch_failure_injection_.store(injection, std::memory_order_release);
    }

    // Pre-commit stage-failure injection (ADR Gate 4: reserve / prepare /
    // commit-boundary). Each stage is armed independently; the submit path
    // checks the seam immediately BEFORE that stage's arena call and, when
    // armed, returns the stage's natural synchronous rejection WITHOUT
    // entering the stage — through the SAME rollback code the natural failure
    // path uses (reserve: nothing to roll back; prepare:
    // rollback_reserved_or_prepared; commit:
    // rollback_binding_before_accept + rollback_reserved_or_prepared). The
    // commit-boundary arm is the ONLY executable instance of
    // rollback_binding_before_accept in the corpus: a natural commit failure
    // (stale handle / non-prepared slot) is unreachable after a same-thread
    // reserve -> prepare -> begin_binding, so no well-formed test could drive
    // that branch without this seam (review P1). `*_fired` increments exactly
    // once per injected submit at that stage; the test reads it to distinguish
    // "seam fired" from a natural failure. TEST-ONLY (AGENTS.md §15):
    // production builds carry no branch, no local, no symbol (the whole seam
    // block is compiled out).
    struct SubmitStageFailureInjection {
        std::atomic<bool> fail_reserve{false};
        std::atomic<bool> fail_prepare{false};
        std::atomic<bool> fail_commit{false};
        std::atomic<std::size_t> reserve_fired{0};
        std::atomic<std::size_t> prepare_fired{0};
        std::atomic<std::size_t> commit_fired{0};
    };
    void set_submit_stage_failure_injection(
        SubmitStageFailureInjection* injection) noexcept {
        submit_stage_failure_injection_.store(injection, std::memory_order_release);
    }

    // Construction-time worker-spawn failure injection (rows 9-10; finding
    // P1-04 "no test injects thread-creation failure"). The
    // constructor-before-instance shape forces a CONTROLLED static seam: an
    // instance member cannot be configured before construction. The value is
    // the zero-based worker index whose std::thread creation must throw
    // std::system_error(errc::resource_unavailable_try_again) (mirroring a
    // real pthread_create EAGAIN); SIZE_MAX disarms. Tests MUST restore
    // SIZE_MAX via RAII even on failure; the seam is serialized (only the
    // constructing thread reads it while armed — the harness runs cases
    // sequentially in one process) and is compiled out of production builds.
    static void set_injected_worker_spawn_failure_index(std::size_t index) noexcept;
    static std::size_t injected_worker_spawn_failure_index() noexcept;

    // --- Phase C2c seams (rows 11-14): route a real accepted Completion
    // through the REAL arena waiter/borrow authorities. No side-band waiter
    // map, no reimplementation of the waiter state machine: the Completion is
    // resolved to its current SlotHandle by the arena's own bounded scan
    // (the same identity bridge the public cancel path uses) and the call is
    // forwarded verbatim. Guarded; production builds carry nothing. ---

    // Register one waiter on the slot bound to a real accepted Completion.
    // Returns the arena's own register_waiter result (not_found for an
    // unbound/stale Completion; invalid_state for a second registration or a
    // non-accepted/unreaped slot — registration is orthogonal to execution
    // state, ADR Decision 10).
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
    // never the I/O. Returns the moved-out RoutingLease (the caller owns it),
    // or not_found when no registered waiter remains (already closed by reap /
    // already canceled / unbound Completion).
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
    // SlotHandle (typically a stale-generation handle from a released
    // occupant) through the REAL arena register/cancel_waiter authorities.
    // This is what proves a stale waiter authority cannot act on a live N+1
    // occupant's registration: handle validation rejects it with not_found and
    // zero side effect. Mirrors cancel_handle_for_test (C2b) — pointer-free
    // identity only, no Completion reverse map, no side-band waiter map.
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
    // delivered ReadyEvent's waiter payload + total delivery count. Read after
    // poll()/wait_one() returns. sink_deliveries() mirrors the FakeAsyncBackend
    // seam (reference_backend_arena_lifecycle_test uses it there).
    std::size_t sink_deliveries() const noexcept { return sink_.deliveries(); }
    bool sink_last_has_waiter() const noexcept { return sink_.last_has_waiter(); }
    detail::WaiterToken sink_last_token() const noexcept { return sink_.last_token(); }
    std::uint64_t sink_last_lease_id() const noexcept { return sink_.last_lease_id(); }
#endif

  private:
    // ---- fixed tagged operation payload (Scheme B: per-slot scratch) -------
    // Sized to request_capacity at construction, indexed by SlotIndex (1:1 with
    // arena slots). Carries NO Completion*/RequestSlot*/lambda/std::function.
    // A worker reads prepared_ops_[h.slot] only after mark_running(h) succeeded,
    // which proves the slot is `running` at generation h.generation.
    struct PreparedBlockingOp {
        detail::OperationKind kind = detail::OperationKind::read;
        int fd = -1;
        const std::byte* buffer = nullptr;  // dst (read) / src (write) / null (sync)
        std::size_t length = 0;
        std::uint64_t offset = 0;
    };

    // ---- bounded dispatch ring (capacity == request_capacity) --------------
    // Stores SlotHandle only. push/pop/remove are noexcept; never allocates
    // after construction; never stores Completion*/RequestSlot*.
    class BoundedDispatchQueue {
      public:
        explicit BoundedDispatchQueue(std::size_t capacity)
            : storage_(capacity), capacity_(capacity) {}
        bool empty() const noexcept { return size_ == 0; }
        std::size_t size() const noexcept { return size_; }
        std::size_t capacity() const noexcept { return capacity_; }
        std::size_t high_water() const noexcept { return high_water_; }

        // noexcept push. Caller guarantees room (dispatch capacity == request
        // capacity, and a committed request holds its slot); a full push is an
        // invariant fail-fast, not would_block (AGENTS.md §12).
        void push_back(detail::SlotHandle h) noexcept;
        // noexcept pop; returns false if empty.
        bool pop_front(detail::SlotHandle& out) noexcept;
        // noexcept bounded compaction: remove the first entry whose slot+gen
        // match h exactly. O(capacity). Returns true if removed.
        bool remove_exact(detail::SlotHandle h) noexcept;

      private:
        std::vector<detail::SlotHandle> storage_;
        std::size_t head_ = 0;
        std::size_t size_ = 0;
        std::size_t high_water_ = 0;
        std::size_t capacity_;
    };

    // Process-wide monotonic id for ContextIdentity provenance (distinct per
    // ThreadPoolBackend instance — ADR Decision 2).
    static std::uint64_t next_backend_id() noexcept {
        static std::atomic<std::uint64_t> id{0x54500000u};  // 'Tp' provenance tag
        return ++id;
    }

    // Descriptor validation for a REAL syscall backend (ADR Decision 6; DIV-14
    // does NOT apply — ThreadPool issues real syscalls). Returns invalid_argument
    // for a malformed descriptor (negative fd, null buffer with nonzero length,
    // offset beyond off_t, length beyond SSIZE_MAX). Does NOT use fcntl(F_GETFD)
    // preflight (TOCTOU — AGENTS.md §9.1): a non-negative but closed fd is
    // accepted and later completes with the real EBADF terminal.
    static Result<void> validate_read(ReadOp op);
    static Result<void> validate_write(WriteOp op);
    static Result<void> validate_sync(SyncDataOp op);
    static Result<void> validate_sync(SyncAllOp op);

    // Five-stage admission for a byte-carrying / void op (ADR Decision 5; mirrors
    // the SyncBackend reference). Records the fixed prepared op into per-slot
    // scratch so the worker can run the real syscall after mark_running.
    template <class Op>
    Result<void> submit_size(Op op, Completion<std::size_t>& c, detail::OperationKind kind,
                             std::size_t len);
    template <class Op>
    Result<void> submit_void(Op op, Completion<void>& c, detail::OperationKind kind);

    // Unified enqueue + dispatch push under one work_mtx_ critical section
    // (Phase E P0). Closes the window where the arena pin is cleared but the
    // ring entry is not yet visible. noexcept because the arena lock and the
    // bounded ring push are allocation-free; the caller has already committed
    // the request, so a failure here would strand an accepted op.
    void enqueue_after_commit(detail::SlotHandle h) noexcept;

    template <class Op>
    static detail::BorrowMetadata borrow_of(const Op& op) noexcept {
        if constexpr (std::is_same_v<Op, ReadOp>) {
            return {op.fd, op.dst, op.len};
        } else {
            return {op.fd, op.src, op.len};
        }
    }

    // Publication thunks (ADR Decision 9): convert the arena's TerminalResult to
    // a Result<T> and publish the Completion-ready through the protected helper.
    // Static + type-erased; the arena calls them inside the leaf domain at reap.
    static void publish_size_ready(void* completion, const detail::TerminalResult& t) noexcept;
    static void publish_void_ready(void* completion, const detail::TerminalResult& t) noexcept;
    static Result<std::size_t> terminal_to_size(const detail::TerminalResult& t) noexcept;
    static Result<void> terminal_to_void(const detail::TerminalResult& t) noexcept;

    // Run the blocking syscall for a prepared op (no backend lock held) and
    // convert the exact outcome to a TerminalResult. retry_on_eintr handles EINTR.
    static detail::TerminalResult run_syscall(const PreparedBlockingOp& p) noexcept;

    // Persistent worker loop (one per worker thread). Waits on work_cv_ for the
    // dispatch ring or stopping_; pops + mark_running atomically under work_mtx_,
    // copies the prepared op, releases work_mtx_, runs the syscall, records the
    // terminal, signals ready progress.
    void worker_loop();

    // Wake the ready domain: publish the progress epoch under the ready mutex
    // and notify ALL parked observers (the split wait allows concurrent
    // observers, so a single notification could strand a second parker).
    void signal_ready_progress() noexcept;

#if defined(SLUICE_ASYNC_INTERNAL_TESTING)
    // Deterministic pause helpers (see the public gate setters above). Each
    // helper is a no-op when the corresponding gate is disarmed.
    void wait_after_enqueue_before_push_pause_(bool inside_work_mtx) noexcept;
    void wait_before_dequeue_pause_() noexcept;
    void wait_running_pause_() noexcept;
    void wait_terminal_publication_pause_() noexcept;
    void wait_before_enqueue_lock_pause_() noexcept;

    // The pre-commit admission stages that carry a synchronous rejection
    // (ADR Gate 4): reserve (capacity-full would_block / admission-closed
    // invalid_state), prepare, and commit.
    enum class SubmitStage { reserve, prepare, commit };

    // C2d pre-commit stage-failure injection (see the public seam above):
    // when the seam is armed at `stage`, increments that stage's `fired`
    // counter and returns the stage's natural synchronous rejection;
    // std::nullopt when disarmed.
    std::optional<IoError> injected_precommit_stage_failure_(SubmitStage stage) noexcept;
#endif

    void tally_canceled() noexcept {
        if (stats_) ++stats_->canceled_ops;
    }

    // ---- members -----------------------------------------------------------
    detail::RequestArena arena_;
    detail::ReferenceReadySink sink_;
    std::vector<PreparedBlockingOp> prepared_ops_;  // size == request_capacity

    // Backend work domain: dispatch ring + dequeue/cancel arbitration.
    mutable std::mutex work_mtx_;
    std::condition_variable work_cv_;
    BoundedDispatchQueue dispatch_;
    std::size_t active_workers_ = 0;
    bool stopping_ = false;

    // Ready/wake domain (issue #67): persistent progress + control epochs so a
    // ready recorded between snapshot and wait is not lost (AC-6; design
    // §4.5). A LEAF domain — never nested with work_mtx_ or the arena lock;
    // waiters park here WITHOUT any context-level lock (I1).
    detail::ReadyWaitSource ready_wait_;

    std::vector<std::thread> workers_;  // fixed worker_count, joined in dtor

    // Observability: monotonic syscall counter (test-only introspection).
    std::atomic<std::uint64_t> syscall_count_{0};

#if defined(SLUICE_ASYNC_INTERNAL_TESTING)
    // Deterministic pause-gate pointers (null when disarmed). Compiled out of
    // production sluice_async; see the public setter declarations above.
    std::atomic<AfterArenaEnqueueBeforeDispatchPushPauseGate*> after_enqueue_before_push_gate_{nullptr};
    std::atomic<BeforeWorkerDequeuePauseGate*> before_dequeue_gate_{nullptr};
    std::atomic<WorkerRunningPauseGate*> running_gate_{nullptr};
    std::atomic<TerminalPublicationPauseGate*> terminal_publication_gate_{nullptr};
    std::atomic<BeforeEnqueueLockPauseGate*> before_enqueue_lock_gate_{nullptr};
    // Phase C2d: post-commit dispatch-failure injection control (see the
    // guarded setter above). Null when disarmed; never dereferenced by
    // production builds (the branch is compiled out).
    std::atomic<DispatchFailureInjection*> dispatch_failure_injection_{nullptr};
    // Phase C2d: pre-commit stage-failure injection control (ADR Gate 4; see
    // the guarded setter above). Null when disarmed; never dereferenced by
    // production builds (the branches are compiled out).
    std::atomic<SubmitStageFailureInjection*> submit_stage_failure_injection_{nullptr};
#endif
};

}  // namespace sluice::async
