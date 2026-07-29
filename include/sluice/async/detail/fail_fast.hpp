// sluice::async::detail::async_mutex_lock_fail_fast
//
// Named fail-fast entry for the Mutex acquisition boundary
// (ASYNC-MUTEX-NOTHROW-AUTHORITY-1 §2/§3).
//
// An internal Mutex acquisition that participates in an authoritative
// Scheduler transition (or, by the Queue design, a winner CommitGap) cannot
// resume user execution after an underlying lock failure while preserving
// winner / ownership / queue-membership / publication invariants. Such a
// failure is therefore process-fatal.
//
// Contract (authority §D2):
//   * [[noreturn]] noexcept;
//   * no allocation, no locking, no I/O;
//   * no virtual dispatch, no function-pointer call, no dynamic string;
//   * does not attempt to recover Scheduler state;
//   * ultimately calls std::terminate() (or an equivalent process terminator).
//
// The winner path must not format or emit complex log output.
//
// This function takes no operation parameter: the operation is known only to
// the (internal-testing-only) failure-injection seam, never to the production
// fail-fast path. Adding a parameter here would invite future formatting /
// logging / allocation on the winner path and is deliberately rejected.
#pragma once

namespace sluice::async::detail {

// Terminates the process. Called only from the Mutex::lock()/try_lock()
// catch (...) boundaries; never returns.
[[noreturn]] void async_mutex_lock_fail_fast() noexcept;

// E13 P3 stage-boundary fail-fast (docs/e13-select-timer-adapter.md §5,
// Mandatory Addendum D). A due ACTIVE SelectTimerRegistration is UNREACHABLE
// in valid P3 production state: there is no admission path, so no ACTIVE
// Select heap entry should ever be observed by the pump. If the pump pops an
// ACTIVE Select entry, that is an invariant violation (either a stale entry
// was observed before a CAS completed, the registration protocol has a bug,
// or a test advanced the clock past an ACTIVE synthetic entry). The pump
// MUST NOT claim a winner, mark CandidateReady, retire/consume, erase, or
// busy-loop; it fails fast instead. This is NOT supported production Select
// behavior — P4 (claim/finalize) is denied pending independent P3 review.
//
// Same contract as async_mutex_lock_fail_fast: [[noreturn]] noexcept, no
// allocation / locking / I/O / dynamic string, no state recovery, ultimately
// std::terminate(). Takes no parameter (the operation is known only to the
// caller; adding one would invite logging on the pump hot path).
[[noreturn]] void select_timer_pump_active_fail_fast() noexcept;

// E13 P6 stage-boundary fail-fast: multi-group shared Event (P8, DENIED).
// docs/e13-select-locking-and-publication.md §6 / production-test-plan.md §7.8.
// One Event::set() broadcast may observe arms belonging to MORE THAN ONE
// distinct eligible SelectGroup (phase==Armed). P8 (multi-group Event
// intrusive worklist + per-group iteration) is not implemented at the P6
// boundary. P6 must therefore detect >1 distinct eligible group BEFORE any
// group winner CAS / candidate mutation / authority close and fail fast,
// rather than silently resolving only one group (a lost resolution) or
// attempting an unsupported multi-group publish. The same Event appearing
// twice in ONE group is NOT this case and is supported (P6-D1).
//
// Same contract as the other fail-fast entries: [[noreturn]] noexcept, no
// allocation / locking / I/O / dynamic string, no state recovery, ultimately
// std::terminate(). No parameter (the operation is known only to the caller).
[[noreturn]] void select_multi_group_event_stage_fail_fast() noexcept;

// E13 P5 CORRECTIVE: general-purpose Select invariant fail-fast. Called when
// the admission core receives a structurally invalid descriptor/count argument
// (descs==nullptr, count==0, count>kSelectMaxArms) or encounters an unknown
// descriptor kind in Release builds. Provides defense-in-depth against
// Release-mode memory safety violations even when a non-friend caller bypasses
// the public select() template's compile-time requires clause gate.
//
// Same contract as the other fail-fast entries.
[[noreturn]] void select_invariant_fail_fast() noexcept;

// E14 D-E14-F2a: Group lifetime fail-fast. Called from ~Group when an Evented
// task Future is still pending at destruction time. This is a caller-contract
// violation (the caller must await or cancel before destroying an Evented
// Group). The destructor MUST NOT call Evented Future::await from a non-Fiber
// context (g_worker is null on an ordinary caller thread, causing a null
// dereference in Scheduler::await_ready_flag). Failing fast surfaces the
// violation deterministically instead of allowing UB.
//
// Same contract as the other fail-fast entries: [[noreturn]] noexcept, no
// allocation / locking / I/O / dynamic string, no state recovery, ultimately
// std::terminate().
[[noreturn]] void group_lifetime_fail_fast() noexcept;

// E14 D-E14-2: Evented admission fail-fast. Called when an Evented public
// admission boundary is reached on a target where fiber_ctx::supported is
// false. Deterministically testable via the bool parameter (production passes
// fiber_ctx::supported; death tests pass false).
//
// Same contract as the other fail-fast entries.
[[noreturn]] void evented_admission_fail_fast() noexcept;

// E15-P1-03 / E15-P2-06: AsyncIoContext outstanding-Completion fail-fast.
// Called from AsyncIoContext::~AsyncIoContext() and operator=(AsyncIoContext&&)
// when the context still owns a backend with >0 outstanding Completions. Per
// ADR §5 L11 this is a caller-contract violation: outstanding Completions are
// address-stable and CALLER-OWNED; silently discarding the backend that
// publishes them would strand them permanently outstanding (no Result channel,
// no path to ready). A destructor / move-assignment has no Result channel to
// surface invalid_state, so the truthful contract is deterministic fail-fast
// in BOTH Debug and Release (no silent abandonment, no claimed-but-unreturnable
// invalid_state). Mirrors group_lifetime_fail_fast.
//
// Same contract as the other fail-fast entries: [[noreturn]] noexcept, no
// allocation / locking / I/O / dynamic string, no state recovery, ultimately
// std::terminate(). No parameter (the operation is known only to the caller).
[[noreturn]] void async_context_outstanding_fail_fast() noexcept;

// E14 D-E14-2: internal testable guard. Production code calls
// require_evented_supported(fiber_ctx::supported). On supported targets this
// is an optimized no-op (the parameter is a compile-time true constant). On
// unsupported targets or when called with false (death test), it calls
// evented_admission_fail_fast().
inline void require_evented_supported(bool supported) noexcept {
    if (!supported) {
        evented_admission_fail_fast();
    }
}

// E14 D-E14-2: Evented admission check. Returns the effective fiber support
// status. Production: returns fiber_ctx::supported (compile-time constant on
// the target). Internal-testing: may be overridden via
// AsyncTestAccess::set_evented_admission_override to simulate unsupported
// targets on x86_64. Defined out-of-line in fail_fast.cpp.
bool evented_admission_check() noexcept;

// I47-F3: invalid runnable-ticket consumption fail-fast. Called from
// run_next_on when make_running() fails (the Fiber is NOT in Runnable state).
// A worker that consumes a ticket whose Fiber is not Runnable would enter an
// invalid context (rsp/rbp/rip not saved). This is process-fatal: the
// invariant violation means the suspend-switch authority protocol was breached.
//
// Same contract as the other fail-fast entries: [[noreturn]] noexcept, no
// allocation / locking / I/O / dynamic string, no state recovery, ultimately
// std::terminate(). No parameter (the operation is known only to the caller).
[[noreturn]] void scheduler_invalid_runnable_ticket_fail_fast() noexcept;

// I47-F2: invalid suspend transition fail-fast. Called from
// commit_suspend_locked when make_waiting() fails (the Fiber is NOT Running).
// A Fiber that cannot transition Running->Waiting is in an impossible protocol
// state: the caller believed it was the current Running Fiber, but the state
// machine disagrees. Process-fatal.
//
// Same contract as the other fail-fast entries.
[[noreturn]] void scheduler_invalid_suspend_transition_fail_fast() noexcept;

}  // namespace sluice::async::detail
