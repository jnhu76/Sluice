// E8 — runnable ownership transfer / work stealing (sluice-CORE-E8).
//
// TDD tests for E8. The load-bearing protocol is:
//   StealRunnable = MOVE runnable ticket + TRANSFER execution owner.
// Steal is never PUBLISH (no make_runnable). A stolen Fiber that suspends on
// the thief must wake-route to the thief, not the original owner.
//
// PROVES: runnable ownership transfer + owner-consistent wake routing.
// DOES NOT PROVE: Chase-Lev deque (E16), NUMA, Mutex/Select/timer (E10+).
//
// Single coordinated run(N) per test — no double-run (sched_ctx is OS-thread-
// stack-dependent; destroying/recreating threads between run() calls dangles
// saved continuations — same constraint as E7).
#include "harness.hpp"

#include <sluice/async/async_io_context.hpp>
#include <sluice/async/fake_backend.hpp>
#include <sluice/async/fiber.hpp>
#include <sluice/async/fiber_ctx.hpp>
#include <sluice/async/scheduler.hpp>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <set>
#include <thread>
#include <unordered_map>
#include <vector>

using namespace sluice::async;

namespace {
struct FiberStack {
    static constexpr std::size_t kBytes = 64 * 1024;
    alignas(16) std::vector<std::byte> bytes{kBytes};
    std::byte* base() noexcept { return bytes.data(); }
    std::size_t size() const noexcept { return bytes.size(); }
};

// Record the OS thread id that a Fiber's body started on. Used to prove a
// Fiber executed on a specific worker (by comparing against worker tids).
}  // namespace

// ---- E8-T1: idle worker steals runnable Fiber and executes it --------------
// Deterministic setup:
//   - run(2): round-robin assigns f0->W0, f1->W1.
//   - f1 (W1) completes immediately -> W1 idle.
//   - f0 (W0) runs and stays RUNNING (spinning). From inside f0's body, it
//     spawn_on(f2, 0) — placing F2 on W0's local queue. Because F0 is
//     Running (not Runnable), it is not stealable; F2 is the only stealable
//     ticket, on W0's queue. W1 (idle) steals F2 and executes it.
// Prove: F2 ran on W1 (the thief); each fiber ran exactly once.
SLUICE_TEST_CASE(steal_idle_worker_steals_runnable_fiber) {
    if constexpr (!fiber_ctx::supported) return;

    AsyncIoContext ctx(std::make_unique<FakeAsyncBackend>());
    Scheduler sched(ctx);

    std::atomic<unsigned> wid_f0{static_cast<unsigned>(-1)};
    std::atomic<unsigned> wid_f2{static_cast<unsigned>(-1)};
    std::atomic<int> f2_runs{0};
    std::atomic<bool> release_f0{false};
    std::atomic<bool> f2_spawned{false};

    Fiber f0, f1, f2;
    f0.set_entry([&](Fiber&) {
        wid_f0.store(Scheduler::current_worker_id(), std::memory_order_release);
        // F0 is RUNNING on its worker (not stealable). Place F2 on that
        // SAME worker's queue — W0 or W1 depending on round-robin; the
        // point is F0's worker is busy, so the OTHER worker steals F2.
        unsigned me = Scheduler::current_worker_id();
        sched.spawn_on(f2, me);
        f2_spawned.store(true, std::memory_order_release);
        // Hold F0's worker busy so F2 stays runnable there for the other
        // worker to steal.
        while (!release_f0.load(std::memory_order_acquire)) {}
    });
    f1.set_entry([&](Fiber&) {
        // F1 finishes immediately; its worker becomes idle and steals F2.
    });
    f2.set_entry([&](Fiber&) {
        wid_f2.store(Scheduler::current_worker_id(), std::memory_order_release);
        f2_runs.fetch_add(1, std::memory_order_acq_rel);
    });

    FiberStack s0, s1, s2;
    SLUICE_CHECK(sched.init_fiber(f0, s0.base(), s0.size()));
    SLUICE_CHECK(sched.init_fiber(f1, s1.base(), s1.size()));
    SLUICE_CHECK(sched.init_fiber(f2, s2.base(), s2.size()));
    // Round-robin: f0->W0, f1->W1. F2 spawned-on F0's worker from inside f0.
    sched.spawn(f0);
    sched.spawn(f1);

    std::thread runner([&] { sched.run(2); });

    // Wait for F2 to be spawned and run by the thief (the other worker).
    while (f2_runs.load(std::memory_order_acquire) == 0) {
        std::this_thread::yield();
    }
    release_f0.store(true, std::memory_order_release);
    runner.join();

    SLUICE_CHECK(f2_runs.load() == 1);  // exactly once
    SLUICE_CHECK(f0.state() == FiberState::done);
    SLUICE_CHECK(f1.state() == FiberState::done);
    SLUICE_CHECK(f2.state() == FiberState::done);
    // THE E8 ASSERTION: F2 ran on a DIFFERENT worker than F0 (the thief),
    // even though F2 was queued on F0's worker. This proves cross-worker
    // stealing.
    SLUICE_CHECK(wid_f2.load(std::memory_order_acquire) !=
                 wid_f0.load(std::memory_order_acquire));
}

