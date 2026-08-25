// issue115_runnable_publication_wake_test — Issue #115 deterministic causal
// regression: runnable publication onto a BUSY worker's queue must publish a
// Scheduler wake obligation (RP-1/RP-2), not only a target-inbox notify.
//
// Defect class (pre-fix master, docs/history/issues/issue-115-runnable-publication-wake.md):
//   Scheduler::spawn() / spawn_on() pushed the runnable ticket onto the
//   target worker's local_runnable and called target->inbox_cv.notify_one().
//   NO thread ever waits on inbox_cv (all parks are on the unified Scheduler
//   wake domain wake_cv_ / the backend wait_one domain), and neither path
//   advanced wake_epoch_. A worker that had already committed its unbounded
//   wake-domain park (baseline recorded, predicate: epoch / terminate / OWN
//   inbox only) therefore slept through the publication: the ticket was
//   stealable, a live steal-capable worker existed, and the run stranded.
//
// Deterministic construction (no sleep-ordering; seams + bounded watchdogs
// only, per production-test-plan.md §1):
//   W1 executes F1 which blocks on a TEST rendezvous (a std::condition_variable
//   inside user fiber code — W1 stays a running observer and cannot drain its
//   queue). W0 reaches the exact post-baseline park boundary and is held by
//   the scheduler_park_baseline_recorded seam (baseline committed, no locks
//   held, cv not yet entered). ONLY THEN does the coordinator publish F2 onto
//   W1's queue (production spawn() in test A, spawn_on() in test B) and
//   release the seam. The publication is strictly post-commit: the ONLY
//   possible transport to the sleeping W0 is the cv predicate's
//   wake_epoch_ != observed_epoch check.
//
//     PRE-FIX : no epoch advance -> W0 enters cv.wait, predicate false ->
//               permanent strand -> the bounded watchdog fail-closes, then
//               rescues the run via the EXTERNAL wake handle (production
//               API) so the binary reports failure instead of hanging.
//     POST-FIX: publication advances wake_epoch_ (state first, then wake) ->
//               predicate fires at wait entry -> W0 re-loops, steals F2 from
//               W1, executes it.
//
//   Role pinning: F0 (on W0) blocks on a second rendezvous until F1 has
//   STARTED on W1. This guarantees a running observer exists on W1 before W0
//   can go idle, so W0's park-commit recheck passes (mw_s1 delegation) and —
//   critically — W0 can never steal F1 from W1's queue: F1 is already popped
//   and Running. Without this pin the worker roles are not deterministic.
//
// A third case covers the normal-delivery counterpart (idle target): the
// publication's target worker is the parked one itself; its own-inbox
// predicate backstop must still deliver (and the new epoch signal must be
// coalescing-safe with that path).
//
// Gated to fiber-capable targets (fiber_ctx::supported), like the E8/E13 suites.
#include <sluice/async/async_io_context.hpp>
#include <sluice/async/fake_backend.hpp>
#include <sluice/async/fiber.hpp>
#include <sluice/async/fiber_ctx.hpp>
#include <sluice/async/scheduler.hpp>

#include "async_test_control.hpp"
#include "harness.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <mutex>
#include <thread>
#include <vector>

namespace sa = sluice::async;
namespace stest = sluice_async_test;

using Scheduler = sa::Scheduler;
using Fiber = sa::Fiber;
using FiberState = sa::FiberState;

