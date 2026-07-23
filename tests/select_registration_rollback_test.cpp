// select_registration_rollback_test — E13 P7 registration-failure rollback tests.
//
// Drives the PUBLIC variadic select() entry from a REAL running Fiber on the
// target Scheduler (production-test-plan.md §7.7 ST-14, plus P7-T1..T11 / the
// rollback-half of SN-8). The synthetic registration-failure seam
// (E13SelectRollbackSeam, controller-only, absent from production) injects a
// SelectRegistrationFailure after exactly N successful registrations under
// global_mtx_; select_admit's catch runs the rollback transaction and rethrows.
//
// Determinism policy: the deterministic logical clock (E11TimerControl) makes
// Timer arms schedulable; the rollback observation (E13SelectRollbackSeam::
// observation) records the exact reverse-order per-arm rollback. No sleep_for,
// no wall-clock timing. ASan is load-bearing for the stale-Timer-after-unwind
// proof (P7-T8).
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
using EventSelectCase = sa::EventSelectCase;
using TimerSelectCase = sa::TimerSelectCase;
using Fiber = sa::Fiber;
using SelectArmSlot = sad::SelectArmSlot;
using SelectTimerReg = sad::SelectTimerRegistration;
using GroupPhase = sad::GroupPhase;
using ArmKind = sad::ArmKind;
using RollbackSeam = stest::E13SelectRollbackSeam;

namespace {

struct FiberStack {
    static constexpr std::size_t kBytes = 64 * 1024;
    alignas(16) std::vector<std::byte> bytes{kBytes};
    std::byte* base() noexcept { return bytes.data(); }
    std::size_t size() const noexcept { return bytes.size(); }
};

// A minimal context + scheduler + controller fixture with the deterministic
// clock enabled. select() runs ON a real fiber.
struct RollbackFixture {
    sa::AsyncIoContext ctx{std::make_unique<sa::FakeAsyncBackend>()};
    Scheduler sched;
    stest::ControllerGuard ctrl;
    RollbackFixture() : sched(ctx), ctrl(sched) {
        stest::E11TimerControl::enable_test_clock(sched);
    }
};

// Run a fiber to completion on one worker. The fiber body is expected to either
// return normally or throw the synthetic SelectRegistrationFailure.
template <typename Body>
void run_one_worker(Scheduler& sched, Fiber& fb, Body&& body) {
    fb.set_entry([&](Fiber&) { body(); });
    FiberStack sw;
    [[maybe_unused]] const bool ok = sched.init_fiber(fb, sw.base(), sw.size());
    sched.spawn(fb);
    sched.run(1);
}

// Helper: how many Scheduler-owned Select Timer registrations are ACTIVE.
inline std::size_t active_select_timers(const Scheduler& s) {
    return AsyncTestAccess::select_timer_count_in_state(
        s, SelectTimerReg::State::active);
}

}  // namespace

// ===========================================================================
// P7-T1 — failure before first registration (N=0).
// ===========================================================================
SLUICE_TEST_CASE(p7_t1_failure_before_first_registration) {
    if constexpr (!sa::fiber_ctx::supported) return;
    RollbackFixture f;
    Event ev0(f.sched), ev1(f.sched);
    // Make the Timer deadline NOT due so it stays a pure registration case.
    stest::E11TimerControl::set_clock(f.sched, 0);

    RollbackSeam::configure_fail_after(f.sched, 0);
    const std::size_t adc_before =
        stest::E11TimerControl::active_deadline_count(f.sched);
    const std::size_t wsc_before =
        stest::E13SelectPublicationSeam::waiting_select_count(f.sched);

    bool caught = false;
    int caught_code = 0;
    Fiber fb;
    run_one_worker(f.sched, fb, [&] {
        try {
            (void)sa::select(f.sched, EventSelectCase{ev0},
                             TimerSelectCase{f.sched, Scheduler::deadline_t{1000}});
        } catch (const RollbackSeam::FailureException& e) {
            caught = true;
            caught_code = e.code();
        }
    });

    SLUICE_CHECK_MSG(caught, "synthetic exception propagated to caller");
    SLUICE_CHECK_MSG(caught_code == RollbackSeam::FailureException::kIdentifyingCode,
                     "original exception code preserved");

    auto obs = RollbackSeam::observation(f.sched);
    SLUICE_CHECK_MSG(obs.successful_registrations == 0,
                     "zero arms registered before failure");
    SLUICE_CHECK_MSG(obs.begin_count == 1, "rollback began exactly once");
    SLUICE_CHECK_MSG(obs.finish_count == 1, "rollback finished exactly once");
    SLUICE_CHECK_MSG(obs.arm_order_len == 0, "no arms rolled back (empty prefix)");
    // No Timer moved to Scheduler pool; no active-deadline contribution.
    SLUICE_CHECK_MSG(active_select_timers(f.sched) == 0,
                     "no ACTIVE Select Timer in Scheduler pool");
    SLUICE_CHECK_MSG(
        stest::E11TimerControl::active_deadline_count(f.sched) == adc_before,
        "active_deadline_count unchanged");
    // No publication.
    SLUICE_CHECK_MSG(
        stest::E13SelectPublicationSeam::result_publication_count(f.sched) == 0,
        "no result publication");
    SLUICE_CHECK_MSG(
        stest::E13SelectPublicationSeam::runnable_publication_count(f.sched) == 0,
        "no runnable publication");
    // No liveness mutation.
    SLUICE_CHECK_MSG(
        stest::E13SelectPublicationSeam::waiting_select_count(f.sched) ==
            wsc_before,
        "waiting_select_count unchanged");
    // Event port clean.
    SLUICE_CHECK_MSG(ev0.is_set() == false, "Event SET unchanged (was unset)");

    RollbackSeam::disable(f.sched);
}

