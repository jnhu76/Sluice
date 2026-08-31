// sluice::async::UringAsyncBackend (ADR §4 Option 4).
//
// The Linux io_uring backend. It uses the bounded RequestArena /
// RequestSlot lifecycle with a PRIVATE io_uring ring per backend instance
// (ADR Decision 18 — Uring execution-ownership amendment):
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
#include <sluice/async/detail/submit_transaction.hpp>
#include <sluice/error.hpp>
#include <sluice/result.hpp>

#if defined(SLUICE_HAS_LIBURING)
// The split-phase wait source uses Linux/POSIX-only headers (<poll.h>,
// <sys/eventfd.h>, <unistd.h>). The Uring STUB/OFF public header must remain
// usable without real liburing, so the wait source is visible only when the
// real path is compiled; every UringWaitSource reference below stays inside
// the SLUICE_HAS_LIBURING guard.
#include <sluice/async/detail/uring_wait_source.hpp>
#endif

#include <atomic>
#include <cassert>
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

#if defined(SLUICE_ASYNC_INTERNAL_TESTING)
// TAX-0 router-fix shootout (#255 campaign): the R3 candidate's fixed
// cookie->router-index table. Non-installed research type; the complete
// definition lives in src/async/uring_test_seams.hpp (included at the
// bottom of this header under the internal-testing guard). Production
// builds never name it; internal-testing object layout carries one extra
// unique_ptr.
struct RouterCookieTableForTest;
#endif
#endif

#if defined(SLUICE_HAS_LIBURING)
// Bounded configuration (AC-7, ADR Decision 13). request_capacity MUST be in
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
// Transport submit/wait injection hooks for the dedicated real-liburing
// fault tests. C4 (issue #135): the definition moved to the NON-INSTALLED
// seam header src/async/uring_test_seams.hpp (included at the bottom of
// this file under this same guard); this forward declaration remains so
// the guarded test constructor overload below can name it. Production
// targets never define SLUICE_ASYNC_INTERNAL_TESTING and therefore expose
// neither this type nor the constructor overload.
struct UringBackendSubmitTestHooks;
#endif

class UringAsyncBackend : public AsyncBackend {
  public:
    // Legacy source-compatible constructor: maps to
    // UringConfig{request_capacity == queue_depth, queue_depth}. Stub mode
    // (no liburing) ignores depth and reports available()==false.
    explicit UringAsyncBackend(unsigned queue_depth = 64);

#if defined(SLUICE_HAS_LIBURING)
    // Explicit bounded configuration. request_capacity MUST be in
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

    // ADR-public-request-handle: the real-liburing backend uses the
    // RequestArena identity contract, so it produces and resolves public
    // RequestHandles. The stub build has no arena, so it inherits the defaults
    // (supports_request_identity == false -> submit_*_request returns
    // not_supported), consistent with the non-arena-backend policy.
#if defined(SLUICE_HAS_LIBURING)
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
#endif

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
    // the registration. Stub builds have no live ring but the RequestArena
    // machinery is still authoritative (the same stub conformance rules).
    Result<void> register_waiter(Completion<std::size_t>& c,
                                 detail::WaiterToken token,
                                 detail::RoutingLease lease) override;
    Result<void> register_waiter(Completion<void>& c,
                                 detail::WaiterToken token,
                                 detail::RoutingLease lease) override;
    Result<detail::RoutingLease> cancel_waiter(Completion<std::size_t>& c) override;
    Result<detail::RoutingLease> cancel_waiter(Completion<void>& c) override;

    std::size_t outstanding() const noexcept override;

    // Whether this instance initialized a real io_uring (false in stub mode
    // or when the host kernel cannot create a ring). This is a capability
    // query, not a health query.
    bool available() const noexcept;

