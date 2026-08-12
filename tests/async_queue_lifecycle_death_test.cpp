// async_queue_lifecycle_death_test — #86-A QueuePort lifecycle serialization.
//
// Deterministic POSIX death tests (fork/exec/waitpid) that prove the
// QueuePort lifecycle ordering contract for `active_port_calls_`:
//
//   ordinary call entered
//       -> active_port_calls_ == 1 (CallGuard alive on the in-flight call)
//       -> ordinary call held before exit
//       -> concurrent begin_teardown()
//       -> teardown MUST NOT pass while the call is active (fail-fast)
//       -> (after the call exits) teardown follows the documented contract
//
// `begin_teardown()` is specified to FAIL-FAST (std::terminate), not block,
// when an ordinary operation is still inside QueuePort. The truthful
// deterministic contract is therefore process termination (exit 86 via the
// death-test harness), not a recoverable result. These tests exercise that
// exact contract.
//
// The lifecycle gate + active_port_calls_ increment are atomic w.r.t.
// begin_teardown under G+S (F.4 corrective). The #86-A fix additionally puts
// the CallGuard dtor DECREMENT under G+S so the counter is observed
// consistently (the pre-fix unsynchronized decrement was a data race and a
// lifecycle-protocol break). The control case below proves the "call exited
// -> teardown succeeds" half of the contract.
//
// POSIX only (fork/exec/waitpid). Gated to linux/macOS.
#include "death_test_runner_posix.hpp"
#include "harness.hpp"

#if defined(__unix__) || defined(__APPLE__)

#include <sluice/async/async_io_context.hpp>
#include <sluice/async/detail/queue_item.hpp>
#include <sluice/async/detail/queue_port.hpp>
#include <sluice/async/detail/queue_test_seam.hpp>
#include <sluice/async/fake_backend.hpp>
#include <sluice/async/fiber.hpp>
#include <sluice/async/fiber_ctx.hpp>
#include <sluice/async/scheduler.hpp>

#include <atomic>
#include <cstddef>
#include <cstdlib>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

using namespace sluice::async;
using namespace sluice::async::detail;
using sluice::Result;

