// FE-3 cross-frontend mixing — representative Fiber↔deferred pairs sharing
// ONE primitive instance and ONE resolution event. NOT the full cartesian
// product: the Queue slice already contains the two Queue mixing cases
// (fe3_q_cross_fiber_waiter_coroutine_resolver /
// fe3_q_cross_coroutine_waiter_fiber_resolver), and every slice already
// proves the fiber path and the deferred path separately. What the slices do
// NOT yet prove is a MIXED waiter set on ONE primitive resolved by ONE
// resolver: the winner-kind publication tail must deliver EACH waiter
// exactly once through its OWN ResumeTarget mechanism (fiber →
// make_runnable + worker routing; deferred → transit obligation + L9
// discharge), with no cross-talk and no kind-specific ordering dependence.
//
// Cases:
//   1. Event: fiber waiter + deferred waiter parked; ONE set() broadcast
//      resolves both (wake_wait_one_locked drain routes the fiber, defers the
//      coroutine; both delivered exactly once).
//   2. AsyncRwLock: fiber writer holder; parked FIBER reader + parked
//      deferred reader; ONE unlock_write head-reconcile batch-grants BOTH
//      readers (mixed kinds inside one reader-prefix batch).
#include <sluice/async/async_io_context.hpp>
#include <sluice/async/async_rwlock.hpp>
#include <sluice/async/event.hpp>
#include <sluice/async/fake_backend.hpp>
#include <sluice/async/fiber.hpp>
#include <sluice/async/fiber_ctx.hpp>
#include <sluice/async/scheduler.hpp>
#include <sluice/async/wait_node.hpp>

#include "harness.hpp"
#include "scheduler_internal.hpp"  // RwWaitCtx (shared, non-installed)

#include <coroutine>
#include <atomic>
#include <memory>
#include <thread>
#include <utility>
#include <vector>

using namespace sluice::async;
using AsyncTestAccess = Scheduler::AsyncTestAccess;
using FeRecord = AsyncTestAccess::FeDeferredRecord;

namespace {

constexpr unsigned kBoundedWaitIters = 200000;
bool bounded_wait(std::atomic<bool>& flag) {
    for (unsigned i = 0; i < kBoundedWaitIters; ++i) {
        if (flag.load(std::memory_order::acquire)) return true;
        std::this_thread::yield();
    }
    return flag.load(std::memory_order::acquire);
}

// Predicate overload (liveness only — never an ordering proof).
template <typename Pred>
bool bounded_wait(Pred&& pred) {
    for (unsigned i = 0; i < kBoundedWaitIters; ++i) {
        if (pred()) return true;
        std::this_thread::yield();
    }
    return pred();
}

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

struct FiberStack {
    static constexpr std::size_t kBytes = 64 * 1024;
    alignas(16) std::vector<std::byte> bytes{kBytes};
    std::byte* base() noexcept { return bytes.data(); }
    std::size_t size() const noexcept { return bytes.size(); }
};

}  // namespace

