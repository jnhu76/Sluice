// fe2_publication_atomicity_death_test — FE-CORRECTIVE-1 P1-1 witness.
//
// Proves the deferred-publication FAILURE boundary: once a terminal winner
// is committed (resolve_ CAS + unlink + resource/accounting closure), a
// storage failure at the transit-list insertion edge MUST NOT return
// control to a continuing process with the delivery obligation lost — the
// only legal outcomes are (a) the insertion succeeds, or (b) the NAMED
// process-terminal fail-fast fires (scheduler_deferred_publication_
// stranded_fail_fast, the same boundary ~Scheduler uses for a stranded
// entry).
//
// Cases (POSIX fork/exec self-re-exec; see death_test_runner_posix.hpp):
//
//   PUB1  — armed deferred Event waiter + one-shot storage-failure
//           injection + event.set(). The resolver commits the winner, then
//           reaches defer_publication_locked, where the synthetic bad_alloc
//           MUST be contained by the named fail-fast (exit 86). The child
//           wraps event.set() in its own try/catch: if the boundary were
//           missing (mutation M1), the bad_alloc would ESCAPE as a
//           recoverable-looking exception while the continuation stays
//           suspended forever — the child detects exactly that shape and
//           exits kChildTestFailExit so the parent fails.
//   PUBCTL — control, injection disarmed: the same scenario stores the
//           publication (transit depth 1, node terminal Woken), then the
//           take/discharge split consumes it. Exit 0 proves the scenario
//           really reaches the publication edge and the healthy path is
//           intact.
//
// The injection is the narrowest local seam (one-shot controller flag
// consumed at the insertion edge under global_mtx_ inside
// defer_publication_locked); no generic allocator fault-injection
// machinery exists or is added. Internal-testing build only; POSIX-only.
#include "death_test_runner_posix.hpp"

#if defined(__unix__)
#include "async_test_control.hpp"

#include <sluice/async/async_io_context.hpp>
#include <sluice/async/event.hpp>
#include <sluice/async/fake_backend.hpp>
#include <sluice/async/scheduler.hpp>
#include <sluice/async/wait_node.hpp>

#include <cstdlib>  // std::_Exit
#include <iostream>
#include <new>      // std::bad_alloc
#include <string>

namespace {

using sluice::async::AsyncIoContext;
using sluice::async::Event;
using sluice::async::FakeAsyncBackend;
using sluice::async::Scheduler;
using sluice::async::WaitNode;
using sluice::async::WaitOutcome;
using AsyncTestAccess = Scheduler::AsyncTestAccess;
using FeRecord = AsyncTestAccess::FeDeferredRecord;

// The witness scenario WITHOUT a coroutine: the deferred epoch is registered
// through the same internal-testing seam the FE-2 PoV awaiter uses, with a
// case-owned WaitNode + FeDeferredRecord (address-stable for the case
// lifetime). PUB1 never resumes (the process must terminate first); PUBCTL
// discharges the record through try_consume without a resumable handle.
struct WitnessEpoch {
    AsyncIoContext ctx{std::make_unique<FakeAsyncBackend>()};
    Scheduler sched{ctx};
    Event event{sched};
    WaitNode node{};
    FeRecord rec{};

    WitnessEpoch() { rec.handle_address = nullptr; }

