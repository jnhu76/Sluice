// E15-P2-02 regression — Group::async_threaded must be exception-safe when
// bookkeeping allocation fails after a worker thread has already been spawned.
//
// Pre-fix: std::thread `w` was constructed BEFORE tasks_/futures_ push_back.
// If push_back threw (vector reallocation / std::bad_alloc), the local `w`
// destructed while JOINABLE — std::thread::~thread on a joinable thread calls
// std::terminate, killing the process while the worker was still running fn
// against the group's CancelToken. The local thread never reached tasks_.
//
// Post-fix: the bookkeeping mutation is wrapped in a try/catch that JOINs the
// already-spawned worker on failure (the body swallows fn exceptions, so join
// returns cleanly), then rethrows. No joinable thread reaches a destructor;
// Group state stays consistent; the caller observes the allocation failure.
//
// Test seam: Group exposes test_set_tasks_throw_on_nth(N) under
// SLUICE_ASYNC_INTERNAL_TESTING that throws std::bad_alloc from the Nth
// tasks_ push_back (1-indexed by the resulting tasks_.size()). This file
// links sluice_async_internal_testing; production builds compile the seam out.
#include "harness.hpp"

#include <sluice/async/cancel.hpp>
#include <sluice/async/future.hpp>
#include <sluice/async/group.hpp>
#include <sluice/result.hpp>

#include <atomic>
#include <new>       // std::bad_alloc
#include <stdexcept>
#include <thread>

using namespace sluice::async;

// ---- Slice 1: throw on the FIRST admission — worker must be joined, no leak -
// The fn increments a counter so we can observe whether the worker actually
// ran. Post-fix: the worker is joined before the bad_alloc propagates, so the
// counter reaches its expected value and the Group is safely destructible.

SLUICE_TEST_CASE(group_async_threaded_throw_on_first_admission_safe) {
    std::atomic<int> ran{0};
    bool caught = false;
    {
        Group g;
        g.test_set_tasks_throw_on_nth(1);  // first tasks_ push throws
        try {
            g.async([&](CancelToken&) { ++ran; });
        } catch (const std::bad_alloc&) {
            caught = true;
        }
        SLUICE_CHECK(caught);
        // The worker was spawned BEFORE the throw; the fix JOINs it before
        // rethrowing, so by the time we get here `ran` is observable. Spin
        // briefly is NOT needed: join() is a happens-before synchronization.
        // (The worker body swallowed fn exceptions; ++ran completed normally.)
        SLUICE_CHECK(ran.load() == 1);
        // The Group has NO tracked task (the push failed). size() == 0.
        SLUICE_CHECK(g.size() == 0);
        // Dtor runs here — must not std::terminate (no joinable local thread,
        // no untracked running thread).
    }
    // If we reach here, the dtor was safe. Re-assert the counter one more time.
    SLUICE_CHECK(ran.load() == 1);
}

// ---- Slice 2: throw on the SECOND admission — first task stays tracked ------
// The first async() succeeds (tracked in tasks_/futures_); the second throws
// on its tasks_ push. The first task's worker is unaffected; the second's
// worker is joined. Group state is consistent: size() == 1.

SLUICE_TEST_CASE(group_async_threaded_throw_on_second_admission_keeps_first) {
    std::atomic<int> ran{0};
    bool caught = false;
    {
        Group g;
        // First task admitted normally.
        g.async([&](CancelToken&) { ++ran; });
        SLUICE_CHECK(g.size() == 1);
        // Second task's tasks_ push (resulting size 2) throws.
        g.test_set_tasks_throw_on_nth(2);
        try {
            g.async([&](CancelToken&) { ++ran; });
        } catch (const std::bad_alloc&) {
            caught = true;
        }
        SLUICE_CHECK(caught);
        SLUICE_CHECK(g.size() == 1);  // first task still tracked; second rolled back
        // Drain the first task cleanly.
        g.await();
    }
    // Both workers ran (the second's body executed before its thread was joined).
    SLUICE_CHECK(ran.load() == 2);
}

// ---- Slice 3: Group remains reusable after an admission failure -------------
// A failed admission does not poison the Group; a later async() succeeds.

SLUICE_TEST_CASE(group_async_threaded_reusable_after_admission_failure) {
    std::atomic<int> ran{0};
    {
        Group g;
        g.test_set_tasks_throw_on_nth(1);
        bool caught = false;
        try {
            g.async([&](CancelToken&) { ++ran; });
        } catch (const std::bad_alloc&) {
            caught = true;
        }
        SLUICE_CHECK(caught);
        SLUICE_CHECK(g.size() == 0);

        // Clear the seam and admit normally.
        g.test_set_tasks_throw_on_nth(0);
        g.async([&](CancelToken&) { ++ran; });
        SLUICE_CHECK(g.size() == 1);
        g.await();
    }
    SLUICE_CHECK(ran.load() == 2);  // failed-admission worker + normal worker
}

// ---- Slice 4: a throwing fn body does NOT terminate the worker --------------
// Sanity: the worker swallows fn exceptions (cancel-propagation boundary). This
// is load-bearing for the fix — join() in the catch path relies on the worker
// body NOT re-propagating. If fn could throw out of the worker, join() would
// still complete (the worker caught it), but the contract is worth pinning.

SLUICE_TEST_CASE(group_async_threaded_throwing_fn_body_does_not_terminate) {
    std::atomic<int> ran{0};
    {
        Group g;
        g.async([&](CancelToken&) {
            ++ran;
            throw std::runtime_error("intentional");
        });
        g.await();  // returns cleanly: the body swallowed the exception
    }
    SLUICE_CHECK(ran.load() == 1);
}

SLUICE_MAIN()
