// fe3_stackless_queue_slice_test — FE campaign stage FE-3, Queue vertical
// slice. A second (stackless C++20 coroutine) frontend drives the Queue
// through the SAME semantic authorities as the stackful Fiber frontend:
//
//   - the shared push/pop admission ladders
//     (queue_push_admit_locked / queue_pop_admit_locked — ONE textual
//     admission law per direction, blocking and timed);
//   - the WaitNode resolve_ terminal winner;
//   - the Q-LIV-1 opposite-role reconcile grants
//     (queue_grant_consumer_locked / queue_grant_producer_locked) with the
//     winner-kind publication tail (queue_publish_winner_locked);
//   - close disposition, timer retirement (ordinary deadline authority), and
//     queue_cancel — each publishing through the same winner-kind tail.
//
// What is test-only (AGENTS.md §3.9 / FE-1c scope): the coroutine task, the
// awaiters, and the FeDeferredRecord delivery record. The QueueWaitCtx and
// the item lease live in the COROUTINE FRAME (FE-1a lifetime rule: the grant
// winner writes through ctx->prod_lease / ctx->cons_out after suspension, so
// the addresses must be stable across the suspension — the property this
// slice proves by construction).
//
// Determinism: every deferred-publication ordering claim below is made
// WITHOUT sleeps — the coroutine parks first (on the test thread), the
// resolver action then runs on the same thread or on a spawned worker whose
// input dependency (the parked coroutine) is already true. The two
// cross-frontend worker cases wait on a bounded flag spin only as liveness
// tolerance for the worker pickup; their assertions hold under either
// interleaving of that pickup (admission inline vs. reconciler grant).
#include "harness.hpp"
#include "queue_detail.hpp"  // QueueWaitCtx (non-installed src/ header)

#include "async_test_control.hpp"

#include <sluice/async/async_io_context.hpp>
#include <sluice/async/detail/queue_item.hpp>
#include <sluice/async/detail/queue_port.hpp>
#include <sluice/async/fake_backend.hpp>
#include <sluice/async/scheduler.hpp>
#include <sluice/async/wait_node.hpp>

#include <coroutine>
#include <atomic>
#include <memory>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

using namespace sluice::async;
using namespace sluice::async::detail;
using AsyncTestAccess = Scheduler::AsyncTestAccess;
using FeRecord = AsyncTestAccess::FeDeferredRecord;

namespace {

// Bounded liveness wait for a worker-thread observable (same shape as the
// queue primitive tests). Never used as ordering proof.
constexpr unsigned kBoundedWaitIters = 200000;
bool bounded_wait(std::atomic<bool>& flag) {
    for (unsigned i = 0; i < kBoundedWaitIters; ++i) {
        if (flag.load(std::memory_order::acquire)) return true;
        std::this_thread::yield();
    }
    return flag.load(std::memory_order::acquire);
}

// Minimal test-local void coroutine task (FE-2 PoV shape): frame-embedded
// awaiter storage, no allocation customization, no exception propagation.
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

// Push statuses observable after resume (QueuePort::push/push_until mapping).
enum FePushStatus : int { kPushCommitted = 0, kPushClosed, kPushExpired,
                          kPushCancelled };
// Pop statuses observable after resume (QueuePort::pop/pop_until mapping).
enum FePopStatus : int { kPopItem = 0, kPopClosed, kPopExpired };

// Stackless awaiter: blocking/timed Queue push through the shared production
// ladder via the internal-testing deferred entry. The lease, the WaitNode,
// the QueueWaitCtx, and the delivery record are ALL members of this awaiter —
// the compiler places the awaiter in the coroutine frame, so every address
// the port/grant seams capture survives the suspension (the FE-1a property
// this slice proves).
struct FeQueuePushAwaiter {
    Scheduler& sched;
    detail::QueuePort& port;
    detail::QueueItemLease lease;
    WaitNode node{};
    QueueWaitCtx ctx{};
    FeRecord rec{};
    bool timed = false;
    Scheduler::deadline_t deadline = 0;
    // Filled at await_resume (QueuePort's post-admit mapping verbatim).
    FePushStatus status = kPushCommitted;
    int recovered = -1;  // exact original T on failure statuses

    FeQueuePushAwaiter(Scheduler& s, detail::QueuePort& p, int v)
        : sched(s), port(p), lease(QueueItemFactory::make<int>(p, v)) {}