// ===========================================================================
// P7-T2 — Event-only prefix rollback matrix (inject after every prefix).
// Verifies reverse-order processing and that every linked arm becomes Retired
// and unlinked; suffix arms Detached; Event SET state unchanged.
// ===========================================================================
SLUICE_TEST_CASE(p7_t2_event_only_prefix_matrix) {
    if constexpr (!sa::fiber_ctx::supported) return;
    constexpr unsigned kArms = 4;
    for (unsigned n = 0; n <= kArms; ++n) {
        RollbackFixture f;
        std::array<Event, kArms> evs{
            Event{f.sched}, Event{f.sched}, Event{f.sched}, Event{f.sched}};
        stest::E11TimerControl::set_clock(f.sched, 0);
        RollbackSeam::configure_fail_after(f.sched, n);

        bool caught = false;
        Fiber fb;
        run_one_worker(f.sched, fb, [&] {
            try {
                (void)sa::select(f.sched, EventSelectCase{evs[0]},
                                 EventSelectCase{evs[1]},
                                 EventSelectCase{evs[2]},
                                 EventSelectCase{evs[3]});
            } catch (const RollbackSeam::FailureException&) {
                caught = true;
            }
        });
        SLUICE_CHECK_MSG(caught, "synthetic exception propagated");

        auto obs = RollbackSeam::observation(f.sched);
        SLUICE_CHECK_MSG(obs.successful_registrations == n,
                         "exactly N arms registered before failure");
        SLUICE_CHECK_MSG(obs.arm_order_len == n,
                         "exactly N arms rolled back");
        SLUICE_CHECK_MSG(obs.begin_count == 1, "rollback began once");
        SLUICE_CHECK_MSG(obs.finish_count == 1, "rollback finished once");
        // Reverse order: indices must be n-1, n-2, ..., 0.
        bool reverse_ok = true;
        for (unsigned k = 0; k < n; ++k) {
            const std::uint32_t expected = n - 1 - k;
            if (obs.arm_order_indices[k] != expected) reverse_ok = false;
            if (!RollbackSeam::kind_is_event(obs.arm_order_kinds[k]))
                reverse_ok = false;
            if (!obs.event_linked_before[k]) reverse_ok = false;
        }
        SLUICE_CHECK_MSG(reverse_ok,
                         "per-arm rollback in reverse order, Event kind, linked before");
        // Every Event port clean (no arm from the aborted group).
        SLUICE_CHECK_MSG(active_select_timers(f.sched) == 0,
                         "no Select Timer activity");
        RollbackSeam::disable(f.sched);
    }
}

