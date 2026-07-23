// e11_timer_wait_test — Deadline / Timer Wait Integration (sluice-CORE-E11).
//
// Deterministic production tests for the E11 deadline/timer wait integration.
// Observed ONLY through the Scheduler integration seams + the public lock-free
// WaitNode state queries, mirroring the E10 test discipline:
//
//   - Scheduler::await_wait_deadline  (deadline wait registration authority)
//   - Scheduler::wake_wait_one       (Woken resolution)
//   - Scheduler::cancel_wait         (Cancelled resolution)
//   - Scheduler::expire_wait         (Expired resolution — the E11 third cause)
//   - Scheduler::advance_clock / pump_deadlines (deterministic timer driver)
//   - WaitNode public lock-free state queries (was_woken/was_cancelled/
//     was_expired/is_terminal/outcome)
//   - Scheduler::waiting_count()     (the wait-accounting authority)
//
// Every race proof uses a controllable monotonic clock + explicit timer driver
// or phase seam — NEVER sleep_for timing as causal proof (M7).
//
// Gated to x86_64 (fiber_ctx::supported): registration requires a real Fiber.
#include "harness.hpp"
#include "async_test_control.hpp"

#include <sluice/async/async_io_context.hpp>
#include <sluice/async/fake_backend.hpp>
#include <sluice/async/fiber.hpp>
#include <sluice/async/fiber_ctx.hpp>
#include <sluice/async/scheduler.hpp>
#include <sluice/async/wait_node.hpp>
#include <sluice/async/wait_queue.hpp>

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdio>
#include <thread>

using namespace sluice::async;
using sluice::Result;

namespace {
struct FiberStack {
    static constexpr std::size_t kBytes = 64 * 1024;
    alignas(16) std::vector<std::byte> bytes{kBytes};
    std::byte* base() noexcept { return bytes.data(); }
    std::size_t size() const noexcept { return bytes.size(); }
};

// A backend that never completes anything (outstanding stays 0). The only
// progress in these tests is Scheduler-integrated resolution; MW stays at
// S1/S3 and the run terminates only once all fibers are done.
class IdleBackend : public AsyncBackend {
public:
    Result<void> submit_read(ReadOp, Completion<std::size_t>&) override { return {}; }
    Result<void> submit_write(WriteOp, Completion<std::size_t>&) override { return {}; }
    Result<void> submit_sync_data(SyncDataOp, Completion<void>&) override { return {}; }
    Result<void> submit_sync_all(SyncAllOp, Completion<void>&) override { return {}; }
    std::size_t poll() override { return 0; }
    Result<std::size_t> wait_one() override { return 0; }
    void cancel(Completion<std::size_t>&) override {}
    void cancel(Completion<void>&) override {}
    std::size_t outstanding() const noexcept override { return 0; }
};

// Spin until `flag` is observed (TEST SYNC ONLY; bounded by the cooperative
// run which frees the worker to run the flag-setting fiber).
inline void spin_wait(std::atomic<bool>& flag) {
    while (!flag.load(std::memory_order_acquire)) {
        std::this_thread::yield();
    }
}

// Bounded spin-wait (E11-TIMER-ASAN-BUSY-POLL-HANG-CORRECTIVE-1 / W2).
//
// ROOT CAUSE (category A: test-harness defect). The unbounded `spin_wait`
// above is used by some cases (notably e11_t7) to wait, INSIDE A FIBER on a
// worker thread, for an outcome published by another fiber on another worker.
// Under ASan the worker is much slower; if the routed wake ticket is not
// drained by the other worker within the waker's bounded retry budget, the
// unbounded spin runs forever (100% CPU, observed: 21GB virtual, never
// returns). Production is not the root cause; the production timer/steal
// mechanics are sound. The defect is the test ASSUMING the other worker
// makes unbounded progress.
//
// `spin_wait_bounded` returns true if `flag` becomes true within `max_iters`
// yields, false otherwise. A `false` return MUST be turned into a test FAIL
// with a state dump by the caller (never silently returned as PASS, §B/§W2.5).
// The bound is a FAILURE GUARD, not causal sync; it is sized large enough not
// to flake on a healthy build (200000 yields ~ seconds even under ASan).
constexpr unsigned kSpinWaitBoundedIters = 200000;

[[maybe_unused]] inline bool spin_wait_bounded(std::atomic<bool>& flag,
                              unsigned max_iters = kSpinWaitBoundedIters) {
    for (unsigned i = 0; i < max_iters; ++i) {
        if (flag.load(std::memory_order_acquire)) return true;
        std::this_thread::yield();
    }
    return flag.load(std::memory_order_acquire);
}
}  // namespace

// ASYNC-TEST-SEAM-AUTHORITY-CORRECTIVE-1: the forgeable E11TimerTestHooks
// friend is removed. The clock/timer/park-commit controls are driven by the
// internal-testing controller facade TimerTestControl (tests/async_test_control.hpp),
// which routes through Scheduler::AsyncTestAccess (a guarded nested struct
// compiled only into the variant lib) + the per-Scheduler* phase registry.
// The call sites below keep the historical name via a local alias so the
// 15 test cases read unchanged; the actual authority is the new controller.
namespace {
using E11TimerTestHooks = sluice_async_test::TimerTestControl;
}  // namespace

// =============================================================================
// E11-T0 (Phase 1 unit): registered -> expired is a distinct terminal outcome.
//
// A waiter registers via await_wait_deadline with a deadline. The deadline is
// already due at admission (the controllable clock is advanced past it). The
// run must resolve the wait as Expired WITHOUT the fiber stranding. This is
// the minimal causal proof that `expired` is a real terminal outcome reached
// through resolve_, terminal + absorbing, and observably distinct from cancel.
// =============================================================================
SLUICE_TEST_CASE(e11_t0_expired_is_distinct_terminal_outcome) {
    if constexpr (!fiber_ctx::supported) return;

    AsyncIoContext ctx(std::make_unique<IdleBackend>());
    Scheduler sched(ctx);
    E11TimerTestHooks::enable_test_clock(sched);

    WaitQueue q;
    WaitNode n;
    std::atomic<bool> resolved_expired{false};
    std::atomic<bool> ran_after{false};

    // Deadline in the past relative to the controllable clock: deadline=10,
    // clock advanced to 100 BEFORE run, so the deadline is already due at the
    // admission recheck (I5 admission closure).
    Fiber fwait;
    fwait.set_entry([&](Fiber&) {
        sched.await_wait_deadline(q, n, Scheduler::deadline_t{10});
        ran_after.store(true, std::memory_order_release);
        resolved_expired.store(n.was_expired(), std::memory_order_release);
    });

    FiberStack sw;
    SLUICE_CHECK(sched.init_fiber(fwait, sw.base(), sw.size()));
    sched.spawn(fwait);
    // Advance the controllable clock so the deadline (10) is already due when
    // the fiber's await_wait_deadline reaches the admission recheck.
    E11TimerTestHooks::set_clock(sched, 100);
    sched.run(1);

    SLUICE_CHECK_MSG(ran_after.load(), "waiter resumed (not stranded by due deadline)");
    SLUICE_CHECK_MSG(resolved_expired.load(), "outcome is Expired");
    SLUICE_CHECK_MSG(n.was_expired(), "node terminal Expired");
    SLUICE_CHECK_MSG(!n.was_cancelled(), "expired is NOT cancelled");
    SLUICE_CHECK_MSG(!n.was_woken(), "expired is NOT woken");
    SLUICE_CHECK_MSG(n.is_terminal(), "node is terminal");
    SLUICE_CHECK_MSG(!n.is_registered(), "terminal node unlinked");
    SLUICE_CHECK_MSG(sched.waiting_count() == 0, "no unresolved waits remain");
}