    // Register + arm the deferred waiter. Returns false if the admission did
    // not authorize suspension (the scenario requires a PARKED waiter).
    bool park() {
        return AsyncTestAccess::event_wait_deferred_for_test(sched, event,
                                                             node, rec);
    }
};

void child_pub1_storage_failure_terminal() {
    WitnessEpoch w;
    // The injection controller must be registered on this Scheduler before
    // arming (the arm/consume pair is a no-op without a controller).
    sluice_async_test::ControllerGuard cg(w.sched);
    if (!w.park()) {
        std::_Exit(sluice_death_test::kChildTestFailExit);
    }
    // Parked: nothing in flight yet.
    if (AsyncTestAccess::deferred_depth_for_test(w.sched) != 0) {
        std::_Exit(sluice_death_test::kChildTestFailExit);
    }
    // One-shot synthetic allocation failure at the NEXT
    // defer_publication_locked insertion edge.
    sluice_async_test::TimerTestControl::arm_deferred_publication_failure(
        w.sched);
    try {
        w.event.set();  // resolve winner -> resource closure -> publication
        // Reaching here means set() returned: with the boundary in place the
        // fail-fast terminated the process inside set(); the ONLY way back
        // is a boundary bypass.
        std::_Exit(sluice_death_test::kUnexpectedReturnExit);
    } catch (const std::bad_alloc&) {
        // MUTATION SHAPE (M1): the storage failure escaped the winner tail
        // as a catchable exception. The terminal winner is already committed
        // (the node below is terminal + unlinked; accounting closed), the
        // continuation stays suspended forever, and no teardown gate can
        // observe the loss. Report the lost-obligation shape distinctly.
        std::_Exit(sluice_death_test::kChildTestFailExit);
    } catch (...) {
        std::_Exit(sluice_death_test::kChildTestFailExit);
    }
}

void child_pubctl_healthy_publication() {
    WitnessEpoch w;
    if (!w.park()) {
        std::_Exit(sluice_death_test::kChildTestFailExit);
    }
    w.event.set();  // no injection: the insertion must succeed
    // The winner committed AND the obligation was stored.
    if (AsyncTestAccess::deferred_depth_for_test(w.sched) != 1) {
        std::_Exit(sluice_death_test::kChildTestFailExit);
    }
    if (w.node.outcome() != WaitOutcome::woken) {
        std::_Exit(sluice_death_test::kChildTestFailExit);
    }
    // take/discharge split: the take empties the transit list; the discharge
    // consumes the record exactly once (no resumable handle is needed — the
    // record's exactly-once guard is the discharge contract under test).
    void* buf[4] = {};
    if (AsyncTestAccess::take_deferred_for_test(w.sched, buf, 4) != 1 ||
        buf[0] != static_cast<void*>(&w.rec)) {
        std::_Exit(sluice_death_test::kChildTestFailExit);
    }
    if (!w.rec.try_consume() || w.rec.try_consume()) {
        std::_Exit(sluice_death_test::kChildTestFailExit);  // not exactly-once
    }
    if (AsyncTestAccess::deferred_depth_for_test(w.sched) != 0) {
        std::_Exit(sluice_death_test::kChildTestFailExit);
    }
    // The Scheduler teardown gate passes: no stranded never-taken entry.
    std::_Exit(0);
}

void dispatch_child(const std::string& name) {
    sluice_death_test::install_deterministic_terminate_handler();
    if (name == "PUB1") child_pub1_storage_failure_terminal();
    if (name == "PUBCTL") child_pubctl_healthy_publication();
    std::cerr << "[death] unknown child case: " << name << "\n";
    std::_Exit(sluice_death_test::kChildTestFailExit);
}

int run_parent() {
    int failures = 0;
    // PUB1: the storage failure MUST be terminal (named fail-fast), in BOTH
    // Debug and Release (a Release-only escape would strand the obligation
    // with no gate able to see it).
    {
        auto r = sluice_death_test::run_death_case("PUB1");
        if (!sluice_death_test::expect_terminated_via_fail_fast(r)) ++failures;
    }
    // PUBCTL: healthy path completes normally.
    {
        auto r = sluice_death_test::run_death_case("PUBCTL");
        if (!sluice_death_test::expect_normal_exit_zero(r)) ++failures;
    }
    if (failures == 0) {
        std::cout << "ALL DEATH TESTS PASSED\n";
        return 0;
    }
    std::cout << failures << " death-test case(s) FAILED\n";
    return 1;
}

}  // namespace

int main(int argc, char** argv) {
    std::string child_case = sluice_death_test::parse_child_case(argc, argv);
    if (!child_case.empty()) {
        dispatch_child(child_case);  // never returns
        return sluice_death_test::kChildTestFailExit;  // unreachable
    }
    return run_parent();
}

#else  // !defined(__unix__)

#include <iostream>

int main() {
    std::cout << "fe2_publication_atomicity_death_test: NOT RUN on this "
                 "platform (POSIX fork/exec harness only)\n";
    return 0;
}

#endif  // defined(__unix__)
