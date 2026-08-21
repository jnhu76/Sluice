// issue161_pub_erase_orphan_test — Issue #161 B4-reclassification causal
// regression: the ROUTE-PUBLICATION idle-count erase (route_runnable_locked)
// orphans a live not-last dance contribution exactly like the two unlocked
// ticketed erases, and a missing generation bump there re-opens the permanent
// all-work-complete stall (the async_rwlock T22 hang shape).
//
// Defect class (TLC B4NoBumpPubErase counterexample in
// spec/tla/e12_rwlock_scheduler_liveness/, exposed by the split-window model
// round):
//   The B4 site classification marked the route-publication erase
//   "self-guarded by the G-atomic erase+signal pair". That verdict was an
//   artifact of the pre-split model's coarser interleaving. With the erase
//   and the generation bump modeled as the two distinct steps the C++
//   performs, TLC finds the stuck shape with the ROUTE erase as the
//   orphaning site: a not-last dance contribution is live (idle=1) when a
//   worker's rwlock write-unlock grants a queued reader and routes its
//   ticket — route_runnable_locked erases the count with NO generation
//   bump, so the contributor's 1-bit flag still claims a live identity.
//   The eraser's own later not-last dance (fetch_add from the erased zero,
//   idle=1 again) emits the run's LAST signal; the orphaned contributor's
//   LATE park commit sees idle == own (1 == 1) and a still-matching
//   generation, arms a baseline AFTER that signal, and absorbs it. Both
//   workers sleep with every fiber complete, idle(1) < live(2), terminate
//   false: the run(2) join hangs.
//
// Deterministic construction (zero sleep-ordering; per-worker seams +
// persistent-state barriers + bounded fail-closed waits, per the #115/#161
// discipline). Roles: FW's runner is the ERASER; FA's runner is the DANCER
// (both discovered from recorded worker ids, never assumed — the startup
// pop races and a steal can move a ticket).
//   1. run(2); FA (pre-run spawn) runs on the dancer and blocks INSIDE user
//      fiber code at a rendezvous — the worker thread is physically held, so
//      the dancer can neither classify nor dance while the eraser walks FW
//      and FR into position.
//   2. FW onto the eraser: write_lock on the free lock (immediate), record
//      fw_locked (BARRIER B1: the write lock is HELD), then
//      await_ready_flag(flag_w=false) suspends (registered wait).
//   3. FR onto the eraser: read_lock queues behind the writer and suspends
//      (BARRIER B2: fr.state() == waiting — the queue registration is
//      persistent state; the eraser meanwhile re-classifies MW-S1 beside
//      the running FA and parks).
//   4. Arm worker_ticket_erase_done FOR THE ERASER (per-worker: the dancer
//      must pass the same site unpaused later) and scheduler_park_candidate
//      FOR THE DANCER, plus the GLOBAL scheduler_park_baseline_recorded
//      seam (whoever parks next pauses strictly AFTER its baseline commit —
//      for this choreography that is the eraser's post-dance park, because
//      the dancer is pinned at the candidate seam until after it).
//   5. flag_w := true + external wake notify: the parked eraser is the ONLY
//      runnable worker (the dancer is physically blocked in FA), so IT
//      drains the flag — the drain routes FW's ticket (idle=0: nothing live
//      yet, this route's own erase is a no-op), the route's wake walks the
//      eraser to the pop, and the erase-done seam holds it strictly AFTER
//      the pop-erase and strictly BEFORE run_next_on: FW is
//      popped-invisible, running 0.
//   6. release FA's rendezvous: FA completes; the dancer classifies MW-S3
//      (FR's queue registration remains, nothing runnable/running),
//      contributes a not-last dance count (idle=1) and is held at the
//      park-candidate boundary — BEFORE its commit recheck.
//   7. release the erase-done seam: the eraser runs FW to the unlock; the
//      unlock grants FR and ROUTES its ticket — route_runnable_locked
//      erases the LIVE idle count (idle_before=1; pre-fix: store(0), no
//      generation bump — the contribution is ORPHANED. Post-fix:
//      exchange(0) + dance_epoch_ bump — the identity is invalidated). FR
//      resumes on the eraser and completes; the eraser re-classifies
//      QUIESCENT (no registrations remain), contributes its own not-last
//      count (idle=1 again) whose signal is the run's LAST, commits its
//      baseline and pauses at the baseline seam — PROOF the last signal was
//      emitted before the dancer's commit is released.
//   8. release the candidate seam: the dancer's park commit re-check sees
//      idle == own (1 == 1) and — pre-fix — a still-matching generation, so
//      it arms a baseline AFTER the eraser's last signal and absorbs it.
//      Both workers asleep/paused, work complete, no producer.
//
//     PRE-FIX : permanent run() stall -> the bounded watchdog fail-closes,
//               dumps the park-state evidence, releases the baseline seam
//               and rescues the run via the EXTERNAL wake handle
//               (production API) so the binary reports the failure instead
//               of hanging.
//     POST-FIX: the route-publication erase bumped the contribution
//               generation; the dancer's park commit refuses on the stale
//               identity (signal + re-loop), the re-dance reaches the
//               last-idle threshold (2 == live), termination releases the
//               eraser's baseline pause (release_all_phases), and run(2)
//               returns normally.
//
// Mirrors the TLC B4NoBumpPubErase counterexample state sequence:
// DanceContribute(not-last) -> pub EraseIdle -> work completes -> eraser
// DanceContribute(not-last) -> stale contributor ParkCommit -> ArmBaseline ->
// both Parked.
//
// All choreography waits are BOUNDED and fail closed (dump + rescue): a
// construction that cannot reach its next seam reports the state and fails
// the case instead of blocking the suite forever. NOTE: the per-worker park
// domain is NOT used as a choreography barrier — it stays stale after a wake
// (an earlier draft used it as an "FR queued" barrier and armed the pop seam
// one pass early, silently pinning the wrong pop; the fiber-state barrier B2
// replaced it).
//
// Gated to fiber-capable targets (fiber_ctx::supported), like the E8/#115/#161
// suites.
#include <sluice/async/async_io_context.hpp>
#include <sluice/async/async_rwlock.hpp>
#include <sluice/async/fake_backend.hpp>
#include <sluice/async/fiber.hpp>
#include <sluice/async/fiber_ctx.hpp>
#include <sluice/async/scheduler.hpp>
#include <sluice/async/wait_node.hpp>

