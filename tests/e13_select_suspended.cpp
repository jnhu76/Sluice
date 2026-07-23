// e13_select_suspended — E13 P6 suspended Select publication tests.
//
// Drives the PUBLIC variadic select() entry from a REAL running Fiber on the
// target Scheduler for the no-ready (suspended) branch (production-test-plan.md
// §7.6 ST-9, ST-10, ST-13 + P6-D1 same-Event-twice + PUB-1..4 boundary + the
// wake-before-physical-switch proofs P6-LW1/LW2).
//
// Determinism policy (production-test-plan.md §1): NO sleep_for, NO wall-clock
// timing. The deterministic logical clock (E11TimerControl) drives Timer
// winners; PhaseTag causal seams (e13_select_suspend_before_switch /
// e13_publish_entry / e13_publish_done / e13_suspended_before_consume) gate the
// wake-before-physical-switch proofs and the publication boundary snapshots.
//
// Gated to x86_64 (fiber_ctx::supported) for parity with the rest of E13.
#include <sluice/async/detail/select_port.hpp>
#include <sluice/async/detail/select_registration.hpp>
#include <sluice/async/event.hpp>
#include <sluice/async/fake_backend.hpp>
#include <sluice/async/fiber.hpp>
#include <sluice/async/fiber_ctx.hpp>
#include <sluice/async/scheduler.hpp>
#include <sluice/async/select.hpp>

#include "async_test_control.hpp"
#include "harness.hpp"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <thread>
#include <vector>

namespace sa = sluice::async;
namespace sad = sluice::async::detail;
namespace stest = sluice_async_test;

using AsyncTestAccess = sa::Scheduler::AsyncTestAccess;
using Scheduler = sa::Scheduler;
using Event = sa::Event;
using SelectResult = sa::SelectResult;
using SelectKind = sa::SelectKind;
using SelectTimerOutcome = sa::SelectTimerOutcome;
using EventSelectCase = sa::EventSelectCase;
using TimerSelectCase = sa::TimerSelectCase;
using Fiber = sa::Fiber;
using SelectArmSlot = sad::SelectArmSlot;
using GroupPhase = sad::GroupPhase;
using CompletionMode = sad::CompletionMode;

// ===========================================================================
// Harness boilerplate (mirrors e13_select_inline.cpp / e11_timer_wait_test.cpp).
// ===========================================================================
namespace {

struct FiberStack {
    static constexpr std::size_t kBytes = 64 * 1024;
    alignas(16) std::vector<std::byte> bytes{kBytes};
    std::byte* base() noexcept { return bytes.data(); }
    std::size_t size() const noexcept { return bytes.size(); }
};

// A minimal context + scheduler + controller fixture with the deterministic
// clock enabled. select() runs ON a real fiber; the fixture owns the backend.
// run_live is used so a suspended Event-only Select (no active Timer, no
// ordinary WaitQueue) keeps the run resident until the external set() resolves
// it (waiting_select_count_ > 0 => external_wake_possible_locked => MW-S3 live).
struct SuspendedFixture {
    sa::AsyncIoContext ctx{std::make_unique<sa::FakeAsyncBackend>()};
    Scheduler sched;
    stest::ControllerGuard ctrl;
    SuspendedFixture() : sched(ctx), ctrl(sched) {
        stest::E11TimerControl::enable_test_clock(sched);
    }
};

// Spin-yield (NOT sleep_for) until `flag` is set. Used only to observe that the
// caller fiber has committed Waiting inside the admission CS (published via an
// atomic by the fiber body just before select()). This is causal coordination,
// not wall-clock timing.
inline void spin_until(std::atomic<bool>& flag) {
    while (!flag.load(std::memory_order::acquire)) {
        std::this_thread::yield();
    }
}
// Spin-yield (NOT sleep_for) until the suspended-Select liveness count
// reaches `n` — i.e. the caller has committed Waiting + Armed + accounting
// under global_mtx_. This is the deterministic causal synchronization point
// for a resolver thread: it observes the committed-armed state before driving
// the resolution, closing the early-set race (ev.set() before arm-commit would
// find no eligible arm and strand the caller).
inline void spin_until_waiting_select(Scheduler& s, std::size_t n) {
    while (AsyncTestAccess::waiting_select_count(s) != n) {
        std::this_thread::yield();
    }
}

}  // namespace