namespace {

// A backend that never completes anything. The lifecycle tests never drive
// backend progress; an idle backend suffices.
class IdleBackend : public AsyncBackend {
public:
    Result<void> submit_read(ReadOp, Completion<std::size_t>&) override { return {}; }
    Result<void> submit_write(WriteOp, Completion<std::size_t>&) override { return {}; }
    Result<void> submit_sync_data(SyncDataOp, Completion<void>&) override { return {}; }
    Result<void> submit_sync_all(SyncAllOp, Completion<void>&) override { return {}; }
    std::size_t poll() override { return 0; }
    Result<std::size_t> wait_one() override { return 0; }
    void cancel(Completion<std::size_t>&) override {}
    void cancel(Completion<void>&) override {}
    std::size_t outstanding() const noexcept override { return 0; }
};

struct FiberStack {
    static constexpr std::size_t kBytes = 64 * 1024;
    alignas(16) std::vector<std::byte> bytes{kBytes};
    std::byte* base() noexcept { return bytes.data(); }
    std::size_t size() const noexcept { return bytes.size(); }
};

// ---- Child: begin_teardown MUST fail-fast while an ordinary call is active --
//
// A consumer Fiber calls pop() on an empty, open ring and PARKS (suspends).
// While it is parked the CallGuard constructed at QueuePort::pop entry is
// still alive on the suspended Fiber's stack, so active_port_calls_ == 1.
//
// A second Fiber (spawned AFTER the consumer, so it is behind it in the
// single-worker runnable FIFO) then calls begin_teardown(). With run(1)
// (single worker, cooperative, FIFO scheduling) the consumer Fiber MUST run
// first and suspend before the teardown Fiber becomes runnable; therefore
// active_port_calls_ == 1 deterministically when begin_teardown runs.
// begin_teardown requires active_port_calls_ == 0 under G+S, so it MUST
// fail-fast (the parked consumer also leaves active_wait_associations_ != 0
// and the consumer role FIFO non-empty; any of these preconditions blocks
// teardown — collectively they prove teardown cannot pass while an ordinary
// call is in flight).
void child_teardown_while_call_active() {
    sluice_death_test::install_deterministic_terminate_handler();
    AsyncIoContext ctx(std::make_unique<IdleBackend>());
    Scheduler sched(ctx);
    QueuePort port(sched, 2);  // empty, open ring

    Fiber consumer;
    consumer.set_entry([&](Fiber&) {
        // pop() on an empty OPEN ring registers + suspends. The CallGuard
        // stays alive on this suspended stack -> active_port_calls_ == 1.
        // Never resumes before teardown: begin_teardown terminates the
        // process first (no producer, no wake).
        auto r = port.pop();
        (void)r;
    });
    FiberStack sc;
    if (!sched.init_fiber(consumer, sc.base(), sc.size())) {
        std::_Exit(sluice_death_test::kChildTestFailExit);
    }
    sched.spawn(consumer);  // spawned FIRST -> runs first under run(1) FIFO

    Fiber teardown;
    teardown.set_entry([&](Fiber&) {
        // Deterministic: run(1) single-worker cooperative scheduling ran the
        // consumer first (FIFO spawn order) and it suspended in pop(); this
        // Fiber is now runnable with active_port_calls_ == 1. begin_teardown
        // MUST fail-fast here.
        port.begin_teardown();
        // If begin_teardown returned, the lifecycle gate failed to exclude an
        // active ordinary call. The ring is empty, so the returned session
        // destroys cleanly; signal the unexpected-return failure.
        std::_Exit(sluice_death_test::kUnexpectedReturnExit);
    });
    FiberStack st;
    if (!sched.init_fiber(teardown, st.base(), st.size())) {
        std::_Exit(sluice_death_test::kChildTestFailExit);
    }
    sched.spawn(teardown);  // spawned SECOND -> runs after consumer suspends

    sched.run(1);
    // run(1) returning means begin_teardown did not terminate the process
    // (the consumer is still parked). Lifecycle gate broken.
    std::_Exit(sluice_death_test::kUnexpectedReturnExit);
}

// ---- Child (control): begin_teardown MUST succeed after a call returned ----
//
// After an ordinary call has fully returned, its CallGuard dtor has
// decremented active_port_calls_ back to 0. begin_teardown must observe 0
// and succeed (exit 0). This proves the "ordinary call exits -> subsequent
// teardown follows the documented contract" half of the lifecycle ordering.
// Under the #86-A pre-fix bug an unsynchronized decrement could let
// begin_teardown observe a stale active_port_calls_ != 0 and spuriously
// fail-fast; that would surface here as exit 86 (not 0).
void child_teardown_after_call_returns() {
    sluice_death_test::install_deterministic_terminate_handler();
    AsyncIoContext ctx(std::make_unique<IdleBackend>());
    Scheduler sched(ctx);
    QueuePort port(sched, 2);

    // An ordinary call that returns immediately (try_push committed). Its
    // CallGuard dtor decrements active_port_calls_ back to 0 on return.
    {
        auto lease = QueueItemFactory::make<int>(port, 42);
        auto r = port.try_push(std::move(lease));
        if (r.status() != QueueOpaquePushStatus::committed) {
            std::_Exit(sluice_death_test::kChildTestFailExit);
        }
    }
    // try_push has returned; active_port_calls_ == 0. Drain the item so the
    // teardown session observes an empty ring, then begin_teardown MUST
    // succeed (all four counters == 0, both role FIFOs empty).
    auto rp = port.try_pop();
    if (rp.status() != QueueOpaquePopStatus::item) {
        std::_Exit(sluice_death_test::kChildTestFailExit);
    }
    QueueItemLease rp_lease = std::move(rp).take_item_lease();
    QueueItemFactory::release_popped<int>(port, std::move(rp_lease));

    QueueTeardownSession session = port.begin_teardown();  // MUST succeed
    if (!session.empty()) {
        std::_Exit(sluice_death_test::kChildTestFailExit);
    }
    std::_Exit(0);
}

// ---- Child: snapshot after teardown MUST fail-fast (one child per projection)
//
// begin_teardown() succeeds on the quiet port; a LATER snapshot is an ordinary
// call whose lifecycle entry rejects tearing_down before CallGuard
// construction (Corrective-2 §6/§7, adversarial trace #29). Each projection
// gets its own child so every fail-fast path is exercised independently. If
// the snapshot returned, the lifecycle gate failed and the child reports
// kUnexpectedReturnExit (87).
void child_snapshot_after_teardown_is_closed() {
    sluice_death_test::install_deterministic_terminate_handler();
    AsyncIoContext ctx(std::make_unique<IdleBackend>());
    Scheduler sched(ctx);
    QueuePort port(sched, 2);

    QueueTeardownSession session = port.begin_teardown();  // MUST succeed
    if (!session.empty()) {
        std::_Exit(sluice_death_test::kChildTestFailExit);
    }
    (void)port.is_closed();  // MUST fail-fast (tearing_down)
    std::_Exit(sluice_death_test::kUnexpectedReturnExit);  // unreachable
}

void child_snapshot_after_teardown_capacity() {
    sluice_death_test::install_deterministic_terminate_handler();
    AsyncIoContext ctx(std::make_unique<IdleBackend>());
    Scheduler sched(ctx);
    QueuePort port(sched, 2);

    QueueTeardownSession session = port.begin_teardown();  // MUST succeed
    if (!session.empty()) {
        std::_Exit(sluice_death_test::kChildTestFailExit);
    }
    (void)port.capacity();  // MUST fail-fast (tearing_down)
    std::_Exit(sluice_death_test::kUnexpectedReturnExit);  // unreachable
}

void child_snapshot_after_teardown_size() {
    sluice_death_test::install_deterministic_terminate_handler();
    AsyncIoContext ctx(std::make_unique<IdleBackend>());
    Scheduler sched(ctx);
    QueuePort port(sched, 2);

    QueueTeardownSession session = port.begin_teardown();  // MUST succeed
    if (!session.empty()) {
        std::_Exit(sluice_death_test::kChildTestFailExit);
    }
    (void)port.size();  // MUST fail-fast (tearing_down)
    std::_Exit(sluice_death_test::kUnexpectedReturnExit);  // unreachable
}

// ---- Child: begin_teardown MUST fail-fast while a snapshot is active ------
//
// The main thread arms the internal-testing pause gate and calls size(). The
// snapshot performs its G+S lifecycle admission (active_port_calls_ == 1),
// constructs its CallGuard, then pauses at the seam (paused published AFTER
// admission). A second OS thread waits for `paused` — provably after the
// snapshot is inside the ordinary-call interval — and calls begin_teardown():
// with active_port_calls_ == 1 the quiet precondition is violated, so teardown
// MUST fail-fast. Deterministic: the teardown thread cannot run before the
// snapshot is admitted, and the snapshot cannot retire before the teardown
// thread runs (it spins on the gate). No sleeps; the pause protocol is the
// ordering proof (AGENTS.md §13.3).
//
// Pre-fix (snapshot without lifecycle entry): size() never increments
// active_port_calls_, begin_teardown returns a session, the teardown thread
// releases the gate, size() returns, and the child exits 87 — the regression
// fails for the intended reason.
void child_teardown_while_snapshot_active() {
    sluice_death_test::install_deterministic_terminate_handler();
    AsyncIoContext ctx(std::make_unique<IdleBackend>());
    Scheduler sched(ctx);
    QueuePort port(sched, 2);  // empty, open ring

    QueueSnapshotPauseGate gate;
    test_hooks::arm_queue_snapshot_pause(gate);

    std::atomic<bool> teardown_returned{false};
    std::thread teardown_thread([&] {
        // Wait until the snapshot has published `paused` (after its G+S
        // admission; active_port_calls_ == 1), then teardown MUST fail-fast.
        test_hooks::queue_snapshot_wait_paused(gate);
        port.begin_teardown();
        // Reachable only when the lifecycle gate failed to exclude the active
        // snapshot (pre-fix). Release the paused snapshot so the child can
        // report the violation and exit cleanly.
        teardown_returned.store(true, std::memory_order_release);
        test_hooks::release_queue_snapshot_pause(gate);
    });

    const std::size_t n = port.size();  // admits, increments, pauses at seam
    (void)n;
    if (teardown_returned.load(std::memory_order_acquire)) {
        // begin_teardown returned while the snapshot was inside QueuePort —
        // the lifecycle gate failed to exclude it.
        std::_Exit(sluice_death_test::kUnexpectedReturnExit);
    }
    // Fixed path unreachable: begin_teardown terminates the process from the
    // teardown thread while this thread still spins in the seam.
    test_hooks::release_queue_snapshot_pause(gate);
    teardown_thread.join();
    std::_Exit(sluice_death_test::kUnexpectedReturnExit);
}

// ---- Child (control): snapshot returns -> CallGuard retires -> teardown OK -
//
// After a snapshot has fully returned, its CallGuard dtor has decremented
// active_port_calls_ back to 0 under G+S. begin_teardown must then succeed —
// proving the "ordinary call exits -> teardown follows the documented
// contract" half of the lifecycle ordering for the snapshot class. A leaked
// guard would surface as exit 86 (spurious fail-fast).
void child_snapshot_then_teardown() {
    sluice_death_test::install_deterministic_terminate_handler();
    AsyncIoContext ctx(std::make_unique<IdleBackend>());
    Scheduler sched(ctx);
    QueuePort port(sched, 2);

    // All three projections while operational; values must be correct.
    if (port.is_closed() || port.capacity() != 2 || port.size() != 0) {
        std::_Exit(sluice_death_test::kChildTestFailExit);
    }
    QueueTeardownSession session = port.begin_teardown();  // MUST succeed
    if (!session.empty()) {
        std::_Exit(sluice_death_test::kChildTestFailExit);
    }
    std::_Exit(0);
}

}  // namespace