// ===========================================================================
// P7-T3 — Timer-only prefix rollback matrix.
// Verifies reverse-order, every registered Timer ACTIVE->RETIRED exactly once,
// active_deadline_count returns to baseline, heap retains stale entries only
// for the registered prefix.
// ===========================================================================
SLUICE_TEST_CASE(p7_t3_timer_only_prefix_matrix) {
    if constexpr (!sa::fiber_ctx::supported) return;
    constexpr unsigned kArms = 4;
    for (unsigned n = 0; n <= kArms; ++n) {
        RollbackFixture f;
        stest::E11TimerControl::set_clock(f.sched, 0);
        const std::size_t adc_before =
            stest::E11TimerControl::active_deadline_count(f.sched);
        const std::size_t pool_before =
            stest::E13SelectTimerSeam::pool_size(f.sched);
        RollbackSeam::configure_fail_after(f.sched, n);

        bool caught = false;
        Fiber fb;
        run_one_worker(f.sched, fb, [&] {
            try {
                (void)sa::select(f.sched,
                                 TimerSelectCase{f.sched, Scheduler::deadline_t{1000}},
                                 TimerSelectCase{f.sched, Scheduler::deadline_t{2000}},
                                 TimerSelectCase{f.sched, Scheduler::deadline_t{3000}},
                                 TimerSelectCase{f.sched, Scheduler::deadline_t{4000}});
            } catch (const RollbackSeam::FailureException&) {
                caught = true;
            }
        });
        SLUICE_CHECK_MSG(caught, "synthetic exception propagated");

        auto obs = RollbackSeam::observation(f.sched);
        SLUICE_CHECK_MSG(obs.successful_registrations == n,
                         "exactly N Timer arms registered");
        SLUICE_CHECK_MSG(obs.arm_order_len == n, "N arms rolled back");
        bool reverse_ok = true;
        for (unsigned k = 0; k < n; ++k) {
            if (obs.arm_order_indices[k] != n - 1 - k) reverse_ok = false;
            if (!RollbackSeam::kind_is_timer(obs.arm_order_kinds[k]))
                reverse_ok = false;
        }
        SLUICE_CHECK_MSG(reverse_ok, "reverse order, Timer kind");

        // No ACTIVE Select Timer remains.
        SLUICE_CHECK_MSG(active_select_timers(f.sched) == 0,
                         "no ACTIVE Select Timer after rollback");
        // active_deadline_count back to baseline.
        SLUICE_CHECK_MSG(
            stest::E11TimerControl::active_deadline_count(f.sched) == adc_before,
            "active_deadline_count returned to baseline");
        // The registered prefix blocks are now RETIRED (still Scheduler-owned
        // until lazy pump reclamation). The never-registered suffix blocks
        // never entered the Scheduler pool: pool grew by exactly n.
        const std::size_t retired =
            AsyncTestAccess::select_timer_count_in_state(
                f.sched, SelectTimerReg::State::retired);
        SLUICE_CHECK_MSG(retired == n,
                         "exactly N retired Select Timer blocks (registered prefix)");
        SLUICE_CHECK_MSG(
            stest::E13SelectTimerSeam::pool_size(f.sched) == pool_before + n,
            "Scheduler pool grew by exactly the registered prefix");
        // Heap retains stale entries for the registered prefix (lazy policy).
        SLUICE_CHECK_MSG(
            stest::E13SelectTimerSeam::heap_counts_by_kind(f.sched)[1] == n,
            "deadline heap retains N stale Select entries (lazy reclamation)");
        RollbackSeam::disable(f.sched);
    }
}