    bool await_ready() const noexcept { return false; }
    bool await_suspend(std::coroutine_handle<> h) {
        // FE-1b L2: bind the resume target BEFORE the epoch becomes
        // resolver-observable.
        rec.handle_address = h.address();
        if (timed) {
            return AsyncTestAccess::queue_push_deferred_until_for_test(
                sched, port, lease, node, ctx, rec, deadline);
        }
        return AsyncTestAccess::queue_push_deferred_for_test(
            sched, port, lease, node, ctx, rec);
    }
    void await_resume() {
        if (node.was_expired()) {
            status = kPushExpired;
        } else if (node.was_cancelled()) {
            status = kPushCancelled;
        } else if (AsyncTestAccess::queue_lease_empty_for_test(lease)) {
            status = kPushCommitted;  // ring owns the control now
        } else {
            status = kPushClosed;  // Woken with the lease retained
        }
        if (status != kPushCommitted) {
            // Discharge the retained custody exactly as QueuePort::push does
            // (validates owner port + location, moves T once, deletes the
            // node outside all locks).
            recovered =
                QueueItemFactory::release_failed<int>(port, std::move(lease));
        }
        // FE-CORRECTIVE-1 P1-2: the QueuePort lifetime pin acquired at
        // deferred entry is released ONLY NOW — after the port-dependent
        // conversion above — mirroring the fiber frontend's CallGuard, whose
        // stack frame spans suspension through result conversion.
        AsyncTestAccess::queue_release_deferred_pin_for_test(port);
    }
};

// Stackless awaiter: blocking/timed Queue pop. Symmetric. The empty
// out-lease is minted through the seam (QueueItemLease's default ctor is
// authority-private; QueuePort::pop constructs it inside the friend class for
// the fiber frontend).
struct FeQueuePopAwaiter {
    Scheduler& sched;
    detail::QueuePort& port;
    detail::QueueItemLease out;
    WaitNode node{};
    QueueWaitCtx ctx{};
    FeRecord rec{};
    bool timed = false;
    Scheduler::deadline_t deadline = 0;
    FePopStatus status = kPopClosed;
    int recovered = -1;  // the granted item on kPopItem

    FeQueuePopAwaiter(Scheduler& s, detail::QueuePort& p)
        : sched(s), port(p),
          out(AsyncTestAccess::queue_make_empty_lease_for_test()) {}

    bool await_ready() const noexcept { return false; }
    bool await_suspend(std::coroutine_handle<> h) {
        rec.handle_address = h.address();
        if (timed) {
            return AsyncTestAccess::queue_pop_deferred_until_for_test(
                sched, port, out, node, ctx, rec, deadline);
        }
        return AsyncTestAccess::queue_pop_deferred_for_test(
            sched, port, out, node, ctx, rec);
    }
    void await_resume() {
        if (!AsyncTestAccess::queue_lease_empty_for_test(out)) {
            status = kPopItem;
            recovered =
                QueueItemFactory::release_popped<int>(port, std::move(out));
        } else if (node.was_expired()) {
            status = kPopExpired;
        } else {
            status = kPopClosed;
        }
        // FE-CORRECTIVE-1 P1-2: release the QueuePort lifetime pin after the
        // port-dependent conversion (see FeQueuePushAwaiter).
        AsyncTestAccess::queue_release_deferred_pin_for_test(port);
    }
};

// Drain helper (FE-2 PoV shape): discharge ALL pending deferred publications.
// Chunked take (under G) + resume with NO lock held (FE-1b L9). The resumed
// bodies count themselves via the caller's atomics.
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
                // Unarmed consume = resume-before-armed (L8 broken);
                // double-consume = double resume. Both are loud failures.
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

// Recover a typed value from an opaque result (the sanctioned conversion
// boundary; free wrappers over the QueueItemFactory statics, as in the queue
// primitive tests).
template <class T>
T release_failed(QueuePort& port, QueueOpaquePushResult&& r) {
    QueueItemLease lease = std::move(r).take_failed_lease();
    return QueueItemFactory::release_failed<T>(port, std::move(lease));
}
template <class T>
T release_popped(QueuePort& port, QueueOpaquePopResult&& r) {
    QueueItemLease lease = std::move(r).take_item_lease();
    return QueueItemFactory::release_popped<T>(port, std::move(lease));
}
template <class T>
T release_teardown(QueuePort& port, QueueItemLease&& lease) {
    return QueueItemFactory::release_teardown<T>(port, std::move(lease));
}

}  // namespace

// ---- Inline push admission: ring has space -> committed, never suspended --
SLUICE_TEST_CASE(fe3_q_push_inline_admissible) {
    AsyncIoContext ctx(std::make_unique<FakeAsyncBackend>());
    Scheduler sched(ctx);
    QueuePort port(sched, 2);
    std::atomic<int> resumed{0};

    struct Case {
        Scheduler& sched;
        QueuePort& port;
        std::atomic<int>& resumed;
        FePushStatus observed = kPushCommitted;
        FeTask run() {
            FeQueuePushAwaiter aw{sched, port, 1};
            co_await aw;
            // The admission resolved inline: await_suspend returned false, the
            // coroutine continued WITHOUT suspending.
            resumed.fetch_add(1, std::memory_order_relaxed);
            observed = aw.status;
            co_return;
        }
    };
    Case c{sched, port, resumed};
    FeTask t = c.run();
    t.start();

    SLUICE_CHECK(resumed.load() == 1);
    SLUICE_CHECK(t.done());
    SLUICE_CHECK(c.observed == kPushCommitted);
    SLUICE_CHECK(port.size() == 1);
    // Inline resolution published nothing (FE-1b L6).
    SLUICE_CHECK(AsyncTestAccess::deferred_depth_for_test(sched) == 0);
    // The Q-LIV-1 grant ran with an empty consumer FIFO (no-op).
    auto rp = port.try_pop();
    SLUICE_CHECK(rp.status() == QueueOpaquePopStatus::item);
    (void)release_popped<int>(port, std::move(rp));
}

