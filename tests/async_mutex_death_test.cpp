// async_mutex_death_test
//
// Verifies the ASYNC-MUTEX-NOTHROW-PRODUCTION-IMPLEMENTATION-1 §F death-test
// obligations: an underlying Mutex acquisition failure must terminate the
// process via the production fail-fast boundary, never return into the caller.
//
// This binary has its OWN int main(int, char**) (NOT SLUICE_MAIN): the existing
// tests/harness.hpp framework is cooperative and cannot survive a
// std::terminate in-process, so each case runs in a forked child that re-execs
// this binary. The child installs a deterministic terminate handler and the
// parent asserts the exact exit code (see death_test_runner_posix.hpp).
//
// The unit under test is the REAL sluice::async::Mutex production entry,
// compiled against sluice_async_internal_testing (which defines
// SLUICE_ASYNC_INTERNAL_TESTING so the injection seam is linkable). The seam
// throws a dedicated test exception from the SAME try block whose catch(...)
// calls the production fail-fast helper; the throw therefore exercises the
// real catch/fail-fast code path, not a fake class and not a direct
// std::terminate() call (§N forbids the latter as a coverage substitute).
//
// Cases (§F2):
//   T1  Mutex::lock()      — armed lock failure must terminate.
//   T2  Mutex::try_lock()  — armed try_lock failure must terminate (not false).
//   T3  std::condition_variable_any reacquire — unique_lock ctor takes the
//        1st lock; cv.wait_for internally unlock+reacquire, the 2nd lock throws
//        inside the cv machinery, the Mutex fail-fast boundary terminates.
//        Deterministic: no notifier thread, no lost-wake race.
//   T4  control — same entries, NO arm; lock/try_lock/cv-wait succeed; exit 0.
#include "death_test_runner_posix.hpp"

#if defined(__unix__)
#include "async_test_control.hpp"

#include <sluice/async/mutex.hpp>

#include <chrono>
#include <condition_variable>
#include <iostream>
#include <mutex>
#include <string>