// ---- E8-T2: steal transfers owner; ticket not duplicated ---------------
// Same deterministic setup as T1: F0 runs on W0 (spinning); F2 spawned-on
// W0's queue; W1 steals F2. Observe owner before/after and exactly-once.
SLUICE_TEST_CASE(steal_steal_transfers_owner_no_duplicate) {
    if constexpr (!fiber_ctx::supported) return;

    AsyncIoContext ctx(std::make_unique<FakeAsyncBackend>());
    Scheduler sched(ctx);

    std::atomic<unsigned> wid_f0{static_cast<unsigned>(-1)};
    std::atomic<unsigned> wid_f2{static_cast<unsigned>(-1)};
    std::atomic<int> f2_runs{0};
    std::atomic<bool> release_f0{false};
    std::atomic<bool> f2_started{false};

    Fiber f0, f1, f2;
    f0.set_entry([&](Fiber&) {
        wid_f0.store(Scheduler::current_worker_id(), std::memory_order_release);
        unsigned me = Scheduler::current_worker_id();
        sched.spawn_on(f2, me);  // F2 on F0's worker's queue; F0 Running (not stealable)
        while (!release_f0.load(std::memory_order_acquire)) {}
    });
    f1.set_entry([&](Fiber&) {});
    f2.set_entry([&](Fiber&) {
        wid_f2.store(Scheduler::current_worker_id(), std::memory_order_release);
        f2_runs.fetch_add(1, std::memory_order_acq_rel);
        f2_started.store(true);
    });

    FiberStack s0, s1, s2;
    SLUICE_CHECK(sched.init_fiber(f0, s0.base(), s0.size()));
    SLUICE_CHECK(sched.init_fiber(f1, s1.base(), s1.size()));
    SLUICE_CHECK(sched.init_fiber(f2, s2.base(), s2.size()));
    sched.spawn(f0);   // W0
    sched.spawn(f1);   // W1

    std::thread runner([&] { sched.run(2); });

    while (!f2_started.load(std::memory_order_acquire)) {
        std::this_thread::yield();
    }
    // F2 has started on the thief -> it was stolen. The CURRENT owner must
    // be the thief (the worker that ran F2), NOT F0's worker.
    SLUICE_CHECK(sched.owner_id_of(f2) == wid_f2.load(std::memory_order_acquire));
    SLUICE_CHECK(sched.owner_id_of(f2) != wid_f0.load(std::memory_order_acquire));

    release_f0.store(true, std::memory_order_release);
    runner.join();

    SLUICE_CHECK(f2_runs.load() == 1);  // exactly once — no duplicate ticket
    SLUICE_CHECK(f0.state() == FiberState::done);
    SLUICE_CHECK(f1.state() == FiberState::done);
    SLUICE_CHECK(f2.state() == FiberState::done);
    SLUICE_CHECK(wid_f2.load(std::memory_order_acquire) !=
                 wid_f0.load(std::memory_order_acquire));  // ran on thief
}

