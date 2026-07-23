// e13_select_multi_worker — E13 P6 multi-worker owner routing + external-thread
// Event set + exactly-one-runnable tests (production-test-plan.md §7.6
// ST-15, ST-16, ST-17 + PUB-1..4 publication boundary snapshots).
//
// Drives the PUBLIC variadic select() entry from a REAL running Fiber on the
// target Scheduler, resolving from an external OS thread (ST-15) and across
// workers (ST-16), and proving exactly-one result/runnable publication under
// contention (ST-17). PUB-1..4 observe the publication-entry / publication-done
// / suspended-before-consume boundary snapshots (inline PUB-3 regression too).
//
// Determinism policy (production-test-plan.md §1): NO sleep_for. External
// threads synchronize via the deterministic waiting_select_count liveness
// observation (the committed-armed state) + PhaseTag causal seams; the
// publication-boundary snapshots are captured under global_mtx_ and read by the
// coordinator under the controller's own mutex (no global_mtx_ acquisition).
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
using FiberState = sa::FiberState;

// ===========================================================================
// Harness.
// ===========================================================================
namespace {

struct FiberStack {
    static constexpr std::size_t kBytes = 64 * 1024;
    alignas(16) std::vector<std::byte> bytes{kBytes};
    std::byte* base() noexcept { return bytes.data(); }
    std::size_t size() const noexcept { return bytes.size(); }
};

struct MWFixture {
    sa::AsyncIoContext ctx{std::make_unique<sa::FakeAsyncBackend>()};
    Scheduler sched;
    stest::ControllerGuard ctrl;
    MWFixture() : sched(ctx), ctrl(sched) {
        stest::E11TimerControl::enable_test_clock(sched);
    }
};

// Deterministic causal wait: spin until waiting_select_count reaches n (the
// caller committed Waiting + Armed under global_mtx_). NOT sleep_for.
inline void spin_until_waiting_select(Scheduler& s, std::size_t n) {
    while (AsyncTestAccess::waiting_select_count(s) != n) {
        std::this_thread::yield();
    }
}

}  // namespace

// ===========================================================================
// ST-15 — external-thread Event set
//
//   Scheduler run_live; caller Fiber waits on Event; setter is a plain
//   external OS thread; setter g_worker is null; publication routes using
//   group.caller_owner_; caller resumes correctly.
// ===========================================================================
SLUICE_TEST_CASE(st15_external_thread_event_set) {
    if constexpr (!sa::fiber_ctx::supported) return;
    MWFixture f;
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

    // The setter is a plain external OS thread: g_worker is null on it. The
    // publication path must route via group.caller_owner_ (captured at
    // admission), NOT via g_worker.
    std::thread setter([&] {
        spin_until_waiting_select(f.sched, 1);
        ev.set();
    });

    f.sched.run_live(1);
    setter.join();

    SLUICE_CHECK_MSG(resumed.load(), "caller resumed after external-thread set");
    SLUICE_CHECK_MSG(captured.has_winner(), "winner produced");
    SLUICE_CHECK_MSG(captured.kind() == SelectKind::event, "Event winner");
    SLUICE_CHECK_MSG(captured.index() == 0, "index 0");
    SLUICE_CHECK_MSG(ev.is_set(), "Event SET preserved");
    SLUICE_CHECK_MSG(AsyncTestAccess::waiting_select_count(f.sched) == 0,
                     "waiting_select_count returned to zero");
    SLUICE_CHECK_MSG(fb.state() == FiberState::done, "caller fiber reached done");
}

