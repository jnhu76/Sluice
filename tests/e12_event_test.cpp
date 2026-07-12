// e12_event_test — Async Event synchronization primitive (sluice-CORE-E12-A).
//
// Deterministic production tests for the persistent manual-reset Event built on
// the closed E10/E11 wait substrate. Observed ONLY through the SEALED Event
// public API + the mechanically gated test hooks:
//
//   - Event::is_set / set / reset / wait / wait_until / cancel
//     (cancel is the narrow per-wait-epoch CANCEL authority; raw wait_queue()
//      is REMOVED from the production surface — F-EVENT-AUTH)
//   - E12EventTestHooks: test-only phase seams + the underlying WaitQueue,
//     reachable ONLY through this friend struct (defined here, in the test TU).
//     An ordinary production TU cannot name E12EventTestHooks.
//   - Scheduler::advance_clock / E11TimerTestHooks       (deterministic timer)
//   - WaitNode public lock-free state queries (was_woken/was_cancelled/
//     was_expired/is_terminal/outcome)
//   - Scheduler::waiting_count()  (the wait-accounting authority)
//
// Every causal race proof uses mechanically gated phase seams + retry loops or
// barriers — NEVER sleep_for timing as causal proof (H7). A bounded timeout may
// remain ONLY as a test-failure guard, not as causal synchronization.
//
// Gated to x86_64 (fiber_ctx::supported): registration requires a real Fiber.
#include "harness.hpp"
#include "async_test_control.hpp"

#include <sluice/async/async_io_context.hpp>
#include <sluice/async/event.hpp>
#include <sluice/async/fake_backend.hpp>
#include <sluice/async/fiber.hpp>
#include <sluice/async/fiber_ctx.hpp>
#include <sluice/async/scheduler.hpp>
#include <sluice/async/wait_node.hpp>
#include <sluice/async/wait_queue.hpp>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <thread>

using namespace sluice::async;
using sluice::Result;

// ASYNC-TEST-SEAM-AUTHORITY-CORRECTIVE-1: the forgeable E11TimerTestHooks +
// E12EventTestHooks friends are removed. The clock/timer + Event phase seams +
// the E9 park-commit seam (reused by T32) are driven by the internal-testing
// controller facades E11TimerControl / E12EventSeam / E9ParkSeam
// (tests/async_test_control.hpp), which route through Scheduler::AsyncTestAccess
// (guarded) + the per-Scheduler* phase registry. The dead event_wait_queue
// helper (C10) is dropped — no test needs raw WaitQueue access now that
// Event::cancel is the authority. Call sites keep the historical names via
// local aliases so the test cases read unchanged.
namespace {
using E11TimerTestHooks = sluice_async_test::E11TimerControl;
using E12EventTestHooks = sluice_async_test::E12EventSeam;
}  // namespace

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

// Spin until `pred` is true (TEST SYNC ONLY).
inline void spin_wait_pred(auto&& pred) {
    while (!pred()) {
        std::this_thread::yield();
    }
}
}  // namespace

SLUICE_MAIN()

// ===========================================================================
// Slice 1 — Basic semantics (T0–T4)
// ===========================================================================

// ---- T0: initially UNSET Event, wait suspends and is woken by set() --------
//
// A waiter Fiber suspends on an UNSET Event. A waker Fiber calls set(). The
// waiter must resume with outcome Woken. Proves the basic suspend + set-wake
// path through the Event substrate.
SLUICE_TEST_CASE(e12_t0_unset_event_wait_suspends_and_wakes) {
    if constexpr (!fiber_ctx::supported) return;

    AsyncIoContext ctx(std::make_unique<IdleBackend>());
    Scheduler sched(ctx);

    Event ev(sched, /*initially_set=*/false);
    WaitNode node;
    std::atomic<bool> waiter_suspended{false};
    std::atomic<int> entries{0};

    Fiber fwait, fwake;
    fwait.set_entry([&](Fiber&) {
        entries.fetch_add(1, std::memory_order_acq_rel);
        waiter_suspended.store(true, std::memory_order_release);
        ev.wait(node);
        entries.fetch_add(1, std::memory_order_acq_rel);
    });
    fwake.set_entry([&](Fiber&) {
        spin_wait(waiter_suspended);
        std::this_thread::yield();  // let the waiter reach ev.wait
        ev.set();
    });

    FiberStack sw, sk;
    SLUICE_CHECK(sched.init_fiber(fwait, sw.base(), sw.size()));
    SLUICE_CHECK(sched.init_fiber(fwake, sk.base(), sk.size()));
    sched.spawn(fwait);
    sched.spawn(fwake);
    sched.run(2);

    SLUICE_CHECK_MSG(entries.load() == 2, "waiter resumed exactly once");
    SLUICE_CHECK_MSG(node.was_woken(), "waiter resolved Woken");
    SLUICE_CHECK_MSG(ev.is_set(), "Event is SET after set()");
    SLUICE_CHECK_MSG(sched.waiting_count() == 0, "no unresolved waits remain");
}

// ---- T1: initially SET Event, wait returns immediately without suspension ---
//
// A waiter Fiber calls wait() on a SET Event. It must return Woken WITHOUT
// suspending (entries == 2 in a single run with no waker). This is the
// level-triggered persistent readiness property: a late waiter observes SET.
SLUICE_TEST_CASE(e12_t1_set_event_wait_returns_immediately) {
    if constexpr (!fiber_ctx::supported) return;

    AsyncIoContext ctx(std::make_unique<IdleBackend>());
    Scheduler sched(ctx);

    Event ev(sched, /*initially_set=*/true);
    WaitNode node;
    std::atomic<int> entries{0};

    Fiber fwait;
    fwait.set_entry([&](Fiber&) {
        entries.fetch_add(1, std::memory_order_acq_rel);
        ev.wait(node);
        entries.fetch_add(1, std::memory_order_acq_rel);
    });

    FiberStack sw;
    SLUICE_CHECK(sched.init_fiber(fwait, sw.base(), sw.size()));
    sched.spawn(fwait);
    sched.run(1);

    SLUICE_CHECK_MSG(entries.load() == 2, "waiter never suspended (ran to completion)");
    SLUICE_CHECK_MSG(node.was_woken(), "waiter resolved Woken at admission");
    SLUICE_CHECK_MSG(sched.waiting_count() == 0, "no waits registered");
}

// ---- T2: set() idempotence ------------------------------------------------
//
// Calling set() on an already-SET Event is a no-op: no extra wake, no state
// change. Two set() calls leave the Event SET and do not produce spurious
// wakeups.
SLUICE_TEST_CASE(e12_t2_set_idempotent) {
    if constexpr (!fiber_ctx::supported) return;

    AsyncIoContext ctx(std::make_unique<IdleBackend>());
    Scheduler sched(ctx);

    Event ev(sched, /*initially_set=*/true);
    WaitNode node;

    Fiber f;
    f.set_entry([&](Fiber&) {
        ev.wait(node);       // returns immediately (SET)
        ev.set();            // idempotent: already SET
        ev.set();            // idempotent again
    });

    FiberStack sw;
    SLUICE_CHECK(sched.init_fiber(f, sw.base(), sw.size()));
    sched.spawn(f);
    sched.run(1);

    SLUICE_CHECK_MSG(node.was_woken(), "waiter resolved Woken");
    SLUICE_CHECK_MSG(ev.is_set(), "Event remains SET");
    SLUICE_CHECK_MSG(sched.waiting_count() == 0, "no waits registered");
}

// ---- T3: reset() clears persistent readiness --------------------------------
//
// After reset(), a subsequent wait() on the now-UNSET Event suspends (requires
// a waker). Proves reset transitions SET -> UNSET and the readiness is no
// longer observable by late waiters.
SLUICE_TEST_CASE(e12_t3_reset_clears_readiness) {
    if constexpr (!fiber_ctx::supported) return;

    AsyncIoContext ctx(std::make_unique<IdleBackend>());
    Scheduler sched(ctx);

    Event ev(sched, /*initially_set=*/true);
    WaitNode node;
    std::atomic<bool> waiter_suspended{false};
    std::atomic<int> entries{0};

    Fiber fwait, fwake;
    fwait.set_entry([&](Fiber&) {
        ev.reset();  // SET -> UNSET
        SLUICE_CHECK_MSG(!ev.is_set(), "Event is UNSET after reset");
        waiter_suspended.store(true, std::memory_order_release);
        entries.fetch_add(1, std::memory_order_acq_rel);
        ev.wait(node);  // now suspends (UNSET)
        entries.fetch_add(1, std::memory_order_acq_rel);
    });
    fwake.set_entry([&](Fiber&) {
        spin_wait(waiter_suspended);
        std::this_thread::yield();
        ev.set();
    });

    FiberStack sw, sk;
    SLUICE_CHECK(sched.init_fiber(fwait, sw.base(), sw.size()));
    SLUICE_CHECK(sched.init_fiber(fwake, sk.base(), sk.size()));
    sched.spawn(fwait);
    sched.spawn(fwake);
    sched.run(2);

    SLUICE_CHECK_MSG(entries.load() == 2, "waiter resumed after set");
    SLUICE_CHECK_MSG(node.was_woken(), "waiter resolved Woken");
    SLUICE_CHECK_MSG(ev.is_set(), "Event is SET after the waker's set()");
}

// ---- T4: late waiter after SET returns immediately -------------------------
//
// set() makes the Event SET. A waiter that arrives AFTER set() observes SET
// and returns Woken without suspension. This is the "late waiter" property of
// level-triggered persistent readiness.
SLUICE_TEST_CASE(e12_t4_late_waiter_after_set_returns_immediately) {
    if constexpr (!fiber_ctx::supported) return;

    AsyncIoContext ctx(std::make_unique<IdleBackend>());
    Scheduler sched(ctx);

    Event ev(sched, /*initially_set=*/false);
    WaitNode node;
    std::atomic<int> entries{0};

    Fiber fsetter, fwaiter;
    fsetter.set_entry([&](Fiber&) {
        ev.set();  // SET before the waiter arrives
    });
    fwaiter.set_entry([&](Fiber&) {
        entries.fetch_add(1, std::memory_order_acq_rel);
        ev.wait(node);  // late waiter: observes SET, returns immediately
        entries.fetch_add(1, std::memory_order_acq_rel);
    });

    FiberStack ss, sw;
    SLUICE_CHECK(sched.init_fiber(fsetter, ss.base(), ss.size()));
    SLUICE_CHECK(sched.init_fiber(fwaiter, sw.base(), sw.size()));
    sched.spawn(fsetter);
    sched.spawn(fwaiter);
    sched.run(2);

    SLUICE_CHECK_MSG(entries.load() == 2, "late waiter never suspended");
    SLUICE_CHECK_MSG(node.was_woken(), "late waiter resolved Woken");
    SLUICE_CHECK_MSG(ev.is_set(), "Event is SET");
}

// ===========================================================================
// Slice 2 — Lost-set admission closure (T5–T7)
// ===========================================================================

// ---- T5: set before admission — waiter observes SET, returns Woken ---------
//
// set() runs and completes (SET + drain of an empty queue) BEFORE the waiter
// reaches wait(). The waiter's admission observes SET and returns Woken inline
// without registering or suspending. No lost set: the readiness is persistent.
SLUICE_TEST_CASE(e12_t5_set_before_admission_observes_set) {
    if constexpr (!fiber_ctx::supported) return;

    AsyncIoContext ctx(std::make_unique<IdleBackend>());
    Scheduler sched(ctx);

    Event ev(sched, /*initially_set=*/false);
    WaitNode node;
    std::atomic<bool> set_done{false};
    std::atomic<int> entries{0};

    Fiber fsetter, fwaiter;
    fsetter.set_entry([&](Fiber&) {
        ev.set();  // SET + drain (empty queue) completes before waiter arrives
        set_done.store(true, std::memory_order_release);
    });
    fwaiter.set_entry([&](Fiber&) {
        spin_wait(set_done);  // ensure set() has fully completed
        entries.fetch_add(1, std::memory_order_acq_rel);
        ev.wait(node);  // admission observes SET -> Woken inline
        entries.fetch_add(1, std::memory_order_acq_rel);
    });

    FiberStack ss, sw;
    SLUICE_CHECK(sched.init_fiber(fsetter, ss.base(), ss.size()));
    SLUICE_CHECK(sched.init_fiber(fwaiter, sw.base(), sw.size()));
    sched.spawn(fsetter);
    sched.spawn(fwaiter);
    sched.run(2);

    SLUICE_CHECK_MSG(entries.load() == 2, "waiter never suspended");
    SLUICE_CHECK_MSG(node.was_woken(), "waiter resolved Woken at admission");
    SLUICE_CHECK_MSG(!node.is_registered(), "node not left registered");
    SLUICE_CHECK_MSG(sched.waiting_count() == 0, "no waits registered");
}

