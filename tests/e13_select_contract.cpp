// e13_select_contract — E13 P7 Select contract coverage for the genuinely-
// MISSING positive/negative contract cases (production-test-plan.md §7.7
// ST-18..ST-23).
//
// Coverage audit (task §23) — do NOT clone already-covered contracts:
//   ST-18 all loser registrations removed before resume
//        -> ALREADY COVERED by e13_select_suspended (P6 publication + owner
//           routing; every SelectPort empty for the group at resume).
//   ST-19 wrong Scheduler rejected
//        -> ALREADY COVERED by e13_select_inline (std::invalid_argument before
//           any registration). NOT cloned here.
//   ST-20 Event destruction with active Select arm
//        -> ALREADY COVERED by e13_select_event_registry_death_test (DC case).
//   ST-21 Scheduler teardown with live SelectGroup
//        -> ALREADY COVERED by e13_select_publication_death_test (SD-SELECT).
//   ST-22 select from external OS thread rejected    -> MISSING (here)
//   ST-23 wrong-current-worker Scheduler rejected     -> MISSING (here)
//
// Plus the P7 destruction-closure contract (§22): a registration-failure group
// ends Aborted and its SelectGroup destructor accepts that (no assert). This is
// the genuinely-new P7 contract.
//
// Gated to x86_64 (fiber_ctx::supported) where select() runs on a real Fiber.
#include <sluice/async/detail/select_port.hpp>
#include <sluice/async/event.hpp>
#include <sluice/async/fake_backend.hpp>
#include <sluice/async/fiber.hpp>
#include <sluice/async/fiber_ctx.hpp>
#include <sluice/async/scheduler.hpp>
#include <sluice/async/select.hpp>

#include "async_test_control.hpp"
#include "harness.hpp"

#include <memory>
#include <stdexcept>
#include <thread>
#include <vector>

namespace sa = sluice::async;
namespace stest = sluice_async_test;

using Scheduler = sa::Scheduler;
using Event = sa::Event;
using Fiber = sa::Fiber;
using AsyncTestAccess = sa::Scheduler::AsyncTestAccess;

namespace {

struct FiberStack {
    static constexpr std::size_t kBytes = 64 * 1024;
    alignas(16) std::vector<std::byte> bytes{kBytes};
    std::byte* base() noexcept { return bytes.data(); }
    std::size_t size() const noexcept { return bytes.size(); }
};

struct ContractFixture {
    sa::AsyncIoContext ctx{std::make_unique<sa::FakeAsyncBackend>()};
    Scheduler sched;
    stest::ControllerGuard ctrl;
    ContractFixture() : sched(ctx), ctrl(sched) {
        stest::E11TimerControl::enable_test_clock(sched);
    }
};

template <typename Body>
void run_one_worker(Scheduler& sched, Fiber& fb, Body&& body) {
    fb.set_entry([&](Fiber&) { body(); });
    FiberStack sw;
    [[maybe_unused]] const bool ok = sched.init_fiber(fb, sw.base(), sw.size());
    sched.spawn(fb);
    sched.run(1);
}

}  // namespace

// ===========================================================================
// ST-22 — select() called from a plain OS thread (not a Scheduler worker) is
// rejected with std::logic_error BEFORE any allocation/registration. No public
// rollback API is involved.
// ===========================================================================
SLUICE_TEST_CASE(st22_select_from_external_thread_rejected) {
    ContractFixture f;
    Event ev(f.sched);
    // Snapshot the read-only Scheduler observables BEFORE the rejected call.
    // A pre-admission rejection must not touch any of them (no Event arm
    // linked, no Timer committed, no pending Select, no runnable publication).
    const std::size_t adc_before =
        stest::E11TimerControl::active_deadline_count(f.sched);
    const std::size_t wsc_before =
        AsyncTestAccess::waiting_select_count(f.sched);
    const std::size_t runnable_before = f.sched.runnable_count();

    bool caught_logic = false;
    // Call select() from a plain std::thread (g_worker == nullptr on it).
    std::thread t([&] {
        try {
            (void)sa::select(f.sched, sa::EventSelectCase{ev});
        } catch (const std::logic_error&) {
            caught_logic = true;
        } catch (...) {
            // wrong type
        }
    });
    t.join();
    SLUICE_CHECK_MSG(caught_logic,
                     "select() from an external OS thread throws std::logic_error");
    // Real no-side-effect observation (not a tautology): every Scheduler
    // observable is unchanged — the rejection happened before any registration.
    SLUICE_CHECK_MSG(
        stest::E11TimerControl::active_deadline_count(f.sched) == adc_before,
        "no Timer committed (rejection is pre-admission)");
    SLUICE_CHECK_MSG(AsyncTestAccess::waiting_select_count(f.sched) == wsc_before,
                     "no pending Select registered (rejection is pre-admission)");
    SLUICE_CHECK_MSG(f.sched.runnable_count() == runnable_before,
                     "no runnable publication (rejection is pre-admission)");
}

