// FE-3 Condition vertical slice — the stackless C++20-coroutine frontend over
// the ONE shared CONDITION-WAIT-PREPARE admission ladder
// (Scheduler::condition_wait_admit_locked).
//
// What this slice PROVES (FE campaign, FE-3 Condition stage):
//   1. The Condition epoch is frontend-neutral: the deferred entry drives the
//      SAME ladder as the fiber entries (register-before-handoff single CS,
//      R2-ALLOC prepare, already-due retention, Mutex handoff, terminal
//      recheck) — no duplicated admission law.
//   2. The released_mutex law is observable at the seam: `released=false`
//      (rejected / already-due Expired) RETAINS the presented Mutex state;
//      `released=true` (authorized / post-handoff terminal) released it.
//   3. The winner runs its OWN reacquire epoch after resume: notify_one /
//      notify_all / cancel / expiry NEVER reacquire for the winner (the
//      presented owner stays released); the resumed body performs the
//      reacquire step itself — including on suspended-Expired and Cancelled.
//   4. notify_one / notify_all / cancel resolve through the ONE winner-kind
//      publication tail (a deferred winner commits a delivery obligation; the
//      discharge resumes with NO lock held — the FE-2 L9 property).
//
// Presented-state note (honest scope boundary, FE-1b A1 §12): Mutex ownership
// identity re-typing ("Mutex/RwLock owner fields are re-typed") is its own
// later slice — RwLock is done; a v1 stackless coroutine therefore cannot
// lawfully OWN an AsyncMutex, and the full AsyncCondition choreography
// composition stays covered by the unchanged fiber tests running over the
// SAME ladder (async_condition_primitive_test). The deferred cases here
// present BARE WaitQueues with an empty bound Mutex queue: the handoff is the
// documented UnlockNoWaiter no-op (`owner = nullptr`), and the sentinel
// non-null `owner` (a valid address that is never used AS a Fiber) makes the
// retain-vs-release law byte-observable.
#include <sluice/async/async_io_context.hpp>
#include <sluice/async/fake_backend.hpp>
#include <sluice/async/scheduler.hpp>
#include <sluice/async/wait_node.hpp>

#include "harness.hpp"
#include "async_test_control.hpp"

#include <coroutine>
#include <atomic>
#include <cstddef>
#include <memory>
#include <utility>

using namespace sluice::async;
using AsyncTestAccess = Scheduler::AsyncTestAccess;
using FeRecord = AsyncTestAccess::FeDeferredRecord;

namespace {

// Drain helper (FE-2/FE-3 shape): discharge ALL pending deferred
// publications; chunked take under G + resume with NO lock held (L9).
std::size_t drain_all(Scheduler& sched, std::atomic<int>& guard_failures) {
    std::size_t total = 0;
    void* buf[16];
    while (true) {
        const std::size_t n =
            AsyncTestAccess::take_deferred_for_test(sched, buf, 16);
        if (n == 0) break;
        for (std::size_t i = 0; i < n; ++i) {
            auto* rec = static_cast<FeRecord*>(buf[i]);
            if (!rec->try_consume()) {
                guard_failures.fetch_add(1, std::memory_order_relaxed);
                continue;
            }
            std::coroutine_handle<>::from_address(rec->handle_address)
                .resume();
            total += 1;
        }
    }
    return total;
}

// Minimal test-local void coroutine task (FE-2 PoV shape).
class FeTask {
public:
    struct promise_type {
        FeTask get_return_object() noexcept {
            return FeTask{
                std::coroutine_handle<promise_type>::from_promise(*this)};
        }
        std::suspend_always initial_suspend() noexcept { return {}; }
        std::suspend_always final_suspend() noexcept { return {}; }
        void return_void() noexcept {}
        void unhandled_exception() noexcept { std::terminate(); }
    };

    explicit FeTask(std::coroutine_handle<promise_type> h) noexcept : h_(h) {}
    FeTask(FeTask&& other) noexcept : h_(std::exchange(other.h_, {})) {}
    FeTask(const FeTask&) = delete;
    FeTask& operator=(const FeTask&) = delete;
    ~FeTask() {
        if (h_) h_.destroy();
    }

    void start() { h_.resume(); }
    bool done() const noexcept { return h_.done(); }

private:
    std::coroutine_handle<promise_type> h_;
};

// Awaiter over the SHARED condition admission ladder. node/rec are
// REFERENCES to caller-owned (case-owned) storage: address-stable across the
// suspension (FE-1a). `owner` is the presented bound-Mutex owner slot.
struct FeCondWaitAwaiter {
    Scheduler& sched;
    WaitQueue& cond_q;
    WaitNode& node;
    WaitQueue& mutex_q;
    Fiber*& owner;
    FeRecord& rec;
    bool timed = false;
    Scheduler::deadline_t deadline = 0;