#include "async_test_control.hpp"
#include "harness.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdio>
#include <mutex>
#include <thread>
#include <vector>

namespace sa = sluice::async;
namespace stest = sluice_async_test;

using Scheduler = sa::Scheduler;
using Fiber = sa::Fiber;
using FiberState = sa::FiberState;
using WorkerState = sa::WorkerState;
using AsyncTestAccess = Scheduler::AsyncTestAccess;

namespace {

struct FiberStack {
    static constexpr std::size_t kBytes = 64 * 1024;
    alignas(16) std::vector<std::byte> bytes{kBytes};
    std::byte* base() noexcept { return bytes.data(); }
    std::size_t size() const noexcept { return bytes.size(); }
};

// Test-side rendezvous: blocks an OS worker thread INSIDE user fiber code
// (the running observer of this construction) without busy-spinning.
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

// Bounded wait on a Fiber reaching the waiting state (BARRIER B2: the wait
// registration is persistent state, so observing it proves the fiber suspended
// — never a timing assumption). Returns false on timeout (fail closed).
bool wait_fiber_waiting(const Fiber& f, std::chrono::milliseconds bound) {
    const auto deadline = std::chrono::steady_clock::now() + bound;
    while (f.state() != FiberState::waiting) {
        if (std::chrono::steady_clock::now() >= deadline) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return true;
}

constexpr auto kWatchdog = std::chrono::milliseconds(10000);

// Bounded seam waits (fail-closed): a choreography that cannot reach its
// next seam must fail the case, never block the suite forever.
bool wait_erase_done_seam_paused(Scheduler& s) {
    const auto deadline = std::chrono::steady_clock::now() + kWatchdog;
    while (!stest::Issue161EraseDoneSeam::is_paused(s)) {
        if (std::chrono::steady_clock::now() >= deadline) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return true;
}

bool wait_candidate_seam_paused(Scheduler& s) {
    const auto deadline = std::chrono::steady_clock::now() + kWatchdog;
    while (!stest::Issue161CandidateSeam::is_paused(s)) {
        if (std::chrono::steady_clock::now() >= deadline) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return true;
}

bool wait_baseline_seam_paused(Scheduler& s) {
    const auto deadline = std::chrono::steady_clock::now() + kWatchdog;
    while (!stest::SchedulerParkBaselineSeam::is_paused(s)) {
        if (std::chrono::steady_clock::now() >= deadline) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return true;
}

}  // namespace

// ---- Test: the route-publication erase orphans a live contribution --------
SLUICE_TEST_CASE(issue161_pub_erase_orphans_dance_contribution) {
    if constexpr (!sa::fiber_ctx::supported) return;

    sa::AsyncIoContext ctx(std::make_unique<sa::FakeAsyncBackend>());
    Scheduler sched(ctx);
    stest::ControllerGuard ctrl(sched);
    sa::AsyncRwLock rw(sched);

    Rendezvous hold_fa;
    std::atomic<bool> flag_w{false};
    std::atomic<bool> fa_started{false};
    std::atomic<bool> fw_locked{false};
    std::atomic<bool> fw_unlocked{false};
    std::atomic<bool> fr_resumed{false};
    std::atomic<bool> run_returned{false};
    std::atomic<unsigned> wid_fa{static_cast<unsigned>(-1)};
    std::atomic<unsigned> wid_fw{static_cast<unsigned>(-1)};
    std::atomic<unsigned> wid_fr{static_cast<unsigned>(-1)};

    Fiber fa, fw, fr;
    fa.set_entry([&](Fiber&) {
        wid_fa.store(Scheduler::current_worker_id(), std::memory_order_release);
        fa_started.store(true, std::memory_order_release);
        hold_fa.wait();  // the dancer stays a running observer while the
                         // eraser walks FW into the flag wait and FR into
                         // the lock queue
    });
    fw.set_entry([&](Fiber&) {
        sa::WaitNode wn;
        wid_fw.store(Scheduler::current_worker_id(), std::memory_order_release);
        rw.write_lock(wn);          // free lock: immediate, no suspension
        fw_locked.store(true, std::memory_order_release);  // BARRIER B1
        sched.await_ready_flag(flag_w);  // false: suspends (registered wait)
        rw.unlock_write();          // grants FR: the ROUTE PUBLICATION whose
                                    // erase orphans the dancer's contribution
        fw_unlocked.store(true, std::memory_order_release);
    });
    fr.set_entry([&](Fiber&) {
        sa::WaitNode rn;
        rw.read_lock(rn);           // writer holds: blocks (queued wait)
        wid_fr.store(Scheduler::current_worker_id(), std::memory_order_release);
        fr_resumed.store(true, std::memory_order_release);
        rw.unlock_read();
    });

    FiberStack sa_, sw, sr;
    SLUICE_CHECK(sched.init_fiber(fa, sa_.base(), sa_.size()));
    SLUICE_CHECK(sched.init_fiber(fw, sw.base(), sw.size()));
    SLUICE_CHECK(sched.init_fiber(fr, sr.base(), sr.size()));
    sched.spawn(fa);

    std::thread runner([&] {
        sched.run(2);
        run_returned.store(true, std::memory_order_release);
    });

    // Role discovery: FA's runner is the DANCER; the other worker is the
    // ERASER (FW's future runner — pinned by the spawn_on below and verified
    // at the end from FW's own recorded id).
    SLUICE_CHECK(wait_flag(fa_started, kWatchdog));
    const unsigned wid_dancer = wid_fa.load(std::memory_order_acquire);
    SLUICE_CHECK(wid_dancer == 0 || wid_dancer == 1);
    const unsigned wid_eraser = 1 - wid_dancer;

    // FW onto the eraser: locks the rwlock (B1 observed), then suspends on
    // the flag. FR onto the eraser: queues behind the writer (B2 observed —
    // the queue registration is persistent, unlike the per-worker park
    // domain, which stays stale after a wake and cannot prove this). At B2
    // the eraser is back in its MW-S1 park beside the running FA observer.
    sched.spawn_on(fw, wid_eraser);
    SLUICE_CHECK(wait_flag(fw_locked, kWatchdog));
    sched.spawn_on(fr, wid_eraser);
    SLUICE_CHECK(wait_fiber_waiting(fr, kWatchdog));
    SLUICE_CHECK(fw.state() == FiberState::waiting);  // FW parked on the flag

    // Arm the holds: the eraser must stop strictly after its pop-erase and
    // strictly before run_next_on (per-worker: FW's flag-reroute pop), the
    // dancer must stop at its park-candidate boundary (before the commit
    // recheck), and the baseline seam (global) pins whoever parks next —
    // the eraser's post-dance park, because the dancer cannot pass its
    // candidate hold until the coordinator releases it in step 8.
    stest::Issue161EraseDoneSeam::arm_for_worker(sched, wid_eraser);
    stest::Issue161CandidateSeam::arm_for_worker(sched, wid_dancer);
    stest::SchedulerParkBaselineSeam::arm(sched);

    // Level-triggered flag + external wake: the parked eraser is the ONLY
    // worker able to run the drain (the dancer is physically blocked inside
    // FA). The drain routes FW's ticket (idle is 0 — nothing live yet, so
    // this route's own erase is a no-op); the route's own wake walks the
    // eraser to the pop, and the erase-done seam holds it in the blind
    // window (FW invisible, running 0).
    flag_w.store(true, std::memory_order_release);
    sched.make_wake_handle().notify();
    SLUICE_CHECK(wait_erase_done_seam_paused(sched));

    // FA completes. The dancer: pop empty -> steal finds nothing (FW is
    // popped and held) -> classify MW-S3 (FR's queue registration remains)
    // -> not-last dance contribution (idle=1) -> held at the park-candidate
    // boundary, BEFORE any park-commit recheck.
    hold_fa.release();
    SLUICE_CHECK(wait_candidate_seam_paused(sched));

    // Release the eraser: it runs FW to the unlock; the unlock grants FR and
    // routes its ticket — route_runnable_locked ERASES the live idle count
    // (idle_before=1; pre-fix: no generation bump — the contribution is
    // orphaned). FR resumes and completes; the eraser's own not-last dance
    // re-counts from zero, its signal is the run's LAST, and its baseline
    // commit pauses at the baseline seam — the persistent proof the last
    // signal was emitted BEFORE the dancer's park commit is released.
    stest::Issue161EraseDoneSeam::release(sched);
    SLUICE_CHECK(wait_flag(fw_unlocked, kWatchdog));
    SLUICE_CHECK(wait_flag(fr_resumed, kWatchdog));
    SLUICE_CHECK(fw.state() == FiberState::done);
    SLUICE_CHECK(fr.state() == FiberState::done);
    SLUICE_CHECK(wait_baseline_seam_paused(sched));

    // Release the stale contributor. Pre-fix, its park commit passes the
    // 1-bit re-check (idle 1 == own stale 1) with the generation still
    // matching (no bump fired since its record), the baseline arms AFTER the
    // eraser's last signal and absorbs it, and both workers sleep one short
    // of last-idle forever. Post-fix, the route-publication erase bumped the
    // generation: the stale identity refuses the park, the re-dance reaches
    // the last-idle threshold, termination releases the eraser's baseline
    // pause, and the run returns.
    stest::Issue161CandidateSeam::release(sched);

    const bool progressed = wait_flag(run_returned, kWatchdog);
    if (!progressed) {
        // B4NoBumpPubErase stall reproduced: both workers stopped post-
        // baseline, every fiber complete, idle_workers_(1) <
        // live_loop_workers_(2), terminate false. Dump the park-state
        // evidence, release the baseline seam, then rescue through the
        // EXTERNAL wake API (production API) so the binary reports failure
        // instead of hanging.
        for (unsigned w = 0; w < 2; ++w) {
            bool available = false;
            const auto domain =
                AsyncTestAccess::worker_park_domain_try(sched, w, available);
            const int cls = AsyncTestAccess::worker_last_classify(sched, w);
            std::printf(
                "[issue161-pub] stall evidence: worker %u park_domain=%d "
                "(available=%d, 0=None 1=Scheduler) last_classify=%d\n",
                w, static_cast<int>(domain), available ? 1 : 0, cls);
        }
        stest::SchedulerParkBaselineSeam::release(sched);
        sched.make_wake_handle().notify();
        // Bounded rescue retry (the join below must not hang a still-stuck
        // run): the first notify deterministically wakes the parked workers
        // to re-dance and terminate for this construction; the loop is
        // belt-and-braces against a racing wake absorption.
        bool rescued = false;
        for (int i = 0; i < 4 && !rescued; ++i) {
            sched.make_wake_handle().notify();
            rescued = wait_flag(run_returned, kWatchdog);
        }
        SLUICE_CHECK(rescued);
    }
    runner.join();

    SLUICE_CHECK(progressed);  // the run must converge on its own
    SLUICE_CHECK(fa.state() == FiberState::done);
    SLUICE_CHECK(fw.state() == FiberState::done);
    SLUICE_CHECK(fr.state() == FiberState::done);
    SLUICE_CHECK(wid_fw.load(std::memory_order_acquire) ==
                 wid_eraser);  // roles held: the eraser executed FW
    SLUICE_CHECK(wid_fr.load(std::memory_order_acquire) ==
                 wid_eraser);  // ... and resumed FR after the grant
}

SLUICE_MAIN()