    // Admission close + drain (ADR Decision 15; mirrors
    // ThreadPoolBackend::close_admission). Rejects new admission atomically:
    // takes the same dispatch_mtx_ the submit admission transaction (reserve
    // .. commit_binding) holds, so after this returns no new acceptance LP
    // can occur (an in-flight submit either completed its LP first or rejects
    // synchronously with invalid_state). Does NOT cancel, rewrite, discard, or
    // release accepted work; cancel/poll/wait_one/reap remain legal. Then
    // wakes every participant parked in the split-phase ready wait (one-shot
    // control generation advance — a re-evaluation signal, never a fabricated
    // completion and never a persistent "never park again" state). Idempotent.
    // Stub mode: no-op (admission never opened).
    void close_admission();

#if defined(SLUICE_HAS_LIBURING)
    // Split-phase readiness wait. wait_for_change()
    // parks in poll(2) on the private ring fd (POLLIN == CQEs pending) and a
    // one-shot control eventfd; it NEVER reaps, records terminals, publishes
    // Completions, mutates RequestArena state, cancels, or changes
    // outstanding. The context keeps serialized poll/reap under access_mtx_.
    // nullptr (the base default) when the ring is unavailable — construction
    // fails (no silent capability downgrade) if the control eventfd cannot be
    // created, so there is no legacy-fallback-on-eventfd-failure state; the
    // legacy serialized wait_one contract applies only when there is no ring
    // at all.
    BackendWaitSource* wait_source() noexcept override {
        return have_ring_ ? wait_source_.get() : nullptr;
    }

    // Resource introspection (method-only seams; no member data).
    std::size_t arena_capacity() const noexcept { return arena_.capacity(); }
    std::size_t arena_slot_in_use() const noexcept { return arena_.slot_in_use(); }
    std::size_t arena_accepted_outstanding() const noexcept {
        return arena_.accepted_outstanding();
    }
    std::size_t arena_capacity_rejections() const noexcept { return arena_.capacity_rejections(); }
    std::size_t configured_queue_depth() const noexcept { return queue_depth_; }
#endif

#if defined(SLUICE_HAS_LIBURING) && defined(SLUICE_ASYNC_INTERNAL_TESTING)
    // ---- internal-testing control plane (C4 / issue #135) ----
    // The pause-gate struct definitions, the bodies of the *_for_test
    // observation mirrors below, and the transport-injection hooks moved to
    // the NON-INSTALLED seam header src/async/uring_test_seams.hpp (included
    // at the bottom of this file under this same guard). This installed
    // production header keeps only the declarations, the layout-bearing test
    // members in the private section, and the private seam helpers the
    // production paths call. Production and stub TUs compile none of it.

    std::uint64_t submit_flushes_for_test() const noexcept;
    std::size_t live_cookies_for_test() const noexcept;
    void inject_cqe_for_test(std::uint64_t cookie, int res) noexcept;
    std::uint64_t peek_next_cookie_for_test() const noexcept;
    std::optional<std::uint64_t> live_cookie_for_offset_for_test(
        std::uint64_t offset) const noexcept;
    static Result<void> validate_write_for_test(WriteOp op) noexcept;
    // Phase D2 read-only bounded-state observations (out-of-line definitions
    // compiled only into internal-testing builds, in uring_backend.cpp).
    std::size_t dispatch_size_for_test() const noexcept;
    std::size_t transport_ledger_size_for_test() const noexcept;
    std::size_t sq_ready_for_test() const noexcept;
    std::size_t live_control_entries_for_test() const noexcept;
    std::size_t backend_ready_count_for_test() const noexcept;
    std::size_t live_control_sqes_for_test() const noexcept;

    // Phase D3 C2b/C2c seams: delegate to REAL production authority
    // (RequestArena, ReferenceReadySink, the production cancel core); no
    // side-band identity/waiter map, no second generation counter.
    std::optional<detail::SlotHandle> handle_for_completion_for_test(
        const void* completion) const noexcept;
    std::optional<detail::RequestArena::RequestObservation> observe_for_test(
        detail::SlotHandle h) const noexcept;
    detail::CancelDisposition cancel_handle_for_test(detail::SlotHandle h) noexcept;
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
    std::size_t sink_deliveries() const noexcept;
    bool sink_last_has_waiter() const noexcept;
    detail::WaiterToken sink_last_token() const noexcept;
    std::uint64_t sink_last_lease_id() const noexcept;