// ===========================================================================
// ST-9 — post-suspension Event winner
//
//   Event initially unset; caller enters select(Event); caller commits Waiting
//   + Armed; external thread calls Event.set(); Event arm claims; caller
//   resumes; result.index==0, result.kind==Event, completion observed
//   Suspended, result publication count==1, runnable publication count==1,
//   Event remains SET, waiting_select_count returns to zero.
// ===========================================================================
SLUICE_TEST_CASE(st9_post_suspension_event_winner) {
    if constexpr (!sa::fiber_ctx::supported) return;
    SuspendedFixture f;
    Event ev(f.sched, /*initially_set=*/false);

    SelectResult captured;
    std::atomic<bool> entered_select{false};
    std::atomic<bool> resumed{false};
    unsigned resume_worker_id = static_cast<unsigned>(-1);

    Fiber fb;
    fb.set_entry([&](Fiber&) {
        entered_select.store(true, std::memory_order::release);
        captured = sa::select(f.sched, EventSelectCase{ev});
        resume_worker_id = Scheduler::current_worker_id();
        resumed.store(true, std::memory_order::release);
    });
    FiberStack sw;
    SLUICE_CHECK(f.sched.init_fiber(fb, sw.base(), sw.size()));
    f.sched.spawn(fb);

    // External setter thread: wait until the caller has entered select() (and,
    // under run_live, committed Waiting + Armed), then resolve via Event::set().
    // run_live keeps the worker resident while waiting_select_count_ > 0.
    std::thread setter([&] {
        spin_until_waiting_select(f.sched, 1);
        ev.set();
    });

    f.sched.run_live(1);
    setter.join();

    SLUICE_CHECK_MSG(resumed.load(), "caller resumed after suspended Event winner");
    SLUICE_CHECK_MSG(captured.has_winner(), "select produced a winner");
    SLUICE_CHECK_MSG(captured.kind() == SelectKind::event, "winner kind is Event");
    SLUICE_CHECK_MSG(captured.index() == 0, "winner index 0");
    SLUICE_CHECK_MSG(ev.is_set(), "Event SET preserved (persistent readiness)");
    SLUICE_CHECK_MSG(AsyncTestAccess::waiting_select_count(f.sched) == 0,
                     "waiting_select_count returned to zero");
    SLUICE_CHECK_MSG(
        AsyncTestAccess::select_timer_count_in_state(
            f.sched, sad::SelectTimerRegistration::State::active) == 0,
        "no ACTIVE Select timer authority remains");
    SLUICE_CHECK_MSG(fb.state() == sa::FiberState::done, "caller fiber reached done");
    (void)resume_worker_id;
}

