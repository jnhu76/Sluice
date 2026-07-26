// E15-P2-02 regression — Group::async_threaded must be exception-safe when
// bookkeeping allocation fails.
//
// F-01 closeout: the implementation now performs ALL fallible capacity
// preparation (vector reserve) BEFORE creating the worker thread. If reserve
// throws, no thread exists, the task body has NOT executed, and the Group's
// vectors are unchanged. This gives correct admission semantics:
// async() failure means the task was NOT admitted.
//
// Test seam: Group exposes test_set_tasks_throw_on_nth(N) under
// SLUICE_ASYNC_INTERNAL_TESTING that throws std::bad_alloc from the Nth
// admission's reserve step (1-indexed by the resulting tasks_.size()).
// This file links sluice_async_internal_testing; production builds compile
// the seam out.
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

// ---- Slice 1: throw on the FIRST admission — task must NOT execute ---------
// The seam fires BEFORE thread creation (at the reserve step). Post-fix:
// no thread is spawned, ran stays 0, Group is safely destructible.

SLUICE_TEST_CASE(group_async_threaded_throw_on_first_admission_safe) {
    std::atomic<int> ran{0};
    bool caught = false;
    {
        Group g;
        g.test_set_tasks_throw_on_nth(1);  // first admission's reserve throws
        try {
            g.async([&](CancelToken&) { ++ran; });
        } catch (const std::bad_alloc&) {
            caught = true;
        }
        SLUICE_CHECK(caught);
        // F-01: reserve fires BEFORE thread creation. The task body did NOT
        // execute. This is the correct admission contract: async() failure
        // means the task was not admitted.
        SLUICE_CHECK(ran.load() == 0);
        // The Group has NO tracked task. size() == 0.
        SLUICE_CHECK(g.size() == 0);
        // Dtor runs here — must not std::terminate.
    }
    // Re-assert: task never ran.
    SLUICE_CHECK(ran.load() == 0);
}

// ---- Slice 2: throw on the SECOND admission — first task stays tracked ------
// The first async() succeeds (tracked in tasks_/futures_); the second throws
// on its reserve. The first task's worker is unaffected; the second's task
// body never executes. Group state is consistent: size() == 1.

SLUICE_TEST_CASE(group_async_threaded_throw_on_second_admission_keeps_first) {
    std::atomic<int> ran{0};
    bool caught = false;
    {
        Group g;
        // First task admitted normally.
        g.async([&](CancelToken&) { ++ran; });
        SLUICE_CHECK(g.size() == 1);
        // Second task's reserve (resulting size 2) throws.
        g.test_set_tasks_throw_on_nth(2);
        try {
            g.async([&](CancelToken&) { ++ran; });
        } catch (const std::bad_alloc&) {
            caught = true;
        }
        SLUICE_CHECK(caught);
        SLUICE_CHECK(g.size() == 1);  // first task still tracked; second not admitted
        // F-01: the second task body did NOT execute.
        // Drain the first task cleanly.
        g.await();
    }
    // Only the first worker ran.
    SLUICE_CHECK(ran.load() == 1);
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
        SLUICE_CHECK(ran.load() == 0);  // F-01: task did NOT execute

        // Clear the seam and admit normally.
        g.test_set_tasks_throw_on_nth(0);
        g.async([&](CancelToken&) { ++ran; });
        SLUICE_CHECK(g.size() == 1);
        g.await();
    }
    SLUICE_CHECK(ran.load() == 1);  // only the normal worker ran
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