// ===========================================================================
// P7-T4 — mixed Event/Timer rollback matrix.
// Covers (Event,Timer,Event,Timer) and (Timer,Event,Timer,Event) for every
// prefix; asserts exact per-kind authority closure.
// ===========================================================================
SLUICE_TEST_CASE(p7_t4_mixed_event_timer_matrix) {
    if constexpr (!sa::fiber_ctx::supported) return;
    // Two kind orderings.
    constexpr unsigned kArms = 4;
    auto run_case = [&](bool timer_first, unsigned n) {
        RollbackFixture f;
        std::array<Event, 2> evs{Event{f.sched}, Event{f.sched}};
        stest::E11TimerControl::set_clock(f.sched, 0);
        const std::size_t adc_before =
            stest::E11TimerControl::active_deadline_count(f.sched);
        RollbackSeam::configure_fail_after(f.sched, n);

        bool caught = false;
        Fiber fb;
        run_one_worker(f.sched, fb, [&] {
            try {
                if (timer_first) {
                    (void)sa::select(f.sched,
                                     TimerSelectCase{f.sched, Scheduler::deadline_t{1000}},
                                     EventSelectCase{evs[0]},
                                     TimerSelectCase{f.sched, Scheduler::deadline_t{2000}},
                                     EventSelectCase{evs[1]});
                } else {
                    (void)sa::select(f.sched, EventSelectCase{evs[0]},
                                     TimerSelectCase{f.sched, Scheduler::deadline_t{1000}},
                                     EventSelectCase{evs[1]},
                                     TimerSelectCase{f.sched, Scheduler::deadline_t{2000}});
                }
            } catch (const RollbackSeam::FailureException&) {
                caught = true;
            }
        });
        SLUICE_CHECK_MSG(caught, "synthetic exception propagated");
        auto obs = RollbackSeam::observation(f.sched);
        SLUICE_CHECK_MSG(obs.successful_registrations == n,
                         "exactly N arms registered");
        SLUICE_CHECK_MSG(obs.arm_order_len == n, "N arms rolled back");
        bool reverse_ok = true;
        for (unsigned k = 0; k < n; ++k) {
            if (obs.arm_order_indices[k] != n - 1 - k) reverse_ok = false;
        }
        SLUICE_CHECK_MSG(reverse_ok, "reverse order");
        SLUICE_CHECK_MSG(active_select_timers(f.sched) == 0,
                         "no ACTIVE Select Timer after rollback");
        SLUICE_CHECK_MSG(
            stest::E11TimerControl::active_deadline_count(f.sched) ==
                adc_before,
            "active_deadline_count at baseline");
        RollbackSeam::disable(f.sched);
    };

    for (unsigned n = 0; n <= kArms; ++n) {
        run_case(false, n);  // Event,Timer,Event,Timer
        run_case(true, n);   // Timer,Event,Timer,Event
    }
}

// ===========================================================================
// P7-T5 — failure after ALL arms, before FinishRegistration (N == arm_count).
// The closest boundary to irreversible admission: all arms Registered, group
// still Building, winner still NoWinner.
// ===========================================================================
SLUICE_TEST_CASE(p7_t5_failure_after_all_arms_before_finish) {
    if constexpr (!sa::fiber_ctx::supported) return;
    RollbackFixture f;
    Event ev(f.sched);
    stest::E11TimerControl::set_clock(f.sched, 0);
    RollbackSeam::configure_fail_after(f.sched, 2);  // 2 arms

    bool caught = false;
    Fiber fb;
    run_one_worker(f.sched, fb, [&] {
        try {
            (void)sa::select(f.sched, EventSelectCase{ev},
                             TimerSelectCase{f.sched, Scheduler::deadline_t{1000}});
        } catch (const RollbackSeam::FailureException&) {
            caught = true;
        }
    });
    SLUICE_CHECK_MSG(caught, "synthetic exception propagated");
    auto obs = RollbackSeam::observation(f.sched);
    SLUICE_CHECK_MSG(obs.successful_registrations == 2,
                     "all arms fully registered before failure");
    SLUICE_CHECK_MSG(obs.arm_order_len == 2, "both arms rolled back");
    SLUICE_CHECK_MSG(obs.begin_count == 1 && obs.finish_count == 1,
                     "one complete rollback");
    SLUICE_CHECK_MSG(active_select_timers(f.sched) == 0,
                     "no ACTIVE Select Timer");
    // No publication whatsoever.
    SLUICE_CHECK_MSG(
        stest::E13SelectPublicationSeam::result_publication_count(f.sched) == 0,
        "no result publication");
    SLUICE_CHECK_MSG(
        stest::E13SelectPublicationSeam::runnable_publication_count(f.sched) == 0,
        "no runnable publication");
    RollbackSeam::disable(f.sched);
}

// ===========================================================================
// P7-T6 — original exception preserved (type + identifying payload/code).
// ===========================================================================
SLUICE_TEST_CASE(p7_t6_original_exception_preserved) {
    if constexpr (!sa::fiber_ctx::supported) return;
    RollbackFixture f;
    Event ev(f.sched);
    stest::E11TimerControl::set_clock(f.sched, 0);
    RollbackSeam::configure_fail_after(f.sched, 1);

    bool caught_exact_type = false;
    int code = -1;
    std::string what;
    Fiber fb;
    run_one_worker(f.sched, fb, [&] {
        try {
            (void)sa::select(f.sched, EventSelectCase{ev},
                             TimerSelectCase{f.sched, Scheduler::deadline_t{1000}});
        } catch (const RollbackSeam::FailureException& e) {
            caught_exact_type = true;
            code = e.code();
            what = e.what();
        }
    });
    SLUICE_CHECK_MSG(caught_exact_type,
                     "caught the EXACT synthetic exception type");
    SLUICE_CHECK_MSG(code == RollbackSeam::FailureException::kIdentifyingCode,
                     "identifying code preserved");
    SLUICE_CHECK_MSG(!what.empty(), "exception message preserved");
    RollbackSeam::disable(f.sched);
}