    // Deterministic pause-gate struct types (definitions in the seam header).
    struct AfterCommitBeforeEnqueuePauseGate;
    struct BeforeDispatchTransferPauseGate;
    struct BeforeCommitBindingPauseGate;
    struct BeforeAdmissionLockPauseGate;

    void set_after_commit_before_enqueue_pause_gate(
        AfterCommitBeforeEnqueuePauseGate* gate) noexcept;
    void set_before_dispatch_transfer_pause_gate(
        BeforeDispatchTransferPauseGate* gate) noexcept;
    void set_before_commit_binding_pause_gate(
        BeforeCommitBindingPauseGate* gate) noexcept;
    void set_before_admission_lock_pause_gate(
        BeforeAdmissionLockPauseGate* gate) noexcept;

    // Phase D4 C2e split-phase-wait seams (forward to the wait source).
    void set_wait_phase_flag_for_test(std::atomic<bool>* flag) noexcept;
    void set_wait_prepark_counter_for_test(std::atomic<int>* counter) noexcept;
    void set_wait_control_wake_final_reap_pause_gate(
        detail::UringWaitSource::ControlWakeFinalReapPauseGate* gate) noexcept;
    void set_wait_before_physical_poll_pause_gate(
        detail::UringWaitSource::BeforePhysicalPollPauseGate* gate) noexcept;
    void set_wait_poll_ring_fd_override_for_test(int fd) noexcept;
    void set_wait_poll_fn_for_test(detail::UringWaitSource::PollFn fn,
                                   void* ctx) noexcept;
    // Returns true = parked on the wait source's epoch domain until the
    // observed token changed; false = no wait source (ring construction
    // failed — typed T5 availability result, never an assert).
    bool wait_epoch_changed_for_test(BackendWaitToken observed) noexcept;
    std::optional<BackendWaitToken> try_wait_token_for_test() const noexcept;
    std::optional<std::size_t> try_outstanding_for_test() const noexcept;
    std::optional<std::size_t> try_backend_ready_count_for_test() const noexcept;

    // Deterministic destructor-order probe (D4-RM11 detector). The alias must
    // stay in the class: the guarded test members below use it.
    using BeforeQueueExitFn = void (*)(void*);
    void set_before_queue_exit_hook_for_test(BeforeQueueExitFn fn, void* ctx) noexcept;

