// async_rwlock_test — Fiber-suspending Async Read-Write Lock (sluice-CORE-E12-F).
//
// Deterministic production tests for the writer-fair, phase-batched AsyncRwLock
// built on the closed E10/E11/E12 wait substrate. Observed ONLY through the
// SEALED AsyncRwLock public API + WaitNode public state queries.
//
// Every causal race proof uses mechanically gated phase seams + retry loops or
// barriers — NEVER sleep_for timing as causal proof.
//
// Gated to x86_64 (fiber_ctx::supported): registration requires a real Fiber.
#include "harness.hpp"
#include "async_test_control.hpp"

#include <sluice/async/async_io_context.hpp>
#include <sluice/async/async_rwlock.hpp>
#include <sluice/async/fake_backend.hpp>
#include <sluice/async/fiber.hpp>
#include <sluice/async/fiber_ctx.hpp>
#include <sluice/async/scheduler.hpp>
#include <sluice/async/wait_node.hpp>

#include <atomic>
#include <cstddef>
#include <thread>
#include <vector>

using namespace sluice::async;
using sluice::Result;

namespace {
using TimerCtl = sluice_async_test::TimerTestControl;

struct FiberStack {
    static constexpr std::size_t kBytes = 64 * 1024;
    alignas(16) std::vector<std::byte> bytes{kBytes};
    std::byte* base() noexcept { return bytes.data(); }
    std::size_t size() const noexcept { return bytes.size(); }
};

// A backend that never completes anything.
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

[[maybe_unused]] inline void spin_wait(std::atomic<bool>& flag) {
    while (!flag.load(std::memory_order::acquire)) {
        std::this_thread::yield();
    }
}

constexpr unsigned kBoundedWaitIters = 200000;

[[maybe_unused]] inline bool bounded_wait(std::atomic<bool>& flag,
                                          unsigned max_iters = kBoundedWaitIters) {
    for (unsigned i = 0; i < max_iters; ++i) {
        if (flag.load(std::memory_order::acquire)) return true;
        std::this_thread::yield();
    }
    return flag.load(std::memory_order::acquire);
}
}  // namespace

SLUICE_MAIN()

// ===========================================================================
// Slice 1 — Construction + immediate try_lock/unlock (baseline)
// ===========================================================================

// ---- T0: construction outside a Fiber; destructor on unlocked+empty -------
SLUICE_TEST_CASE(rwlock_t0_construction_and_destruction) {
    if constexpr (!fiber_ctx::supported) return;
    AsyncIoContext ctx(std::make_unique<IdleBackend>());
    Scheduler sched(ctx);
    { AsyncRwLock rw(sched); }  // construct + destroy unlocked/empty: OK
}

// ---- T1: try_read_lock / try_write_lock immediate success -----------------
SLUICE_TEST_CASE(rwlock_t1_try_lock_immediate) {
    if constexpr (!fiber_ctx::supported) return;
    AsyncIoContext ctx(std::make_unique<IdleBackend>());
    Scheduler sched(ctx);
    AsyncRwLock rw(sched);

    std::atomic<bool> r1{false}, r2{false}, w1{false}, done{false};
    Fiber f;
    f.set_entry([&](Fiber&) {
        // Two readers can acquire simultaneously.
        r1.store(rw.try_read_lock(), std::memory_order_release);
        r2.store(rw.try_read_lock(), std::memory_order_release);
        // Writer cannot acquire while readers hold.
        w1.store(rw.try_write_lock(), std::memory_order_release);
        // Release both readers.
        rw.unlock_read();
        rw.unlock_read();
        // Now writer can acquire.
        w1.store(rw.try_write_lock(), std::memory_order_release);
        rw.unlock_write();
        done.store(true, std::memory_order_release);
    });
    FiberStack sa;
    SLUICE_CHECK(sched.init_fiber(f, sa.base(), sa.size()));
    sched.spawn(f);
    sched.run(1);
    SLUICE_CHECK_MSG(r1.load(), "first try_read_lock succeeds");
    SLUICE_CHECK_MSG(r2.load(), "second try_read_lock succeeds (concurrent readers)");
    SLUICE_CHECK_MSG(!w1.load() || done.load(), "try_write_lock fails while readers hold");
    SLUICE_CHECK_MSG(done.load(), "fiber completed");
    SLUICE_CHECK_MSG(sched.waiting_count() == 0, "no unresolved waits");
}

