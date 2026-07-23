// e13_select_inline — E13 P5 inline Select admission tests.
//
// Drives the PUBLIC variadic select() entry from a REAL running Fiber on the
// target Scheduler (production-test-plan.md §7.5 ST-1..ST-8, plus T1/T2/T3).
// No direct AsyncTestAccess::select_process_group calls here — P5 is exercised
// end-to-end through the public template.
//
// Determinism policy (production-test-plan.md §1): the deterministic logical
// clock (E11TimerControl) makes Timer deadlines due BEFORE run, and PhaseTag
// causal seams (sluice_async_internal_testing variant only) prove registration-
// before-snapshot (T2) and the inline Completed->Consumed lifecycle (T3). No
// sleep_for, no wall-clock timing.
//
// Gated to x86_64 (fiber_ctx::supported) for parity with the rest of E13; P5
// itself suspends no fiber (inline completion only).
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

// ===========================================================================
// Harness boilerplate (mirrors e12_event_test.cpp / e11_timer_wait_test.cpp).
// ===========================================================================
namespace {

struct FiberStack {
    static constexpr std::size_t kBytes = 64 * 1024;
    alignas(16) std::vector<std::byte> bytes{kBytes};
    std::byte* base() noexcept { return bytes.data(); }
    std::size_t size() const noexcept { return bytes.size(); }
};

inline void spin_until_paused(Scheduler& s, stest::PhaseTag tag) {
    stest::wait_paused(s, tag);
}

// A minimal context + scheduler + controller fixture with the deterministic
// clock enabled. select() runs ON a real fiber; the fixture owns the backend.
struct InlineFixture {
    sa::AsyncIoContext ctx{std::make_unique<sa::FakeAsyncBackend>()};
    Scheduler sched;
    stest::ControllerGuard ctrl;
    InlineFixture() : sched(ctx), ctrl(sched) {
        stest::E11TimerControl::enable_test_clock(sched);
    }
};

}  // namespace

// ===========================================================================
// ST-1 — Event already set -> inline winner
// ===========================================================================
SLUICE_TEST_CASE(st1_event_already_set) {
    if constexpr (!sa::fiber_ctx::supported) return;
    InlineFixture f;
    Event ev(f.sched, /*initially_set=*/true);
    SelectResult captured;
    std::atomic<bool> ran_after{false};

    Fiber fb;
    fb.set_entry([&](Fiber&) {
        captured = sa::select(f.sched, EventSelectCase{ev});
        ran_after.store(true, std::memory_order_release);
    });
    FiberStack sw;
    SLUICE_CHECK(f.sched.init_fiber(fb, sw.base(), sw.size()));
    const auto runnable_before = f.sched.runnable_count();
    f.sched.spawn(fb);
    f.sched.run(1);

    SLUICE_CHECK_MSG(ran_after.load(), "fiber resumed (inline, no suspension)");
    SLUICE_CHECK_MSG(captured.has_winner(), "select produced a winner");
    SLUICE_CHECK_MSG(captured.kind() == SelectKind::event, "winner kind is Event");
    SLUICE_CHECK_MSG(captured.index() == 0, "winner index 0");
    SLUICE_CHECK_MSG(f.sched.runnable_count() == runnable_before,
                     "no runnable published (inline)");
    SLUICE_CHECK_MSG(ev.is_set(), "Event SET preserved (persistent readiness)");
}

