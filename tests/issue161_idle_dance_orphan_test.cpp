// issue161_idle_dance_orphan_test — Issue #161 deterministic causal
// regression: an idle-dance contribution orphaned by a ticketed worker's
// unlocked idle_workers_ store(0) leaves the run one short of the last-idle
// termination forever (the async_rwlock T22 hang shape; TLC counterexample
// M4 in spec/tla/e12_rwlock_scheduler_liveness/).
//
// Defect class (as-built master, docs/architecture/
// issue-161-idle-dance-contribution-generation-gate.md):
//   A worker that pops a runnable ticket erases the shared idle count at
//   scheduler.cpp (the `if (f)` branch) with NO wake-domain signal. If a
//   peer had just contributed a not-last idle-dance count, that erase
//   ORPHANS it: the count is gone, but the peer's 1-bit
//   idle_dance_contributed_ flag still claims "my contribution is live".
//   The eraser later runs its own not-last dance (its fetch_add now starts
//   from the erased zero) and parks; the stale contributor's park commit
//   re-check sees idle == own (1 == 1) — the 1-bit flag cannot distinguish
//   "my own stale count" from "the eraser's fresh count" — so it parks too,
//   arming a baseline that absorbs the eraser's already-emitted not-last
//   signal. Both workers sleep with every unit of work COMPLETE,
//   idle_workers_(1) < live_loop_workers_(2), terminate false, and no
//   remaining producer: the run(2) join hangs.
//
// Deterministic construction (zero sleep-ordering; per-worker seams +
// bounded fail-closed waits, per the #115/#116 discipline):
//   Role DISCOVERY, not assumption: the two workers race the startup pop —
//   the F1 ticket may be stolen, so "worker 0 runs F1" is NOT guaranteed
//   (an ASan-timing repro of exactly that assumption flip). F1's body
//   records Scheduler::current_worker_id(); THAT worker is the dancer (the
//   stale contributor), and the OTHER is the eraser. The coordinator then:
//     1. arms worker_ticket_popped FOR THE ERASER and
//        scheduler_park_candidate FOR THE DANCER (per-worker arming: both
//        sites are traversed by every worker, so the roles cannot be pinned
//        with global arms);
//     2. spawn_on(F2, eraser) -> the eraser pops F2 and is held at the
//        ticket seam — F2 is REMOVED from its inbox (invisible to the
//        dancer's steal and to classify) and running is not yet
//        incremented;
//     3. releases the rendezvous -> F1 completes -> the dancer finds no
//        runnable work anywhere, classifies QUIESCENT, contributes a
//        not-last dance count (idle=1) and is held at the park-candidate
//        boundary;
//     4. releases the ticket seam -> the eraser erases the idle count
//        (ORPHANING the dancer's contribution), runs F2 to completion,
//        classifies quiescent, contributes its own not-last count (idle=1
//        again) whose signal finds no waiter, and parks (observed via the
//        park-domain probe — persistent state, never a timing assumption);
//     5. releases the candidate seam -> the dancer's park commit re-check
//        passes (idle 1 == own stale 1), its baseline absorbs the eraser's
//        signal, and it parks. Both parked, work complete, no producer.
//
//     PRE-FIX : permanent run() stall -> the bounded watchdog fail-closes,
//               dumps the park-state evidence, then rescues the run via the
//               EXTERNAL wake handle (production API) so the binary reports
//               the failure instead of hanging.
//     POST-FIX: the orphaning erase advances the contribution generation;
//               the dancer's park commit refuses on the stale identity
//               (signal + re-loop), the re-dance reaches the last-idle
//               threshold, and run(2) returns normally.
//
// Every step mirrors the TLC M4 counterexample state sequence:
// PopTicket -> EraseIdle -> peer DanceContribute -> stale contributor
// ParkCommit -> ArmBaseline -> both Parked.
//
// All choreography waits are BOUNDED and fail closed (dump + rescue): a
// construction that cannot reach its next seam reports the state and fails
// the case instead of blocking the suite forever.
//
// Gated to fiber-capable targets (fiber_ctx::supported), like the E8/#115
// suites.
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