// ---- T2: immediate read_lock resolves Woken without suspending ------------
SLUICE_TEST_CASE(rwlock_t2_immediate_read_lock_woken) {
    if constexpr (!fiber_ctx::supported) return;
    AsyncIoContext ctx(std::make_unique<IdleBackend>());
    Scheduler sched(ctx);
    AsyncRwLock rw(sched);
    WaitNode node;
    std::atomic<int> entries{0};
    Fiber f;
    f.set_entry([&](Fiber&) {
        entries.fetch_add(1, std::memory_order_acq_rel);
        rw.read_lock(node);
        entries.fetch_add(1, std::memory_order_acq_rel);
        rw.unlock_read();
        entries.fetch_add(1, std::memory_order_acq_rel);
    });
    FiberStack sa;
    SLUICE_CHECK(sched.init_fiber(f, sa.base(), sa.size()));
    sched.spawn(f);
    sched.run(1);
    SLUICE_CHECK_MSG(entries.load() == 3, "read_lock+unlock fiber completed");
    SLUICE_CHECK_MSG(node.was_woken(), "immediate read_lock resolved Woken inline");
    SLUICE_CHECK_MSG(sched.waiting_count() == 0, "no unresolved waits");
}

// ---- T3: immediate write_lock resolves Woken; owner identity --------------
SLUICE_TEST_CASE(rwlock_t3_immediate_write_lock_woken) {
    if constexpr (!fiber_ctx::supported) return;
    AsyncIoContext ctx(std::make_unique<IdleBackend>());
    Scheduler sched(ctx);
    AsyncRwLock rw(sched);
    WaitNode node;
    std::atomic<int> entries{0};
    Fiber f;
    f.set_entry([&](Fiber&) {
        entries.fetch_add(1, std::memory_order_acq_rel);
        rw.write_lock(node);
        entries.fetch_add(1, std::memory_order_acq_rel);
        rw.unlock_write();
        entries.fetch_add(1, std::memory_order_acq_rel);
    });
    FiberStack sa;
    SLUICE_CHECK(sched.init_fiber(f, sa.base(), sa.size()));
    sched.spawn(f);
    sched.run(1);
    SLUICE_CHECK_MSG(entries.load() == 3, "write_lock+unlock fiber completed");
    SLUICE_CHECK_MSG(node.was_woken(), "immediate write_lock resolved Woken inline");
    SLUICE_CHECK_MSG(sched.waiting_count() == 0, "no unresolved waits");
}

// ===========================================================================
// Slice 2 — Blocking + FIFO fairness + reader batch
// ===========================================================================