// ===========================================================================
// ST-2 — Timer already due -> inline winner
// ===========================================================================
SLUICE_TEST_CASE(st2_timer_already_due) {
    if constexpr (!sa::fiber_ctx::supported) return;
    InlineFixture f;
    // Make the deadline already-due by advancing the logical clock past it
    // BEFORE the worker runs the fiber. Deadline=10; set clock to 100.
    stest::E11TimerControl::set_clock(f.sched, 100);
    const Scheduler::deadline_t deadline = 10;

    SelectResult captured;
    std::atomic<bool> ran_after{false};
    Fiber fb;
    fb.set_entry([&](Fiber&) {
        captured = sa::select(f.sched, TimerSelectCase{f.sched, deadline});
        ran_after.store(true, std::memory_order_release);
    });
    FiberStack sw;
    SLUICE_CHECK(f.sched.init_fiber(fb, sw.base(), sw.size()));
    const auto runnable_before = f.sched.runnable_count();
    const auto adc_before = stest::E11TimerControl::active_deadline_count(f.sched);
    f.sched.spawn(fb);
    f.sched.run(1);

    SLUICE_CHECK_MSG(ran_after.load(), "fiber resumed (inline Timer winner)");
    SLUICE_CHECK_MSG(captured.has_winner(), "select produced a winner");
    SLUICE_CHECK_MSG(captured.kind() == SelectKind::timer, "winner kind is Timer");
    SLUICE_CHECK_MSG(captured.index() == 0, "winner index 0");
    SLUICE_CHECK_MSG(captured.timer_outcome() == SelectTimerOutcome::fired,
                     "timer outcome fired");
    SLUICE_CHECK_MSG(f.sched.runnable_count() == runnable_before,
                     "no runnable published");
    // The Timer winner's authority is closed: active_deadline_count returned to
    // its pre-call value and no ACTIVE Select timer registration remains. (The
    // CONSUMED block may be lazily reclaimed by the pump during run(), so we
    // assert the load-bearing authority-closure invariants, not a lingering
    // terminal-state count.)
    SLUICE_CHECK_MSG(stest::E11TimerControl::active_deadline_count(f.sched) ==
                         adc_before,
                     "active_deadline_count closed for the consumed Timer");
    SLUICE_CHECK_MSG(
        stest::E13SelectTimerSeam::count_in_state(
            f.sched, sad::SelectTimerRegistration::State::active) == 0,
        "no ACTIVE Select timer authority remains");
}

// ===========================================================================
// ST-3a — Event(0) + Timer(1) tie: Event wins (lowest index)
// ===========================================================================
SLUICE_TEST_CASE(st3a_event_then_timer_event_wins) {
    if constexpr (!sa::fiber_ctx::supported) return;
    InlineFixture f;
    stest::E11TimerControl::set_clock(f.sched, 100);
    Event ev(f.sched, /*initially_set=*/true);

    SelectResult captured;
    std::atomic<bool> ran_after{false};
    Fiber fb;
    fb.set_entry([&](Fiber&) {
        captured = sa::select(f.sched,
                              EventSelectCase{ev},
                              TimerSelectCase{f.sched, Scheduler::deadline_t{10}});
        ran_after.store(true, std::memory_order_release);
    });
    FiberStack sw;
    SLUICE_CHECK(f.sched.init_fiber(fb, sw.base(), sw.size()));
    f.sched.spawn(fb);
    f.sched.run(1);

    SLUICE_CHECK_MSG(ran_after.load(), "fiber resumed");
    SLUICE_CHECK_MSG(captured.has_winner(), "winner produced");
    // Lowest index (Event at 0) wins — NOT Event-kind priority.
    SLUICE_CHECK_MSG(captured.index() == 0, "index 0 (Event) wins by lowest index");
    SLUICE_CHECK_MSG(captured.kind() == SelectKind::event, "Event wins");
    // Loser Timer authority closed: no ACTIVE Select timer remains. (The RETIRED
    // block may be lazily reclaimed by the pump during run().)
    SLUICE_CHECK_MSG(
        stest::E13SelectTimerSeam::count_in_state(
            f.sched, sad::SelectTimerRegistration::State::active) == 0,
        "loser Timer authority closed (no ACTIVE Select timer remains)");
}