// ---- Inline pop admission: item available -> granted inline ---------------
SLUICE_TEST_CASE(fe3_q_pop_inline_admissible) {
    AsyncIoContext ctx(std::make_unique<FakeAsyncBackend>());
    Scheduler sched(ctx);
    QueuePort port(sched, 2);
    std::atomic<int> resumed{0};
    {
        auto lease = QueueItemFactory::make<int>(port, 5);
        (void)port.try_push(std::move(lease));
    }

    struct Case {
        Scheduler& sched;
        QueuePort& port;
        std::atomic<int>& resumed;
        FePopStatus observed = kPopClosed;
        int got = -1;
        FeTask run() {
            FeQueuePopAwaiter aw{sched, port};
            co_await aw;
            resumed.fetch_add(1, std::memory_order_relaxed);
            observed = aw.status;
            got = aw.recovered;
            co_return;
        }
    };
    Case c{sched, port, resumed};
    FeTask t = c.run();
    t.start();

    SLUICE_CHECK(resumed.load() == 1);
    SLUICE_CHECK(t.done());
    SLUICE_CHECK(c.observed == kPopItem);
    SLUICE_CHECK(c.got == 5);
    SLUICE_CHECK(port.size() == 0);
    SLUICE_CHECK(AsyncTestAccess::deferred_depth_for_test(sched) == 0);
}

// ---- Deferred push granted by a consumer's try_pop (Q-LIV-1, deferred) ----
SLUICE_TEST_CASE(fe3_q_push_deferred_granted_by_try_pop) {
    AsyncIoContext ctx(std::make_unique<FakeAsyncBackend>());
    Scheduler sched(ctx);
    QueuePort port(sched, 1);  // capacity 1: the coroutine producer must park
    std::atomic<int> resumed{0};
    std::atomic<int> guard_failures{0};
    {
        auto lease = QueueItemFactory::make<int>(port, 1);
        (void)port.try_push(std::move(lease));
    }

    struct Case {
        Scheduler& sched;
        QueuePort& port;
        std::atomic<int>& resumed;
        FePushStatus observed = kPushCommitted;
        FeTask run() {
            FeQueuePushAwaiter aw{sched, port, 2};
            co_await aw;  // parks: ring full, FIFO head
            resumed.fetch_add(1, std::memory_order_relaxed);
            observed = aw.status;
            co_return;
        }
    };
    Case c{sched, port, resumed};
    FeTask t = c.run();
    t.start();  // suspends inside the co_await (record armed in the CS)

    SLUICE_CHECK(port.size() == 1);  // ring still holds 1
    SLUICE_CHECK(AsyncTestAccess::deferred_depth_for_test(sched) == 0);
    SLUICE_CHECK(resumed.load() == 0);

    // try_pop: FastPopCommit frees the slot AND the Q-LIV-1 grant commits the
    // parked producer's lease (2) into the freed slot, winner-before-
    // publication, then defers the delivery.
    {
        auto rp = port.try_pop();
        SLUICE_CHECK(rp.status() == QueueOpaquePopStatus::item);
        SLUICE_CHECK(release_popped<int>(port, std::move(rp)) == 1);
    }
    SLUICE_CHECK(port.size() == 1);  // item 2 now buffered
    SLUICE_CHECK(AsyncTestAccess::deferred_depth_for_test(sched) == 1);

    const std::size_t drained = drain_all(sched, guard_failures);
    SLUICE_CHECK(drained == 1);
    SLUICE_CHECK(resumed.load() == 1);  // exactly-once
    SLUICE_CHECK(guard_failures.load() == 0);
    SLUICE_CHECK(c.observed == kPushCommitted);
    SLUICE_CHECK(t.done());
    SLUICE_CHECK(AsyncTestAccess::deferred_depth_for_test(sched) == 0);
    // Teardown balance: a deferred winner went through the grant seam and the
    // counters must still be back at zero (begin_teardown preconditions).
    {
        auto session = port.begin_teardown();
        auto l = session.take_next();
        if (static_cast<bool>(l)) {
            SLUICE_CHECK(release_teardown<int>(port, std::move(l)) == 2);
        }
    }
}

// ---- Deferred pop granted by a producer's try_push (fast-path grant) ------
SLUICE_TEST_CASE(fe3_q_pop_deferred_granted_by_try_push) {
    AsyncIoContext ctx(std::make_unique<FakeAsyncBackend>());
    Scheduler sched(ctx);
    QueuePort port(sched, 1);
    std::atomic<int> resumed{0};
    std::atomic<int> guard_failures{0};

    struct Case {
        Scheduler& sched;
        QueuePort& port;
        std::atomic<int>& resumed;
        FePopStatus observed = kPopClosed;
        int got = -1;
        FeTask run() {
            FeQueuePopAwaiter aw{sched, port};
            co_await aw;  // parks: ring empty, open
            resumed.fetch_add(1, std::memory_order_relaxed);
            observed = aw.status;
            got = aw.recovered;
            co_return;
        }
    };
    Case c{sched, port, resumed};
    FeTask t = c.run();
    t.start();

    SLUICE_CHECK(AsyncTestAccess::deferred_depth_for_test(sched) == 0);
    {
        auto lease = QueueItemFactory::make<int>(port, 7);
        (void)port.try_push(std::move(lease));  // FastPushCommit + grant
    }
    SLUICE_CHECK(AsyncTestAccess::deferred_depth_for_test(sched) == 1);
    SLUICE_CHECK(port.size() == 0);  // granted directly, not buffered

    const std::size_t drained = drain_all(sched, guard_failures);
    SLUICE_CHECK(drained == 1);
    SLUICE_CHECK(resumed.load() == 1);
    SLUICE_CHECK(guard_failures.load() == 0);
    SLUICE_CHECK(c.observed == kPopItem);
    SLUICE_CHECK(c.got == 7);
    SLUICE_CHECK(t.done());
    {  // teardown balance on an empty ring
        (void)port.begin_teardown();
    }
}