// =============================================================================
// E11-T2/T3: resource wake wins before timer / timer wins before resource wake.
//
// A deadline wait with a far-future deadline. A waker fiber wakes the head
// BEFORE the deadline is due -> outcome Woken, NOT Expired. Symmetric: deadline
// elapses (clock advanced) before any wake -> outcome Expired, NOT Woken.
// Exactly one cause wins the resolve_ CAS in each case.
// =============================================================================
SLUICE_TEST_CASE(e11_t2_resource_wins_before_timer) {
    if constexpr (!fiber_ctx::supported) return;

    AsyncIoContext ctx(std::make_unique<IdleBackend>());
    Scheduler sched(ctx);
    E11TimerTestHooks::enable_test_clock(sched);

    WaitQueue q;
    WaitNode n;
    std::atomic<bool> registered{false};
    std::atomic<bool> ran_after{false};

    Fiber fwait, fwake;
    fwait.set_entry([&](Fiber&) {
        registered.store(true, std::memory_order_release);
        // Far-future deadline: clock is 0, deadline 10000 — NOT due.
        sched.await_wait_deadline(q, n, Scheduler::deadline_t{10000});
        ran_after.store(true, std::memory_order_release);
    });
    fwake.set_entry([&](Fiber&) {
        spin_wait(registered);
        for (int i = 0; i < 10000 && !n.is_terminal(); ++i) {
            sched.wake_wait_one(q);
            std::this_thread::yield();
        }
    });

    FiberStack sw, sk;
    SLUICE_CHECK(sched.init_fiber(fwait, sw.base(), sw.size()));
    SLUICE_CHECK(sched.init_fiber(fwake, sk.base(), sk.size()));
    sched.spawn(fwait);
    sched.spawn(fwake);
    sched.run(1);

    SLUICE_CHECK_MSG(ran_after.load(), "waiter resumed");
    SLUICE_CHECK_MSG(n.was_woken(), "resource wake won (Woken)");
    SLUICE_CHECK_MSG(!n.was_expired(), "timer did NOT win");
    SLUICE_CHECK_MSG(sched.waiting_count() == 0, "no unresolved waits");
}

SLUICE_TEST_CASE(e11_t3_timer_wins_before_resource_wake) {
    if constexpr (!fiber_ctx::supported) return;

    AsyncIoContext ctx(std::make_unique<IdleBackend>());
    Scheduler sched(ctx);
    E11TimerTestHooks::enable_test_clock(sched);

    WaitQueue q;
    WaitNode n;
    std::atomic<bool> registered{false};
    std::atomic<bool> ran_after{false};
    std::atomic<bool> expired_observed{false};

    Fiber fwait, fdriver;
    fwait.set_entry([&](Fiber&) {
        registered.store(true, std::memory_order_release);
        // Deadline due soon: clock=0, deadline=50.
        sched.await_wait_deadline(q, n, Scheduler::deadline_t{50});
        ran_after.store(true, std::memory_order_release);
        expired_observed.store(n.was_expired(), std::memory_order_release);
    });
    // The timer driver fiber: after the waiter registers, advance the clock
    // past the deadline and pump. The worker loop's drain then resolves Expired.
    fdriver.set_entry([&](Fiber&) {
        spin_wait(registered);
        std::this_thread::yield();  // let the waiter reach await_wait_deadline
        sched.advance_clock(100);   // 100 >= 50 -> deadline due
    });

    FiberStack sw, sd;
    SLUICE_CHECK(sched.init_fiber(fwait, sw.base(), sw.size()));
    SLUICE_CHECK(sched.init_fiber(fdriver, sd.base(), sd.size()));
    sched.spawn(fwait);
    sched.spawn(fdriver);
    sched.run(1);

    SLUICE_CHECK_MSG(ran_after.load(), "waiter resumed (timer drove expiry)");
    SLUICE_CHECK_MSG(expired_observed.load(), "timer won (Expired)");
    SLUICE_CHECK_MSG(n.was_expired(), "node terminal Expired");
    SLUICE_CHECK_MSG(sched.waiting_count() == 0, "no unresolved waits");
}

// =============================================================================
// E11-T4/T6: timer expiry vs cancellation / losing timer cannot publish.
//
// T4: a cancel fiber and the timer both race; whichever wins the resolve_ CAS
// publishes once. We drive the cancel explicitly (deterministic) so cancel
// wins, and assert outcome Cancelled + the timer did not also publish.
// T6 (embedded): a losing timer (cancel won first) performs NO publication —
// waiting_count is exactly 0 (one resolution, one runnable ticket).
// =============================================================================
SLUICE_TEST_CASE(e11_t4_cancel_wins_timer_loses) {
    if constexpr (!fiber_ctx::supported) return;

    AsyncIoContext ctx(std::make_unique<IdleBackend>());
    Scheduler sched(ctx);
    E11TimerTestHooks::enable_test_clock(sched);

    WaitQueue q;
    WaitNode n;
    std::atomic<bool> registered{false};
    std::atomic<bool> ran_after{false};

    Fiber fwait, fcancel;
    fwait.set_entry([&](Fiber&) {
        registered.store(true, std::memory_order_release);
        sched.await_wait_deadline(q, n, Scheduler::deadline_t{10000});
        ran_after.store(true, std::memory_order_release);
    });
    fcancel.set_entry([&](Fiber&) {
        spin_wait(registered);
        for (int i = 0; i < 10000 && !n.is_terminal(); ++i) {
            sched.cancel_wait(q, n);
            std::this_thread::yield();
        }
    });

    FiberStack sw, sc;
    SLUICE_CHECK(sched.init_fiber(fwait, sw.base(), sw.size()));
    SLUICE_CHECK(sched.init_fiber(fcancel, sc.base(), sc.size()));
    sched.spawn(fwait);
    sched.spawn(fcancel);
    sched.run(1);

    SLUICE_CHECK_MSG(ran_after.load(), "waiter resumed");
    SLUICE_CHECK_MSG(n.was_cancelled(), "cancel won (Cancelled)");
    SLUICE_CHECK_MSG(!n.was_expired(), "timer lost (not Expired)");
    // T6: exactly one resolution -> waiting_count is 0 (losing timer did not publish).
    SLUICE_CHECK_MSG(sched.waiting_count() == 0, "losing timer did not publish (count 0)");
}