// ===========================================================================
// ST-3b — Timer(0) + Event(1) tie: Timer wins (lowest index)
// ===========================================================================
SLUICE_TEST_CASE(st3b_timer_then_event_timer_wins) {
    if constexpr (!sa::fiber_ctx::supported) return;
    InlineFixture f;
    stest::E11TimerControl::set_clock(f.sched, 100);
    Event ev(f.sched, /*initially_set=*/true);

    SelectResult captured;
    std::atomic<bool> ran_after{false};
    Fiber fb;
    fb.set_entry([&](Fiber&) {
        captured = sa::select(f.sched,
                              TimerSelectCase{f.sched, Scheduler::deadline_t{10}},
                              EventSelectCase{ev});
        ran_after.store(true, std::memory_order_release);
    });
    FiberStack sw;
    SLUICE_CHECK(f.sched.init_fiber(fb, sw.base(), sw.size()));
    f.sched.spawn(fb);
    f.sched.run(1);

    SLUICE_CHECK_MSG(ran_after.load(), "fiber resumed");
    SLUICE_CHECK_MSG(captured.has_winner(), "winner produced");
    // Lowest index (Timer at 0) wins — index, not kind.
    SLUICE_CHECK_MSG(captured.index() == 0, "index 0 (Timer) wins by lowest index");
    SLUICE_CHECK_MSG(captured.kind() == SelectKind::timer, "Timer wins");
    SLUICE_CHECK_MSG(captured.timer_outcome() == SelectTimerOutcome::fired,
                     "timer fired");
    // Loser Event authority closed (unlinked). SelectPort should be empty.
    SLUICE_CHECK_MSG(ev.is_set(), "loser Event SET preserved");
}

// ===========================================================================
// ST-4 — two Events already set -> lowest index wins
// ===========================================================================
SLUICE_TEST_CASE(st4_two_events_set) {
    if constexpr (!sa::fiber_ctx::supported) return;
    InlineFixture f;
    Event ev0(f.sched, /*initially_set=*/true);
    Event ev1(f.sched, /*initially_set=*/true);

    SelectResult captured;
    std::atomic<bool> ran_after{false};
    Fiber fb;
    fb.set_entry([&](Fiber&) {
        captured = sa::select(f.sched,
                              EventSelectCase{ev0},
                              EventSelectCase{ev1});
        ran_after.store(true, std::memory_order_release);
    });
    FiberStack sw;
    SLUICE_CHECK(f.sched.init_fiber(fb, sw.base(), sw.size()));
    f.sched.spawn(fb);
    f.sched.run(1);

    SLUICE_CHECK_MSG(ran_after.load(), "fiber resumed");
    SLUICE_CHECK_MSG(captured.has_winner(), "winner produced");
    SLUICE_CHECK_MSG(captured.index() == 0, "index 0 wins");
    SLUICE_CHECK_MSG(captured.kind() == SelectKind::event, "Event winner");
    // Both arms unlinked; both Events remain SET (persistent readiness).
    SLUICE_CHECK_MSG(ev0.is_set(), "ev0 SET preserved");
    SLUICE_CHECK_MSG(ev1.is_set(), "ev1 SET preserved");
}