// ---- Event: ONE set() resolves a parked fiber AND a parked coroutine ------
SLUICE_TEST_CASE(fe3_mix_event_set_resolves_fiber_and_deferred) {
    if (!fiber_ctx::supported) return;
    AsyncIoContext ctx(std::make_unique<FakeAsyncBackend>());
    Scheduler sched(ctx);
    Event ev(sched, /*initially_set=*/false);
    std::atomic<bool> fiber_parked{false};
    std::atomic<bool> fiber_done{false};
    std::atomic<int> resumed{0};
    std::atomic<int> guard_failures{0};

    // Stackful waiter: parks on the Event.
    WaitNode nf;
    Fiber fwait;
    fwait.set_entry([&](Fiber&) {
        fiber_parked.store(true, std::memory_order::release);
        ev.wait(nf);
        fiber_done.store(true, std::memory_order::release);
    });
    FiberStack sf;
    SLUICE_CHECK(sched.init_fiber(fwait, sf.base(), sf.size()));
    sched.spawn(fwait);
    std::thread runner([&] { sched.run_live(1); });
    SLUICE_CHECK(bounded_wait(fiber_parked));

    // Stackless waiter: parks through the FE-2 shared ladder (proper awaiter:
    // await_suspend parks the node AND suspends the coroutine).
    WaitNode nd;
    FeRecord rec;
    struct FeEventWaiter {
        Scheduler& sched;
        Event& ev;
        WaitNode& node;
        FeRecord& rec;
        bool did_suspend = false;
        bool await_ready() const noexcept { return false; }
        bool await_suspend(std::coroutine_handle<> h) {
            rec.handle_address = h.address();
            did_suspend = AsyncTestAccess::event_wait_deferred_for_test(
                sched, ev, node, rec);
            return did_suspend;
        }
        bool await_resume() noexcept {
            node.set_user(nullptr);  // set/clear symmetry (see rwlock slice)
            return did_suspend;
        }
    };
    struct Case {
        Scheduler& sched;
        Event& ev;
        WaitNode& node;
        FeRecord& rec;
        std::atomic<int>& resumed;
        bool suspended = false;
        FeTask run() {
            suspended = co_await FeEventWaiter{sched, ev, node, rec};
            resumed.fetch_add(1, std::memory_order_relaxed);
            co_return;
        }
    };
    Case c{sched, ev, nd, rec, resumed};
    FeTask t = c.run();
    t.start();
    SLUICE_CHECK(rec.state.load() == FeRecord::State::armed);  // parked
    SLUICE_CHECK(!t.done());
    // Liveness only: both waiters are Registered (waiting_count() sums the
    // queue registrations AND the fiber's Waiting state, so the settled value
    // is >= 2; the exact total is not a stable cross-domain contract). Every
    // assertion below holds under either pickup interleaving.
    SLUICE_CHECK(bounded_wait([&] {
        return sched.waiting_count() >= 2;
    }));

    // ONE resolver: the broadcast drain publishes EACH winner through its OWN
    // ResumeTarget kind — the fiber is routed to its worker, the coroutine
    // commits a transit obligation. Exactly one delivery per waiter.
    ev.set();
    SLUICE_CHECK(AsyncTestAccess::deferred_depth_for_test(sched) == 1);
    (void)drain_all(sched, guard_failures);

    SLUICE_CHECK(bounded_wait(fiber_done));
    runner.join();
    SLUICE_CHECK(resumed.load() == 1);
    SLUICE_CHECK(guard_failures.load() == 0);
    SLUICE_CHECK(t.done());
    SLUICE_CHECK(c.suspended);
    SLUICE_CHECK(nf.was_woken());
    SLUICE_CHECK(nd.was_woken());
    SLUICE_CHECK(AsyncTestAccess::deferred_depth_for_test(sched) == 0);
}