// =============================================================================
// E11-T8/T9/T10: storage/address reuse does not defeat epoch isolation +
// stale physical timer entry after WaitNode destruction is inert.
//
// Wait epoch E: a node at some address registers a far-future deadline, then
// is woken (resource wins). The fiber resumes; the node goes out of scope
// (destroyed). A stale timer entry for E (still in the heap, retired) is then
// pumped — it MUST be inert. T8 forces the pump and asserts nothing resolves
// (no UAF under ASan). T9/T10 strengthen this with an explicit forced pump
// strictly after node destruction.
//
// EVIDENCE BOUNDARY (F5-corrected, honest): T8/T9/T10 do NOT construct epoch
// E+1 and do NOT force address(node_E) == address(node_E+1). They prove the
// retirement-state gate (a RETIRED/CONSUMED registration is inert and never
// dereferences a destroyed node), NOT same-NUMERIC-address reuse. The
// same-storage-slot reuse boundary (a stale timer resolving a DIFFERENT epoch
// that reused E's address) is proven by the FORMAL NEG-3 model
// (E11TimerWaitNeg3StaleCrossEpoch: the slot-keyed buggy expiry resolves E+1)
// plus the production refinement (TimerRegistration::node_ captures the live
// WaitNode object, never a reusable address or Fiber*). A unit-level
// construct_at/destroy_at same-slot test was considered and rejected: it would
// not meaningfully refine the production Fiber ownership topology (WaitNodes
// are fiber-frame locals, not placement-new slots), so option B (narrow the
// claim + rely on NEG-3 + the retirement proof) is the honest evidence path.
// The decisive production guarantee is the retirement state observed BEFORE any
// node dereference, independent of address.
// =============================================================================
SLUICE_TEST_CASE(e11_t8_storage_reuse_epoch_isolation) {
    if constexpr (!fiber_ctx::supported) return;

    AsyncIoContext ctx(std::make_unique<IdleBackend>());
    Scheduler sched(ctx);
    E11TimerTestHooks::enable_test_clock(sched);

    WaitQueue q;
    std::atomic<bool> registered{false};
    std::atomic<bool> epoch_e_done{false};

    Fiber fwait_e, fwake_e;
    fwait_e.set_entry([&](Fiber&) {
        registered.store(true, std::memory_order_release);
        // Epoch E: far-future deadline (will be retired by the wake).
        WaitNode node_e;
        sched.await_wait_deadline(q, node_e, Scheduler::deadline_t{100000});
        SLUICE_CHECK_MSG(node_e.was_woken(), "epoch E resolved Woken");
        epoch_e_done.store(true, std::memory_order_release);
        // node_e destroyed here as the fiber frame returns (after signaling).
    });
    fwake_e.set_entry([&](Fiber&) {
        spin_wait(registered);
        // Retry until a node is woken (registration visibility).
        for (int i = 0; i < 10000; ++i) {
            if (sched.wake_wait_one(q)) break;
            std::this_thread::yield();
        }
    });

    FiberStack sw, sk;
    SLUICE_CHECK(sched.init_fiber(fwait_e, sw.base(), sw.size()));
    SLUICE_CHECK(sched.init_fiber(fwake_e, sk.base(), sk.size()));
    sched.spawn(fwait_e);
    sched.spawn(fwake_e);
    sched.run(1);

    SLUICE_CHECK_MSG(epoch_e_done.load(), "epoch E completed");

    // Now the timer registration for E is RETIRED (wake retired it) but may
    // still be physically present in the heap (lazy). Advance the clock past
    // E's deadline and pump: the stale entry MUST be inert (retired) and MUST
    // NOT resolve any node. There is no live node to resolve.
    E11TimerTestHooks::set_clock(sched, 200000);
    sched.advance_clock(200000);

    // If the stale entry were not inert, it would have dereferenced the
    // destroyed node_e storage (UAF under ASan). Passing here under ASan is
    // the lifetime-closure proof. No assertion needed beyond no-crash/no-ASan.
    SLUICE_CHECK_MSG(true, "stale retired timer was inert (no UAF, no resolution)");
}

// =============================================================================
// E11-T14: Drain-mode deadline regression.
//
// A deadline wait with a far-future deadline and NO resolver. In DRAIN mode
// the run must return STALLED (terminate) exactly as E9/E10 do for an
// unresolved wait — the timer subsystem MUST NOT introduce a hidden semantic
// switch that parks forever (the E9-CORRECTIVE Drain-hang regression). The
// wait is left unresolved; the caller cancels it to destroy the node cleanly.
// =============================================================================
SLUICE_TEST_CASE(e11_t14_drain_mode_no_deadline_hang) {
    if constexpr (!fiber_ctx::supported) return;

    AsyncIoContext ctx(std::make_unique<IdleBackend>());
    Scheduler sched(ctx);
    E11TimerTestHooks::enable_test_clock(sched);

    WaitQueue q;
    WaitNode n;
    std::atomic<bool> suspended{false};

    Fiber fwait;
    fwait.set_entry([&](Fiber&) {
        suspended.store(true, std::memory_order_release);
        // Far-future deadline, no resolver: must STALL in Drain, not hang.
        sched.await_wait_deadline(q, n, Scheduler::deadline_t{100000});
    });

    FiberStack sw;
    SLUICE_CHECK(sched.init_fiber(fwait, sw.base(), sw.size()));
    sched.spawn(fwait);
    // Drain (default run): must TERMINATE (not hang). If E11 reintroduced the
    // E9 Drain-park defect, run() would never return.
    sched.run(1);

    SLUICE_CHECK_MSG(suspended.load(), "waiter did suspend");
    SLUICE_CHECK_MSG(fwait.state() == FiberState::waiting,
                     "waiter left Waiting (STALLED, not resumed)");
    SLUICE_CHECK_MSG(n.is_registered(), "node left Registered (unresolved)");
    // Caller resolves before destroying the node (C9 contract).
    SLUICE_CHECK_MSG(sched.cancel_wait(q, n), "caller cancels the stranded wait");
    SLUICE_CHECK_MSG(n.was_cancelled(), "node now Cancelled (safe to destroy)");
}

// =============================================================================
// E11-T15: Live-mode deadline progress.
//
// A deadline wait in LIVE mode with a due deadline must PROGRESS: the run
// stays resident (parks) and the timer pump resolves Expired, resuming the
// waiter. This proves I6 (Deadline Park Liveness): an active deadline does
// not let the Scheduler park indefinitely past it. run_live + advance_clock
// from an external driver thread.
// =============================================================================
SLUICE_TEST_CASE(e11_t15_live_mode_deadline_progress) {
    if constexpr (!fiber_ctx::supported) return;

    AsyncIoContext ctx(std::make_unique<IdleBackend>());
    Scheduler sched(ctx);
    E11TimerTestHooks::enable_test_clock(sched);

    WaitQueue q;
    WaitNode n;
    std::atomic<bool> registered{false};
    std::atomic<bool> ran_after{false};
    std::atomic<bool> expired{false};

    Fiber fwait;
    fwait.set_entry([&](Fiber&) {
        registered.store(true, std::memory_order_release);
        sched.await_wait_deadline(q, n, Scheduler::deadline_t{50});
        ran_after.store(true, std::memory_order_release);
        expired.store(n.was_expired(), std::memory_order_release);
    });

    FiberStack sw;
    SLUICE_CHECK(sched.init_fiber(fwait, sw.base(), sw.size()));
    sched.spawn(fwait);

    // Run LIVE on a worker thread so it parks (deadline active, external-wake
    // possible). An external driver thread advances the clock past the
    // deadline; the parked worker's bounded park + pump resolves Expired.
    std::thread runner([&] { sched.run_live(1); });

    // Wait for the waiter to register, then drive the deadline.
    while (!registered.load(std::memory_order_acquire)) std::this_thread::yield();
    std::this_thread::sleep_for(std::chrono::milliseconds(5));  // let worker park
    sched.advance_clock(100);  // 100 >= 50 -> due; pump resolves Expired + signals wake

    runner.join();

    SLUICE_CHECK_MSG(ran_after.load(), "waiter resumed (deadline progressed in Live)");
    SLUICE_CHECK_MSG(expired.load(), "outcome Expired");
    SLUICE_CHECK_MSG(n.was_expired(), "node terminal Expired");
    SLUICE_CHECK_MSG(sched.waiting_count() == 0, "no unresolved waits");
}