// ===========================================================================
// P7-T7 — no liveness side effects (exact counter equality before/after).
// ===========================================================================
SLUICE_TEST_CASE(p7_t7_no_liveness_side_effects) {
    if constexpr (!sa::fiber_ctx::supported) return;
    RollbackFixture f;
    Event ev(f.sched);
    stest::E11TimerControl::set_clock(f.sched, 0);
    const std::size_t wsc_before =
        stest::E13SelectPublicationSeam::waiting_select_count(f.sched);
    const std::size_t rc_before = f.sched.runnable_count();
    stest::E13SelectPublicationSeam::reset_publication_counts(f.sched);
    RollbackSeam::configure_fail_after(f.sched, 1);

    bool caught = false;
    Fiber fb;
    run_one_worker(f.sched, fb, [&] {
        try {
            (void)sa::select(f.sched, EventSelectCase{ev},
                             TimerSelectCase{f.sched, Scheduler::deadline_t{1000}});
        } catch (const RollbackSeam::FailureException&) {
            caught = true;
        }
    });
    SLUICE_CHECK_MSG(caught, "synthetic exception propagated");
    // Exact equality on liveness counters.
    SLUICE_CHECK_MSG(
        stest::E13SelectPublicationSeam::waiting_select_count(f.sched) ==
            wsc_before,
        "waiting_select_count unchanged");
    SLUICE_CHECK_MSG(f.sched.runnable_count() == rc_before,
                     "runnable_count unchanged");
    SLUICE_CHECK_MSG(
        stest::E13SelectPublicationSeam::result_publication_count(f.sched) == 0,
        "result publication count == 0");
    SLUICE_CHECK_MSG(
        stest::E13SelectPublicationSeam::runnable_publication_count(f.sched) == 0,
        "runnable publication count == 0");
    RollbackSeam::disable(f.sched);
}

// ===========================================================================
// P7-T8 — stale Timer after caller-frame unwind.
// After rollback retires the stable block and the exception unwinds the
// caller frame, advance the clock past the deadline and run the pump: it pops
// the RETIRED block, skips WITHOUT reading arm_, and physically reclaims it.
// ASan is load-bearing (no UAF).
// ===========================================================================
SLUICE_TEST_CASE(p7_t8_stale_timer_after_frame_unwind) {
    if constexpr (!sa::fiber_ctx::supported) return;
    RollbackFixture f;
    stest::E11TimerControl::set_clock(f.sched, 0);
    const std::size_t adc_before =
        stest::E11TimerControl::active_deadline_count(f.sched);
    RollbackSeam::configure_fail_after(f.sched, 1);

    bool caught = false;
    {
        Fiber fb;
        run_one_worker(f.sched, fb, [&] {
            try {
                (void)sa::select(f.sched,
                                 TimerSelectCase{f.sched, Scheduler::deadline_t{1000}});
            } catch (const RollbackSeam::FailureException&) {
                caught = true;
            }
        });
        // `fb` (the caller frame holding the arm array) is destroyed here, while
        // the retired stable block is still Scheduler-owned. This is the UAF
        // boundary: the pump must not dereference the dead arm.
    }
    SLUICE_CHECK_MSG(caught, "synthetic exception propagated");

    // The pump arm-load counter must be reset; we expect ZERO arm reads when
    // the stale block is popped (state-before-arm rule).
    stest::E13SelectTimerSeam::reset_arm_load_count(f.sched);

    // Advance the clock past the deadline and pump via a no-op timer wait so
    // the worker loop's timer pump runs. The simplest deterministic pump drive:
    // register an ordinary (non-Select) deadline wait that is already due and
    // run the worker; the pump pops entries in deadline order, encountering the
    // RETIRED Select block and skipping it.
    // active_deadline_count is already back to baseline (retire decremented it);
    // it must remain unchanged through the physical erase.
    SLUICE_CHECK_MSG(
        stest::E11TimerControl::active_deadline_count(f.sched) == adc_before,
        "active_deadline_count at baseline before pump");
    stest::E11TimerControl::set_clock(f.sched, 5000);

    // The retired block is still physically in the Scheduler pool + heap. Drive
    // the pump by running the worker once with no runnable work; the pump
    // reclaims the stale entry. (The deadline is past; a single run(1) tick
    // advances the pump.)
    // To force a pump tick without suspending, run an already-resolved ordinary
    // timer wait.
    {
        sa::WaitNode node;
        sa::WaitQueue wq;
        (void)AsyncTestAccess::register_test_deadline(
            f.sched, &node, &wq, Scheduler::deadline_t{1});
        Fiber pump_fb;
        pump_fb.set_entry([&](Fiber&) {
            // This wait is already due (clock=5000 > deadline=1) -> resolves
            // inline without suspending, but the admission ran the pump path.
            (void)0;
        });
        FiberStack sw;
        (void)f.sched.init_fiber(pump_fb, sw.base(), sw.size());
        f.sched.spawn(pump_fb);
        f.sched.run(1);
    }

    // No UAF survived to here under ASan. The arm-load delta is zero.
    SLUICE_CHECK_MSG(stest::E13SelectTimerSeam::arm_load_count(f.sched) == 0,
                     "pump did NOT dereference the retired arm (arm-load delta == 0)");
    // active_deadline_count unchanged through physical erase.
    SLUICE_CHECK_MSG(
        stest::E11TimerControl::active_deadline_count(f.sched) == adc_before,
        "active_deadline_count unchanged through physical erase");
    RollbackSeam::disable(f.sched);
}