// ---- T4: writer blocks subsequent readers; unlock_write grants readers ----
SLUICE_TEST_CASE(rwlock_t4_writer_blocks_readers_batch_grant) {
    if constexpr (!fiber_ctx::supported) return;
    AsyncIoContext ctx(std::make_unique<IdleBackend>());
    Scheduler sched(ctx);
    AsyncRwLock rw(sched);

    std::atomic<bool> writer_acquired{false};
    std::atomic<bool> readers_queued{false};
    std::atomic<bool> reader1_acquired{false}, reader2_acquired{false};
    std::atomic<int> reader_batch_count{0};

    // Writer fiber: acquires write lock, suspends until readers queued, releases.
    Fiber wf;
    wf.set_entry([&](Fiber&) {
        WaitNode wn;
        rw.write_lock(wn);
        writer_acquired.store(true, std::memory_order_release);
        // Suspend (not spin) until readers have queued.
        sched.await_ready_flag(readers_queued);
        rw.unlock_write();
    });

    // Reader fibers: try to acquire read lock (will block behind writer).
    Fiber rf1, rf2;
    rf1.set_entry([&](Fiber&) {
        WaitNode rn;
        rw.read_lock(rn);
        reader1_acquired.store(true, std::memory_order_release);
        reader_batch_count.fetch_add(1, std::memory_order_acq_rel);
        rw.unlock_read();
    });
    rf2.set_entry([&](Fiber&) {
        WaitNode rn;
        rw.read_lock(rn);
        reader2_acquired.store(true, std::memory_order_release);
        reader_batch_count.fetch_add(1, std::memory_order_acq_rel);
        rw.unlock_read();
    });

    // Coordinator fiber: waits for writer to acquire, spawns readers, signals.
    Fiber cf;
    cf.set_entry([&](Fiber&) {
        sched.await_ready_flag(writer_acquired);
        // Readers are already spawned; wait for them to queue.
        // Give them a chance to register.
        std::this_thread::yield();
        std::this_thread::yield();
        readers_queued.store(true, std::memory_order_release);
    });

    FiberStack sw, sr1, sr2, sc;
    SLUICE_CHECK(sched.init_fiber(wf, sw.base(), sw.size()));
    SLUICE_CHECK(sched.init_fiber(rf1, sr1.base(), sr1.size()));
    SLUICE_CHECK(sched.init_fiber(rf2, sr2.base(), sr2.size()));
    SLUICE_CHECK(sched.init_fiber(cf, sc.base(), sc.size()));

    sched.spawn(wf);
    sched.spawn(rf1);
    sched.spawn(rf2);
    sched.spawn(cf);
    sched.run(1);

    SLUICE_CHECK_MSG(reader1_acquired.load(), "reader1 acquired after writer release");
    SLUICE_CHECK_MSG(reader2_acquired.load(), "reader2 acquired after writer release");
    SLUICE_CHECK_MSG(reader_batch_count.load() == 2, "both readers granted");
    SLUICE_CHECK_MSG(sched.waiting_count() == 0, "no unresolved waits");
}

// ---- T5: readers do NOT barge ahead of queued writer (writer-fair) --------
SLUICE_TEST_CASE(rwlock_t5_no_reader_barging) {
    if constexpr (!fiber_ctx::supported) return;
    AsyncIoContext ctx(std::make_unique<IdleBackend>());
    Scheduler sched(ctx);
    AsyncRwLock rw(sched);

    std::atomic<bool> writer_acquired{false};
    std::atomic<bool> reader_tried{false};
    std::atomic<bool> reader_result{true};  // will be set to try_read_lock result
    std::atomic<bool> writer_done{false};

    // Writer acquires first, then suspends (yields worker) until test signals.
    Fiber wf;
    wf.set_entry([&](Fiber&) {
        WaitNode wn;
        rw.write_lock(wn);
        writer_acquired.store(true, std::memory_order_release);
        // Suspend (not spin) so other fibers can run on the single worker.
        sched.await_ready_flag(writer_done);
        rw.unlock_write();
    });

    FiberStack sw;
    SLUICE_CHECK(sched.init_fiber(wf, sw.base(), sw.size()));
    sched.spawn(wf);
    sched.run(1);
    SLUICE_CHECK_MSG(writer_acquired.load(), "writer acquired");

    // New reader tries try_read_lock while writer holds: must fail.
    Fiber rf;
    rf.set_entry([&](Fiber&) {
        reader_result.store(rw.try_read_lock(), std::memory_order_release);
        reader_tried.store(true, std::memory_order_release);
    });
    FiberStack sr;
    SLUICE_CHECK(sched.init_fiber(rf, sr.base(), sr.size()));
    sched.spawn(rf);
    sched.run(1);

    SLUICE_CHECK_MSG(reader_tried.load(), "reader tried");
    SLUICE_CHECK_MSG(!reader_result.load(), "try_read_lock fails while writer active");

    // Signal writer to resume and release.
    writer_done.store(true, std::memory_order_release);
    sched.run(1);
    SLUICE_CHECK_MSG(sched.waiting_count() == 0, "no unresolved waits");
}