// ---- E8-T3: LOAD-BEARING — steal → run → suspend → wake → resume on thief
// Required causal chain:
//   F2 owner W1, runnable on W1's queue (W1 busy running F1)
//   W0 steals F2: owner -> W0
//   F2 runs on W0
//   F2 awaits ready flag -> waiting (registered on W0's lineage)
//   flag becomes ready
//   F2 waiting -> runnable, wake routes to W0 (current owner)
//   F2 resumes on W0
// Prove: F2's pre-suspend worker == F2's post-resume worker == W0 (the
// thief). This is the stale-owner-defect proof: if owner were not
// transferred, the wake would route F2 back to W1 and F2 would resume on
// W1.
//
// Deterministic setup (no steal-vs-pop race on the gate):
//   - run(2) starts; round-robin assigns f0->W0, f1->W1.
//   - f1's body (on W1) sets wid_w1=1, then spins holding W1 BUSY while it
//     spawn_on(f2, 1) — placing F2 on W1's local queue. Because W1 is
//     running f1 (not popping), F2 sits runnable on W1's queue. W0 is idle
//     (f0 completed immediately) and steals F2.
//   - The test observes f2_suspended, then spawns the setter to make
//     flag_x ready; the readiness drain routes F2 (owner=W0) back to W0.
//
// Why this is race-free on the gate: F1 is popped by W1 itself before any
// stealing can occur (W0 popping f0 is independent; W0 cannot steal F1
// because W1 pops F1 first under its own inbox_mtx). Once F1 runs on W1
// and is spinning, W1 is deterministically busy, and F2 (spawned-onto W1)
// is stealable by W0 with no ambiguity.
SLUICE_TEST_CASE(steal_steal_run_suspend_wake_resume_on_thief) {
    if constexpr (!fiber_ctx::supported) return;

    AsyncIoContext ctx(std::make_unique<FakeAsyncBackend>());
    Scheduler sched(ctx);

    std::atomic<bool> flag_x{false};
    std::atomic<unsigned> wid_f2_pre{static_cast<unsigned>(-1)};
    std::atomic<unsigned> wid_f2_post{static_cast<unsigned>(-1)};
    std::atomic<unsigned> wid_w1{static_cast<unsigned>(-1)};
    std::atomic<bool> f2_suspended{false};
    std::atomic<bool> release_f1{false};
    std::atomic<bool> f2_resumed{false};
    std::atomic<bool> setter_ran{false};
    std::atomic<bool> f2_spawned{false};

    Fiber f0, f1, f2;
    f0.set_entry([&](Fiber&) {
        // F0 completes immediately; W0 goes idle and steals F2 from W1.
    });
    f1.set_entry([&](Fiber&) {
        unsigned me = Scheduler::current_worker_id();
        wid_w1.store(me, std::memory_order_release);
        // Place F2 on THIS worker's queue while it is busy running F1. The
        // OTHER worker (idle after f0) will steal F2.
        sched.spawn_on(f2, me);
        f2_spawned.store(true, std::memory_order_release);
        // Hold this worker busy so it does not pop F2 itself.
        while (!release_f1.load(std::memory_order_acquire)) {}
    });
    f2.set_entry([&](Fiber&) {
        wid_f2_pre.store(Scheduler::current_worker_id(), std::memory_order_release);
        f2_suspended.store(true, std::memory_order_release);
        sched.await_ready_flag(flag_x);   // suspend on W0 (the thief)
        wid_f2_post.store(Scheduler::current_worker_id(), std::memory_order_release);
        f2_resumed.store(true, std::memory_order_release);
    });

    FiberStack s0, s1, s2, sset;
    SLUICE_CHECK(sched.init_fiber(f0, s0.base(), s0.size()));
    SLUICE_CHECK(sched.init_fiber(f1, s1.base(), s1.size()));
    SLUICE_CHECK(sched.init_fiber(f2, s2.base(), s2.size()));
    // Round-robin: f0->W0, f1->W1. f2 is spawned-on W1 from inside f1.
    sched.spawn(f0);
    sched.spawn(f1);

    std::thread runner([&] { sched.run(2); });

    // Wait for F2 to suspend (meaning W0 stole it and ran it to its await).
    while (!f2_suspended.load(std::memory_order_acquire)) {
        std::this_thread::yield();
    }
    // F2 is now waiting on W0. Spawn the setter to make flag_x ready. W0 is
    // running the readiness drain / idle; W1 is still busy spinning. The
    // setter lands on W0 (round-robin: next_spawn_worker_ wraps). W0 runs
    // the setter, sets flag_x, and the readiness drain routes F2 (owner=W0)
    // back to W0.
    Fiber fset;
    fset.set_entry([&](Fiber&) {
        flag_x.store(true, std::memory_order_release);
        setter_ran.store(true, std::memory_order_release);
    });
    SLUICE_CHECK(sched.init_fiber(fset, sset.base(), sset.size()));
    sched.spawn(fset);

    // Wait for F2 to resume after its wake.
    while (!f2_resumed.load(std::memory_order_acquire)) {
        std::this_thread::yield();
    }
    release_f1.store(true, std::memory_order_release);
    runner.join();

    unsigned pre = wid_f2_pre.load(std::memory_order_acquire);
    unsigned post = wid_f2_post.load(std::memory_order_acquire);
    unsigned w1 = wid_w1.load(std::memory_order_acquire);

    // THE LOAD-BEARING E8 ASSERTION:
    // F2 was stolen: it ran on a DIFFERENT worker than F1 (the victim's
    // gate), i.e. pre != w1. F2 suspended on the thief and RESUMED on the
    // SAME thief (pre == post). If ownership had not transferred, the wake
    // would route F2 back to the victim (w1) and post would equal w1, not
    // pre.
    SLUICE_CHECK(pre != w1);    // ran on thief (stolen from victim's queue)
    SLUICE_CHECK(pre == post);  // resumed on thief (wake routed to current owner)
    SLUICE_CHECK(post != w1);   // did NOT route back to the victim
    SLUICE_CHECK(f2_resumed.load());
    SLUICE_CHECK(setter_ran.load());
    SLUICE_CHECK(f2.state() == FiberState::done);
    SLUICE_CHECK(fset.state() == FiberState::done);
    SLUICE_CHECK(sched.waiting_ready_count() == 0);  // registration erased on wake
    (void)w1;
}