    // ---- TAX-0 EXP-U0 router-scan research seam (#250 campaign) ----------
    // Research-only causal-ablation control + exact scan-iteration witness
    // for find_live_router_cookie_ (the per-CQE cookie->router lookup).
    // NOTHING here is compiled into production targets. The default mode is
    // forward_production, so an internal-testing build that never touches
    // the seam executes the production forward scan (plus the diagnostic
    // counter folding; plain non-atomic members — see the private section).
    enum class RouterScanModeForTest : std::uint8_t {
        forward_production, // the production scan (and the seam default)
        reverse_ablation,   // EXP-U0 research ablation: scan high -> low
    };
    // Which find_live_router_cookie_ callsite family a lookup belonged to
    // (operation CQE / tagged-control CQE / transport accounting — the
    // consumed-cancel-control SQE and Class-A recovery sites). The U0
    // no-cancel READ workload expects control == 0 and transport == 0.
    enum class RouterLookupKindForTest : std::uint8_t {
        operation_cqe,
        control_cqe,
        transport,
    };
    struct RouterScanDiagnosticsForTest {
        // Kind-specific accounting (folded at the callsites).
        std::uint64_t operation_cookie_lookup_calls = 0;
        std::uint64_t control_cookie_lookup_calls = 0;
        std::uint64_t transport_cookie_lookup_calls = 0;
        std::uint64_t operation_lookup_iterations_total = 0;
        std::uint64_t operation_lookup_iterations_max = 0;
        std::uint64_t control_lookup_iterations_total = 0;
        std::uint64_t control_lookup_iterations_max = 0;
        std::uint64_t transport_lookup_iterations_total = 0;
        std::uint64_t transport_lookup_iterations_max = 0;
        // Kind-agnostic accounting (folded inside the scan itself).
        std::uint64_t lookup_calls = 0;
        std::uint64_t lookup_hits = 0;
        std::uint64_t lookup_misses = 0;
        std::uint64_t matched_router_index_sum = 0;
        std::uint64_t matched_router_index_max = 0;
        std::uint64_t reverse_mode_calls = 0;
        std::uint64_t last_call_iterations = 0;
        // TAX-0 router-fix shootout (#255): R3 bounded-table structural
        // probes. Zero unless the bounded_cookie_table mode is active.
        std::uint64_t table_insert_calls = 0;
        std::uint64_t table_insert_probes_total = 0;
        std::uint64_t table_insert_probes_max = 0;
        std::uint64_t table_lookup_probes_total = 0;
        std::uint64_t table_lookup_probes_max = 0;
        std::uint64_t table_erase_calls = 0;
        std::uint64_t table_erase_probes_total = 0;
        std::uint64_t table_erase_probes_max = 0;
    };
    void set_router_scan_mode_for_test(RouterScanModeForTest mode) noexcept;
    RouterScanModeForTest router_scan_mode_for_test() const noexcept;
    // Read-only cookie -> router-index resolution through the EXACT
    // production find_live_router_cookie_ under the current scan mode
    // (diagnostics folded). Returns router capacity on a miss, exactly
    // like the production lookup.
    std::size_t find_live_router_cookie_for_test(std::uint64_t cookie)
        const noexcept;
    const RouterScanDiagnosticsForTest& router_scan_diagnostics_for_test()
        const noexcept;
    void reset_router_scan_diagnostics_for_test() noexcept;

    // ---- TAX-0 router-fix candidate shootout seam (#255 campaign) --------
    // Research-only FIX-CANDIDATE selector for the per-CQE cookie->router
    // resolution. NOTHING here is compiled into production targets; the
    // default mode is production_baseline, under which the backend executes
    // EXACTLY the production behavior (including any U0 scan-mode ablation).
    // Candidates:
    //   production_baseline   R0 — forward scan + high-index LIFO placement
    //                         (the shipped representation; the comparator)
    //   reverse_scan          R1 — identical predicate traversed high -> low
    //                         (placement unchanged; the EXP-U0 ablation as
    //                         a candidate)
    //   low_placement_forward R2 — free-list seeded descending so the live
    //                         set occupies LOW indices; forward scan kept
    //                         (the placement dual of R1)
    //   bounded_cookie_table  R3 — fixed open-addressed cookie->router-index
    //                         table (construction-time bounded, no
    //                         steady-state allocation); placement unchanged
    //                         (R0 placement) so the table's own cost is
    //                         isolated
    enum class RouterFixModeForTest : std::uint8_t {
        production_baseline,
        reverse_scan,
        low_placement_forward,
        bounded_cookie_table,
    };
    // Mode switch is a fresh-backend operation: the router must be fully
    // retired AND the backend quiescent (outstanding() == 0), otherwise this
    // fail-fasts. Switching reseeds the router free-list in the mode's
    // physical order (R2 descending, others ascending); cookie values are
    // NEVER reseeded. Defined in the non-installed seam header.
    void set_router_fix_mode_for_test(RouterFixModeForTest mode) noexcept;
    RouterFixModeForTest router_fix_mode_for_test() const noexcept;
    // Layer-A microbench micro-ops (research instrument only; NOT a
    // production surface). install mirrors ONLY the router-install slice of
    // dispatch_one_locked — free-list pop (mode-ordered), no-wrap cookie
    // allocation, RouterEntry install, R3 table insert — with no SQE, ring,
    // arena, dispatch, or ledger interaction. retire delegates to the EXACT
    // production retire_router_entry_ path (including the R3 table erase).
    std::size_t router_install_cookie_for_test() noexcept;
    void router_retire_cookie_for_test(std::size_t router_index) noexcept;
    // Structural memory facts for the Layer-A evidence (research only):
    // the fixed per-capacity metadata cost of the router representation
    // (router array entry bytes; R3 table bytes when the table exists).
    static std::size_t router_entry_bytes_for_test() noexcept;
    std::size_t router_table_bytes_for_test() const noexcept;
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
    // Kernel-visible identity discipline: the SQE user_data carries the
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

