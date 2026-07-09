// e10_corrective_c2_c3_test — E10-CORRECTIVE C2 resolution-authority + C3
// cancel_all surface (sluice-CORE-E10).
//
// C2 (accepted defect): a Scheduler-integrated wait (registered via
// Scheduler::await_wait, which owns waiting_waitq_count_ + the runnable-route
// obligation) could be terminally resolved by calling WaitQueue's PUBLIC
// wake_one / cancel / cancel_all directly — bypassing Scheduler::wake_wait_one
// / Scheduler::cancel_wait. That would resolve + unlink the node WITHOUT
// decrementing waiting_waitq_count_ and WITHOUT routing the fiber through the
// canonical wake seam (route_runnable_locked), stranding the fiber and leaving
// MW classification stale.
//
// C3 (authority drift): WaitQueue::cancel_all existed with ZERO production
// callsites, no authoritative E10 shutdown semantic required it, and its header
// text ("the Scheduler cancels-all on run termination") was false — the
// Scheduler does NOT auto-resolve waits on run termination. Decision: REMOVED.
//
// T2 (structural): the direct-resolution bypass is no longer EXPRESSIBLE.
// wake_one / cancel / cancel_all are gone from the public surface (wake_one/
// cancel are now private, friended only to Scheduler + the test hook;
// cancel_all is deleted). This is a compile-time structural proof: the bypass
// literally does not compile. It uses static_assert + member-detection so a
// future regression (re-adding a public resolver) fails the build.
//
// T4 (non-bypass): a Scheduler-integrated resolution via the Scheduler seams
// DOES route through the canonical wake seam AND keeps waiting_waitq_count_
// consistent. C10 already proves exactly-once resume; this adds the focused
// count-correctness assertion: after a wake/cancel, waiting_count()==0.
//
// T6 (cancel_all surface): cancel_all is not a member of WaitQueue (REMOVED).
//
// Gated to x86_64 (the T4 fiber case depends on fiber_ctx::supported).
#include "harness.hpp"

#include <sluice/async/async_io_context.hpp>
#include <sluice/async/fake_backend.hpp>
#include <sluice/async/fiber.hpp>
#include <sluice/async/fiber_ctx.hpp>
#include <sluice/async/scheduler.hpp>
#include <sluice/async/wait_node.hpp>
#include <sluice/async/wait_queue.hpp>

#include <type_traits>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <thread>

using namespace sluice::async;
using sluice::Result;

// =============================================================================
// T2 / T6 — structural: the direct-resolution bypass + cancel_all are NOT part
// of the public WaitQueue surface. Member-detection via SFINAE so this is a
// COMPILE-TIME proof that re-introducing a public resolver fails the build.
// =============================================================================
namespace {

// has_public_wake_one<Q>: true iff Q has a PUBLIC member `wake_one` callable
// with no args. The (now private) resolver is detected regardless of access,
// so we additionally gate on accessibility below by requiring the detected
// call expression to be WELL-FORMED from an UNFRIENDED context — which a
// private member is not. We approximate by detecting the member name and then
// asserting the unfriended-call SFINAE is false.
template <typename Q, typename = void>
struct has_public_wake_one : std::false_type {};
template <typename Q>
struct has_public_wake_one<Q,
    std::void_t<decltype(std::declval<Q&>().wake_one())>> : std::true_type {};

template <typename Q, typename = void>
struct has_public_cancel : std::false_type {};
// cancel takes a WaitNode&; detect that signature.
template <typename Q>
struct has_public_cancel<Q,
    std::void_t<decltype(std::declval<Q&>().cancel(std::declval<WaitNode&>()))>>
    : std::true_type {};

template <typename Q, typename = void>
struct has_cancel_all : std::false_type {};
template <typename Q>
struct has_cancel_all<Q,
    std::void_t<decltype(std::declval<Q&>().cancel_all())>> : std::true_type {};

}  // namespace

// T2: a public wake_one/cancel (the bypass) is NOT expressible. If a future
// change re-adds either, this static_assert fails the build.
static_assert(!has_public_wake_one<WaitQueue>::value,
    "C2: WaitQueue::wake_one must NOT be public (Scheduler-only resolution authority)");
static_assert(!has_public_cancel<WaitQueue>::value,
    "C2: WaitQueue::cancel must NOT be public (Scheduler-only resolution authority)");

// T6: cancel_all is removed from the surface entirely.
static_assert(!has_cancel_all<WaitQueue>::value,
    "C3: WaitQueue::cancel_all must be removed (no authoritative E10 shutdown semantic, "
    "zero production callsites)");

SLUICE_TEST_CASE(e10_corrective_c2_bypass_not_expressible_runtime_mirror) {
    // Runtime mirror of the compile-time proof above, so a release build that
    // somehow lost the static_assert (e.g. header rot) still catches a regression.
    SLUICE_CHECK_MSG(!has_public_wake_one<WaitQueue>::value,
        "wake_one is not public (no direct bypass)");
    SLUICE_CHECK_MSG(!has_public_cancel<WaitQueue>::value,
        "cancel is not public (no direct bypass)");
    SLUICE_CHECK_MSG(!has_cancel_all<WaitQueue>::value,
        "cancel_all is removed from the surface");
}

