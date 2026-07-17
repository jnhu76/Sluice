// sluice::async::detail — internal-testing failure-injection seam for
// sluice::async::Mutex (ASYNC-MUTEX-NOTHROW-AUTHORITY-1 §7 obligation 4;
// ASYNC-MUTEX-NOTHROW-PRODUCTION-IMPLEMENTATION-1 §E).
//
// LAYERING (this is load-bearing, see plan review P0-1):
//
//   production Mutex entry  (include/sluice/async/mutex.hpp)
//       |
//       v   detail::maybe_inject_mutex_failure(...)   [under the macro]
//   internal-testing fault state (src/async/mutex_test_seam.cpp)
//       ^
//       |   MutexFailSeam / detail::test_hooks::*     [test authority]
//   tests/async_test_control.hpp
//
// `mutex.hpp` depends ONLY on `sluice::async::detail`, never on the
// `sluice_async_test` namespace. The fault state lives inside the
// `sluice_async_internal_testing` library (compiled from src/async/*.cpp with
// SLUICE_ASYNC_INTERNAL_TESTING defined); the production `sluice_async` target
// compiles mutex_test_seam.cpp to an EMPTY translation unit, so the production
// archive carries NO injection symbol and the production preprocessed `mutex.hpp`
// contains NO injection call (verified by nm + clang -E in the evidence report).
//
// The fault state is process-global and arms only an injected fault counter
// (fail next lock / fail next try_lock / fail on Nth lock). It performs NO
// production synchronization (memory_order_relaxed): it counts test faults for
// a single death-test child only.
//
// This header is installed (it sits under include/) because mutex.hpp must see
// the declaration when compiled under the internal-testing macro. Production
// downstream never defines the macro, so the declarations below are absent in
// production TUs — no public API surface is added.
#pragma once

namespace sluice::async::detail {

// Operation tag for the injection seam. Used ONLY to select which acquisition
// entry the armed fault applies to. Never reaches the production fail-fast
// helper (async_mutex_lock_fail_fast takes no parameter).
enum class MutexTestOperation : unsigned char {
    lock,
    try_lock,
};

#if defined(SLUICE_ASYNC_INTERNAL_TESTING)

// MAY throw a dedicated test exception (InjectedMutexFailure) when an armed
// fault matches `op`. Declared noexcept(false) so the throw is well-formed.
// Defined in src/async/mutex_test_seam.cpp; that TU compiles to empty under
// the production target.
void maybe_inject_mutex_failure(MutexTestOperation op) noexcept(false);

// Test-authority hooks used by tests/async_test_control.hpp's MutexFailSeam.
// They live in sluice::async::detail (not sluice_async_test) because they
// mutate the library-internal fault state; MutexFailSeam is only a thin
// test-facing facade over them.
namespace test_hooks {

// Arm the Nth-lock countdown. Semantics:
//   n == 0 : disabled (no lock will throw).
//   n == 1 : the NEXT Mutex::lock throws (and the countdown becomes 0).
//   n  > 1 : the next (n-1) locks succeed (countdown decrements); the Nth
//            throws. Used by T3 (condition_variable_any reacquire): the
//            unique_lock ctor takes the 1st lock (n:2->1), cv.wait_for
//            internally unlock+reacquire takes the 2nd lock (n:1->0, throws).
void arm_lock_countdown(unsigned n) noexcept;

// Arm a one-shot failure for the next Mutex::try_lock only.
void arm_next_try_lock_fail() noexcept;

// Clear all armed fault state. Every death-test child should start from a
// disarmed state; the control (T4) case must disarm before exercising the
// entries.
void disarm() noexcept;

}  // namespace test_hooks

#endif  // defined(SLUICE_ASYNC_INTERNAL_TESTING)

}  // namespace sluice::async::detail