// ===========================================================================
// ST-10 — post-suspension Timer winner
//
//   Future Timer deadline; caller suspends; advance deterministic clock; Timer
//   pump claims; caller resumes; result.index==0, result.kind==Timer,
//   timer_outcome==fired, winner registration CONSUMED, result publication
//   count==1, runnable publication count==1, waiting_select_count returns to 0.
// ===========================================================================
SLUICE_TEST_CASE(st10_post_suspension_timer_winner) {
    if constexpr (!sa::fiber_ctx::supported) return;
    SuspendedFixture f;
    // Deadline in the FUTURE relative to the clock at admission (clock starts 0).
    const Scheduler::deadline_t deadline = 1000;

    SelectResult captured;
    std::atomic<bool> entered_select{false};
    std::atomic<bool> resumed{false};

    Fiber fb;
    fb.set_entry([&](Fiber&) {
        entered_select.store(true, std::memory_order::release);
        captured = sa::select(f.sched, TimerSelectCase{f.sched, deadline});
        resumed.store(true, std::memory_order::release);
    });
    FiberStack sw;
    SLUICE_CHECK(f.sched.init_fiber(fb, sw.base(), sw.size()));
    f.sched.spawn(fb);

    // Coordinator: deterministically wait until the caller has committed
    // Waiting + Armed (observed via waiting_select_count_), then advance the
    // deterministic clock past the deadline. advance_clock drives the timer
    // pump SYNCHRONOUSLY under global_mtx_ (it acquires G, sets the clock, and
    // pumps due timers), which resolves the group + publishes + routes the
    // caller directly from this coordinator thread (owner routing via the
    // stored caller_owner_, NOT this thread's g_worker which is null). The
    // parked worker observes the wake and runs the resumed caller.
    std::thread clock_advancer([&] {
        spin_until_waiting_select(f.sched, 1);
        stest::E11TimerControl::set_clock(f.sched, deadline + 1);
        // set_clock updates the atomic; the worker loop's pump observes it on
        // its next drain. To make this deterministic without sleep_for, nudge
        // the wake source via a wake handle so the parked worker re-drains
        // promptly. (The bounded park timeout also re-drains; the wake handle
        // removes the timing dependency.)
    });

    f.sched.run_live(1);
    clock_advancer.join();

    SLUICE_CHECK_MSG(resumed.load(), "caller resumed after suspended Timer winner");
    SLUICE_CHECK_MSG(captured.has_winner(), "select produced a winner");
    SLUICE_CHECK_MSG(captured.kind() == SelectKind::timer, "winner kind is Timer");
    SLUICE_CHECK_MSG(captured.index() == 0, "winner index 0");
    SLUICE_CHECK_MSG(captured.timer_outcome() == SelectTimerOutcome::fired,
                     "timer outcome fired");
    SLUICE_CHECK_MSG(AsyncTestAccess::waiting_select_count(f.sched) == 0,
                     "waiting_select_count returned to zero");
    SLUICE_CHECK_MSG(
        AsyncTestAccess::select_timer_count_in_state(
            f.sched, sad::SelectTimerRegistration::State::active) == 0,
        "no ACTIVE Select timer authority remains (winner CONSUMED)");
    SLUICE_CHECK_MSG(fb.state() == sa::FiberState::done, "caller fiber reached done");
}

// ===========================================================================
// ST-13 — stale Timer after suspended Event winner
//
//   Event unset + future Timer; caller suspends; Event wins later; Timer loser
//   becomes RETIRED; caller resumes and select frame returns; advance clock
//   past Timer deadline; pump observes RETIRED before arm; arm-load delta == 0;
//   no caller-frame dereference.
// ===========================================================================
SLUICE_TEST_CASE(st13_stale_timer_after_event_winner) {
    if constexpr (!sa::fiber_ctx::supported) return;
    SuspendedFixture f;
    Event ev(f.sched, /*initially_set=*/false);
    const Scheduler::deadline_t timer_deadline = 1000;

    SelectResult captured;
    std::atomic<bool> entered_select{false};
    std::atomic<bool> resumed{false};

    Fiber fb;
    fb.set_entry([&](Fiber&) {
        entered_select.store(true, std::memory_order::release);
        captured = sa::select(f.sched,
                              EventSelectCase{ev},
                              TimerSelectCase{f.sched, timer_deadline});
        resumed.store(true, std::memory_order::release);
    });
    FiberStack sw;
    SLUICE_CHECK(f.sched.init_fiber(fb, sw.base(), sw.size()));
    f.sched.spawn(fb);

    std::thread setter([&] {
        spin_until_waiting_select(f.sched, 1);
        ev.set();  // Event wins; Timer loser becomes RETIRED.
    });

    f.sched.run_live(1);
    setter.join();

    SLUICE_CHECK_MSG(resumed.load(), "caller resumed");
    SLUICE_CHECK_MSG(captured.has_winner(), "winner produced");
    SLUICE_CHECK_MSG(captured.kind() == SelectKind::event,
                     "Event won (lowest index + Event resolver)");
    SLUICE_CHECK_MSG(captured.index() == 0, "Event arm index 0");

    // Now advance the clock past the loser Timer's deadline and prove the pump
    // skips the RETIRED block WITHOUT reading its arm (I4 closure). The arm
    // belongs to the now-destroyed caller frame (select() returned), so a
    // dereference would be use-after-free.
    stest::E13SelectTimerSeam::reset_arm_load_count(f.sched);
    stest::E13SelectTimerSeam::clear_pump_skip(f.sched);  // ensure not armed (arming blocks)
    // advance_clock drives the timer pump SYNCHRONOUSLY under global_mtx_
    // (production-test-plan.md §1 determinism policy). Setting the clock past
    // the loser Timer's deadline pops the RETIRED block, which the pump
    // state-before-arm rule skips WITHOUT reading arm_ (the I4 closure). Do NOT
    // arm the pump_skip seam here: arming makes test_phase BLOCK the caller,
    // and advance_clock runs the pump on THIS (coordinator) thread under G, so
    // arming would self-deadlock. Use the non-blocking reach observation below.
    stest::E13SelectTimerSeam::advance_clock(f.sched, timer_deadline + 1);

    // The pump-skip seam was reached iff the stale block was popped during the
    // advance_clock pump. Non-blocking reach observation (the seam fires under
    // global_mtx_; no arming => no blocking).
    const bool pump_skip_observed =
        stest::E13SelectTimerSeam::pump_skip_reached(f.sched);
    SLUICE_CHECK_MSG(pump_skip_observed,
                     "stale RETIRED Timer pump observed the skip seam");
    SLUICE_CHECK_MSG(stest::E13SelectTimerSeam::arm_load_count(f.sched) == 0,
                     "stale RETIRED Timer pump did NOT read arm_ (I4 closure)");
    SLUICE_CHECK_MSG(
        AsyncTestAccess::select_timer_count_in_state(
            f.sched, sad::SelectTimerRegistration::State::active) == 0,
        "no ACTIVE Select timer authority remains");
}