// ---- Deferred push + close: closed disposition, exact lease retained ------
SLUICE_TEST_CASE(fe3_q_push_deferred_closed) {
    AsyncIoContext ctx(std::make_unique<FakeAsyncBackend>());
    Scheduler sched(ctx);
    QueuePort port(sched, 1);
    std::atomic<int> resumed{0};
    std::atomic<int> guard_failures{0};
    {
        auto lease = QueueItemFactory::make<int>(port, 8);
        (void)port.try_push(std::move(lease));
    }

    struct Case {
        Scheduler& sched;
        QueuePort& port;
        std::atomic<int>& resumed;
        FePushStatus observed = kPushCommitted;
        int got = -1;
        FeTask run() {
            FeQueuePushAwaiter aw{sched, port, 9};
            co_await aw;  // parks: ring full
            resumed.fetch_add(1, std::memory_order_relaxed);
            observed = aw.status;
            got = aw.recovered;
            co_return;
        }
    };
    Case c{sched, port, resumed};
    FeTask t = c.run();
    t.start();

    port.close();  // drains the producer FIFO: closed, lease retained
    SLUICE_CHECK(AsyncTestAccess::deferred_depth_for_test(sched) == 1);
    const std::size_t drained = drain_all(sched, guard_failures);
    SLUICE_CHECK(drained == 1);
    SLUICE_CHECK(resumed.load() == 1);
    SLUICE_CHECK(guard_failures.load() == 0);
    SLUICE_CHECK(c.observed == kPushClosed);
    SLUICE_CHECK(c.got == 9);  // the EXACT original T
    SLUICE_CHECK(t.done());
    // Drain the buffered item: the QueuePort dtor fail-fasts on a non-empty
    // ring (destruction contract).
    {
        auto rp = port.try_pop();
        SLUICE_CHECK(rp.status() == QueueOpaquePopStatus::item);
        SLUICE_CHECK(release_popped<int>(port, std::move(rp)) == 8);
    }
}

// ---- Deferred pop + close: closed+empty disposition ------------------------
SLUICE_TEST_CASE(fe3_q_pop_deferred_closed) {
    AsyncIoContext ctx(std::make_unique<FakeAsyncBackend>());
    Scheduler sched(ctx);
    QueuePort port(sched, 1);
    std::atomic<int> resumed{0};
    std::atomic<int> guard_failures{0};

    struct Case {
        Scheduler& sched;
        QueuePort& port;
        std::atomic<int>& resumed;
        FePopStatus observed = kPopItem;
        FeTask run() {
            FeQueuePopAwaiter aw{sched, port};
            co_await aw;  // parks: ring empty, open
            resumed.fetch_add(1, std::memory_order_relaxed);
            observed = aw.status;
            co_return;
        }
    };
    Case c{sched, port, resumed};
    FeTask t = c.run();
    t.start();

    port.close();  // closed+empty: Woken with `out` left empty
    SLUICE_CHECK(AsyncTestAccess::deferred_depth_for_test(sched) == 1);
    (void)drain_all(sched, guard_failures);
    SLUICE_CHECK(resumed.load() == 1);
    SLUICE_CHECK(guard_failures.load() == 0);
    SLUICE_CHECK(c.observed == kPopClosed);
    SLUICE_CHECK(t.done());
}

// ---- Deferred push + cancel: Cancelled winner through the same seam -------
SLUICE_TEST_CASE(fe3_q_push_deferred_cancel) {
    AsyncIoContext ctx(std::make_unique<FakeAsyncBackend>());
    Scheduler sched(ctx);
    QueuePort port(sched, 1);
    std::atomic<int> resumed{0};
    std::atomic<int> guard_failures{0};
    {
        auto lease = QueueItemFactory::make<int>(port, 3);
        (void)port.try_push(std::move(lease));
    }

    struct Case {
        Scheduler& sched;
        QueuePort& port;
        std::atomic<int>& resumed;
        WaitNode* parked_node = nullptr;  // set inside the frame at await time
        FePushStatus observed = kPushCommitted;
        int got = -1;
        FeTask run() {
            FeQueuePushAwaiter aw{sched, port, 4};
            parked_node = &aw.node;
            co_await aw;  // parks: ring full
            resumed.fetch_add(1, std::memory_order_relaxed);
            observed = aw.status;
            got = aw.recovered;
            co_return;
        }
    };
    Case c{sched, port, resumed};
    FeTask t = c.run();
    t.start();

    SLUICE_CHECK(c.parked_node != nullptr);
    // Same production cancellation closure the fiber frontend uses.
    SLUICE_CHECK(AsyncTestAccess::queue_cancel_deferred_for_test(
        sched, port, QueueRole::producer, *c.parked_node));
    SLUICE_CHECK(AsyncTestAccess::deferred_depth_for_test(sched) == 1);
    (void)drain_all(sched, guard_failures);
    SLUICE_CHECK(resumed.load() == 1);
    SLUICE_CHECK(guard_failures.load() == 0);
    SLUICE_CHECK(c.observed == kPushCancelled);
    SLUICE_CHECK(c.got == 4);  // custody retained through cancellation
    SLUICE_CHECK(t.done());
    // Cancel of the now-terminal node is a loser: false, no publication.
    SLUICE_CHECK(!AsyncTestAccess::queue_cancel_deferred_for_test(
        sched, port, QueueRole::producer, *c.parked_node));
    SLUICE_CHECK(AsyncTestAccess::deferred_depth_for_test(sched) == 0);
    {
        auto rp = port.try_pop();
        SLUICE_CHECK(rp.status() == QueueOpaquePopStatus::item);
        SLUICE_CHECK(release_popped<int>(port, std::move(rp)) == 3);
    }
}