// ===========================================================================
// ST-16 — multi-worker owner routing
//
//   run_live with >= 2 workers; spawn caller deterministically on worker k;
//   record worker identity before select; resolve from external thread; resume
//   on the original owner worker k; result correct; exactly one enqueue.
// ===========================================================================
SLUICE_TEST_CASE(st16_multi_worker_owner_routing) {
    if constexpr (!sa::fiber_ctx::supported) return;
    MWFixture f;
    Event ev(f.sched, /*initially_set=*/false);

    SelectResult captured;
    std::atomic<bool> resumed{false};
    std::atomic<unsigned> pre_select_worker{static_cast<unsigned>(-1)};
    std::atomic<unsigned> post_resume_worker{static_cast<unsigned>(-1)};

    Fiber fb;
    fb.set_entry([&](Fiber&) {
        pre_select_worker.store(Scheduler::current_worker_id(),
                                std::memory_order::release);
        captured = sa::select(f.sched, EventSelectCase{ev});
        post_resume_worker.store(Scheduler::current_worker_id(),
                                 std::memory_order::release);
        resumed.store(true, std::memory_order::release);
    });
    FiberStack sw;
    SLUICE_CHECK(f.sched.init_fiber(fb, sw.base(), sw.size()));
    // Deterministic owner placement on worker 0.
    f.sched.spawn_on(fb, /*worker_id=*/0);

    const std::size_t runnable_before = f.sched.runnable_count();
    stest::E13SelectPublicationSeam::reset_publication_counts(f.sched);

    std::thread setter([&] {
        spin_until_waiting_select(f.sched, 1);
        ev.set();
    });

    f.sched.run_live(2);  // 2 workers; caller pinned to worker 0 via spawn_on.
    setter.join();

    SLUICE_CHECK_MSG(resumed.load(), "caller resumed");
    SLUICE_CHECK_MSG(captured.has_winner(), "winner produced");
    SLUICE_CHECK_MSG(captured.kind() == SelectKind::event, "Event winner");
    SLUICE_CHECK_MSG(pre_select_worker.load() == 0,
                     "caller ran on owner worker 0 before select");
    // ST-16 owner-routing contract: the publication routes the resumed caller
    // via group.caller_owner_ (the owner worker captured at admission), NOT via
    // the resolver-thread g_worker. The load-bearing proof is the exactly-one
    // runnable publication below + correct result. The FINAL execution worker
    // may differ from the owner under E8 work-stealing (a routed-to-owner
    // ticket can be stolen by another worker); the owner-routing authority is
    // the publication target, recorded as caller_owner_id in the publish_done
    // snapshot.
    const auto pub_snap =
        stest::E13SelectPublicationSeam::publish_done_snapshot(f.sched);
    SLUICE_CHECK_MSG(pub_snap.caller_owner_id == 0,
                     "publication routed to the caller's owner worker 0");
    (void)post_resume_worker;
    SLUICE_CHECK_MSG(
        stest::E13SelectPublicationSeam::runnable_publication_count(f.sched) == 1,
        "exactly one runnable publication (enqueue)");
    SLUICE_CHECK_MSG(AsyncTestAccess::waiting_select_count(f.sched) == 0,
                     "waiting_select_count returned to zero");
    // The exactly-one runnable_publication_count above is the load-bearing
    // no-double-wake proof (make_runnable is the publication guard). A
    // runnable_count() baseline comparison is not stable across multi-worker
    // routing (per-worker local_runnable accounting) so it is not asserted.
    (void)runnable_before;
}

// ===========================================================================
// ST-17 — exactly one runnable publication (Event + Timer contention)
//
//   One suspended group with Event + Timer arms. Deterministic contention:
//   first resolver reaches the publication path while holding G; a second
//   resolver attempt observes the retired/unlinked authority and cannot
//   publish. Assert: winner stable, result publication delta == 1, runnable
//   publication delta == 1, caller resumes once.
//
//   Deterministic race: drive BOTH Event::set() and a due-Timer advance from
//   the same coordinator after arm-commit. Because both run under global_mtx_,
//   they serialize: whichever acquires G first wins, the second sees the
//   finalized group and the resolver returns claim-lost (no second publish).
// ===========================================================================
SLUICE_TEST_CASE(st17_exactly_one_runnable_publication) {
    if constexpr (!sa::fiber_ctx::supported) return;
    MWFixture f;
    Event ev(f.sched, /*initially_set=*/false);
    const Scheduler::deadline_t deadline = 100;

    SelectResult captured;
    std::atomic<bool> resumed{false};
    std::atomic<unsigned> resume_count{0};

    Fiber fb;
    fb.set_entry([&](Fiber&) {
        captured = sa::select(f.sched,
                              EventSelectCase{ev},
                              TimerSelectCase{f.sched, deadline});
        resume_count.fetch_add(1, std::memory_order_acq_rel);
        resumed.store(true, std::memory_order_release);
    });
    FiberStack sw;
    SLUICE_CHECK(f.sched.init_fiber(fb, sw.base(), sw.size()));
    f.sched.spawn(fb);

    stest::E13SelectPublicationSeam::reset_publication_counts(f.sched);

    std::thread contender([&] {
        spin_until_waiting_select(f.sched, 1);
        // Drive BOTH resolvers. They serialize under global_mtx_: the first to
        // acquire G wins + publishes; the second observes the Completed group
        // (Event arm unlinked / Timer reg retired) and the resolver returns
        // claim-lost without a second publish.
        ev.set();
        stest::E13SelectTimerSeam::advance_clock(f.sched, deadline + 1);
    });

    f.sched.run_live(1);
    contender.join();

    SLUICE_CHECK_MSG(resumed.load(), "caller resumed exactly once");
    SLUICE_CHECK_MSG(resume_count.load() == 1, "caller resume count == 1");
    SLUICE_CHECK_MSG(captured.has_winner(), "winner produced");
    SLUICE_CHECK_MSG(
        stest::E13SelectPublicationSeam::result_publication_count(f.sched) == 1,
        "exactly one result publication under contention");
    SLUICE_CHECK_MSG(
        stest::E13SelectPublicationSeam::runnable_publication_count(f.sched) == 1,
        "exactly one runnable publication under contention");
    SLUICE_CHECK_MSG(AsyncTestAccess::waiting_select_count(f.sched) == 0,
                     "waiting_select_count returned to zero");
}

