// fe3_stackless_rwlock_slice_test — FE campaign stage FE-3, AsyncRwLock
// vertical slice. A second (stackless C++20 coroutine) frontend drives the
// RwLock through the SAME semantic authorities as the stackful Fiber
// frontend:
//
//   - the shared read/write admission ladders
//     (rwlock_read_admit_locked / rwlock_write_admit_locked — ONE textual
//     admission law per mode, blocking+timed);
//   - the head-prefix claim primitive (rwlock_claim_node_woken_locked) and
//     the unified head reconcile (rwlock_grant_from_head_locked) — writer
//     fairness, reader prefix batching, cancel/expire reconcile;
//   - the winner-kind publication edge (publish_wait_winner_locked);
//   - the ActorIdentity seam (FE-1b A1): writer ownership is committed and
//     compared on ActorId — try-write recursive detection, the checked
//     release core, and the grant-time ownership commit NEVER touch the
//     ResumeTarget delivery token.
//
// What is test-only (AGENTS.md §15 / FE-1c scope): the coroutine task, the
// awaiters, the FeDeferredRecord delivery record, and the ACTOR TOKENS
// (plain test-scope objects whose addresses outlive each suspension — the
// FE-1a stability rule; the awaiters here reference case-owned node/ctx/
// record storage, which is equally address-stable). Everything
// semantically load-bearing is production code.
#include "harness.hpp"
#include "scheduler_internal.hpp"  // RwWaitCtx (shared, non-installed)

#include "async_test_control.hpp"

#include <sluice/async/async_io_context.hpp>
#include <sluice/async/async_rwlock.hpp>
#include <sluice/async/fake_backend.hpp>
#include <sluice/async/fiber.hpp>
#include <sluice/async/scheduler.hpp>
#include <sluice/async/wait_node.hpp>

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

// Awaiter over the shared WRITE ladder. node/ctx/rec are REFERENCES to
// caller-owned (case-owned) storage: address-stable across the suspension
// (FE-1a). The record address (ResumeTarget) is deliberately distinct from
// the actor token — the ownership/delivery separation under test.
struct FeRwWriteAwaiter {
    Scheduler& sched;
    AsyncRwLock& lock;
    WaitNode& node;
    RwWaitCtx& ctx;
    FeRecord& rec;
    void* actor_token;
    bool timed = false;
    Scheduler::deadline_t deadline = 0;

    bool did_suspend = false;
    bool await_ready() const noexcept { return false; }
    bool await_suspend(std::coroutine_handle<> h) {
        // FE-1b L2: bind the resume target BEFORE the epoch becomes
        // resolver-observable.
        rec.handle_address = h.address();
        if (timed) {
            did_suspend =
                AsyncTestAccess::rwlock_write_deferred_until_for_test(
                    sched, lock, node, actor_token, ctx, rec, deadline);
        } else {
            did_suspend = AsyncTestAccess::rwlock_write_deferred_for_test(
                sched, lock, node, actor_token, ctx, rec);
        }
        return did_suspend;
    }
    // true when the admission resolved INLINE (the caller never suspended).
    bool await_resume() const noexcept { return !did_suspend; }
};

// Awaiter over the shared READ ladder. Symmetric (actor bound but ignored by
// the v1 reader-count model).
struct FeRwReadAwaiter {
    Scheduler& sched;
    AsyncRwLock& lock;
    WaitNode& node;
    RwWaitCtx& ctx;
    FeRecord& rec;
    void* actor_token;
    bool timed = false;
    Scheduler::deadline_t deadline = 0;

    bool did_suspend = false;
    bool await_ready() const noexcept { return false; }
    bool await_suspend(std::coroutine_handle<> h) {
        rec.handle_address = h.address();
        if (timed) {
            did_suspend = AsyncTestAccess::rwlock_read_deferred_until_for_test(
                sched, lock, node, actor_token, ctx, rec, deadline);
        } else {
            did_suspend = AsyncTestAccess::rwlock_read_deferred_for_test(
                sched, lock, node, actor_token, ctx, rec);
        }
        return did_suspend;
    }
    bool await_resume() const noexcept { return !did_suspend; }
};

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

struct FiberStack {
    static constexpr std::size_t kBytes = 64 * 1024;
    alignas(16) std::vector<std::byte> bytes{kBytes};
    std::byte* base() noexcept { return bytes.data(); }
    std::size_t size() const noexcept { return bytes.size(); }
};

}  // namespace