    // Five-stage admission mirroring ThreadPoolBackend (ADR Decision 5): ONE
    // call into the shared pre-accept ladder (detail::submit_transaction)
    // under this backend's admission discipline
    // (dispatch_mtx_, with the in-lock Stage-0 poison/admission gate).
    template <class Op>
    Result<void> submit_size(Op op, Completion<std::size_t>& c, detail::OperationKind kind);
    template <class Op>
    Result<void> submit_void(Op op, Completion<void>& c, detail::OperationKind kind);

    // The backend's policy for detail::submit_transaction: the
    // REAL io_uring backend adds to the ThreadPool shape exactly the Stage-0
    // ring/poison/admission gate (hook-internal order:
    // ring -> poison -> admission; D4-M5 precedence: poison error verbatim
    // BEFORE admission-closed; ring availability is construction-time
    // immutable state — read under dispatch_mtx_ on the Stage-0 admission
    // path, and elsewhere (e.g. wait_source) read lock-free relying on that
    // immutability plus the quiescent-lifetime guarantee, D4-RM1) and
    // the native-length scratch normalization (op.len is validated <=
    // UINT_MAX at Stage 1.5, so the dispatch fill uses prep.native_length
    // directly). The stage-injection harness is ThreadPool-only (this
    // backend probes the same windows through its own pause seams).
    template <class Op, class Comp>
    struct SubmitPolicy {
        using completion_type = Comp;
        using op_type = Op;

        SubmitPolicy(UringAsyncBackend& self, detail::OperationKind kind) noexcept
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
                return &UringAsyncBackend::publish_size_ready;
            } else {
                return &UringAsyncBackend::publish_void_ready;
            }
        }
        // --- binding trio (protected AsyncBackend statics, reached through
        // the enclosing backend — the trusted backend-author role) ---
        static bool begin_binding(Comp& c) noexcept {
            return UringAsyncBackend::begin_binding(c);
        }
        static void install_binding(Comp& c, detail::RequestArena* arena,
                                    detail::SlotHandle h) noexcept {
            UringAsyncBackend::install_binding(c, arena, h);
        }
        static void commit_binding(Comp& c) noexcept {
            UringAsyncBackend::commit_binding(c);
        }
        static void rollback_binding(Comp& c) noexcept {
            UringAsyncBackend::rollback_binding_before_accept(c);
        }
        // --- production hooks ---
        Result<void> stage0_precheck() const noexcept {
            // Stage 0 (hook-internal order
            // ring -> poison -> admission): the single authority for
            // every Uring pre-reserve rejection, running under the
            // caller-held dispatch_mtx_.
            //
            // Ring availability: construction-time immutable state
            // (written once in the constructor, reset only in the
            // destructor), so reading it here under dispatch_mtx_ is
            // safe and does not require additional synchronization.
            // Centralizing it in the hook (rather than as an unlocked
            // fast-path in submit_size/submit_void) makes Stage-0 the
            // ONE policy authority per the accepted design.
            if (!self_.have_ring_) {
                return make_unexpected<void>(IoError{IoError::Code::backend_error});
            }
            // Poison precedence (D4-M5): a poisoned backend reports its
            // permanent backend_error verbatim even after
            // close_admission(). admission_closed_ / fatal_error_ are
            // read ONLY under dispatch_mtx_ — the SAME lock
            // close_admission() and poison_and_recover_locked() write
            // under. There is deliberately NO unlocked fast-path read
            // of these fields (D4-RM1): both are written under
            // this mutex, so an unlocked read would be a C++ data race
            // on the exact submit-vs-close arbitration D4 supports.
            if (self_.fatal_error_.has_value()) {
                return make_unexpected<void>(*self_.fatal_error_);
            }
            if (self_.admission_closed_) {
                return make_unexpected<void>(IoError{IoError::Code::invalid_state});
            }
            return {};
        }
        Result<void> validate(const Op& op) const noexcept { return self_.validate_op(op); }
        void write_scratch(detail::SlotHandle h, const Op& op) const noexcept {
            if constexpr (std::is_same_v<Comp, Completion<std::size_t>>) {
                // Normalize the length to liburing's `unsigned nbytes` here
                // (validation already proved op.len <= UINT_MAX), so the
                // dispatch fill uses prep.native_length directly with no
                // implicit narrowing.
                self_.prepared_ops_[h.slot.value] =
                    PreparedUringOp{kind_, op.fd,
                                    static_cast<const std::byte*>(borrow_of(op).address),
                                    op.len, static_cast<unsigned>(op.len), op.offset};
            } else {
                self_.prepared_ops_[h.slot.value] =
                    PreparedUringOp{kind_, op.fd, nullptr, std::size_t{0},
                                    /*native_length=*/0u, std::uint64_t{0}};
            }
        }
        void pause_before_commit_binding() noexcept {
#if defined(SLUICE_ASYNC_INTERNAL_TESTING)
            // D4 C2e (submit-vs-close LP window): the seam body is compiled
            // out of production builds (empty inline).
            self_.wait_before_commit_binding_pause_();
#endif
        }