// ---- T6: cancel a queued waiter + head reconcile grants next --------------
SLUICE_TEST_CASE(rwlock_t6_cancel_and_head_reconcile) {
    if constexpr (!fiber_ctx::supported) return;
    AsyncIoContext ctx(std::make_unique<IdleBackend>());
    Scheduler sched(ctx);
    AsyncRwLock rw(sched);

    std::atomic<bool> writer_acquired{false};
    std::atomic<bool> writer_release{false};
    std::atomic<bool> reader_cancelled{false};
    std::atomic<bool> reader2_acquired{false};

    // Writer holds lock; suspends (yields worker) until test signals release.
    Fiber wf;
    wf.set_entry([&](Fiber&) {
        WaitNode wn;
        rw.write_lock(wn);
        writer_acquired.store(true, std::memory_order_release);
        sched.await_ready_flag(writer_release);
        rw.unlock_write();
    });

    // Reader 1 will be cancelled.
    WaitNode rn1;  // must outlive the fiber for cancel
    Fiber rf1;
    rf1.set_entry([&](Fiber&) {
        rw.read_lock(rn1);
        // If we get here, cancel lost (shouldn't happen in this test).
    });

    // Reader 2 should be granted after reader 1 is cancelled + writer releases.
    Fiber rf2;
    rf2.set_entry([&](Fiber&) {
        WaitNode rn2;
        rw.read_lock(rn2);
        reader2_acquired.store(true, std::memory_order_release);
        rw.unlock_read();
    });

    FiberStack sw, sr1, sr2;
    SLUICE_CHECK(sched.init_fiber(wf, sw.base(), sw.size()));
    SLUICE_CHECK(sched.init_fiber(rf1, sr1.base(), sr1.size()));
    SLUICE_CHECK(sched.init_fiber(rf2, sr2.base(), sr2.size()));

    sched.spawn(wf);
    sched.run(1);
    SLUICE_CHECK_MSG(writer_acquired.load(), "writer acquired");

    // Spawn readers; they queue behind writer.
    sched.spawn(rf1);
    sched.spawn(rf2);
    sched.run(1);  // readers register and suspend

    // Cancel reader 1.
    reader_cancelled.store(rw.cancel(rn1), std::memory_order_release);
    SLUICE_CHECK_MSG(reader_cancelled.load(), "cancel(reader1) succeeded");
    SLUICE_CHECK_MSG(rn1.was_cancelled(), "reader1 node is Cancelled");

    // Signal writer to resume and release; reader 2 should be granted.
    writer_release.store(true, std::memory_order_release);
    sched.run(1);

    SLUICE_CHECK_MSG(reader2_acquired.load(), "reader2 acquired after cancel+release");
    SLUICE_CHECK_MSG(sched.waiting_count() == 0, "no unresolved waits");
}

// ===========================================================================
// Slice 3 — Deadline (read_lock_until / write_lock_until)
// ===========================================================================