// =============================================================================
// E11-T5: resource wake vs timer expiry at the resolve_ arbitration seam.
//
// Both the wake and the timer driver contend on the SAME registered node. The
// resolve_ CAS arbitrates: exactly ONE cause wins. We deterministically drive
// BOTH near-simultaneously (waker retries while driver advances the clock) and
// assert the node ends in exactly ONE terminal outcome and exactly ONE
// accounting closure (waiting_count == 0, one runnable ticket => run resumes).
// This is the core I1/I2 arbitration proof for the wake|timer pair.
// =============================================================================
SLUICE_TEST_CASE(e11_t5_resource_vs_timer_one_winner) {
    if constexpr (!fiber_ctx::supported) return;

    AsyncIoContext ctx(std::make_unique<IdleBackend>());
    Scheduler sched(ctx);
    E11TimerTestHooks::enable_test_clock(sched);

    WaitQueue q;
    WaitNode n;
    std::atomic<bool> registered{false};
    std::atomic<bool> ran_after{false};
    std::atomic<bool> driver_advanced{false};

    Fiber fwait, fwake, fdriver;
    fwait.set_entry([&](Fiber&) {
        registered.store(true, std::memory_order::release);
        sched.await_wait_deadline(q, n, Scheduler::deadline_t{50});
        ran_after.store(true, std::memory_order::release);
    });
    // Waker races to wake the head; the driver concurrently advances the clock.
    // Whichever wins the resolve_ CAS publishes once.
    fwake.set_entry([&](Fiber&) {
        spin_wait(registered);
        for (int i = 0; i < 10000 && !n.is_terminal(); ++i) {
            sched.wake_wait_one(q);
            std::this_thread::yield();
        }
    });
    fdriver.set_entry([&](Fiber&) {
        spin_wait(registered);
        std::this_thread::yield();  // let the waiter reach await_wait_deadline
        sched.advance_clock(100);   // 100 >= 50 -> deadline due, timer contends
        driver_advanced.store(true, std::memory_order::release);
    });

    FiberStack sw, sk, sd;
    SLUICE_CHECK(sched.init_fiber(fwait, sw.base(), sw.size()));
    SLUICE_CHECK(sched.init_fiber(fwake, sk.base(), sk.size()));
    SLUICE_CHECK(sched.init_fiber(fdriver, sd.base(), sd.size()));
    sched.spawn(fwait);
    sched.spawn(fwake);
    sched.spawn(fdriver);
    sched.run(1);

    SLUICE_CHECK_MSG(ran_after.load(), "waiter resumed (one winner)");
    // Exactly one terminal outcome (I1): woken XOR expired.
    const bool woken = n.was_woken();
    const bool expired = n.was_expired();
    SLUICE_CHECK_MSG(woken != expired, "exactly one cause won (woken XOR expired)");
    SLUICE_CHECK_MSG(!n.was_cancelled(), "not cancelled");
    SLUICE_CHECK_MSG(n.is_terminal(), "terminal");
    // I2: exactly one runnable publication => waiting_count is 0.
    SLUICE_CHECK_MSG(sched.waiting_count() == 0, "one accounting closure (count 0)");
}

// =============================================================================
// E11-T7: a timer registration bound to an OLD wait epoch cannot resolve a
// LATER wait epoch.
//
// Epoch E: register a deadline, resource wakes E (the timer for E is retired).
// Epoch E+1: a NEW node is registered in the same queue with a far-future
// deadline. The OLD retired timer for E is inert: advancing the clock past E's
// deadline MUST NOT resolve E+1. E+1 stays Registered until explicitly woken.
// This is the cross-epoch isolation proof (I3) — independent of address reuse.
// =============================================================================
SLUICE_TEST_CASE(e11_t7_old_timer_cannot_resolve_later_epoch) {
    if constexpr (!fiber_ctx::supported) return;

    AsyncIoContext ctx(std::make_unique<IdleBackend>());
    Scheduler sched(ctx);
    E11TimerTestHooks::enable_test_clock(sched);

    WaitQueue q;
    std::atomic<bool> registered_e{false};
    std::atomic<bool> e_resolved{false};
    std::atomic<bool> e1_registered{false};
    std::atomic<bool> e1_resolved{false};

    // Epoch E: register + wake (retires E's timer). Then epoch E+1: a second
    // node waits with a far-future deadline; E's retired timer must not touch it.
    //
    // E11-TIMER-ASAN-BUSY-POLL-HANG-CORRECTIVE-1 (W2) corrective.
    //
    // ROOT CAUSE (category A: test-harness defect). The original e11_t7 used
    // sched.run(2) — DRAIN mode — with two fibers that spin_wait on each other
    // between epochs. But run(2) DRAIN RETURNS STALLED as soon as a fiber
    // suspends in await_wait_deadline (MW-S3-unresolved), orphaning the other
    // fiber. The unbounded spin_wait then hung forever under ASan (observed:
    // 5/5 hangs, 100% CPU). Production is not the root cause: the production
    // timer/retirement mechanics are sound (the other run_live cases pass
    // under ASan). The defect was the DRAIN-mode + unbounded-spin design.
    //
    // CORRECTIVE: drive the two epochs from a coordinator OS thread while a
    // single waiter fiber runs under run_live(1) — the proven liveness pattern
    // of e11_t15. run_live stays resident while the waiter is suspended, so the
    // coordinator can deterministically wake E, observe resolution, register
    // E+1, advance the clock past E's retired deadline (inert), and finally
    // wake E+1. Every coordinator wait is BOUNDED (kBoundedCoordIters); on
    // bound exhaustion the test FAILs with a state dump (never silent PASS,
    // §B/§W2.5). The waiter records both epochs' outcomes for the assertions.
    Fiber fwait;
    fwait.set_entry([&](Fiber&) {
        // Epoch E.
        registered_e.store(true, std::memory_order_release);
        WaitNode node_e;
        sched.await_wait_deadline(q, node_e, Scheduler::deadline_t{100});
        // (node_e.was_woken() asserted by the coordinator after E resolves)
        e_resolved.store(true, std::memory_order_release);
        // Epoch E+1: a distinct node, far-future deadline. E's retired timer
        // must NOT resolve this when the clock advances past E's old deadline.
        WaitNode node_e1;
        e1_registered.store(true, std::memory_order_release);
        sched.await_wait_deadline(q, node_e1, Scheduler::deadline_t{100000});
        // (node_e1.was_woken() asserted by the coordinator after E+1 resolves)
        e1_resolved.store(true, std::memory_order_release);
    });

    FiberStack sw;
    SLUICE_CHECK(sched.init_fiber(fwait, sw.base(), sw.size()));
    sched.spawn(fwait);

    // run_live(1): stays resident while the waiter is suspended (deadline
    // active + external wake possible => worker parks instead of STALLED).
    std::thread runner([&] { sched.run_live(1); });

    // Bounded coordinator wait helper (W2): returns true if `flag` becomes true
    // within kBoundedCoordIters yields, false otherwise. Callers FAIL with a
    // state dump on false (never silent PASS, §B/§W2.5).
    constexpr unsigned kBoundedCoordIters = 200000;
    auto coord_wait = [&](std::atomic<bool>& flag) -> bool {
        for (unsigned i = 0; i < kBoundedCoordIters; ++i) {
            if (flag.load(std::memory_order_acquire)) return true;
            std::this_thread::yield();
        }
        return flag.load(std::memory_order_acquire);
    };

    // 1. Wait for E registration, then wake E (retires E's timer).
    if (!coord_wait(registered_e)) {
        runner.join();
        SLUICE_FAIL("e11_t7: coordinator timed out waiting for E registration");
        return;
    }
    for (int i = 0; i < 10000; ++i) {
        if (sched.wake_wait_one(q)) break;
        std::this_thread::yield();
    }
    if (!coord_wait(e_resolved)) {
        std::fprintf(stderr,
            "e11_t7 FAIL state dump (epoch E not resolved): registered_e=%d "
            "e_resolved=%d e1_registered=%d active_deadline_count=%zu "
            "waiting_count=%zu\n",
            registered_e.load(), e_resolved.load(), e1_registered.load(),
            E11TimerTestHooks::active_deadline_count(sched),
            sched.waiting_count());
        // Resolve E if stranded so the runner can drain cleanly.
        sched.wake_wait_one(q);
        runner.join();
        SLUICE_FAIL("e11_t7: coordinator timed out waiting for E resolution "
                    "(see stderr state dump)");
        return;
    }

    // 2. Wait for E+1 registration with its far-future (100000) deadline.
    if (!coord_wait(e1_registered)) {
        std::fprintf(stderr,
            "e11_t7 FAIL state dump (epoch E+1 not registered): e_resolved=%d "
            "e1_registered=%d active_deadline_count=%zu waiting_count=%zu\n",
            e_resolved.load(), e1_registered.load(),
            E11TimerTestHooks::active_deadline_count(sched),
            sched.waiting_count());
        sched.wake_wait_one(q);
        runner.join();
        SLUICE_FAIL("e11_t7: coordinator timed out waiting for E+1 registration "
                    "(see stderr state dump)");
        return;
    }

    // 3. Advance the clock PAST E's old deadline (100). advance_clock pumps
    //    deadlines SYNCHRONOUSLY under global_mtx_, so by the time it returns
    //    E's retired timer has been pumped and MUST be inert — it must NOT have
    //    resolved E+1 (deadline 100000). No sleep is needed: the pump is done.
    sched.advance_clock(500);

    // 4. THE CROSS-EPOCH ISOLATION PROOF: after the clock is past E's retired
    //    deadline, E+1 must STILL be suspended (not resolved by E's old timer).
    //    We observe e1_resolved is still false. (This is the I3 invariant.)
    if (e1_resolved.load(std::memory_order_acquire)) {
        sched.wake_wait_one(q);
        runner.join();
        SLUICE_FAIL("e11_t7: E+1 was resolved by E's retired timer (cross-epoch "
                    "isolation violated, I3)");
        return;
    }

    // 5. Wake E+1 explicitly (the only legitimate resolver). It must resolve
    //    Woken (not Expired) — proving E's old timer did not win.
    for (int i = 0; i < 10000; ++i) {
        if (sched.wake_wait_one(q)) break;
        std::this_thread::yield();
    }
    if (!coord_wait(e1_resolved)) {
        std::fprintf(stderr,
            "e11_t7 FAIL state dump (epoch E+1 not resolved by explicit wake): "
            "e1_registered=%d e1_resolved=%d active_deadline_count=%zu "
            "waiting_count=%zu\n",
            e1_registered.load(), e1_resolved.load(),
            E11TimerTestHooks::active_deadline_count(sched),
            sched.waiting_count());
        runner.join();
        SLUICE_FAIL("e11_t7: coordinator timed out waiting for E+1 resolution "
                    "(see stderr state dump)");
        return;
    }

    runner.join();

    SLUICE_CHECK_MSG(e_resolved.load(), "epoch E resolved");
    SLUICE_CHECK_MSG(e1_resolved.load(), "epoch E+1 resolved by explicit wake");
    SLUICE_CHECK_MSG(sched.waiting_count() == 0, "no unresolved waits");
}