#if defined(SLUICE_ASYNC_INTERNAL_TESTING)
        // This backend probes the pre-commit windows through its D3/D4 pause
        // seams, not the C2d stage-injection harness.
        std::optional<IoError> injected_precommit_stage_failure(
            detail::SubmitStage) const noexcept {
            return std::nullopt;
        }
#endif

      private:
        UringAsyncBackend& self_;
        detail::OperationKind kind_;
    };

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
    // Peek protocol: assumes dispatch_mtx_ is held AND h == dispatch_
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
    // the zero-consumption theorem) from a pure wait error with to_submit=0.
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
    void wait_before_commit_binding_pause_() noexcept;
    void wait_before_admission_lock_pause_() noexcept;
#endif

    // Wake the ready domain. Advances the split-phase wait source's
    // progress epoch and wakes every parked participant AFTER real readiness
    // is published (state first, then notify). No-op when no wait source
    // exists (stub / no ring / eventfd unavailable).
    void signal_ready_progress() noexcept {
        if (wait_source_) {
            wait_source_->signal_progress();
        }
    }

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
    // Split-phase readiness wait (observe-only). Created when the
    // ring initializes; null only in stub mode / ring-init failure (no ring —
    // wait_source() then returns nullptr, the base default). eventfd
    // construction failure THROWS from the ring-init try block (ring torn
    // down, construction fails) — there is no silent capability downgrade.
    std::unique_ptr<detail::UringWaitSource> wait_source_;
    bool have_ring_ = false;
    bool admission_closed_ = false;
    std::optional<IoError> fatal_error_; // permanent transport poison

    // Backend dispatch domain: local dispatch ring + dispatch/cancel
    // arbitration + cookie/router installation and cancel-side lookup/scratch
    // mutation. CQE lookup/retirement is serialized by the documented
    // AsyncIoContext single-driver call domain and intentionally does not take
    // this mutex before arena.record_terminal(). Lock order:
    // dispatch_mtx_ -> arena leaf only — the ready-wait mutex is a LEAF domain
    // and is NEVER acquired while holding dispatch_mtx_ (D4-RM14:
    // signal_ready_progress() is called only after dispatch_mtx_ is released —
    // state first, then wake). io_uring_submit() (syscall) is transport
    // progress and may be called under dispatch_mtx_ but NEVER under the
    // arena mutex.
    mutable std::mutex dispatch_mtx_;
    std::unique_ptr<BoundedDispatchQueue> dispatch_;

    std::atomic<std::uint64_t> submit_flushes_{0};
    std::atomic<std::size_t> live_cookies_{0};
    std::atomic<std::size_t> live_control_sqes_{0};