// ---- Deferred timed push: pump expiry retires the timer + defers ----------
SLUICE_TEST_CASE(fe3_q_push_deferred_expired) {
    AsyncIoContext ctx(std::make_unique<FakeAsyncBackend>());
    Scheduler sched(ctx);
    QueuePort port(sched, 1);
    std::atomic<int> resumed{0};
    std::atomic<int> guard_failures{0};
    {
        auto lease = QueueItemFactory::make<int>(port, 30);
        (void)port.try_push(std::move(lease));
    }
    sluice_async_test::TimerTestControl::enable_test_clock(sched);
    const Scheduler::deadline_t deadline =
        AsyncTestAccess::clock_now(sched) + 50;

    struct Case {
        Scheduler& sched;
        QueuePort& port;
        std::atomic<int>& resumed;
        Scheduler::deadline_t deadline;
        FePushStatus observed = kPushCommitted;
        int got = -1;
        FeTask run() {
            FeQueuePushAwaiter aw{sched, port, 31};
            aw.timed = true;
            aw.deadline = deadline;
            co_await aw;  // parks: ring full, deadline in the future
            resumed.fetch_add(1, std::memory_order_relaxed);
            observed = aw.status;
            got = aw.recovered;
            co_return;
        }
    };
    Case c{sched, port, resumed, deadline};
    FeTask t = c.run();
    t.start();

    SLUICE_CHECK(AsyncTestAccess::deferred_depth_for_test(sched) == 0);
    sched.advance_clock(100);  // 100 >= 50: pump wins Expired
    SLUICE_CHECK(AsyncTestAccess::deferred_depth_for_test(sched) == 1);
    (void)drain_all(sched, guard_failures);
    SLUICE_CHECK(resumed.load() == 1);
    SLUICE_CHECK(guard_failures.load() == 0);
    SLUICE_CHECK(c.observed == kPushExpired);
    SLUICE_CHECK(c.got == 31);
    SLUICE_CHECK(t.done());
    // Drain the buffered item (destruction contract), then the timer
    // retirement balance: teardown succeeds (active_queue_timers_ == 0).
    {
        auto rp = port.try_pop();
        SLUICE_CHECK(rp.status() == QueueOpaquePopStatus::item);
        SLUICE_CHECK(release_popped<int>(port, std::move(rp)) == 30);
    }
    { (void)port.begin_teardown(); }
}

// ---- Deferred timed pop: pump expiry --------------------------------------
SLUICE_TEST_CASE(fe3_q_pop_deferred_expired) {
    AsyncIoContext ctx(std::make_unique<FakeAsyncBackend>());
    Scheduler sched(ctx);
    QueuePort port(sched, 1);
    std::atomic<int> resumed{0};
    std::atomic<int> guard_failures{0};
    sluice_async_test::TimerTestControl::enable_test_clock(sched);
    const Scheduler::deadline_t deadline =
        AsyncTestAccess::clock_now(sched) + 50;

    struct Case {
        Scheduler& sched;
        QueuePort& port;
        std::atomic<int>& resumed;
        Scheduler::deadline_t deadline;
        FePopStatus observed = kPopItem;
        FeTask run() {
            FeQueuePopAwaiter aw{sched, port};
            aw.timed = true;
            aw.deadline = deadline;
            co_await aw;
            resumed.fetch_add(1, std::memory_order_relaxed);
            observed = aw.status;
            co_return;
        }
    };
    Case c{sched, port, resumed, deadline};
    FeTask t = c.run();
    t.start();

    sched.advance_clock(100);
    SLUICE_CHECK(AsyncTestAccess::deferred_depth_for_test(sched) == 1);
    (void)drain_all(sched, guard_failures);
    SLUICE_CHECK(resumed.load() == 1);
    SLUICE_CHECK(guard_failures.load() == 0);
    SLUICE_CHECK(c.observed == kPopExpired);
    SLUICE_CHECK(t.done());
}