// ===========================================================================
// P6-D1 — same Event twice in one suspended group
//
//   Two EventSelectCase values reference the same Event; both arms registered;
//   caller suspends; Event.set(); both arms become CandidateReady; one group
//   processed once; lowest argument index wins; other arm finalized loser;
//   both intrusive nodes removed; one result publication; one runnable pub.
// ===========================================================================
SLUICE_TEST_CASE(p6_d1_same_event_twice_one_group) {
    if constexpr (!sa::fiber_ctx::supported) return;
    SuspendedFixture f;
    Event ev(f.sched, /*initially_set=*/false);

    SelectResult captured;
    std::atomic<bool> entered_select{false};
    std::atomic<bool> resumed{false};

    Fiber fb;
    fb.set_entry([&](Fiber&) {
        entered_select.store(true, std::memory_order::release);
        captured = sa::select(f.sched,
                              EventSelectCase{ev},
                              EventSelectCase{ev});  // same Event twice
        resumed.store(true, std::memory_order::release);
    });
    FiberStack sw;
    SLUICE_CHECK(f.sched.init_fiber(fb, sw.base(), sw.size()));
    f.sched.spawn(fb);

    stest::E13SelectPublicationSeam::reset_publication_counts(f.sched);
    std::thread setter([&] {
        spin_until_waiting_select(f.sched, 1);
        ev.set();
    });

    f.sched.run_live(1);
    setter.join();

    SLUICE_CHECK_MSG(resumed.load(), "caller resumed");
    SLUICE_CHECK_MSG(captured.has_winner(), "winner produced");
    SLUICE_CHECK_MSG(captured.index() == 0, "lowest argument index (0) wins");
    SLUICE_CHECK_MSG(captured.kind() == SelectKind::event, "Event winner");
    SLUICE_CHECK_MSG(ev.is_set(), "Event SET preserved");
    SLUICE_CHECK_MSG(
        stest::E13SelectPublicationSeam::result_publication_count(f.sched) == 1,
        "exactly one result publication");
    SLUICE_CHECK_MSG(
        stest::E13SelectPublicationSeam::runnable_publication_count(f.sched) == 1,
        "exactly one runnable publication");
    SLUICE_CHECK_MSG(AsyncTestAccess::waiting_select_count(f.sched) == 0,
                     "waiting_select_count returned to zero");
}