// ---- Parent test cases ------------------------------------------------------

SLUICE_TEST_CASE(queue_lifecycle_death_teardown_while_call_active) {
    auto r = sluice_death_test::run_death_case("teardown-while-call-active");
    SLUICE_CHECK_MSG(
        sluice_death_test::expect_terminated_via_fail_fast(r),
        "begin_teardown must fail-fast while an ordinary QueuePort call is "
        "active (active_port_calls_ != 0)");
}

SLUICE_TEST_CASE(queue_lifecycle_death_control_teardown_after_call) {
    auto r = sluice_death_test::run_death_case("control-teardown-after-call");
    SLUICE_CHECK_MSG(
        sluice_death_test::expect_normal_exit_zero(r),
        "begin_teardown must succeed after an ordinary call has returned "
        "(active_port_calls_ == 0)");
}

// #86-D — Queue snapshot lifecycle compliance: snapshots are ordinary
// lifecycle-gated calls (Corrective-2 §7), so a snapshot after teardown MUST
// fail-fast and a snapshot inside the port MUST exclude begin_teardown.

SLUICE_TEST_CASE(queue_lifecycle_death_snapshot_is_closed_after_teardown) {
    auto r = sluice_death_test::run_death_case("snapshot-after-teardown-is-closed");
    SLUICE_CHECK_MSG(
        sluice_death_test::expect_terminated_via_fail_fast(r),
        "is_closed() after begin_teardown must fail-fast (snapshots are "
        "ordinary lifecycle-gated calls)");
}