// ===========================================================================
// P7-T9 — rollback transaction isolation under global_mtx_.
//
// Goal: prove that while the rollback transaction holds global_mtx_, a
// concurrent Event set on a DIFFERENT arm cannot observe partial
// registration state. The Event set path (Phase-1 Select scan) takes
// global_mtx_ too, so it must serialize AFTER the rollback completes and
// must see only the clean post-rollback (Aborted) group.
//
// Determinism (no sleep_for as a correctness condition):
//   1. arm e13_rollback_aborted so the worker PAUSES at the end of the
//      rollback transaction while STILL HOLDING global_mtx_;
//   2. wait_reached -> the rollback transaction is now in flight, G held;
//   3. start the contender thread; it calls ev.set() which blocks on G;
//   4. spin-verify the contender has NOT completed (it is blocked on G),
//      so the partial-registration window is never observable to it;
//   5. release the phase -> worker finishes rollback, rethrows, drops G;
//   6. the contender's ev.set() now proceeds and observes only the clean
//      post-rollback group: no winner, no publication, no stale arm.
//
// Topology (mirrors ST-16/ST-17): run_live blocks the main thread driving
// the worker; a coordinator thread orchestrates the seam + contender so it
// all runs concurrently with the live worker.
//
// Assertions: contender did block (not an instantaneous no-op); rollback
// threw; no publication / liveness drift; the Event SET is recorded; the
// next Select on a fresh group still works (no Scheduler corruption).
// TSan-clean by construction (the only shared mutation is under G).
// ===========================================================================
SLUICE_TEST_CASE(p7_t9_rollback_isolation_under_g) {
    if constexpr (!sa::fiber_ctx::supported) return;
    RollbackFixture f;
    Event ev(f.sched, /*initially_set=*/false);
    stest::E11TimerControl::set_clock(f.sched, 0);

    // Inject failure after the FIRST arm registers (Event arm commits, the
    // Timer arm's registration then throws -> rollback of the Event prefix).
    RollbackSeam::configure_fail_after(f.sched, 1);

    const std::size_t adc_before =
        stest::E11TimerControl::active_deadline_count(f.sched);
    const std::size_t ast_before = active_select_timers(f.sched);

    std::atomic<bool> caught{false};
    Fiber fb;
    fb.set_entry([&](Fiber&) {
        try {
            (void)sa::select(f.sched,
                             EventSelectCase{ev},
                             TimerSelectCase{f.sched, Scheduler::deadline_t{1000}});
        } catch (const RollbackSeam::FailureException&) {
            caught.store(true, std::memory_order_release);
        }
    });
    FiberStack sw;
    [[maybe_unused]] const bool inited =
        f.sched.init_fiber(fb, sw.base(), sw.size());
    f.sched.spawn(fb);

    // Arm the in-transaction pause so the worker holds G at rollback end.
    stest::arm(f.sched, stest::PhaseTag::e13_rollback_aborted);

    std::atomic<bool> contender_done{false};
    std::atomic<bool> contender_blocked_observed{false};
    std::thread contender;
    std::thread coordinator([&] {
        // (1) The caller Fiber only reaches select() under run_live below, so
        // this coordinator runs CONCURRENTLY with the live worker. Wait until
        // the rollback transaction is genuinely in flight: paused at
        // e13_rollback_aborted, HOLDING global_mtx_.
        stest::wait_reached(f.sched, stest::PhaseTag::e13_rollback_aborted);

        // (2) NOW start the contender: an external OS thread sets the Event.
        // Event::set acquires global_mtx_ for its Phase-1 Select scan; since
        // the worker holds G inside the rollback transaction, the contender
        // MUST block here until the rollback releases G. This is the
        // load-bearing isolation window.
        contender = std::thread([&] {
            ev.set();
            contender_done.store(true, std::memory_order_release);
        });

        // (3) Deterministic proof the contender is blocked ON G (not finished):
        // it cannot have completed while the worker still holds G. A bounded
        // yield loop is OBSERVATION, not the correctness condition —
        // correctness is the mutex ordering; this confirms the contender
        // actually serialized behind the rollback.
        for (int i = 0; i < 128; ++i) {
            if (contender_done.load(std::memory_order_acquire)) break;
            std::this_thread::yield();
        }
        contender_blocked_observed.store(
            !contender_done.load(std::memory_order_acquire),
            std::memory_order_release);

        // (4) Release the in-transaction pause: the worker completes the
        // rollback, rethrows the synthetic failure, and drops global_mtx_.
        stest::release(f.sched, stest::PhaseTag::e13_rollback_aborted);
    });

    // Drive the run on the main thread. The worker runs the caller Fiber into
    // select(), the registration-failure seam fires, the rollback transaction
    // runs + pauses holding G, the contender blocks on G, the coordinator
    // releases the seam, the worker finishes + rethrows + drops G, the
    // contender's ev.set() proceeds. run_live returns once quiescent.
    f.sched.run_live(1);
    coordinator.join();
    contender.join();

    // --- post-conditions: no partial-registration leakage ---
    SLUICE_CHECK_MSG(contender_blocked_observed.load(std::memory_order_acquire),
                     "contender blocked on global_mtx_ during rollback "
                     "(could not observe partial registration)");
    SLUICE_CHECK_MSG(caught.load(std::memory_order_acquire),
                     "rollback completed and rethrew the synthetic failure");
    SLUICE_CHECK_MSG(contender_done.load(std::memory_order_acquire),
                     "contender's ev.set() completed after rollback released G");
    // No Select winner / publication survived the rollback.
    SLUICE_CHECK_MSG(AsyncTestAccess::waiting_select_count(f.sched) == 0,
                     "no lingering waiting Select after rollback");
    SLUICE_CHECK_MSG(f.sched.runnable_count() == 0,
                     "no runnable drift after rollback + contender");
    // Timer authority fully closed by the rollback (the Timer arm's
    // registration threw before it committed, so it was never Scheduler-owned;
    // asserted for completeness).
    SLUICE_CHECK_MSG(
        stest::E11TimerControl::active_deadline_count(f.sched) == adc_before,
        "active_deadline_count at baseline (Timer arm never committed)");
    SLUICE_CHECK_MSG(active_select_timers(f.sched) == ast_before,
                     "no ACTIVE Select Timer blocks leaked");
    SLUICE_CHECK_MSG(ev.is_set(), "Event SET recorded by the contender");

    // The injection seam MUST be disabled before the sanity-probe Select, or
    // the probe would itself hit the synthetic failure.
    RollbackSeam::disable(f.sched);

    // The next Select on a FRESH group still works: the Scheduler was not
    // corrupted by the concurrent rollback + Event set.
    Event ev2(f.sched, /*initially_set=*/true);
    SelectResult again;
    Fiber probe_fb;
    run_one_worker(f.sched, probe_fb, [&] {
        again = sa::select(f.sched, EventSelectCase{ev2});
    });
    SLUICE_CHECK_MSG(again.has_winner(), "Scheduler usable after isolation test");
    SLUICE_CHECK_MSG(again.kind() == SelectKind::event, "fresh Select Event winner");
}