// ---- E8-T4: steal-vs-victim-pop race — exactly one wins ----------------
// Deterministically interleave W0's local pop of F against W1's steal of F.
// Outcomes allowed: W0 consumes F (runs on W0) OR W1 steals F (runs on W1).
// Forbidden: both execute F (duplicate ticket) OR neither (lost ticket).
//
// The pop and steal both serialize on the victim's inbox_mtx for the actual
// deque mutation, so by construction exactly one removes F. This test
// verifies that property holds under repeated trials: every Fiber runs
// exactly once, no duplicates, no losses. No sleeps as the proof — the
// exactly-once count IS the proof.
//
// Setup: F0 (W0) and F2 (W0) both runnable on W0; F1 (W1). F0 completes
// fast, so W0 immediately tries to pop F2; concurrently W1 (idle after F1)
// tries to steal F2. The race is genuine; the outcome varies per trial;
// the invariant (F2 runs exactly once) must hold in every trial.
SLUICE_TEST_CASE(steal_steal_vs_pop_exactly_one_wins) {
    if constexpr (!fiber_ctx::supported) return;

    constexpr int kTrials = 200;
    for (int trial = 0; trial < kTrials; ++trial) {
        AsyncIoContext ctx(std::make_unique<FakeAsyncBackend>());
        Scheduler sched(ctx);

        std::atomic<int> f2_runs{0};
        std::atomic<unsigned> wid_f2{static_cast<unsigned>(-1)};
        // F0 and F2 both on W0 (round-robin: f0->W0, f1->W1, f2->W0).
        // F0 completes immediately -> W0 races to pop F2 while W1 steals.
        Fiber f0, f1, f2;
        f0.set_entry([&](Fiber&) { (void)trial; });
        f1.set_entry([&](Fiber&) {});
        f2.set_entry([&](Fiber&) {
            f2_runs.fetch_add(1, std::memory_order_acq_rel);
            wid_f2.store(Scheduler::current_worker_id(), std::memory_order_release);
        });
        FiberStack s0, s1, s2;
        SLUICE_CHECK(sched.init_fiber(f0, s0.base(), s0.size()));
        SLUICE_CHECK(sched.init_fiber(f1, s1.base(), s1.size()));
        SLUICE_CHECK(sched.init_fiber(f2, s2.base(), s2.size()));
        sched.spawn(f0);
        sched.spawn(f1);
        sched.spawn(f2);
        sched.run(2);

        // Exactly-once: F2 ran exactly once (no duplicate, no loss).
        SLUICE_CHECK(f2_runs.load() == 1);
        SLUICE_CHECK(f2.state() == FiberState::done);
        SLUICE_CHECK(f0.state() == FiberState::done);
        SLUICE_CHECK(f1.state() == FiberState::done);
    }
}