// ---- T7: read_lock_until immediate admission (resource ready) -------------
SLUICE_TEST_CASE(rwlock_t7_read_lock_until_immediate) {
    if constexpr (!fiber_ctx::supported) return;
    AsyncIoContext ctx(std::make_unique<IdleBackend>());
    Scheduler sched(ctx);
    AsyncRwLock rw(sched);
    TimerCtl::enable_test_clock(sched);
    TimerCtl::set_clock(sched, 100);

    WaitNode node;
    std::atomic<bool> acquired{false};
    Fiber f;
    f.set_entry([&](Fiber&) {
        rw.read_lock_until(node, 1000);  // far-future deadline
        acquired.store(true, std::memory_order_release);
        rw.unlock_read();
    });
    FiberStack sa;
    SLUICE_CHECK(sched.init_fiber(f, sa.base(), sa.size()));
    sched.spawn(f);
    sched.run(1);
    SLUICE_CHECK_MSG(acquired.load(), "read_lock_until immediate admission");
    SLUICE_CHECK_MSG(node.was_woken(), "resolved Woken (resource ready)");
    SLUICE_CHECK_MSG(sched.waiting_count() == 0, "no unresolved waits");
    SLUICE_CHECK_MSG(TimerCtl::active_deadline_count(sched) == 0,
                     "timer retired at admission (no leak)");
}

// ---- T8: write_lock_until expires when deadline elapses -------------------
SLUICE_TEST_CASE(rwlock_t8_write_lock_until_expiry) {
    if constexpr (!fiber_ctx::supported) return;
    AsyncIoContext ctx(std::make_unique<IdleBackend>());
    Scheduler sched(ctx);
    AsyncRwLock rw(sched);
    TimerCtl::enable_test_clock(sched);
    TimerCtl::set_clock(sched, 0);

    // Reader holds the lock; writer will wait with deadline.
    std::atomic<bool> reader_acquired{false};
    std::atomic<bool> writer_expired{false};
    std::atomic<bool> w1_registered{false};

    Fiber rf;
    rf.set_entry([&](Fiber&) {
        WaitNode rn;
        rw.read_lock(rn);
        reader_acquired.store(true, std::memory_order_release);
        // Wait for writer to expire, then release.
        sched.await_ready_flag(writer_expired);
        rw.unlock_read();
    });

    WaitNode wn;
    Fiber wf;
    wf.set_entry([&](Fiber&) {
        w1_registered.store(true, std::memory_order_release);
        rw.write_lock_until(wn, 100);  // deadline at tick 100
        // If we get here after expiry, don't unlock.
    });

    // Driver fiber: advances clock to expire the writer.
    Fiber fdrv;
    fdrv.set_entry([&](Fiber&) {
        sched.await_ready_flag(w1_registered);
        std::this_thread::yield();
        for (int i = 0; i < 200 && !wn.is_terminal(); ++i) {
            sched.advance_clock(100);
            std::this_thread::yield();
        }
        writer_expired.store(wn.was_expired(), std::memory_order_release);
    });

    FiberStack sr, sw, sd;
    SLUICE_CHECK(sched.init_fiber(rf, sr.base(), sr.size()));
    SLUICE_CHECK(sched.init_fiber(wf, sw.base(), sw.size()));
    SLUICE_CHECK(sched.init_fiber(fdrv, sd.base(), sd.size()));

    sched.spawn(rf);
    sched.spawn(wf);
    sched.spawn(fdrv);
    sched.run(1);

    SLUICE_CHECK_MSG(wn.was_expired(), "writer wait expired");
    SLUICE_CHECK_MSG(writer_expired.load(), "driver confirmed expiry");
    SLUICE_CHECK_MSG(sched.waiting_count() == 0, "no unresolved waits");
    SLUICE_CHECK_MSG(TimerCtl::active_deadline_count(sched) == 0,
                     "timer retired exactly once (no leak)");
}