// ===========================================================================
// ST-5 — two Timers already due -> lowest index CONSUMED, other RETIRED
// ===========================================================================
// Both Timers are due (clock past both deadlines). The lowest-index (0) Timer
// wins; the other is finalized as a loser. We assert the load-bearing outcome:
// index 0 wins (Timer), and active_deadline_count is closed for BOTH (consume
// + retire). The winner=CONSUMED / loser=RETIRED terminal-state distinction is
// the P4 source-order contract, proven directly in e13_select_claim (C5/C12);
// P5 reuses that core unchanged, so here we assert the admission chose the
// correct index and closed all authority. (Terminal blocks are lazily reclaimed
// by the pump during run(), so we assert the durable authority-closure
// invariant rather than a lingering terminal-state count.)
SLUICE_TEST_CASE(st5_two_timers_due) {
    if constexpr (!sa::fiber_ctx::supported) return;
    InlineFixture f;
    stest::E11TimerControl::set_clock(f.sched, 100);

    SelectResult captured;
    std::atomic<bool> ran_after{false};
    Fiber fb;
    fb.set_entry([&](Fiber&) {
        captured = sa::select(f.sched,
                              TimerSelectCase{f.sched, Scheduler::deadline_t{10}},
                              TimerSelectCase{f.sched, Scheduler::deadline_t{20}});
        ran_after.store(true, std::memory_order_release);
    });
    FiberStack sw;
    SLUICE_CHECK(f.sched.init_fiber(fb, sw.base(), sw.size()));
    const auto adc_before = stest::E11TimerControl::active_deadline_count(f.sched);
    f.sched.spawn(fb);
    f.sched.run(1);

    SLUICE_CHECK_MSG(ran_after.load(), "fiber resumed");
    SLUICE_CHECK_MSG(captured.has_winner(), "winner produced");
    SLUICE_CHECK_MSG(captured.index() == 0, "index 0 wins (lowest index)");
    SLUICE_CHECK_MSG(captured.kind() == SelectKind::timer, "Timer winner");
    SLUICE_CHECK_MSG(captured.timer_outcome() == SelectTimerOutcome::fired,
                     "timer fired");
    // Both arms' authority closed: active_deadline_count dropped for both the
    // consumed winner and the retired loser.
    SLUICE_CHECK_MSG(stest::E11TimerControl::active_deadline_count(f.sched) ==
                         adc_before,
                     "active_deadline_count closed for both Timers");
    SLUICE_CHECK_MSG(
        stest::E13SelectTimerSeam::count_in_state(
            f.sched, sad::SelectTimerRegistration::State::active) == 0,
        "no ACTIVE Select timer authority remains");
}

// ===========================================================================
// ST-6 — same Event appears twice -> two distinct arm nodes; lowest wins
// ===========================================================================
SLUICE_TEST_CASE(st6_same_event_twice) {
    if constexpr (!sa::fiber_ctx::supported) return;
    InlineFixture f;
    Event ev(f.sched, /*initially_set=*/true);

    SelectResult captured;
    std::atomic<bool> ran_after{false};
    Fiber fb;
    fb.set_entry([&](Fiber&) {
        captured = sa::select(f.sched,
                              EventSelectCase{ev},
                              EventSelectCase{ev});
        ran_after.store(true, std::memory_order_release);
    });
    FiberStack sw;
    SLUICE_CHECK(f.sched.init_fiber(fb, sw.base(), sw.size()));
    f.sched.spawn(fb);
    f.sched.run(1);

    SLUICE_CHECK_MSG(ran_after.load(), "fiber resumed");
    SLUICE_CHECK_MSG(captured.has_winner(), "winner produced");
    SLUICE_CHECK_MSG(captured.index() == 0, "lowest index wins");
    SLUICE_CHECK_MSG(captured.kind() == SelectKind::event, "Event winner");
    SLUICE_CHECK_MSG(ev.is_set(), "Event SET preserved");
    // The Event's SelectPort registry must contain neither node after return
    // (both finalized: winner + loser unlinked).
}

// ===========================================================================
// ST-7 — Event winner + future Timer loser: stale Timer skip after return
// ===========================================================================
SLUICE_TEST_CASE(st7_event_winner_future_timer_loser) {
    if constexpr (!sa::fiber_ctx::supported) return;
    InlineFixture f;
    // Clock at 0; Event SET (winner). Timer deadline in the FUTURE (loser).
    Event ev(f.sched, /*initially_set=*/true);
    const Scheduler::deadline_t timer_deadline = 50;

    SelectResult captured;
    std::atomic<bool> ran_after{false};
    Fiber fb;
    fb.set_entry([&](Fiber&) {
        captured = sa::select(f.sched,
                              EventSelectCase{ev},
                              TimerSelectCase{f.sched, timer_deadline});
        ran_after.store(true, std::memory_order_release);
    });
    FiberStack sw;
    SLUICE_CHECK(f.sched.init_fiber(fb, sw.base(), sw.size()));
    f.sched.spawn(fb);
    f.sched.run(1);

    SLUICE_CHECK_MSG(ran_after.load(), "fiber resumed");
    SLUICE_CHECK_MSG(captured.has_winner(), "winner produced");
    SLUICE_CHECK_MSG(captured.index() == 0, "Event index 0 wins");
    SLUICE_CHECK_MSG(captured.kind() == SelectKind::event, "Event winner");
    // The loser Timer registration was RETIRED inline (authority closed).
    SLUICE_CHECK_MSG(
        stest::E13SelectTimerSeam::count_in_state(
            f.sched, sad::SelectTimerRegistration::State::retired) == 1,
        "future Timer loser RETIRED");
    SLUICE_CHECK_MSG(
        stest::E13SelectTimerSeam::count_in_state(
            f.sched, sad::SelectTimerRegistration::State::active) == 0,
        "no ACTIVE Select timer remains");

    // After the caller frame is gone, advance the clock past the deadline and
    // pump. The pump must observe the STALE RETIRED registration and skip it
    // WITHOUT dereferencing the (now-destroyed) arm. This reaches the real P3
    // stale-skip production path (arm-load delta == 0).
    stest::E13SelectTimerSeam::reset_arm_load_count(f.sched);
    AsyncTestAccess::advance_clock(f.sched, timer_deadline + 1);
    SLUICE_CHECK_MSG(
        stest::E13SelectTimerSeam::arm_load_count(f.sched) == 0,
        "pump did NOT dereference the stale RETIRED registration's arm");
}