// ---- Close drains MULTIPLE parked deferred producers in FIFO order --------
SLUICE_TEST_CASE(fe3_q_close_drains_deferred_producers_fifo) {
    AsyncIoContext ctx(std::make_unique<FakeAsyncBackend>());
    Scheduler sched(ctx);
    QueuePort port(sched, 1);  // one buffered slot; second producer parks
    std::atomic<int> resumed{0};
    std::atomic<int> guard_failures{0};
    std::mutex order_mtx;
    std::vector<int> close_order;  // completion order of the two producers
    {
        auto lease = QueueItemFactory::make<int>(port, 100);
        (void)port.try_push(std::move(lease));
    }

    struct Case {
        Scheduler& sched;
        QueuePort& port;
        std::atomic<int>& resumed;
        std::mutex& order_mtx;
        std::vector<int>& close_order;
        int v;
        FePushStatus observed = kPushCommitted;
        int got = -1;
        FeTask run() {
            FeQueuePushAwaiter aw{sched, port, v};
            co_await aw;  // first parks on the full ring; second behind it
            resumed.fetch_add(1, std::memory_order_relaxed);
            observed = aw.status;
            got = aw.recovered;
            { std::lock_guard<std::mutex> lk(order_mtx); close_order.push_back(v); }
            co_return;
        }
    };
    Case c1{sched, port, resumed, order_mtx, close_order, 101};
    Case c2{sched, port, resumed, order_mtx, close_order, 102};
    FeTask t1 = c1.run();
    FeTask t2 = c2.run();
    t1.start();  // parks (ring full, FIFO head)
    t2.start();  // parks behind t1 (FIFO tail)

    SLUICE_CHECK(AsyncTestAccess::deferred_depth_for_test(sched) == 0);
    port.close();  // drains BOTH producers: closed + retained leases
    SLUICE_CHECK(AsyncTestAccess::deferred_depth_for_test(sched) == 2);
    (void)drain_all(sched, guard_failures);
    SLUICE_CHECK(resumed.load() == 2);
    SLUICE_CHECK(guard_failures.load() == 0);
    SLUICE_CHECK(c1.observed == kPushClosed && c1.got == 101);
    SLUICE_CHECK(c2.observed == kPushClosed && c2.got == 102);
    // Role FIFO preserved: 101 resolved (and delivered) before 102.
    SLUICE_CHECK(close_order.size() == 2);
    SLUICE_CHECK(close_order[0] == 101 && close_order[1] == 102);
    SLUICE_CHECK(t1.done() && t2.done());
    // Drain the buffered pre-fill (destruction contract); two deferred
    // winners went through the grant seams, so this also proves the teardown
    // counter balance.
    {
        auto rp = port.try_pop();
        SLUICE_CHECK(rp.status() == QueueOpaquePopStatus::item);
        SLUICE_CHECK(release_popped<int>(port, std::move(rp)) == 100);
    }
    { (void)port.begin_teardown(); }
}

// ---- Fiber BLOCKING pop entry as resolver: the ladder's Q-LIV-1 grant
// obligation (resolved_inline_grant) delivers the parked deferred head -----
SLUICE_TEST_CASE(fe3_q_fiber_blocking_pop_grants_deferred_producer) {
    if (!fiber_ctx::supported) return;
    AsyncIoContext ctx(std::make_unique<FakeAsyncBackend>());
    Scheduler sched(ctx);
    QueuePort port(sched, 1);  // capacity 1: the deferred producer parks
    std::atomic<int> resumed{0};
    std::atomic<int> guard_failures{0};
    {
        auto lease = QueueItemFactory::make<int>(port, 31);
        (void)port.try_push(std::move(lease));
    }

    struct Case {
        Scheduler& sched;
        QueuePort& port;
        std::atomic<int>& resumed;
        FePushStatus observed = kPushCommitted;
        FeTask run() {
            FeQueuePushAwaiter aw{sched, port, 32};
            co_await aw;  // parks: ring full
            resumed.fetch_add(1, std::memory_order_relaxed);
            observed = aw.status;
            co_return;
        }
    };
    Case c{sched, port, resumed};
    FeTask t = c.run();
    t.start();  // parked (armed); nothing in flight yet
    SLUICE_CHECK(!t.done());  // parked (the eligibility record is frame-arm)
    SLUICE_CHECK(AsyncTestAccess::deferred_depth_for_test(sched) == 0);

    // The resolver is the FIBER BLOCKING pop ADMISSION ENTRY (port.pop, not
    // the try_pop reconcile): its inline consume MUST return
    // resolved_inline_grant, and the ENTRY must run the grant after its
    // consumer role mutex release. Skipping the obligation strands the
    // parked deferred producer (caught by the resumed check below).
    Fiber resolver;
    std::atomic<bool> resolver_done{false};
    int got = -1;
    resolver.set_entry([&](Fiber&) {
        auto rp = port.pop();
        if (rp.status() == QueueOpaquePopStatus::item) {
            got = release_popped<int>(port, std::move(rp));
        }
        resolver_done.store(true, std::memory_order::release);
    });
    FiberStack sr;
    SLUICE_CHECK(sched.init_fiber(resolver, sr.base(), sr.size()));
    sched.spawn(resolver);
    std::thread runner([&] { sched.run_live(1); });
    SLUICE_CHECK(bounded_wait(resolver_done));
    runner.join();

    // The ladder grant committed the deferred producer's delivery obligation.
    SLUICE_CHECK(AsyncTestAccess::deferred_depth_for_test(sched) == 1);
    (void)drain_all(sched, guard_failures);
    SLUICE_CHECK(resumed.load() == 1);
    SLUICE_CHECK(guard_failures.load() == 0);
    SLUICE_CHECK(c.observed == kPushCommitted);  // lease 32 committed
    SLUICE_CHECK(t.done());
    SLUICE_CHECK(got == 31);
    SLUICE_CHECK(port.size() == 1);
    {  // drain the ring; teardown balance across both frontends
        auto session = port.begin_teardown();
        auto l = session.take_next();
        if (static_cast<bool>(l)) {
            SLUICE_CHECK(release_teardown<int>(port, std::move(l)) == 32);
        }
    }
}