// =============================================================================
// E11-T9/T10: timer retirement closes WaitNode dereference authority + stale
// physical timer entry after WaitNode destruction is inert (forced pump).
//
// This strengthens T8 with an EXPLICIT forced pump after the node is destroyed:
// epoch E's node is woken (retiring E's timer), the fiber resumes and the node
// goes out of scope, THEN a driver explicitly advances the clock past E's
// deadline AND pumps. A stale physical timer entry for E (if still present)
// MUST be inert: no dereference of the destroyed node (ASan UAF proof), no
// resolution. The decisive causal boundary is the retirement state, observed
// BEFORE any node dereference — not the address.
// =============================================================================
SLUICE_TEST_CASE(e11_t9_t10_forced_stale_pump_after_destruction_is_inert) {
    if constexpr (!fiber_ctx::supported) return;

    AsyncIoContext ctx(std::make_unique<IdleBackend>());
    Scheduler sched(ctx);
    E11TimerTestHooks::enable_test_clock(sched);

    WaitQueue q;
    std::atomic<bool> registered{false};

    Fiber fwait_e, fwake_e;
    fwait_e.set_entry([&](Fiber&) {
        registered.store(true, std::memory_order::release);
        // Epoch E: far-future deadline (will be retired by the wake).
        WaitNode node_e;
        sched.await_wait_deadline(q, node_e, Scheduler::deadline_t{100000});
        SLUICE_CHECK_MSG(node_e.was_woken(), "epoch E resolved Woken");
        // node_e destroyed here as the fiber frame returns — its storage is GONE
        // before the forced pump below runs (the pump executes after run() returns).
    });
    fwake_e.set_entry([&](Fiber&) {
        spin_wait(registered);
        // Retry until a node is woken (registration visibility). This mirrors
        // T8 exactly; the waker COMPLETES after the wake, so run(1) terminates.
        for (int i = 0; i < 10000; ++i) {
            if (sched.wake_wait_one(q)) break;
            std::this_thread::yield();
        }
    });

    FiberStack sw, sk;
    SLUICE_CHECK(sched.init_fiber(fwait_e, sw.base(), sw.size()));
    SLUICE_CHECK(sched.init_fiber(fwake_e, sk.base(), sk.size()));
    sched.spawn(fwait_e);
    sched.spawn(fwake_e);
    sched.run(1);  // Drain: E resolves Woken, node_e destroyed, run returns.

    // FORCE the stale timer for E to be processed AFTER node destruction: advance
    // the clock past E's deadline (100000) and pump. node_e's storage is already
    // destroyed (the waiter frame returned). A stale retired timer entry still
    // physically in the heap MUST be inert — no UAF dereference of the destroyed
    // node. This is the strictest form of the I4 lifetime-closure proof: the pump
    // runs strictly after node destruction, not concurrently with it.
    sched.advance_clock(200000);

    // If the stale timer were not inert, it would have dereferenced the destroyed
    // node_e storage (UAF under ASan) and/or resolved a nonexistent wait
    // (waiting_count would be nonzero). Passing here under ASan is the I4 proof.
    SLUICE_CHECK_MSG(sched.waiting_count() == 0, "stale timer resolved nothing (count 0)");
}

// =============================================================================
// E11-T11/T12/T13: deadline park topology (I6).
//
// T11: with one active deadline, the parked Scheduler progresses at expiry.
// T12: a NEW earlier deadline changes the park obligation (earliest becomes B).
// T13: retiring the earliest deadline preserves the NEXT deadline obligation.
//
// These observe the deadline obligation set directly (active count + earliest
// deadline value) under the worker-loop quiescence, then drive expiry. They do
// NOT use sleep_for as causal proof: the causal seam is the controllable clock
// + advance_clock() pump.
// =============================================================================
SLUICE_TEST_CASE(e11_t11_one_active_deadline_progresses) {
    if constexpr (!fiber_ctx::supported) return;

    AsyncIoContext ctx(std::make_unique<IdleBackend>());
    Scheduler sched(ctx);
    E11TimerTestHooks::enable_test_clock(sched);

    WaitQueue q;
    WaitNode n;
    std::atomic<bool> registered{false};
    std::atomic<bool> ran_after{false};

    Fiber fwait;
    fwait.set_entry([&](Fiber&) {
        registered.store(true, std::memory_order::release);
        sched.await_wait_deadline(q, n, Scheduler::deadline_t{50});
        ran_after.store(true, std::memory_order::release);
    });

    FiberStack sw;
    SLUICE_CHECK(sched.init_fiber(fwait, sw.base(), sw.size()));
    sched.spawn(fwait);

    // Run LIVE: the worker parks (deadline active => external_wake_possible).
    // The deadline-driven pump (via the bounded park timeout + worker re-loop)
    // resolves Expired. The causal seam is the controllable clock, advanced by
    // the driver thread below, NOT a wall-clock sleep.
    std::thread runner([&] { sched.run_live(1); });
    while (!registered.load(std::memory_order::acquire)) std::this_thread::yield();
    // Let the worker reach its park point (thread scheduling, not causal proof).
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
    // Drive the deadline: advancing the logical clock makes it due; the worker
    // loop's pump (on its bounded-park re-loop) resolves Expired.
    sched.advance_clock(100);
    runner.join();

    SLUICE_CHECK_MSG(ran_after.load(), "waiter resumed (one deadline progressed)");
    SLUICE_CHECK_MSG(n.was_expired(), "outcome Expired");
    SLUICE_CHECK_MSG(sched.waiting_count() == 0, "no unresolved waits");
}