// Bounded wait on the per-worker park domain becoming the Scheduler wake
// domain. A persistent-state probe (the domain is set at park ENTRY, after
// the dance contribution and its not-last signal), so observing it proves
// the worker passed the dance — never a timing assumption. Returns false on
// timeout (the caller fail-closes).
bool wait_park_domain_scheduler(Scheduler& s, unsigned worker_id,
                                std::chrono::milliseconds bound) {
    const auto deadline = std::chrono::steady_clock::now() + bound;
    for (;;) {
        bool available = false;
        const auto domain =
            AsyncTestAccess::worker_park_domain_try(s, worker_id, available);
        if (available && domain == WorkerState::ParkDomain::Scheduler) {
            return true;
        }
        if (std::chrono::steady_clock::now() >= deadline) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}

constexpr auto kWatchdog = std::chrono::milliseconds(10000);

// Bounded seam wait (fail-closed): a choreography that cannot reach its
// next seam must fail the case, never block the suite forever. Polls the
// phase's paused flag; on timeout (or the eraser's F2 running while the
// seam never held — the role pin failed) returns false.
bool wait_ticket_seam_paused(Scheduler& s, const std::atomic<bool>& f2_ran) {
    const auto deadline = std::chrono::steady_clock::now() + kWatchdog;
    while (!stest::Issue161TicketSeam::is_paused(s)) {
        if (f2_ran.load(std::memory_order_acquire)) return false;
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

}  // namespace

// ---- Test: the orphaned contribution stalls the run forever (pre-fix) ------
SLUICE_TEST_CASE(issue161_orphaned_dance_contribution_stalls_run) {
    if constexpr (!sa::fiber_ctx::supported) return;

    sa::AsyncIoContext ctx(std::make_unique<sa::FakeAsyncBackend>());
    Scheduler sched(ctx);
    stest::ControllerGuard ctrl(sched);

    Rendezvous hold_f1;
    std::atomic<bool> f1_started{false};
    std::atomic<bool> f2_ran{false};
    std::atomic<bool> run_returned{false};
    std::atomic<unsigned> wid_f1{static_cast<unsigned>(-1)};
    std::atomic<unsigned> wid_f2{static_cast<unsigned>(-1)};

    Fiber f1, f2;
    f1.set_entry([&](Fiber&) {
        wid_f1.store(Scheduler::current_worker_id(), std::memory_order_release);
        f1_started.store(true, std::memory_order_release);
        hold_f1.wait();  // the dancer stays a running observer until the
                         // coordinator has pinned the eraser at the ticket
                         // seam
    });
    f2.set_entry([&](Fiber&) {
        wid_f2.store(Scheduler::current_worker_id(), std::memory_order_release);
        f2_ran.store(true, std::memory_order_release);
    });

    FiberStack s1, s2;
    SLUICE_CHECK(sched.init_fiber(f1, s1.base(), s1.size()));
    SLUICE_CHECK(sched.init_fiber(f2, s2.base(), s2.size()));
    // Pre-run spawn -> pending_spawn_; run(2) distributes the first ticket
    // to worker 0's inbox — but the two workers RACE the startup pop, and a
    // steal may move F1 before its owner pops it. The roles are therefore
    // DISCOVERED from F1's body, never assumed (the #115 discipline).
    sched.spawn(f1);

    std::thread runner([&] {
        sched.run(2);
        run_returned.store(true, std::memory_order_release);
    });

    // The dancer is inside F1 at the rendezvous (it cannot reach any seam
    // before the rendezvous releases); the eraser has parked as the mw_s1
    // observer (or is looping toward it) and cannot reach the ticket seam
    // before F2 exists. Discover the roles, then arm both holds.
    SLUICE_CHECK(wait_flag(f1_started, kWatchdog));
    const unsigned wid_dancer = wid_f1.load(std::memory_order_acquire);
    SLUICE_CHECK(wid_dancer == 0 || wid_dancer == 1);
    const unsigned wid_eraser = 1 - wid_dancer;
    stest::Issue161TicketSeam::arm_for_worker(sched, wid_eraser);
    stest::Issue161CandidateSeam::arm_for_worker(sched, wid_dancer);

    // The eraser pops F2 (F2 leaves its inbox: invisible to the dancer's
    // steal and to classify; running not yet incremented) and pauses at the
    // seam. BOUNDED + role-checked: if F2 ever runs while the seam never
    // held, the pin failed — fail closed.
    sched.spawn_on(f2, wid_eraser);
    SLUICE_CHECK(wait_ticket_seam_paused(sched, f2_ran));
    {
        bool available = false;
        const auto domain =
            AsyncTestAccess::worker_park_domain_try(sched, wid_eraser, available);
        SLUICE_CHECK(available);
        SLUICE_CHECK(domain ==
                     WorkerState::ParkDomain::None);  // pre-park: the hold is
                                                     // in the worker_loop,
                                                     // not a park
    }

    // F1 completes. The dancer: pop empty -> steal finds nothing (F2 is
    // popped and held) -> classify QUIESCENT -> not-last dance contribution
    // (idle=1) -> held at the park-candidate boundary, before any park
    // commit recheck. BOUNDED: fail closed if the boundary never holds.
    hold_f1.release();
    SLUICE_CHECK(wait_candidate_seam_paused(sched));

    // Release the eraser: the unlocked idle-count erase ORPHANS the
    // dancer's contribution; F2 runs to completion; the eraser's own
    // not-last dance re-counts from zero, its signal finds no waiter, and
    // it parks (park-domain probe = the persistent proof the dance happened
    // before the dancer's park commit is released).
    stest::Issue161TicketSeam::release(sched);
    SLUICE_CHECK(wait_flag(f2_ran, kWatchdog));
    SLUICE_CHECK(wait_park_domain_scheduler(sched, wid_eraser, kWatchdog));

    // Release the stale contributor. Pre-fix, its park commit passes the
    // 1-bit re-check (idle 1 == own stale 1), the baseline absorbs the
    // eraser's signal, and both workers sleep with the run one short of
    // last-idle forever. Post-fix, the stale contribution identity refuses
    // the park and the re-dance terminates the run.
    stest::Issue161CandidateSeam::release(sched);

    const bool progressed = wait_flag(run_returned, kWatchdog);
    if (!progressed) {
        // M4 stall reproduced: both workers parked, every fiber complete,
        // idle_workers_(1) < live_loop_workers_(2), terminate false. Dump
        // the park-state evidence, then rescue through the EXTERNAL wake
        // API (production API) so the binary reports failure instead of
        // hanging.
        for (unsigned w = 0; w < 2; ++w) {
            bool available = false;
            const auto domain =
                AsyncTestAccess::worker_park_domain_try(sched, w, available);
            const int cls = AsyncTestAccess::worker_last_classify(sched, w);
            std::printf(
                "[issue161] stall evidence: worker %u park_domain=%d "
                "(available=%d, 0=None 1=Scheduler) last_classify=%d\n",
                w, static_cast<int>(domain), available ? 1 : 0, cls);
        }
        sched.make_wake_handle().notify();
        SLUICE_CHECK(wait_flag(run_returned, kWatchdog));
    }
    runner.join();

    SLUICE_CHECK(progressed);  // the run must converge on its own
    SLUICE_CHECK(f2_ran.load());
    SLUICE_CHECK(f1.state() == FiberState::done);
    SLUICE_CHECK(f2.state() == FiberState::done);
    SLUICE_CHECK(wid_f2.load(std::memory_order_acquire) ==
                 wid_eraser);  // roles held: the eraser executed F2
    SLUICE_CHECK(wid_f2.load(std::memory_order_acquire) !=
                 wid_dancer);  // the dancer never drained it
}

SLUICE_MAIN()