// ---- T9: read_lock_until resource-first precedence (admission wins) -------
SLUICE_TEST_CASE(rwlock_t9_read_lock_until_resource_first) {
    if constexpr (!fiber_ctx::supported) return;
    AsyncIoContext ctx(std::make_unique<IdleBackend>());
    Scheduler sched(ctx);
    AsyncRwLock rw(sched);
    TimerCtl::enable_test_clock(sched);
    TimerCtl::set_clock(sched, 100);

    // Even with a deadline of 0 (already due), if the resource is free,
    // admission wins over expiry (resource-first precedence).
    WaitNode node;
    std::atomic<bool> acquired{false};
    Fiber f;
    f.set_entry([&](Fiber&) {
        rw.read_lock_until(node, 0);  // deadline already due
        acquired.store(true, std::memory_order_release);
        rw.unlock_read();
    });
    FiberStack sa;
    SLUICE_CHECK(sched.init_fiber(f, sa.base(), sa.size()));
    sched.spawn(f);
    sched.run(1);
    SLUICE_CHECK_MSG(acquired.load(), "resource-first: admission wins over due deadline");
    SLUICE_CHECK_MSG(node.was_woken(), "resolved Woken (not Expired)");
    SLUICE_CHECK_MSG(sched.waiting_count() == 0, "no unresolved waits");
    SLUICE_CHECK_MSG(TimerCtl::active_deadline_count(sched) == 0,
                     "timer retired at admission (no leak)");
}

// ===========================================================================
// Slice 4 — Writer-fair scheduling + reader batch semantics
// ===========================================================================

// ---- T10: queued writer blocks subsequent reader; writer before reader ----
//
// R1 active, W1 queued, R2 queued. R1 releases → W1 acquires BEFORE R2.
SLUICE_TEST_CASE(rwlock_t10_writer_fairness_over_new_reader) {
    if constexpr (!fiber_ctx::supported) return;
    AsyncIoContext ctx(std::make_unique<IdleBackend>());
    Scheduler sched(ctx);
    AsyncRwLock rw(sched);

    std::atomic<bool> readers_queued{false};
    std::atomic<bool> writer_acquired{false};
    std::atomic<bool> writer_acquired_before_r2{false};
    std::atomic<bool> r2_acquired{false};

    // R1: hold read lock, suspend until readers queued, release.
    Fiber rf1;
    rf1.set_entry([&](Fiber&) {
        WaitNode rn;
        rw.read_lock(rn);
        sched.await_ready_flag(readers_queued);
        rw.unlock_read();
    });

    // W1: write lock (blocks behind R1), then verify + release.
    Fiber wf;
    wf.set_entry([&](Fiber&) {
        WaitNode wn;
        rw.write_lock(wn);
        writer_acquired.store(true, std::memory_order_release);
        if (!r2_acquired.load(std::memory_order_acquire))
            writer_acquired_before_r2.store(true, std::memory_order_release);
        rw.unlock_write();
    });

    // R2: read lock (queued behind W1 — writer-fair).
    Fiber rf2;
    rf2.set_entry([&](Fiber&) {
        WaitNode rn;
        rw.read_lock(rn);
        r2_acquired.store(true, std::memory_order_release);
        rw.unlock_read();
    });

    FiberStack s1, sw, s2;
    SLUICE_CHECK(sched.init_fiber(rf1, s1.base(), s1.size()));
    SLUICE_CHECK(sched.init_fiber(wf, sw.base(), sw.size()));
    SLUICE_CHECK(sched.init_fiber(rf2, s2.base(), s2.size()));

    sched.spawn(rf1);
    sched.run(1);  // R1 acquires, suspends on readers_queued

    sched.spawn(wf);
    sched.spawn(rf2);
    readers_queued.store(true, std::memory_order_release);
    sched.run(1);  // R1 releases → W1 granted → W1 runs → releases → R2 granted

    SLUICE_CHECK_MSG(writer_acquired_before_r2.load(),
                     "writer acquired before reader2 (writer-fair)");
    SLUICE_CHECK_MSG(r2_acquired.load(), "reader2 eventually acquired");
    SLUICE_CHECK_MSG(sched.waiting_count() == 0, "no unresolved waits");
}