    bool did_suspend = false;
    bool released = false;
    bool await_ready() const noexcept { return false; }
    bool await_suspend(std::coroutine_handle<> h) {
        // FE-1b L2: bind the resume target BEFORE the epoch becomes
        // resolver-observable.
        rec.handle_address = h.address();
        if (timed) {
            did_suspend =
                AsyncTestAccess::condition_wait_deferred_until_for_test(
                    sched, cond_q, node, mutex_q, owner, deadline, rec,
                    released);
        } else {
            did_suspend = AsyncTestAccess::condition_wait_deferred_for_test(
                sched, cond_q, node, mutex_q, owner, rec, released);
        }
        return did_suspend;
    }
    // The Condition node's terminal outcome (valid after resume, or inline
    // when await_suspend returned false).
    WaitOutcome await_resume() const noexcept { return node.outcome(); }
};

// One presented Condition-wait case: bare queues + sentinel Mutex owner.
struct alignas(max_align_t) FakeFiberStorage {
    std::byte bytes[64];
};

struct CondCase {
    Scheduler& sched;
    WaitQueue& cond_q;
    WaitQueue& mutex_q;
    Fiber*& owner;
    WaitNode& node;
    FeRecord& rec;
    std::atomic<int>& resumed;
    // Body-visible results.
    WaitOutcome outcome = WaitOutcome::unresolved;
    bool released = false;
    bool inline_done = false;     // the ladder resolved without suspension
    bool reacquire_done = false;  // the body's OWN reacquire-epoch step
    bool owner_null_at_resume = false;
    FeTask run(bool timed = false, Scheduler::deadline_t deadline = 0) {
        FeCondWaitAwaiter aw{sched, cond_q, node, mutex_q, owner, rec};
        aw.timed = timed;
        aw.deadline = deadline;
        outcome = co_await aw;
        released = aw.released;
        inline_done = !aw.did_suspend;
        if (released) {
            // The REACQUIRE EPOCH belongs to the winner's body (FE-3
            // Condition law): the resolver published Woken/Expired/Cancelled
            // and did NOT reacquire the Mutex for us. The reacquire happens
            // HERE, after resume — witnessed by the presented owner staying
            // released. A `released=false` resolution (rejected /
            // already-due Expired) RETAINS the Mutex: no reacquire epoch.
            owner_null_at_resume = (owner == nullptr);
            reacquire_done = true;
        }
        resumed.fetch_add(1, std::memory_order_relaxed);
        co_return;
    }
};

}  // namespace

// ---- notify_one: deferred winner resumes and runs its OWN reacquire -------
SLUICE_TEST_CASE(fe3_condition_deferred_notify_one_own_reacquire) {
    AsyncIoContext ctx(std::make_unique<FakeAsyncBackend>());
    Scheduler sched(ctx);
    WaitQueue cond_q;
    WaitQueue mutex_q;
    FakeFiberStorage sentinel_storage;
    Fiber* owner = reinterpret_cast<Fiber*>(sentinel_storage.bytes);
    std::atomic<int> resumed{0};
    std::atomic<int> guard_failures{0};

    WaitNode node;
    FeRecord rec;
    CondCase c{sched, cond_q, mutex_q, owner, node, rec, resumed};
    FeTask t = c.run();
    t.start();

    // Parked: the ladder ran the register-before-handoff CS — the presented
    // Mutex was RELEASED (sentinel -> nullptr) and the deferred publication
    // eligibility is armed. No winner exists yet, so nothing is in flight.
    SLUICE_CHECK(rec.state.load() == FeRecord::State::armed);
    SLUICE_CHECK(!t.done());
    SLUICE_CHECK(owner == nullptr);
    SLUICE_CHECK(AsyncTestAccess::deferred_depth_for_test(sched) == 0);

    // notify_one resolves the head Woken through the winner-kind tail: the
    // deferred branch commits a delivery obligation. The resolver does NOT
    // reacquire the Mutex for the winner.
    AsyncTestAccess::condition_notify_one_for_test(sched, cond_q);
    SLUICE_CHECK(AsyncTestAccess::deferred_depth_for_test(sched) == 1);
    SLUICE_CHECK(owner == nullptr);

    (void)drain_all(sched, guard_failures);
    // The body resumed and ran its OWN reacquire epoch (witnessed: the owner
    // slot was still released at resume time — the runtime never reacquired
    // for it; the reacquire flag is the body's own step).
    SLUICE_CHECK(resumed.load() == 1);
    SLUICE_CHECK(guard_failures.load() == 0);
    SLUICE_CHECK(t.done());
    SLUICE_CHECK(c.outcome == WaitOutcome::woken);
    SLUICE_CHECK(c.released);
    SLUICE_CHECK(c.owner_null_at_resume);
    SLUICE_CHECK(c.reacquire_done);
    SLUICE_CHECK(AsyncTestAccess::deferred_depth_for_test(sched) == 0);
}

