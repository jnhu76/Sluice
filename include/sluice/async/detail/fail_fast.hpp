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

// E13 P5 stage-boundary fail-fast (task §14). After the registration
// transaction completes and a final readiness snapshot is taken, if NO arm is
// ready, P5 has no inline winner. Suspended completion (caller suspension +
// later publication) is P6, DENIED at the P5 boundary. The inline-only
// admission MUST NOT: return a no-winner result, unwind while Event arms
// remain linked / Timer regs remain ACTIVE, fake-cancel every arm, perform a
// P7 rollback after FinishRegistration, or suspend the caller. It fails fast
// instead, in the SAME global_mtx_ critical section, without unwinding the
// caller frame. This is a temporary stage guard on a Draft PR, NOT final public
// API behavior.
//
// Same contract as the other fail-fast entries: [[noreturn]] noexcept, no
// allocation / locking / I/O / dynamic string, no state recovery, ultimately
// std::terminate(). The explanatory text ("P5 inline-only Select reached
// no-ready admission; suspended completion is P6") lives only in this comment,
// not on the hot path.
[[noreturn]] void select_admission_no_ready_fail_fast() noexcept;

}  // namespace sluice::async::detail