// ---- A1 core: deferred writer owns AND releases on ActorId alone -----------
SLUICE_TEST_CASE(fe3_rwlock_deferred_writer_owns_releases) {
    AsyncIoContext ctx(std::make_unique<FakeAsyncBackend>());
    Scheduler sched(ctx);
    AsyncRwLock lock(sched);
    int actor_a = 0;  // test-scope ACTOR token (stable through the epoch)

    std::atomic<int> resumed{0};
    // Case-owned epoch storage (address-stable; the case outlives the task).
    WaitNode node;
    RwWaitCtx rwctx{};
    FeRecord rec;
    struct Case {
        Scheduler& sched;
        AsyncRwLock& lock;
        void* actor;
        WaitNode& node;
        RwWaitCtx& rwctx;
        FeRecord& rec;
        std::atomic<int>& resumed;
        bool was_inline = false;
        FeTask run() {
            FeRwWriteAwaiter aw{sched, lock, node, rwctx, rec, actor};
            was_inline = co_await aw;  // true = inline (never suspended)
            resumed.fetch_add(1, std::memory_order_relaxed);
            co_return;
        }
    };
    Case c{sched, lock, &actor_a, node, rwctx, rec, resumed};
    FeTask t = c.run();
    t.start();

    // The write was admitted INLINE (free lock): the coroutine never
    // suspended, and ownership was committed to the ACTOR (not the record).
    SLUICE_CHECK(c.was_inline);
    SLUICE_CHECK(resumed.load() == 1);
    SLUICE_CHECK(t.done());
    SLUICE_CHECK(AsyncTestAccess::rwlock_writer_active_for_test(lock));
    SLUICE_CHECK(AsyncTestAccess::rwlock_owned_by_for_test(lock, &actor_a));
    SLUICE_CHECK(rec.state.load() == FeRecord::State::unarmed);
    SLUICE_CHECK(AsyncTestAccess::deferred_depth_for_test(sched) == 0);

    // Release through the SAME checked core with the SAME actor token: the
    // ownership comparison passes on ActorId alone (A1).
    AsyncTestAccess::rwlock_unlock_write_deferred_for_test(sched, lock,
                                                           &actor_a);
    SLUICE_CHECK(!AsyncTestAccess::rwlock_writer_active_for_test(lock));
}

// ---- A1: same ActorIdentity + different ResumeTarget -----------------------
SLUICE_TEST_CASE(fe3_rwlock_same_actor_different_resume_target) {
    AsyncIoContext ctx(std::make_unique<FakeAsyncBackend>());
    Scheduler sched(ctx);
    AsyncRwLock lock(sched);
    int actor_a = 0;
    int actor_b = 0;

    std::atomic<int> resumed{0};
    WaitNode node;
    RwWaitCtx rwctx{};
    FeRecord rec;
    struct Case {
        Scheduler& sched;
        AsyncRwLock& lock;
        void* actor;
        WaitNode& node;
        RwWaitCtx& rwctx;
        FeRecord& rec;
        std::atomic<int>& resumed;
        bool was_inline = false;
        FeTask run() {
            FeRwWriteAwaiter aw{sched, lock, node, rwctx, rec, actor};
            was_inline = co_await aw;  // true = inline (never suspended)
            resumed.fetch_add(1, std::memory_order_relaxed);
            co_return;
        }
    };
    // Coroutine 1 carries actor_a. Its RESUME TARGET (its own delivery
    // record at &rec) is a different object than the actor token.
    Case c1{sched, lock, &actor_a, node, rwctx, rec, resumed};
    FeTask t1 = c1.run();
    t1.start();

    SLUICE_CHECK(c1.was_inline);
    SLUICE_CHECK(AsyncTestAccess::rwlock_owned_by_for_test(lock, &actor_a));

    // The SAME actor (via any future resume target) is recognized as the
    // owner: try-write hits the recursive-actor rule (false), NOT a
    // non-owner refusal — and no state changes.
    SLUICE_CHECK(!AsyncTestAccess::rwlock_try_write_deferred_for_test(
        sched, lock, &actor_a));
    SLUICE_CHECK(AsyncTestAccess::rwlock_owned_by_for_test(lock, &actor_a));

    // A DIFFERENT actor is refused while the writer is active.
    SLUICE_CHECK(!AsyncTestAccess::rwlock_try_write_deferred_for_test(
        sched, lock, &actor_b));
    SLUICE_CHECK(AsyncTestAccess::rwlock_owned_by_for_test(lock, &actor_a));

    // Release with the owner actor; a different actor then acquires.
    AsyncTestAccess::rwlock_unlock_write_deferred_for_test(sched, lock,
                                                           &actor_a);
    SLUICE_CHECK(AsyncTestAccess::rwlock_try_write_deferred_for_test(
        sched, lock, &actor_b));
    SLUICE_CHECK(AsyncTestAccess::rwlock_owned_by_for_test(lock, &actor_b));
    AsyncTestAccess::rwlock_unlock_write_deferred_for_test(sched, lock,
                                                           &actor_b);
    SLUICE_CHECK(!AsyncTestAccess::rwlock_writer_active_for_test(lock));
    SLUICE_CHECK(resumed.load() == 1);
}