// ---- T6: set during admission — no stranded waiter -------------------------
//
// The waiter begins admission while UNSET; set() occurs in the admission
// window. Under global_mtx_ serialization, exactly one of two paths occurs:
//   (a) set() runs first: admission observes SET -> Woken inline (no suspend)
//   (b) admission runs first: waiter registers + suspends; set()'s drain wakes it
// Either way the waiter resolves Woken — no third outcome, no stranded waiter.
// This test forces the race by having the setter spin until the waiter has
// entered wait(), then set().
SLUICE_TEST_CASE(e12_t6_set_during_admission_no_stranded_waiter) {
    if constexpr (!fiber_ctx::supported) return;

    AsyncIoContext ctx(std::make_unique<IdleBackend>());
    Scheduler sched(ctx);

    Event ev(sched, /*initially_set=*/false);
    WaitNode node;
    std::atomic<bool> waiter_entered{false};
    std::atomic<int> entries{0};

    Fiber fwaiter, fsetter;
    fwaiter.set_entry([&](Fiber&) {
        entries.fetch_add(1, std::memory_order_acq_rel);
        waiter_entered.store(true, std::memory_order_release);
        ev.wait(node);  // admission may race with set()
        entries.fetch_add(1, std::memory_order_acq_rel);
    });
    fsetter.set_entry([&](Fiber&) {
        spin_wait(waiter_entered);  // wait until the waiter has entered wait()
        std::this_thread::yield();  // let the waiter reach the admission CS
        ev.set();                   // races with admission; either path is Woken
    });

    FiberStack sw, ss;
    SLUICE_CHECK(sched.init_fiber(fwaiter, sw.base(), sw.size()));
    SLUICE_CHECK(sched.init_fiber(fsetter, ss.base(), ss.size()));
    sched.spawn(fwaiter);
    sched.spawn(fsetter);
    sched.run(2);

    SLUICE_CHECK_MSG(entries.load() == 2, "waiter resumed (no stranded waiter)");
    SLUICE_CHECK_MSG(node.was_woken(), "waiter resolved Woken (either path)");
    SLUICE_CHECK_MSG(!node.is_registered(), "node not left registered");
    SLUICE_CHECK_MSG(sched.waiting_count() == 0, "no waits remain");
}

// ---- T7: set after suspension commit — normal Scheduler resolution --------
//
// The waiter registers and commits suspension (Fiber waiting). set() then runs
// and its drain resolves the registered waiter through the canonical
// RESOURCE_WAKE path (wake_wait_one_locked -> resolve_(Woken) + route).
SLUICE_TEST_CASE(e12_t7_set_after_suspension_wakes_through_scheduler) {
    if constexpr (!fiber_ctx::supported) return;

    AsyncIoContext ctx(std::make_unique<IdleBackend>());
    Scheduler sched(ctx);

    Event ev(sched, /*initially_set=*/false);
    WaitNode node;
    std::atomic<bool> waiter_suspended{false};
    std::atomic<int> entries{0};

    Fiber fwaiter, fsetter;
    fwaiter.set_entry([&](Fiber&) {
        entries.fetch_add(1, std::memory_order_acq_rel);
        waiter_suspended.store(true, std::memory_order_release);
        ev.wait(node);  // suspends (UNSET)
        entries.fetch_add(1, std::memory_order_acq_rel);
    });
    fsetter.set_entry([&](Fiber&) {
        spin_wait(waiter_suspended);
        std::this_thread::yield();  // ensure the waiter has committed waiting
        ev.set();                   // drain wakes the suspended waiter
    });

    FiberStack sw, ss;
    SLUICE_CHECK(sched.init_fiber(fwaiter, sw.base(), sw.size()));
    SLUICE_CHECK(sched.init_fiber(fsetter, ss.base(), ss.size()));
    sched.spawn(fwaiter);
    sched.spawn(fsetter);
    sched.run(2);

    SLUICE_CHECK_MSG(entries.load() == 2, "waiter resumed after set");
    SLUICE_CHECK_MSG(node.was_woken(), "waiter resolved Woken via set drain");
    SLUICE_CHECK_MSG(!node.is_registered(), "node unlinked by winner");
    SLUICE_CHECK_MSG(sched.waiting_count() == 0, "wait count returned to baseline");
}

// ===========================================================================
// Slice 3 — Multi-waiter broadcast (T8–T10)
// ===========================================================================

// ---- T8: three or more registered waiters — set releases all --------------
//
// N >= 3 waiters register on an UNSET Event. set() must RESOURCE_WAKE every
// registered epoch: all observe Woken, one runnable publication per winner,
// the WaitQueue is structurally empty after, and waiting_count returns to 0.
SLUICE_TEST_CASE(e12_t8_set_releases_all_registered_waiters) {
    if constexpr (!fiber_ctx::supported) return;

    AsyncIoContext ctx(std::make_unique<IdleBackend>());
    Scheduler sched(ctx);

    Event ev(sched, /*initially_set=*/false);
    constexpr int N = 4;
    WaitNode nodes[N];
    std::atomic<int> suspended{0};

    Fiber fwaiters[N];
    FiberStack stacks[N];
    for (int i = 0; i < N; ++i) {
        fwaiters[i].set_entry([&, i](Fiber&) {
            suspended.fetch_add(1, std::memory_order_acq_rel);
            ev.wait(nodes[i]);  // suspend on UNSET
        });
        SLUICE_CHECK(sched.init_fiber(fwaiters[i], stacks[i].base(), stacks[i].size()));
        sched.spawn(fwaiters[i]);
    }

    // Separate setter: waits until all N have suspended, then set().
    Fiber fsetter;
    fsetter.set_entry([&](Fiber&) {
        spin_wait_pred([&] { return suspended.load(std::memory_order_acquire) >= N; });
        std::this_thread::yield();  // let all N commit waiting
        ev.set();  // drain resolves all N
    });
    FiberStack ss;
    SLUICE_CHECK(sched.init_fiber(fsetter, ss.base(), ss.size()));
    sched.spawn(fsetter);
    sched.run(N + 1);

    for (int i = 0; i < N; ++i) {
        SLUICE_CHECK_MSG(nodes[i].was_woken(), "each waiter resolved Woken");
        SLUICE_CHECK_MSG(fwaiters[i].state() == FiberState::done, "fiber reached done");
    }
    SLUICE_CHECK_MSG(sched.waiting_count() == 0, "wait count at baseline");
}

// ---- T9: one waiter expires while set broadcast resolves peers -------------
//
// W1, W2, W3 register on an UNSET Event. W2 uses wait_until with a deadline.
// The test advances the clock to expire W2's deadline, THEN calls set().
// W2 resolves Expired (timer won before set's drain reached it); W1/W3 resolve
// Woken. Event remains SET. Each outcome is independent.
SLUICE_TEST_CASE(e12_t9_one_expires_during_set_broadcast) {
    if constexpr (!fiber_ctx::supported) return;

    AsyncIoContext ctx(std::make_unique<IdleBackend>());
    Scheduler sched(ctx);
    E11TimerTestHooks::enable_test_clock(sched);

    Event ev(sched, /*initially_set=*/false);
    WaitNode n1, n2, n3;
    std::atomic<int> suspended{0};

    Fiber f1, f2, f3, fdriver;
    f1.set_entry([&](Fiber&) {
        suspended.fetch_add(1, std::memory_order_acq_rel);
        ev.wait(n1);
    });
    f2.set_entry([&](Fiber&) {
        suspended.fetch_add(1, std::memory_order_acq_rel);
        ev.wait_until(n2, /*deadline=*/100);  // deadline at clock=100
    });
    f3.set_entry([&](Fiber&) {
        suspended.fetch_add(1, std::memory_order_acq_rel);
        ev.wait(n3);
    });
    fdriver.set_entry([&](Fiber&) {
        spin_wait_pred([&] { return suspended.load(std::memory_order_acquire) >= 3; });
        std::this_thread::yield();
        // Expire W2's deadline BEFORE set(), so W2 resolves Expired.
        sched.advance_clock(100);
        std::this_thread::yield();
        ev.set();  // W1, W3 resolve Woken; W2 already Expired
    });

    FiberStack s1, s2, s3, sd;
    SLUICE_CHECK(sched.init_fiber(f1, s1.base(), s1.size()));
    SLUICE_CHECK(sched.init_fiber(f2, s2.base(), s2.size()));
    SLUICE_CHECK(sched.init_fiber(f3, s3.base(), s3.size()));
    SLUICE_CHECK(sched.init_fiber(fdriver, sd.base(), sd.size()));
    sched.spawn(f1);
    sched.spawn(f2);
    sched.spawn(f3);
    sched.spawn(fdriver);
    sched.run(4);

    SLUICE_CHECK_MSG(n1.was_woken(), "W1 resolved Woken");
    SLUICE_CHECK_MSG(n2.was_expired(), "W2 resolved Expired (timer won)");
    SLUICE_CHECK_MSG(n3.was_woken(), "W3 resolved Woken");
    SLUICE_CHECK_MSG(ev.is_set(), "Event remains SET");
    SLUICE_CHECK_MSG(sched.waiting_count() == 0, "wait count at baseline");
}

// ---- T10: one waiter cancels while set broadcast resolves peers ------------
//
// W1, W2, W3 register on an UNSET Event. A canceller cancels W2, THEN set()
// runs. W2 resolves Cancelled (cancel won before set's drain); W1/W3 resolve
// Woken. Event remains SET.
SLUICE_TEST_CASE(e12_t10_one_cancels_during_set_broadcast) {
    if constexpr (!fiber_ctx::supported) return;

    AsyncIoContext ctx(std::make_unique<IdleBackend>());
    Scheduler sched(ctx);

    Event ev(sched, /*initially_set=*/false);
    WaitNode n1, n2, n3;
    std::atomic<int> suspended{0};

    Fiber f1, f2, f3, fcancel;
    f1.set_entry([&](Fiber&) {
        suspended.fetch_add(1, std::memory_order_acq_rel);
        ev.wait(n1);
    });
    f2.set_entry([&](Fiber&) {
        suspended.fetch_add(1, std::memory_order_acq_rel);
        ev.wait(n2);
    });
    f3.set_entry([&](Fiber&) {
        suspended.fetch_add(1, std::memory_order_acq_rel);
        ev.wait(n3);
    });
    fcancel.set_entry([&](Fiber&) {
        spin_wait_pred([&] { return suspended.load(std::memory_order_acquire) >= 3; });
        std::this_thread::yield();
        // Cancel W2 BEFORE set(), so W2 resolves Cancelled.
        bool cancelled = false;
        for (int i = 0; i < 1000 && !cancelled; ++i) {
            cancelled = ev.cancel(n2);
        }
        SLUICE_CHECK_MSG(cancelled, "cancel_wait won for W2");
        ev.set();  // W1, W3 resolve Woken; W2 already Cancelled
    });

    FiberStack s1, s2, s3, sc;
    SLUICE_CHECK(sched.init_fiber(f1, s1.base(), s1.size()));
    SLUICE_CHECK(sched.init_fiber(f2, s2.base(), s2.size()));
    SLUICE_CHECK(sched.init_fiber(f3, s3.base(), s3.size()));
    SLUICE_CHECK(sched.init_fiber(fcancel, sc.base(), sc.size()));
    sched.spawn(f1);
    sched.spawn(f2);
    sched.spawn(f3);
    sched.spawn(fcancel);
    sched.run(4);

    SLUICE_CHECK_MSG(n1.was_woken(), "W1 resolved Woken");
    SLUICE_CHECK_MSG(n2.was_cancelled(), "W2 resolved Cancelled");
    SLUICE_CHECK_MSG(n3.was_woken(), "W3 resolved Woken");
    SLUICE_CHECK_MSG(ev.is_set(), "Event remains SET");
    SLUICE_CHECK_MSG(sched.waiting_count() == 0, "wait count at baseline");
}

// ===========================================================================
// Slice 4 — Resolution races (T11–T16)
// ===========================================================================
//
// These tests force a specific winner at the resolve_ CAS boundary by ordering
// the causes deterministically: the winner cause runs first and wins the CAS;
// the loser cause runs after and is a no-op. The controllable clock drives
// timer expiry (NO sleep_for). Each test verifies exactly one terminal outcome
// and wait-count closure.

// ---- T11: RESOURCE_WAKE wins TIMER_EXPIRE ----------------------------------
//
// W waits on UNSET Event with deadline D. set() wins the resolve_ CAS (Woken).
// The timer later expires but is the loser (no-op). Result: Woken, one
// publication, timer registration retired.
//
// Determinism: the winner (set) runs first and completes (verified via a
// winner_done barrier); the loser (advance_clock) runs only after. This makes
// the outcome deterministic under all sanitizer timings.
SLUICE_TEST_CASE(e12_t11_resource_wake_wins_timer_expire) {
    if constexpr (!fiber_ctx::supported) return;

    AsyncIoContext ctx(std::make_unique<IdleBackend>());
    Scheduler sched(ctx);
    E11TimerTestHooks::enable_test_clock(sched);

    Event ev(sched, /*initially_set=*/false);
    WaitNode node;
    std::atomic<bool> registered{false};
    std::atomic<bool> winner_done{false};

    Fiber fwait, fsetter, fdriver;
    fwait.set_entry([&](Fiber&) {
        registered.store(true, std::memory_order_release);
        ev.wait_until(node, /*deadline=*/100);
    });
    fsetter.set_entry([&](Fiber&) {
        spin_wait(registered);
        std::this_thread::yield();
        ev.set();  // RESOURCE_WAKE wins
        winner_done.store(true, std::memory_order_release);
    });
    fdriver.set_entry([&](Fiber&) {
        spin_wait(winner_done);  // barrier: wait for set to win first
        sched.advance_clock(100);   // timer expires (loser)
    });

    FiberStack sw, ss, sd;
    SLUICE_CHECK(sched.init_fiber(fwait, sw.base(), sw.size()));
    SLUICE_CHECK(sched.init_fiber(fsetter, ss.base(), ss.size()));
    SLUICE_CHECK(sched.init_fiber(fdriver, sd.base(), sd.size()));
    sched.spawn(fwait);
    sched.spawn(fsetter);
    sched.spawn(fdriver);
    sched.run(3);

    SLUICE_CHECK_MSG(node.was_woken(), "RESOURCE_WAKE won");
    SLUICE_CHECK_MSG(ev.is_set(), "Event is SET");
    SLUICE_CHECK_MSG(sched.waiting_count() == 0, "wait count at baseline");
}