#if defined(SLUICE_ASYNC_INTERNAL_TESTING)
    // D3/D4 deterministic pause gates (see the guarded setters above).
    // Compiled out of production builds; the layout cost in the
    // internal-testing target is accepted and documented (AGENTS.md §15).
    std::atomic<AfterCommitBeforeEnqueuePauseGate*> after_commit_before_enqueue_gate_{nullptr};
    std::atomic<BeforeDispatchTransferPauseGate*> before_dispatch_transfer_gate_{nullptr};
    std::atomic<BeforeCommitBindingPauseGate*> before_commit_binding_gate_{nullptr};
    std::atomic<BeforeAdmissionLockPauseGate*> before_admission_lock_gate_{nullptr};
    std::atomic<BeforeQueueExitFn> before_queue_exit_fn_{nullptr};
    std::atomic<void*> before_queue_exit_ctx_{nullptr};

    // TAX-0 EXP-U0 research seam state: scan-mode selector + diagnostic
    // counters. PLAIN (non-atomic) members are sound: every
    // find_live_router_cookie_ callsite runs on the documented single-driver
    // call domain (poll/wait_one/submit/cancel serialization — the same
    // domain that already serializes CQE reap without dispatch_mtx_).
    // mutable: the lookup itself is const. Layout cost exists only in
    // internal-testing builds (AGENTS.md §15); production objects keep the
    // pre-seam layout.
    mutable RouterScanModeForTest router_scan_mode_for_test_ =
        RouterScanModeForTest::forward_production;
    mutable RouterScanDiagnosticsForTest router_diag_for_test_{};
    // TAX-0 router-fix shootout state (#255): candidate selector + the R3
    // fixed cookie table. Same single-driver call domain as the U0 seam
    // state above (plain members are sound; mutable: lookups are const).
    // Layout cost exists only in internal-testing builds (AGENTS.md §15).
    mutable RouterFixModeForTest router_fix_mode_for_test_ =
        RouterFixModeForTest::production_baseline;
    std::unique_ptr<RouterCookieTableForTest> cookie_table_for_test_;
    // R3 table maintenance. No-ops unless bounded_cookie_table is active;
    // called from the production dispatch/retire paths under this guard so
    // the table can never desync from the router it mirrors. Terminate on
    // any impossible state (duplicate insert / missing erase / probe
    // overrun) — deterministic fail-fast, never silent repair.
    void router_table_insert_(std::uint64_t cookie,
                              std::size_t router_index) noexcept;
    void router_table_erase_(std::uint64_t cookie) noexcept;
    // Fold one completed R3 table operation's probe count (micro-op and
    // production-path folding share this; single-driver domain).
    void fold_router_table_probes_for_test_(char which,
                                            std::uint64_t probes) const noexcept;
    // Fold one completed lookup's iterations into its callsite family.
    void fold_router_lookup_diag_for_test(RouterLookupKindForTest kind)
        const noexcept;
#endif
#endif // SLUICE_HAS_LIBURING

    bool available_ = false;
};

} // namespace sluice::async

#if defined(SLUICE_HAS_LIBURING) && defined(SLUICE_ASYNC_INTERNAL_TESTING)
// C4 (issue #135): the complete internal-testing control plane for
// UringAsyncBackend (gate struct definitions, *_for_test bodies, the
// transport-injection hooks) lives in the NON-INSTALLED seam header
// src/async/uring_test_seams.hpp, resolved via the internal-testing-only
// include path. Production and stub TUs never compile this include.
#include "uring_test_seams.hpp"
#endif  // defined(SLUICE_HAS_LIBURING) && defined(SLUICE_ASYNC_INTERNAL_TESTING)