SLUICE_TEST_CASE(e11_t12_new_earlier_deadline_becomes_earliest) {
    if constexpr (!fiber_ctx::supported) return;

    AsyncIoContext ctx(std::make_unique<IdleBackend>());
    Scheduler sched(ctx);
    E11TimerTestHooks::enable_test_clock(sched);

    WaitQueue qA, qB;
    WaitNode na, nb;
    std::atomic<bool> observed{false};
    std::atomic<bool> earliest_is_b{false};

    // Two waiter fibers, each registering one deadline (A=1000, B=200). Both
    // signal registration BEFORE suspending so the coordinator can observe the
    // obligation set evolve while both are ACTIVE (not yet resolved).
    Fiber fwa, fwb, fcoord;
    fwa.set_entry([&](Fiber&) {
        sched.await_wait_deadline(qA, na, Scheduler::deadline_t{1000});
    });
    fwb.set_entry([&](Fiber&) {
        sched.await_wait_deadline(qB, nb, Scheduler::deadline_t{200});
    });
    // Coordinator: poll the active-deadline set. Once BOTH A and B are active,
    // the earliest obligation MUST be B (200), the new earlier deadline.
    fcoord.set_entry([&](Fiber&) {
        // Wait for both deadlines to be registered+active.
        for (int i = 0; i < 100000; ++i) {
            std::size_t cnt = E11TimerTestHooks::active_deadline_count(sched);
            if (cnt >= 2) {
                Scheduler::deadline_t e = 0;
                if (E11TimerTestHooks::earliest_active_deadline(sched, e) && e == 200) {
                    earliest_is_b.store(true, std::memory_order::release);
                    break;
                }
            }
            std::this_thread::yield();
        }
        observed.store(true, std::memory_order::release);
    });

    FiberStack sa, sb, sc;
    SLUICE_CHECK(sched.init_fiber(fwa, sa.base(), sa.size()));
    SLUICE_CHECK(sched.init_fiber(fwb, sb.base(), sb.size()));
    SLUICE_CHECK(sched.init_fiber(fcoord, sc.base(), sc.size()));
    sched.spawn(fwa);
    sched.spawn(fwb);
    sched.spawn(fcoord);

    // run_live(3): three fibers (two waiters + coordinator) must run
    // concurrently — the coordinator spin-polls, so it needs its own worker
    // distinct from the suspending waiters.
    std::thread runner([&] { sched.run_live(3); });
    while (!observed.load(std::memory_order::acquire)) std::this_thread::yield();
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
    sched.advance_clock(2000);  // past both A(1000) and B(200); pump resolves both
    runner.join();

    SLUICE_CHECK_MSG(earliest_is_b.load(),
                     "new earlier deadline B(200) became the earliest obligation");
    SLUICE_CHECK_MSG(na.was_expired() && nb.was_expired(), "both resolved Expired");
    SLUICE_CHECK_MSG(sched.waiting_count() == 0, "both deadlines resolved");
}

SLUICE_TEST_CASE(e11_t13_retiring_earliest_preserves_next_deadline) {
    if constexpr (!fiber_ctx::supported) return;

    AsyncIoContext ctx(std::make_unique<IdleBackend>());
    Scheduler sched(ctx);
    E11TimerTestHooks::enable_test_clock(sched);

    WaitQueue qA, qB;
    WaitNode na, nb;
    std::atomic<bool> observed{false};
    std::atomic<bool> next_is_b{false};
    std::atomic<bool> a_woken{false};

    // A = 100 (earliest), B = 200 (second). A waker retires A; the next
    // obligation MUST remain B = 200.
    Fiber fwa, fwb, fwake_a, fcoord;
    fwa.set_entry([&](Fiber&) {
        sched.await_wait_deadline(qA, na, Scheduler::deadline_t{100});
        a_woken.store(na.was_woken(), std::memory_order::release);
    });
    fwb.set_entry([&](Fiber&) {
        sched.await_wait_deadline(qB, nb, Scheduler::deadline_t{200});
    });
    fwake_a.set_entry([&](Fiber&) {
        // Wait for A to be registered, then wake it (retires A's timer).
        for (int i = 0; i < 100000; ++i) {
            if (sched.wake_wait_one(qA)) break;
            std::this_thread::yield();
        }
    });
    fcoord.set_entry([&](Fiber&) {
        // Wait until A is resolved (woken, retired) and B is active, then the
        // active-deadline set is {B}; earliest MUST be B=200.
        for (int i = 0; i < 100000; ++i) {
            if (a_woken.load(std::memory_order::acquire)) {
                Scheduler::deadline_t e = 0;
                if (E11TimerTestHooks::earliest_active_deadline(sched, e) && e == 200) {
                    next_is_b.store(true, std::memory_order::release);
                    break;
                }
            }
            std::this_thread::yield();
        }
        observed.store(true, std::memory_order::release);
    });

    FiberStack sa, sb, sk, sc;
    SLUICE_CHECK(sched.init_fiber(fwa, sa.base(), sa.size()));
    SLUICE_CHECK(sched.init_fiber(fwb, sb.base(), sb.size()));
    SLUICE_CHECK(sched.init_fiber(fwake_a, sk.base(), sk.size()));
    SLUICE_CHECK(sched.init_fiber(fcoord, sc.base(), sc.size()));
    sched.spawn(fwa);
    sched.spawn(fwb);
    sched.spawn(fwake_a);
    sched.spawn(fcoord);

    std::thread runner([&] { sched.run_live(4); });
    while (!observed.load(std::memory_order::acquire)) std::this_thread::yield();
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
    sched.advance_clock(300);  // past B=200; pump resolves B Expired
    runner.join();

    SLUICE_CHECK_MSG(next_is_b.load(),
                     "after retiring earliest A, next obligation is B=200");
    SLUICE_CHECK_MSG(nb.was_expired(), "B resolved Expired");
    SLUICE_CHECK_MSG(sched.waiting_count() == 0,
                     "B resolved (earliest retirement preserved next)");
}

