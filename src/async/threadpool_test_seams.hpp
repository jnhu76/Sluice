// threadpool_test_seams.hpp - NON-INSTALLED internal-testing seam header for
// ThreadPoolBackend (C4 / issue #135: the internal-testing control plane must
// not shape the installed production header).
//
// Contains, under SLUICE_ASYNC_INTERNAL_TESTING only:
//   - the out-of-line definitions of the deterministic pause-gate and
//     failure-injection nested structs;
//   - the out-of-line `inline` definitions of the `*_for_test` observation /
//     mirror member functions (their declarations remain in
//     <sluice/async/threadpool_backend.hpp>);
//   - the test-side gate resume/rearm and #110 generation-handshake helpers.
//
// The installed header includes this file at its bottom under the same guard,
// so every internal-testing TU that includes threadpool_backend.hpp sees the
// complete types without per-test changes; production TUs (macro undefined)
// compile none of it. This header is on the include path ONLY of the
// sluice_async_internal_testing target; the production sluice_async target
// cannot see it. No production behavior, symbol, or layout changes.
#pragma once

#include <sluice/async/threadpool_backend.hpp>

#if defined(SLUICE_ASYNC_INTERNAL_TESTING)

#include <atomic>
#include <cstdint>
#include <mutex>
#include <optional>

namespace sluice::async {

// ---- deterministic pause gates -------------------------------------------
// The PRODUCTION side of the protocol is universally blocking (issue #92,
// completing #86-B): the production path sets `paused` (and calls
// paused.notify_one) to mark the exact observation window, BLOCKS on
// `resume.wait(false)` until the test resumes it, then sets `exited` (and
// calls exited.notify_one) when it leaves. The production resume side is the
// one part that is blocking everywhere; test consumers are NOT required to
// block on paused.wait/exited.wait uniformly — a consumer may pair the
// blocking waits (paused.wait / exited.wait) OR use an existing bounded
// observation loop (e.g. yield + poll). When a test does resume the gate it
// MUST do so ONLY through resume_threadpool_gate() (release-store +
// notify_all), so every resume publisher provably pairs its store with a
// notification — a missing notify would leave the production thread blocked
// in resume.wait and is caught by the case-level watchdog, never a silent
// spin. These are compiled out of production sluice_async; the layout cost
// in the internal-testing target is accepted and documented (AGENTS.md §3.9).
struct ThreadPoolBackend::AfterArenaEnqueueBeforeDispatchPushPauseGate {
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

// Issue #110: the {paused, resume, exited} booleans below carry NO
// iteration identity. `exited` proves only that the worker left the pause
// gate — NOT that it consumed (or observed empty) the dispatch entry of the
// current test iteration. A test that rearms the booleans and submits
// iteration N+1 after observing `exited == true` can race the still
// descheduled iteration-N worker continuation (between gate exit and
// pop_front), which then consumes N+1's dispatch entry WITHOUT visiting
// the re-armed gate — a permanent wait_paused stall (the #110 defect).
// The bool trio therefore supports SINGLE-VISIT use only (arm once, pause
// once, resume once, never rearm the same gate object); multi-iteration
// protocols MUST use the generation handshake fields below.
struct ThreadPoolBackend::BeforeWorkerDequeuePauseGate {
    // Legacy single-visit bool protocol (single-shot cases only).
    std::atomic<bool> paused{false};
    std::atomic<bool> resume{false};
    std::atomic<bool> exited{true};