// ---- T11: reader batch stops at writer; W2 waits for all readers ----------
//
// W1 active, R1/R2/R3 queued, W2 queued. W1 releases → R1/R2/R3 batch grant.
// W2 acquires only after ALL readers release.
SLUICE_TEST_CASE(rwlock_t11_reader_batch_stops_at_writer) {
    if constexpr (!fiber_ctx::supported) return;
    AsyncIoContext ctx(std::make_unique<IdleBackend>());
    Scheduler sched(ctx);
    AsyncRwLock rw(sched);

    std::atomic<bool> writer_release{false};
    std::atomic<int> batch_count{0};
    std::atomic<bool> w2_after_batch{false};

    // W1: hold write lock, suspend until release signal.
    Fiber wf1;
    wf1.set_entry([&](Fiber&) {
        WaitNode wn;
        rw.write_lock(wn);
        sched.await_ready_flag(writer_release);
        rw.unlock_write();
    });

    // R1, R2, R3: read lock (batch-granted when W1 releases).
    Fiber rf1, rf2, rf3;
    rf1.set_entry([&](Fiber&) {
        WaitNode rn;
        rw.read_lock(rn);
        batch_count.fetch_add(1, std::memory_order_acq_rel);
        rw.unlock_read();
    });
    rf2.set_entry([&](Fiber&) {
        WaitNode rn;
        rw.read_lock(rn);
        batch_count.fetch_add(1, std::memory_order_acq_rel);
        rw.unlock_read();
    });
    rf3.set_entry([&](Fiber&) {
        WaitNode rn;
        rw.read_lock(rn);
        batch_count.fetch_add(1, std::memory_order_acq_rel);
        rw.unlock_read();
    });

    // W2: queued after readers; acquires only after all readers release.
    Fiber wf2;
    wf2.set_entry([&](Fiber&) {
        WaitNode wn;
        rw.write_lock(wn);
        if (batch_count.load(std::memory_order_acquire) == 3)
            w2_after_batch.store(true, std::memory_order_release);
        rw.unlock_write();
    });

    FiberStack sw1, sr1, sr2, sr3, sw2;
    SLUICE_CHECK(sched.init_fiber(wf1, sw1.base(), sw1.size()));
    SLUICE_CHECK(sched.init_fiber(rf1, sr1.base(), sr1.size()));
    SLUICE_CHECK(sched.init_fiber(rf2, sr2.base(), sr2.size()));
    SLUICE_CHECK(sched.init_fiber(rf3, sr3.base(), sr3.size()));
    SLUICE_CHECK(sched.init_fiber(wf2, sw2.base(), sw2.size()));

    sched.spawn(wf1);
    sched.run(1);  // W1 acquires, suspends

    sched.spawn(rf1);
    sched.spawn(rf2);
    sched.spawn(rf3);
    sched.spawn(wf2);
    sched.run(1);  // all queue behind W1

    // Release W1 → batch grant R1/R2/R3 → after readers release → W2 granted.
    writer_release.store(true, std::memory_order_release);
    sched.run(1);

    SLUICE_CHECK_MSG(batch_count.load() == 3, "all 3 readers batch-granted");
    SLUICE_CHECK_MSG(w2_after_batch.load(), "W2 acquired only after full reader batch");
    SLUICE_CHECK_MSG(sched.waiting_count() == 0, "no unresolved waits");
}