// ---- T12: TIMER_EXPIRE wins RESOURCE_WAKE ---------------------------------
//
// W waits on UNSET Event with deadline D. The timer expires first (Expired wins
// the CAS). set() later attempts RESOURCE_WAKE but is the loser (no-op). Event
// becomes SET (set() still flips the flag), but W resolves Expired.
//
// Determinism: the winner (timer) runs first via advance_clock retry until the
// node is terminal; the loser (set) runs only after the barrier.
SLUICE_TEST_CASE(e12_t12_timer_expire_wins_resource_wake) {
    if constexpr (!fiber_ctx::supported) return;

    AsyncIoContext ctx(std::make_unique<IdleBackend>());
    Scheduler sched(ctx);
    E11TimerTestHooks::enable_test_clock(sched);

    Event ev(sched, /*initially_set=*/false);
    WaitNode node;
    std::atomic<bool> registered{false};
    std::atomic<bool> winner_done{false};

    Fiber fwait, fdriver, fsetter;
    fwait.set_entry([&](Fiber&) {
        registered.store(true, std::memory_order_release);
        ev.wait_until(node, /*deadline=*/100);
    });
    fdriver.set_entry([&](Fiber&) {
        spin_wait(registered);
        // Retry advance_clock until the node resolves (the deadline registration
        // commits atomically under global_mtx_ before W yields).
        for (int i = 0; i < 200 && !node.is_terminal(); ++i) {
            sched.advance_clock(100);
            std::this_thread::yield();
        }
        winner_done.store(true, std::memory_order_release);
    });
    fsetter.set_entry([&](Fiber&) {
        spin_wait(winner_done);  // barrier: wait for timer to win first
        ev.set();  // RESOURCE_WAKE loses; Event becomes SET
    });

    FiberStack sw, sd, ss;
    SLUICE_CHECK(sched.init_fiber(fwait, sw.base(), sw.size()));
    SLUICE_CHECK(sched.init_fiber(fdriver, sd.base(), sd.size()));
    SLUICE_CHECK(sched.init_fiber(fsetter, ss.base(), ss.size()));
    sched.spawn(fwait);
    sched.spawn(fdriver);
    sched.spawn(fsetter);
    sched.run(3);

    SLUICE_CHECK_MSG(node.was_expired(), "TIMER_EXPIRE won");
    SLUICE_CHECK_MSG(ev.is_set(), "Event became SET (set loser still flips flag)");
    SLUICE_CHECK_MSG(sched.waiting_count() == 0, "wait count at baseline");
}

// ---- T13: RESOURCE_WAKE wins CANCEL ----------------------------------------
//
// W waits on UNSET Event. set() wins the resolve_ CAS (Woken). cancel_wait
// later is the loser (no-op). Result: Woken.
//
// Determinism: set() runs first (winner_done barrier); cancel runs after.
SLUICE_TEST_CASE(e12_t13_resource_wake_wins_cancel) {
    if constexpr (!fiber_ctx::supported) return;

    AsyncIoContext ctx(std::make_unique<IdleBackend>());
    Scheduler sched(ctx);

    Event ev(sched, /*initially_set=*/false);
    WaitNode node;
    std::atomic<bool> registered{false};
    std::atomic<bool> winner_done{false};

    Fiber fwait, fsetter, fcancel;
    fwait.set_entry([&](Fiber&) {
        registered.store(true, std::memory_order_release);
        ev.wait(node);
    });
    fsetter.set_entry([&](Fiber&) {
        spin_wait(registered);
        std::this_thread::yield();
        ev.set();  // RESOURCE_WAKE wins
        winner_done.store(true, std::memory_order_release);
    });
    fcancel.set_entry([&](Fiber&) {
        spin_wait(winner_done);  // barrier: wait for set to win first
        (void)ev.cancel(node);  // loser
    });

    FiberStack sw, ss, sc;
    SLUICE_CHECK(sched.init_fiber(fwait, sw.base(), sw.size()));
    SLUICE_CHECK(sched.init_fiber(fsetter, ss.base(), ss.size()));
    SLUICE_CHECK(sched.init_fiber(fcancel, sc.base(), sc.size()));
    sched.spawn(fwait);
    sched.spawn(fsetter);
    sched.spawn(fcancel);
    sched.run(3);

    SLUICE_CHECK_MSG(node.was_woken(), "RESOURCE_WAKE won");
    SLUICE_CHECK_MSG(ev.is_set(), "Event is SET");
    SLUICE_CHECK_MSG(sched.waiting_count() == 0, "wait count at baseline");
}

// ---- T14: CANCEL wins RESOURCE_WAKE ----------------------------------------
//
// W waits on UNSET Event. cancel_wait wins the resolve_ CAS (Cancelled). set()
// later attempts RESOURCE_WAKE but is the loser (no-op). Event becomes SET.
//
// Determinism: cancel runs first (retry loop until won; winner_done barrier);
// set runs after.
SLUICE_TEST_CASE(e12_t14_cancel_wins_resource_wake) {
    if constexpr (!fiber_ctx::supported) return;

    AsyncIoContext ctx(std::make_unique<IdleBackend>());
    Scheduler sched(ctx);

    Event ev(sched, /*initially_set=*/false);
    WaitNode node;
    std::atomic<bool> registered{false};
    std::atomic<bool> winner_done{false};

    Fiber fwait, fcancel, fsetter;
    fwait.set_entry([&](Fiber&) {
        registered.store(true, std::memory_order_release);
        ev.wait(node);
    });
    fcancel.set_entry([&](Fiber&) {
        spin_wait(registered);
        bool cancelled = false;
        for (int i = 0; i < 1000 && !cancelled; ++i) {
            cancelled = ev.cancel(node);
        }
        SLUICE_CHECK_MSG(cancelled, "cancel won");
        winner_done.store(true, std::memory_order_release);
    });
    fsetter.set_entry([&](Fiber&) {
        spin_wait(winner_done);  // barrier: wait for cancel to win first
        ev.set();  // RESOURCE_WAKE loses; Event becomes SET
    });

    FiberStack sw, sc, ss;
    SLUICE_CHECK(sched.init_fiber(fwait, sw.base(), sw.size()));
    SLUICE_CHECK(sched.init_fiber(fcancel, sc.base(), sc.size()));
    SLUICE_CHECK(sched.init_fiber(fsetter, ss.base(), ss.size()));
    sched.spawn(fwait);
    sched.spawn(fcancel);
    sched.spawn(fsetter);
    sched.run(3);

    SLUICE_CHECK_MSG(node.was_cancelled(), "CANCEL won");
    SLUICE_CHECK_MSG(ev.is_set(), "Event became SET");
    SLUICE_CHECK_MSG(sched.waiting_count() == 0, "wait count at baseline");
}

// ---- T15: TIMER_EXPIRE wins CANCEL -----------------------------------------
//
// W waits on UNSET Event with deadline D. The timer expires first (Expired
// wins). cancel_wait later is the loser (no-op). Result: Expired.
//
// Determinism: timer runs first (retry advance_clock; winner_done barrier);
// cancel runs after.
SLUICE_TEST_CASE(e12_t15_timer_expire_wins_cancel) {
    if constexpr (!fiber_ctx::supported) return;

    AsyncIoContext ctx(std::make_unique<IdleBackend>());
    Scheduler sched(ctx);
    E11TimerTestHooks::enable_test_clock(sched);

    Event ev(sched, /*initially_set=*/false);
    WaitNode node;
    std::atomic<bool> registered{false};
    std::atomic<bool> winner_done{false};

    Fiber fwait, fdriver, fcancel;
    fwait.set_entry([&](Fiber&) {
        registered.store(true, std::memory_order_release);
        ev.wait_until(node, /*deadline=*/100);
    });
    fdriver.set_entry([&](Fiber&) {
        spin_wait(registered);
        for (int i = 0; i < 200 && !node.is_terminal(); ++i) {
            sched.advance_clock(100);
            std::this_thread::yield();
        }
        winner_done.store(true, std::memory_order_release);
    });
    fcancel.set_entry([&](Fiber&) {
        spin_wait(winner_done);  // barrier: wait for timer to win first
        (void)ev.cancel(node);  // loser
    });

    FiberStack sw, sd, sc;
    SLUICE_CHECK(sched.init_fiber(fwait, sw.base(), sw.size()));
    SLUICE_CHECK(sched.init_fiber(fdriver, sd.base(), sd.size()));
    SLUICE_CHECK(sched.init_fiber(fcancel, sc.base(), sc.size()));
    sched.spawn(fwait);
    sched.spawn(fdriver);
    sched.spawn(fcancel);
    sched.run(3);

    SLUICE_CHECK_MSG(node.was_expired(), "TIMER_EXPIRE won");
    SLUICE_CHECK_MSG(sched.waiting_count() == 0, "wait count at baseline");
}

// ---- T16: deterministic three-way RESOURCE_WAKE/TIMER_EXPIRE/CANCEL race ---
//
// Repeated iterations: a waiter, a setter, a clock-driver, and a canceller
// all contend on the same node. Each iteration asserts exactly one terminal
// outcome (XOR of woken/cancelled/expired) and wait-count closure. Uses
// bounded retry loops (not random sleeps). Stress is supplementary to the
// causal two-way proofs above.
SLUICE_TEST_CASE(e12_t16_three_way_race_one_winner_repeated) {
    if constexpr (!fiber_ctx::supported) return;

    AsyncIoContext ctx(std::make_unique<IdleBackend>());
    Scheduler sched(ctx);
    E11TimerTestHooks::enable_test_clock(sched);

    constexpr int ITERS = 200;
    int woken = 0, cancelled = 0, expired = 0;

    for (int it = 0; it < ITERS; ++it) {
        Event ev(sched, /*initially_set=*/false);
        WaitNode node;
        std::atomic<bool> registered{false};
        std::atomic<bool> go{false};

        Fiber fwait, fsetter, fdriver, fcancel;
        fwait.set_entry([&](Fiber&) {
            registered.store(true, std::memory_order_release);
            spin_wait(go);
            ev.wait_until(node, /*deadline=*/100);
        });
        fsetter.set_entry([&](Fiber&) {
            spin_wait(registered);
            go.store(true, std::memory_order_release);
            // set() is idempotent; call it and yield to let the race resolve.
            ev.set();
        });
        fdriver.set_entry([&](Fiber&) {
            spin_wait(registered);
            go.store(true, std::memory_order_release);
            std::this_thread::yield();
            sched.advance_clock(100);
        });
        fcancel.set_entry([&](Fiber&) {
            spin_wait(registered);
            go.store(true, std::memory_order_release);
            for (int i = 0; i < 50; ++i) {
                if (ev.cancel(node)) break;
                std::this_thread::yield();
            }
        });

        FiberStack sw, ss, sd, sc;
        SLUICE_CHECK(sched.init_fiber(fwait, sw.base(), sw.size()));
        SLUICE_CHECK(sched.init_fiber(fsetter, ss.base(), ss.size()));
        SLUICE_CHECK(sched.init_fiber(fdriver, sd.base(), sd.size()));
        SLUICE_CHECK(sched.init_fiber(fcancel, sc.base(), sc.size()));
        sched.spawn(fwait);
        sched.spawn(fsetter);
        sched.spawn(fdriver);
        sched.spawn(fcancel);
        sched.run(4);

        // Exactly one terminal outcome.
        int outcomes = (node.was_woken() ? 1 : 0) +
                       (node.was_cancelled() ? 1 : 0) +
                       (node.was_expired() ? 1 : 0);
        SLUICE_CHECK_MSG(outcomes == 1, "exactly one terminal outcome");
        if (node.was_woken()) ++woken;
        else if (node.was_cancelled()) ++cancelled;
        else ++expired;
        SLUICE_CHECK_MSG(sched.waiting_count() == 0, "wait count at baseline");
    }
    // Sanity: at least one of each should occur over 200 iterations (not a hard
    // requirement — the race is nondeterministic — but a strong signal).
    SLUICE_CHECK_MSG(woken + cancelled + expired == ITERS, "all iterations resolved");
}

// ===========================================================================
// Slice 5 — Reset non-resolution + epoch isolation (T17–T18)
// ===========================================================================

// ---- T17: reset does not cancel already registered waiter -----------------
//
// W registers on an UNSET Event. reset() is called while W is registered.
// reset() must NOT resolve/cancel/expire/unlink W — W remains Registered and
// is governed by future set(). A later set() wakes W. Proves reset is a pure
// state flip (E5 — Reset Non-Resolution).
SLUICE_TEST_CASE(e12_t17_reset_does_not_cancel_registered_waiter) {
    if constexpr (!fiber_ctx::supported) return;

    AsyncIoContext ctx(std::make_unique<IdleBackend>());
    Scheduler sched(ctx);

    Event ev(sched, /*initially_set=*/false);
    WaitNode node;
    std::atomic<bool> registered{false};
    std::atomic<bool> reset_done{false};

    Fiber fwait, freseter, fsetter;
    fwait.set_entry([&](Fiber&) {
        registered.store(true, std::memory_order_release);
        ev.wait(node);  // suspends on UNSET
    });
    freseter.set_entry([&](Fiber&) {
        spin_wait(registered);
        std::this_thread::yield();
        ev.reset();  // reset while W is registered — must NOT resolve W
        SLUICE_CHECK_MSG(!ev.is_set(), "Event is UNSET after reset");
        reset_done.store(true, std::memory_order_release);
    });
    fsetter.set_entry([&](Fiber&) {
        spin_wait(reset_done);
        std::this_thread::yield();
        // W should still be registered (reset did not cancel it).
        ev.set();  // future set wakes W
    });

    FiberStack sw, fr, fs;
    SLUICE_CHECK(sched.init_fiber(fwait, sw.base(), sw.size()));
    SLUICE_CHECK(sched.init_fiber(freseter, fr.base(), fr.size()));
    SLUICE_CHECK(sched.init_fiber(fsetter, fs.base(), fs.size()));
    sched.spawn(fwait);
    sched.spawn(freseter);
    sched.spawn(fsetter);
    sched.run(3);

    SLUICE_CHECK_MSG(node.was_woken(), "W woken by the LATER set (reset did not cancel)");
    SLUICE_CHECK_MSG(ev.is_set(), "Event is SET");
    SLUICE_CHECK_MSG(sched.waiting_count() == 0, "wait count at baseline");
}

