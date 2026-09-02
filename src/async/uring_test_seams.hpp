// uring_test_seams.hpp - NON-INSTALLED internal-testing seam header for
// UringAsyncBackend (C4 / issue #135: the internal-testing control plane must
// not shape the installed production header).
//
// Contains, under SLUICE_HAS_LIBURING && SLUICE_ASYNC_INTERNAL_TESTING only:
//   - the UringBackendSubmitTestHooks transport-injection struct;
//   - the out-of-line definitions of the deterministic pause-gate nested
//     structs;
//   - the out-of-line `inline` definitions of the `*_for_test` observation /
//     mirror member functions (their declarations remain in
//     <sluice/async/uring_backend.hpp>).
//
// The installed header includes this file at its bottom under the same guard,
// so every internal-testing TU that includes uring_backend.hpp sees the
// complete types without per-test changes; production and stub builds
// compile none of it. This header is on the include path ONLY of the
// sluice_async_internal_testing target. No production behavior, symbol, or
// layout changes.
#pragma once

#include <sluice/async/uring_backend.hpp>

#if defined(SLUICE_HAS_LIBURING) && defined(SLUICE_ASYNC_INTERNAL_TESTING)

#include <cassert>
#include <cstdio>
#include <exception>

namespace sluice::async {

// Non-installed transport submit/wait seams used by the dedicated
// real-liburing fault tests.
// Production targets never define SLUICE_ASYNC_INTERNAL_TESTING and therefore
// expose neither this type nor the constructor overload.
struct UringBackendSubmitTestHooks {
    using SubmitFn = int (*)(void*, ::io_uring*) noexcept;
    using SubmitAndWaitFn = int (*)(void*, ::io_uring*, unsigned) noexcept;
    using BeforePoisonWaitFn = void (*)(void*) noexcept;

