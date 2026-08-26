// sluice::async::ThreadPoolBackend.
//
// The portable, always-buildable REAL async backend: a bounded set of persistent
// blocking-I/O workers that run pread/pwrite/fdatasync/fsync. This is the
// FALLBACK where io_uring is absent (ADR §4) and is the first PRODUCTION backend
// to drive real POSIX syscalls through the bounded RequestArena / RequestSlot
// lifecycle (ADR-explicit-io-request-contract, Accepted).
//
// The backend replaces the legacy "one std::thread per op + std::function +
// Completion* ready deque" model (DIV-03 / DIV-12) with:
//
//   fixed count of persistent blocking-I/O workers (worker_count)
// + construction-time bounded dispatch ring (capacity == request_capacity)
// + RequestArena / RequestSlot as the ONE request lifecycle authority
// + a fixed tagged PreparedBlockingOp payload (no std::function, no Completion*)
// + worker consumes a SlotHandle, runs the syscall, records backend-ready ONLY
// + reap (poll/wait_one) is the SOLE Completion-ready publication authority
//
// See docs/history/implementation-plans/phase-e-bounded-threadpool-backend.md
// (the frozen design record) and docs/architecture/phase-e-compliance-gate.md
// (the evidence ledger).
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
// interruption would record err(canceled) explicitly and win; this backend does
// not implement one. cancel never publishes Completion-ready directly.
//
// Shutdown (ADR Decision 15; AGENTS.md §14): close_admission() rejects new
// reserve with invalid_state (Completion idle, no borrow) while existing accepted
// requests continue; cancel/poll/wait_one/reap remain legal. close_admission()
// ALSO wakes any participant parked in the ready wait: a one-shot
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
#include <sluice/async/detail/submit_transaction.hpp>
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