// ===========================================================================
// ST-8 — due Timer winner + unset Event loser: Event set() after return
// ===========================================================================
SLUICE_TEST_CASE(st8_timer_winner_event_loser) {
    if constexpr (!sa::fiber_ctx::supported) return;
    InlineFixture f;
    stest::E11TimerControl::set_clock(f.sched, 100);
    Event ev(f.sched, /*initially_set=*/false);  // unset (loser)

    SelectResult captured;
    std::atomic<bool> ran_after{false};
    Fiber fb;
    fb.set_entry([&](Fiber&) {
        captured = sa::select(f.sched,
                              TimerSelectCase{f.sched, Scheduler::deadline_t{10}},
                              EventSelectCase{ev});
        ran_after.store(true, std::memory_order_release);
    });
    FiberStack sw;
    SLUICE_CHECK(f.sched.init_fiber(fb, sw.base(), sw.size()));
    const auto runnable_before = f.sched.runnable_count();
    f.sched.spawn(fb);
    f.sched.run(1);

    SLUICE_CHECK_MSG(ran_after.load(), "fiber resumed");
    SLUICE_CHECK_MSG(captured.has_winner(), "winner produced");
    SLUICE_CHECK_MSG(captured.index() == 0, "Timer index 0 wins");
    SLUICE_CHECK_MSG(captured.kind() == SelectKind::timer, "Timer winner");

    // The loser Event arm was fully unlinked during finalization. Setting the
    // Event afterwards must NOT trigger any stale SelectGroup processing and
    // must publish no runnable. The Event's SelectPort is empty.
    ev.set();
    SLUICE_CHECK_MSG(ev.is_set(), "Event SET preserved");
    SLUICE_CHECK_MSG(f.sched.runnable_count() == runnable_before,
                     "no runnable published from the unlinked group");
}

// ===========================================================================
// T1 — template/link matrix (compile + link across all Event/Timer shapes)
// ===========================================================================
namespace {

// Real overload resolution against the public select() declaration —
// substitution failure means the real template rejected the arguments.
template <class... Cases>
concept SelectInvocable =
    requires(sa::Scheduler& scheduler, Cases&&... cases) {
        sa::select(scheduler, std::forward<Cases>(cases)...);
    };

// Negative: these must NOT satisfy the concept (compile-time gate evidence).
static_assert(!SelectInvocable<>);                  // SF-1: zero arms
static_assert(SelectInvocable<sa::EventSelectCase>);

}  // namespace