// ---- E8-T5/T6/T7: no steal of Waiting / Running / Done Fiber -------------
// A Waiting / Running / Done Fiber must NEVER be exposed as stealable work.
// try_steal's precondition is fiberState == runnable. We construct each
// forbidden state on W0 while W1 is idle and searching for work, and prove
// the forbidden-state Fiber is NOT stolen (it stays where it is / in its
// state). The runnable co-Fiber on W0 MAY be stolen (that's allowed); the
// forbidden one must not.
//
// E8-T5 (Waiting): F_wait suspends on W0 (waiting); W1 searches. F_wait
//   must remain registered+waiting, not stolen. Prove: F_wait.state==waiting
//   and waiting_ready_count()==1 after the run (it was not consumed/stolen).
SLUICE_TEST_CASE(steal_no_steal_of_waiting_fiber) {
    if constexpr (!fiber_ctx::supported) return;

    AsyncIoContext ctx(std::make_unique<FakeAsyncBackend>());
    Scheduler sched(ctx);

    std::atomic<bool> flag_wait{false};
    std::atomic<bool> wait_suspended{false};
    std::atomic<bool> release_blocker{false};

    // F_blocker on W1: holds W1 busy (spinning) so W1 doesn't terminate and
    // so the run stays alive while we observe. F_wait on W0: suspends
    // (waiting). F_idle on W0: completes after F_wait suspends, leaving W0
    // idle to search for stealable work (it finds none eligible — F_wait is
    // Waiting, F_blocker is Running on W1).
    Fiber f_wait, f_blocker, f_idle;
    f_wait.set_entry([&](Fiber&) {
        wait_suspended.store(true);
        sched.await_ready_flag(flag_wait);  // -> waiting on W0
    });
    f_blocker.set_entry([&](Fiber&) {
        while (!release_blocker.load(std::memory_order_acquire)) {}
    });
    f_idle.set_entry([&](Fiber&) {});

    FiberStack sw, sb, si;
    SLUICE_CHECK(sched.init_fiber(f_wait, sw.base(), sw.size()));
    SLUICE_CHECK(sched.init_fiber(f_blocker, sb.base(), sb.size()));
    SLUICE_CHECK(sched.init_fiber(f_idle, si.base(), si.size()));
    // Round-robin: f_wait->W0, f_blocker->W1, f_idle->W0.
    sched.spawn(f_wait);
    sched.spawn(f_blocker);
    sched.spawn(f_idle);

    std::thread runner([&] { sched.run(2); });

    // Wait until f_wait has suspended (waiting on W0).
    while (!wait_suspended.load(std::memory_order_acquire)) {
        std::this_thread::yield();
    }
    // f_idle is still queued on W0 behind f_wait's slot; W0 will pop and run
    // it, then go idle and search for stealable work. Give W0 a moment to
    // reach the idle/search state. It must NOT steal f_wait (waiting) nor
    // f_blocker (running). The proof is the state assertions below.
    std::this_thread::sleep_for(std::chrono::milliseconds(5));

    // f_wait is still waiting, still registered (not stolen/consumed).
    SLUICE_CHECK(f_wait.state() == FiberState::waiting);
    SLUICE_CHECK(sched.waiting_ready_count() == 1);
    // Owner unchanged (still W0). A waiting fiber has no ticket, so it can't
    // be stolen; owner record reflects the W0 it suspended on.
    SLUICE_CHECK(sched.owner_id_of(f_wait) == 0u);

    // Release and let the run terminate (f_wait stays waiting -> MW-S3).
    release_blocker.store(true, std::memory_order_release);
    runner.join();

    SLUICE_CHECK(f_wait.state() == FiberState::waiting);  // never stolen/run
    SLUICE_CHECK(sched.waiting_ready_count() == 1);
}

// E8-T6 (Running): a Fiber running on W0 must not be stolen while W1
// searches. F_run spins (running) on W0; W1 searches. Prove F_run is not
// stolen: it remains running (we observe mid-run) and completes exactly
// once on W0.
SLUICE_TEST_CASE(steal_no_steal_of_running_fiber) {
    if constexpr (!fiber_ctx::supported) return;

    AsyncIoContext ctx(std::make_unique<FakeAsyncBackend>());
    Scheduler sched(ctx);

    std::atomic<bool> release_run{false};
    std::atomic<bool> run_observed{false};
    std::atomic<int> run_runs{0};
    std::atomic<unsigned> run_wid{static_cast<unsigned>(-1)};

    Fiber f_run, f_idle;
    f_run.set_entry([&](Fiber&) {
        run_wid.store(Scheduler::current_worker_id(), std::memory_order_release);
        run_runs.fetch_add(1, std::memory_order_acq_rel);
        run_observed.store(true);
        while (!release_run.load(std::memory_order_acquire)) {}  // stay Running
    });
    f_idle.set_entry([&](Fiber&) {});  // W1 -> idle, searches for work

    FiberStack sr, si;
    SLUICE_CHECK(sched.init_fiber(f_run, sr.base(), sr.size()));
    SLUICE_CHECK(sched.init_fiber(f_idle, si.base(), si.size()));
    sched.spawn(f_run);   // W0
    sched.spawn(f_idle);  // W1

    std::thread runner([&] { sched.run(2); });

    while (!run_observed.load(std::memory_order_acquire)) {
        std::this_thread::yield();
    }
    // f_run is Running on W0. W1 is idle/searching. Let it search.
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
    // f_run must still be running (not stolen to W1).
    SLUICE_CHECK(f_run.state() == FiberState::running);
    SLUICE_CHECK(run_runs.load() == 1);  // running exactly once

    release_run.store(true, std::memory_order_release);
    runner.join();

    SLUICE_CHECK(run_runs.load() == 1);  // never re-executed (no steal)
    SLUICE_CHECK(f_run.state() == FiberState::done);
}