// ---- Deferred writer granted by a fiber's unlock (ownership commit path) --
SLUICE_TEST_CASE(fe3_rwlock_deferred_writer_granted_by_fiber_unlock) {
    if (!fiber_ctx::supported) return;
    AsyncIoContext ctx(std::make_unique<FakeAsyncBackend>());
    Scheduler sched(ctx);
    AsyncRwLock lock(sched);
    std::atomic<int> resumed{0};
    std::atomic<int> guard_failures{0};
    int actor_a = 0;

    std::atomic<bool> fiber_holding{false};
    std::atomic<bool> go_unlock{false};
    std::atomic<bool> fiber_done{false};

    Fiber holder;
    holder.set_entry([&](Fiber&) {
        WaitNode nf;
        lock.write_lock(nf);  // free lock: inline claim
        fiber_holding.store(true, std::memory_order::release);
        // Bounded liveness wait only (the main thread drives the step).
        while (!go_unlock.load(std::memory_order::acquire)) {
            std::this_thread::yield();
        }
        lock.unlock_write();  // grant head: the parked DEFERRED writer
        fiber_done.store(true, std::memory_order::release);
    });
    FiberStack sh;
    SLUICE_CHECK(sched.init_fiber(holder, sh.base(), sh.size()));
    sched.spawn(holder);
    std::thread runner([&] { sched.run_live(1); });
    SLUICE_CHECK(bounded_wait(fiber_holding));

    WaitNode node;
    RwWaitCtx rwctx{};
    FeRecord rec;
    struct Case {
        Scheduler& sched;
        AsyncRwLock& lock;
        void* actor;
        WaitNode& node;
        RwWaitCtx& rwctx;
        FeRecord& rec;
        std::atomic<int>& resumed;
        bool suspended = false;
        FeTask run() {
            FeRwWriteAwaiter aw{sched, lock, node, rwctx, rec, actor};
            suspended = !co_await aw;  // true = parked behind the writer
            resumed.fetch_add(1, std::memory_order_relaxed);
            co_return;
        }
    };
    Case c{sched, lock, &actor_a, node, rwctx, rec, resumed};
    FeTask t = c.run();
    t.start();
    // Parked behind the fiber writer: the admission committed the deferred
    // PublicationEligibility (record ARMED) synchronously inside
    // await_suspend; the coroutine has NOT resumed, no winner exists yet, and
    // ownership still belongs to the FIBER actor. (`c.suspended` is only
    // assigned at co_await completion — i.e. after the grant+discharge.)
    SLUICE_CHECK(rec.state.load() == FeRecord::State::armed);
    SLUICE_CHECK(resumed.load() == 0);
    SLUICE_CHECK(!t.done());
    SLUICE_CHECK(AsyncTestAccess::rwlock_writer_active_for_test(lock));
    SLUICE_CHECK(!AsyncTestAccess::rwlock_owned_by_for_test(lock, &actor_a));
    SLUICE_CHECK(AsyncTestAccess::deferred_depth_for_test(sched) == 0);

    go_unlock.store(true, std::memory_order::release);
    const bool fdone = bounded_wait(fiber_done);
    SLUICE_CHECK(fdone);
    runner.join();

    // The grant committed the DEFERRED winner's ACTOR identity and deferred
    // the delivery.
    SLUICE_CHECK(AsyncTestAccess::rwlock_writer_active_for_test(lock));
    SLUICE_CHECK(AsyncTestAccess::rwlock_owned_by_for_test(lock, &actor_a));
    SLUICE_CHECK(AsyncTestAccess::deferred_depth_for_test(sched) == 1);
    (void)drain_all(sched, guard_failures);
    // The parked coroutine resumed (suspended is assigned at co_await
    // completion, so it is observable only after the discharge).
    SLUICE_CHECK(c.suspended);
    SLUICE_CHECK(resumed.load() == 1);
    SLUICE_CHECK(guard_failures.load() == 0);
    SLUICE_CHECK(t.done());
    // The coroutine actor releases with its own token.
    AsyncTestAccess::rwlock_unlock_write_deferred_for_test(sched, lock,
                                                           &actor_a);
    SLUICE_CHECK(!AsyncTestAccess::rwlock_writer_active_for_test(lock));
}