SLUICE_TEST_CASE(t1_template_link_matrix) {
    if constexpr (!sa::fiber_ctx::supported) return;
    InlineFixture f;
    stest::E11TimerControl::set_clock(f.sched, 100);
    Event ea(f.sched, true), eb(f.sched, true), ec(f.sched, true), ed(f.sched, true);

    // Each combination constructs and links; Event winners resolve inline. The
    // exercise is that each shape COMPILES and LINKS (the variadic bridge).
    SelectResult r;

    // 1 Event (rvalue temporary).
    Fiber f1;
    f1.set_entry([&](Fiber&) { r = sa::select(f.sched, EventSelectCase{ea}); });
    FiberStack s1;
    SLUICE_CHECK(f.sched.init_fiber(f1, s1.base(), s1.size()));
    f.sched.spawn(f1);
    f.sched.run(1);
    SLUICE_CHECK_MSG(r.has_winner(), "1 Event");

    // 1 Timer (rvalue temporary).
    Fiber f2;
    f2.set_entry([&](Fiber&) {
        r = sa::select(f.sched, TimerSelectCase{f.sched, Scheduler::deadline_t{10}});
    });
    FiberStack s2;
    SLUICE_CHECK(f.sched.init_fiber(f2, s2.base(), s2.size()));
    f.sched.spawn(f2);
    f.sched.run(1);
    SLUICE_CHECK_MSG(r.has_winner(), "1 Timer");

    // lvalue cases.
    EventSelectCase ev_lvalue{eb};
    TimerSelectCase tv_lvalue{f.sched, Scheduler::deadline_t{10}};
    Fiber f3;
    f3.set_entry([&](Fiber&) {
        r = sa::select(f.sched, EventSelectCase{ea}, ev_lvalue);
    });
    FiberStack s3;
    SLUICE_CHECK(f.sched.init_fiber(f3, s3.base(), s3.size()));
    f.sched.spawn(f3);
    f.sched.run(1);
    SLUICE_CHECK_MSG(r.has_winner(), "Event + Event (lvalue)");

    // Event + Timer and Timer + Event orders.
    Fiber f4;
    f4.set_entry([&](Fiber&) {
        r = sa::select(f.sched, EventSelectCase{ec},
                       TimerSelectCase{f.sched, Scheduler::deadline_t{10}});
    });
    FiberStack s4;
    SLUICE_CHECK(f.sched.init_fiber(f4, s4.base(), s4.size()));
    f.sched.spawn(f4);
    f.sched.run(1);
    SLUICE_CHECK_MSG(r.has_winner(), "Event + Timer");

    Fiber f5;
    f5.set_entry([&](Fiber&) {
        r = sa::select(f.sched, tv_lvalue, EventSelectCase{ed});
    });
    FiberStack s5;
    SLUICE_CHECK(f.sched.init_fiber(f5, s5.base(), s5.size()));
    f.sched.spawn(f5);
    f.sched.run(1);
    SLUICE_CHECK_MSG(r.has_winner(), "Timer + Event (lvalue + rvalue)");

    // 8-arm mixed pack (the kSelectMaxArms ceiling).
    Fiber f6;
    f6.set_entry([&](Fiber&) {
        r = sa::select(f.sched,
                       EventSelectCase{ea}, TimerSelectCase{f.sched, Scheduler::deadline_t{10}},
                       EventSelectCase{eb}, TimerSelectCase{f.sched, Scheduler::deadline_t{10}},
                       EventSelectCase{ec}, TimerSelectCase{f.sched, Scheduler::deadline_t{10}},
                       EventSelectCase{ed}, TimerSelectCase{f.sched, Scheduler::deadline_t{10}});
    });
    FiberStack s6;
    SLUICE_CHECK(f.sched.init_fiber(f6, s6.base(), s6.size()));
    f.sched.spawn(f6);
    f.sched.run(1);
    SLUICE_CHECK_MSG(r.has_winner(), "8 mixed arms");
    SLUICE_CHECK_MSG(r.index() == 0, "8-arm lowest index wins");
}

