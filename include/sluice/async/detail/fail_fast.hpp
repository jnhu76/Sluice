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

}  // namespace sluice::async::detail