    void* context = nullptr;
    SubmitFn submit = nullptr;
    SubmitAndWaitFn submit_and_wait = nullptr;
    BeforePoisonWaitFn before_poison_wait = nullptr;
};

// ---- deterministic pause gates (mirror the ThreadPool discipline) ---------

struct UringAsyncBackend::AfterCommitBeforeEnqueuePauseGate {
    std::atomic<bool> paused{false};
    std::atomic<bool> resume{false};
    std::atomic<bool> exited{false};
};
struct UringAsyncBackend::BeforeDispatchTransferPauseGate {
    std::atomic<bool> paused{false};
    std::atomic<bool> resume{false};
    std::atomic<bool> exited{false};
    // true iff the gate fired with dispatch_mtx_ RELEASED (mirrors the
    // ThreadPool Gate-B discipline: the request stays enqueued while the
    // test drives cancel() against it).
    std::atomic<bool> dispatch_domain_released{false};
};
struct UringAsyncBackend::BeforeCommitBindingPauseGate {
    std::atomic<bool> paused{false};
    std::atomic<bool> resume{false};
    std::atomic<bool> exited{false};
    // true iff the gate fired INSIDE dispatch_mtx_ (the admission
    // transaction lock — close_admission() blocks on it while paused).
    std::atomic<bool> admission_domain_held{false};
};
struct UringAsyncBackend::BeforeAdmissionLockPauseGate {
    std::atomic<bool> paused{false};
    std::atomic<bool> resume{false};
    std::atomic<bool> exited{false};
};

// ---- observation mirrors (out-of-line inline member definitions) ---------

// Test-only: number of io_uring_submit() transport flushes actually issued
// (proves submit is transport progress, decoupled from lifecycle).
inline std::uint64_t UringAsyncBackend::submit_flushes_for_test() const noexcept {
    return submit_flushes_.load(std::memory_order_relaxed);
}
// Test-only: live operation cookies in the CqeRouter (bounded by
// request_capacity).
inline std::size_t UringAsyncBackend::live_cookies_for_test() const noexcept {
    return live_cookies_.load(std::memory_order_relaxed);
}
// Test-only: route a synthetic CQE (cookie + res) through the same
// handle_one_cqe path a real CQE takes. Used by the stale-cookie detector
// to prove a retired cookie no longer matches any LIVE router entry and is
// dropped (P0-B ABA fix). Does NOT touch the io_uring ring; it injects the
// CQE directly into the routing/terminal layer.
inline void UringAsyncBackend::inject_cqe_for_test(std::uint64_t cookie, int res) noexcept {
    handle_one_cqe(cookie, res);
}
// Test-only: read the next operation cookie that WILL be allocated by the
// next dispatch_one_locked without advancing the counter. Lets a test
// predict the cookie an in-flight op will carry so it can inject a stale
// cookie distinct from it. (next_cookie_ is mutated only under
// dispatch_mtx_; this snapshot is read single-driver.)
inline std::uint64_t UringAsyncBackend::peek_next_cookie_for_test() const noexcept {
    return next_cookie_;
}
// Test-only, single-driver read-only observation of the live router. Used
// to prove SQ-pressure enqueue dispatches the FIFO front rather than the
// newly appended tail. Offsets are unique in that detector.
inline std::optional<std::uint64_t>
UringAsyncBackend::live_cookie_for_offset_for_test(std::uint64_t offset) const noexcept {
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
inline Result<void> UringAsyncBackend::validate_write_for_test(WriteOp op) noexcept {
    return validate_write(op);
}
// Test-only: number of backend_ready slots not yet reaped.
inline std::size_t UringAsyncBackend::backend_ready_count_for_test() const noexcept {
    return arena_.backend_ready_count();
}
// Test-only: live tagged control execution references (submitted
// AsyncCancel SQEs not yet retired by their control CQE).
inline std::size_t UringAsyncBackend::live_control_sqes_for_test() const noexcept {
    return live_control_sqes_.load(std::memory_order_relaxed);
}

// --- Phase D3 C2b/C2c seams (rows 3-8 / 11-14a): mirror the approved
// ThreadPool observation style. Every seam delegates to REAL production
// authority (RequestArena, ReferenceReadySink, the production cancel
// core). No test-side state machine, no side-band identity/waiter map, no
// second generation counter. Guarded; production builds carry nothing. ---

// Resolve a Completion pointer to its current slot+generation (the same
// bounded arena scan the public cancel path uses).
inline std::optional<detail::SlotHandle>
UringAsyncBackend::handle_for_completion_for_test(const void* completion)
    const noexcept {
    return arena_.resolve_completion(completion);
}

// Single-lock observation that validates generation, context, and non-free
// state. Returns nullopt for a stale/released/unknown handle.
inline std::optional<detail::RequestArena::RequestObservation>
UringAsyncBackend::observe_for_test(detail::SlotHandle h) const noexcept {
    return arena_.observe_for_test(h);
}

// Identity-injection seam (C2b row 4): drive a CAPTURED SlotHandle through
// the SAME production cancel core the public Completion-keyed cancel() uses
// (dispatch remove_exact + arena_.cancel + terminal_won tally/signal).
// Proves a stale-generation handle cannot act on a live N+1 occupant.
inline detail::CancelDisposition UringAsyncBackend::cancel_handle_for_test(
    detail::SlotHandle h) noexcept {
    return cancel_handle_(h);
}

// Register one waiter on the slot bound to a real accepted Completion.
// Forwards verbatim to the arena authority (not_found for an unbound/stale
// Completion; invalid_state for a second registration or an
// already-reaped slot — registration is orthogonal to execution state,
// ADR Decision 10).
inline Result<void> UringAsyncBackend::register_waiter_for_test(
    Completion<std::size_t>& c, detail::WaiterToken token,
    detail::RoutingLease lease) {
    auto h = arena_.resolve_completion(&c);
    if (!h.has_value()) {
        return make_unexpected<void>(IoError{IoError::Code::not_found});
    }
    return arena_.register_waiter(*h, token, std::move(lease));
}
inline Result<void> UringAsyncBackend::register_waiter_for_test(
    Completion<void>& c, detail::WaiterToken token, detail::RoutingLease lease) {
    auto h = arena_.resolve_completion(&c);
    if (!h.has_value()) {
        return make_unexpected<void>(IoError{IoError::Code::not_found});
    }
    return arena_.register_waiter(*h, token, std::move(lease));
}

// Wait-cancel through the REAL arena authority: removes ONLY the waiter,
// never the I/O. Returns the moved-out RoutingLease, or not_found when no
// registered waiter remains.
inline Result<detail::RoutingLease> UringAsyncBackend::cancel_waiter_for_test(
    Completion<std::size_t>& c) {
    auto h = arena_.resolve_completion(&c);
    if (!h.has_value()) {
        return make_unexpected<detail::RoutingLease>(
            IoError{IoError::Code::not_found});
    }
    return arena_.cancel_waiter(*h);
}
inline Result<detail::RoutingLease> UringAsyncBackend::cancel_waiter_for_test(
    Completion<void>& c) {
    auto h = arena_.resolve_completion(&c);
    if (!h.has_value()) {
        return make_unexpected<detail::RoutingLease>(
            IoError{IoError::Code::not_found});
    }
    return arena_.cancel_waiter(*h);
}

// Stale-generation waiter injection (C2c row 14a): drive a CAPTURED
// SlotHandle through the REAL arena register/cancel_waiter authorities.
inline Result<void> UringAsyncBackend::register_waiter_handle_for_test(
    detail::SlotHandle h, detail::WaiterToken token, detail::RoutingLease lease) {
    return arena_.register_waiter(h, token, std::move(lease));
}
inline Result<detail::RoutingLease> UringAsyncBackend::cancel_waiter_handle_for_test(
    detail::SlotHandle h) {
    return arena_.cancel_waiter(h);
}

// Generation-validated by-value borrow snapshot for a captured SlotHandle.
inline std::optional<detail::RequestArena::BorrowSnapshot>
UringAsyncBackend::borrow_for_test(detail::SlotHandle h) const noexcept {
    return arena_.borrow_for_test(h);
}

// Generation-validated by-value single-waiter registration observation.
inline std::optional<detail::RequestArena::WaiterObservation>
UringAsyncBackend::waiter_for_test(detail::SlotHandle h) const noexcept {
    return arena_.waiter_for_test(h);
}

// C2c sink observation (fixed-size, allocation-free, test-only): the last
// delivered ReadyEvent's waiter payload + total delivery count.
inline std::size_t UringAsyncBackend::sink_deliveries() const noexcept {
    return sink_.deliveries();
}
inline bool UringAsyncBackend::sink_last_has_waiter() const noexcept {
    return sink_.last_has_waiter();
}
inline detail::WaiterToken UringAsyncBackend::sink_last_token() const noexcept {
    return sink_.last_token();
}
inline std::uint64_t UringAsyncBackend::sink_last_lease_id() const noexcept {
    return sink_.last_lease_id();
}

// ---- gate / wait-source setters --------------------------------------------

inline void UringAsyncBackend::set_after_commit_before_enqueue_pause_gate(
    AfterCommitBeforeEnqueuePauseGate* gate) noexcept {
    after_commit_before_enqueue_gate_.store(gate, std::memory_order_release);
}
inline void UringAsyncBackend::set_before_dispatch_transfer_pause_gate(
    BeforeDispatchTransferPauseGate* gate) noexcept {
    before_dispatch_transfer_gate_.store(gate, std::memory_order_release);
}
inline void UringAsyncBackend::set_before_commit_binding_pause_gate(
    BeforeCommitBindingPauseGate* gate) noexcept {
    before_commit_binding_gate_.store(gate, std::memory_order_release);
}
inline void UringAsyncBackend::set_before_admission_lock_pause_gate(
    BeforeAdmissionLockPauseGate* gate) noexcept {
    before_admission_lock_gate_.store(gate, std::memory_order_release);
}

// Phase D4 C2e split-phase-wait seams (forward to the wait source).
// Wait-phase entry flag: the wait source stores `true` immediately before
// it blocks in poll(2), so a test can deterministically observe "a
// participant has completed its empty reap and is now parked".
inline void UringAsyncBackend::set_wait_phase_flag_for_test(
    std::atomic<bool>* flag) noexcept {
    if (wait_source_) {
        wait_source_->set_wait_phase_flag(flag);
    }
}
// Per-participant pre-poll park counter: counts EACH waiter reaching the
// final pre-poll point, so the multi-waiter detector can wait for count ==
// N (bounded deadline = hang watchdog only) instead of a sleep.
inline void UringAsyncBackend::set_wait_prepark_counter_for_test(
    std::atomic<int>* counter) noexcept {
    if (wait_source_) {
        wait_source_->set_wait_prepark_counter(counter);
    }
}
// Deterministic interrupt-vs-final-ready window (fires when a control
// wake is about to be reported; see UringWaitSource).
inline void UringAsyncBackend::set_wait_control_wake_final_reap_pause_gate(
    detail::UringWaitSource::ControlWakeFinalReapPauseGate* gate) noexcept {
    if (wait_source_) {
        wait_source_->set_control_wake_final_reap_pause_gate(gate);
    }
}
// Deterministic pre-poll barrier (see UringWaitSource): one arrival per
// distinct participant reaching the physical-poll boundary.
inline void UringAsyncBackend::set_wait_before_physical_poll_pause_gate(
    detail::UringWaitSource::BeforePhysicalPollPauseGate* gate) noexcept {
    if (wait_source_) {
        wait_source_->set_before_physical_poll_pause_gate(gate);
    }
}
// Test-only ring-fd override (see UringWaitSource): poll this fd instead of
// the production ring fd. Install BEFORE launching the waiter.
inline void UringAsyncBackend::set_wait_poll_ring_fd_override_for_test(int fd) noexcept {
    if (wait_source_) {
        wait_source_->set_poll_ring_fd_override_for_test(fd);
    }
}
// Test-only poll(2) seam (see UringWaitSource): inject a deterministic
// poll outcome (e.g. non-EINTR failure) without an invalid fd.
inline void UringAsyncBackend::set_wait_poll_fn_for_test(
    detail::UringWaitSource::PollFn fn, void* ctx) noexcept {
    if (wait_source_) {
        wait_source_->set_poll_fn_for_test(fn, ctx);
    }
}

// Test-only epoch observer + try-reads for the case watchdog (issue
// #129; mirrors the ThreadPoolBackend watchdog seam). The blocking reads
// take wait-source/arena leaf locks, so a watchdog diagnosing a stall
// could otherwise block behind the very defect it is diagnosing (a
// paused control-wake gate holds the wait-source leaf mutex while
// spinning). The observer parks on the wait source's own mtx_ + cv_
// domain (see UringWaitSource::wait_epoch_changed); the try variants
// return nullopt when the domain is contended and the caller reports
// "locked". Compiled out of production sluice_async.
inline bool UringAsyncBackend::wait_epoch_changed_for_test(
    BackendWaitToken observed) noexcept {
    // T5 (docs/architecture/failure-model.md): a missing wait source is an
    // environment-availability condition — the backend is
    // constructible-but-unavailable because ring construction failed (kernel
    // without io_uring, sandbox seccomp) — not a programmer error. The typed
    // response is `false` ("no epochs to observe; nothing waited"), which the
    // caller turns into an explicit failed observation. A bare assert here
    // was Debug-only and left Release test binaries a null dereference.
    if (!wait_source_) return false;
    wait_source_->wait_epoch_changed(observed);
    return true;
}
inline std::optional<BackendWaitToken> UringAsyncBackend::try_wait_token_for_test()
    const noexcept {
    // T5 (docs/architecture/failure-model.md): nullopt now covers BOTH
    // "no wait source" (ring construction failed — environment
    // availability, not a programmer error; previously a bare assert that
    // vanished under NDEBUG into a null dereference) and genuine
    // leaf-domain contention (the watchdog "locked" report). The diagnostic
    // consumer cannot distinguish them, which is acceptable for a
    // best-effort stall report; the blocking seam
    // (wait_epoch_changed_for_test) returns the distinguishing `false`.
    if (!wait_source_) return std::nullopt;
    return wait_source_->try_snapshot();
}
inline std::optional<std::size_t> UringAsyncBackend::try_outstanding_for_test()
    const noexcept {
    return arena_.try_accepted_outstanding();
}
inline std::optional<std::size_t> UringAsyncBackend::try_backend_ready_count_for_test()
    const noexcept {
    return arena_.try_backend_ready_count();
}

// Deterministic destructor-order probe (D4-RM11 detector): an allocation-
// free function pointer + context invoked in the destructor BETWEEN the
// quiescent preflight and io_uring_queue_exit(). The death child installs
// a fn that _Exit(90) so a mutant that removes/bypasses the preflight is
// caught AT the teardown boundary (exit 90), distinct from exit 86
// (preflight fail-fast), 87 (unexpected return), 88 (child setup fail).
// Production behavior is unchanged when no fn is installed.
inline void UringAsyncBackend::set_before_queue_exit_hook_for_test(
    BeforeQueueExitFn fn, void* ctx) noexcept {
    before_queue_exit_fn_.store(fn, std::memory_order_release);
    before_queue_exit_ctx_.store(ctx, std::memory_order_release);
}

// ---- TAX-0 EXP-U0 router-scan research seam (#250 campaign) --------------
// Research-only scan-direction ablation + exact scan-iteration witness.
// set_router_scan_mode_for_test selects reverse_production (the shipped
// production scan since the R1 landing — also the default for every
// internal-testing construction) or forward_ablation (the pre-fix forward
// traversal, kept as the EXP-U0 causal-comparator direction: same matching
// predicate, low->high traversal). Call from a quiescent point on the
// single-driver domain (e.g. before the runtime starts driving the
// backend). The setter is deliberately NOT synchronized: the mode is read
// on the driver domain only.
inline void UringAsyncBackend::set_router_scan_mode_for_test(
    RouterScanModeForTest mode) noexcept {
    router_scan_mode_for_test_ = mode;
}
inline UringAsyncBackend::RouterScanModeForTest
UringAsyncBackend::router_scan_mode_for_test() const noexcept {
    return router_scan_mode_for_test_;
}
// Read-only resolution through the EXACT production lookup under the
// current mode; diagnostics fold exactly as a real call would.
inline std::size_t UringAsyncBackend::find_live_router_cookie_for_test(
    std::uint64_t cookie) const noexcept {
    return find_live_router_cookie_(cookie);
}
inline const UringAsyncBackend::RouterScanDiagnosticsForTest&
UringAsyncBackend::router_scan_diagnostics_for_test() const noexcept {
    return router_diag_for_test_;
}
inline void UringAsyncBackend::reset_router_scan_diagnostics_for_test() noexcept {
    router_diag_for_test_ = RouterScanDiagnosticsForTest{};
}

// ---- TAX-0 router-fix candidate shootout seam (#255 campaign) ------------

// Deterministic research fail-fast for impossible table states. Same
// discipline as the production invariant violations (typed message +
// terminate); unreachable for any caller that honors the insert-on-install /
// erase-on-retire pairing.
[[noreturn]] inline void router_table_fatal_(const char* what) {
    std::fprintf(stderr,
                 "sluice::async::RouterCookieTableForTest: %s "
                 "(impossible internal state - invariant violation)\n",
                 what);
    std::fflush(stderr);
    std::terminate();
}

// R3 candidate: fixed-capacity open-addressed cookie -> router-index table
// (linear probing, backward-shift deletion - NO tombstones). Bounded at
// construction: capacity = next power of two >= 2 * request_capacity
// (>= 16), so the load factor stays <= 50% at the legal maximum of C live
// cookies and an empty slot always terminates every probe. Identity
// contract preserved by construction: the kernel-visible user_data stays
// the no-wrap never-reused operation cookie; this table is pure derived
// transport metadata (insert exactly on router install, erase exactly on
// router retirement), so a stale cookie's probe walks to an empty slot and
// misses - the same semantic answer the production linear scan produces.
// Zero steady-state allocation; teardown is one delete. Research-only
// (#255); production builds never compile this type.
struct RouterCookieTableForTest {
    struct Slot {
        std::uint64_t cookie = 0; // 0 = empty (operation cookies are >= 1)
        std::uint32_t router_index = 0;
        std::uint32_t pad = 0;
    };

    static constexpr std::size_t kMiss = static_cast<std::size_t>(-1);

    std::uint32_t log2_size = 0;
    std::size_t size = 0;      // == 1 << log2_size
    std::size_t mask = 0;      // size - 1
    std::vector<Slot> slots;   // fixed at construction; never resized
    // Probe count of the most recent operation (single-driver domain - the
    // same domain that owns the backend diagnostics).
    mutable std::uint64_t last_probes = 0;

    explicit RouterCookieTableForTest(std::size_t request_capacity) {
        std::size_t want = request_capacity * 2;
        if (want < 16)
            want = 16;
        while ((std::size_t{1} << log2_size) < want)
            ++log2_size;
        size = std::size_t{1} << log2_size;
        mask = size - 1;
        slots.assign(size, Slot{});
    }

    static std::size_t hash(std::uint64_t cookie, std::uint32_t log2) noexcept {
        // Multiply-shift (Fibonacci) hashing: spreads the sequential
        // operation-cookie domain across the table without stored state.
        return static_cast<std::size_t>(
            (cookie * std::uint64_t{0x9E3779B97F4A7C15ull}) >> (64 - log2));
    }

    void insert(std::uint64_t cookie, std::size_t router_index) noexcept {
        // 0 is the empty-slot sentinel AND outside the operation-cookie
        // domain [1, 2^63-1] (allocate_cookie_ starts at 1, never returns
        // 0); a 0 insert is an impossible state, not a silent drop.
        if (cookie == 0)
            router_table_fatal_("insert of cookie 0 (outside key domain)");
        std::uint64_t probes = 0;
        std::size_t i = hash(cookie, log2_size);
        for (;;) {
            Slot& s = slots[i];
            if (s.cookie == 0) {
                s.cookie = cookie;
                s.router_index = static_cast<std::uint32_t>(router_index);
                last_probes = probes + 1;
                return;
            }
            if (s.cookie == cookie)
                router_table_fatal_("duplicate insert");
            ++probes;
            if (probes > size)
                router_table_fatal_("insert probe overrun (table full)");
            i = (i + 1) & mask;
        }
    }

    // kMiss on miss (stale/unknown cookie - the expected stale-CQE answer,
    // identical to the linear scan's not-found). Cookie 0 is outside the
    // operation-cookie domain: it misses by domain (the production linear
    // scan agrees - no live entry ever carries cookie 0).
    std::size_t lookup(std::uint64_t cookie) const noexcept {
        if (cookie == 0) {
            last_probes = 1;
            return kMiss;
        }
        std::uint64_t probes = 0;
        std::size_t i = hash(cookie, log2_size);
        for (;;) {
            const Slot& s = slots[i];
            if (s.cookie == cookie) {
                last_probes = probes + 1;
                return s.router_index;
            }
            if (s.cookie == 0) {
                last_probes = probes + 1;
                return kMiss;
            }
            ++probes;
            if (probes > size)
                router_table_fatal_("lookup probe overrun (no empty slot)");
            i = (i + 1) & mask;
        }
    }

    void erase(std::uint64_t cookie) noexcept {
        if (cookie == 0)
            router_table_fatal_("erase of cookie 0 (outside key domain)");
        std::uint64_t probes = 0;
        std::size_t i = hash(cookie, log2_size);
        for (;;) {
            if (slots[i].cookie == cookie)
                break;
            if (slots[i].cookie == 0)
                router_table_fatal_("erase of absent cookie");
            ++probes;
            if (probes > size)
                router_table_fatal_("erase probe overrun (no empty slot)");
            i = (i + 1) & mask;
        }
        const std::uint64_t erase_probes = probes + 1;
        // Backward-shift deletion: every cluster entry after the hole whose
        // probe path crosses the hole moves into it; the first entry whose
        // home lies strictly beyond the hole stops the shift. Keeps the
        // table tombstone-free so lookups always terminate at an empty slot.
        std::size_t hole = i;
        std::size_t j = (i + 1) & mask;
        while (slots[j].cookie != 0) {
            const std::size_t home = hash(slots[j].cookie, log2_size);
            const std::size_t d_hole = (hole + size - home) & mask;
            const std::size_t d_j = (j + size - home) & mask;
            if (d_hole <= d_j) {
                slots[hole] = slots[j];
                hole = j;
            }
            j = (j + 1) & mask;
        }
        slots[hole] = Slot{};
        last_probes = erase_probes;
    }

    std::size_t fixed_bytes() const noexcept { return size * sizeof(Slot); }
};

inline void UringAsyncBackend::set_router_fix_mode_for_test(
    RouterFixModeForTest mode) noexcept {
    // Fresh-backend operation: the whole backend must be quiescent and the
    // router fully retired before the physical placement may change.
    if (outstanding() != 0 || live_cookies_.load(std::memory_order_relaxed) != 0 ||
        cookie_free_list_.size() != router_.size()) {
        std::fprintf(stderr,
                     "sluice::async::UringAsyncBackend: router-fix mode "
                     "switch on a non-quiescent backend (invariant "
                     "violation)\n");
        std::fflush(stderr);
        std::terminate();
    }
    router_fix_mode_for_test_ = mode;
    // Reseed the free list in the mode's physical order. Cookie values are
    // NEVER reseeded (next_cookie_ is untouched - no identity reset).
    cookie_free_list_.assign(router_.size(), detail::SlotIndex{0});
    if (mode == RouterFixModeForTest::low_placement_forward) {
        // Descending seed: back() == index 0 first, so the live set parks
        // at LOW indices under the unchanged back()-pop/back()-push LIFO.
        for (std::size_t i = 0; i < router_.size(); ++i)
            cookie_free_list_[i] =
                detail::SlotIndex{static_cast<std::uint32_t>(router_.size() - 1 - i)};
    } else {
        // Ascending seed: the production placement (back() == highest).
        for (std::size_t i = 0; i < router_.size(); ++i)
            cookie_free_list_[i] = detail::SlotIndex{static_cast<std::uint32_t>(i)};
    }
}
inline UringAsyncBackend::RouterFixModeForTest
UringAsyncBackend::router_fix_mode_for_test() const noexcept {
    return router_fix_mode_for_test_;
}
// Layer-A microbench micro-ops. install mirrors ONLY the router-install
// slice of dispatch_one_locked (free-list pop, no-wrap cookie allocation,
// RouterEntry install, R3 table insert); it never touches the SQE ring,
// arena, dispatch queue, or transport ledger.
inline std::size_t UringAsyncBackend::router_install_cookie_for_test() noexcept {
    if (cookie_free_list_.empty()) {
        std::fprintf(stderr,
                     "sluice::async::UringAsyncBackend: router exhaustion "
                     "(invariant violation)\n");
        std::fflush(stderr);
        std::terminate();
    }
    detail::SlotIndex router_slot = cookie_free_list_.back();
    cookie_free_list_.pop_back();
    const std::uint64_t op_cookie = allocate_cookie_(); // no-wrap; fail-fast
    RouterEntry& route = router_[router_slot.value];
    route = RouterEntry{};
    route.cookie = op_cookie;
    route.in_use = true;
    live_cookies_.fetch_add(1, std::memory_order_relaxed);
    router_table_insert_(op_cookie, router_slot.value);
    return router_slot.value;
}
inline void UringAsyncBackend::router_retire_cookie_for_test(
    std::size_t router_index) noexcept {
    // The EXACT production retirement path (invariant checks + R3 table
    // erase + free-list push + live-cookie accounting).
    retire_router_entry_(router_index);
}
// Structural memory facts for the Layer-A evidence.
inline std::size_t UringAsyncBackend::router_entry_bytes_for_test() noexcept {
    return sizeof(RouterEntry);
}
inline std::size_t UringAsyncBackend::router_table_bytes_for_test()
    const noexcept {
    return cookie_table_for_test_ ? cookie_table_for_test_->fixed_bytes() : 0;
}

}  // namespace sluice::async

#endif  // defined(SLUICE_HAS_LIBURING) && defined(SLUICE_ASYNC_INTERNAL_TESTING)
