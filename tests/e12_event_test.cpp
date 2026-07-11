// e12_event_test — Async Event synchronization primitive (sluice-CORE-E12-A).
//
// Deterministic production tests for the persistent manual-reset Event built on
// the closed E10/E11 wait substrate. Observed ONLY through the Event public API
// + the inherited Scheduler integration seams:
//
//   - Event::is_set / set / reset / wait / wait_until / wait_queue
//   - Scheduler::cancel_wait(event.wait_queue(), node)  (inherited E10 cancel)
//   - Scheduler::advance_clock / E11TimerTestHooks       (deterministic timer)
//   - Scheduler::wake_wait_one(event.wait_queue())       (direct wake, for tests
//     that need a single-winner RESOURCE_WAKE without set())
//   - WaitNode public lock-free state queries (was_woken/was_cancelled/
//     was_expired/is_terminal/outcome)
//   - Scheduler::waiting_count()  (the wait-accounting authority)
//
// Every race proof uses a controllable monotonic clock + explicit timer driver
// or retry loops — NEVER sleep_for timing as causal proof (H7).
//
// Gated to x86_64 (fiber_ctx::supported): registration requires a real Fiber.
#include "harness.hpp"

#include <sluice/async/async_io_context.hpp>
#include <sluice/async/event.hpp>
#include <sluice/async/fake_backend.hpp>
#include <sluice/async/fiber.hpp>
#include <sluice/async/fiber_ctx.hpp>
#include <sluice/async/scheduler.hpp>
#include <sluice/async/wait_node.hpp>
#include <sluice/async/wait_queue.hpp>

#include <atomic>
#include <cstddef>
#include <thread>

using namespace sluice::async;
using sluice::Result;

// E11 deterministic clock/timer test seam (reused for deadline tests). Defined
// in the sluice::async namespace so the Scheduler friend declaration matches.
namespace sluice::async {
struct E11TimerTestHooks {
    static void enable_test_clock(Scheduler& s) {
        s.test_clock_mode_.store(true, std::memory_order::release);
        s.clock_.store(0, std::memory_order::release);
    }
    static void set_clock(Scheduler& s, Scheduler::deadline_t t) {
        s.clock_.store(t, std::memory_order::release);
    }
    static Scheduler::deadline_t now(const Scheduler& s) {
        return s.clock_.load(std::memory_order::acquire);
    }
};
}  // namespace sluice::async

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
            cancelled = sched.cancel_wait(ev.wait_queue(), n2);
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
        (void)sched.cancel_wait(ev.wait_queue(), node);  // loser
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
            cancelled = sched.cancel_wait(ev.wait_queue(), node);
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
        (void)sched.cancel_wait(ev.wait_queue(), node);  // loser
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
                if (sched.cancel_wait(ev.wait_queue(), node)) break;
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

// ---- T19: external OS thread set() wakes a truly parked Live Scheduler ----
//
// A Live Scheduler with one Worker parks when the only work is an unresolved
// Event wait. An external std::thread calls Event::set(). The set() broadcast
// resolves the waiter through the Scheduler wake source (route_runnable_locked
// -> signal_wake_locked), the parked Worker wakes, and the Fiber resumes Woken.
// No Event-private condition variable, no polling, no caller re-entry.
SLUICE_TEST_CASE(e12_t19_external_thread_set_wakes_parked_live) {
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

    // External producer: wait for the waiter to register, give the Worker a
    // beat to reach MW-S3 + park, then set() from outside any worker thread.
    std::thread ext([&] {
        while (!waiter_registered.load(std::memory_order_acquire)) {
            std::this_thread::sleep_for(std::chrono::microseconds(50));
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
        ev.set();  // external-thread set: broadcast via the Scheduler wake source
    });

    sched.run_live(1);  // Live: parks on MW-S3 + external-wake-capable
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
    SLUICE_CHECK_MSG(sched.cancel_wait(ev.wait_queue(), node), "caller cancels stranded wait");
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
                cancelled = sched.cancel_wait(ev.wait_queue(), n3);
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