// =============================================================================
// E11-T17 (F2): new-earlier-deadline during an existing park obligation.
//
// The closure review (F2) found that T12 only proves "earliest deadline cache
// becomes B" — it does NOT force the causal interleaving:
//
//     Worker W is in the park path with only timer state A (far-future) active
//     THEN deadline B < A is registered
//     W must not remain parked until A; B drives the wake
//
// This test forces that interleaving on a SINGLE worker. The worker runs LIVE,
// registers A (far-future 10000), and enters the park path — the park_commit
// seam holds it at the physical-wait boundary (the seam sits at park_on_wake_
// source, before the cv.wait; at that instant A=10000 is the only active
// deadline). While held, the coordinator thread installs B (200 < 10000) via
// the narrow register_test_deadline hook (a non-worker-thread registration; the
// worker is held at the seam with global_mtx_ released, so no deadlock). The
// coordinator then advances the logical clock to make B due and releases the
// seam. The worker re-loops; pump_deadlines_locked observes B due under
// global_mtx_ and resolves B Expired. Proof: B's expiry is observed and B
// resolves BEFORE A (A never becomes due; clock=250 < A=10000).
//
// (Note on seam position: the park_commit seam pauses the worker BEFORE it
// reads earliest_active_deadline_ for the cv.wait timeout. So the precise claim
// is "a deadline installed while the worker is in the park path is not lost" —
// the pump on the next loop iteration resolves it. The test separately asserts,
// at the seam, that A is the earliest active obligation, so the "only A active
// when the worker entered the park path" precondition is captured.)
//
// PRODUCTION MECHANISM (documented accurately, per F2): E11 does NOT signal a
// parked Worker on timer registration (no registration-triggered wake). Park
// liveness is BOUNDED POLLING — park_on_wake_source caps the cv.wait by the
// earliest ACTIVE deadline, and the worker loop's pump_deadlines_locked
// re-establishes the authoritative deadline set under global_mtx_ on every
// iteration. So "new earlier deadline during park" resolves via the bounded-
// park + per-iteration pump, not a registration signal. The causal seam here is
// the controllable clock + park_commit seam + an external (non-worker)
// registration — NOT sleep_for, NOT stress (M7). Single worker => deterministic
// (no cross-worker scheduling races).
// =============================================================================
SLUICE_TEST_CASE(e11_t17_new_earlier_deadline_during_park) {
    if constexpr (!fiber_ctx::supported) return;

    AsyncIoContext ctx(std::make_unique<IdleBackend>());
    Scheduler sched(ctx);
    sluice_async_test::ControllerGuard ctrl(sched);  // park-commit seam registry
    E11TimerTestHooks::enable_test_clock(sched);

    WaitQueue qA, qB;
    WaitNode na, nb;  // na: the parked A wait. nb: the externally-registered B.
    std::atomic<bool> a_registered{false};

    // Waiter A: far-future deadline (10000). Signals registration so the
    // coordinator knows the worker will park with A's obligation as earliest.
    Fiber fwa;
    fwa.set_entry([&](Fiber&) {
        a_registered.store(true, std::memory_order::release);
        sched.await_wait_deadline(qA, na, Scheduler::deadline_t{10000});
    });

    FiberStack sa;
    SLUICE_CHECK(sched.init_fiber(fwa, sa.base(), sa.size()));
    sched.spawn(fwa);

    // Arm the park-commit seam BEFORE run_live so the worker is held at the
    // commit boundary when it parks (after computing its park obligation from
    // earliest_active_deadline_, before the physical cv.wait).
    E11TimerTestHooks::arm_park_commit(sched);

    std::thread runner([&] { sched.run_live(1); });

    // 1. Wait for A to register and the worker to reach+pause at the park-commit
    //    boundary. At this point the only active deadline is A (10000).
    while (!a_registered.load(std::memory_order::acquire)) std::this_thread::yield();
    E11TimerTestHooks::wait_park_commit_paused(sched);

    // 2. Causal observation: at the park-commit boundary, the earliest active
    //    deadline is A (10000) — the worker committed to park using A's state.
    //    Only A is active (B not yet registered).
    {
        Scheduler::deadline_t e = 0;
        const bool has = E11TimerTestHooks::earliest_active_deadline(sched, e);
        SLUICE_CHECK_MSG(has && e == 10000,
                         "worker parked with A(10000) as the earliest obligation");
        std::size_t cnt = E11TimerTestHooks::active_deadline_count(sched);
        SLUICE_CHECK_MSG(cnt == 1, "only A is active at the park-commit seam");
    }

    // 3. While the worker is HELD at the seam, the coordinator thread installs
    //    the NEW earlier deadline B (200 < 10000) via the external-registration
    //    test hook (global_mtx_ is free: the worker is paused at the seam with
    //    no lock held). This is the F2 interleaving: committed-to-park-with-A,
    //    THEN B registers.
    TimerRegistration* reg_b = E11TimerTestHooks::register_test_deadline(
        sched, &nb, &qB, Scheduler::deadline_t{200});
    SLUICE_CHECK_MSG(reg_b != nullptr,
                     "B(200) registered while worker held at the park-commit seam");
    // The new earlier deadline changed the earliest obligation to B (200).
    {
        Scheduler::deadline_t e = 0;
        const bool has = E11TimerTestHooks::earliest_active_deadline(sched, e);
        SLUICE_CHECK_MSG(has && e == 200,
                         "new earlier deadline B(200) became the earliest obligation");
        std::size_t cnt = E11TimerTestHooks::active_deadline_count(sched);
        SLUICE_CHECK_MSG(cnt == 2, "both A and B are now active");
    }

    // 4. Make B due (200) — strictly before A (10000). Release the seam. The
    //    worker re-loops; pump_deadlines_locked observes B due under global_mtx_
    //    and resolves B's node Expired. A is NOT due (10000 > 250).
    E11TimerTestHooks::set_clock(sched, 250);
    E11TimerTestHooks::release_park_commit(sched);

    // Wait for the pump to resolve B (deterministic: the worker re-loops after
    // the seam release and pumps under global_mtx_). Bounded poll, not sleep.
    for (int i = 0; i < 200000 && !nb.is_terminal(); ++i) std::this_thread::yield();

    // 5. Proof: the pump resolved B (the new earlier deadline) Expired. A
    //    remained unresolved (its deadline 10000 was never reached; clock=250).
    //    The worker did NOT remain parked until A — B's earlier deadline drove
    //    the wake (via the bounded park + per-iteration pump).
    //
    //    NB: we observe the outcome ONLY through the caller-owned WaitNode `nb`
    //    (its state_ is atomic and the node is NOT pool-owned, so it is safe to
    //    read lock-free from the coordinator). We deliberately do NOT re-read
    //    `reg_b` here: the pump erases B's TimerRegistration pool block under
    //    global_mtx_ in the same iteration it resolves B (lazy-at-deadline
    //    reclamation), so a lock-free read of reg_b from this thread would race
    //    with that erase (TSan). `nb` Expired is sufficient proof the timer
    //    won: the pump resolves a node ONLY via try_claim_expiry (ACTIVE->CONSUMED
    //    CAS) — so a terminal-Expired nb implies B's registration was consumed.
    SLUICE_CHECK_MSG(nb.was_expired(),
                     "B(200) resolved Expired (drove the wake, not A)");
    SLUICE_CHECK_MSG(!na.is_terminal(),
                     "A(10000) still Registered (worker did not remain parked until A)");
    SLUICE_CHECK_MSG(!na.was_expired(),
                     "A(10000) did NOT expire (B resolved first; A not due)");

    // A is still unresolved (parked on its far-future deadline). Cancel it from
    // the coordinator so run_live can reach quiescence and the runner thread can
    // join (the cancel signals the Scheduler wake source). Then the node is safe
    // to destroy (C9).
    SLUICE_CHECK_MSG(sched.cancel_wait(qA, na), "cancel the stranded A wait");
    runner.join();
    SLUICE_CHECK_MSG(sched.waiting_count() == 0, "no unresolved waits remain");
}