    // Issue #110 generation-scoped handshake (multi-iteration race loop).
    // All four are monotonic for the life of the gate object — never
    // reset, so no ABA-style false observation is possible. The test is
    // the sole writer of `armed`/`resumed_at`; the (single) worker is the
    // sole writer of `paused_at`/`acked_at`. `armed == 0` (the initial
    // value) selects the legacy bool path in the production seam; a test
    // that arms a generation MUST resume it ONLY through
    // resume_dequeue_gate_generation (a legacy resume_threadpool_gate
    // store would not release the generation wait — the stall is caught
    // by the case watchdog, never silently mis-synchronized).
    //
    // Disarm constraint (review follow-up): do NOT disarm or swap the
    // gate pointer while a generation visit is in flight (between arming
    // a generation and observing its ACK). The ACK publisher re-loads the
    // gate pointer, so an in-flight swap silently drops the ACK and
    // wait_dequeue_gate_ack stalls until the case watchdog. Disarming
    // between fully ACKed visits is fine; legacy gen-0 visits carry no
    // ACK obligation and may disarm after observing `exited`.
    //
    //   test:  arm(N)                      -> submit iteration N
    //   worker: pauses                     -> publishes paused_at >= N
    //   test:  observes paused_at >= N     -> races cancel vs dequeue
    //   test:  resume(N)
    //   worker: crosses THIS cycle's pop_front decision
    //                                   -> publishes acked_at >= N  (the ACK)
    //   test:  waits acked_at >= N         -> ONLY THEN arms N+1
    //
    // The ACK is published after the dequeue decision of the pausing
    // cycle: once popped (or observed empty), that worker continuation
    // can consume another entry only by re-entering work_cv_ wait ->
    // gate check -> pop, so it cannot steal iteration N+1's entry
    // without visiting the N+1 gate.
    std::atomic<std::uint64_t> armed{0};
    std::atomic<std::uint64_t> paused_at{0};
    std::atomic<std::uint64_t> resumed_at{0};
    std::atomic<std::uint64_t> acked_at{0};
};

// Issue #110 deterministic regression seam (single visit): a plain bool
// pause placed between the BeforeWorkerDequeuePauseGate resume-wait
// returning and the worker re-taking work_mtx_ / pop_front — the EXACT
// post-gate / pre-dequeue window in which the old bool protocol already
// published `exited == true`. Held with work_mtx_ released so the test
// can submit/cancel/inspect. Used only by the cross-generation theft
// regression; null (no-op) everywhere else.
struct ThreadPoolBackend::PostResumePrePopHoldGate {
    std::atomic<bool> paused{false};
    std::atomic<bool> resume{false};
    std::atomic<bool> exited{true};
};
struct ThreadPoolBackend::WorkerRunningPauseGate {
    std::atomic<bool> paused{false};
    std::atomic<bool> resume{false};
    std::atomic<bool> exited{true};
};
struct ThreadPoolBackend::TerminalPublicationPauseGate {
    std::atomic<bool> paused{false};
    std::atomic<bool> resume{false};
    std::atomic<bool> exited{true};
};
// C2e (row 15): deterministic interrupt-vs-final-ready window. wait_one()
// pauses between the interrupted control wake and its ONE final reap, so a
// test can record the final terminal in that exact window and prove the
// final reap returns it (the control interrupt never swallows the last
// ready). Compiled out of production sluice_async (see
// tp_c2e_interrupt_final_reap_closes_ready_race; mutant M4 detector).
struct ThreadPoolBackend::ControlWakeFinalReapPauseGate {
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
struct ThreadPoolBackend::BeforeEnqueueLockPauseGate {
    std::atomic<bool> paused{false};
    std::atomic<bool> resume{false};
    std::atomic<bool> exited{true};
};
// C2e (row 15; B1): deterministic admission-transaction windows. The
// submit path pauses (a) BEFORE taking admission_mtx_ — the close-wins
// arbitration: close_admission() completes with no contention and the
// resumed submit must reject at reserve (ADR Decision 15); and (b) AFTER
// arena_.commit() and BEFORE install_binding/commit_binding — the
// close-waits arbitration: close_admission() must BLOCK on the in-flight
// Step 1-5 acceptance protocol, because the `binding -> outstanding`
// release-store (ADR §"Commit / accept" Step 5) is the commit/accept
// linearization point and no new acceptance LP may occur after close
// returns. See tp_c2e_close_waits_for_inflight_acceptance_lp /
// tp_c2e_close_wins_submit_started_before_close_rejected (mutant M11
// detector). Compiled out of production sluice_async.
struct ThreadPoolBackend::BeforeAdmissionLockPauseGate {
    std::atomic<bool> paused{false};
    std::atomic<bool> resume{false};
    std::atomic<bool> exited{true};
};
struct ThreadPoolBackend::BeforeCommitBindingPauseGate {
    std::atomic<bool> paused{false};
    std::atomic<bool> resume{false};
    std::atomic<bool> exited{true};
};

// ---- failure-injection control structs ------------------------------------

// Post-commit dispatch-failure injection. When `armed` and the submit
// path's enqueue won (outcome == enqueued), the submit path records the
// defined `backend_error` terminal through the arena's terminal-winner
// authority INSTEAD of pushing the handle onto the dispatch ring — the
// ADR Decision-12 "post-commit dispatch failure after execution ownership
// is proven absent" winner candidate (AGENTS.md §3.2). The handle was
// never visible to any worker (workers dequeue only under work_mtx_, which
// the injection holds), so no worker, ring, kernel, or other executor
// holds execution ownership; submit still returns success; reap publishes
// the defined terminal exactly once. `fired` increments exactly once per
// injected submit; the test reads it to distinguish "injection fired" from
// "a cancel won first". The control object must be declared before the
// backend and outlive it (same lifetime rule as the pause gates).
struct ThreadPoolBackend::DispatchFailureInjection {
    std::atomic<bool> armed{false};
    std::atomic<std::size_t> fired{0};
};

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
// "seam fired" from a natural failure. TEST-ONLY (AGENTS.md §3.9):
// production builds carry no branch, no local, no symbol (the whole seam
// block is compiled out).
struct ThreadPoolBackend::SubmitStageFailureInjection {
    std::atomic<bool> fail_reserve{false};
    std::atomic<bool> fail_prepare{false};
    std::atomic<bool> fail_commit{false};
    std::atomic<std::size_t> reserve_fired{0};
    std::atomic<std::size_t> prepare_fired{0};
    std::atomic<std::size_t> commit_fired{0};
};

// ---- observation mirrors (out-of-line inline member definitions) ---------

// Test-only: persistent workers spawned (== worker_count for the backend's
// whole life; never grows). Replaces the legacy unjoined_workers_for_test
// (the per-op thread model is gone).
inline std::size_t ThreadPoolBackend::workers_spawned_for_test() const noexcept {
    return workers_.size();
}
// Test-only: active workers currently between mark_running and
// record_terminal. Bounded by worker_count.
inline std::size_t ThreadPoolBackend::active_workers_for_test() const {
    std::lock_guard<std::mutex> lk(work_mtx_);
    return active_workers_;
}
// Test-only: current dispatch ring occupancy and high-water mark.
inline std::size_t ThreadPoolBackend::dispatch_size_for_test() const {
    std::lock_guard<std::mutex> lk(work_mtx_);
    return dispatch_.size();
}
inline std::size_t ThreadPoolBackend::dispatch_high_water_for_test() const {
    std::lock_guard<std::mutex> lk(work_mtx_);
    return dispatch_.high_water();
}
// Test-only: number of real syscalls the workers have executed (for
// cancel/no-execute assertions). Monotonic; not a public contract.
inline std::uint64_t ThreadPoolBackend::syscall_count_for_test() const noexcept {
    return syscall_count_.load();
}

// Test-only: number of backend_ready slots not yet reaped.
inline std::size_t ThreadPoolBackend::backend_ready_count_for_test() const noexcept {
    return arena_.backend_ready_count();
}

// Test-only try-reads for the case watchdog (issue #128 review): the
// blocking wait-source/arena reads above take the corresponding leaf
// locks, so a watchdog diagnosing a stall could otherwise block behind
// the very defect it is diagnosing. These try-lock variants return
// nullopt when the domain is contended; the caller reports "locked".
// Compiled out of production sluice_async.
inline std::optional<BackendWaitToken> ThreadPoolBackend::try_wait_token_for_test()
    const noexcept {
    return ready_wait_.try_snapshot();
}
inline std::optional<std::size_t> ThreadPoolBackend::try_outstanding_for_test()
    const noexcept {
    return arena_.try_accepted_outstanding();
}
inline std::optional<std::size_t> ThreadPoolBackend::try_backend_ready_count_for_test()
    const noexcept {
    return arena_.try_backend_ready_count();
}

// Test-only: wait-phase entry flag (issue #67 drain-starvation regression).
// The ready wait domain stores `true` into the pointed-to atomic
// immediately before it blocks in the ready-cv wait, so a test can
// deterministically observe "a participant has completed its empty reap
// and is now parked in the backend ready wait" (the exact state from
// which the old code held access_mtx_ across the block and starved every
// other poll/reap path). Disarm by passing nullptr. Compiled out of
// production sluice_async.
inline void ThreadPoolBackend::set_wait_phase_flag_for_test(
    std::atomic<bool>* flag) noexcept {
    ready_wait_.set_wait_phase_flag(flag);
}

// Test-only: per-entry wait counter (D4-RM14 commit-to-park re-entry
// detector). Counts every wait_for_change entry of the ready wait domain
// (monotonic — no reset race), so a test can prove the run terminated and
// RE-ENTERED after a stop injected in the commit-to-wait_one window: the
// re-entered participant's second entry parks; a single entry means the
// first wait parked THROUGH the interrupt (the P0-1 mutant). Disarm by
// passing nullptr. Compiled out of production sluice_async.
inline void ThreadPoolBackend::set_wait_prepark_counter_for_test(
    std::atomic<int>* counter) noexcept {
    ready_wait_.set_wait_prepark_counter(counter);
}

// Test-only: zero-CPU blocking observer on the ACTUAL ready-wait epochs
// (ReadyWaitSource::wait_epoch_changed). Blocks until the control/progress
// epoch pair differs from `observed`, parking on the ready wait source's
// own mtx_ + ready_cv_ predicate domain (single source of truth — the
// epoch fields; no second notification channel). Compiled out of
// production sluice_async. Non-const: the observer parks on the cv.
inline void ThreadPoolBackend::wait_epoch_changed_for_test(
    BackendWaitToken observed) noexcept {
    ready_wait_.wait_epoch_changed(observed);
}

// Test-only: resolve a Completion pointer to its current slot+generation.
// Returns nullopt if the Completion is not bound to any slot.
inline std::optional<detail::SlotHandle>
ThreadPoolBackend::handle_for_completion_for_test(const void* completion)
    const noexcept {
    return arena_.resolve_completion(completion);
}

// Test-only: single-lock observation that validates generation, context, and
// non-free state. Returns nullopt for a stale/released/unknown handle.
inline std::optional<detail::RequestArena::RequestObservation>
ThreadPoolBackend::observe_for_test(detail::SlotHandle h) const noexcept {
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
inline detail::CancelDisposition ThreadPoolBackend::cancel_handle_for_test(
    detail::SlotHandle h) noexcept {
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

// ---- gate / injection setters ---------------------------------------------

inline void ThreadPoolBackend::set_after_enqueue_before_push_pause_gate(
    AfterArenaEnqueueBeforeDispatchPushPauseGate* gate) noexcept {
    after_enqueue_before_push_gate_.store(gate, std::memory_order_release);
}
inline void ThreadPoolBackend::set_before_dequeue_pause_gate(
    BeforeWorkerDequeuePauseGate* gate) noexcept {
    before_dequeue_gate_.store(gate, std::memory_order_release);
}
// Issue #110: arm the post-resume/pre-pop hold seam (single visit; disarm
// by storing nullptr between gate generations). Test-only.
inline void ThreadPoolBackend::set_post_resume_pre_pop_hold_gate(
    PostResumePrePopHoldGate* gate) noexcept {
    post_resume_pre_pop_hold_gate_.store(gate, std::memory_order_release);
}
inline void ThreadPoolBackend::set_running_pause_gate(
    WorkerRunningPauseGate* gate) noexcept {
    running_gate_.store(gate, std::memory_order_release);
}
inline void ThreadPoolBackend::set_terminal_publication_pause_gate(
    TerminalPublicationPauseGate* gate) noexcept {
    terminal_publication_gate_.store(gate, std::memory_order_release);
}
inline void ThreadPoolBackend::set_control_wake_final_reap_pause_gate(
    ControlWakeFinalReapPauseGate* gate) noexcept {
    control_wake_final_reap_gate_.store(gate, std::memory_order_release);
}
inline void ThreadPoolBackend::set_before_enqueue_lock_pause_gate(
    BeforeEnqueueLockPauseGate* gate) noexcept {
    before_enqueue_lock_gate_.store(gate, std::memory_order_release);
}
inline void ThreadPoolBackend::set_before_admission_lock_pause_gate(
    BeforeAdmissionLockPauseGate* gate) noexcept {
    before_admission_lock_gate_.store(gate, std::memory_order_release);
}
inline void ThreadPoolBackend::set_before_commit_binding_pause_gate(
    BeforeCommitBindingPauseGate* gate) noexcept {
    before_commit_binding_gate_.store(gate, std::memory_order_release);
}
inline void ThreadPoolBackend::set_dispatch_failure_injection(
    DispatchFailureInjection* injection) noexcept {
    dispatch_failure_injection_.store(injection, std::memory_order_release);
}
inline void ThreadPoolBackend::set_submit_stage_failure_injection(
    SubmitStageFailureInjection* injection) noexcept {
    submit_stage_failure_injection_.store(injection, std::memory_order_release);
}

// ---- Phase C2c mirrors: route a real accepted Completion through the REAL
// arena waiter/borrow authorities. No side-band waiter map, no
// reimplementation of the waiter state machine: the Completion is resolved
// to its current SlotHandle by the arena's own bounded scan (the same
// identity bridge the public cancel path uses) and the call is forwarded
// verbatim. Guarded; production builds carry nothing. ----------------------

// Register one waiter on the slot bound to a real accepted Completion.
// Returns the arena's own register_waiter result (not_found for an
// unbound/stale Completion; invalid_state for a second registration or a
// non-accepted/unreaped slot — registration is orthogonal to execution
// state, ADR Decision 10).
inline Result<void> ThreadPoolBackend::register_waiter_for_test(
    Completion<std::size_t>& c, detail::WaiterToken token,
    detail::RoutingLease lease) {
    auto h = arena_.resolve_completion(&c);
    if (!h.has_value()) {
        return make_unexpected<void>(IoError{IoError::Code::not_found});
    }
    return arena_.register_waiter(*h, token, std::move(lease));
}
inline Result<void> ThreadPoolBackend::register_waiter_for_test(
    Completion<void>& c, detail::WaiterToken token, detail::RoutingLease lease) {
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
inline Result<detail::RoutingLease> ThreadPoolBackend::cancel_waiter_for_test(
    Completion<std::size_t>& c) {
    auto h = arena_.resolve_completion(&c);
    if (!h.has_value()) {
        return make_unexpected<detail::RoutingLease>(
            IoError{IoError::Code::not_found});
    }
    return arena_.cancel_waiter(*h);
}
inline Result<detail::RoutingLease> ThreadPoolBackend::cancel_waiter_for_test(
    Completion<void>& c) {
    auto h = arena_.resolve_completion(&c);
    if (!h.has_value()) {
        return make_unexpected<detail::RoutingLease>(
            IoError{IoError::Code::not_found});
    }
    return arena_.cancel_waiter(*h);
}

// Stale-generation waiter injection (C2c row 14a): drive a CAPTURED
// SlotHandle (typically a stale-generation handle from a released occupant)
// through the REAL arena register/cancel_waiter authorities. This is what
// proves a stale waiter authority cannot act on a live N+1 occupant's
// registration: handle validation rejects it with not_found and zero side
// effect. Mirrors cancel_handle_for_test (C2b) — pointer-free identity only,
// no Completion reverse map, no side-band waiter map.
inline Result<void> ThreadPoolBackend::register_waiter_handle_for_test(
    detail::SlotHandle h, detail::WaiterToken token, detail::RoutingLease lease) {
    return arena_.register_waiter(h, token, std::move(lease));
}
inline Result<detail::RoutingLease> ThreadPoolBackend::cancel_waiter_handle_for_test(
    detail::SlotHandle h) {
    return arena_.cancel_waiter(h);
}

// Generation-validated by-value borrow snapshot for a captured SlotHandle.
inline std::optional<detail::RequestArena::BorrowSnapshot>
ThreadPoolBackend::borrow_for_test(detail::SlotHandle h) const noexcept {
    return arena_.borrow_for_test(h);
}

// Generation-validated by-value single-waiter registration observation.
inline std::optional<detail::RequestArena::WaiterObservation>
ThreadPoolBackend::waiter_for_test(detail::SlotHandle h) const noexcept {
    return arena_.waiter_for_test(h);
}

// C2c sink observation (fixed-size, allocation-free, test-only): the last
// delivered ReadyEvent's waiter payload + total delivery count. Read after
// poll()/wait_one() returns. sink_deliveries() mirrors the FakeAsyncBackend
// seam (reference_backend_arena_lifecycle_test uses it there).
inline std::size_t ThreadPoolBackend::sink_deliveries() const noexcept {
    return sink_.deliveries();
}
inline bool ThreadPoolBackend::sink_last_has_waiter() const noexcept {
    return sink_.last_has_waiter();
}
inline detail::WaiterToken ThreadPoolBackend::sink_last_token() const noexcept {
    return sink_.last_token();
}
inline std::uint64_t ThreadPoolBackend::sink_last_lease_id() const noexcept {
    return sink_.last_lease_id();
}

// ---- test-side gate helpers -----------------------------------------------

// Issue #92: the bidirectional wait/notify helpers for the ThreadPoolBackend
// pause gates. The production seam (wait_*_pause_) blocks on
// `resume.wait(false, acquire)`; therefore EVERY test-side resume publisher
// MUST perform the release-store AND the matching notification. Calling these
// helpers is the only supported way to resume a ThreadPoolBackend gate, so the
// store+notify pair cannot be forgotten at a call site. notify_all is used so
// the resume notification does not depend on a permanent single-waiter
// assumption; it is a harmless no-op when no thread is in atomic::wait. This
// does NOT make the bool-based {paused,resume,exited} protocol a general
// multi-waiter rearm barrier: reusable gate protocols still require their
// documented exit/rearm discipline (observe exited==true, then rearm).
// SLUICE_ASYNC_INTERNAL_TESTING behavior only — production sluice_async carries
// no pause symbol and its ThreadPool semantics are unchanged.
template <class Gate>
void resume_threadpool_gate(Gate& gate) noexcept {
    gate.resume.store(true, std::memory_order_release);
    gate.resume.notify_all();
}

// Re-arm a gate for reuse AFTER the test has observed `exited == true` (the
// production path has fully left the gate). Resets to the constructed state
// {paused=false, resume=false, exited=true}. resume=false is safe here because
// no production thread can be blocked in resume.wait(false) once exited was
// observed true: a production thread only reaches resume.wait AFTER setting
// exited=false. Centralizing the order locks the re-arm invariant so a future
// reuse site cannot reset resume=false under a still-waiting epoch.
template <class Gate>
void rearm_threadpool_gate(Gate& gate) noexcept {
    gate.paused.store(false, std::memory_order_release);
    gate.resume.store(false, std::memory_order_release);
    gate.exited.store(true, std::memory_order_release);
}

// Issue #110: generation-scoped handshake for BeforeWorkerDequeuePauseGate
// (see the struct's field comment for the full protocol). These are the ONLY
// supported test-side operations on a generation-armed gate. All
// publications use monotonic max (C++20 has no atomic fetch_max — P0493 is
// C++26 — so the max is a compare_exchange_weak loop), and every publication
// a waiter can block on is paired with notify_all: atomic::wait permits
// spurious unblocking, so every consumer below re-checks the value in a
// predicate loop (never trusts a single wake).
namespace dequeue_gate_detail {
inline void publish_max_(std::atomic<std::uint64_t>& a, std::uint64_t v) noexcept {
    std::uint64_t cur = a.load(std::memory_order_relaxed);
    while (cur < v &&
           !a.compare_exchange_weak(cur, v, std::memory_order_release,
                                    std::memory_order_relaxed)) {
    }
}
}  // namespace dequeue_gate_detail

// Arm generation `generation` (strictly increasing per gate object; never
// reset). MUST be called BEFORE the submit whose dispatch entry the worker
// will meet at this gate visit. Nobody waits on `armed`, so no notify.
inline void arm_dequeue_gate_generation(ThreadPoolBackend::BeforeWorkerDequeuePauseGate& gate,
                                        std::uint64_t generation) noexcept {
    dequeue_gate_detail::publish_max_(gate.armed, generation);
}

// Block (zero-CPU) until the worker has paused for `generation` or later.
inline void wait_dequeue_gate_paused(ThreadPoolBackend::BeforeWorkerDequeuePauseGate& gate,
                                     std::uint64_t generation) noexcept {
    std::uint64_t seen = gate.paused_at.load(std::memory_order_acquire);
    while (seen < generation) {
        gate.paused_at.wait(seen, std::memory_order_acquire);
        seen = gate.paused_at.load(std::memory_order_acquire);
    }
}

// Resume generation `generation` (idempotent, monotonic). This is the ONLY
// way to release a generation-armed pause; resume_threadpool_gate's boolean
// store would not release the worker's resumed_at wait.
//
// Pre-release constraint (review follow-up): do NOT publish a resume for a
// generation that will be armed LATER on this gate. A pre-satisfied
// `resumed_at >= N` makes the worker's gen-N pause a NON-HOLD — it still
// publishes `paused_at >= N` but proceeds without blocking, so the gate no
// longer forces the pre-dequeue window and the race loses its determinism.
// Releasing an already-past generation (an idempotent high-water resume
// above the highest ACKed generation) is safe ONLY as terminal cleanup,
// where no further generation is ever armed.
inline void resume_dequeue_gate_generation(
    ThreadPoolBackend::BeforeWorkerDequeuePauseGate& gate,
    std::uint64_t generation) noexcept {
    dequeue_gate_detail::publish_max_(gate.resumed_at, generation);
    gate.resumed_at.notify_all();
}

// Block (zero-CPU) until the worker has published the dequeue-boundary ACK
// for `generation` or later: the pausing cycle's pop_front decision is
// complete, so that worker continuation can no longer consume a dispatch
// entry without re-entering the gate. ONLY AFTER this returns may the test
// arm generation N+1 (the #110 invariant).
inline void wait_dequeue_gate_ack(ThreadPoolBackend::BeforeWorkerDequeuePauseGate& gate,
                                  std::uint64_t generation) noexcept {
    std::uint64_t seen = gate.acked_at.load(std::memory_order_acquire);
    while (seen < generation) {
        gate.acked_at.wait(seen, std::memory_order_acquire);
        seen = gate.acked_at.load(std::memory_order_acquire);
    }
}

}  // namespace sluice::async

#endif  // defined(SLUICE_ASYNC_INTERNAL_TESTING)