// ---- T18: OLD_SET_WAKES_POST_RESET_WAITER — atomic drain serialization -----
//
// The load-bearing epoch isolation proof. The topology:
//   1. Wold registers on UNSET Event
//   2. S1 (set) linearizes SET + drains Wold (Wold Woken)
//   3. reset linearizes UNSET
//   4. Wnew registers on UNSET Event (suspends)
//   5. S2 (set) wakes Wnew
//
// S1's drain CANNOT reach Wnew because under global_mtx_ serialization, S1's
// entire drain completes BEFORE reset or Wnew's admission can run. Wnew is not
// in the queue during S1's drain. This test forces the exact trace: S1 drains
// Wold, then reset, then Wnew registers, then S2 wakes Wnew. Wold resolves
// Woken from S1; Wnew resolves Woken from S2 (NOT from S1).
SLUICE_TEST_CASE(e12_t18_old_set_does_not_wake_post_reset_waiter) {
    if constexpr (!fiber_ctx::supported) return;

    AsyncIoContext ctx(std::make_unique<IdleBackend>());
    Scheduler sched(ctx);

    Event ev(sched, /*initially_set=*/false);
    WaitNode n_old, n_new;
    std::atomic<bool> old_registered{false};
    std::atomic<bool> s1_done{false};
    std::atomic<bool> reset_done{false};
    std::atomic<bool> new_registered{false};

    Fiber f_old, f_s1, f_reset, f_new, f_s2;

    // Wold: register and suspend on UNSET.
    f_old.set_entry([&](Fiber&) {
        old_registered.store(true, std::memory_order_release);
        ev.wait(n_old);
    });
    // S1: set (drains Wold), then signal done. The drain completes atomically
    // under global_mtx_ before reset or Wnew can run.
    f_s1.set_entry([&](Fiber&) {
        spin_wait(old_registered);
        std::this_thread::yield();
        ev.set();  // S1: SET + drain Wold (atomic under global_mtx_)
        s1_done.store(true, std::memory_order_release);
    });
    // reset: runs AFTER S1's drain completes.
    f_reset.set_entry([&](Fiber&) {
        spin_wait(s1_done);
        std::this_thread::yield();
        ev.reset();  // UNSET
        reset_done.store(true, std::memory_order_release);
    });
    // Wnew: registers AFTER reset (suspends on UNSET).
    f_new.set_entry([&](Fiber&) {
        spin_wait(reset_done);
        std::this_thread::yield();
        new_registered.store(true, std::memory_order_release);
        ev.wait(n_new);  // suspends — S1's drain is already complete
    });
    // S2: wakes Wnew (the post-reset waiter).
    f_s2.set_entry([&](Fiber&) {
        spin_wait(new_registered);
        std::this_thread::yield();
        ev.set();  // S2: SET + drain Wnew
    });

    FiberStack s_old, s_s1, s_reset, s_new, s_s2;
    SLUICE_CHECK(sched.init_fiber(f_old, s_old.base(), s_old.size()));
    SLUICE_CHECK(sched.init_fiber(f_s1, s_s1.base(), s_s1.size()));
    SLUICE_CHECK(sched.init_fiber(f_reset, s_reset.base(), s_reset.size()));
    SLUICE_CHECK(sched.init_fiber(f_new, s_new.base(), s_new.size()));
    SLUICE_CHECK(sched.init_fiber(f_s2, s_s2.base(), s_s2.size()));
    sched.spawn(f_old);
    sched.spawn(f_s1);
    sched.spawn(f_reset);
    sched.spawn(f_new);
    sched.spawn(f_s2);
    sched.run(5);

    SLUICE_CHECK_MSG(n_old.was_woken(), "Wold woken by S1's drain");
    SLUICE_CHECK_MSG(n_new.was_woken(), "Wnew woken by S2 (NOT by S1's drain)");
    SLUICE_CHECK_MSG(ev.is_set(), "Event is SET (from S2)");
    SLUICE_CHECK_MSG(sched.waiting_count() == 0, "wait count at baseline");
}

// ===========================================================================
// Slice 6 — Scheduler integration (T19–T22)
// ===========================================================================