// ---- Writer fairness: a parked reader must not bypass a parked writer -----
SLUICE_TEST_CASE(fe3_rwlock_writer_fairness_deferred) {
    AsyncIoContext ctx(std::make_unique<FakeAsyncBackend>());
    Scheduler sched(ctx);
    AsyncRwLock lock(sched);
    std::atomic<int> resumed{0};
    std::atomic<int> guard_failures{0};
    int actor_w = 0;
    int actor_r = 0;

    // Epoch storage for three waiters (case-owned, address-stable).
    WaitNode n1, n2, nw, n3;
    RwWaitCtx x1, x2, xw, x3;
    FeRecord c1, c2, cw, c3;

    struct ReadCase {
        Scheduler& sched;
        AsyncRwLock& lock;
        void* actor;
        WaitNode& node;
        RwWaitCtx& rwctx;
        FeRecord& rec;
        std::atomic<int>& resumed;
        bool suspended = false;
        FeTask run() {
            FeRwReadAwaiter aw{sched, lock, node, rwctx, rec, actor};
            suspended = !co_await aw;
            resumed.fetch_add(1, std::memory_order_relaxed);
            co_return;
        }
    };
    struct WriteCase {
        Scheduler& sched;
        AsyncRwLock& lock;
        void* actor;
        WaitNode& node;
        RwWaitCtx& rwctx;
        FeRecord& rec;
        std::atomic<int>& resumed;
        bool suspended = false;
        FeTask run() {
            FeRwWriteAwaiter aw{sched, lock, node, rwctx, rec, actor};
            suspended = !co_await aw;  // true = parked
            resumed.fetch_add(1, std::memory_order_relaxed);
            co_return;
        }
    };

    // Two inline deferred readers hold the lock.
    ReadCase r1{sched, lock, &actor_r, n1, x1, c1, resumed};
    ReadCase r2{sched, lock, &actor_r, n2, x2, c2, resumed};
    FeTask t1 = r1.run();
    FeTask t2 = r2.run();
    t1.start();
    t2.start();
    SLUICE_CHECK(!r1.suspended && !r2.suspended);  // both inline (no writer)

    // A writer parks; then a reader parks BEHIND it.
    WriteCase w{sched, lock, &actor_w, nw, xw, cw, resumed};
    FeTask tw = w.run();
    tw.start();
    // Parked: the admission armed the record synchronously in await_suspend
    // (`suspended` is assigned only at co_await completion, after resume).
    SLUICE_CHECK(cw.state.load() == FeRecord::State::armed);
    SLUICE_CHECK(!tw.done());

    ReadCase r3{sched, lock, &actor_r, n3, x3, c3, resumed};
    FeTask tr = r3.run();
    tr.start();
    SLUICE_CHECK(c3.state.load() == FeRecord::State::armed);  // parked behind w
    SLUICE_CHECK(!tr.done());

    // Last reader releases: head = WRITER (fairness — r3 must NOT be granted,
    // so exactly ONE deferred publication is in flight).
    AsyncTestAccess::rwlock_unlock_read_for_test(sched, lock);
    AsyncTestAccess::rwlock_unlock_read_for_test(sched, lock);
    SLUICE_CHECK(AsyncTestAccess::deferred_depth_for_test(sched) == 1);
    (void)drain_all(sched, guard_failures);
    SLUICE_CHECK(AsyncTestAccess::rwlock_writer_active_for_test(lock));
    SLUICE_CHECK(AsyncTestAccess::rwlock_owned_by_for_test(lock, &actor_w));
    SLUICE_CHECK(resumed.load() == 3);  // r1, r2 inline + w drained

    // A barging try-read is refused while the writer holds.
    SLUICE_CHECK(!AsyncTestAccess::rwlock_try_read_for_test(sched, lock));

    // Writer releases: the exposed head reader (r3) is granted.
    AsyncTestAccess::rwlock_unlock_write_deferred_for_test(sched, lock,
                                                           &actor_w);
    SLUICE_CHECK(AsyncTestAccess::deferred_depth_for_test(sched) == 1);
    (void)drain_all(sched, guard_failures);
    SLUICE_CHECK(!AsyncTestAccess::rwlock_writer_active_for_test(lock));
    SLUICE_CHECK(resumed.load() == 4);
    AsyncTestAccess::rwlock_unlock_read_for_test(sched, lock);  // r3 releases
    SLUICE_CHECK(AsyncTestAccess::rwlock_try_read_for_test(sched, lock));
    AsyncTestAccess::rwlock_unlock_read_for_test(sched, lock);  // probe off
}