// =============================================================================
// T4 — Scheduler-integrated resolution keeps waiting_waitq_count_ consistent
// and routes through the canonical seam. C10 proves exactly-once resume; this
// asserts the COUNT invariants that a bypass would corrupt.
// =============================================================================
namespace {
struct FiberStack {
    static constexpr std::size_t kBytes = 64 * 1024;
    alignas(16) std::vector<std::byte> bytes{kBytes};
    std::byte* base() noexcept { return bytes.data(); }
    std::size_t size() const noexcept { return bytes.size(); }
};

// Idle backend (no outstanding ops) so MW stays at S3; the only progress is the
// Scheduler-integrated resolution.
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
}  // namespace

// T4a: wake_wait_one resolution decrements waiting_count back to 0 (no stale
// count from a bypass). A direct WaitQueue resolve would leave it at 1.
SLUICE_TEST_CASE(e10_corrective_c4_wake_keeps_wait_count_zero) {
    if constexpr (!fiber_ctx::supported) return;

    AsyncIoContext ctx(std::make_unique<IdleBackend>());
    Scheduler sched(ctx);

    WaitQueue q;
    WaitNode node;
    std::atomic<bool> suspended{false};
    std::atomic<int> entries{0};

    Fiber fwait, fwake;
    fwait.set_entry([&](Fiber&) {
        entries.fetch_add(1, std::memory_order_acq_rel);
        suspended.store(true, std::memory_order_release);
        sched.await_wait(q, node);
        entries.fetch_add(1, std::memory_order_acq_rel);
    });
    fwake.set_entry([&](Fiber&) {
        // The waiter sets `suspended` BEFORE it registers inside await_wait, so
        // the queue may be (briefly) empty when we first arrive. Retry until we
        // are the winner — this mirrors how an external waker must tolerate the
        // register-vs-wake race. Exactly one retry wins (the single-winner CAS).
        bool woke = false;
        while (!suspended.load(std::memory_order_acquire)) {}
        for (int i = 0; i < 1000 && !woke; ++i) {
            woke = sched.wake_wait_one(q);
        }
        SLUICE_CHECK_MSG(woke, "wake_wait_one won (canonical seam)");
    });

    FiberStack sw, sk;
    SLUICE_CHECK(sched.init_fiber(fwait, sw.base(), sw.size()));
    SLUICE_CHECK(sched.init_fiber(fwake, sk.base(), sk.size()));
    sched.spawn(fwait);
    sched.spawn(fwake);
    sched.run(2);

    SLUICE_CHECK_MSG(entries.load() == 2, "waiter resumed exactly once");
    // The load-bearing T4 assertion: the Scheduler-integrated resolution
    // decremented waiting_waitq_count_, so no stale unresolved-wait count.
    SLUICE_CHECK_MSG(sched.waiting_count() == 0,
        "waiting_count()==0 after wake_wait_one (no stale count from a bypass)");
    SLUICE_CHECK_MSG(node.was_woken(), "resolved Woken via the Scheduler seam");
}

// T4b: cancel_wait resolution also keeps waiting_count at 0.
SLUICE_TEST_CASE(e10_corrective_c4_cancel_keeps_wait_count_zero) {
    if constexpr (!fiber_ctx::supported) return;

    AsyncIoContext ctx(std::make_unique<IdleBackend>());
    Scheduler sched(ctx);

    WaitQueue q;
    WaitNode node;
    std::atomic<bool> suspended{false};
    std::atomic<int> entries{0};

    Fiber fwait, fcancel;
    fwait.set_entry([&](Fiber&) {
        entries.fetch_add(1, std::memory_order_acq_rel);
        suspended.store(true, std::memory_order_release);
        sched.await_wait(q, node);
        entries.fetch_add(1, std::memory_order_acq_rel);
    });
    fcancel.set_entry([&](Fiber&) {
        // Retry until winner: see T4a note — the waiter may not have registered
        // when we first arrive. Exactly one retry wins (the single-winner CAS).
        bool cancelled = false;
        while (!suspended.load(std::memory_order_acquire)) {}
        for (int i = 0; i < 1000 && !cancelled; ++i) {
            cancelled = sched.cancel_wait(q, node);
        }
        SLUICE_CHECK_MSG(cancelled, "cancel_wait won (canonical seam)");
    });

    FiberStack sw, sk;
    SLUICE_CHECK(sched.init_fiber(fwait, sw.base(), sw.size()));
    SLUICE_CHECK(sched.init_fiber(fcancel, sk.base(), sk.size()));
    sched.spawn(fwait);
    sched.spawn(fcancel);
    sched.run(2);

    SLUICE_CHECK_MSG(entries.load() == 2, "waiter resumed exactly once");
    SLUICE_CHECK_MSG(sched.waiting_count() == 0,
        "waiting_count()==0 after cancel_wait (no stale count from a bypass)");
    SLUICE_CHECK_MSG(node.was_cancelled(), "resolved Cancelled via the Scheduler seam");
}

SLUICE_MAIN()