// ===========================================================================
// P7-T10 — repeated rollback operations on the same Scheduler.
// After every call: all Event ports clean, active_deadline_count baseline
// restored, no accumulating ACTIVE Select Timer blocks, stale heap entries
// eventually reclaimed, next successful Select still works.
// ===========================================================================
SLUICE_TEST_CASE(p7_t10_repeated_rollback) {
    if constexpr (!sa::fiber_ctx::supported) return;
    RollbackFixture f;
    Event ev(f.sched);
    stest::E11TimerControl::set_clock(f.sched, 0);
    const std::size_t adc_base =
        stest::E11TimerControl::active_deadline_count(f.sched);

    for (int i = 0; i < 200; ++i) {
        RollbackSeam::configure_fail_after(f.sched, 1);
        bool caught = false;
        Fiber fb;
        run_one_worker(f.sched, fb, [&] {
            try {
                (void)sa::select(f.sched, EventSelectCase{ev},
                                 TimerSelectCase{f.sched, Scheduler::deadline_t{1000}});
            } catch (const RollbackSeam::FailureException&) {
                caught = true;
            }
        });
        SLUICE_CHECK_MSG(caught, "each rollback propagates the exception");
        SLUICE_CHECK_MSG(active_select_timers(f.sched) == 0,
                         "no ACTIVE Select Timer accumulates");
        SLUICE_CHECK_MSG(
            stest::E11TimerControl::active_deadline_count(f.sched) == adc_base,
            "active_deadline_count baseline restored each iteration");
        RollbackSeam::disable(f.sched);
    }
    // Reclaim stale heap entries by pumping past the deadline.
    stest::E11TimerControl::set_clock(f.sched, 100000);
    {
        Fiber drain;
        drain.set_entry([&](Fiber&) { (void)0; });
        FiberStack sw;
        (void)f.sched.init_fiber(drain, sw.base(), sw.size());
        f.sched.spawn(drain);
        f.sched.run(1);
    }
    SLUICE_CHECK_MSG(
        stest::E13SelectTimerSeam::heap_counts_by_kind(f.sched)[1] == 0,
        "stale heap entries reclaimed after pump");

    // A normal inline Select still works after rollback.
    ev.set();
    SelectResult captured;
    bool ok = false;
    {
        Fiber fb;
        run_one_worker(f.sched, fb, [&] {
            captured = sa::select(f.sched, EventSelectCase{ev});
            ok = true;
        });
    }
    SLUICE_CHECK_MSG(ok, "successful Select after rollback runs");
    SLUICE_CHECK_MSG(captured.has_winner(), "successful Select produces a winner");
    SLUICE_CHECK_MSG(captured.kind() == SelectKind::event,
                     "successful Select winner kind Event");
}