// ---- Reader prefix batch: two deferred readers granted by one reconcile ---
SLUICE_TEST_CASE(fe3_rwlock_reader_batch_deferred) {
    if (!fiber_ctx::supported) return;
    AsyncIoContext ctx(std::make_unique<FakeAsyncBackend>());
    Scheduler sched(ctx);
    AsyncRwLock lock(sched);
    std::atomic<int> resumed{0};
    std::atomic<int> guard_failures{0};
    int actor_b = 0;
    int actor_r = 0;

    // A fiber writer holds; two deferred readers park behind it.
    std::atomic<bool> fiber_holding{false};
    std::atomic<bool> go_unlock{false};
    std::atomic<bool> fiber_done{false};
    Fiber holder;
    holder.set_entry([&](Fiber&) {
        WaitNode nf;
        lock.write_lock(nf);
        fiber_holding.store(true, std::memory_order::release);
        while (!go_unlock.load(std::memory_order::acquire)) {
            std::this_thread::yield();
        }
        lock.unlock_write();  // batch-grants the parked deferred readers
        fiber_done.store(true, std::memory_order::release);
    });
    FiberStack sh;
    SLUICE_CHECK(sched.init_fiber(holder, sh.base(), sh.size()));
    sched.spawn(holder);
    std::thread runner([&] { sched.run_live(1); });
    SLUICE_CHECK(bounded_wait(fiber_holding));

    WaitNode n1, n2;
    RwWaitCtx x1, x2;
    FeRecord c1, c2;
    struct ReadCase {
        Scheduler& sched;
        AsyncRwLock& lock;
        void* actor;
        WaitNode& node;
        RwWaitCtx& rwctx;
        FeRecord& rec;
        std::atomic<int>& resumed;
        bool suspended = false;
        FeTask run() {
            FeRwReadAwaiter aw{sched, lock, node, rwctx, rec, actor};
            suspended = !co_await aw;
            resumed.fetch_add(1, std::memory_order_relaxed);
            co_return;
        }
    };
    ReadCase r1{sched, lock, &actor_r, n1, x1, c1, resumed};
    ReadCase r2{sched, lock, &actor_r, n2, x2, c2, resumed};
    FeTask t1 = r1.run();
    FeTask t2 = r2.run();
    t1.start();
    t2.start();
    // Both parked behind the fiber writer (records armed synchronously).
    SLUICE_CHECK(c1.state.load() == FeRecord::State::armed);
    SLUICE_CHECK(c2.state.load() == FeRecord::State::armed);
    SLUICE_CHECK(!t1.done() && !t2.done());

    go_unlock.store(true, std::memory_order::release);
    SLUICE_CHECK(bounded_wait(fiber_done));
    runner.join();

    // ONE reconcile granted BOTH parked deferred readers (prefix batch).
    SLUICE_CHECK(AsyncTestAccess::deferred_depth_for_test(sched) == 2);
    (void)drain_all(sched, guard_failures);
    SLUICE_CHECK(resumed.load() == 2);
    SLUICE_CHECK(guard_failures.load() == 0);
    SLUICE_CHECK(t1.done() && t2.done());

    // Reader-count accounting with deferred holders: two shares out.
    SLUICE_CHECK(!AsyncTestAccess::rwlock_try_write_deferred_for_test(
        sched, lock, &actor_b));
    AsyncTestAccess::rwlock_unlock_read_for_test(sched, lock);
    SLUICE_CHECK(!AsyncTestAccess::rwlock_try_write_deferred_for_test(
        sched, lock, &actor_b));  // r2 still holds
    AsyncTestAccess::rwlock_unlock_read_for_test(sched, lock);
    SLUICE_CHECK(AsyncTestAccess::rwlock_try_write_deferred_for_test(
        sched, lock, &actor_b));  // count reached zero
    AsyncTestAccess::rwlock_unlock_write_deferred_for_test(sched, lock,
                                                           &actor_b);
}