// ---- already-due deadline: inline Expired, presented Mutex RETAINED -------
SLUICE_TEST_CASE(fe3_condition_deferred_due_inline_retains_mutex) {
    AsyncIoContext ctx(std::make_unique<FakeAsyncBackend>());
    Scheduler sched(ctx);
    sluice_async_test::TimerTestControl::enable_test_clock(sched);
    WaitQueue cond_q;
    WaitQueue mutex_q;
    FakeFiberStorage sentinel_storage;
    Fiber* owner = reinterpret_cast<Fiber*>(sentinel_storage.bytes);
    std::atomic<int> resumed{0};
    std::atomic<int> guard_failures{0};

    WaitNode node;
    FeRecord rec;
    const Scheduler::deadline_t deadline = AsyncTestAccess::clock_now(sched);
    CondCase c{sched, cond_q, mutex_q, owner, node, rec, resumed};
    FeTask t = c.run(/*timed=*/true, deadline);
    t.start();

    // WaitDueInline / InvDueInlineRetainsOwnership: resolved Expired INLINE,
    // the Mutex was NOT released (owner STILL the sentinel), the record was
    // never armed, and the reacquire obligation is FALSE.
    SLUICE_CHECK(t.done());
    SLUICE_CHECK(c.inline_done);
    SLUICE_CHECK(c.outcome == WaitOutcome::expired);
    SLUICE_CHECK(!c.released);
    SLUICE_CHECK(rec.state.load() == FeRecord::State::unarmed);
    SLUICE_CHECK(owner != nullptr);  // the sentinel survived: Mutex retained
    SLUICE_CHECK(AsyncTestAccess::deferred_depth_for_test(sched) == 0);
    SLUICE_CHECK(resumed.load() == 1);
    SLUICE_CHECK(guard_failures.load() == 0);
}

// ---- pump expiry: suspended Expired, the body STILL reacquires ------------
SLUICE_TEST_CASE(fe3_condition_deferred_pump_expiry_reacquire) {
    AsyncIoContext ctx(std::make_unique<FakeAsyncBackend>());
    Scheduler sched(ctx);
    sluice_async_test::TimerTestControl::enable_test_clock(sched);
    WaitQueue cond_q;
    WaitQueue mutex_q;
    FakeFiberStorage sentinel_storage;
    Fiber* owner = reinterpret_cast<Fiber*>(sentinel_storage.bytes);
    std::atomic<int> resumed{0};
    std::atomic<int> guard_failures{0};

    WaitNode node;
    FeRecord rec;
    const Scheduler::deadline_t deadline =
        AsyncTestAccess::clock_now(sched) + 50;
    CondCase c{sched, cond_q, mutex_q, owner, node, rec, resumed};
    FeTask t = c.run(/*timed=*/true, deadline);
    t.start();

    // Parked before the deadline: the Mutex was released at admission and the
    // timer is live (nothing to observe but the armed record + non-done task).
    SLUICE_CHECK(rec.state.load() == FeRecord::State::armed);
    SLUICE_CHECK(!t.done());
    SLUICE_CHECK(owner == nullptr);
    SLUICE_CHECK(AsyncTestAccess::deferred_depth_for_test(sched) == 0);

    sched.advance_clock(100);  // pump wins Expired -> winner-kind tail -> defer
    SLUICE_CHECK(AsyncTestAccess::deferred_depth_for_test(sched) == 1);
    (void)drain_all(sched, guard_failures);

    // A suspended-Expired resolution carries the SAME reacquire obligation as
    // Woken (released=true): the body reacquires even though it timed out.
    SLUICE_CHECK(resumed.load() == 1);
    SLUICE_CHECK(guard_failures.load() == 0);
    SLUICE_CHECK(t.done());
    SLUICE_CHECK(c.outcome == WaitOutcome::expired);
    SLUICE_CHECK(c.released);
    SLUICE_CHECK(c.owner_null_at_resume);
    SLUICE_CHECK(c.reacquire_done);
    SLUICE_CHECK(AsyncTestAccess::deferred_depth_for_test(sched) == 0);
}