// ---- Cross-frontend: coroutine resolver publishes a parked FIBER waiter ---
SLUICE_TEST_CASE(fe3_q_cross_fiber_waiter_coroutine_resolver) {
    if (!fiber_ctx::supported) return;
    AsyncIoContext ctx(std::make_unique<FakeAsyncBackend>());
    Scheduler sched(ctx);
    QueuePort port(sched, 4);
    std::atomic<bool> consumer_parked{false};
    std::atomic<bool> consumer_done{false};
    int consumed = -1;

    Fiber consumer;
    consumer.set_entry([&](Fiber&) {
        consumer_parked.store(true, std::memory_order::release);
        auto r = port.pop();
        if (r.status() == QueueOpaquePopStatus::item) {
            consumed = release_popped<int>(port, std::move(r));
        }
        consumer_done.store(true, std::memory_order::release);
    });
    FiberStack sc;
    SLUICE_CHECK(sched.init_fiber(consumer, sc.base(), sc.size()));
    sched.spawn(consumer);
    std::thread runner([&] { sched.run_live(1); });
    // Liveness wait only: the assertions below hold under either pickup
    // interleaving (reconciler grant vs. inline pop admission).
    (void)bounded_wait(consumer_parked);
    std::this_thread::yield();

    // The COROUTINE (main thread) resolves the fiber waiter: the grant seam's
    // fiber branch routes through granted_not_resumed_ + owner routing.
    {
        auto lease = QueueItemFactory::make<int>(port, 11);
        (void)port.try_push(std::move(lease));
    }
    SLUICE_CHECK(bounded_wait(consumer_done));
    runner.join();
    SLUICE_CHECK(consumed == 11);  // exactly-once, no lost item
    SLUICE_CHECK(port.size() == 0);
    { (void)port.begin_teardown(); }
}

// ---- Cross-frontend: parked FIBER producer reconciled, deferred coroutine
// winner granted by that same free-slot reconcile -------------------------
SLUICE_TEST_CASE(fe3_q_cross_coroutine_waiter_fiber_resolver) {
    if (!fiber_ctx::supported) return;
    AsyncIoContext ctx(std::make_unique<FakeAsyncBackend>());
    Scheduler sched(ctx);
    QueuePort port(sched, 1);  // capacity 1: a parked producer must wait
    std::atomic<int> resumed{0};
    std::atomic<int> guard_failures{0};
    std::atomic<bool> resolver_done{false};
    {
        auto lease = QueueItemFactory::make<int>(port, 21);
        (void)port.try_push(std::move(lease));
    }

    struct Case {
        Scheduler& sched;
        QueuePort& port;
        std::atomic<int>& resumed;
        FePushStatus observed = kPushCommitted;
        FeTask run() {
            FeQueuePushAwaiter aw{sched, port, 22};
            co_await aw;  // parks: ring full (coroutine waits on the worker)
            resumed.fetch_add(1, std::memory_order_relaxed);
            observed = aw.status;
            co_return;
        }
    };
    Case c{sched, port, resumed};
    FeTask t = c.run();
    t.start();  // coroutine parked BEFORE the worker exists: deterministic

    Fiber resolver;
    resolver.set_entry([&](Fiber&) {
        auto rp = port.try_pop();  // frees the slot + grants the deferred head
        if (rp.status() == QueueOpaquePopStatus::item) {
            (void)release_popped<int>(port, std::move(rp));  // recover 21
        }
        resolver_done.store(true, std::memory_order::release);
    });
    FiberStack sr;
    SLUICE_CHECK(sched.init_fiber(resolver, sr.base(), sr.size()));
    sched.spawn(resolver);
    std::thread runner([&] { sched.run_live(1); });
    SLUICE_CHECK(bounded_wait(resolver_done));
    runner.join();

    SLUICE_CHECK(AsyncTestAccess::deferred_depth_for_test(sched) == 1);
    (void)drain_all(sched, guard_failures);
    SLUICE_CHECK(resumed.load() == 1);
    SLUICE_CHECK(guard_failures.load() == 0);
    SLUICE_CHECK(c.observed == kPushCommitted);  // lease 22 committed to ring
    SLUICE_CHECK(t.done());
    SLUICE_CHECK(port.size() == 1);
    {  // drain the ring; teardown balance across both frontends
        auto session = port.begin_teardown();
        auto l = session.take_next();
        if (static_cast<bool>(l)) {
            SLUICE_CHECK(release_teardown<int>(port, std::move(l)) == 22);
        }
    }
}
// ---- FE-CORRECTIVE-1 P1-2: QPIN phase witnesses ----------------------------
//
// The deferred Queue ordinary operation holds the QueuePort ordinary-call
// pin (active_port_calls_) from entry acceptance THROUGH suspension, terminal
// resolution, deferred publication, and resumption, until resume-side result
// conversion (release_popped / release_failed — both validate owner_port_
// against a LIVE port) completes. The fiber frontend spans the same interval
// with its CallGuard living on the suspended fiber stack; the deferred
// frontend transfers the pin to the awaiter, which releases it in
// await_resume. The death child QD1 (async_queue_lifecycle_death_test)
// proves begin_teardown itself fail-fasts inside the window.