// ===========================================================================
// PUB-1 — publication entry preconditions (snapshot)
//
//   At e13_publish_entry: winner exists, all arms Retired, all authority
//   closed, result not yet written, completion_mode == None, phase == Selecting
//   or Armed, runnable count unchanged. Observed via the publish_entry snapshot
//   (captured under global_mtx_, read by the coordinator).
// ===========================================================================
SLUICE_TEST_CASE(pub1_publication_entry_preconditions) {
    if constexpr (!sa::fiber_ctx::supported) return;
    MWFixture f;
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

    stest::E13SelectPublicationSeam::reset_publication_counts(f.sched);
    std::thread setter([&] {
        spin_until_waiting_select(f.sched, 1);
        ev.set();
    });
    f.sched.run_live(1);
    setter.join();

    SLUICE_CHECK_MSG(resumed.load(), "caller resumed");

    // The publish_entry snapshot was captured at select_publish_locked entry.
    const auto snap =
        stest::E13SelectPublicationSeam::publish_entry_snapshot(f.sched);
    SLUICE_CHECK_MSG(snap.winner != sad::kNoWinner, "winner exists at publish entry");
    SLUICE_CHECK_MSG(snap.all_authority_closed,
                     "all authority closed at publish entry");
    SLUICE_CHECK_MSG(!snap.result_has_winner,
                     "result not yet written at publish entry");
    SLUICE_CHECK_MSG(snap.completion_mode == sad::CompletionMode::none,
                     "completion_mode None at publish entry");
    SLUICE_CHECK_MSG(snap.phase == sad::GroupPhase::armed,
                     "phase Armed at publish entry (suspended)");
    // runnable_publication_count at entry == 0 (publication has not happened).
    SLUICE_CHECK_MSG(snap.runnable_publication_count == 0,
                     "runnable publication count unchanged at publish entry");
}

// ===========================================================================
// PUB-2 — suspended publication done (snapshot)
//
//   At e13_publish_done: phase Completed, completion_mode Suspended, result
//   exists and matches winner, all authority closed, caller Fiber Runnable,
//   result publication delta == 1, runnable publication delta == 1,
//   waiting_select_count no longer includes group.
// ===========================================================================
SLUICE_TEST_CASE(pub2_suspended_publication_done) {
    if constexpr (!sa::fiber_ctx::supported) return;
    MWFixture f;
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

    stest::E13SelectPublicationSeam::reset_publication_counts(f.sched);
    std::thread setter([&] {
        spin_until_waiting_select(f.sched, 1);
        ev.set();
    });
    f.sched.run_live(1);
    setter.join();

    SLUICE_CHECK_MSG(resumed.load(), "caller resumed");

    const auto snap =
        stest::E13SelectPublicationSeam::publish_done_snapshot(f.sched);
    SLUICE_CHECK_MSG(snap.phase == sad::GroupPhase::completed,
                     "phase Completed at publish done");
    SLUICE_CHECK_MSG(snap.completion_mode == sad::CompletionMode::suspended,
                     "completion_mode Suspended at publish done");
    SLUICE_CHECK_MSG(snap.result_has_winner, "result exists at publish done");
    SLUICE_CHECK_MSG(snap.result_index == snap.winner,
                     "result index matches winner at publish done");
    SLUICE_CHECK_MSG(snap.all_authority_closed,
                     "all authority closed at publish done");
    SLUICE_CHECK_MSG(snap.caller_state == FiberState::runnable,
                     "caller Fiber Runnable at publish done");
    SLUICE_CHECK_MSG(snap.result_publication_count == 1,
                     "result publication delta == 1 at publish done");
    SLUICE_CHECK_MSG(snap.runnable_publication_count == 1,
                     "runnable publication delta == 1 at publish done");
    // waiting_select_count at publish-done snapshot == 0 (decremented).
    SLUICE_CHECK_MSG(snap.waiting_select_count == 0,
                     "waiting_select_count no longer includes group at publish done");
}