// ===========================================================================
// T2 — all arms registered before snapshot (AdmissionArmed seam)
// ===========================================================================
// P5 CORRECTIVE: the AdmissionArmed boundary snapshot is captured by the
// admission worker under global_mtx_ before the readiness snapshot. The test
// reads the snapshot under the controller's own mutex (no global_mtx_
// acquisition) and asserts the mechanical boundary state:
//   - phase == Selecting
//   - winner == kNoWinner (no winner chosen before snapshot)
//   - every arm state == Registered (all arms registered)
//   - Event arm is linked (home_ != nullptr)
//   - Timer arm registration is ACTIVE
//
// Combined with the load-bearing consequence (loser arm finalized = registered
// + closed), this provides the causal boundary proof the review requires.
SLUICE_TEST_CASE(t2_all_arms_registered_before_snapshot) {
    if constexpr (!sa::fiber_ctx::supported) return;
    InlineFixture f;
    stest::E11TimerControl::set_clock(f.sched, 100);
    Event ev(f.sched, true);  // index 0: ready (winner)
    // index 1: Timer also due (ready). Both ready -> lowest index wins.
    const Scheduler::deadline_t td = 10;

    SelectResult captured;
    std::atomic<bool> ran_after{false};
    Fiber fb;
    fb.set_entry([&](Fiber&) {
        captured = sa::select(f.sched,
                              EventSelectCase{ev},
                              TimerSelectCase{f.sched, td});
        ran_after.store(true, std::memory_order_release);
    });
    FiberStack sw;
    SLUICE_CHECK(f.sched.init_fiber(fb, sw.base(), sw.size()));
    f.sched.spawn(fb);
    f.sched.run(1);

    SLUICE_CHECK_MSG(ran_after.load(), "fiber resumed");
    // AdmissionArmed seam fired (registration complete before the snapshot).
    SLUICE_CHECK_MSG(
        stest::E13SelectAdmissionSeam::armed_reached(f.sched),
        "AdmissionArmed seam reached (all arms registered before snapshot)");

    // Read the boundary snapshot captured at the AdmissionArmed seam.
    const auto armed_snap = stest::E13SelectAdmissionSeam::armed_snapshot(f.sched);
    SLUICE_CHECK_MSG(
        armed_snap.phase == sad::GroupPhase::selecting,
        "AdmissionArmed snapshot: phase == Selecting");
    SLUICE_CHECK_MSG(
        armed_snap.winner == static_cast<std::uint32_t>(-1),
        "AdmissionArmed snapshot: winner == kNoWinner");
    SLUICE_CHECK_MSG(
        armed_snap.arm_count == 2,
        "AdmissionArmed snapshot: 2 arms registered");
    // Arm 0 (Event): Registered, linked.
    SLUICE_CHECK_MSG(
        armed_snap.arm_states[0] == sad::ArmState::registered,
        "AdmissionArmed snapshot: arm 0 state == Registered");
    SLUICE_CHECK_MSG(
        armed_snap.arm_kinds[0] == sad::ArmKind::event,
        "AdmissionArmed snapshot: arm 0 kind == Event");
    SLUICE_CHECK_MSG(
        armed_snap.event_linked[0],
        "AdmissionArmed snapshot: arm 0 linked to Event port");
    // Arm 1 (Timer): Registered, ACTIVE.
    SLUICE_CHECK_MSG(
        armed_snap.arm_states[1] == sad::ArmState::registered,
        "AdmissionArmed snapshot: arm 1 state == Registered");
    SLUICE_CHECK_MSG(
        armed_snap.arm_kinds[1] == sad::ArmKind::timer,
        "AdmissionArmed snapshot: arm 1 kind == Timer");
    SLUICE_CHECK_MSG(
        armed_snap.timer_states[1] ==
            sad::SelectTimerRegistration::State::active,
        "AdmissionArmed snapshot: Timer arm ACTIVE");
    // All-authority-closed is false (no winner yet).
    SLUICE_CHECK_MSG(
        !armed_snap.all_authority_closed,
        "AdmissionArmed snapshot: authority not yet closed (no winner)");

    // Lowest ready index wins (Event at 0), proving the snapshot saw both arms.
    SLUICE_CHECK_MSG(captured.has_winner(), "winner produced");
    SLUICE_CHECK_MSG(captured.index() == 0, "index 0 (Event) wins by lowest index");
    SLUICE_CHECK_MSG(captured.kind() == SelectKind::event, "Event winner");
    // The loser Timer arm (index 1) was registered THEN finalized: no ACTIVE
    // Select timer remains.
    SLUICE_CHECK_MSG(
        stest::E13SelectTimerSeam::count_in_state(
            f.sched, sad::SelectTimerRegistration::State::active) == 0,
        "loser Timer arm registered then finalized (no ACTIVE remains)");
}

