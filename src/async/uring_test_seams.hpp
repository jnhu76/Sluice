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
inline void UringAsyncBackend::wait_epoch_changed_for_test(
    BackendWaitToken observed) noexcept {
    // A missing wait source has no epochs to observe; silently returning
    // would park the test thread until the case watchdog with a
    // misleading stall report — fail fast with the real reason instead.
    assert(wait_source_ != nullptr &&
           "wait_epoch_changed_for_test: backend has no wait source "
           "(ring construction failed)");
    wait_source_->wait_epoch_changed(observed);
}
inline std::optional<BackendWaitToken> UringAsyncBackend::try_wait_token_for_test()
    const noexcept {
    // The assert keeps "no wait source" (a construction contract
    // failure) distinct from a nullopt caused by genuine leaf-domain
    // contention, which callers report as "locked".
    assert(wait_source_ != nullptr &&
           "try_wait_token_for_test: backend has no wait source "
           "(ring construction failed)");
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

}  // namespace sluice::async

#endif  // defined(SLUICE_HAS_LIBURING) && defined(SLUICE_ASYNC_INTERNAL_TESTING)