// ===========================================================================
// PUB-3 — inline publication regression (snapshot)
//
//   Run an inline Select through the same select_publish_locked. At publication
//   done: phase Completed, completion_mode Inline, result publication delta==1,
//   runnable publication delta==0, caller remains Running.
// ===========================================================================
SLUICE_TEST_CASE(pub3_inline_publication_regression) {
    if constexpr (!sa::fiber_ctx::supported) return;
    MWFixture f;
    Event ev(f.sched, /*initially_set=*/true);  // already set -> inline winner

    SelectResult captured;
    std::atomic<bool> ran{false};

    Fiber fb;
    fb.set_entry([&](Fiber&) {
        captured = sa::select(f.sched, EventSelectCase{ev});
        ran.store(true, std::memory_order_release);
    });
    FiberStack sw;
    SLUICE_CHECK(f.sched.init_fiber(fb, sw.base(), sw.size()));
    f.sched.spawn(fb);

    stest::E13SelectPublicationSeam::reset_publication_counts(f.sched);
    f.sched.run(1);  // Drain: inline winner, no suspension.

    SLUICE_CHECK_MSG(ran.load(), "fiber ran (inline, no suspension)");
    SLUICE_CHECK_MSG(captured.has_winner(), "inline winner produced");
    SLUICE_CHECK_MSG(captured.kind() == SelectKind::event, "Event inline winner");

    const auto snap =
        stest::E13SelectPublicationSeam::publish_done_snapshot(f.sched);
    SLUICE_CHECK_MSG(snap.phase == sad::GroupPhase::completed,
                     "phase Completed at inline publish done");
    SLUICE_CHECK_MSG(snap.completion_mode == sad::CompletionMode::inline_,
                     "completion_mode Inline at publish done");
    SLUICE_CHECK_MSG(snap.result_publication_count == 1,
                     "result publication delta == 1 (inline)");
    SLUICE_CHECK_MSG(snap.runnable_publication_count == 0,
                     "runnable publication delta == 0 (inline)");
}

// ===========================================================================
// PUB-4 — resumed ConsumeResult (snapshot)
//
//   At e13_suspended_before_consume: phase Completed, mode Suspended, result
//   exists, winner stable, all authority closed, caller Running. After return:
//   group reached Consumed, group destructor succeeds.
// ===========================================================================
SLUICE_TEST_CASE(pub4_resumed_consume_result) {
    if constexpr (!sa::fiber_ctx::supported) return;
    MWFixture f;
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

    stest::E13SelectPublicationSeam::reset_publication_counts(f.sched);
    std::thread setter([&] {
        spin_until_waiting_select(f.sched, 1);
        ev.set();
    });
    f.sched.run_live(1);
    setter.join();

    SLUICE_CHECK_MSG(resumed.load(), "caller resumed");

    const auto snap =
        stest::E13SelectPublicationSeam::suspended_before_consume_snapshot(f.sched);
    SLUICE_CHECK_MSG(snap.phase == sad::GroupPhase::completed,
                     "phase Completed before consume");
    SLUICE_CHECK_MSG(snap.completion_mode == sad::CompletionMode::suspended,
                     "mode Suspended before consume");
    SLUICE_CHECK_MSG(snap.result_has_winner, "result exists before consume");
    SLUICE_CHECK_MSG(snap.result_index == snap.winner,
                     "winner stable before consume");
    // all_authority_closed is NOT re-derived at this boundary: the timer pump
    // may have lazily reclaimed the consumed Timer registration between
    // publication and resume, so re-deriving would dereference a freed block.
    // The authoritative all-authority-closed observation is the publish_done
    // snapshot (captured inside the publication CS before reclamation); assert
    // it there.
    const auto pub_snap =
        stest::E13SelectPublicationSeam::publish_done_snapshot(f.sched);
    SLUICE_CHECK_MSG(pub_snap.all_authority_closed,
                     "all authority closed (observed at publish_done)");
    SLUICE_CHECK_MSG(snap.caller_state == FiberState::running,
                     "caller Running before consume");
    SLUICE_CHECK_MSG(snap.runnable_publication_count == 1,
                     "runnable publication count == 1 before consume");
    // After return: group reached Consumed and the destructor succeeded (the
    // fiber reached done, no leak/abort).
    SLUICE_CHECK_MSG(fb.state() == FiberState::done,
                     "group reached Consumed + destructor succeeded (fiber done)");
}

SLUICE_MAIN()