namespace {

using sluice::async::Mutex;
using MutexFailSeam = sluice_async_test::MutexFailSeam;
using namespace std::chrono_literals;

// --------------------------------------------------------------------------
// Child-mode bodies. Each runs in a forked child that has already installed
// the deterministic terminate handler. On the intended fail-fast path the
// body never returns (the Mutex catch boundary calls std::terminate, whose
// handler calls std::_Exit(86)). If control flow somehow returns past the
// call, the body exits kUnexpectedReturnExit to make the regression obvious.
// --------------------------------------------------------------------------

// T1 — lock failure. Direct local Mutex; armed next-lock failure.
void child_t1_lock_failure() {
    Mutex m;
    MutexFailSeam::disarm();
    MutexFailSeam::arm_next_lock_fail();
    m.lock();  // MUST terminate here; never returns.
    std::_Exit(sluice_death_test::kUnexpectedReturnExit);
}

// T2 — try_lock failure. Direct local Mutex; armed next-try_lock failure.
// Must terminate, NOT return false.
void child_t2_try_lock_failure() {
    Mutex m;
    MutexFailSeam::disarm();
    MutexFailSeam::arm_next_try_lock_fail();
    (void)m.try_lock();  // MUST terminate here; must not return false.
    std::_Exit(sluice_death_test::kUnexpectedReturnExit);
}

// T3 — condition_variable_any reacquire failure (deterministic).
//
//   unique_lock ctor                -> 1st Mutex::lock succeeds (countdown 2->1)
//   cv.wait_for(lk, timeout)        -> internally unlock, wait, timeout, then
//                                      REACQUIRE via 2nd Mutex::lock
//                                    -> 2nd lock throws inside the cv machinery
//                                    -> Mutex catch(...) -> fail-fast
//
// No notifier thread: the timeout alone drives the reacquire, so there is no
// lost-wake window and no cross-thread scheduling nondeterminism. We do NOT
// simplify this to a manual second m.lock() call (§F2 forbids that).
void child_t3_cv_reacquire_failure() {
    Mutex m;
    std::condition_variable_any cv;
    MutexFailSeam::disarm();
    MutexFailSeam::arm_lock_countdown(2);  // 1st lock ok, 2nd lock (reacquire) throws
    std::unique_lock<Mutex> lk(m);         // 1st lock (countdown 2 -> 1)
    // wait_for: unlocks m, waits, times out, then reacquires m via Mutex::lock
    // (2nd lock, countdown 1 -> 0, throws). The throw is caught by the
    // production Mutex catch(...) boundary and converted to fail-fast.
    cv.wait_for(lk, 5ms);  // MUST terminate during reacquire; never returns.
    std::_Exit(sluice_death_test::kUnexpectedReturnExit);
}

// T4 — control path. Same entries, NO fault armed. Everything must succeed
// and the child must exit 0. Proves the seam is inert when disarmed and the
// Mutex still works as a normal lock/cv backend.
void child_t4_control() {
    MutexFailSeam::disarm();  // belt-and-suspenders: start from a clean state
    {
        Mutex m;
        m.lock();
        m.unlock();
        if (m.try_lock()) m.unlock();
    }
    {
        Mutex m;
        std::condition_variable_any cv;
        std::unique_lock<Mutex> lk(m);
        // wait_for times out quickly and reacquires normally (no arm).
        cv.wait_for(lk, 1ms);
    }
    std::_Exit(0);
}

// dispatch_child never returns: every known case calls _Exit (either via the
// terminate handler on the fail-fast path, or _Exit(0) for the control case,
// or _Exit(kUnexpectedReturnExit) if a must-terminate case wrongly returns).
void dispatch_child(const std::string& name) {
    sluice_death_test::install_deterministic_terminate_handler();
    if      (name == "T1") child_t1_lock_failure();
    else if (name == "T2") child_t2_try_lock_failure();
    else if (name == "T3") child_t3_cv_reacquire_failure();
    else if (name == "T4") child_t4_control();
    std::cerr << "[death] unknown child case: " << name << "\n";
    std::_Exit(sluice_death_test::kChildTestFailExit);
}

// --------------------------------------------------------------------------
// Parent mode: run each case in a forked child and assert the exit protocol.
// --------------------------------------------------------------------------

int run_parent() {
    int failures = 0;

    const auto must_term = [&](const char* name) {
        auto r = sluice_death_test::run_death_case(name);
        if (!sluice_death_test::expect_terminated_via_fail_fast(r)) ++failures;
    };
    const auto must_zero = [&](const char* name) {
        auto r = sluice_death_test::run_death_case(name);
        if (!sluice_death_test::expect_normal_exit_zero(r)) ++failures;
    };

    must_term("T1");  // lock failure terminates
    must_term("T2");  // try_lock failure terminates (not false)
    must_term("T3");  // condition_variable_any reacquire failure terminates
    must_zero("T4");  // control: no arm, all succeed, exit 0

    if (failures == 0) {
        std::cout << "ALL DEATH TESTS PASSED (T1 lock / T2 try_lock / "
                     "T3 cv-reacquire / T4 control)\n";
        return 0;
    }
    std::cout << failures << " death-test case(s) FAILED\n";
    return 1;
}

}  // namespace

int main(int argc, char** argv) {
    std::string child_case = sluice_death_test::parse_child_case(argc, argv);
    if (!child_case.empty()) {
        dispatch_child(child_case);  // never returns
        return sluice_death_test::kChildTestFailExit;  // unreachable
    }
    return run_parent();
}

#else  // !defined(__unix__)

#include <iostream>

int main() {
    std::cout << "async_mutex_death_test: NOT RUN on this platform "
                 "(POSIX fork/exec harness only; see death_test_runner_posix.hpp)\n";
    return 0;
}

#endif  // defined(__unix__)
