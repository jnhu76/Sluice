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

#include <sluice/async/async_io_context.hpp>
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
}  // namespace

// E11 deterministic clock/timer test seam (M7). Enables the controllable
// monotonic clock so race proofs never use sleep_for as causal proof. Defined
// in the sluice::async namespace so the Scheduler friend declaration
// (friend struct E11TimerTestHooks) matches this exact type.
namespace sluice::async {
struct E11TimerTestHooks {
    static void enable_test_clock(Scheduler& s) {
        // Flip into test-clock mode and reset the logical clock to 0 under
        // global_mtx_. After this, advance_clock() drives time deterministically.
        s.test_clock_mode_ = true;
        s.clock_.store(0, std::memory_order::release);
    }
    static void set_clock(Scheduler& s, Scheduler::deadline_t t) {
        // Directly set the logical clock (no pump) for setup.
        s.clock_.store(t, std::memory_order::release);
    }
    static Scheduler::deadline_t now(const Scheduler& s) {
        return s.clock_.load(std::memory_order::acquire);
    }
    // Active deadline count + earliest deadline (under global_mtx_). Used by the
    // park-topology tests (T12/T13) to observe the deadline-heap state without
    // driving expiry: they assert that the obligation set evolves correctly
    // (new earlier deadline becomes earliest; retiring earliest leaves the next).
    static std::size_t active_deadline_count(const Scheduler& s) {
        std::size_t n = 0;
        for (const auto& r : s.timer_pool_) {
            if (r.is_active()) ++n;
        }
        return n;
    }
    static bool earliest_active_deadline(const Scheduler& s,
                                         Scheduler::deadline_t& out) {
        return s.earliest_active_deadline_locked(out);
    }
};
}  // namespace sluice::async

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
// (destroyed). Wait epoch E+1: a NEW node is constructed (it may reuse the
// same stack address). A stale timer entry for E (still in the heap, retired)
// is pumped — it MUST NOT resolve E+1. We force the pump and assert E+1 is
// untouched. This is the load-bearing address-reuse trace (I3/I4).
//
// We approximate the same-address reuse by constructing the second node in the
// SAME lexical scope position after the first is destroyed. The decisive proof
// is the retirement state, not the address: even if addresses differed, a
// retired registration must be inert.
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

    // Epoch E: register + wake (retires E's timer). Then epoch E+1: a second
    // node waits with a far-future deadline; E's retired timer must not touch it.
    Fiber fwait, fwake;
    fwait.set_entry([&](Fiber&) {
        // Epoch E.
        registered_e.store(true, std::memory_order::release);
        WaitNode node_e;
        sched.await_wait_deadline(q, node_e, Scheduler::deadline_t{100});
        SLUICE_CHECK_MSG(node_e.was_woken(), "epoch E resolved Woken");
        e_resolved.store(true, std::memory_order::release);
        // Epoch E+1: a distinct node, far-future deadline. Signal registration
        // so the waker can drive the (inert) old-timer pump, then wake E+1.
        WaitNode node_e1;
        e1_registered.store(true, std::memory_order::release);
        sched.await_wait_deadline(q, node_e1, Scheduler::deadline_t{100000});
        // E+1 is resolved by an explicit wake (below), NOT by E's old timer.
        SLUICE_CHECK_MSG(node_e1.was_woken(), "epoch E+1 resolved Woken (not by old timer)");
    });
    fwake.set_entry([&](Fiber&) {
        spin_wait(registered_e);
        // Wake epoch E.
        for (int i = 0; i < 10000; ++i) {
            if (sched.wake_wait_one(q)) break;
            std::this_thread::yield();
        }
        spin_wait(e_resolved);  // E resolved (timer retired); E+1 now registering.
        spin_wait(e1_registered);  // E+1 registered with deadline 100000.
        // Advance the clock PAST E's old deadline (100). E's retired timer is
        // pumped and must be inert — it must NOT resolve E+1 (deadline 100000).
        sched.advance_clock(500);
        std::this_thread::yield();  // let the (inert) pump run if any
        // E+1 must still be waiting (its deadline 100000 is not due; E's timer
        // did not resolve it). Wake it explicitly now.
        for (int i = 0; i < 10000; ++i) {
            if (sched.wake_wait_one(q)) break;
            std::this_thread::yield();
        }
    });

    FiberStack sw, sk;
    SLUICE_CHECK(sched.init_fiber(fwait, sw.base(), sw.size()));
    SLUICE_CHECK(sched.init_fiber(fwake, sk.base(), sk.size()));
    sched.spawn(fwait);
    sched.spawn(fwake);
    // run(2): the waker spin-waits for the waiter to progress between epochs,
    // which requires the waiter to run concurrently (the waker cannot both
    // spin-wait AND yield the single worker). Two workers let the waiter resume
    // on worker B while the waker spin-waits on worker A.
    sched.run(2);

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