// Bounded configuration (AC-7, ADR Decision 13). Both MUST be > 0. The default
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

    // ADR-public-request-handle: this backend uses the RequestArena
    // identity contract, so it produces and resolves public RequestHandles.
    bool supports_request_identity() const noexcept override { return true; }

  private:
    // Sealed override of the private virtual in AsyncBackend: reached only via
    // AsyncIoContext::request_state -> AsyncBackend::request_handle_state. A raw
    // backend pointer must not expose the raw identity-tuple consumer.
    Result<RequestHandleState> resolve_identity_state(std::uint64_t ctx, std::uint32_t slot,
                                                      std::uint64_t gen) const override {
        return arena_.identity_handle_state(detail::SlotIndex{slot},
                                            detail::Generation{gen},
                                            detail::ContextIdentity{ctx});
    }

  public:

    std::size_t poll() override;
    Result<std::size_t> wait_one() override;

    void cancel(Completion<std::size_t>& c) override;
    void cancel(Completion<void>& c) override;

    // Production waiter registration / cancellation
    // (ADR Decision 10), forwarded verbatim to the REAL arena authorities
    // through the same resolve_completion identity bridge as cancel. No
    // side-band waiter map. register_waiter: success, or invalid_state for a
    // second registration / a non-accepted-or-already-reaped slot; not_found
    // for an unresolvable (unbound, cross-context, stale) Completion.
    // cancel_waiter: removes ONLY the waiter (never the I/O, never the borrow)
    // and returns the moved-out lease, or not_found when reap already closed
    // the registration.
    Result<void> register_waiter(Completion<std::size_t>& c,
                                 detail::WaiterToken token,
                                 detail::RoutingLease lease) override;
    Result<void> register_waiter(Completion<void>& c,
                                 detail::WaiterToken token,
                                 detail::RoutingLease lease) override;
    Result<detail::RoutingLease> cancel_waiter(Completion<std::size_t>& c) override;
    Result<detail::RoutingLease> cancel_waiter(Completion<void>& c) override;

    std::size_t outstanding() const noexcept override;

    // Split-phase wait capability: the backend's ready wait is a
    // pure observe-only epoch wait (see detail::ReadyWaitSource). AsyncIoContext
    // uses it to park for readiness WITHOUT holding access_mtx_, so a second
    // participant's poll/reap path always stays reachable.
    BackendWaitSource* wait_source() noexcept override { return &ready_wait_; }

    // Production admission close (ADR Decision 15). New reserve() returns
    // invalid_state (Completion idle, no borrow); existing accepted requests
    // continue; cancel/poll/wait_one/reap remain legal. Takes the backend
    // admission transaction lock (ADR §"Commit / accept" :453-462 — the
    // winning submit retains its context/admission lock through the Step 5
    // `binding -> outstanding` release-store, the commit/accept linearization
    // point), so after this returns no new acceptance LP can occur. Wakes any
    // participant parked in the ready wait as a one-shot
    // re-evaluation signal. Idempotent.
    void close_admission();

    // Resource introspection (method-only seams; no member data exposed).
    std::size_t arena_capacity() const noexcept { return arena_.capacity(); }
    std::size_t arena_slot_in_use() const noexcept { return arena_.slot_in_use(); }
    std::size_t arena_capacity_rejections() const noexcept {
        return arena_.capacity_rejections();
    }
    std::size_t configured_worker_count() const noexcept { return workers_.size(); }

    // AC-1a (issues #234/#227) minimal resource observations. These are
    // PRODUCTION read-only forwarders over state that ALREADY has a single
    // authority; no new counter, lock, or atomic is added, and the submit /
    // dispatch / reap hot paths are unchanged.
    //
    // Terms follow the R4 four-quantity discipline: capacity (bounded amount),
    // occupancy (currently consumed units), high-water (peak occupancy since
    // construction). "Demand" and "pressure" are NOT reported here: demand is
    // not observable in AC-1a and no time/progress-loss dimension exists at
    // this tier (rejection counts are refusals, not PSI-style pressure).
    //
    // Peak occupancy of the request slots since construction (maintained by
    // the arena itself where slot_in_use changes). Monotonic; <= capacity.
    std::size_t arena_high_water_mark() const noexcept { return arena_.high_water_mark(); }
    // Current dispatch-ring occupancy (enqueued-but-not-yet-dequeued accepted
    // requests) under the backend work domain.
    std::size_t dispatch_occupancy() const;
    // Peak dispatch-ring occupancy since construction. Monotonic; <= capacity.
    std::size_t dispatch_high_water_mark() const;
    // Workers currently between mark_running success and their post-syscall
    // bookkeeping decrement — i.e. threads OWNING AND EXECUTING one operation.
    // This is not "thread exists" and not "thread is awake"; an idle parked
    // worker counts as 0. Bounded by configured_worker_count().
    std::size_t active_workers() const;

    // Component-wise coherent by-value snapshot: each field is read under its
    // OWN existing authoritative lock (arena leaf mutex_ for the arena fields,
    // work_mtx_ for the dispatch/worker fields), but the combined struct is a
    // SEQUENCE of separate critical sections. It is NOT a globally atomic
    // snapshot and MUST NOT be used to prove cross-field consistency at one
    // instant; use individual accessors + tests when a single-domain fact is
    // needed. Convenient for RX-1 sampling loops where approximate totality is
    // acceptable and documented staleness is fine.
    struct ResourceSnapshot {
        std::size_t arena_capacity = 0;
        std::size_t arena_slot_in_use = 0;
        std::size_t arena_high_water_mark = 0;
        std::size_t arena_capacity_rejections = 0;
        std::size_t accepted_outstanding = 0;
        std::size_t dispatch_capacity = 0;
        std::size_t dispatch_occupancy = 0;
        std::size_t dispatch_high_water_mark = 0;
        std::size_t configured_workers = 0;
        std::size_t active_workers = 0;
    };
    ResourceSnapshot resource_snapshot() const noexcept;