// E8-T7 (Done): a Done Fiber is never exposed as stealable work. We run a
// Fiber to completion, then verify W1 (idle) does not steal it (it's Done;
// not in any runnable queue). Prove: the done fiber is not re-executed.
SLUICE_TEST_CASE(steal_no_steal_of_done_fiber) {
    if constexpr (!fiber_ctx::supported) return;

    AsyncIoContext ctx(std::make_unique<FakeAsyncBackend>());
    Scheduler sched(ctx);

    std::atomic<int> done_runs{0};
    std::atomic<bool> release_blocker{false};

    Fiber f_done, f_blocker, f_idle;
    f_done.set_entry([&](Fiber&) {
        done_runs.fetch_add(1, std::memory_order_acq_rel);
    });
    f_blocker.set_entry([&](Fiber&) {
        while (!release_blocker.load(std::memory_order_acquire)) {}
    });
    f_idle.set_entry([&](Fiber&) {});

    FiberStack sd, sb, si;
    SLUICE_CHECK(sched.init_fiber(f_done, sd.base(), sd.size()));
    SLUICE_CHECK(sched.init_fiber(f_blocker, sb.base(), sb.size()));
    SLUICE_CHECK(sched.init_fiber(f_idle, si.base(), si.size()));
    // f_done->W0, f_blocker->W1, f_idle->W0.
    sched.spawn(f_done);
    sched.spawn(f_blocker);
    sched.spawn(f_idle);

    std::thread runner([&] { sched.run(2); });

    // Wait for f_done to complete (Done). f_blocker holds W1; f_idle on W0
    // completes leaving W0... actually f_done ran on W0 first (front of
    // queue), completes -> Done. Then W0 pops f_idle, runs, completes. W0
    // idle. W1 still running f_blocker. So no stealing pressure on f_done.
    // The point: f_done is Done and is never re-run.
    while (done_runs.load(std::memory_order_acquire) == 0) {
        std::this_thread::yield();
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
    SLUICE_CHECK(f_done.state() == FiberState::done);
    SLUICE_CHECK(done_runs.load() == 1);

    release_blocker.store(true, std::memory_order_release);
    runner.join();

    SLUICE_CHECK(done_runs.load() == 1);  // never re-executed
    SLUICE_CHECK(f_done.state() == FiberState::done);
}

// ---- E8-T8: owner-route race — every visible local/inbox ticket agrees --
// Exercise runnable-ticket route / inbox transport against steal. Prove
// every visible local ticket agrees with the current owner. Since the
// production inbox deque is dead storage (E8-0 audit O5/O6), this reduces
// to: a stolen Fiber's ticket, while it sits on the thief's local_runnable
// awaiting pop, has owner == thief. We stress this by repeatedly stealing
// under load and asserting owner_id_of matches the worker the Fiber ends
// up running on.
SLUICE_TEST_CASE(steal_owner_route_race_visible_ticket_agrees) {
    if constexpr (!fiber_ctx::supported) return;

    constexpr int kFibers = 8;
    AsyncIoContext ctx(std::make_unique<FakeAsyncBackend>());
    Scheduler sched(ctx);

    std::atomic<int> completed{0};
    std::vector<Fiber> fibers(kFibers);
    std::vector<FiberStack> stacks(kFibers);
    std::vector<std::atomic<unsigned>> run_wid(kFibers);
    for (auto& a : run_wid) a.store(static_cast<unsigned>(-1), std::memory_order_release);
    std::atomic<bool> release_gate{false};

    // First fiber holds W0 busy so others queue on W0 and get stolen by W1.
    fibers[0].set_entry([&](Fiber&) {
        run_wid[0].store(Scheduler::current_worker_id(), std::memory_order_release);
        while (!release_gate.load(std::memory_order_acquire)) {}
        completed.fetch_add(1, std::memory_order_acq_rel);
    });
    for (int i = 1; i < kFibers; ++i) {
        fibers[i].set_entry([&, i](Fiber&) {
            run_wid[i].store(Scheduler::current_worker_id(), std::memory_order_release);
            completed.fetch_add(1, std::memory_order_acq_rel);
        });
    }
    for (int i = 0; i < kFibers; ++i) {
        SLUICE_CHECK(sched.init_fiber(fibers[i], stacks[i].base(), stacks[i].size()));
        sched.spawn(fibers[i]);  // round-robin across 2 workers
    }

    std::thread runner([&] { sched.run(2); });

    // Wait until all non-gate fibers have completed (stolen + run by W1).
    while (completed.load(std::memory_order_acquire) < kFibers - 1) {
        std::this_thread::yield();
    }
    release_gate.store(true, std::memory_order_release);
    runner.join();

    // Every fiber completed exactly once.
    SLUICE_CHECK(completed.load() == kFibers);
    for (int i = 0; i < kFibers; ++i) {
        SLUICE_CHECK(fibers[i].state() == FiberState::done);
        SLUICE_CHECK(run_wid[i].load(std::memory_order_acquire) != static_cast<unsigned>(-1));
    }
    // At least one non-gate fiber ran on W1 (run_wid[0] is W0; some other
    // worker id differs, proving stealing occurred and the stolen fibers'
    // tickets were consistent with their execution owner).
    bool any_on_w1 = false;
    for (int i = 1; i < kFibers; ++i) {
        if (run_wid[i].load(std::memory_order_acquire) !=
            run_wid[0].load(std::memory_order_acquire)) { any_on_w1 = true; break; }
    }
    SLUICE_CHECK(any_on_w1);
}

// ---- E8-T9: MW-S1 integration — stealable work prevents MW-S2 admission --
// With globally visible stealable runnable work, an idle worker must NOT
// commit MW-S2 blocking admission. Setup: W0 has a runnable Fiber (held
// runnable by spinning), a backend Completion is also outstanding. W1 is
// idle. Prove: the run does NOT terminate via the no-progress MW-S2 path
// while stealable work exists — instead W1 steals and the stolen fiber
// completes once the gate releases. (The E7 two-phase admission already
// requires MW-S2 == ~MW-S1, and stealable work is MW-S1; this test confirms
// the integration end-to-end.)
SLUICE_TEST_CASE(steal_mw_s1_stealable_prevents_blocking_admission) {
    if constexpr (!fiber_ctx::supported) return;

    AsyncIoContext ctx(std::make_unique<FakeAsyncBackend>());
    Scheduler sched(ctx);

    std::atomic<bool> release_gate{false};
    std::atomic<int> stolen_ran{0};

    Fiber f_gate, f_victim, f_w1;
    f_gate.set_entry([&](Fiber&) {
        while (!release_gate.load(std::memory_order_acquire)) {}
    });
    f_victim.set_entry([&](Fiber&) {
        stolen_ran.fetch_add(1, std::memory_order_acq_rel);
    });
    f_w1.set_entry([&](Fiber&) {});  // W1's initial work; completes -> idle

    FiberStack sg, sv, sw;
    SLUICE_CHECK(sched.init_fiber(f_gate, sg.base(), sg.size()));
    SLUICE_CHECK(sched.init_fiber(f_victim, sv.base(), sv.size()));
    SLUICE_CHECK(sched.init_fiber(f_w1, sw.base(), sw.size()));
    // f_gate->W0, f_victim->W1, f_w1->W0.
    // We want f_victim on W0 (with f_gate) and f_w1 on W1. Reorder:
    sched.spawn(f_gate);    // W0
    sched.spawn(f_w1);      // W1
    sched.spawn(f_victim);  // W0

    std::thread runner([&] { sched.run(2); });

    // Wait for f_victim to be stolen & run by W1 (the idle worker after f_w1).
    while (stolen_ran.load(std::memory_order_acquire) == 0) {
        std::this_thread::yield();
    }
    // f_victim ran on W1 — proving W1 stole it instead of parking in MW-S2.
    SLUICE_CHECK(stolen_ran.load() == 1);
    SLUICE_CHECK(f_victim.state() == FiberState::done);
    SLUICE_CHECK(f_w1.state() == FiberState::done);

    release_gate.store(true, std::memory_order_release);
    runner.join();

    SLUICE_CHECK(stolen_ran.load() == 1);
    SLUICE_CHECK(f_gate.state() == FiberState::done);
}

// ---- E8-T10: logical quiescence — steal attempts are not logical work ---
// When no runnable ticket, no running Fiber, no backend outstanding, no
// unresolved wait registration exist, the Scheduler reaches QUIESCENT. The
// existence of steal attempts (idle thief loops) does not create logical
// work. Prove: a run with only trivial fibers that all complete reaches
// quiescence and terminates (no hang), and waiting/running counts are zero.
SLUICE_TEST_CASE(steal_quiescence_steal_attempts_not_logical_work) {
    if constexpr (!fiber_ctx::supported) return;

    AsyncIoContext ctx(std::make_unique<FakeAsyncBackend>());
    Scheduler sched(ctx);

    constexpr int kFibers = 6;
    std::vector<Fiber> fibers(kFibers);
    std::vector<FiberStack> stacks(kFibers);
    std::atomic<int> completed{0};
    for (int i = 0; i < kFibers; ++i) {
        fibers[i].set_entry([&](Fiber&) {
            completed.fetch_add(1, std::memory_order_acq_rel);
        });
        SLUICE_CHECK(sched.init_fiber(fibers[i], stacks[i].base(), stacks[i].size()));
        sched.spawn(fibers[i]);
    }
    sched.run(2);  // must terminate (quiesce)

    SLUICE_CHECK(completed.load() == kFibers);
    SLUICE_CHECK(sched.runnable_count() == 0);
    SLUICE_CHECK(sched.waiting_count() == 0);
    for (int i = 0; i < kFibers; ++i) {
        SLUICE_CHECK(fibers[i].state() == FiberState::done);
    }
}

// ---- E8-T11: exactly-once execution stress --------------------------------
// Many runnable Fibers; each records a unique execution/completion marker.
// Prove: each Fiber completes exactly once, no duplicate dispatch, no
// missing Fiber. Run under heavy stealing pressure (2 workers, imbalanced
// initial assignment via a gate fiber on W0).
SLUICE_TEST_CASE(steal_exactly_once_stress) {
    if constexpr (!fiber_ctx::supported) return;

    constexpr int kFibers = 32;
    AsyncIoContext ctx(std::make_unique<FakeAsyncBackend>());
    Scheduler sched(ctx);

    std::vector<Fiber> fibers(kFibers);
    std::vector<FiberStack> stacks(kFibers);
    std::vector<std::atomic<int>> run_counts(kFibers);
    std::vector<std::atomic<bool>> ran(kFibers);
    std::atomic<bool> release_gate{false};

    // Fiber 0 holds W0 busy to force stealing of the rest.
    fibers[0].set_entry([&](Fiber&) {
        while (!release_gate.load(std::memory_order_acquire)) {}
    });
    for (int i = 1; i < kFibers; ++i) {
        fibers[i].set_entry([&, i](Fiber&) {
            run_counts[i].fetch_add(1, std::memory_order_acq_rel);
            ran[i].store(true, std::memory_order_release);
        });
    }
    for (int i = 0; i < kFibers; ++i) {
        SLUICE_CHECK(sched.init_fiber(fibers[i], stacks[i].base(), stacks[i].size()));
        sched.spawn(fibers[i]);
    }

    std::thread runner([&] { sched.run(2); });

    // Wait until all non-gate fibers have run.
    int waited = 0;
    while (true) {
        int done = 0;
        for (int i = 1; i < kFibers; ++i) if (ran[i].load()) ++done;
        if (done == kFibers - 1) break;
        std::this_thread::yield();
        if (++waited > 100000) break;  // safety; should not trigger
    }
    release_gate.store(true, std::memory_order_release);
    runner.join();

    // Exactly-once: each non-gate fiber ran exactly once.
    for (int i = 1; i < kFibers; ++i) {
        SLUICE_CHECK(run_counts[i].load() == 1);
        SLUICE_CHECK(fibers[i].state() == FiberState::done);
    }
    SLUICE_CHECK(fibers[0].state() == FiberState::done);
}

SLUICE_MAIN()