// ---- Deferred writer cancel: Cancelled winner + head reconcile ------------
SLUICE_TEST_CASE(fe3_rwlock_cancel_deferred) {
    AsyncIoContext ctx(std::make_unique<FakeAsyncBackend>());
    Scheduler sched(ctx);
    AsyncRwLock lock(sched);
    std::atomic<int> resumed{0};
    std::atomic<int> guard_failures{0};
    int actor_w = 0;
    int actor_b = 0;
    int actor_r = 0;

    WaitNode node;
    RwWaitCtx rwctx{};
    FeRecord rec;
    struct Case {
        Scheduler& sched;
        AsyncRwLock& lock;
        void* actor;
        WaitNode& node;
        RwWaitCtx& rwctx;
        FeRecord& rec;
        std::atomic<int>& resumed;
        bool suspended = false;
        FeTask run() {
            FeRwWriteAwaiter aw{sched, lock, node, rwctx, rec, actor};
            suspended = !co_await aw;
            resumed.fetch_add(1, std::memory_order_relaxed);
            co_return;
        }
    };

    // A deferred reader holds one read share INLINE (free lock): a writer
    // cannot claim, so the writer below genuinely PARKS (cancel needs a
    // Registered loser-to-be, not an inline winner).
    WaitNode rn;
    RwWaitCtx rx{};
    FeRecord cr;
    struct ReadCase {
        Scheduler& sched;
        AsyncRwLock& lock;
        void* actor;
        WaitNode& node;
        RwWaitCtx& rwctx;
        FeRecord& rec;
        std::atomic<int>& resumed;
        FeTask run() {
            FeRwReadAwaiter aw{sched, lock, node, rwctx, rec, actor};
            co_await aw;  // inline: no writer, empty queue
            resumed.fetch_add(1, std::memory_order_relaxed);
            co_return;
        }
    };
    ReadCase r{sched, lock, &actor_r, rn, rx, cr, resumed};
    FeTask tr = r.run();
    tr.start();
    SLUICE_CHECK(tr.done());  // inline completion
    SLUICE_CHECK(cr.state.load() == FeRecord::State::unarmed);
    SLUICE_CHECK(resumed.load() == 1);

    Case c{sched, lock, &actor_w, node, rwctx, rec, resumed};
    FeTask t = c.run();
    t.start();
    // Parked behind the read share (record armed synchronously; `suspended`
    // is observable only after the cancel drain resumes the coroutine).
    SLUICE_CHECK(rec.state.load() == FeRecord::State::armed);
    SLUICE_CHECK(!t.done());

    // Cancel through the production seam (membership + CANCEL CAS + head
    // reconcile + winner-kind publication).
    SLUICE_CHECK(AsyncTestAccess::rwlock_cancel_deferred_for_test(sched, lock,
                                                                  node));
    SLUICE_CHECK(AsyncTestAccess::deferred_depth_for_test(sched) == 1);
    (void)drain_all(sched, guard_failures);
    SLUICE_CHECK(resumed.load() == 2);  // inline reader + cancelled writer
    SLUICE_CHECK(guard_failures.load() == 0);
    SLUICE_CHECK(t.done());
    // Release the read share: the queue is empty, so nothing is granted.
    AsyncTestAccess::rwlock_unlock_read_for_test(sched, lock);
    SLUICE_CHECK(AsyncTestAccess::deferred_depth_for_test(sched) == 0);
    // The lock stayed free; a different actor acquires.
    SLUICE_CHECK(AsyncTestAccess::rwlock_try_write_deferred_for_test(
        sched, lock, &actor_b));
    AsyncTestAccess::rwlock_unlock_write_deferred_for_test(sched, lock,
                                                           &actor_b);
    // Cancel of the now-terminal node is a loser.
    SLUICE_CHECK(!AsyncTestAccess::rwlock_cancel_deferred_for_test(sched, lock,
                                                                   node));
}