#if defined(SLUICE_ASYNC_INTERNAL_TESTING)
    // ---- internal-testing control plane (C4 / issue #135) ----
    // The pause-gate / failure-injection struct definitions, the bodies of
    // the *_for_test observation mirrors below, and the test-side gate
    // resume/rearm + #110 generation-handshake helpers moved to the
    // NON-INSTALLED seam header src/async/threadpool_test_seams.hpp
    // (included at the bottom of this file under this same guard). This
    // installed production header keeps only the declarations, the
    // layout-bearing test members in the private section, and the private
    // seam helpers the production paths call (below). Production TUs
    // (macro undefined) compile none of it.

    std::size_t workers_spawned_for_test() const noexcept;
    std::size_t active_workers_for_test() const;
    std::size_t dispatch_size_for_test() const;
    std::size_t dispatch_high_water_for_test() const;
    std::uint64_t syscall_count_for_test() const noexcept;
    std::size_t backend_ready_count_for_test() const noexcept;
    std::optional<BackendWaitToken> try_wait_token_for_test() const noexcept;
    std::optional<std::size_t> try_outstanding_for_test() const noexcept;
    std::optional<std::size_t> try_backend_ready_count_for_test() const noexcept;
    void set_wait_phase_flag_for_test(std::atomic<bool>* flag) noexcept;
    void set_wait_prepark_counter_for_test(std::atomic<int>* counter) noexcept;
    void wait_epoch_changed_for_test(BackendWaitToken observed) noexcept;
    std::optional<detail::SlotHandle> handle_for_completion_for_test(
        const void* completion) const noexcept;
    std::optional<detail::RequestArena::RequestObservation> observe_for_test(
        detail::SlotHandle h) const noexcept;
    detail::CancelDisposition cancel_handle_for_test(detail::SlotHandle h) noexcept;

    // Deterministic pause-gate struct types (definitions in the seam header;
    // see that file for the full issue #92 / #110 gate protocol contracts).
    struct AfterArenaEnqueueBeforeDispatchPushPauseGate;
    struct BeforeWorkerDequeuePauseGate;
    struct PostResumePrePopHoldGate;
    struct WorkerRunningPauseGate;
    struct TerminalPublicationPauseGate;
    struct ControlWakeFinalReapPauseGate;
    struct BeforeEnqueueLockPauseGate;
    struct BeforeAdmissionLockPauseGate;
    struct BeforeCommitBindingPauseGate;
    // Phase C2d failure-injection control types (definitions in the seam
    // header; see that file for the ADR Gate 4 stage-injection contract).
    struct DispatchFailureInjection;
    struct SubmitStageFailureInjection;

    void set_after_enqueue_before_push_pause_gate(
        AfterArenaEnqueueBeforeDispatchPushPauseGate* gate) noexcept;
    void set_before_dequeue_pause_gate(BeforeWorkerDequeuePauseGate* gate) noexcept;
    void set_post_resume_pre_pop_hold_gate(PostResumePrePopHoldGate* gate) noexcept;
    void set_running_pause_gate(WorkerRunningPauseGate* gate) noexcept;
    void set_terminal_publication_pause_gate(
        TerminalPublicationPauseGate* gate) noexcept;
    void set_control_wake_final_reap_pause_gate(
        ControlWakeFinalReapPauseGate* gate) noexcept;
    void set_before_enqueue_lock_pause_gate(BeforeEnqueueLockPauseGate* gate) noexcept;
    void set_before_admission_lock_pause_gate(
        BeforeAdmissionLockPauseGate* gate) noexcept;
    void set_before_commit_binding_pause_gate(
        BeforeCommitBindingPauseGate* gate) noexcept;
    void set_dispatch_failure_injection(DispatchFailureInjection* injection) noexcept;
    void set_submit_stage_failure_injection(
        SubmitStageFailureInjection* injection) noexcept;

    // Construction-time worker-spawn failure injection (rows 9-10; finding
    // P1-04): a static seam because an instance member cannot be configured
    // before construction. Defined in threadpool_backend.cpp under the same
    // guard. SIZE_MAX disarms.
    static void set_injected_worker_spawn_failure_index(std::size_t index) noexcept;
    static std::size_t injected_worker_spawn_failure_index() noexcept;

    // Phase C2c waiter/borrow mirrors: route a real accepted Completion
    // through the REAL arena authorities (no side-band waiter map).
    Result<void> register_waiter_for_test(Completion<std::size_t>& c,
                                          detail::WaiterToken token,
                                          detail::RoutingLease lease);
    Result<void> register_waiter_for_test(Completion<void>& c,
                                          detail::WaiterToken token,
                                          detail::RoutingLease lease);
    Result<detail::RoutingLease> cancel_waiter_for_test(Completion<std::size_t>& c);
    Result<detail::RoutingLease> cancel_waiter_for_test(Completion<void>& c);
    Result<void> register_waiter_handle_for_test(detail::SlotHandle h,
                                                 detail::WaiterToken token,
                                                 detail::RoutingLease lease);
    Result<detail::RoutingLease> cancel_waiter_handle_for_test(detail::SlotHandle h);
    std::optional<detail::RequestArena::BorrowSnapshot> borrow_for_test(
        detail::SlotHandle h) const noexcept;
    std::optional<detail::RequestArena::WaiterObservation> waiter_for_test(
        detail::SlotHandle h) const noexcept;

    // C2c sink observation (test-only).
    std::size_t sink_deliveries() const noexcept;
    bool sink_last_has_waiter() const noexcept;
    detail::WaiterToken sink_last_token() const noexcept;
    std::uint64_t sink_last_lease_id() const noexcept;
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

    // Dispatch the malformed-descriptor probe by op kind. Called INSIDE the
    // admission transaction, AFTER reserve (Stage 1.5) so the Reserve-stage
    // rejections — admission closed (invalid_state, Decision 15) and capacity
    // full (would_block) — take precedence over the Prepare-stage
    // invalid_argument (ADR Decision 5 stage order). A rejected
    // descriptor rolls back the reserved slot through
    // rollback_reserved_or_prepared — zero residue.
    template <class Op>
    static Result<void> validate_op(const Op& op) noexcept;

    // Five-stage admission for a byte-carrying / void op: ONE call into
    // the shared pre-accept ladder (detail::submit_transaction).
    // Records the fixed prepared op into per-slot scratch (the policy's
    // write_scratch) so the worker can run the real syscall after
    // mark_running.
    template <class Op>
    Result<void> submit_size(Op op, Completion<std::size_t>& c, detail::OperationKind kind);
    template <class Op>
    Result<void> submit_void(Op op, Completion<void>& c, detail::OperationKind kind);

    // The backend's policy for detail::submit_transaction: a
    // REAL syscall backend — descriptor validation (Stage 1.5, AFTER
    // reserve: admission/capacity precedence over malformed descriptors,
    // ADR Decision 5 stage order), prepared-op scratch (the
    // worker reads it only after a current-generation running transition,
    // Scheme B), the deterministic pause seam between the arena commit and
    // the acceptance LP, and the pre-commit stage-failure injection harness
    // (test-guarded). No Stage-0 gate: admission arbitration is the arena's
    // reserve check under admission_mtx_ — this backend has no poison state.
    template <class Op, class Comp>
    struct SubmitPolicy {
        using completion_type = Comp;
        using op_type = Op;

        SubmitPolicy(ThreadPoolBackend& self, detail::OperationKind kind) noexcept
            : self_(self), kind_(kind) {}

        // --- data accessors ---
        detail::OperationKind kind() const noexcept { return kind_; }
        static detail::BorrowMetadata borrow(const Op& op) noexcept {
            if constexpr (std::is_same_v<Comp, Completion<std::size_t>>) {
                return borrow_of(op);
            } else {
                return detail::BorrowMetadata{op.fd, nullptr, 0};
            }
        }
        static std::uint64_t requested_bytes(const Op& op) noexcept {
            if constexpr (std::is_same_v<Comp, Completion<std::size_t>>) {
                return op.len;
            } else {
                return 0;
            }
        }
        static auto publish_thunk() noexcept {
            if constexpr (std::is_same_v<Comp, Completion<std::size_t>>) {
                return &ThreadPoolBackend::publish_size_ready;
            } else {
                return &ThreadPoolBackend::publish_void_ready;
            }
        }
        // --- binding trio (protected AsyncBackend statics, reached through
        // the enclosing backend — the trusted backend-author role) ---
        static bool begin_binding(Comp& c) noexcept {
            return ThreadPoolBackend::begin_binding(c);
        }
        static void install_binding(Comp& c, detail::RequestArena* arena,
                                    detail::SlotHandle h) noexcept {
            ThreadPoolBackend::install_binding(c, arena, h);
        }
        static void commit_binding(Comp& c) noexcept {
            ThreadPoolBackend::commit_binding(c);
        }
        static void rollback_binding(Comp& c) noexcept {
            ThreadPoolBackend::rollback_binding_before_accept(c);
        }
        // --- production hooks ---
        Result<void> stage0_precheck() const noexcept { return {}; }
        Result<void> validate(const Op& op) const noexcept { return self_.validate_op(op); }
        void write_scratch(detail::SlotHandle h, const Op& op) const noexcept {
            if constexpr (std::is_same_v<Comp, Completion<std::size_t>>) {
                self_.prepared_ops_[h.slot.value] =
                    PreparedBlockingOp{kind_, op.fd,
                                       static_cast<const std::byte*>(borrow_of(op).address),
                                       op.len, op.offset};
            } else {
                self_.prepared_ops_[h.slot.value] =
                    PreparedBlockingOp{kind_, op.fd, nullptr, std::size_t{0},
                                       std::uint64_t{0}};
            }
        }
        void pause_before_commit_binding() noexcept {
#if defined(SLUICE_ASYNC_INTERNAL_TESTING)
            // C2e (row 15; B1): deterministic close-vs-LP window — the seam
            // body is compiled out of production builds (empty inline).
            self_.wait_before_commit_binding_pause_();
#endif
        }
#if defined(SLUICE_ASYNC_INTERNAL_TESTING)
        // C2d (ADR Gate 4): the pre-commit stage-failure injection harness.
        std::optional<IoError> injected_precommit_stage_failure(
            detail::SubmitStage stage) const noexcept {
            return self_.injected_precommit_stage_failure_(stage);
        }
#endif

      private:
        ThreadPoolBackend& self_;
        detail::OperationKind kind_;
    };

    // Unified enqueue + dispatch push under one work_mtx_ critical section.
    // Closes the window where the arena pin is cleared but the
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
    // Issue #110: returns the generation this visit observed (0 = legacy
    // single-visit bool protocol, or the gate disappeared). The caller MUST
    // pass that generation to ack_dequeue_gate_generation_ AFTER this
    // dispatch cycle's dequeue decision, closing the #110 protocol hole.
    std::uint64_t wait_before_dequeue_pause_() noexcept;
    // Issue #110: publish the dequeue-boundary ACK for `generation` (monotonic
    // max + notify_all). No-op for generation 0 (legacy visits never ack).
    void ack_dequeue_gate_generation_(std::uint64_t generation) noexcept;
    // Issue #110 regression seam: single-visit bool pause between the
    // dequeue-gate resume-wait returning and the worker re-taking work_mtx_ /
    // pop_front. No-op when the hold gate is disarmed.
    void wait_post_resume_pre_pop_hold_() noexcept;
    void wait_running_pause_() noexcept;
    void wait_terminal_publication_pause_() noexcept;
    void wait_before_enqueue_lock_pause_() noexcept;
    void wait_control_wake_final_reap_pause_() noexcept;
    void wait_before_admission_lock_pause_() noexcept;
    void wait_before_commit_binding_pause_() noexcept;

    // The pre-commit admission stages that carry a synchronous rejection
    // (ADR Gate 4): reserve (capacity-full would_block / admission-closed
    // invalid_state), prepare, and commit. Issue #137: the vocabulary moved
    // to the shared detail header (the ladder's guarded injection seam uses
    // it); this alias preserves the in-class name for the harness and the
    // .cpp call sites.
    using SubmitStage = detail::SubmitStage;

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

    // Backend admission transaction domain (ADR §"Commit / accept" :453-462:
    // the winning submit retains its context/admission lock through Step 5 —
    // the `binding -> outstanding` release-store, the commit/accept
    // linearization point). close_admission() takes the same lock, so after it
    // returns no new acceptance LP can occur (Decision 15). Acquired ONLY by
    // the submit paths (reserve .. commit_binding) and close_admission();
    // released BEFORE enqueue (no-fail, needs no admission serialization).
    // Lock order: admission_mtx_ -> arena leaf only — never nested with
    // work_mtx_ or the ready-wait mutex.
    mutable std::mutex admission_mtx_;

    // Backend work domain: dispatch ring + dequeue/cancel arbitration.
    mutable std::mutex work_mtx_;
    std::condition_variable work_cv_;
    BoundedDispatchQueue dispatch_;
    std::size_t active_workers_ = 0;
    bool stopping_ = false;

    // Ready/wake domain: persistent progress + control epochs so a
    // ready recorded between snapshot and wait is not lost (AC-6; design
    // §4.5). A LEAF domain — never nested with work_mtx_ or the arena lock;
    // waiters park here WITHOUT any context-level lock.
    detail::ReadyWaitSource ready_wait_;

    std::vector<std::thread> workers_;  // fixed worker_count, joined in dtor

    // Observability: monotonic syscall counter (test-only introspection).
    std::atomic<std::uint64_t> syscall_count_{0};