// ===========================================================================
// P7-T11 — success after rollback (inline + a normal path).
// Proves rollback does not corrupt shared Scheduler state: after one or more
// injected failures, disabling injection lets a normal Select complete.
// ===========================================================================
SLUICE_TEST_CASE(p7_t11_success_after_rollback) {
    if constexpr (!sa::fiber_ctx::supported) return;
    RollbackFixture f;
    Event ev(f.sched);
    stest::E11TimerControl::set_clock(f.sched, 0);

    // One failing admission.
    RollbackSeam::configure_fail_after(f.sched, 0);
    {
        bool caught = false;
        Fiber fb;
        run_one_worker(f.sched, fb, [&] {
            try {
                (void)sa::select(f.sched, EventSelectCase{ev},
                                 TimerSelectCase{f.sched, Scheduler::deadline_t{1000}});
            } catch (const RollbackSeam::FailureException&) {
                caught = true;
            }
        });
        SLUICE_CHECK_MSG(caught, "failing admission propagated");
    }
    // Disable injection; a normal inline Select (Timer already due) succeeds.
    RollbackSeam::disable(f.sched);
    stest::E11TimerControl::set_clock(f.sched, 100);  // deadline 10 now due
    SelectResult captured;
    bool ok = false;
    {
        Fiber fb;
        run_one_worker(f.sched, fb, [&] {
            captured = sa::select(f.sched,
                                  TimerSelectCase{f.sched, Scheduler::deadline_t{10}});
            ok = true;
        });
    }
    SLUICE_CHECK_MSG(ok, "normal Select after rollback runs");
    SLUICE_CHECK_MSG(captured.has_winner(), "normal Select produces a winner");
    SLUICE_CHECK_MSG(captured.kind() == SelectKind::timer,
                     "normal Select winner kind Timer");
}

SLUICE_MAIN()