// QPIN-1 — granted item: the pin survives the winner + publication phase.
SLUICE_TEST_CASE(fe3_q_qpin1_pop_pin_through_result_consumption) {
    AsyncIoContext ctx(std::make_unique<FakeAsyncBackend>());
    Scheduler sched(ctx);
    QueuePort port(sched, 1);
    std::atomic<int> resumed{0};
    std::atomic<int> guard_failures{0};

    struct Case {
        Scheduler& sched;
        QueuePort& port;
        std::atomic<int>& resumed;
        FePopStatus observed = kPopClosed;
        int got = -1;
        FeTask run() {
            FeQueuePopAwaiter aw{sched, port};
            co_await aw;  // parks: ring empty
            resumed.fetch_add(1, std::memory_order_relaxed);
            observed = aw.status;
            got = aw.recovered;
            co_return;
        }
    };
    Case c{sched, port, resumed};
    FeTask t = c.run();
    t.start();  // parked; the pin transferred to the awaiter

    // Phase: parked — entry accepted, suspension authorized.
    SLUICE_CHECK(AsyncTestAccess::queue_active_port_calls_for_test(port) == 1);
    SLUICE_CHECK(AsyncTestAccess::queue_active_wait_associations_for_test(
                     port) == 1);
    SLUICE_CHECK(AsyncTestAccess::deferred_depth_for_test(sched) == 0);

    // Grant: FastPushCommit resolves the parked consumer Woken (terminal
    // winner + resource commit + unlinks the role FIFO) and defers the
    // publication.
    {
        auto lease = QueueItemFactory::make<int>(port, 7);
        (void)port.try_push(std::move(lease));
    }
    // Phase: publication pending, BEFORE discharge/result consumption. This
    // is the exact pre-corrective window in which every OTHER begin_teardown
    // precondition was already zero — the pin is the only surviving
    // obligation, and it belongs to the deferred op.
    SLUICE_CHECK(AsyncTestAccess::queue_active_port_calls_for_test(port) == 1);
    SLUICE_CHECK(AsyncTestAccess::queue_active_wait_associations_for_test(
                     port) == 0);
    SLUICE_CHECK(AsyncTestAccess::queue_active_queue_timers_for_test(port) ==
                 0);
    SLUICE_CHECK(
        AsyncTestAccess::queue_granted_not_resumed_for_test(port) == 0);
    SLUICE_CHECK(AsyncTestAccess::deferred_depth_for_test(sched) == 1);
    SLUICE_CHECK(resumed.load() == 0);  // continuation NOT yet discharged

    // Discharge + resume + resume-side conversion; the pin releases in
    // await_resume after release_popped consumed the granted lease.
    (void)drain_all(sched, guard_failures);
    SLUICE_CHECK(resumed.load() == 1);
    SLUICE_CHECK(guard_failures.load() == 0);
    SLUICE_CHECK(c.observed == kPopItem);
    SLUICE_CHECK(c.got == 7);

    // Phase: result consumed — every counter back to zero; teardown passes.
    SLUICE_CHECK(AsyncTestAccess::queue_active_port_calls_for_test(port) == 0);
    { (void)port.begin_teardown(); }
}

// QPIN-2 — failed/closed producer path: the pin survives through the
// retained-lease conversion (release_failed) on resume.
SLUICE_TEST_CASE(fe3_q_qpin2_push_pin_through_closed_result) {
    AsyncIoContext ctx(std::make_unique<FakeAsyncBackend>());
    Scheduler sched(ctx);
    QueuePort port(sched, 1);
    std::atomic<int> resumed{0};
    std::atomic<int> guard_failures{0};
    {
        auto lease = QueueItemFactory::make<int>(port, 5);
        (void)port.try_push(std::move(lease));
    }

    struct Case {
        Scheduler& sched;
        QueuePort& port;
        std::atomic<int>& resumed;
        FePushStatus observed = kPushCommitted;
        int got = -1;
        FeTask run() {
            FeQueuePushAwaiter aw{sched, port, 6};
            co_await aw;  // parks: ring full
            resumed.fetch_add(1, std::memory_order_relaxed);
            observed = aw.status;
            got = aw.recovered;
            co_return;
        }
    };
    Case c{sched, port, resumed};
    FeTask t = c.run();
    t.start();

    // Phase: parked.
    SLUICE_CHECK(AsyncTestAccess::queue_active_port_calls_for_test(port) == 1);

    // Close resolves the parked producer (closed disposition, lease
    // RETAINED) and defers the publication.
    port.close();
    // Phase: publication pending; pin still held; role FIFO already drained.
    SLUICE_CHECK(AsyncTestAccess::queue_active_port_calls_for_test(port) == 1);
    SLUICE_CHECK(AsyncTestAccess::queue_active_wait_associations_for_test(
                     port) == 0);
    SLUICE_CHECK(AsyncTestAccess::deferred_depth_for_test(sched) == 1);

    (void)drain_all(sched, guard_failures);
    SLUICE_CHECK(resumed.load() == 1);
    SLUICE_CHECK(guard_failures.load() == 0);
    SLUICE_CHECK(c.observed == kPushClosed);
    SLUICE_CHECK(c.got == 6);  // release_failed consumed the retained lease

    // Phase: result consumed.
    SLUICE_CHECK(AsyncTestAccess::queue_active_port_calls_for_test(port) == 0);
    // Drain the buffered item (destruction contract), then teardown passes.
    {
        auto rp = port.try_pop();
        SLUICE_CHECK(rp.status() == QueueOpaquePopStatus::item);
        SLUICE_CHECK(release_popped<int>(port, std::move(rp)) == 5);
    }
    { (void)port.begin_teardown(); }
}
SLUICE_MAIN()