// ---- cancel: exactly-once delivery; terminal re-wait is rejected ----------
SLUICE_TEST_CASE(fe3_condition_deferred_cancel_loser_exactly_once) {
    AsyncIoContext ctx(std::make_unique<FakeAsyncBackend>());
    Scheduler sched(ctx);
    WaitQueue cond_q;
    WaitQueue mutex_q;
    FakeFiberStorage sentinel_storage;
    Fiber* owner = reinterpret_cast<Fiber*>(sentinel_storage.bytes);
    std::atomic<int> resumed{0};
    std::atomic<int> guard_failures{0};

    WaitNode node;
    FeRecord rec;
    CondCase c{sched, cond_q, mutex_q, owner, node, rec, resumed};
    FeTask t = c.run();
    t.start();
    SLUICE_CHECK(rec.state.load() == FeRecord::State::armed);
    SLUICE_CHECK(!t.done());

    // Cancel through the production seam (membership gate + CANCEL CAS +
    // winner-kind publication).
    SLUICE_CHECK(
        AsyncTestAccess::condition_cancel_for_test(sched, cond_q, node));
    SLUICE_CHECK(AsyncTestAccess::deferred_depth_for_test(sched) == 1);
    (void)drain_all(sched, guard_failures);
    SLUICE_CHECK(resumed.load() == 1);
    SLUICE_CHECK(guard_failures.load() == 0);
    SLUICE_CHECK(t.done());
    SLUICE_CHECK(c.outcome == WaitOutcome::cancelled);
    SLUICE_CHECK(c.released);  // released at admission -> reacquire on cancel
    SLUICE_CHECK(c.owner_null_at_resume);
    SLUICE_CHECK(c.reacquire_done);

    // Cancel of the now-terminal node is a loser WITHOUT mutation.
    SLUICE_CHECK(
        !AsyncTestAccess::condition_cancel_for_test(sched, cond_q, node));

    // A second wait on the SAME TERMINAL node: registration fails
    // (rejected_retain) — inline, Mutex retained, no publication.
    FeRecord rec2;
    CondCase c2{sched, cond_q, mutex_q, owner, node, rec2, resumed};
    FeTask t2 = c2.run();
    t2.start();
    SLUICE_CHECK(t2.done());
    SLUICE_CHECK(c2.inline_done);
    SLUICE_CHECK(!c2.released);
    SLUICE_CHECK(c2.outcome == WaitOutcome::cancelled);  // the latched outcome
    SLUICE_CHECK(rec2.state.load() == FeRecord::State::unarmed);
    SLUICE_CHECK(!c2.reacquire_done);  // retained Mutex: NO reacquire epoch
    SLUICE_CHECK(AsyncTestAccess::deferred_depth_for_test(sched) == 0);
    SLUICE_CHECK(resumed.load() == 2);
}

// ---- notify_all: both deferred winners drain, each reacquires itself ------
SLUICE_TEST_CASE(fe3_condition_deferred_notify_all_own_reacquire) {
    AsyncIoContext ctx(std::make_unique<FakeAsyncBackend>());
    Scheduler sched(ctx);
    WaitQueue cond_q;
    WaitQueue mutex_q;
    FakeFiberStorage sentinel_storage;
    Fiber* owner = reinterpret_cast<Fiber*>(sentinel_storage.bytes);
    std::atomic<int> resumed{0};
    std::atomic<int> guard_failures{0};

    WaitNode n1, n2;
    FeRecord c1r, c2r;
    CondCase c1{sched, cond_q, mutex_q, owner, n1, c1r, resumed};
    CondCase c2{sched, cond_q, mutex_q, owner, n2, c2r, resumed};
    FeTask t1 = c1.run();
    FeTask t2 = c2.run();
    t1.start();
    t2.start();
    SLUICE_CHECK(c1r.state.load() == FeRecord::State::armed);
    SLUICE_CHECK(c2r.state.load() == FeRecord::State::armed);
    SLUICE_CHECK(!t1.done() && !t2.done());
    SLUICE_CHECK(owner == nullptr);  // the FIRST admission released the Mutex

    // Atomic snapshot-and-drain: both parked deferred waiters resolve Woken
    // under ONE global_mtx_ CS; each commits its own delivery obligation.
    SLUICE_CHECK(AsyncTestAccess::condition_notify_all_for_test(sched,
                                                                cond_q) == 2);
    SLUICE_CHECK(AsyncTestAccess::deferred_depth_for_test(sched) == 2);
    (void)drain_all(sched, guard_failures);

    SLUICE_CHECK(resumed.load() == 2);
    SLUICE_CHECK(guard_failures.load() == 0);
    SLUICE_CHECK(t1.done() && t2.done());
    SLUICE_CHECK(c1.outcome == WaitOutcome::woken);
    SLUICE_CHECK(c2.outcome == WaitOutcome::woken);
    SLUICE_CHECK(c1.reacquire_done && c2.reacquire_done);
    SLUICE_CHECK(c1.owner_null_at_resume && c2.owner_null_at_resume);
    SLUICE_CHECK(AsyncTestAccess::deferred_depth_for_test(sched) == 0);
}

SLUICE_MAIN()