namespace {

struct FiberStack {
    static constexpr std::size_t kBytes = 64 * 1024;
    alignas(16) std::vector<std::byte> bytes{kBytes};
    std::byte* base() noexcept { return bytes.data(); }
    std::size_t size() const noexcept { return bytes.size(); }
};

// Test-side rendezvous: blocks an OS worker thread INSIDE user fiber code
// (the running observer of the #115 shape) without busy-spinning.
struct Rendezvous {
    std::mutex mtx;
    std::condition_variable cv;
    bool released = false;
    void wait() {
        std::unique_lock<std::mutex> lk(mtx);
        cv.wait(lk, [this] { return released; });
    }
    void release() {
        {
            std::lock_guard<std::mutex> lk(mtx);
            released = true;
        }
        cv.notify_all();
    }
};

// Bounded wait on an atomic flag. WATCHDOG ONLY — never an ordering proof.
// Returns false on timeout (the caller fail-closes and rescues the run).
bool wait_flag(const std::atomic<bool>& flag, std::chrono::milliseconds bound) {
    const auto deadline = std::chrono::steady_clock::now() + bound;
    while (!flag.load(std::memory_order_acquire)) {
        if (std::chrono::steady_clock::now() >= deadline) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return true;
}

constexpr auto kWatchdog = std::chrono::milliseconds(10000);

}  // namespace

// ---- Test A: production spawn() publishes the wake obligation -------------
// The stranded fiber is published through Scheduler::spawn() — the production
// admission path — with the round-robin target deterministically positioned
// on the BUSY worker: F0 (on W0) first spawns a filler through the same
// production path, consuming round-robin slot 0 (the filler lands on W0's own
// queue and W0 pops it itself before parking), so the coordinator's spawn()
// takes slot 1 == W1. The seam hold proves W0 finished F0 + filler before the
// load-bearing publication (same-thread, sequential).
SLUICE_TEST_CASE(issue115_spawn_wakes_parked_peer_busy_target) {
    if constexpr (!sa::fiber_ctx::supported) return;

    sa::AsyncIoContext ctx(std::make_unique<sa::FakeAsyncBackend>());
    Scheduler sched(ctx);
    stest::ControllerGuard ctrl(sched);

    Rendezvous hold_f1;
    Rendezvous f0_until_f1_started;
    std::atomic<bool> f1_started{false};
    std::atomic<bool> f2_ran{false};
    std::atomic<int> f2_runs{0};
    std::atomic<unsigned> wid_f0{static_cast<unsigned>(-1)};
    std::atomic<unsigned> wid_f1{static_cast<unsigned>(-1)};
    std::atomic<unsigned> wid_f2{static_cast<unsigned>(-1)};

    Fiber f0, f_fill, f1, f2;
    f0.set_entry([&](Fiber&) {
        wid_f0.store(Scheduler::current_worker_id(), std::memory_order_release);
        // Pin the roles: stay a running observer on W0 until F1 has STARTED
        // on W1 (F1 popped + Running — unstealable; W1 busy for good).
        f0_until_f1_started.wait();
        // Consume round-robin slot 0 via the production path. The filler
        // lands on THIS worker's queue; W0 pops it itself before idling.
        sched.spawn(f_fill);
    });
    f_fill.set_entry([&](Fiber&) {});
    f1.set_entry([&](Fiber&) {
        wid_f1.store(Scheduler::current_worker_id(), std::memory_order_release);
        f1_started.store(true, std::memory_order_release);
        hold_f1.wait();  // W1 stays a running observer; cannot drain its queue
    });
    f2.set_entry([&](Fiber&) {
        wid_f2.store(Scheduler::current_worker_id(), std::memory_order_release);
        f2_runs.fetch_add(1, std::memory_order_acq_rel);
        f2_ran.store(true, std::memory_order_release);
    });

    FiberStack s0, sfill, s1, s2;
    SLUICE_CHECK(sched.init_fiber(f0, s0.base(), s0.size()));
    SLUICE_CHECK(sched.init_fiber(f_fill, sfill.base(), sfill.size()));
    SLUICE_CHECK(sched.init_fiber(f1, s1.base(), s1.size()));
    SLUICE_CHECK(sched.init_fiber(f2, s2.base(), s2.size()));
    // Pre-run spawns: no active participants -> pending_spawn_; run(2)
    // distributes f0->W0, f1->W1 (distribute order == spawn order).
    sched.spawn(f0);
    sched.spawn(f1);

    // Arm the post-baseline park seam BEFORE the run: whoever commits a
    // wake-domain park pauses with its baseline recorded and no locks held.
    stest::SchedulerParkBaselineSeam::arm(sched);

    std::thread runner([&] { sched.run(2); });

    // W1 is inside F1 (running observer), held on the test rendezvous.
    SLUICE_CHECK(wait_flag(f1_started, kWatchdog));
    f0_until_f1_started.release();  // W0 may now finish F0 + filler and park
    // W0 committed its park: baseline recorded, cv not yet entered.
    stest::SchedulerParkBaselineSeam::wait_paused(sched);

    // THE LOAD-BEARING PRODUCTION PUBLICATION: post-commit, onto busy W1
    // (round-robin slot 1 — F0's filler consumed slot 0).
    sched.spawn(f2);

    // Release the park boundary: W0 enters cv.wait and must observe the
    // publication through the wake epoch (post-fix) or sleep into the #115
    // strand (pre-fix).
    stest::SchedulerParkBaselineSeam::release(sched);

    const bool progressed = wait_flag(f2_ran, kWatchdog);
    if (!progressed) {
        // #115 reproduced: F2 runnable on busy W1's queue, steal-capable W0
        // asleep, no wake reason. Fail-closed evidence, then rescue the run
        // through the EXTERNAL wake API so the binary reports cleanly.
        sched.make_wake_handle().notify();
        hold_f1.release();
        SLUICE_CHECK(wait_flag(f2_ran, kWatchdog));
    } else {
        hold_f1.release();
    }
    runner.join();

    SLUICE_CHECK(progressed);  // publication alone must progress F2
    SLUICE_CHECK(f2_runs.load() == 1);
    SLUICE_CHECK(f2.state() == FiberState::done);
    SLUICE_CHECK(f0.state() == FiberState::done);
    SLUICE_CHECK(f_fill.state() == FiberState::done);
    SLUICE_CHECK(f1.state() == FiberState::done);
    // F2 executed on the previously parked worker (the thief), not on the
    // busy owner — and the steal transferred ownership to that worker.
    const unsigned parked = wid_f0.load(std::memory_order_acquire);
    const unsigned busy = wid_f1.load(std::memory_order_acquire);
    const unsigned ran_on = wid_f2.load(std::memory_order_acquire);
    SLUICE_CHECK(busy != parked);       // roles pinned: F1 on the other worker
    SLUICE_CHECK(ran_on == parked);
    SLUICE_CHECK(ran_on != busy);
    SLUICE_CHECK(sched.owner_id_of(f2) == ran_on);
}

// ---- Test B: spawn_on() explicit busy target — the #115-named shape --------
SLUICE_TEST_CASE(issue115_spawn_on_wakes_parked_peer_busy_target) {
    if constexpr (!sa::fiber_ctx::supported) return;

    sa::AsyncIoContext ctx(std::make_unique<sa::FakeAsyncBackend>());
    Scheduler sched(ctx);
    stest::ControllerGuard ctrl(sched);

    Rendezvous hold_f1;
    Rendezvous f0_until_f1_started;
    std::atomic<bool> f1_started{false};
    std::atomic<bool> f2_ran{false};
    std::atomic<int> f2_runs{0};
    std::atomic<unsigned> wid_f0{static_cast<unsigned>(-1)};
    std::atomic<unsigned> wid_f1{static_cast<unsigned>(-1)};
    std::atomic<unsigned> wid_f2{static_cast<unsigned>(-1)};

    Fiber f0, f1, f2;
    f0.set_entry([&](Fiber&) {
        wid_f0.store(Scheduler::current_worker_id(), std::memory_order_release);
        // Pin the roles (see test A), then complete: W0 idles -> mw_s1
        // (F1 running on W1) -> park.
        f0_until_f1_started.wait();
    });
    f1.set_entry([&](Fiber&) {
        wid_f1.store(Scheduler::current_worker_id(), std::memory_order_release);
        f1_started.store(true, std::memory_order_release);
        hold_f1.wait();
    });
    f2.set_entry([&](Fiber&) {
        wid_f2.store(Scheduler::current_worker_id(), std::memory_order_release);
        f2_runs.fetch_add(1, std::memory_order_acq_rel);
        f2_ran.store(true, std::memory_order_release);
    });

    FiberStack s0, s1, s2;
    SLUICE_CHECK(sched.init_fiber(f0, s0.base(), s0.size()));
    SLUICE_CHECK(sched.init_fiber(f1, s1.base(), s1.size()));
    SLUICE_CHECK(sched.init_fiber(f2, s2.base(), s2.size()));
    sched.spawn(f0);
    sched.spawn(f1);

    stest::SchedulerParkBaselineSeam::arm(sched);

    std::thread runner([&] { sched.run(2); });

    SLUICE_CHECK(wait_flag(f1_started, kWatchdog));
    f0_until_f1_started.release();
    stest::SchedulerParkBaselineSeam::wait_paused(sched);

    // Explicit-target publication onto the BUSY worker, strictly post-commit.
    sched.spawn_on(f2, /*worker_id=*/1);

    stest::SchedulerParkBaselineSeam::release(sched);

    const bool progressed = wait_flag(f2_ran, kWatchdog);
    if (!progressed) {
        sched.make_wake_handle().notify();
        hold_f1.release();
        SLUICE_CHECK(wait_flag(f2_ran, kWatchdog));
    } else {
        hold_f1.release();
    }
    runner.join();

    SLUICE_CHECK(progressed);
    SLUICE_CHECK(f2_runs.load() == 1);
    SLUICE_CHECK(f2.state() == FiberState::done);
    SLUICE_CHECK(f0.state() == FiberState::done);
    SLUICE_CHECK(f1.state() == FiberState::done);
    const unsigned parked = wid_f0.load(std::memory_order_acquire);
    const unsigned busy = wid_f1.load(std::memory_order_acquire);
    const unsigned ran_on = wid_f2.load(std::memory_order_acquire);
    SLUICE_CHECK(busy != parked);       // roles pinned: F1 on the other worker
    SLUICE_CHECK(ran_on == parked);     // the parked peer stole + executed
    SLUICE_CHECK(sched.owner_id_of(f2) == ran_on);
}

// ---- Test C: idle target — normal delivery must survive the new signal ----
// The publication's target is the parked worker ITSELF (Phase 6 case 1). The
// park predicate's own-inbox backstop already covers this pre-fix; post-fix
// the wake-epoch signal coexists with it. Prove: F2 is delivered exactly once
// (no duplicate from signal + backstop double coverage), the run converges.
SLUICE_TEST_CASE(issue115_spawn_on_idle_target_delivers_once) {
    if constexpr (!sa::fiber_ctx::supported) return;

    sa::AsyncIoContext ctx(std::make_unique<sa::FakeAsyncBackend>());
    Scheduler sched(ctx);

    Rendezvous hold_f0;
    std::atomic<bool> f0_started{false};
    std::atomic<bool> f2_ran{false};
    std::atomic<int> f2_runs{0};
    std::atomic<unsigned> wid_f0{static_cast<unsigned>(-1)};
    std::atomic<unsigned> wid_f2{static_cast<unsigned>(-1)};

    Fiber f0, f2;
    f0.set_entry([&](Fiber&) {
        wid_f0.store(Scheduler::current_worker_id(), std::memory_order_release);
        f0_started.store(true, std::memory_order_release);
        hold_f0.wait();  // running observer keeps the run resident
    });
    f2.set_entry([&](Fiber&) {
        wid_f2.store(Scheduler::current_worker_id(), std::memory_order_release);
        f2_runs.fetch_add(1, std::memory_order_acq_rel);
        f2_ran.store(true, std::memory_order_release);
    });

    FiberStack s0, s2;
    SLUICE_CHECK(sched.init_fiber(f0, s0.base(), s0.size()));
    SLUICE_CHECK(sched.init_fiber(f2, s2.base(), s2.size()));
    // Only f0 is admitted pre-run (pending_spawn_ — case 3: no live wake
    // consumer required while no invocation is active); run(2) distributes
    // f0->W0 before any worker thread starts.
    sched.spawn(f0);

    std::thread runner([&] { sched.run(2); });

    // W0 blocks inside f0 from its first pop, so it can never steal f2. Once
    // f0 runs, W1 is idle — looping toward or already in its wake-domain
    // park. The mid-run publication below targets W1: whether it lands while
    // W1 is mid-loop (direct pop) or parked (own-inbox backstop + the new
    // epoch signal), W1 must execute f2 exactly once.
    SLUICE_CHECK(wait_flag(f0_started, kWatchdog));
    sched.spawn_on(f2, /*worker_id=*/1);
    const bool progressed = wait_flag(f2_ran, kWatchdog);
    if (!progressed) {
        // Pre-fix strand shape (target parked before publication): fail
        // closed, then rescue via the external wake API for a clean exit.
        sched.make_wake_handle().notify();
        SLUICE_CHECK(wait_flag(f2_ran, kWatchdog));
    }

    hold_f0.release();
    runner.join();

    SLUICE_CHECK(progressed);  // publication alone must deliver to the target
    SLUICE_CHECK(f2_runs.load() == 1);  // exactly once — no double delivery
    SLUICE_CHECK(f2.state() == FiberState::done);
    SLUICE_CHECK(f0.state() == FiberState::done);
    SLUICE_CHECK(wid_f2.load(std::memory_order_acquire) !=
                 wid_f0.load(std::memory_order_acquire));  // the other worker
}

// ---- Test D: pre-baseline publication must REFUSE the park (G1) -------------
// The other half of #115: the publication lands entirely BEFORE the park's
// baseline record — while the parking worker sits at the park-commit seam
// (strictly pre-baseline, no locks held). Its wake signal (epoch E -> E+1)
// is then CONSUMED by the baseline the worker records on resume (baseline =
// E+1; the cv predicate compares against it and never fires), so the ONLY
// remaining transport is the G1 persistent-state recheck under global_mtx_.
// That recheck must not be short-circuited by the observer exemption: a
// running Fiber (F1 on busy W1) is an observer for ITSELF, not for another
// runnable ticket (F2) queued behind it. Deterministic by construction: the
// seam holds W0 at the commit boundary with the publication strictly before
// the baseline; current-HEAD strands (watchdog rescue fails the case), the
// fixed G1 refuses the park -> re-loop -> steal -> F2 executes exactly once.
SLUICE_TEST_CASE(issue115_absorbed_publication_refuses_park) {
    if constexpr (!sa::fiber_ctx::supported) return;

    sa::AsyncIoContext ctx(std::make_unique<sa::FakeAsyncBackend>());
    Scheduler sched(ctx);
    stest::ControllerGuard ctrl(sched);

    Rendezvous hold_f1;
    Rendezvous f0_until_f1_started;
    std::atomic<bool> f1_started{false};
    std::atomic<bool> f2_ran{false};
    std::atomic<int> f2_runs{0};
    std::atomic<unsigned> wid_f0{static_cast<unsigned>(-1)};
    std::atomic<unsigned> wid_f1{static_cast<unsigned>(-1)};
    std::atomic<unsigned> wid_f2{static_cast<unsigned>(-1)};

    Fiber f0, f1, f2;
    f0.set_entry([&](Fiber&) {
        wid_f0.store(Scheduler::current_worker_id(), std::memory_order_release);
        // Pin the roles (see test A): stay a running observer on W0 until F1
        // has STARTED on W1 (popped + Running, unstealable), so W0's later
        // steal attempt provably fails against an empty W1 queue.
        f0_until_f1_started.wait();
    });
    f1.set_entry([&](Fiber&) {
        wid_f1.store(Scheduler::current_worker_id(), std::memory_order_release);
        f1_started.store(true, std::memory_order_release);
        hold_f1.wait();  // W1 stays a running observer; cannot drain its queue
    });
    f2.set_entry([&](Fiber&) {
        wid_f2.store(Scheduler::current_worker_id(), std::memory_order_release);
        f2_runs.fetch_add(1, std::memory_order_acq_rel);
        f2_ran.store(true, std::memory_order_release);
    });

    FiberStack s0, s1, s2;
    SLUICE_CHECK(sched.init_fiber(f0, s0.base(), s0.size()));
    SLUICE_CHECK(sched.init_fiber(f1, s1.base(), s1.size()));
    SLUICE_CHECK(sched.init_fiber(f2, s2.base(), s2.size()));
    // Pre-run spawns -> pending_spawn_; run(2) distributes f0->W0, f1->W1.
    sched.spawn(f0);
    sched.spawn(f1);

    // Arm the PRE-baseline park-commit seam: whoever commits a wake-domain
    // park pauses BEFORE the G1 recheck / baseline record, holding no locks.
    stest::SchedulerParkSeam::arm_commit(sched);

    std::thread runner([&] { sched.run(2); });

    // W1 is inside F1 (running observer), held on the test rendezvous.
    SLUICE_CHECK(wait_flag(f1_started, kWatchdog));
    f0_until_f1_started.release();  // W0 finishes F0, fails its steal, parks

    // W0 sits at the park-commit boundary: post last-steal, pre everything.
    stest::SchedulerParkSeam::wait_commit_paused(sched);

    // THE LOAD-BEARING PUBLICATION: strictly BEFORE the baseline, onto busy
    // W1. Its epoch signal will be absorbed by the baseline W0 is about to
    // record; the G1 persistent-state recheck is the only transport left.
    sched.spawn_on(f2, /*worker_id=*/1);

    // Release the commit boundary: W0 runs the G1 recheck. It MUST see F2 on
    // W1's queue, REFUSE the park, re-loop, and steal F2 (the absorbed epoch
    // cannot help it; no other wake source exists in this shape).
    stest::SchedulerParkSeam::release_commit(sched);

    const bool progressed = wait_flag(f2_ran, kWatchdog);
    if (!progressed) {
        // Absorbed-baseline strand reproduced: F2 runnable on busy W1's
        // queue, steal-capable W0 asleep, epoch consumed by its baseline.
        // Fail-closed evidence, then rescue through the EXTERNAL wake API.
        sched.make_wake_handle().notify();
        hold_f1.release();
        SLUICE_CHECK(wait_flag(f2_ran, kWatchdog));
    } else {
        hold_f1.release();
    }
    runner.join();

    SLUICE_CHECK(progressed);  // the G1 refusal alone must progress F2
    SLUICE_CHECK(f2_runs.load() == 1);
    SLUICE_CHECK(f2.state() == FiberState::done);
    SLUICE_CHECK(f0.state() == FiberState::done);
    SLUICE_CHECK(f1.state() == FiberState::done);
    const unsigned parked = wid_f0.load(std::memory_order_acquire);
    const unsigned busy = wid_f1.load(std::memory_order_acquire);
    const unsigned ran_on = wid_f2.load(std::memory_order_acquire);
    SLUICE_CHECK(busy != parked);       // roles pinned: F1 on the other worker
    SLUICE_CHECK(ran_on == parked);     // the refuser re-looped and STOLE F2
    SLUICE_CHECK(ran_on != busy);       // the busy owner never drained it
    SLUICE_CHECK(sched.owner_id_of(f2) == ran_on);  // steal transferred owner
}

SLUICE_MAIN()