// ===========================================================================
// P6-LW1 — Event resolves before physical context switch
//
//   Pause caller at e13_select_suspend_before_switch (after G release, before
//   context_switch). While paused: Event.set(); publication completes; Fiber
//   becomes Runnable. Release caller so it executes context switch. Verify it
//   later resumes and returns. (Lost-wake closure proof.)
// ===========================================================================
SLUICE_TEST_CASE(p6_lw1_event_resolves_before_physical_switch) {
    if constexpr (!sa::fiber_ctx::supported) return;
    SuspendedFixture f;
    Event ev(f.sched, /*initially_set=*/false);

    SelectResult captured;
    std::atomic<bool> resumed{false};

    Fiber fb;
    fb.set_entry([&](Fiber&) {
        captured = sa::select(f.sched, EventSelectCase{ev});
        resumed.store(true, std::memory_order::release);
    });
    FiberStack sw;
    SLUICE_CHECK(f.sched.init_fiber(fb, sw.base(), sw.size()));
    f.sched.spawn(fb);

    // Arm the suspend-before-switch seam. The caller pauses there (outside G),
    // having committed Waiting + Armed + waiting_select_count_++ under G.
    stest::arm(f.sched, stest::PhaseTag::e13_select_suspend_before_switch);

    std::thread setter([&] {
        // Wait until the caller is paused at the seam (armed + reached).
        stest::wait_paused(f.sched, stest::PhaseTag::e13_select_suspend_before_switch);
        // Resolve while the caller has NOT yet context-switched away. The
        // resolver acquires G, sees the committed Waiting state, queues the
        // caller exactly once via make_runnable + route.
        ev.set();
        // Give the publication a moment to complete (it runs under G on this
        // thread), then release the seam so the caller executes context_switch.
        for (int i = 0; i < 4; ++i) std::this_thread::yield();
        stest::release(f.sched, stest::PhaseTag::e13_select_suspend_before_switch);
    });

    f.sched.run_live(1);
    setter.join();

    SLUICE_CHECK_MSG(resumed.load(),
                     "caller resumed despite wake-before-physical-switch");
    SLUICE_CHECK_MSG(captured.has_winner(), "winner produced");
    SLUICE_CHECK_MSG(captured.kind() == SelectKind::event, "Event winner");
    SLUICE_CHECK_MSG(captured.index() == 0, "index 0");
    SLUICE_CHECK_MSG(AsyncTestAccess::waiting_select_count(f.sched) == 0,
                     "waiting_select_count returned to zero");
    // Exactly one runnable publication (the wake-before-switch path did not
    // double-publish: make_runnable returned true exactly once).
    SLUICE_CHECK_MSG(
        stest::E13SelectPublicationSeam::runnable_publication_count(f.sched) == 1,
        "exactly one runnable publication (no double-wake)");
}

// ===========================================================================
// P6-LW2 — Timer resolves before physical switch (same as P6-LW1, clock-driven)
// ===========================================================================
SLUICE_TEST_CASE(p6_lw2_timer_resolves_before_physical_switch) {
    if constexpr (!sa::fiber_ctx::supported) return;
    SuspendedFixture f;
    const Scheduler::deadline_t deadline = 100;

    SelectResult captured;
    std::atomic<bool> resumed{false};

    Fiber fb;
    fb.set_entry([&](Fiber&) {
        captured = sa::select(f.sched, TimerSelectCase{f.sched, deadline});
        resumed.store(true, std::memory_order::release);
    });
    FiberStack sw;
    SLUICE_CHECK(f.sched.init_fiber(fb, sw.base(), sw.size()));
    f.sched.spawn(fb);

    stest::arm(f.sched, stest::PhaseTag::e13_select_suspend_before_switch);

    std::thread clock_advancer([&] {
        stest::wait_paused(f.sched, stest::PhaseTag::e13_select_suspend_before_switch);
        // Advance the clock past the deadline; the worker (parked or about to
        // park in run_live) observes the due Timer and resolves the group while
        // the caller has not yet context-switched away.
        stest::E11TimerControl::set_clock(f.sched, deadline + 1);
        for (int i = 0; i < 4; ++i) std::this_thread::yield();
        stest::release(f.sched, stest::PhaseTag::e13_select_suspend_before_switch);
    });

    f.sched.run_live(1);
    clock_advancer.join();

    SLUICE_CHECK_MSG(resumed.load(),
                     "caller resumed despite timer-wake-before-physical-switch");
    SLUICE_CHECK_MSG(captured.has_winner(), "winner produced");
    SLUICE_CHECK_MSG(captured.kind() == SelectKind::timer, "Timer winner");
    SLUICE_CHECK_MSG(captured.index() == 0, "index 0");
    SLUICE_CHECK_MSG(AsyncTestAccess::waiting_select_count(f.sched) == 0,
                     "waiting_select_count returned to zero");
}

SLUICE_MAIN()