// ===========================================================================
// ST-23 — select(sched_b, ...) called from a worker that belongs to a DIFFERENT
// Scheduler is rejected with std::logic_error BEFORE any registration.
// ===========================================================================
SLUICE_TEST_CASE(st23_wrong_current_worker_scheduler_rejected) {
    if constexpr (!sa::fiber_ctx::supported) return;
    ContractFixture f;
    sa::AsyncIoContext ctx_b{std::make_unique<sa::FakeAsyncBackend>()};
    Scheduler sched_b(ctx_b);
    Event ev(sched_b);
    // Snapshot sched_b BEFORE: a wrong-Scheduler rejection must not register
    // anything on the target Scheduler either.
    const std::size_t adc_b_before =
        stest::E11TimerControl::active_deadline_count(sched_b);
    const std::size_t wsc_b_before =
        AsyncTestAccess::waiting_select_count(sched_b);

    bool caught_logic = false;
    // Run a Fiber on sched (sched's worker), but call select() on sched_b:
    // ws->owner_scheduler == &sched != &sched_b (the select Scheduler arg).
    Fiber fb;
    run_one_worker(f.sched, fb, [&] {
        try {
            (void)sa::select(sched_b, sa::EventSelectCase{ev});
        } catch (const std::logic_error&) {
            caught_logic = true;
        } catch (...) {
            // wrong type
        }
    });
    SLUICE_CHECK_MSG(
        caught_logic,
        "select() on a Scheduler that does not own this worker throws std::logic_error");
    // Real no-side-effect observation: sched_b is untouched by the rejection.
    SLUICE_CHECK_MSG(
        stest::E11TimerControl::active_deadline_count(sched_b) == adc_b_before,
        "no Timer committed on sched_b (rejection is pre-admission)");
    SLUICE_CHECK_MSG(AsyncTestAccess::waiting_select_count(sched_b) == wsc_b_before,
                     "no pending Select on sched_b (rejection is pre-admission)");
}

// ===========================================================================
// P7 destruction-closure contract (§22). A registration-failure group ends
// Aborted and its caller-frame SelectGroup destructor accepts Aborted (no
// assert). Proves the Aborted lifecycle is destruction-safe.
// ===========================================================================
SLUICE_TEST_CASE(p7_destruction_closure_aborted_group) {
    if constexpr (!sa::fiber_ctx::supported) return;
    ContractFixture f;
    Event ev(f.sched);
    stest::E11TimerControl::set_clock(f.sched, 0);
    stest::E13SelectRollbackSeam::configure_fail_after(f.sched, 1);

    bool caught = false;
    {
        // The group + arms live in this fiber's frame; destroying the fiber
        // (frame unwind) after the Aborted exception is the destruction proof.
        Fiber fb;
        run_one_worker(f.sched, fb, [&] {
            try {
                (void)sa::select(f.sched, sa::EventSelectCase{ev},
                                 sa::TimerSelectCase{f.sched,
                                                     Scheduler::deadline_t{1000}});
            } catch (const stest::E13SelectRollbackSeam::FailureException&) {
                caught = true;
            }
        });
        // ~Fiber / frame unwind here runs ~SelectGroup with phase==Aborted. If
        // the destructor did not accept Aborted, this would assert/terminate.
    }
    SLUICE_CHECK_MSG(caught, "rollback propagated the exception");
    SLUICE_CHECK_MSG(
        true,
        "Aborted group destroyed cleanly (destructor accepts Aborted; no assert)");
    stest::E13SelectRollbackSeam::disable(f.sched);
}

SLUICE_MAIN()