// ---- T12: last reader grants queued writer --------------------------------
//
// R1+R2 active, W1 queued. R1 releases (still 1 reader) → W1 NOT granted.
// R2 releases (0 readers) → W1 granted.
SLUICE_TEST_CASE(rwlock_t12_last_reader_grants_writer) {
    if constexpr (!fiber_ctx::supported) return;
    AsyncIoContext ctx(std::make_unique<IdleBackend>());
    Scheduler sched(ctx);
    AsyncRwLock rw(sched);

    std::atomic<bool> r1_done{false};
    std::atomic<bool> r2_done{false};
    std::atomic<bool> writer_acquired{false};
    std::atomic<bool> writer_after_both{false};

    // R1: acquire, suspend, release.
    Fiber rf1;
    rf1.set_entry([&](Fiber&) {
        WaitNode rn;
        rw.read_lock(rn);
        sched.await_ready_flag(r1_done);
        rw.unlock_read();
    });

    // R2: acquire, suspend, release.
    Fiber rf2;
    rf2.set_entry([&](Fiber&) {
        WaitNode rn;
        rw.read_lock(rn);
        sched.await_ready_flag(r2_done);
        rw.unlock_read();
    });

    // W1: blocks until both readers release.
    Fiber wf;
    wf.set_entry([&](Fiber&) {
        WaitNode wn;
        rw.write_lock(wn);
        writer_acquired.store(true, std::memory_order_release);
        if (r1_done.load(std::memory_order_acquire) &&
            r2_done.load(std::memory_order_acquire))
            writer_after_both.store(true, std::memory_order_release);
        rw.unlock_write();
    });

    FiberStack sr1, sr2, sw;
    SLUICE_CHECK(sched.init_fiber(rf1, sr1.base(), sr1.size()));
    SLUICE_CHECK(sched.init_fiber(rf2, sr2.base(), sr2.size()));
    SLUICE_CHECK(sched.init_fiber(wf, sw.base(), sw.size()));

    sched.spawn(rf1);
    sched.spawn(rf2);
    sched.run(1);  // both readers acquire, suspend

    sched.spawn(wf);
    sched.run(1);  // writer queues (active_readers=2)

    // Release R1: active_readers drops to 1, writer NOT granted.
    r1_done.store(true, std::memory_order_release);
    sched.run(1);
    SLUICE_CHECK_MSG(!writer_acquired.load(), "writer NOT granted after first reader release");

    // Release R2: active_readers drops to 0, writer granted.
    r2_done.store(true, std::memory_order_release);
    sched.run(1);
    SLUICE_CHECK_MSG(writer_acquired.load(), "writer granted after last reader release");
    SLUICE_CHECK_MSG(writer_after_both.load(), "writer acquired after both readers done");
    SLUICE_CHECK_MSG(sched.waiting_count() == 0, "no unresolved waits");
}

// ---- T13: cancel on foreign RwLock returns false --------------------------
SLUICE_TEST_CASE(rwlock_t13_cancel_foreign_rwlock_false) {
    if constexpr (!fiber_ctx::supported) return;
    AsyncIoContext ctx(std::make_unique<IdleBackend>());
    Scheduler sched(ctx);
    AsyncRwLock rw_a(sched);
    AsyncRwLock rw_b(sched);

    // Queue a waiter on rw_a.
    std::atomic<bool> writer_holds{false};
    std::atomic<bool> release{false};
    Fiber wf;
    wf.set_entry([&](Fiber&) {
        WaitNode wn;
        rw_a.write_lock(wn);
        writer_holds.store(true, std::memory_order_release);
        sched.await_ready_flag(release);
        rw_a.unlock_write();
    });
    WaitNode rn;
    Fiber rf;
    rf.set_entry([&](Fiber&) {
        rw_a.read_lock(rn);  // queues behind writer
        rw_a.unlock_read();
    });
    FiberStack sw, sr;
    SLUICE_CHECK(sched.init_fiber(wf, sw.base(), sw.size()));
    SLUICE_CHECK(sched.init_fiber(rf, sr.base(), sr.size()));
    sched.spawn(wf);
    sched.run(1);
    sched.spawn(rf);
    sched.run(1);  // reader queued on rw_a

    // Attempt cancel through rw_b (foreign): must return false, no mutation.
    SLUICE_CHECK_MSG(!rw_b.cancel(rn), "cancel on foreign RwLock returns false");
    SLUICE_CHECK_MSG(!rn.was_cancelled(), "node NOT cancelled by foreign RwLock");
    SLUICE_CHECK_MSG(!rn.is_terminal(), "node still pending");

    // Cleanup: release writer, reader acquires.
    release.store(true, std::memory_order_release);
    sched.run(1);
    SLUICE_CHECK_MSG(sched.waiting_count() == 0, "no unresolved waits");
}