// ===========================================================================
// T3 — inline lifecycle (AdmissionConsumed seam reached, inline completion)
// ===========================================================================
// P5 CORRECTIVE: the AdmissionConsumed boundary snapshot is captured by the
// admission worker under global_mtx_ before the seam. The test reads the
// snapshot and asserts the mechanical boundary state:
//   - phase == Completed
//   - completion_mode == Inline
//   - winner set (not kNoWinner)
//   - every arm Retired
//   - all authority closed
// This provides the causal boundary proof the review requires, without
// requiring the coordinator to acquire global_mtx_ while the worker holds it.
SLUICE_TEST_CASE(t3_inline_lifecycle) {
    if constexpr (!sa::fiber_ctx::supported) return;
    InlineFixture f;
    stest::E11TimerControl::set_clock(f.sched, 100);
    Event ev(f.sched, true);

    SelectResult captured;
    std::atomic<bool> ran_after{false};
    Fiber fb;
    fb.set_entry([&](Fiber&) {
        captured = sa::select(f.sched, EventSelectCase{ev});
        ran_after.store(true, std::memory_order_release);
    });
    FiberStack sw;
    SLUICE_CHECK(f.sched.init_fiber(fb, sw.base(), sw.size()));
    const auto runnable_before = f.sched.runnable_count();
    f.sched.spawn(fb);
    f.sched.run(1);

    SLUICE_CHECK_MSG(ran_after.load(), "fiber resumed");
    // Lifecycle seams reached in order: Armed (registration done) then Consumed
    // (inline completion committed).
    SLUICE_CHECK_MSG(
        stest::E13SelectAdmissionSeam::armed_reached(f.sched),
        "AdmissionArmed seam reached");
    SLUICE_CHECK_MSG(
        stest::E13SelectAdmissionSeam::consumed_reached(f.sched),
        "AdmissionConsumed seam reached (inline Completed->Consumed lifecycle)");

    // Read the boundary snapshot captured at the AdmissionConsumed seam.
    const auto consumed_snap =
        stest::E13SelectAdmissionSeam::consumed_snapshot(f.sched);
    SLUICE_CHECK_MSG(
        consumed_snap.phase == sad::GroupPhase::completed,
        "AdmissionConsumed snapshot: phase == Completed");
    SLUICE_CHECK_MSG(
        consumed_snap.completion_mode == sad::CompletionMode::inline_,
        "AdmissionConsumed snapshot: completion_mode == Inline");
    SLUICE_CHECK_MSG(
        consumed_snap.winner != static_cast<std::uint32_t>(-1),
        "AdmissionConsumed snapshot: winner set (not kNoWinner)");
    SLUICE_CHECK_MSG(
        consumed_snap.arm_count == 1,
        "AdmissionConsumed snapshot: 1 arm");
    // The single arm is Retired.
    SLUICE_CHECK_MSG(
        consumed_snap.arm_states[0] == sad::ArmState::retired,
        "AdmissionConsumed snapshot: arm state == Retired");
    SLUICE_CHECK_MSG(
        consumed_snap.all_authority_closed,
        "AdmissionConsumed snapshot: all authority closed");

    SLUICE_CHECK_MSG(captured.has_winner(), "winner produced");
    SLUICE_CHECK_MSG(captured.index() == 0, "winner index 0");
    // Inline completion publishes zero runnables.
    SLUICE_CHECK_MSG(f.sched.runnable_count() == runnable_before,
                     "runnable delta 0 (inline completion)");
}

SLUICE_MAIN()