SLUICE_TEST_CASE(queue_lifecycle_death_snapshot_capacity_after_teardown) {
    auto r = sluice_death_test::run_death_case("snapshot-after-teardown-capacity");
    SLUICE_CHECK_MSG(
        sluice_death_test::expect_terminated_via_fail_fast(r),
        "capacity() after begin_teardown must fail-fast (snapshots are "
        "ordinary lifecycle-gated calls)");
}

SLUICE_TEST_CASE(queue_lifecycle_death_snapshot_size_after_teardown) {
    auto r = sluice_death_test::run_death_case("snapshot-after-teardown-size");
    SLUICE_CHECK_MSG(
        sluice_death_test::expect_terminated_via_fail_fast(r),
        "size() after begin_teardown must fail-fast (snapshots are ordinary "
        "lifecycle-gated calls)");
}

SLUICE_TEST_CASE(queue_lifecycle_death_teardown_while_snapshot_active) {
    auto r = sluice_death_test::run_death_case("teardown-while-snapshot-active");
    SLUICE_CHECK_MSG(
        sluice_death_test::expect_terminated_via_fail_fast(r),
        "begin_teardown must fail-fast while a snapshot is inside QueuePort "
        "(active_port_calls_ == 1)");
}

SLUICE_TEST_CASE(queue_lifecycle_death_control_snapshot_then_teardown) {
    auto r = sluice_death_test::run_death_case("control-snapshot-then-teardown");
    SLUICE_CHECK_MSG(
        sluice_death_test::expect_normal_exit_zero(r),
        "begin_teardown must succeed after snapshots have returned "
        "(active_port_calls_ == 0)");
}

int main(int argc, char** argv) {
    std::string child_case = sluice_death_test::parse_child_case(argc, argv);
    if (!child_case.empty()) {
        if (child_case == "teardown-while-call-active") {
            child_teardown_while_call_active();
        } else if (child_case == "control-teardown-after-call") {
            child_teardown_after_call_returns();
        } else if (child_case == "snapshot-after-teardown-is-closed") {
            child_snapshot_after_teardown_is_closed();
        } else if (child_case == "snapshot-after-teardown-capacity") {
            child_snapshot_after_teardown_capacity();
        } else if (child_case == "snapshot-after-teardown-size") {
            child_snapshot_after_teardown_size();
        } else if (child_case == "teardown-while-snapshot-active") {
            child_teardown_while_snapshot_active();
        } else if (child_case == "control-snapshot-then-teardown") {
            child_snapshot_then_teardown();
        } else {
            std::cerr << "[death] unknown child case: " << child_case << "\n";
            std::_Exit(sluice_death_test::kChildTestFailExit);
        }
        std::_Exit(sluice_death_test::kChildTestFailExit);
    }
    return sluice_test::run_all();
}

#else
// Non-POSIX: no fork/exec/waitpid. Provide an empty main so the target links.
int main() { return 0; }
#endif  // POSIX
