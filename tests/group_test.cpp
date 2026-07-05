// Tests for sluice::async::Group (sluice-CORE-029, T3).
//
// Unordered task set derived from Zig std.Io Group (Io.zig:1218-1303).
// await/cancel are whole-group; group is a cancel-propagation boundary (tasks
// swallow cancel). Each case asserts ONE Group semantic, TDD-vertical.
#include "harness.hpp"

#include <sluice/async/cancel.hpp>
#include <sluice/async/future.hpp>
#include <sluice/async/group.hpp>
#include <sluice/error.hpp>
#include <sluice/result.hpp>

#include <atomic>
#include <chrono>
#include <thread>

using namespace sluice::async;
using sluice::IoError;
using sluice::Result;

// ---- Slice 1 (tracer): two tasks, await waits for both --------------------
// The core shape: async() spawns tasks; await() blocks until ALL complete.
SLUICE_TEST_CASE(group_await_waits_for_all_tasks) {
    Group g;
    std::atomic<int> counter{0};
    g.async([&](CancelToken&) { std::this_thread::sleep_for(std::chrono::milliseconds(10)); ++counter; });
    g.async([&](CancelToken&) { ++counter; });
    SLUICE_CHECK(g.size() == 2);
    g.await();
    SLUICE_CHECK(counter.load() == 2);
    SLUICE_CHECK(g.size() == 0);  // await reaped
}

// ---- Slice 2: cancel-propagation boundary — tasks observe the token -------
// Group is a cancel-propagation boundary (Zig Io.zig:1240). cancel() requests
// the shared token; tasks observing it return promptly; the group's await
// completes (no exception escapes — cancel is swallowed inside tasks).
SLUICE_TEST_CASE(group_cancel_propagates_to_tasks) {
    Group g;
    std::atomic<int> observed{0};
    g.async([&](CancelToken& tok) {
        // Pre-work cancel point: observe the token before doing real work.
        for (int i = 0; i < 1000 && !tok.is_requested(); ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        if (tok.is_requested()) { ++observed; return; }
        ++observed;  // would do real work
    });
    g.async([&](CancelToken& tok) {
        for (int i = 0; i < 1000 && !tok.is_requested(); ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        if (tok.is_requested()) { ++observed; return; }
        ++observed;
    });
    // Let both reach their cancel points, then cancel.
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    g.cancel();  // requests + awaits
    SLUICE_CHECK(observed.load() == 2);  // both tasks ran to their terminal
}

// ---- Slice 3: destructor drains if await was never called -----------------
// Resource guarantee (Zig Io.zig:1211): per-task resources are freed when each
// task returns. If the caller never awaits, the group destructor joins the
// workers (no detached threads, CP.26). The group going out of scope must not
// leak threads or hang.
SLUICE_TEST_CASE(group_destructor_drains_unjoined_tasks) {
    std::atomic<int> ran{0};
    {
        Group g;
        g.async([&](CancelToken&) { ++ran; });
        g.async([&](CancelToken&) { std::this_thread::sleep_for(std::chrono::milliseconds(5)); ++ran; });
        // No await — let the destructor drain.
    }  // ~Group joins
    SLUICE_CHECK(ran.load() == 2);
}

// ---- Slice 4: idempotent await/cancel ------------------------------------
// await()/cancel() may be called more than once; the second call is a no-op
// (tasks already reaped). Mirrors Zig Io.zig:1282 / Io.zig:1298 idempotency.
SLUICE_TEST_CASE(group_await_and_cancel_are_idempotent) {
    Group g;
    std::atomic<int> n{0};
    g.async([&](CancelToken&) { ++n; });
    g.await();
    g.await();    // idempotent — no tasks, no hang
    g.cancel();   // idempotent — no tasks
    SLUICE_CHECK(n.load() == 1);
    SLUICE_CHECK(g.size() == 0);
}

SLUICE_MAIN()