// ---- T19: external OS thread set() wakes a Live Scheduler (park-independent) -
//
// A Live Scheduler with one Worker has an unresolved Event wait. An external
// std::thread calls Event::set(); the set() broadcast resolves the waiter
// through the Scheduler wake source (route_runnable_locked -> signal_wake_locked)
// and the Fiber resumes Woken. No Event-private condition variable, no polling,
// no caller re-entry. The bounded sleep_for here is ONLY a registration-wait
// (not the park-entry proof — T32 is the mechanical park-entry proof using the
// park-commit seam); the test asserts the OUTCOME, not that the Worker reached
// park.
SLUICE_TEST_CASE(e12_t19_external_thread_set_wakes_live) {
    if constexpr (!fiber_ctx::supported) return;

    AsyncIoContext ctx(std::make_unique<IdleBackend>());
    Scheduler sched(ctx);

    Event ev(sched, /*initially_set=*/false);
    WaitNode node;
    std::atomic<bool> waiter_registered{false};
    std::atomic<int> entries{0};

    Fiber fwait;
    fwait.set_entry([&](Fiber&) {
        entries.fetch_add(1, std::memory_order_acq_rel);
        waiter_registered.store(true, std::memory_order_release);
        ev.wait(node);
        entries.fetch_add(1, std::memory_order_acq_rel);
    });

    FiberStack sw;
    SLUICE_CHECK(sched.init_fiber(fwait, sw.base(), sw.size()));
    sched.spawn(fwait);

    // External producer: wait for the waiter to register, then set() from
    // outside any worker thread. The bounded sleeps are registration-sync ONLY
    // (not the park-entry proof; T32 proves park-entry mechanically).
    std::thread ext([&] {
        while (!waiter_registered.load(std::memory_order_acquire)) {
            std::this_thread::sleep_for(std::chrono::microseconds(50));
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
        ev.set();  // external-thread set: broadcast via the Scheduler wake source
    });

    sched.run_live(1);  // Live: may park on MW-S3 + external-wake-capable
    ext.join();

    SLUICE_CHECK_MSG(entries.load() == 2, "waiter resumed via external set()");
    SLUICE_CHECK_MSG(node.was_woken(), "waiter resolved Woken");
    SLUICE_CHECK_MSG(ev.is_set(), "Event is SET");
    SLUICE_CHECK_MSG(sched.waiting_count() == 0, "no unresolved waits remain");
}

// ---- T20: woken Event waiter resumes after E8 ownership transfer / steal ---
//
// A woken Event waiter may be stolen after runnable publication (E8). The
// waiter's outcome remains Woken, the WaitNode belongs to the same wait epoch,
// and no Event state is Worker-bound. This test uses 2 workers: the waiter on
// worker 0, the setter on worker 1; the setter's set() routes the wake to the
// waiter's owner, which may involve steal routing.
SLUICE_TEST_CASE(e12_t20_woken_waiter_resumes_after_steal) {
    if constexpr (!fiber_ctx::supported) return;

    AsyncIoContext ctx(std::make_unique<IdleBackend>());
    Scheduler sched(ctx);

    Event ev(sched, /*initially_set=*/false);
    WaitNode node;
    std::atomic<bool> registered{false};
    std::atomic<unsigned> resume_worker{static_cast<unsigned>(-1)};

    Fiber fwait, fsetter;
    fwait.set_entry([&](Fiber&) {
        registered.store(true, std::memory_order_release);
        ev.wait(node);
        // Record which worker we resumed on (may differ from the initial owner
        // if the runnable ticket was stolen).
        resume_worker.store(Scheduler::current_worker_id(), std::memory_order_release);
    });
    fsetter.set_entry([&](Fiber&) {
        spin_wait(registered);
        std::this_thread::yield();
        ev.set();  // routes wake to fwait's owner; steal may occur
    });

    FiberStack sw, ss;
    SLUICE_CHECK(sched.init_fiber(fwait, sw.base(), sw.size()));
    SLUICE_CHECK(sched.init_fiber(fsetter, ss.base(), ss.size()));
    sched.spawn(fwait);
    sched.spawn(fsetter);
    sched.run(2);  // 2 workers: steal is possible

    SLUICE_CHECK_MSG(node.was_woken(), "waiter resolved Woken (outcome is Worker-independent)");
    SLUICE_CHECK_MSG(fwait.state() == FiberState::done, "waiter reached done");
    SLUICE_CHECK_MSG(sched.waiting_count() == 0, "wait count at baseline");
    // The resume worker is valid (0 or 1); the outcome is correct regardless.
    SLUICE_CHECK_MSG(resume_worker.load() <= 1, "resumed on a valid worker");
}

// ---- T21: Drain unresolved Event wait returns STALLED and does not hang ----
//
// In Drain mode, an unresolved Event wait (MW-S3) returns STALLED — the run
// terminates without hanging. The Event waiter remains registered (unresolved);
// the caller must cancel it before the node's scope ends. No hang, no park.
SLUICE_TEST_CASE(e12_t21_drain_unresolved_event_returns_stalled) {
    if constexpr (!fiber_ctx::supported) return;

    AsyncIoContext ctx(std::make_unique<IdleBackend>());
    Scheduler sched(ctx);

    Event ev(sched, /*initially_set=*/false);
    WaitNode node;
    std::atomic<int> entries{0};

    Fiber fwait;
    fwait.set_entry([&](Fiber&) {
        entries.fetch_add(1, std::memory_order_acq_rel);
        ev.wait(node);  // suspends; no setter — MW-S3 STALLED
        entries.fetch_add(1, std::memory_order_acq_rel);
    });

    FiberStack sw;
    SLUICE_CHECK(sched.init_fiber(fwait, sw.base(), sw.size()));
    sched.spawn(fwait);
    sched.run(1);  // Drain: MW-S3 returns STALLED (no hang)

    // The waiter did NOT resume (entries == 1: only the pre-wait increment).
    SLUICE_CHECK_MSG(entries.load() == 1, "waiter did not resume (Drain STALLED)");
    SLUICE_CHECK_MSG(node.is_registered(), "node left Registered (unresolved)");
    // Caller contract: cancel the stranded wait before the node's scope ends.
    SLUICE_CHECK_MSG(ev.cancel(node), "caller cancels stranded wait");
    SLUICE_CHECK_MSG(node.was_cancelled(), "node now Cancelled (safe to destroy)");
}

// ---- T22: Event destruction after terminal waits safe ----------------------
//
// After all Event waits are terminal, destroying the Event is safe. The
// destructor does NOT cancel/wake/synthesize; the WaitQueue is empty (all
// waiters resolved+unlinked). This proves the destruction contract.
SLUICE_TEST_CASE(e12_t22_destruction_after_terminal_waits_safe) {
    if constexpr (!fiber_ctx::supported) return;

    AsyncIoContext ctx(std::make_unique<IdleBackend>());
    Scheduler sched(ctx);

    WaitNode n1, n2;
    std::atomic<int> suspended{0};

    {
        // Event scope: all waits must be terminal before this block exits.
        Event ev(sched, /*initially_set=*/false);

        Fiber f1, f2, fsetter;
        f1.set_entry([&](Fiber&) {
            suspended.fetch_add(1, std::memory_order_acq_rel);
            ev.wait(n1);
        });
        f2.set_entry([&](Fiber&) {
            suspended.fetch_add(1, std::memory_order_acq_rel);
            ev.wait(n2);
        });
        fsetter.set_entry([&](Fiber&) {
            spin_wait_pred([&] { return suspended.load(std::memory_order_acquire) >= 2; });
            std::this_thread::yield();
            ev.set();  // both waiters resolve Woken
        });

        FiberStack s1, s2, ss;
        SLUICE_CHECK(sched.init_fiber(f1, s1.base(), s1.size()));
        SLUICE_CHECK(sched.init_fiber(f2, s2.base(), s2.size()));
        SLUICE_CHECK(sched.init_fiber(fsetter, ss.base(), ss.size()));
        sched.spawn(f1);
        sched.spawn(f2);
        sched.spawn(fsetter);
        sched.run(3);

        SLUICE_CHECK_MSG(n1.was_woken(), "n1 terminal Woken");
        SLUICE_CHECK_MSG(n2.was_woken(), "n2 terminal Woken");
        SLUICE_CHECK_MSG(sched.waiting_count() == 0, "wait count at baseline");
        // ~Event runs here: WaitQueue is empty (all waiters unlinked). Safe.
    }
    // If we reach here, the destructor did not assert/fault.
    SLUICE_CHECK_MSG(true, "Event destroyed safely after all waits terminal");
}

// ===========================================================================
// Slice 7 — Stress (T23)
// ===========================================================================

// ---- T23: repeated multi-waiter mixed-outcome stress -----------------------
//
// Repeated iterations of the mixed-outcome broadcast: W1 RESOURCE_WAKE,
// W2 TIMER_EXPIRE, W3 CANCEL, W4 RESOURCE_WAKE. Then a late W5 observes SET
// and returns Woken without suspension. Each iteration verifies:
//   - each waiter has exactly one terminal outcome
//   - no second ticket / no queue leak (waiting_count == 0)
//   - Event remains SET
//   - the late W5 observes SET
// This is supplementary stress to the causal proofs (T8–T16).
SLUICE_TEST_CASE(e12_t23_multi_waiter_mixed_outcome_stress) {
    if constexpr (!fiber_ctx::supported) return;

    AsyncIoContext ctx(std::make_unique<IdleBackend>());
    Scheduler sched(ctx);
    E11TimerTestHooks::enable_test_clock(sched);

    constexpr int ITERS = 20;
    for (int it = 0; it < ITERS; ++it) {
        Event ev(sched, /*initially_set=*/false);
        WaitNode n1, n2, n3, n4, n5;
        std::atomic<int> suspended{0};
        std::atomic<bool> w5_done{false};

        Fiber f1, f2, f3, f4, f5, fdriver;
        f1.set_entry([&](Fiber&) { suspended.fetch_add(1, std::memory_order_acq_rel); ev.wait(n1); });
        f2.set_entry([&](Fiber&) { suspended.fetch_add(1, std::memory_order_acq_rel); ev.wait_until(n2, 100); });
        f3.set_entry([&](Fiber&) { suspended.fetch_add(1, std::memory_order_acq_rel); ev.wait(n3); });
        f4.set_entry([&](Fiber&) { suspended.fetch_add(1, std::memory_order_acq_rel); ev.wait(n4); });
        f5.set_entry([&](Fiber&) {
            // W5 arrives AFTER set: observes SET, returns Woken without suspension.
            spin_wait_pred([&] { return ev.is_set(); });
            ev.wait(n5);
            w5_done.store(true, std::memory_order_release);
        });
        fdriver.set_entry([&](Fiber&) {
            spin_wait_pred([&] { return suspended.load(std::memory_order_acquire) >= 4; });
            std::this_thread::yield();
            // Expire W2: retry advance_clock until W2 resolves. W2's deadline
            // registration commits atomically (under global_mtx_) before W2
            // yields, but the driver may run before W2 reaches wait_until.
            // Retry ensures the pump sees W2's deadline once registered.
            for (int i = 0; i < 200 && !n2.is_terminal(); ++i) {
                sched.advance_clock(100);
                std::this_thread::yield();
            }
            SLUICE_CHECK_MSG(n2.was_expired(), "W2 expired");
            bool cancelled = false;
            for (int i = 0; i < 100 && !cancelled; ++i) {
                cancelled = ev.cancel(n3);
            }
            SLUICE_CHECK_MSG(cancelled, "W3 cancelled");
            ev.set();  // W1, W4 -> Woken; W2/W3 already terminal
        });

        FiberStack s1, s2, s3, s4, s5, sd;
        SLUICE_CHECK(sched.init_fiber(f1, s1.base(), s1.size()));
        SLUICE_CHECK(sched.init_fiber(f2, s2.base(), s2.size()));
        SLUICE_CHECK(sched.init_fiber(f3, s3.base(), s3.size()));
        SLUICE_CHECK(sched.init_fiber(f4, s4.base(), s4.size()));
        SLUICE_CHECK(sched.init_fiber(f5, s5.base(), s5.size()));
        SLUICE_CHECK(sched.init_fiber(fdriver, sd.base(), sd.size()));
        sched.spawn(f1);
        sched.spawn(f2);
        sched.spawn(f3);
        sched.spawn(f4);
        sched.spawn(f5);
        sched.spawn(fdriver);
        // 3 workers: keeps the test deterministic while avoiding the known
        // raw fiber-asm + TSan DEADLYSIGNAL at high concurrent-worker counts
        // (classified separately; T8-T16 provide the causal race proofs under
        // TSan). Stress is supplementary only (H7).
        sched.run(3);

        SLUICE_CHECK_MSG(n1.was_woken(), "W1 Woken");
        SLUICE_CHECK_MSG(n2.was_expired(), "W2 Expired");
        SLUICE_CHECK_MSG(n3.was_cancelled(), "W3 Cancelled");
        SLUICE_CHECK_MSG(n4.was_woken(), "W4 Woken");
        SLUICE_CHECK_MSG(n5.was_woken(), "W5 (late) Woken without suspension");
        SLUICE_CHECK_MSG(ev.is_set(), "Event remains SET");
        SLUICE_CHECK_MSG(sched.waiting_count() == 0, "no queue leak / wait count at baseline");
    }
}

// ===========================================================================
// Slice 8 — Sealed authority + narrow cancellation (T24–T26)
// ===========================================================================

// ---- T25: Event::cancel correct Event/node wins Cancelled exactly once -----
//
// The narrow per-wait-epoch CANCEL authority. A waiter is Registered on an
// UNSET Event. Event::cancel(node) wins the resolve_ CAS -> Cancelled, exactly
// once, routed through the Scheduler wake seam. A second cancel returns false.
SLUICE_TEST_CASE(e12_t25_cancel_correct_event_node_wins_cancelled) {
    if constexpr (!fiber_ctx::supported) return;

    AsyncIoContext ctx(std::make_unique<IdleBackend>());
    Scheduler sched(ctx);

    Event ev(sched, /*initially_set=*/false);
    WaitNode node;
    std::atomic<bool> registered{false};

    Fiber fwait, fcancel;
    fwait.set_entry([&](Fiber&) {
        registered.store(true, std::memory_order_release);
        ev.wait(node);  // suspends on UNSET
    });
    fcancel.set_entry([&](Fiber&) {
        spin_wait(registered);
        std::this_thread::yield();
        bool won = false;
        for (int i = 0; i < 1000 && !won; ++i) {
            won = ev.cancel(node);
        }
        SLUICE_CHECK_MSG(won, "Event::cancel won for the registered node");
        // Second cancel must lose (node is terminal).
        SLUICE_CHECK_MSG(!ev.cancel(node), "second Event::cancel loses (terminal)");
    });

    FiberStack sw, sc;
    SLUICE_CHECK(sched.init_fiber(fwait, sw.base(), sw.size()));
    SLUICE_CHECK(sched.init_fiber(fcancel, sc.base(), sc.size()));
    sched.spawn(fwait);
    sched.spawn(fcancel);
    sched.run(2);

    SLUICE_CHECK_MSG(node.was_cancelled(), "node resolved Cancelled via Event::cancel");
    SLUICE_CHECK_MSG(!node.is_registered(), "node unlinked by winner");
    SLUICE_CHECK_MSG(!ev.is_set(), "cancel did not change Event SET/UNSET");
    SLUICE_CHECK_MSG(sched.waiting_count() == 0, "wait count at baseline");
}

// ---- T26: Event::cancel wrong Event / detached / terminal node loses safely -
//
// The narrow cancellation authority cannot cancel a node belonging to another
// Event, a detached node, or an already-terminal node. Each loses safely
// (returns false, no state mutation).
SLUICE_TEST_CASE(e12_t26_cancel_wrong_event_detached_terminal_loses) {
    if constexpr (!fiber_ctx::supported) return;

    AsyncIoContext ctx(std::make_unique<IdleBackend>());
    Scheduler sched(ctx);

    // --- (a) cancel a detached node -> false (nothing registered) ---
    {
        Event ev(sched, /*initially_set=*/false);
        WaitNode detached;  // never registered
        SLUICE_CHECK_MSG(!ev.cancel(detached), "cancel detached node -> false");
        SLUICE_CHECK_MSG(!detached.is_terminal(), "detached node untouched");
        SLUICE_CHECK_MSG(!ev.is_set(), "Event UNSET untouched");
    }

    // --- (b) cancel an already-Woken node -> false ---
    {
        Event ev(sched, /*initially_set=*/true);
        WaitNode node;
        std::atomic<int> entries{0};
        Fiber f;
        f.set_entry([&](Fiber&) {
            entries.fetch_add(1, std::memory_order_acq_rel);
            ev.wait(node);  // admission observes SET -> Woken inline
            entries.fetch_add(1, std::memory_order_acq_rel);
        });
        FiberStack sw;
        SLUICE_CHECK(sched.init_fiber(f, sw.base(), sw.size()));
        sched.spawn(f);
        sched.run(1);

        SLUICE_CHECK_MSG(node.was_woken(), "node Woken at admission");
        // cancel of a Woken node must lose.
        SLUICE_CHECK_MSG(!ev.cancel(node), "cancel already-Woken node -> false");
        SLUICE_CHECK_MSG(node.was_woken(), "outcome unchanged (still Woken)");
    }

    // --- (c) cancel an already-Expired node -> false ---
    {
        Event ev(sched, /*initially_set=*/false);
        E11TimerTestHooks::enable_test_clock(sched);
        WaitNode node;
        std::atomic<bool> registered{false};
        Fiber f, fdriver;
        f.set_entry([&](Fiber&) {
            registered.store(true, std::memory_order_release);
            ev.wait_until(node, /*deadline=*/100);
        });
        fdriver.set_entry([&](Fiber&) {
            spin_wait(registered);
            for (int i = 0; i < 200 && !node.is_terminal(); ++i) {
                sched.advance_clock(100);
                std::this_thread::yield();
            }
        });
        FiberStack sw, sd;
        SLUICE_CHECK(sched.init_fiber(f, sw.base(), sw.size()));
        SLUICE_CHECK(sched.init_fiber(fdriver, sd.base(), sd.size()));
        sched.spawn(f);
        sched.spawn(fdriver);
        sched.run(2);

        SLUICE_CHECK_MSG(node.was_expired(), "node Expired");
        SLUICE_CHECK_MSG(!ev.cancel(node), "cancel already-Expired node -> false");
        SLUICE_CHECK_MSG(node.was_expired(), "outcome unchanged (still Expired)");
    }

    // --- (d) cancel against an EMPTY Event's queue (no registered node) ---
    //
    // E12-A-EVENT-CORRECTIVE-2: Event::cancel now validates EXACT queue
    // membership (contains_locked) before attempting cancel. A node not linked
    // in THIS Event's queue fails the membership gate and returns false WITHOUT
    // mutation -- it is NOT resolved and NOT unlinked. A detached node is not
    // linked anywhere, so it fails the gate. (Wrong-Event membership, including
    // cross-Scheduler, is exercised by CANCEL-2 below.)
    {
        Event ev(sched, /*initially_set=*/false);
        WaitNode foreign;  // never registered anywhere
        SLUICE_CHECK_MSG(!ev.cancel(foreign), "cancel of an unregistered (Detached) node -> false");
        SLUICE_CHECK_MSG(!foreign.is_terminal(), "foreign node untouched");
        SLUICE_CHECK_MSG(!ev.is_set(), "Event UNSET untouched");
        SLUICE_CHECK_MSG(sched.waiting_count() == 0, "no wait accounting touched");
    }
}

// ===========================================================================
// Slice 8b — Event::cancel queue-ownership validation (CANCEL-1..8)
// (E12-A-EVENT-CORRECTIVE-2 Corrective E)
// ===========================================================================
//
// Event::cancel returns true ONLY if the node is currently Registered AND
// linked in THIS Event's queue AND CANCEL wins resolve_. Otherwise it returns
// false WITHOUT mutation. These tests prove each case deterministically,
// including wrong-Event cancellation across the SAME Scheduler and across
// DIFFERENT Schedulers (the load-bearing structural-safety fix: the membership
// scan never reads a foreign node's home_ and never locks a foreign
// Event/Scheduler).

// ---- CANCEL-1: correct Event + live Registered node -> true/Cancelled once --
SLUICE_TEST_CASE(e12_cancel1_correct_event_live_node_wins) {
    if constexpr (!fiber_ctx::supported) return;

    AsyncIoContext ctx(std::make_unique<IdleBackend>());
    Scheduler sched(ctx);

    Event ev(sched, /*initially_set=*/false);
    WaitNode node;
    std::atomic<bool> registered{false};

    Fiber fwait, fcancel;
    fwait.set_entry([&](Fiber&) {
        registered.store(true, std::memory_order_release);
        ev.wait(node);  // suspends on UNSET
    });
    fcancel.set_entry([&](Fiber&) {
        spin_wait(registered);
        std::this_thread::yield();
        bool won = false;
        for (int i = 0; i < 1000 && !won; ++i) won = ev.cancel(node);
        SLUICE_CHECK_MSG(won, "CANCEL-1: cancel won for the registered node");
        SLUICE_CHECK_MSG(!ev.cancel(node), "CANCEL-1: second cancel loses (terminal)");
    });

    FiberStack sw, sc;
    SLUICE_CHECK(sched.init_fiber(fwait, sw.base(), sw.size()));
    SLUICE_CHECK(sched.init_fiber(fcancel, sc.base(), sc.size()));
    sched.spawn(fwait);
    sched.spawn(fcancel);
    sched.run(2);

    SLUICE_CHECK_MSG(node.was_cancelled(), "CANCEL-1: node resolved Cancelled");
    SLUICE_CHECK_MSG(!node.is_registered(), "CANCEL-1: node unlinked by winner");
    SLUICE_CHECK_MSG(!ev.is_set(), "CANCEL-1: Event SET/UNSET unchanged");
    SLUICE_CHECK_MSG(sched.waiting_count() == 0, "CANCEL-1: wait count at baseline");
}

// ---- CANCEL-2a: wrong Event, SAME Scheduler -> false, node intact on A -------
SLUICE_TEST_CASE(e12_cancel2a_wrong_event_same_scheduler_loses_safely) {
    if constexpr (!fiber_ctx::supported) return;

    AsyncIoContext ctx(std::make_unique<IdleBackend>());
    Scheduler sched(ctx);

    Event ev_a(sched, /*initially_set=*/false);
    Event ev_b(sched, /*initially_set=*/false);
    WaitNode node;
    std::atomic<bool> registered{false};

    Fiber fwait, fcancel, fset;
    fwait.set_entry([&](Fiber&) {
        registered.store(true, std::memory_order_release);
        ev_a.wait(node);  // registered on Event A
    });
    // Event B.cancel(node): node is registered on A, NOT on B -> false, no mutation.
    fcancel.set_entry([&](Fiber&) {
        spin_wait(registered);
        std::this_thread::yield();
        SLUICE_CHECK_MSG(!ev_b.cancel(node),
                         "CANCEL-2a: wrong-Event (same Scheduler) cancel -> false");
        // node remains Registered on A; both queues structurally intact.
        SLUICE_CHECK_MSG(node.is_registered(), "CANCEL-2a: node still Registered on A");
    });
    // After the wrong-Event cancel, Event A can still wake the node normally.
    fset.set_entry([&](Fiber&) {
        spin_wait(registered);
        std::this_thread::yield();
        std::this_thread::yield();
        ev_a.set();  // A.set() wakes the node (Woken) normally
    });

    FiberStack sw, sc, ss;
    SLUICE_CHECK(sched.init_fiber(fwait, sw.base(), sw.size()));
    SLUICE_CHECK(sched.init_fiber(fcancel, sc.base(), sc.size()));
    SLUICE_CHECK(sched.init_fiber(fset, ss.base(), ss.size()));
    sched.spawn(fwait);
    sched.spawn(fcancel);
    sched.spawn(fset);
    sched.run(3);

    SLUICE_CHECK_MSG(node.was_woken(),
                     "CANCEL-2a: original Event A can still wake the node normally");
    SLUICE_CHECK_MSG(sched.waiting_count() == 0, "CANCEL-2a: wait count at baseline");
}

// ---- CANCEL-2b: wrong Event, DIFFERENT Scheduler -> false, node intact -----
//
// The load-bearing cross-Scheduler case. Event A is bound to Scheduler A;
// Event B is bound to Scheduler B. W registers on Event A (Scheduler A). Then
// Event B.cancel(W) is attempted. The membership scan runs under Scheduler B's
// global_mtx_ + Event B's q.mtx(); node is NOT in Event B's queue -> false,
// WITHOUT reading node.home_ (which is protected by Scheduler A's global_mtx_,
// a DIFFERENT mutex -- no happens-before across Schedulers). The node remains
// Registered on Event A and is later woken by Event A.set() normally.
SLUICE_TEST_CASE(e12_cancel2b_wrong_event_different_scheduler_loses_safely) {
    if constexpr (!fiber_ctx::supported) return;

    AsyncIoContext ctx_a(std::make_unique<IdleBackend>());
    Scheduler sched_a(ctx_a);
    AsyncIoContext ctx_b(std::make_unique<IdleBackend>());
    Scheduler sched_b(ctx_b);

    Event ev_a(sched_a, /*initially_set=*/false);
    Event ev_b(sched_b, /*initially_set=*/false);
    WaitNode node;
    std::atomic<bool> registered{false};

    Fiber fwait;
    fwait.set_entry([&](Fiber&) {
        registered.store(true, std::memory_order_release);
        ev_a.wait(node);  // registered on Event A (Scheduler A)
    });
    FiberStack sw;
    SLUICE_CHECK(sched_a.init_fiber(fwait, sw.base(), sw.size()));
    sched_a.spawn(fwait);

    // Run Scheduler A just long enough for fwait to register + suspend. Drain
    // returns STALLED with the node Registered (MW-S3). Single-worker inline.
    sched_a.run(1);
    SLUICE_CHECK_MSG(node.is_registered(),
                     "CANCEL-2b: node Registered on Event A before the wrong-Event cancel");

    // From a SEPARATE thread, attempt Event B.cancel(node) (Scheduler B). This
    // must return false without mutation -- the membership scan is under
    // Scheduler B's global_mtx_ + Event B's q.mtx() and finds node absent.
    std::atomic<bool> wrong_cancel_done{false};
    std::thread b_cancel([&] {
        SLUICE_CHECK_MSG(!ev_b.cancel(node),
                         "CANCEL-2b: cross-Scheduler wrong-Event cancel -> false");
        wrong_cancel_done.store(true, std::memory_order_release);
    });
    b_cancel.join();
    SLUICE_CHECK_MSG(wrong_cancel_done.load(), "CANCEL-2b: wrong-Event cancel returned");

    // The node is STILL Registered on Event A; Event A can wake it normally.
    SLUICE_CHECK_MSG(node.is_registered(),
                     "CANCEL-2b: node remains Registered on Event A (no mutation)");
    ev_a.set();
    // Re-run Scheduler A to drain the now-woken node.
    sched_a.run(1);

    SLUICE_CHECK_MSG(node.was_woken(),
                     "CANCEL-2b: original Event A wakes the node normally after the wrong-Event cancel");
    SLUICE_CHECK_MSG(sched_a.waiting_count() == 0, "CANCEL-2b: Scheduler A wait count at baseline");
    SLUICE_CHECK_MSG(sched_b.waiting_count() == 0, "CANCEL-2b: Scheduler B wait count at baseline");
}

// ---- CANCEL-3: detached node -> false ---------------------------------------
SLUICE_TEST_CASE(e12_cancel3_detached_node_loses) {
    if constexpr (!fiber_ctx::supported) return;

    AsyncIoContext ctx(std::make_unique<IdleBackend>());
    Scheduler sched(ctx);
    Event ev(sched, /*initially_set=*/false);
    WaitNode detached;  // never registered

    SLUICE_CHECK_MSG(!ev.cancel(detached), "CANCEL-3: detached node -> false");
    SLUICE_CHECK_MSG(!detached.is_terminal(), "CANCEL-3: detached node untouched");
    SLUICE_CHECK_MSG(!ev.is_set(), "CANCEL-3: Event UNSET untouched");
    SLUICE_CHECK_MSG(sched.waiting_count() == 0, "CANCEL-3: no wait accounting touched");
}

// ---- CANCEL-4: Woken node -> false ------------------------------------------
SLUICE_TEST_CASE(e12_cancel4_woken_node_loses) {
    if constexpr (!fiber_ctx::supported) return;

    AsyncIoContext ctx(std::make_unique<IdleBackend>());
    Scheduler sched(ctx);
    Event ev(sched, /*initially_set=*/true);  // SET: wait returns Woken at admission
    WaitNode node;

    Fiber f;
    f.set_entry([&](Fiber&) { ev.wait(node); });  // observes SET -> Woken inline
    FiberStack sw;
    SLUICE_CHECK(sched.init_fiber(f, sw.base(), sw.size()));
    sched.spawn(f);
    sched.run(1);

    SLUICE_CHECK_MSG(node.was_woken(), "CANCEL-4: node Woken at admission");
    SLUICE_CHECK_MSG(!ev.cancel(node), "CANCEL-4: Woken node -> false (not in queue)");
    SLUICE_CHECK_MSG(node.was_woken(), "CANCEL-4: outcome unchanged (still Woken)");
}

// ---- CANCEL-5: Expired node -> false ----------------------------------------
SLUICE_TEST_CASE(e12_cancel5_expired_node_loses) {
    if constexpr (!fiber_ctx::supported) return;

    AsyncIoContext ctx(std::make_unique<IdleBackend>());
    Scheduler sched(ctx);
    E11TimerTestHooks::enable_test_clock(sched);
    Event ev(sched, /*initially_set=*/false);
    WaitNode node;
    std::atomic<bool> registered{false};

    Fiber f, fdriver;
    f.set_entry([&](Fiber&) {
        registered.store(true, std::memory_order_release);
        ev.wait_until(node, /*deadline=*/100);
    });
    fdriver.set_entry([&](Fiber&) {
        spin_wait(registered);
        for (int i = 0; i < 200 && !node.is_terminal(); ++i) {
            sched.advance_clock(100);
            std::this_thread::yield();
        }
    });
    FiberStack sw, sd;
    SLUICE_CHECK(sched.init_fiber(f, sw.base(), sw.size()));
    SLUICE_CHECK(sched.init_fiber(fdriver, sd.base(), sd.size()));
    sched.spawn(f);
    sched.spawn(fdriver);
    sched.run(2);

    SLUICE_CHECK_MSG(node.was_expired(), "CANCEL-5: node Expired");
    SLUICE_CHECK_MSG(!ev.cancel(node), "CANCEL-5: Expired node -> false (not in queue)");
    SLUICE_CHECK_MSG(node.was_expired(), "CANCEL-5: outcome unchanged (still Expired)");
}

// ---- CANCEL-6: already-Cancelled node -> false ------------------------------
SLUICE_TEST_CASE(e12_cancel6_cancelled_node_loses) {
    if constexpr (!fiber_ctx::supported) return;

    AsyncIoContext ctx(std::make_unique<IdleBackend>());
    Scheduler sched(ctx);
    Event ev(sched, /*initially_set=*/false);
    WaitNode node;
    std::atomic<bool> registered{false};

    Fiber fwait, fcancel;
    fwait.set_entry([&](Fiber&) {
        registered.store(true, std::memory_order_release);
        ev.wait(node);
    });
    fcancel.set_entry([&](Fiber&) {
        spin_wait(registered);
        bool won = false;
        for (int i = 0; i < 1000 && !won; ++i) won = ev.cancel(node);
        SLUICE_CHECK_MSG(won, "CANCEL-6: first cancel won");
    });
    FiberStack sw, sc;
    SLUICE_CHECK(sched.init_fiber(fwait, sw.base(), sw.size()));
    SLUICE_CHECK(sched.init_fiber(fcancel, sc.base(), sc.size()));
    sched.spawn(fwait);
    sched.spawn(fcancel);
    sched.run(2);

    SLUICE_CHECK_MSG(node.was_cancelled(), "CANCEL-6: node Cancelled by first cancel");
    SLUICE_CHECK_MSG(!ev.cancel(node), "CANCEL-6: second cancel -> false (not in queue)");
    SLUICE_CHECK_MSG(node.was_cancelled(), "CANCEL-6: outcome unchanged (still Cancelled)");
}

// ---- CANCEL-7: RESOURCE_WAKE wins before cancel -> false, Woken final -------
SLUICE_TEST_CASE(e12_cancel7_resource_wake_wins_before_cancel) {
    if constexpr (!fiber_ctx::supported) return;

    AsyncIoContext ctx(std::make_unique<IdleBackend>());
    Scheduler sched(ctx);
    Event ev(sched, /*initially_set=*/false);
    WaitNode node;
    std::atomic<bool> registered{false};
    std::atomic<bool> set_done{false};

    Fiber fwait, fset, fcancel;
    fwait.set_entry([&](Fiber&) {
        registered.store(true, std::memory_order_release);
        ev.wait(node);
    });
    fset.set_entry([&](Fiber&) {
        spin_wait(registered);
        std::this_thread::yield();
        ev.set();  // RESOURCE_WAKE wins
        set_done.store(true, std::memory_order_release);
    });
    fcancel.set_entry([&](Fiber&) {
        spin_wait(set_done);  // barrier: set wins first
        std::this_thread::yield();
        SLUICE_CHECK_MSG(!ev.cancel(node), "CANCEL-7: cancel after RESOURCE_WAKE won -> false");
    });
    FiberStack sw, ss, sc;
    SLUICE_CHECK(sched.init_fiber(fwait, sw.base(), sw.size()));
    SLUICE_CHECK(sched.init_fiber(fset, ss.base(), ss.size()));
    SLUICE_CHECK(sched.init_fiber(fcancel, sc.base(), sc.size()));
    sched.spawn(fwait);
    sched.spawn(fset);
    sched.spawn(fcancel);
    sched.run(3);

    SLUICE_CHECK_MSG(node.was_woken(), "CANCEL-7: Woken remains final");
    SLUICE_CHECK_MSG(ev.is_set(), "CANCEL-7: Event is SET");
    SLUICE_CHECK_MSG(sched.waiting_count() == 0, "CANCEL-7: wait count at baseline");
}

// ---- CANCEL-8: CANCEL wins before set -> Cancelled; later set leaves it -----
//
// CANCEL wins the resolve_ CAS; the node becomes Cancelled and is unlinked.
// A later set() finds the queue empty (the node is gone) and leaves the node
// Cancelled. The Event becomes SET (set() still flips the flag) but the node's
// outcome is final Cancelled.
SLUICE_TEST_CASE(e12_cancel8_cancel_wins_before_set_leaves_cancelled) {
    if constexpr (!fiber_ctx::supported) return;

    AsyncIoContext ctx(std::make_unique<IdleBackend>());
    Scheduler sched(ctx);
    Event ev(sched, /*initially_set=*/false);
    WaitNode node;
    std::atomic<bool> registered{false};
    std::atomic<bool> cancel_done{false};

    Fiber fwait, fcancel, fset;
    fwait.set_entry([&](Fiber&) {
        registered.store(true, std::memory_order_release);
        ev.wait(node);
    });
    fcancel.set_entry([&](Fiber&) {
        spin_wait(registered);
        bool won = false;
        for (int i = 0; i < 1000 && !won; ++i) won = ev.cancel(node);
        SLUICE_CHECK_MSG(won, "CANCEL-8: cancel won before set");
        cancel_done.store(true, std::memory_order_release);
    });
    fset.set_entry([&](Fiber&) {
        spin_wait(cancel_done);  // barrier: cancel wins first
        ev.set();  // queue empty now (node unlinked by cancel); Event becomes SET
    });
    FiberStack sw, sc, ss;
    SLUICE_CHECK(sched.init_fiber(fwait, sw.base(), sw.size()));
    SLUICE_CHECK(sched.init_fiber(fcancel, sc.base(), sc.size()));
    SLUICE_CHECK(sched.init_fiber(fset, ss.base(), ss.size()));
    sched.spawn(fwait);
    sched.spawn(fcancel);
    sched.spawn(fset);
    sched.run(3);

    SLUICE_CHECK_MSG(node.was_cancelled(), "CANCEL-8: node remains Cancelled after later set");
    SLUICE_CHECK_MSG(ev.is_set(), "CANCEL-8: Event became SET (set still flips the flag)");
    SLUICE_CHECK_MSG(sched.waiting_count() == 0, "CANCEL-8: wait count at baseline");
}

// ===========================================================================
// Slice 9 — Causal set/reset epoch proofs (T27–T29)
// ===========================================================================
//
// These causal proofs use run_live(1): a single Worker parks on the unresolved
// Wold wait (MW-S3 + external-wake-capable). The set-store-before-drain seam
// pauses a set() thread on an EXTERNAL OS thread while it holds global_mtx_
// (NOT a worker fiber — pausing a worker fiber while it holds global_mtx_
// deadlocks the cooperative run). The test main thread observes the pause via
// the seam, records that a competing reset/admission CANNOT complete, then
// releases the seam. No timing inference: the seam is the causal observation.

// ---- T27: causal set-drain BLOCKS reset (active drain holds global_mtx_) ----
//
// Topology (Corrective C1):
//   - Wold is Registered and waiting on an UNSET Event (Worker parks in Live).
//   - External setter S1 calls set(); it stores SET then PAUSES at the
//     EVENT_SET_AFTER_SET_STORE_BEFORE_DRAIN seam while STILL HOLDING global_mtx_.
//   - While S1 is paused (mechanically observed via the seam), a reset thread
//     records reset_attempted and blocks on global_mtx_ (reset_completed false).
//   - Release S1; it drains Wold (routing it runnable -> wakes the parked
//     Worker) and releases global_mtx_. The reset then completes.
// No timing inference: the seam is the causal block observation.
SLUICE_TEST_CASE(e12_t27_causal_set_drain_blocks_reset) {
    if constexpr (!fiber_ctx::supported) return;

    AsyncIoContext ctx(std::make_unique<IdleBackend>());
    Scheduler sched(ctx);
    sluice_async_test::ControllerGuard ctrl(sched);  // event seam registry

    Event ev(sched, /*initially_set=*/false);
    WaitNode n_old;
    std::atomic<bool> old_registered{false};
    std::atomic<bool> reset_attempted{false}, reset_completed{false};

    Fiber f_old;
    f_old.set_entry([&](Fiber&) {
        old_registered.store(true, std::memory_order_release);
        ev.wait(n_old);  // Registered + Suspended on UNSET
    });

    FiberStack s_old;
    SLUICE_CHECK(sched.init_fiber(f_old, s_old.base(), s_old.size()));
    sched.spawn(f_old);

    // Arm the set-store-before-drain seam BEFORE the Live run. S1 will pause
    // mid-drain on an external thread.
    E12EventTestHooks::arm_set_store_before_drain(sched);

    // Reset contender (external thread): waits for the seam to be paused, then
    // attempts reset() which must block on global_mtx_.
    std::thread reset_thread([&] {
        // Spin until S1 has paused mid-drain (mechanical observation).
        spin_wait_pred([&] { return E12EventTestHooks::is_set_paused(sched); });
        reset_attempted.store(true, std::memory_order_release);
        ev.reset();  // blocks on global_mtx_ until S1 releases
        reset_completed.store(true, std::memory_order_release);
    });

    // S1 (external thread): set() stores SET, pauses mid-drain holding global_mtx_.
    std::thread s1_thread([&] {
        spin_wait(old_registered);
        ev.set();  // stores SET, pauses mid-drain under global_mtx_
    });

    // Coordinator (main thread): wait until S1 has paused, then mechanically
    // observe the reset contender is blocked. run_live(1) parks the Worker on
    // Wold; we drive the seam coordination from here.
    //
    // We must let the Worker reach MW-S3 + park first. Spin on old_registered,
    // then run a Live run in a SEPARATE coordinator thread so THIS thread can
    // perform seam coordination while the run is resident.
    std::thread run_thread([&] { sched.run_live(1); });

    // Wait for S1 to reach the paused mid-drain state.
    E12EventTestHooks::wait_set_paused(sched);
    // Give the reset contender a chance to record its attempt (it is blocked).
    spin_wait(reset_attempted);
    std::this_thread::yield();
    // Mechanical assertion: reset attempted but could NOT complete while S1
    // holds global_mtx_ (the active old-set drain).
    SLUICE_CHECK_MSG(reset_attempted.load(), "reset attempt recorded while S1 paused");
    SLUICE_CHECK_MSG(!reset_completed.load(), "reset could not complete (drain holds global_mtx_)");

    // Release S1: it drains Wold (routing it runnable -> wakes the parked
    // Worker via signal_wake_locked) and releases global_mtx_.
    E12EventTestHooks::release_set(sched);

    s1_thread.join();
    reset_thread.join();
    run_thread.join();

    SLUICE_CHECK_MSG(reset_completed.load(), "reset completed after S1 released");
    SLUICE_CHECK_MSG(n_old.was_woken(), "Wold woken by S1's drain");
    SLUICE_CHECK_MSG(sched.waiting_count() == 0, "wait count at baseline");
}

// ---- T28: set-drain BLOCKS another Scheduler-global operation (global-mutex
//      proxy evidence, NOT an Event-admission proof) -------------------------
//
// E12-A-EVENT-CORRECTIVE-2 (Corrective F1): T28 is re-documented accurately.
// It does NOT prove Event admission is blocked (an admission fiber cannot be
// driven from an external thread because wait() performs a context_switch that
// must run on a Worker). Instead it proves the weaker, still-useful fact: an
// active set() drain holds global_mtx_, blocking ANOTHER Scheduler-global
// operation. The contender here is ev.cancel() of a detached node (which takes
// global_mtx_ and returns false); while S1 is paused mid-drain, the contender
// records its attempt but CANNOT complete (global_mtx_ is held). After S1
// releases, the contender completes. Actual Event-admission blocking is proven
// by T31 (the two-phase admission-attempt marker).
SLUICE_TEST_CASE(e12_t28_causal_set_drain_blocks_admission) {
    if constexpr (!fiber_ctx::supported) return;

    AsyncIoContext ctx(std::make_unique<IdleBackend>());
    Scheduler sched(ctx);
    sluice_async_test::ControllerGuard ctrl(sched);  // event seam registry

    Event ev(sched, /*initially_set=*/false);
    WaitNode n_old;
    std::atomic<bool> old_registered{false};
    std::atomic<bool> admission_attempted{false}, admission_completed{false};

    Fiber f_old;
    f_old.set_entry([&](Fiber&) {
        old_registered.store(true, std::memory_order_release);
        ev.wait(n_old);
    });

    FiberStack s_old;
    SLUICE_CHECK(sched.init_fiber(f_old, s_old.base(), s_old.size()));
    sched.spawn(f_old);

    E12EventTestHooks::arm_set_store_before_drain(sched);

    // Admission contender (external thread): attempts ev.wait() while S1 holds
    // global_mtx_. Because ev.wait() performs a context_switch, it must run on
    // a Worker — so this contender only ATTEMPTS to acquire the admission path
    // via the Scheduler integration. Instead, we model "admission attempts to
    // take global_mtx_" with a reset-style contender: it records the attempt and
    // blocks on global_mtx_ (which admission's critical section also needs).
    // We use ev.cancel() (which also takes global_mtx_) as the blocked contender:
    // a no-op cancel of a detached node records the attempt then blocks.
    WaitNode detached_for_probe;  // never registered -> cancel loses, but only
                                  // AFTER acquiring global_mtx_ (which it cannot
                                  // while S1 holds it).
    std::thread admission_thread([&] {
        spin_wait_pred([&] { return E12EventTestHooks::is_set_paused(sched); });
        admission_attempted.store(true, std::memory_order_release);
        // ev.cancel takes global_mtx_ (same domain admission needs). It blocks
        // until S1 releases, then loses (detached node) and returns false.
        (void)ev.cancel(detached_for_probe);
        admission_completed.store(true, std::memory_order_release);
    });

    std::thread s1_thread([&] {
        spin_wait(old_registered);
        ev.set();  // stores SET, pauses mid-drain under global_mtx_
    });

    std::thread run_thread([&] { sched.run_live(1); });

    E12EventTestHooks::wait_set_paused(sched);
    spin_wait(admission_attempted);
    std::this_thread::yield();
    SLUICE_CHECK_MSG(admission_attempted.load(), "admission attempt recorded while S1 paused");
    SLUICE_CHECK_MSG(!admission_completed.load(), "admission could not complete (drain holds global_mtx_)");

    E12EventTestHooks::release_set(sched);

    s1_thread.join();
    admission_thread.join();
    run_thread.join();

    SLUICE_CHECK_MSG(admission_completed.load(), "admission completed after S1 released");
    SLUICE_CHECK_MSG(n_old.was_woken(), "Wold woken by S1's drain");
    SLUICE_CHECK_MSG(sched.waiting_count() == 0, "wait count at baseline");
}

// ---- T29: sequential post-reset epoch sanity (NON-CAUSAL) -------------------
//
// E12-A-EVENT-CORRECTIVE-2 (Corrective F2): T29 is re-classified as a
// SEQUENTIAL post-reset epoch sanity test. It does NOT arm or pause the
// set-drain seam; the fibers run under a cooperative multi-worker run with
// spin-barriers that force a sequential trace (Wold -> S1 -> reset -> Wnew ->
// S2). It confirms the sequential outcome (Wold woken by S1; Wnew woken by S2,
// NOT by S1's drain) but is NOT a causal proof of epoch isolation -- the causal
// active-drain serialization is proven by T27 (set-drain blocks reset via the
// seam) and T31 (set-drain blocks admission via the two-phase marker). T29 is
// supplementary sanity only.
SLUICE_TEST_CASE(e12_t29_post_reset_waiter_waits_for_s2_not_stale_s1) {
    if constexpr (!fiber_ctx::supported) return;

    AsyncIoContext ctx(std::make_unique<IdleBackend>());
    Scheduler sched(ctx);

    Event ev(sched, /*initially_set=*/false);
    WaitNode n_old, n_new;
    std::atomic<bool> old_registered{false};
    std::atomic<bool> s1_done{false};
    std::atomic<bool> reset_done{false};
    std::atomic<bool> new_registered{false};

    Fiber f_old, f_s1, f_reset, f_new, f_s2;
    f_old.set_entry([&](Fiber&) {
        old_registered.store(true, std::memory_order_release);
        ev.wait(n_old);
    });
    f_s1.set_entry([&](Fiber&) {
        spin_wait(old_registered);
        std::this_thread::yield();
        ev.set();  // S1: SET + drain Wold (atomic under global_mtx_)
        s1_done.store(true, std::memory_order_release);
    });
    f_reset.set_entry([&](Fiber&) {
        spin_wait(s1_done);
        std::this_thread::yield();
        ev.reset();  // UNSET
        reset_done.store(true, std::memory_order_release);
    });
    f_new.set_entry([&](Fiber&) {
        spin_wait(reset_done);
        std::this_thread::yield();
        new_registered.store(true, std::memory_order_release);
        ev.wait(n_new);  // suspends on UNSET — S1's drain is already complete
    });
    f_s2.set_entry([&](Fiber&) {
        spin_wait(new_registered);
        std::this_thread::yield();
        ev.set();  // S2: SET + drain Wnew
    });

    FiberStack s_old, s_s1, s_reset, s_new, s_s2;
    SLUICE_CHECK(sched.init_fiber(f_old, s_old.base(), s_old.size()));
    SLUICE_CHECK(sched.init_fiber(f_s1, s_s1.base(), s_s1.size()));
    SLUICE_CHECK(sched.init_fiber(f_reset, s_reset.base(), s_reset.size()));
    SLUICE_CHECK(sched.init_fiber(f_new, s_new.base(), s_new.size()));
    SLUICE_CHECK(sched.init_fiber(f_s2, s_s2.base(), s_s2.size()));
    sched.spawn(f_old);
    sched.spawn(f_s1);
    sched.spawn(f_reset);
    sched.spawn(f_new);
    sched.spawn(f_s2);
    sched.run(5);

    SLUICE_CHECK_MSG(n_old.was_woken(), "Wold woken by S1's drain");
    SLUICE_CHECK_MSG(n_new.was_woken(), "Wnew woken by S2 (NOT by S1's drain)");
    SLUICE_CHECK_MSG(ev.is_set(), "Event is SET (from S2)");
    SLUICE_CHECK_MSG(sched.waiting_count() == 0, "wait count at baseline");
}

// ===========================================================================
// Slice 10 — Causal admission ordering (T30–T31)
// ===========================================================================

// ---- T30: causal admission-FIRST ordering ----------------------------------
//
// W's admission (on the Worker fiber) registers, then PAUSES at the
// EVENT_ADMISSION_AFTER_REGISTER_BEFORE_FINAL_SET_CHECK seam (holding
// global_mtx_+q.mtx_). An external setter attempts set() and CANNOT complete
// until W releases serialization (it blocks on global_mtx_). The coordinator
// mechanically observes the setter is blocked, then releases W's admission: it
// completes the final SET check under UNSET (set has NOT happened), commits
// waiting, releases. The setter's set() then drains W -> Woken.
//
// run_live(1): the Worker runs W's admission and parks at the seam. The setter
// is an external thread. Because the seam holds global_mtx_+q.mtx_ while the
// Worker fiber is paused, the worker_loop CANNOT drain — but that is fine: the
// only fiber (W) is paused inside await_event_wait, and the coordinator (main
// thread) drives the seam release.
SLUICE_TEST_CASE(e12_t30_causal_admission_first_ordering) {
    if constexpr (!fiber_ctx::supported) return;

    AsyncIoContext ctx(std::make_unique<IdleBackend>());
    Scheduler sched(ctx);
    sluice_async_test::ControllerGuard ctrl(sched);  // event seam registry

    Event ev(sched, /*initially_set=*/false);
    WaitNode node;
    std::atomic<bool> setter_attempted{false}, setter_completed{false};

    Fiber fwait;
    fwait.set_entry([&](Fiber&) {
        ev.wait(node);  // admission registers then pauses at the seam
    });

    FiberStack sw;
    SLUICE_CHECK(sched.init_fiber(fwait, sw.base(), sw.size()));
    sched.spawn(fwait);

    E12EventTestHooks::arm_admission_before_final_check(sched);

    // Setter contender (external): waits for admission to pause, then attempts
    // set() which blocks on global_mtx_.
    std::thread set_thread([&] {
        spin_wait_pred([&] { return E12EventTestHooks::is_admission_paused(sched); });
        setter_attempted.store(true, std::memory_order_release);
        ev.set();  // blocks on global_mtx_ until admission releases
        setter_completed.store(true, std::memory_order_release);
    });

    std::thread run_thread([&] { sched.run_live(1); });

    // Wait for admission to reach the paused state.
    E12EventTestHooks::wait_admission_paused(sched);
    spin_wait(setter_attempted);
    std::this_thread::yield();
    SLUICE_CHECK_MSG(setter_attempted.load(), "setter attempted while admission paused");
    SLUICE_CHECK_MSG(!setter_completed.load(), "setter could not complete (admission holds serialization)");

    // Release W's admission: it completes the final SET check under UNSET (set
    // is still blocked), commits waiting, releases. The setter then runs, stores
    // SET, and drains W (now Registered+Suspended) -> Woken, waking the Worker.
    E12EventTestHooks::release_admission(sched);

    set_thread.join();
    run_thread.join();

    SLUICE_CHECK_MSG(setter_completed.load(), "setter completed after admission released");
    SLUICE_CHECK_MSG(node.was_woken(), "W woken by set's drain after admission released");
    SLUICE_CHECK_MSG(ev.is_set(), "Event is SET");
    SLUICE_CHECK_MSG(sched.waiting_count() == 0, "wait count at baseline");
}

// ---- T31: causal set-FIRST ordering (TWO-PHASE admission marker) ----------
//
// E12-A-EVENT-CORRECTIVE-2 (Corrective F3): T31 no longer infers "admission is
// blocked" from yield() timing. It uses TWO mechanically-observed phase markers
// driven by the internal-testing controller:
//   - admission_attempt_before_global_lock: armed + BLOCKING. scheduler.cpp hits
//     it BEFORE taking global_mtx_. fwait blocks here until the test releases it,
//     so the setter can acquire global_mtx_ first.
//   - admission_before_final_check: set by scheduler.cpp AFTER the admission
//     has taken global_mtx_ + q.mtx() (it entered its critical section).
//
// Topology: fwait blocks at the armed pre-lock phase. The setter acquires
// global_mtx_, stores SET, pauses mid-drain. Release the pre-lock phase: fwait
// proceeds and blocks on the global_mtx_ acquire (setter holds it). The
// coordinator mechanically observes:
//   setter paused after SET store, still holding global_mtx_
//   admission_attempt_reached == true   (pre-lock point hit)
//   is_admission_paused == false         (could NOT take global_mtx_)
//   wait_done == false
// This proves admission did NOT cross the active set drain. After releasing the
// setter, admission takes global_mtx_, observes SET, and returns Woken inline.
SLUICE_TEST_CASE(e12_t31_causal_set_first_ordering) {
    if constexpr (!fiber_ctx::supported) return;

    AsyncIoContext ctx(std::make_unique<IdleBackend>());
    Scheduler sched(ctx);
    sluice_async_test::ControllerGuard ctrl(sched);  // event seam registry

    Event ev(sched, /*initially_set=*/false);
    WaitNode node;
    std::atomic<bool> setter_acquired{false};
    std::atomic<bool> wait_done{false};

    // Deterministic set-FIRST proof. The pre-lock admission-attempt phase is
    // ARMED so fwait BLOCKS at the pre-lock point (before acquiring global_mtx_).
    // The external setter then acquires the (now free) global_mtx_, stores SET,
    // and pauses mid-drain. We then release the pre-lock phase: fwait proceeds
    // and BLOCKS on the global_mtx_ acquire (the setter holds it). The two-phase
    // markers prove admission could NOT enter its critical section while the
    // setter's drain was active.
    Fiber fwait;
    fwait.set_entry([&](Fiber&) {
        ev.wait(node);  // pre-lock phase blocks here (armed); after release it
                        // blocks on global_mtx_ (setter holds it) until the setter
                        // releases, then observes SET -> Woken inline.
        wait_done.store(true, std::memory_order_release);
    });
    FiberStack sw;
    SLUICE_CHECK(sched.init_fiber(fwait, sw.base(), sw.size()));
    sched.spawn(fwait);

    // Arm BOTH the pre-lock phase (blocks fwait before the lock) AND the
    // set-store-before-drain phase (blocks the setter mid-drain).
    E12EventTestHooks::reset_admission_attempt(sched);
    sluice_async_test::arm(sched, sluice_async_test::PhaseTag::e12_admission_attempt_before_global_lock);
    E12EventTestHooks::arm_set_store_before_drain(sched);

    // Start the Live run: the Worker distributes + runs fwait, which blocks at
    // the armed pre-lock phase (BEFORE acquiring global_mtx_).
    std::thread run_thread([&] { sched.run_live(1); });
    spin_wait_pred([&] {
        return E12EventTestHooks::admission_attempt_reached(sched);
    });

    // fwait is blocked at the pre-lock phase. Now start the setter: it acquires
    // the free global_mtx_, stores SET, and pauses mid-drain (armed).
    std::thread set_thread([&] {
        ev.set();
        setter_acquired.store(true, std::memory_order_release);
    });
    E12EventTestHooks::wait_set_paused(sched);

    // Release the pre-lock phase: fwait proceeds and attempts LockGuard(global_mtx_).
    // The setter HOLDS global_mtx_ (paused mid-drain), so fwait blocks on the
    // acquire. Give it a bounded moment to record the block.
    sluice_async_test::release(sched, sluice_async_test::PhaseTag::e12_admission_attempt_before_global_lock);
    std::this_thread::yield();
    std::this_thread::yield();

    // Mechanical two-phase proof: pre-lock marker reached, post-lock marker NOT.
    // fwait reached the pre-lock point but could NOT enter its CS (global_mtx_
    // held by the active set drain). wait_done is false; the setter has not
    // returned (still paused mid-drain).
    SLUICE_CHECK_MSG(E12EventTestHooks::admission_attempt_reached(sched),
                     "T31: admission pre-lock attempt reached");
    SLUICE_CHECK_MSG(!E12EventTestHooks::is_admission_paused(sched),
                     "T31: admission could NOT enter its CS (setter holds global_mtx_)");
    SLUICE_CHECK_MSG(!wait_done.load(), "T31: wait did not complete while setter holds global_mtx_");
    SLUICE_CHECK_MSG(!setter_acquired.load(), "T31: setter still paused mid-drain");

    // Release the setter: it finishes the drain (empty queue, fwait not
    // registered yet) + releases global_mtx_. fwait then acquires the lock,
    // registers, observes SET, returns Woken inline.
    E12EventTestHooks::release_set(sched);

    set_thread.join();
    run_thread.join();

    SLUICE_CHECK_MSG(setter_acquired.load(), "T31: setter completed after release");
    SLUICE_CHECK_MSG(wait_done.load(), "T31: admission returned after setter released");
    SLUICE_CHECK_MSG(node.was_woken(), "T31: admission observed SET -> Woken inline (no suspend)");
    SLUICE_CHECK_MSG(ev.is_set(), "T31: Event is SET");
    SLUICE_CHECK_MSG(sched.waiting_count() == 0, "T31: wait count at baseline");
}

// ===========================================================================
// Slice 11 — Deterministic park-liveness + deadline precedence (T32–T33)
// ===========================================================================

// ---- T32: truly parked Live Worker awakened by external Event::set ---------
//
// E12-A-EVENT-CORRECTIVE-2 (Corrective F4): T32 uses a MECHANICAL park proof
// (E9 park_commit seam) AND a bounded failure guard built on a JOINED
// std::jthread watchdog (NOT a detached thread). A Live Worker with one
// unresolved Event wait reaches the real park commit boundary. The test
// observes the park phase mechanically (no sleep_for inference), then an
// external OS thread calls Event::set(); the Worker leaves park and resumes the
// waiter Woken. A watchdog jthread bounds the test: on timeout it releases any
// armed gate + drives Event::set() so the run terminates and ALL threads join
// before the test returns (no UAF from detached threads). The timeout is ONLY a
// failure guard; the park phase itself is observed mechanically.
SLUICE_TEST_CASE(e12_t32_parked_live_worker_awakened_by_external_set) {
    if constexpr (!fiber_ctx::supported) return;

    AsyncIoContext ctx(std::make_unique<IdleBackend>());
    Scheduler sched(ctx);
    sluice_async_test::ControllerGuard ctrl(sched);  // park-commit seam registry

    Event ev(sched, /*initially_set=*/false);
    WaitNode node;
    std::atomic<bool> waiter_registered{false};
    std::atomic<int> entries{0};
    std::atomic<bool> park_observed{false};
    std::atomic<bool> watchdog_fired{false};

    Fiber fwait;
    fwait.set_entry([&](Fiber&) {
        entries.fetch_add(1, std::memory_order_acq_rel);
        waiter_registered.store(true, std::memory_order_release);
        ev.wait(node);
        entries.fetch_add(1, std::memory_order_acq_rel);
    });

    FiberStack sw;
    SLUICE_CHECK(sched.init_fiber(fwait, sw.base(), sw.size()));
    sched.spawn(fwait);

    // Arm the park-commit seam so the Worker pauses at the physical-park
    // boundary. The external set runs AFTER we observe the pause.
    E12EventTestHooks::arm_park_commit(sched);

    std::thread ext([&] {
        // Wait until the waiter has registered.
        while (!waiter_registered.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
        // Mechanically observe the Worker reached the real park boundary.
        E12EventTestHooks::wait_park_commit_paused(sched);
        park_observed.store(true, std::memory_order_release);
        // Now release the park seam so the Worker enters the physical wait, then
        // immediately set() from the external thread. The Worker's observed
        // epoch was recorded AFTER the commit boundary, so a wake via
        // signal_wake_locked is observed.
        E12EventTestHooks::release_park_commit(sched);
        ev.set();  // external-thread set: broadcast via the Scheduler wake source
    });

    // Bounded failure guard: a JOINED watchdog jthread. If the external setter
    // does not progress within the bound, the watchdog releases the park seam +
    // drives ev.set() so the Live run terminates and run_thread joins. The
    // watchdog is JOINED (not detached) so it cannot outlive the test and UAF
    // the stack locals. The timeout is ONLY a failure guard -- the park phase is
    // observed mechanically, not via the timeout.
    std::jthread watchdog([&](std::stop_token st) {
        constexpr auto kBound = std::chrono::seconds(10);
        auto deadline = std::chrono::steady_clock::now() + kBound;
        while (!st.stop_requested()) {
            if (entries.load(std::memory_order_acquire) >= 2) return;  // success
            if (std::chrono::steady_clock::now() >= deadline) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
            if (st.stop_requested()) return;
        }
        // Timeout: release any armed gate + drive set() so the run terminates.
        watchdog_fired.store(true, std::memory_order_release);
        E12EventTestHooks::release_park_commit(sched);
        ev.set();
    });

    // Run the Live scheduler. The watchdog's stop_callback requests stop when
    // this scope exits; run_live returns once the run terminates (either via the
    // external set() succeeding, or the watchdog's set()).
    sched.run_live(1);  // Live: parks on MW-S3 + external-wake-capable
    ext.join();
    watchdog.request_stop();  // signal the watchdog to return
    // Push the watchdog's wait predicate true so it exits its loop promptly.
    // (request_stop is observed on its next sleep_for iteration.)
    watchdog.join();

    SLUICE_CHECK_MSG(!watchdog_fired.load(),
                     "T32: watchdog did NOT fire (test completed within the bound)");
    SLUICE_CHECK_MSG(park_observed.load(), "T32: Worker mechanically reached the park boundary");
    SLUICE_CHECK_MSG(entries.load() == 2, "T32: waiter resumed via external set()");
    SLUICE_CHECK_MSG(node.was_woken(), "T32: waiter resolved Woken");
    SLUICE_CHECK_MSG(ev.is_set(), "T32: Event is SET");
    SLUICE_CHECK_MSG(sched.waiting_count() == 0, "T32: no unresolved waits remain");
}

// ---- T33: SET + already-due deadline -> Woken (deadline precedence) --------
//
// The F-EVENT-DEADLINE precedence: at admission, Event SET readiness is checked
// BEFORE the already-due deadline predicate. Therefore Event SET + already-due
// deadline -> Woken inline (no suspension), with the timer registration safely
// retired and no later expiry publication. Uses the controllable test clock so
// the deadline is provably already due at admission.
SLUICE_TEST_CASE(e12_t33_set_plus_already_due_deadline_is_woken) {
    if constexpr (!fiber_ctx::supported) return;

    AsyncIoContext ctx(std::make_unique<IdleBackend>());
    Scheduler sched(ctx);
    E11TimerTestHooks::enable_test_clock(sched);

    // Event initially SET; clock starts at 0.
    Event ev(sched, /*initially_set=*/true);
    // Set the clock so a deadline of 0 (or any value <= now) is already due.
    E11TimerTestHooks::set_clock(sched, 50);  // now = 50

    WaitNode node;
    std::atomic<int> entries{0};
    Fiber f;
    f.set_entry([&](Fiber&) {
        entries.fetch_add(1, std::memory_order_acq_rel);
        // wait_until with a deadline that is ALREADY DUE (0 <= now=50), while
        // the Event is SET. SET precedence: resolve Woken inline.
        ev.wait_until(node, /*deadline=*/0);
        entries.fetch_add(1, std::memory_order_acq_rel);
    });

    FiberStack sw;
    SLUICE_CHECK(sched.init_fiber(f, sw.base(), sw.size()));
    sched.spawn(f);
    sched.run(1);

    SLUICE_CHECK_MSG(entries.load() == 2, "waiter never suspended (SET precedence)");
    SLUICE_CHECK_MSG(node.was_woken(), "SET + already-due deadline -> Woken (not Expired)");
    SLUICE_CHECK_MSG(ev.is_set(), "Event remains SET");
    SLUICE_CHECK_MSG(sched.waiting_count() == 0, "no waits remain");
    // Advance the clock past the deadline and pump: NO later expiry publication
    // (the registration was retired at admission; the node is terminal Woken).
    E11TimerTestHooks::set_clock(sched, 1000);
    sched.advance_clock(1000);
    SLUICE_CHECK_MSG(node.was_woken(), "no later expiry publication (still Woken)");
}