// ---- Deferred timed writer: pump expiry + reconcile ------------------------
SLUICE_TEST_CASE(fe3_rwlock_expire_deferred) {
    AsyncIoContext ctx(std::make_unique<FakeAsyncBackend>());
    Scheduler sched(ctx);
    AsyncRwLock lock(sched);
    std::atomic<int> resumed{0};
    std::atomic<int> guard_failures{0};
    int actor_w = 0;
    int actor_b = 0;
    int actor_r = 0;
    sluice_async_test::TimerTestControl::enable_test_clock(sched);
    const Scheduler::deadline_t deadline =
        AsyncTestAccess::clock_now(sched) + 50;

    WaitNode node;
    RwWaitCtx rwctx{};
    FeRecord rec;
    struct Case {
        Scheduler& sched;
        AsyncRwLock& lock;
        void* actor;
        Scheduler::deadline_t deadline;
        WaitNode& node;
        RwWaitCtx& rwctx;
        FeRecord& rec;
        std::atomic<int>& resumed;
        bool suspended = false;
        FeTask run() {
            FeRwWriteAwaiter aw{sched, lock, node, rwctx, rec, actor};
            aw.timed = true;
            aw.deadline = deadline;
            suspended = !co_await aw;  // parks: clock < deadline
            resumed.fetch_add(1, std::memory_order_relaxed);
            co_return;
        }
    };

    // A deferred reader holds one read share INLINE: the timed writer below
    // cannot claim and genuinely PARKS with its deadline armed.
    WaitNode rn;
    RwWaitCtx rx{};
    FeRecord cr;
    struct ReadCase {
        Scheduler& sched;
        AsyncRwLock& lock;
        void* actor;
        WaitNode& node;
        RwWaitCtx& rwctx;
        FeRecord& rec;
        std::atomic<int>& resumed;
        FeTask run() {
            FeRwReadAwaiter aw{sched, lock, node, rwctx, rec, actor};
            co_await aw;  // inline: no writer, empty queue
            resumed.fetch_add(1, std::memory_order_relaxed);
            co_return;
        }
    };
    ReadCase r{sched, lock, &actor_r, rn, rx, cr, resumed};
    FeTask tr = r.run();
    tr.start();
    SLUICE_CHECK(tr.done());  // inline completion
    SLUICE_CHECK(resumed.load() == 1);

    Case c{sched, lock, &actor_w, deadline, node, rwctx, rec, resumed};
    FeTask t = c.run();
    t.start();
    // Parked before the deadline (record armed synchronously).
    SLUICE_CHECK(rec.state.load() == FeRecord::State::armed);
    SLUICE_CHECK(!t.done());
    SLUICE_CHECK(AsyncTestAccess::deferred_depth_for_test(sched) == 0);

    sched.advance_clock(100);  // pump wins Expired -> reconcile -> defer
    SLUICE_CHECK(AsyncTestAccess::deferred_depth_for_test(sched) == 1);
    (void)drain_all(sched, guard_failures);
    SLUICE_CHECK(resumed.load() == 2);  // inline reader + expired writer
    SLUICE_CHECK(guard_failures.load() == 0);
    SLUICE_CHECK(t.done());
    // Release the read share: the queue is empty, so nothing is granted.
    AsyncTestAccess::rwlock_unlock_read_for_test(sched, lock);
    SLUICE_CHECK(AsyncTestAccess::deferred_depth_for_test(sched) == 0);
    // The lock stayed free; the timer was retired (a fresh writer acquires).
    SLUICE_CHECK(AsyncTestAccess::rwlock_try_write_deferred_for_test(
        sched, lock, &actor_b));
    AsyncTestAccess::rwlock_unlock_write_deferred_for_test(sched, lock,
                                                           &actor_b);
}
SLUICE_MAIN()