// ---- RwLock: ONE unlock_write batch-grants a fiber AND a coroutine reader -
SLUICE_TEST_CASE(fe3_mix_rwlock_batch_grants_fiber_and_deferred_readers) {
    if (!fiber_ctx::supported) return;
    AsyncIoContext ctx(std::make_unique<FakeAsyncBackend>());
    Scheduler sched(ctx);
    AsyncRwLock lock(sched);
    std::atomic<bool> fiber_holding{false};
    std::atomic<bool> go_unlock{false};
    std::atomic<bool> fiber_reader_done{false};
    std::atomic<int> resumed{0};
    std::atomic<int> guard_failures{0};

    // Writer holder (fiber frontend): claims inline, parks until stepped.
    Fiber holder;
    holder.set_entry([&](Fiber&) {
        WaitNode nh;
        lock.write_lock(nh);
        fiber_holding.store(true, std::memory_order::release);
        while (!go_unlock.load(std::memory_order::acquire)) {
            std::this_thread::yield();
        }
        lock.unlock_write();  // batch-grants BOTH parked readers
        fiber_reader_done.store(true, std::memory_order::release);
    });
    FiberStack sh;
    SLUICE_CHECK(sched.init_fiber(holder, sh.base(), sh.size()));
    sched.spawn(holder);
    // TWO workers: the holder OCCUPIES one while spinning on go_unlock; the
    // fiber reader below needs the other worker to reach read_lock.
    std::thread runner([&] { sched.run_live(2); });
    SLUICE_CHECK(bounded_wait(fiber_holding));

    // Two readers park behind the writer: one DEFERRED (main-thread
    // coroutine), one FIBER (worker-run). Registration order is deterministic
    // (deferred first), but the batch grant delivers BOTH either way.
    WaitNode nd;
    FeRecord rec;
    RwWaitCtx rd_ctx{};
    struct FeRwReadWaiter {
        Scheduler& sched;
        AsyncRwLock& lock;
        void* actor;
        RwWaitCtx& ctx;
        WaitNode& node;
        FeRecord& rec;
        bool did_suspend = false;
        bool await_ready() const noexcept { return false; }
        bool await_suspend(std::coroutine_handle<> h) {
            rec.handle_address = h.address();
            did_suspend = AsyncTestAccess::rwlock_read_deferred_for_test(
                sched, lock, node, actor, ctx, rec);
            return did_suspend;
        }
        bool await_resume() noexcept {
            node.set_user(nullptr);  // set/clear symmetry (see rwlock slice)
            return did_suspend;
        }
    };
    struct ReadCase {
        Scheduler& sched;
        AsyncRwLock& lock;
        RwWaitCtx& ctx;
        WaitNode& node;
        FeRecord& rec;
        std::atomic<int>& resumed;
        bool suspended = false;
        FeTask run() {
            // `this` is the coroutine-frame address: a stable ACTOR token for
            // the epoch (the v1 read model ignores it; the seam binds it).
            suspended = co_await FeRwReadWaiter{sched, lock, this, ctx, node,
                                                rec};
            resumed.fetch_add(1, std::memory_order_relaxed);
            co_return;
        }
    };
    ReadCase cd{sched, lock, rd_ctx, nd, rec, resumed};
    FeTask td = cd.run();
    td.start();
    SLUICE_CHECK(rec.state.load() == FeRecord::State::armed);  // parked

    WaitNode nf;
    Fiber freader;
    std::atomic<bool> freader_parked{false};
    freader.set_entry([&](Fiber&) {
        freader_parked.store(true, std::memory_order::release);
        lock.read_lock(nf);  // parks behind the writer (writer-fair queue)
        fiber_reader_done.store(true, std::memory_order::release);
    });
    FiberStack sfr;
    SLUICE_CHECK(sched.init_fiber(freader, sfr.base(), sfr.size()));
    sched.spawn(freader);
    SLUICE_CHECK(bounded_wait(freader_parked));
    // Both readers Registered (deferred + fiber) behind the writer.
    SLUICE_CHECK(bounded_wait([&] {
        return sched.waiting_count() >= 2;
    }));

    go_unlock.store(true, std::memory_order::release);
    SLUICE_CHECK(bounded_wait(fiber_reader_done));  // holder released
    runner.join();

    // The ONE head-reconcile batch published BOTH readers: the deferred
    // reader through its transit obligation, the fiber reader through its
    // worker route. Both hold a read share; the writer is gone.
    SLUICE_CHECK(AsyncTestAccess::deferred_depth_for_test(sched) == 1);
    (void)drain_all(sched, guard_failures);
    SLUICE_CHECK(resumed.load() == 1);
    SLUICE_CHECK(guard_failures.load() == 0);
    SLUICE_CHECK(td.done());
    SLUICE_CHECK(cd.suspended);
    SLUICE_CHECK(nd.was_woken());
    SLUICE_CHECK(nf.was_woken());
    SLUICE_CHECK(!AsyncTestAccess::rwlock_writer_active_for_test(lock));
    // Both readers hold: release both read shares, then the deferred reader
    // releases too — the lock returns to the fully-drained state.
    AsyncTestAccess::rwlock_unlock_read_for_test(sched, lock);  // fiber's
    AsyncTestAccess::rwlock_unlock_read_for_test(sched, lock);  // deferred's
    SLUICE_CHECK(AsyncTestAccess::deferred_depth_for_test(sched) == 0);
}

SLUICE_MAIN()