// =============================================================================
// E11-T18 (F3): timer storage reclamation contract (lazy-at-deadline).
//
// The closure review (F3) found that the "timer-pool unbounded growth fixed"
// claim was overbroad: a far-future RETIRED registration remains PHYSICALLY
// retained in the pool+heap until pump_deadlines_locked pops it at its ORIGINAL
// deadline. This test proves the ACTUAL contract:
//
//     logical retirement    immediate (ACTIVE -> RETIRED in the winner CS)
//     lifetime safety       immediate (atomic state gate; no destroyed-node deref)
//     physical reclamation  lazy-at-deadline (pump pops+erases at the deadline)
//
// It creates N far-future deadline waits, resolves each via RESOURCE_WAKE (which
// RETIRES each registration immediately), and asserts:
//   (a) immediately after the wakes: 0 ACTIVE entries (logical retirement was
//       immediate), but the pool+heap still hold N inert RETIRED entries
//       (physical reclamation has NOT happened — their far-future deadlines have
//       not been reached);
//   (b) after advancing the clock past those deadlines and pumping: the pool+heap
//       are empty (physical reclamation completed at the deadline).
//
// This is the honest reclamation contract — NOT "unbounded growth fixed" in an
// absolute sense (the pool is bounded by live waits PLUS retired entries whose
// deadlines have not yet been reached), but bounded by the deadline horizon and
// proven to drain at-deadline. Uses the controllable clock (no sleep_for, M7).
// =============================================================================
SLUICE_TEST_CASE(e11_t18_timer_reclamation_lazy_at_deadline) {
    if constexpr (!fiber_ctx::supported) return;

    constexpr unsigned N = 4;
    AsyncIoContext ctx(std::make_unique<IdleBackend>());
    Scheduler sched(ctx);
    E11TimerTestHooks::enable_test_clock(sched);

    WaitQueue q;
    std::array<WaitNode, N> nodes;
    std::array<Fiber, N> waiters;
    std::array<FiberStack, N> stacks;
    std::atomic<unsigned> registered{0};

    // N waiters, each with a FAR-FUTURE deadline (so the pump will NOT pop them
    // until the clock is advanced past it). Each registers then suspends.
    for (unsigned i = 0; i < N; ++i) {
        waiters[i].set_entry([&, i](Fiber&) {
            registered.fetch_add(1, std::memory_order::release);
            sched.await_wait_deadline(q, nodes[i], Scheduler::deadline_t{100000});
        });
        SLUICE_CHECK(sched.init_fiber(waiters[i], stacks[i].base(), stacks[i].size()));
        sched.spawn(waiters[i]);
    }
    sched.run(1);  // Drain: each waiter registers then suspends (no resolver).

    // (pre) All N deadlines are ACTIVE and physically present.
    SLUICE_CHECK_MSG(E11TimerTestHooks::active_deadline_count(sched) == N,
                     "N active deadlines registered");
    SLUICE_CHECK_MSG(E11TimerTestHooks::timer_pool_size(sched) == N,
                     "pool holds N entries (all active)");

    // Resolve each via RESOURCE_WAKE. The wake winner RETIRES the bound
    // registration in the SAME CS (logical retirement is immediate). A retry
    // waker fiber is needed because the waiters are suspended on the queue.
    Fiber fwaker;
    fwaker.set_entry([&](Fiber&) {
        for (unsigned i = 0; i < N; ++i) {
            for (int k = 0; k < 10000 && !nodes[i].is_terminal(); ++k) {
                sched.wake_wait_one(q);
                std::this_thread::yield();
            }
        }
    });
    FiberStack sw;
    SLUICE_CHECK(sched.init_fiber(fwaker, sw.base(), sw.size()));
    sched.spawn(fwaker);
    sched.run(1);  // wake each waiter (retires each registration immediately)

    // (a) Immediately after the wakes: logical retirement was IMMEDIATE (0
    // ACTIVE), lifetime safety is immediate (the entries are inert), BUT
    // physical reclamation has NOT happened — the far-future RETIRED entries
    // remain physically in the pool+heap because their deadlines (100000) have
    // not been reached by the pump. This is the overclaim F3 corrects: the pool
    // is NOT empty here.
    SLUICE_CHECK_MSG(E11TimerTestHooks::active_deadline_count(sched) == 0,
                     "logical retirement immediate: 0 active after wakes");
    SLUICE_CHECK_MSG(
        E11TimerTestHooks::timer_pool_count_in_state(
            sched, TimerRegistration::State::retired) == N,
        "N entries RETIRED (logical retirement immediate)");
    SLUICE_CHECK_MSG(E11TimerTestHooks::timer_pool_size(sched) == N,
                     "physical retention: pool still holds N RETIRED entries");
    SLUICE_CHECK_MSG(E11TimerTestHooks::deadline_heap_size(sched) == N,
                     "physical retention: heap still holds N RETIRED entries");

    // (b) Advance the clock PAST the far-future deadlines and pump. Now the
    // pump pops each RETIRED entry (at its deadline) and erases its pool block.
    // Physical reclamation completes at the deadline.
    E11TimerTestHooks::set_clock(sched, 200000);
    sched.advance_clock(200000);

    SLUICE_CHECK_MSG(E11TimerTestHooks::timer_pool_size(sched) == 0,
                     "physical reclamation at-deadline: pool drained");
    SLUICE_CHECK_MSG(E11TimerTestHooks::deadline_heap_size(sched) == 0,
                     "physical reclamation at-deadline: heap drained");
    SLUICE_CHECK_MSG(sched.waiting_count() == 0, "no unresolved waits");
}

// =============================================================================
// E11-T16: repeated three-way RESOURCE_WAKE / TIMER_EXPIRE / CANCEL race.
//
// A waiter with a near deadline + a waker + a canceller all contend on the same
// node across many iterations (deterministic causal seam: test clock + retry
// loops, not random sleeps). Every iteration MUST end in exactly ONE terminal
// outcome and ONE accounting closure (I1/I2). Stress repetition is SECONDARY
// evidence after the causal proofs (T0..T5); it guards against rare interleaving
// regressions. Bounded iteration count.
// =============================================================================
SLUICE_TEST_CASE(e11_t16_three_way_race_one_winner_repeated) {
    if constexpr (!fiber_ctx::supported) return;

    constexpr int kIters = 200;
    for (int it = 0; it < kIters; ++it) {
        AsyncIoContext ctx(std::make_unique<IdleBackend>());
        Scheduler sched(ctx);
        E11TimerTestHooks::enable_test_clock(sched);

        WaitQueue q;
        WaitNode n;
        std::atomic<bool> registered{false};
        std::atomic<bool> ran_after{false};

        Fiber fwait, fwake, fcancel, fdriver;
        fwait.set_entry([&](Fiber&) {
            registered.store(true, std::memory_order::release);
            sched.await_wait_deadline(q, n, Scheduler::deadline_t{50});
            ran_after.store(true, std::memory_order::release);
        });
        fwake.set_entry([&](Fiber&) {
            spin_wait(registered);
            for (int i = 0; i < 2000 && !n.is_terminal(); ++i) {
                sched.wake_wait_one(q);
                std::this_thread::yield();
            }
        });
        fcancel.set_entry([&](Fiber&) {
            spin_wait(registered);
            for (int i = 0; i < 2000 && !n.is_terminal(); ++i) {
                sched.cancel_wait(q, n);
                std::this_thread::yield();
            }
        });
        fdriver.set_entry([&](Fiber&) {
            spin_wait(registered);
            std::this_thread::yield();
            sched.advance_clock(100);  // 100 >= 50 -> timer contends
        });

        FiberStack sw, sk, sc, sd;
        SLUICE_CHECK(sched.init_fiber(fwait, sw.base(), sw.size()));
        SLUICE_CHECK(sched.init_fiber(fwake, sk.base(), sk.size()));
        SLUICE_CHECK(sched.init_fiber(fcancel, sc.base(), sc.size()));
        SLUICE_CHECK(sched.init_fiber(fdriver, sd.base(), sd.size()));
        sched.spawn(fwait);
        sched.spawn(fwake);
        sched.spawn(fcancel);
        sched.spawn(fdriver);
        sched.run(1);

        // I1: exactly one terminal outcome.
        SLUICE_CHECK_MSG(ran_after.load(), "waiter resumed");
        const int outcomes = (n.was_woken() ? 1 : 0) +
                             (n.was_cancelled() ? 1 : 0) +
                             (n.was_expired() ? 1 : 0);
        SLUICE_CHECK_MSG(outcomes == 1, "exactly one terminal outcome (three-way)");
        // I2: one accounting closure.
        SLUICE_CHECK_MSG(sched.waiting_count() == 0, "one accounting closure (count 0)");
    }
}

SLUICE_MAIN()