#if defined(SLUICE_ASYNC_INTERNAL_TESTING)
    // Deterministic pause-gate pointers (null when disarmed). Compiled out of
    // production sluice_async; see the public setter declarations above.
    std::atomic<AfterArenaEnqueueBeforeDispatchPushPauseGate*> after_enqueue_before_push_gate_{nullptr};
    std::atomic<BeforeWorkerDequeuePauseGate*> before_dequeue_gate_{nullptr};
    std::atomic<PostResumePrePopHoldGate*> post_resume_pre_pop_hold_gate_{nullptr};
    std::atomic<WorkerRunningPauseGate*> running_gate_{nullptr};
    std::atomic<TerminalPublicationPauseGate*> terminal_publication_gate_{nullptr};
    std::atomic<BeforeEnqueueLockPauseGate*> before_enqueue_lock_gate_{nullptr};
    // C2e: deterministic interrupt-vs-final-ready window in wait_one (see the
    // public gate struct above). Compiled out of production builds.
    std::atomic<ControlWakeFinalReapPauseGate*> control_wake_final_reap_gate_{nullptr};
    // C2e (B1): deterministic admission-transaction windows (see the public
    // gate structs above). Compiled out of production builds.
    std::atomic<BeforeAdmissionLockPauseGate*> before_admission_lock_gate_{nullptr};
    std::atomic<BeforeCommitBindingPauseGate*> before_commit_binding_gate_{nullptr};
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

#if defined(SLUICE_ASYNC_INTERNAL_TESTING)
// C4 (issue #135): the complete internal-testing control plane for
// ThreadPoolBackend (gate/injection struct definitions, *_for_test
// bodies, test-side gate helpers) lives in the NON-INSTALLED seam
// header src/async/threadpool_test_seams.hpp, resolved via the
// internal-testing-only include path. Production TUs never compile
// this include.
#include "threadpool_test_seams.hpp"
#endif  // defined(SLUICE_ASYNC_INTERNAL_TESTING)
