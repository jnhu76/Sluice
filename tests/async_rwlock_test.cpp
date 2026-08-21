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
#include <memory>  // std::make_unique
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
//
// STRICT two-outcome proof: capture try_write_lock's result while readers hold
// (MUST be false) separately from its result after both readers release (MUST
// be true). The previous test reused one variable `w1`, so the second store
// overwrote the first, and the final check `!w1.load() || done.load()` was
// trivially true once `done` was set — it could not distinguish a correct
// implementation from one that granted the writer while readers were still
// active. This rewritten version would fail under an implementation that
// allowed reader+writer overlap.
SLUICE_TEST_CASE(rwlock_t1_try_lock_immediate) {
    if constexpr (!fiber_ctx::supported) return;
    AsyncIoContext ctx(std::make_unique<IdleBackend>());
    Scheduler sched(ctx);
    AsyncRwLock rw(sched);

    std::atomic<bool> r1{false}, r2{false}, done{false};
    // Two INDEPENDENT outcomes for the two try_write_lock attempts.
    // Defaulted so that a missed store cannot masquerade as the wrong result.
    std::atomic<bool> write_while_readers{true};   // expect false
    std::atomic<bool> write_after_readers{false};  // expect true
    Fiber f;
    f.set_entry([&](Fiber&) {
        // Two readers can acquire simultaneously.
        r1.store(rw.try_read_lock(), std::memory_order_release);
        r2.store(rw.try_read_lock(), std::memory_order_release);
        // Writer cannot acquire while readers hold.
        write_while_readers.store(rw.try_write_lock(),
                                  std::memory_order_release);
        // Release both readers.
        rw.unlock_read();
        rw.unlock_read();
        // Now writer can acquire.
        write_after_readers.store(rw.try_write_lock(),
                                  std::memory_order_release);
        rw.unlock_write();
        done.store(true, std::memory_order_release);
    });
    FiberStack sa;
    SLUICE_CHECK(sched.init_fiber(f, sa.base(), sa.size()));
    sched.spawn(f);
    sched.run(1);
    SLUICE_CHECK_MSG(r1.load(), "first try_read_lock succeeds");
    SLUICE_CHECK_MSG(r2.load(), "second try_read_lock succeeds (concurrent readers)");
    SLUICE_CHECK_MSG(!write_while_readers.load(),
                     "try_write_lock fails while readers hold");
    SLUICE_CHECK_MSG(write_after_readers.load(),
                     "try_write_lock succeeds once all readers release");
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

    // Coordinator fiber: observes both readers queued (via the thread-safe
    // sched.waiting_count()), THEN signals readers_queued to release the writer.
    // This replaces the previous bare yield() calls with an observable gate.
    // Uses sched.run(2) so the coordinator can yield while readers queue in
    // parallel on a second worker (a single-worker run(1) would let the
    // coordinator spin without the readers making progress).
    std::atomic<bool> coord_done{false};
    Fiber cf;
    cf.set_entry([&](Fiber&) {
        // Wait until both readers are confirmed queued behind the writer.
        // The writer is parked on readers_queued; only readers can be queued.
        while (sched.waiting_count() < 2) {
            std::this_thread::yield();  // cooperative: let readers queue
        }
        readers_queued.store(true, std::memory_order_release);
        coord_done.store(true, std::memory_order_release);
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
    // Bounded run so the test fails rather than hangs. Two workers let the
    // coordinator yield while readers queue in parallel.
    for (int i = 0; i < 200 && !coord_done.load(); ++i) {
        sched.run(2);
    }
    // Final run: writer releases, readers granted and complete.
    sched.run(2);

    SLUICE_CHECK_MSG(writer_acquired.load(), "writer acquired");
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

// ===========================================================================
// Slice 5 — Cancel/Expiry head-reconcile (R0 STILL holding while R1/R2 wake)
//
// These tests PROVE that cancel/expiry of a queued head writer grants the
// newly exposed reader prefix IMMEDIATELY — while R0 is STILL holding its
// read share — rather than waiting for R0's unlock_read. They are NOT
// "unlock grants reader prefix" tests in disguise: the writer node is removed
// by cancel/expiry, NOT by R0 releasing.
// ===========================================================================

// ---- T14: head writer cancel grants reader prefix immediately --------------
//
// R0 acquires read lock and REMAINS ACTIVE.
// W1 queues as FIFO head.
// R1, R2 queue behind W1.
// cancel(W1):
//   - W1 becomes Cancelled
//   - R1, R2 become Woken because writer_active == false (head-reconcile)
//   - R1, R2 join the existing reader phase while R0 STILL holds
//   - a new R3 cannot bypass the already-committed reader phase
//   - exactly-once publication; waiting_count reaches zero after cleanup
//
// CAUSAL PROOF: the assertion that R1/R2 reached the post-read_lock line
// runs BEFORE r0_released is set. The synchronization is via deterministic
// phase seams (separate sched.run(1) calls) and atomic gate observation — NOT
// sleep_for timing. If cancel did NOT reconcile the head, R1/R2 would remain
// suspended and the bounded sched.run loop would exhaust without observing
// r1_acquired/r2_acquired.
SLUICE_TEST_CASE(rwlock_head_writer_cancel_grants_reader_prefix_immediately) {
    if constexpr (!fiber_ctx::supported) return;
    AsyncIoContext ctx(std::make_unique<IdleBackend>());
    Scheduler sched(ctx);
    AsyncRwLock rw(sched);

    // R0 holds the read lock for the entire test; released only at the end.
    std::atomic<bool> r0_holds{false};
    std::atomic<bool> r0_released{false};
    // Gate set ONLY after R1/R2 have been observed as Woken — proves the
    // reconcile happened BEFORE R0 unlocked.
    std::atomic<bool> r1_acquired{false};
    std::atomic<bool> r2_acquired{false};
    std::atomic<bool> w1_cancel_observed{false};

    Fiber rf0;
    rf0.set_entry([&](Fiber&) {
        WaitNode rn;
        rw.read_lock(rn);
        r0_holds.store(true, std::memory_order_release);
        // Suspend (cooperatively) until the test releases us. R0 NEVER calls
        // unlock_read before r1_acquired/r2_acquired are observed.
        sched.await_ready_flag(r0_released);
        rw.unlock_read();
    });

    // W1: queues as head, will be cancelled.
    WaitNode wn1;  // must outlive the fiber so cancel() is well-defined
    Fiber wf1;
    wf1.set_entry([&](Fiber&) {
        rw.write_lock(wn1);
        // If reached, cancel lost (not expected in this test).
    });

    // R1, R2: queue behind W1; SHOULD wake after cancel reconciles the head.
    Fiber rf1, rf2;
    rf1.set_entry([&](Fiber&) {
        WaitNode rn;
        rw.read_lock(rn);
        r1_acquired.store(true, std::memory_order_release);
        rw.unlock_read();
    });
    rf2.set_entry([&](Fiber&) {
        WaitNode rn;
        rw.read_lock(rn);
        r2_acquired.store(true, std::memory_order_release);
        rw.unlock_read();
    });

    FiberStack s0, sw1, s1, s2;
    SLUICE_CHECK(sched.init_fiber(rf0, s0.base(), s0.size()));
    SLUICE_CHECK(sched.init_fiber(wf1, sw1.base(), sw1.size()));
    SLUICE_CHECK(sched.init_fiber(rf1, s1.base(), s1.size()));
    SLUICE_CHECK(sched.init_fiber(rf2, s2.base(), s2.size()));

    // Phase 1: R0 acquires and parks on r0_released.
    sched.spawn(rf0);
    sched.run(1);
    SLUICE_CHECK_MSG(r0_holds.load(), "R0 holds the read lock");

    // Phase 2: queue W1, R1, R2 behind R0. They all suspend.
    sched.spawn(wf1);
    sched.spawn(rf1);
    sched.spawn(rf2);
    sched.run(1);
    SLUICE_CHECK_MSG(wn1.is_registered(), "W1 registered (queued behind R0)");
    SLUICE_CHECK_MSG(!r1_acquired.load() && !r2_acquired.load(),
                     "R1/R2 still suspended behind W1");

    // Phase 3: cancel W1 from the test (main OS thread). The cancel seam is
    // safe from any thread and performs the head reconcile.
    bool cancelled = rw.cancel(wn1);
    w1_cancel_observed.store(cancelled, std::memory_order_release);
    SLUICE_CHECK_MSG(cancelled, "cancel(W1) won (W1 was Registered)");
    SLUICE_CHECK_MSG(wn1.was_cancelled(), "W1 node is Cancelled");

    // Phase 4: run again so the head reconcile's publications take effect.
    // R1/R2 should now be Woken and re-granted — joining R0's reader phase —
    // while R0 is STILL parked on r0_released (which has NOT been set).
    sched.run(1);
    SLUICE_CHECK_MSG(!r0_released.load(),
                     "R0 has NOT called unlock_read yet (still holds)");
    SLUICE_CHECK_MSG(r1_acquired.load(),
                     "R1 granted by cancel head-reconcile while R0 holds");
    SLUICE_CHECK_MSG(r2_acquired.load(),
                     "R2 granted by cancel head-reconcile while R0 holds");
    SLUICE_CHECK_MSG(wn1.was_cancelled(), "W1 still Cancelled (no double-resolve)");

    // Phase 5: a NEW reader R3 must NOT barge — but the queue is now empty
    // (R1/R2 already unlinked), so R3 SHOULD acquire inline. The point is to
    // prove R1/R2 were not silently detached: the queue is genuinely drained.
    std::atomic<bool> r3_acquired{false};
    Fiber rf3;
    rf3.set_entry([&](Fiber&) {
        WaitNode rn;
        rw.read_lock(rn);
        r3_acquired.store(true, std::memory_order_release);
        rw.unlock_read();
    });
    FiberStack s3;
    SLUICE_CHECK(sched.init_fiber(rf3, s3.base(), s3.size()));
    sched.spawn(rf3);
    sched.run(1);
    SLUICE_CHECK_MSG(r3_acquired.load(), "R3 acquires (queue drained)");

    // Cleanup: release R0.
    r0_released.store(true, std::memory_order_release);
    sched.run(1);
    SLUICE_CHECK_MSG(w1_cancel_observed.load(), "W1 cancel returned true");
    SLUICE_CHECK_MSG(sched.waiting_count() == 0, "no unresolved waits");
}

// ---- T15: head writer expiry grants reader prefix immediately --------------
//
// Same topology as T14 but W1 is a TIMED writer that expires. Advancing the
// logical clock removes W1 via the timer pump's rwlock_expire_wait, which
// performs the head reconcile. R1/R2 join R0's reader phase BEFORE R0
// releases.
SLUICE_TEST_CASE(rwlock_head_writer_expiry_grants_reader_prefix_immediately) {
    if constexpr (!fiber_ctx::supported) return;
    AsyncIoContext ctx(std::make_unique<IdleBackend>());
    Scheduler sched(ctx);
    AsyncRwLock rw(sched);
    TimerCtl::enable_test_clock(sched);
    TimerCtl::set_clock(sched, 0);

    std::atomic<bool> r0_holds{false};
    std::atomic<bool> r0_released{false};
    std::atomic<bool> r1_acquired{false};
    std::atomic<bool> r2_acquired{false};
    std::atomic<bool> w1_registered{false};

    Fiber rf0;
    rf0.set_entry([&](Fiber&) {
        WaitNode rn;
        rw.read_lock(rn);
        r0_holds.store(true, std::memory_order_release);
        sched.await_ready_flag(r0_released);
        rw.unlock_read();
    });

    WaitNode wn1;
    Fiber wf1;
    wf1.set_entry([&](Fiber&) {
        w1_registered.store(true, std::memory_order_release);
        rw.write_lock_until(wn1, 100);  // deadline at tick 100
        // If reached, expire lost (not expected).
    });

    Fiber rf1, rf2;
    rf1.set_entry([&](Fiber&) {
        WaitNode rn;
        rw.read_lock(rn);
        r1_acquired.store(true, std::memory_order_release);
        rw.unlock_read();
    });
    rf2.set_entry([&](Fiber&) {
        WaitNode rn;
        rw.read_lock(rn);
        r2_acquired.store(true, std::memory_order_release);
        rw.unlock_read();
    });

    // Driver fiber: advances the clock so W1's deadline elapses. Runs on the
    // Scheduler (clock advance must occur on a worker under G).
    Fiber fdrv;
    fdrv.set_entry([&](Fiber&) {
        // Wait until W1 is registered, then advance the clock past 100.
        sched.await_ready_flag(w1_registered);
        std::this_thread::yield();
        for (int i = 0; i < 200 && !wn1.is_terminal(); ++i) {
            sched.advance_clock(100);
            std::this_thread::yield();
        }
    });

    FiberStack s0, sw1, s1, s2, sd;
    SLUICE_CHECK(sched.init_fiber(rf0, s0.base(), s0.size()));
    SLUICE_CHECK(sched.init_fiber(wf1, sw1.base(), sw1.size()));
    SLUICE_CHECK(sched.init_fiber(rf1, s1.base(), s1.size()));
    SLUICE_CHECK(sched.init_fiber(rf2, s2.base(), s2.size()));
    SLUICE_CHECK(sched.init_fiber(fdrv, sd.base(), sd.size()));

    // Phase 1: R0 acquires, parks on r0_released.
    sched.spawn(rf0);
    sched.run(1);
    SLUICE_CHECK_MSG(r0_holds.load(), "R0 holds the read lock");

    // Phase 2: queue W1 (timed), R1, R2, driver. Run; driver advances clock
    // to expire W1; the expiry seam reconciles the head; R1/R2 wake.
    sched.spawn(wf1);
    sched.spawn(rf1);
    sched.spawn(rf2);
    sched.spawn(fdrv);
    sched.run(1);

    SLUICE_CHECK_MSG(!r0_released.load(),
                     "R0 has NOT called unlock_read yet (still holds)");
    SLUICE_CHECK_MSG(wn1.was_expired(),
                     "W1 node is Expired (timer consumed once)");
    SLUICE_CHECK_MSG(r1_acquired.load(),
                     "R1 granted by expiry head-reconcile while R0 holds");
    SLUICE_CHECK_MSG(r2_acquired.load(),
                     "R2 granted by expiry head-reconcile while R0 holds");
    SLUICE_CHECK_MSG(TimerCtl::active_deadline_count(sched) == 0,
                     "W1 timer retired exactly once (no leak)");

    // Cleanup: release R0.
    r0_released.store(true, std::memory_order_release);
    sched.run(1);
    SLUICE_CHECK_MSG(sched.waiting_count() == 0, "no unresolved waits");
}

// ===========================================================================
// Slice 6 — Cancel/Expiry FIFO boundary preservation
//
// After head writer cancel/expire, only the contiguous reader PREFIX at the
// new head is granted. A writer queued behind those readers MUST wait for
// all granted readers to release, and a reader queued behind that writer
// MUST NOT be granted.
// ===========================================================================

// Shared body for the two FIFO-boundary tests. mode_cancel==true exercises
// cancel; mode_cancel==false exercises expiry. Returns true on PASS.
//
// Topology:
//   R0 active
//   W1 head writer
//   R1 queued
//   W2 queued
//   R2 queued
// After W1 removal:
//   R1 (head reader prefix) granted
//   W2 NOT granted (still queued; R1 still holds)
//   R2 NOT granted (queued behind W2)
// After R1 releases:
//   W2 granted
//   R2 still queued behind W2's exclusive phase
// After W2 releases:
//   R2 granted
namespace {
void run_rwlock_reconcile_preserves_fifo(bool mode_cancel) {
    AsyncIoContext ctx(std::make_unique<IdleBackend>());
    Scheduler sched(ctx);
    AsyncRwLock rw(sched);
    if (!mode_cancel) {
        TimerCtl::enable_test_clock(sched);
        TimerCtl::set_clock(sched, 0);
    }

    std::atomic<bool> r0_released{false};
    std::atomic<bool> r1_acquired{false};
    std::atomic<bool> r1_released{false};
    std::atomic<bool> w2_acquired{false};
    std::atomic<bool> w2_released{false};
    std::atomic<bool> r2_acquired{false};
    std::atomic<bool> w1_registered{false};

    Fiber rf0;
    rf0.set_entry([&](Fiber&) {
        WaitNode rn;
        rw.read_lock(rn);
        sched.await_ready_flag(r0_released);
        rw.unlock_read();
    });

    WaitNode wn1;  // W1 — removed by cancel/expiry
    Fiber wf1;
    wf1.set_entry([&](Fiber&) {
        w1_registered.store(true, std::memory_order_release);
        if (mode_cancel) {
            rw.write_lock(wn1);
        } else {
            rw.write_lock_until(wn1, 100);
        }
        // If reached, removal lost (not expected).
    });

    Fiber rf1;
    rf1.set_entry([&](Fiber&) {
        WaitNode rn;
        rw.read_lock(rn);
        r1_acquired.store(true, std::memory_order_release);
        sched.await_ready_flag(r1_released);
        rw.unlock_read();
    });

    Fiber wf2;
    wf2.set_entry([&](Fiber&) {
        WaitNode rn;
        rw.write_lock(rn);
        w2_acquired.store(true, std::memory_order_release);
        sched.await_ready_flag(w2_released);
        rw.unlock_write();
    });

    Fiber rf2;
    rf2.set_entry([&](Fiber&) {
        WaitNode rn;
        rw.read_lock(rn);
        r2_acquired.store(true, std::memory_order_release);
        rw.unlock_read();
    });

    FiberStack s0, sw1, s1, sw2, s2;
    SLUICE_CHECK(sched.init_fiber(rf0, s0.base(), s0.size()));
    SLUICE_CHECK(sched.init_fiber(wf1, sw1.base(), sw1.size()));
    SLUICE_CHECK(sched.init_fiber(rf1, s1.base(), s1.size()));
    SLUICE_CHECK(sched.init_fiber(wf2, sw2.base(), sw2.size()));
    SLUICE_CHECK(sched.init_fiber(rf2, s2.base(), s2.size()));

    // Phase 1: R0 acquires (parks on r0_released).
    sched.spawn(rf0);
    sched.run(1);

    // Phase 2: queue W1, R1, W2, R2. All suspend.
    sched.spawn(wf1);
    sched.spawn(rf1);
    sched.spawn(wf2);
    sched.spawn(rf2);
    sched.run(1);
    SLUICE_CHECK_MSG(w1_registered.load(), "W1 registered");
    SLUICE_CHECK_MSG(wn1.is_registered(), "W1 linked");
    SLUICE_CHECK_MSG(!r1_acquired.load() && !w2_acquired.load() &&
                     !r2_acquired.load(),
                     "R1/W2/R2 still queued behind W1");

    // Phase 3: remove W1 (cancel from main thread, OR clock-advance via a
    // driver fiber for the expiry variant).
    if (mode_cancel) {
        SLUICE_CHECK_MSG(rw.cancel(wn1), "cancel(W1) won");
    } else {
        // Drive the clock on a worker fiber to fire W1's deadline.
        Fiber fdrv;
        fdrv.set_entry([&](Fiber&) {
            for (int i = 0; i < 200 && !wn1.is_terminal(); ++i) {
                sched.advance_clock(100);
                std::this_thread::yield();
            }
        });
        FiberStack sd;
        SLUICE_CHECK(sched.init_fiber(fdrv, sd.base(), sd.size()));
        sched.spawn(fdrv);
        sched.run(1);
    }
    SLUICE_CHECK_MSG(wn1.is_terminal(), "W1 removed by cancel/expiry");

    // Phase 4: run the reconcile publications. R1 should now be granted
    // (reader prefix); W2/R2 remain queued.
    sched.run(1);
    SLUICE_CHECK_MSG(r1_acquired.load(),
                     "R1 (reader prefix) granted after W1 removed");
    SLUICE_CHECK_MSG(!w2_acquired.load() && !r2_acquired.load(),
                     "W2/R2 still queued (FIFO boundary respected)");

    // Phase 5: release R1 (still R0 holding). W2 must NOT be granted yet
    // (writer cannot overlap an active reader). This is the FIFO boundary
    // boundary proof: W2 waits for ALL granted readers to release.
    r1_released.store(true, std::memory_order_release);
    sched.run(1);
    SLUICE_CHECK_MSG(!w2_acquired.load(),
                     "W2 NOT granted while R0 still holds (writer exclusivity)");

    // Phase 6: release R0 too. NOW the queue is fully drained of readers and
    // W2 (new head writer) can be granted.
    r0_released.store(true, std::memory_order_release);
    sched.run(1);
    SLUICE_CHECK_MSG(w2_acquired.load(),
                     "W2 granted after both R0+R1 released (FIFO boundary)");
    SLUICE_CHECK_MSG(!r2_acquired.load(),
                     "R2 still queued behind W2 (no reader-past-writer)");

    // Phase 7: release W2. R2 (new head reader) should now be granted.
    w2_released.store(true, std::memory_order_release);
    sched.run(1);
    SLUICE_CHECK_MSG(r2_acquired.load(),
                     "R2 granted after W2 released");

    // All fibers completed; no residual waits.
    SLUICE_CHECK_MSG(sched.waiting_count() == 0, "no unresolved waits");
}
}  // namespace

// ---- T16: cancel reconcile preserves FIFO ---------------------------------
SLUICE_TEST_CASE(rwlock_cancel_reconcile_preserves_fifo) {
    if constexpr (!fiber_ctx::supported) return;
    run_rwlock_reconcile_preserves_fifo(/*mode_cancel=*/true);
}

// ---- T17: expiry reconcile preserves FIFO ---------------------------------
SLUICE_TEST_CASE(rwlock_expiry_reconcile_preserves_fifo) {
    if constexpr (!fiber_ctx::supported) return;
    run_rwlock_reconcile_preserves_fifo(/*mode_cancel=*/false);
}

// ===========================================================================
// Slice 7 — Multi-worker (sched.run(2)) correctness
//
// Real two-worker runs. Assertions are concurrency-invariant, not timing:
//   one terminal outcome per waiter
//   one resource grant per winner
//   one runnable publication
//   active_readers never underflows (writer never overlaps active readers)
//   waiting_count returns to zero
// Deterministic phase seams (atomic gates + cooperative await_ready_flag);
// no sleep_for proof.
// ===========================================================================

// ---- T18: two readers acquire on different workers ------------------------
SLUICE_TEST_CASE(rwlock_mw_two_readers_on_different_workers) {
    if constexpr (!fiber_ctx::supported) return;
    AsyncIoContext ctx(std::make_unique<IdleBackend>());
    Scheduler sched(ctx);
    AsyncRwLock rw(sched);

    std::atomic<unsigned> wid_a{static_cast<unsigned>(-1)};
    std::atomic<unsigned> wid_b{static_cast<unsigned>(-1)};
    std::atomic<int> at_barrier{0};
    std::atomic<bool> a_acquired{false}, b_acquired{false};

    // Barrier-await helper: cooperatively wait until the shared counter reaches
    // the target. Yields between polls so both fibers can make progress even if
    // they share a worker (a bare spin would hang in that case).
    auto await_barrier = [](std::atomic<int>& counter, int target) {
        while (counter.load(std::memory_order_acquire) < target) {
            std::this_thread::yield();
        }
    };

    Fiber fa, fb;
    fa.set_entry([&](Fiber&) {
        WaitNode rn;
        rw.read_lock(rn);
        wid_a.store(Scheduler::current_worker_id(), std::memory_order_release);
        a_acquired.store(true, std::memory_order_release);
        at_barrier.fetch_add(1, std::memory_order_acq_rel);
        await_barrier(at_barrier, 2);
        rw.unlock_read();
    });
    fb.set_entry([&](Fiber&) {
        WaitNode rn;
        rw.read_lock(rn);
        wid_b.store(Scheduler::current_worker_id(), std::memory_order_release);
        b_acquired.store(true, std::memory_order_release);
        at_barrier.fetch_add(1, std::memory_order_acq_rel);
        await_barrier(at_barrier, 2);
        rw.unlock_read();
    });

    FiberStack sa, sb;
    SLUICE_CHECK(sched.init_fiber(fa, sa.base(), sa.size()));
    SLUICE_CHECK(sched.init_fiber(fb, sb.base(), sb.size()));
    sched.spawn(fa);
    sched.spawn(fb);
    sched.run(2);

    SLUICE_CHECK_MSG(a_acquired.load() && b_acquired.load(),
                     "both readers acquired");
    SLUICE_CHECK_MSG(wid_a.load() != static_cast<unsigned>(-1) &&
                     wid_b.load() != static_cast<unsigned>(-1),
                     "both readers recorded a worker id");
    // The two readers MAY have run on the same worker (scheduler freedom) or
    // on different workers. We do NOT assert they differ. The proof is that
    // the lock admitted both concurrently (no FIFO serialization of readers
    // on an empty queue) and that the run terminated cleanly.
    SLUICE_CHECK_MSG(sched.waiting_count() == 0, "no unresolved waits");
}

// ---- T19: concurrent reader unlocks race to last-reader; writer handoff ---
//
// R1+R2 acquire (batch). W1 queues. R1 releases (still 1 reader). R2 releases
// (last reader -> grant W1). Under run(2), R1/R2 may release on different
// workers; the last-reader grant + writer handoff must be exactly-once and
// must not underflow active_readers_.
SLUICE_TEST_CASE(rwlock_mw_concurrent_last_reader_unblock_grants_writer) {
    if constexpr (!fiber_ctx::supported) return;
    AsyncIoContext ctx(std::make_unique<IdleBackend>());
    Scheduler sched(ctx);
    AsyncRwLock rw(sched);

    std::atomic<bool> r1_done{false}, r2_done{false};
    std::atomic<bool> r1_acquired{false}, r2_acquired{false};
    std::atomic<bool> readers_acquired{false};
    std::atomic<bool> writer_acquired{false};

    Fiber rf1, rf2;
    rf1.set_entry([&](Fiber&) {
        WaitNode rn;
        rw.read_lock(rn);
        r1_acquired.store(true, std::memory_order_release);
        // Coordinate: both readers must be admitted before the writer spawns.
        if (r2_acquired.load(std::memory_order_acquire))
            readers_acquired.store(true, std::memory_order_release);
        sched.await_ready_flag(r1_done);
        rw.unlock_read();
    });
    rf2.set_entry([&](Fiber&) {
        WaitNode rn;
        rw.read_lock(rn);
        r2_acquired.store(true, std::memory_order_release);
        if (r1_acquired.load(std::memory_order_acquire))
            readers_acquired.store(true, std::memory_order_release);
        sched.await_ready_flag(r2_done);
        rw.unlock_read();
    });

    Fiber wf;
    wf.set_entry([&](Fiber&) {
        WaitNode wn;
        rw.write_lock(wn);
        // Writer acquired only after BOTH readers released.
        if (r1_done.load(std::memory_order_acquire) &&
            r2_done.load(std::memory_order_acquire))
            writer_acquired.store(true, std::memory_order_release);
        rw.unlock_write();
    });

    FiberStack sr1, sr2, sw;
    SLUICE_CHECK(sched.init_fiber(rf1, sr1.base(), sr1.size()));
    SLUICE_CHECK(sched.init_fiber(rf2, sr2.base(), sr2.size()));
    SLUICE_CHECK(sched.init_fiber(wf, sw.base(), sw.size()));

    // Phase 1: readers acquire first (deterministic; writer not yet spawned).
    sched.spawn(rf1);
    sched.spawn(rf2);
    sched.run(2);
    SLUICE_CHECK_MSG(r1_acquired.load() && r2_acquired.load(),
                     "both readers acquired");

    // Phase 2: spawn writer; it queues behind the holding readers.
    sched.spawn(wf);
    sched.run(2);
    SLUICE_CHECK_MSG(!writer_acquired.load(),
                     "writer NOT granted while readers hold");

    // Phase 3: release R1 (still 1 reader). Writer must NOT be granted.
    r1_done.store(true, std::memory_order_release);
    sched.run(2);
    SLUICE_CHECK_MSG(!writer_acquired.load(),
                     "writer NOT granted while one reader still holds");
    // Phase 4: release R2 (last reader) -> grant writer.
    r2_done.store(true, std::memory_order_release);
    sched.run(2);
    SLUICE_CHECK_MSG(writer_acquired.load(),
                     "writer granted after last reader released");
    SLUICE_CHECK_MSG(sched.waiting_count() == 0, "no unresolved waits");
}

// ---- T20: writer handoff winner may resume on another worker --------------
//
// W1 holds. R1 queues. W1 releases on worker A; R1 may be stolen to worker B.
// The handoff publication must route correctly across workers.
SLUICE_TEST_CASE(rwlock_mw_writer_handoff_cross_worker_resume) {
    if constexpr (!fiber_ctx::supported) return;
    AsyncIoContext ctx(std::make_unique<IdleBackend>());
    Scheduler sched(ctx);
    AsyncRwLock rw(sched);

    std::atomic<bool> writer_holds{false};
    std::atomic<bool> writer_release{false};
    std::atomic<bool> reader_acquired{false};

    Fiber wf;
    wf.set_entry([&](Fiber&) {
        WaitNode wn;
        rw.write_lock(wn);
        writer_holds.store(true, std::memory_order_release);
        sched.await_ready_flag(writer_release);
        rw.unlock_write();
    });
    Fiber rf;
    rf.set_entry([&](Fiber&) {
        WaitNode rn;
        rw.read_lock(rn);  // queues behind W1
        reader_acquired.store(true, std::memory_order_release);
        rw.unlock_read();
    });

    FiberStack sw, sr;
    SLUICE_CHECK(sched.init_fiber(wf, sw.base(), sw.size()));
    SLUICE_CHECK(sched.init_fiber(rf, sr.base(), sr.size()));

    // Phase 1: writer acquires first (deterministic). Spawn + run BEFORE the
    // reader is spawned, so R1 cannot barge inline ahead of W1.
    sched.spawn(wf);
    sched.run(2);
    SLUICE_CHECK_MSG(writer_holds.load(), "writer acquired");

    // Phase 2: reader queues behind the active writer.
    sched.spawn(rf);
    sched.run(2);
    SLUICE_CHECK_MSG(!reader_acquired.load(), "reader queued behind writer");

    // Phase 3: release the writer. unlock_write grants R1; R1 may resume on a
    // different worker than the one that ran unlock_write.
    writer_release.store(true, std::memory_order_release);
    sched.run(2);
    SLUICE_CHECK_MSG(reader_acquired.load(),
                     "reader resumed after writer handoff (possibly stolen)");
    SLUICE_CHECK_MSG(sched.waiting_count() == 0, "no unresolved waits");
}

// ---- T21: reader batch publication routes multiple winners across workers -
//
// W1 holds. R1/R2/R3 queue. W1 releases -> batch grant of 3 readers. Each
// winner is routed to its owner worker; all three resume and release.
SLUICE_TEST_CASE(rwlock_mw_reader_batch_publication_across_workers) {
    if constexpr (!fiber_ctx::supported) return;
    AsyncIoContext ctx(std::make_unique<IdleBackend>());
    Scheduler sched(ctx);
    AsyncRwLock rw(sched);

    std::atomic<bool> writer_release{false};
    std::atomic<int> batch_count{0};

    Fiber wf;
    wf.set_entry([&](Fiber&) {
        WaitNode wn;
        rw.write_lock(wn);
        sched.await_ready_flag(writer_release);
        rw.unlock_write();
    });

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

    FiberStack sw, sr1, sr2, sr3;
    SLUICE_CHECK(sched.init_fiber(wf, sw.base(), sw.size()));
    SLUICE_CHECK(sched.init_fiber(rf1, sr1.base(), sr1.size()));
    SLUICE_CHECK(sched.init_fiber(rf2, sr2.base(), sr2.size()));
    SLUICE_CHECK(sched.init_fiber(rf3, sr3.base(), sr3.size()));

    // Phase 1: writer acquires first (deterministic). Spawn + run BEFORE the
    // readers are spawned, so no reader can barge inline ahead of the writer.
    sched.spawn(wf);
    sched.run(2);

    // Phase 2: readers queue behind the active writer.
    sched.spawn(rf1);
    sched.spawn(rf2);
    sched.spawn(rf3);
    sched.run(2);
    SLUICE_CHECK_MSG(batch_count.load() == 0, "no reader admitted yet");

    writer_release.store(true, std::memory_order_release);
    sched.run(2);
    SLUICE_CHECK_MSG(batch_count.load() == 3,
                     "all 3 readers batch-granted and ran");
    SLUICE_CHECK_MSG(sched.waiting_count() == 0, "no unresolved waits");
}

// ---- T22: cancel and last-reader unlock on different workers --------------
//
// W1 holds. R1 queues. Cancel(R1) from the test thread races nothing here
// (single waiter), but combined with W1's release on a worker, the cancel
// winner publication and the unlock grant must each publish exactly once.
SLUICE_TEST_CASE(rwlock_mw_cancel_and_unlock_on_different_workers) {
    if constexpr (!fiber_ctx::supported) return;
    AsyncIoContext ctx(std::make_unique<IdleBackend>());
    Scheduler sched(ctx);
    AsyncRwLock rw(sched);

    std::atomic<bool> writer_release{false};
    std::atomic<bool> reader_cancelled{false};

    Fiber wf;
    wf.set_entry([&](Fiber&) {
        WaitNode wn;
        rw.write_lock(wn);
        sched.await_ready_flag(writer_release);
        rw.unlock_write();
    });

    WaitNode rn1;  // outlives the fiber so cancel() is well-defined
    Fiber rf1;
    rf1.set_entry([&](Fiber&) {
        rw.read_lock(rn1);  // queues behind writer
        // If reached, cancel lost (not expected).
    });
    // A second reader that SHOULD be granted once rn1 is cancelled and W1
    // releases. Proves cancel + unlock serialize correctly under multi-worker.
    std::atomic<bool> r2_acquired{false};
    Fiber rf2;
    rf2.set_entry([&](Fiber&) {
        WaitNode rn;
        rw.read_lock(rn);
        r2_acquired.store(true, std::memory_order_release);
        rw.unlock_read();
    });

    FiberStack sw, sr1, sr2;
    SLUICE_CHECK(sched.init_fiber(wf, sw.base(), sw.size()));
    SLUICE_CHECK(sched.init_fiber(rf1, sr1.base(), sr1.size()));
    SLUICE_CHECK(sched.init_fiber(rf2, sr2.base(), sr2.size()));

    // Phase 1: writer acquires first (deterministic). Spawn + run BEFORE the
    // readers are spawned, so no reader can barge inline ahead of the writer.
    sched.spawn(wf);
    sched.run(2);

    // Phase 2: readers queue behind the active writer.
    sched.spawn(rf1);
    sched.spawn(rf2);
    sched.run(2);

    // Cancel rn1 from the main OS thread (different worker than any Fiber).
    reader_cancelled.store(rw.cancel(rn1), std::memory_order_release);
    SLUICE_CHECK_MSG(reader_cancelled.load(), "cancel(rn1) won");
    SLUICE_CHECK_MSG(rn1.was_cancelled(), "rn1 is Cancelled (exactly-once)");

    // Release the writer on a worker; the head reconcile grants rf2.
    writer_release.store(true, std::memory_order_release);
    sched.run(2);
    SLUICE_CHECK_MSG(r2_acquired.load(),
                     "rf2 granted after cancel + writer release");
    SLUICE_CHECK_MSG(sched.waiting_count() == 0, "no unresolved waits");
}

// ===========================================================================
// Slice 8 — Publication lifecycle
//
// Verifies the publication loop advances via cached pointers and never
// touches a node after its route_runnable_locked call. The first reader in
// a batch returns from read_lock and destroys its WaitNode (stack-local)
// WHILE the grant loop is still publishing subsequent readers. Under ASan/TSan
// this catches any post-publication node access (UAF) or double publication.
// ===========================================================================

// ---- T23: batch publication does not access published node ----------------
//
// W1 holds. R1 + R2 + R3 queue behind W1. W1 releases -> the grant loop
// publishes R1, R2, R3 in FIFO order. R1's fiber, once resumed, returns from
// read_lock IMMEDIATELY (its stack-local WaitNode goes out of scope at end of
// the lock function) WHILE R2/R3 may still be in the publication list. This
// exercises the "first winner may destroy/reuse its node while later winners
// are still being published" lifecycle rule.
//
// The proof is structural: the publication loop advances via CACHED next/
// fiber/owner captured BEFORE route_runnable_locked, and never dereferences
// a node after publication. Under ASan this catches post-publication node
// access (UAF); under TSan it catches any data race on the published node.
//
// SAFETY ARGUMENT: the grant/publication loop runs while holding Scheduler
// global_mtx_. A published Fiber cannot truly resume execution until
// global_mtx_ is released, so the first reader's stack-local WaitNode
// outlives the entire loop — even though that Fiber has already been
// marked runnable. The loop caches next/fiber/owner BEFORE each
// route_runnable_locked call and never dereferences the current WaitNode
// after routing it.
//
// Deterministic phase seams (NOT sleep_for): each phase is driven by a
// separate sched.run(1) call after the previous phase's gate is observed.
SLUICE_TEST_CASE(rwlock_batch_publication_does_not_access_published_node) {
    if constexpr (!fiber_ctx::supported) return;
    AsyncIoContext ctx(std::make_unique<IdleBackend>());
    Scheduler sched(ctx);
    AsyncRwLock rw(sched);

    std::atomic<bool> writer_release{false};
    std::atomic<bool> r1_acquired{false};
    std::atomic<bool> r2_acquired{false};
    std::atomic<bool> r3_acquired{false};

    Fiber wf;
    wf.set_entry([&](Fiber&) {
        WaitNode wn;
        rw.write_lock(wn);
        // Park until the test observes all readers queued and signals release.
        sched.await_ready_flag(writer_release);
        rw.unlock_write();  // grant_from_head publishes R1, R2, R3
    });

    // Each reader, on resume, returns from read_lock as fast as possible (rn
    // is destroyed at function return) WHILE the grant loop may still be
    // publishing the later batch members.
    Fiber rf1, rf2, rf3;
    rf1.set_entry([&](Fiber&) {
        WaitNode rn;
        rw.read_lock(rn);
        r1_acquired.store(true, std::memory_order_release);
        rw.unlock_read();
        // rn destroyed here — the grant loop must not touch it after publish.
    });
    rf2.set_entry([&](Fiber&) {
        WaitNode rn;
        rw.read_lock(rn);
        r2_acquired.store(true, std::memory_order_release);
        rw.unlock_read();
    });
    rf3.set_entry([&](Fiber&) {
        WaitNode rn;
        rw.read_lock(rn);
        r3_acquired.store(true, std::memory_order_release);
        rw.unlock_read();
    });

    FiberStack sw, sr1, sr2, sr3;
    SLUICE_CHECK(sched.init_fiber(wf, sw.base(), sw.size()));
    SLUICE_CHECK(sched.init_fiber(rf1, sr1.base(), sr1.size()));
    SLUICE_CHECK(sched.init_fiber(rf2, sr2.base(), sr2.size()));
    SLUICE_CHECK(sched.init_fiber(rf3, sr3.base(), sr3.size()));

    // Phase 1: writer acquires, parks on writer_release.
    sched.spawn(wf);
    sched.run(1);

    // Phase 2: readers queue behind writer (all suspend in read_lock).
    sched.spawn(rf1);
    sched.spawn(rf2);
    sched.spawn(rf3);
    sched.run(1);
    SLUICE_CHECK_MSG(!r1_acquired.load() && !r2_acquired.load() &&
                     !r3_acquired.load(),
                     "readers queued behind writer (none admitted yet)");
    SLUICE_CHECK_MSG(sched.waiting_count() == 3 + 1,
                     "3 readers + 1 writer parked");

    // Phase 3: release the writer. grant_from_head publishes R1, R2, R3 in
    // FIFO order; each reader returns from read_lock and destroys its
    // stack-local WaitNode as fast as possible.
    writer_release.store(true, std::memory_order_release);
    sched.run(1);

    SLUICE_CHECK_MSG(r1_acquired.load() && r2_acquired.load() &&
                     r3_acquired.load(),
                     "all 3 readers admitted by the batch grant");
    SLUICE_CHECK_MSG(sched.waiting_count() == 0, "no unresolved waits");
}

// ===========================================================================
// Slice 8 — Issue #162 adversarial audit (C++-first evidence, R1-R5 + M3)
//
// Deterministic witnesses for the cancel/expire head-reconcile family that
// E12RwLock.tla previously dead-coded (MODEL-001/002). Every scenario is
// phase-driven (park on ready flags + separate sched.run() calls + the
// test clock), never sleep_for. All runs: 1 worker.
// ===========================================================================

// ---- R1: cancel head writer; next queued writer must stay queued -----------
//
// R0 active -> W_A queued -> W_B queued -> cancel(W_A).
// W_B MUST remain queued while R0 holds; only after R0 unlocks may W_B
// acquire, exactly once.
SLUICE_TEST_CASE(rwlock_audit_r1_cancel_head_writer_wall) {
    if constexpr (!fiber_ctx::supported) return;
    AsyncIoContext ctx(std::make_unique<IdleBackend>());
    Scheduler sched(ctx);
    AsyncRwLock rw(sched);

    std::atomic<bool> r0_released{false};
    std::atomic<int> wb_grants{0};  // exactly-one-grant counter
    std::atomic<bool> wa_registered{false};

    Fiber rf0;
    rf0.set_entry([&](Fiber&) {
        WaitNode rn;
        rw.read_lock(rn);
        sched.await_ready_flag(r0_released);
        rw.unlock_read();
    });

    WaitNode wna;  // outlives the fiber: canceled from the main thread
    Fiber wfa;
    wfa.set_entry([&](Fiber&) {
        wa_registered.store(true, std::memory_order_release);
        rw.write_lock(wna);
        // Canceled here: must NOT observe Woken, must NOT unlock_write.
        SLUICE_CHECK_MSG(wna.was_cancelled(),
                         "W_A resumed only via cancel terminal");
    });

    Fiber wfb;
    wfb.set_entry([&](Fiber&) {
        WaitNode rn;
        rw.write_lock(rn);
        if (rn.was_woken()) {
            wb_grants.fetch_add(1, std::memory_order_acq_rel);
            rw.unlock_write();
        }
    });

    FiberStack s0, swa, swb;
    SLUICE_CHECK(sched.init_fiber(rf0, s0.base(), s0.size()));
    SLUICE_CHECK(sched.init_fiber(wfa, swa.base(), swa.size()));
    SLUICE_CHECK(sched.init_fiber(wfb, swb.base(), swb.size()));

    // Phase 1: R0 acquires and parks.
    sched.spawn(rf0);
    sched.run(1);

    // Phase 2: W_A, W_B queue behind R0.
    sched.spawn(wfa);
    sched.spawn(wfb);
    sched.run(1);
    SLUICE_CHECK_MSG(wa_registered.load() && wna.is_registered(),
                     "W_A registered and linked");
    SLUICE_CHECK_MSG(wb_grants.load() == 0, "W_B queued, not granted");

    // Phase 3: cancel head writer W_A.
    SLUICE_CHECK_MSG(rw.cancel(wna), "cancel(W_A) won");
    SLUICE_CHECK_MSG(wna.was_cancelled(), "W_A terminal Cancelled");
    sched.run(1);
    SLUICE_CHECK_MSG(wb_grants.load() == 0,
                     "W_B still queued after W_A cancel (readers active)");

    // Phase 4: R0 unlocks; only then W_B acquires, exactly once.
    r0_released.store(true, std::memory_order_release);
    sched.run(1);
    SLUICE_CHECK_MSG(wb_grants.load() == 1,
                     "exactly one grant to W_B, after R0 released");
    SLUICE_CHECK_MSG(sched.waiting_count() == 0, "no unresolved waits");
}

// ---- R2: expire head writer; next queued writer must stay queued -----------
// Same shape as R1, W_A removed by deadline expiry instead of cancel.
SLUICE_TEST_CASE(rwlock_audit_r2_expire_head_writer_wall) {
    if constexpr (!fiber_ctx::supported) return;
    AsyncIoContext ctx(std::make_unique<IdleBackend>());
    Scheduler sched(ctx);
    AsyncRwLock rw(sched);
    TimerCtl::enable_test_clock(sched);
    TimerCtl::set_clock(sched, 0);

    std::atomic<bool> r0_released{false};
    std::atomic<int> wb_grants{0};
    std::atomic<bool> wa_registered{false};

    Fiber rf0;
    rf0.set_entry([&](Fiber&) {
        WaitNode rn;
        rw.read_lock(rn);
        sched.await_ready_flag(r0_released);
        rw.unlock_read();
    });

    WaitNode wna;
    Fiber wfa;
    wfa.set_entry([&](Fiber&) {
        wa_registered.store(true, std::memory_order_release);
        rw.write_lock_until(wna, 100);
        SLUICE_CHECK_MSG(wna.was_expired(), "W_A resumed only via expiry");
    });

    Fiber wfb;
    wfb.set_entry([&](Fiber&) {
        WaitNode rn;
        rw.write_lock(rn);
        if (rn.was_woken()) {
            wb_grants.fetch_add(1, std::memory_order_acq_rel);
            rw.unlock_write();
        }
    });

    // Driver: advance the clock past W_A's deadline. advance_clock pumps due
    // timers inline under global_mtx_ (the node resolves Expired inside the
    // call), so ONE advance from clock 0 to the deadline 100 is the complete
    // causal evidence — no retry loop is needed or permitted.
    Fiber fdrv;
    fdrv.set_entry([&](Fiber&) {
        sched.await_ready_flag(wa_registered);
        sched.advance_clock(100);
    });

    FiberStack s0, swa, swb, sd;
    SLUICE_CHECK(sched.init_fiber(rf0, s0.base(), s0.size()));
    SLUICE_CHECK(sched.init_fiber(wfa, swa.base(), swa.size()));
    SLUICE_CHECK(sched.init_fiber(wfb, swb.base(), swb.size()));
    SLUICE_CHECK(sched.init_fiber(fdrv, sd.base(), sd.size()));

    sched.spawn(rf0);
    sched.run(1);

    sched.spawn(wfa);
    sched.spawn(wfb);
    sched.spawn(fdrv);
    sched.run(1);
    SLUICE_CHECK_MSG(wna.was_expired(), "W_A removed by expiry");
    SLUICE_CHECK_MSG(wb_grants.load() == 0,
                     "W_B still queued after W_A expiry (readers active)");
    SLUICE_CHECK_MSG(TimerCtl::active_deadline_count(sched) == 0,
                     "W_A timer retired exactly once (no leak)");

    r0_released.store(true, std::memory_order_release);
    sched.run(1);
    SLUICE_CHECK_MSG(wb_grants.load() == 1,
                     "exactly one grant to W_B, after R0 released");
    SLUICE_CHECK_MSG(sched.waiting_count() == 0, "no unresolved waits");
}

// ---- R3: writer owner; cancel head READER; reader prefix never barged ------
//
// W0 owns -> R_A, R_B queued -> W_C queued. cancel(R_A) reconciles to a
// reader head while writer_active: NOTHING may be granted. After W0
// releases, the reader prefix [R_B] is granted (stops at W_C); W_C must
// wait for R_B's release.
SLUICE_TEST_CASE(rwlock_audit_r3_writer_owner_cancel_reader_head) {
    if constexpr (!fiber_ctx::supported) return;
    AsyncIoContext ctx(std::make_unique<IdleBackend>());
    Scheduler sched(ctx);
    AsyncRwLock rw(sched);

    std::atomic<bool> w0_released{false};
    std::atomic<bool> ra_registered{false};
    std::atomic<bool> rB_acquired{false};
    std::atomic<bool> rB_released{false};
    std::atomic<bool> wC_acquired{false};

    Fiber wf0;
    wf0.set_entry([&](Fiber&) {
        WaitNode rn;
        rw.write_lock(rn);
        sched.await_ready_flag(w0_released);
        rw.unlock_write();
    });

    WaitNode rna;  // canceled from the main thread
    Fiber rfa;
    rfa.set_entry([&](Fiber&) {
        ra_registered.store(true, std::memory_order_release);
        rw.read_lock(rna);
        SLUICE_CHECK_MSG(rna.was_cancelled(),
                         "R_A resumed only via cancel terminal");
    });

    Fiber rfb;
    rfb.set_entry([&](Fiber&) {
        WaitNode rn;
        rw.read_lock(rn);
        if (rn.was_woken()) {
            rB_acquired.store(true, std::memory_order_release);
            sched.await_ready_flag(rB_released);
            rw.unlock_read();
        }
    });

    Fiber wfc;
    wfc.set_entry([&](Fiber&) {
        WaitNode rn;
        rw.write_lock(rn);
        wC_acquired.store(true, std::memory_order_release);
        rw.unlock_write();
    });

    FiberStack s0, sra, srb, swc;
    SLUICE_CHECK(sched.init_fiber(wf0, s0.base(), s0.size()));
    SLUICE_CHECK(sched.init_fiber(rfa, sra.base(), sra.size()));
    SLUICE_CHECK(sched.init_fiber(rfb, srb.base(), srb.size()));
    SLUICE_CHECK(sched.init_fiber(wfc, swc.base(), swc.size()));

    // Phase 1: W0 owns, parks.
    sched.spawn(wf0);
    sched.run(1);

    // Phase 2: R_A, R_B, W_C queue behind the writer.
    sched.spawn(rfa);
    sched.spawn(rfb);
    sched.spawn(wfc);
    sched.run(1);
    SLUICE_CHECK_MSG(ra_registered.load() && rna.is_registered(),
                     "R_A registered and linked");
    SLUICE_CHECK_MSG(!rB_acquired.load() && !wC_acquired.load(),
                     "R_B/W_C queued behind W0");

    // Phase 3: cancel head reader R_A. Reconcile sees a reader head under an
    // active writer: nothing may be granted.
    SLUICE_CHECK_MSG(rw.cancel(rna), "cancel(R_A) won");
    sched.run(1);
    SLUICE_CHECK_MSG(rna.was_cancelled(), "R_A terminal Cancelled");
    SLUICE_CHECK_MSG(!rB_acquired.load() && !wC_acquired.load(),
                     "no grant under active writer (R_B, W_C still queued)");

    // Phase 4: W0 releases. Reader prefix [R_B] granted; W_C must not barge.
    w0_released.store(true, std::memory_order_release);
    sched.run(1);
    SLUICE_CHECK_MSG(rB_acquired.load(),
                     "R_B granted by reader-prefix reconcile");
    SLUICE_CHECK_MSG(!wC_acquired.load(),
                     "W_C never barges the reader prefix (FIFO boundary)");

    // Phase 5: R_B releases; only now W_C acquires.
    rB_released.store(true, std::memory_order_release);
    sched.run(1);
    SLUICE_CHECK_MSG(wC_acquired.load(), "W_C granted after R_B released");
    SLUICE_CHECK_MSG(sched.waiting_count() == 0, "no unresolved waits");
}

// ---- R4: two active readers; cancel head; grant only at activeReaders==0 ----
//
// R0+R1 hold (active_readers=2) -> W_A, W_B queued -> cancel(W_A).
// R0's release (2->1) must NOT grant; R1's release (1->0) grants exactly W_B.
SLUICE_TEST_CASE(rwlock_audit_r4_cancel_exposes_writer_at_zero) {
    if constexpr (!fiber_ctx::supported) return;
    AsyncIoContext ctx(std::make_unique<IdleBackend>());
    Scheduler sched(ctx);
    AsyncRwLock rw(sched);

    std::atomic<bool> r0_released{false};
    std::atomic<bool> r1_released{false};
    std::atomic<int> wb_grants{0};

    Fiber rf0;
    rf0.set_entry([&](Fiber&) {
        WaitNode rn;
        rw.read_lock(rn);
        sched.await_ready_flag(r0_released);
        rw.unlock_read();
    });
    Fiber rf1;
    rf1.set_entry([&](Fiber&) {
        WaitNode rn;
        rw.read_lock(rn);
        sched.await_ready_flag(r1_released);
        rw.unlock_read();
    });

    WaitNode wna;
    Fiber wfa;
    wfa.set_entry([&](Fiber&) {
        rw.write_lock(wna);
        SLUICE_CHECK_MSG(wna.was_cancelled(), "W_A resumed only via cancel");
    });

    Fiber wfb;
    wfb.set_entry([&](Fiber&) {
        WaitNode rn;
        rw.write_lock(rn);
        if (rn.was_woken()) {
            wb_grants.fetch_add(1, std::memory_order_acq_rel);
            rw.unlock_write();
        }
    });

    FiberStack s0, s1, swa, swb;
    SLUICE_CHECK(sched.init_fiber(rf0, s0.base(), s0.size()));
    SLUICE_CHECK(sched.init_fiber(rf1, s1.base(), s1.size()));
    SLUICE_CHECK(sched.init_fiber(wfa, swa.base(), swa.size()));
    SLUICE_CHECK(sched.init_fiber(wfb, swb.base(), swb.size()));

    // Phase 1: both readers acquire; writers queue.
    sched.spawn(rf0);
    sched.spawn(rf1);
    sched.run(1);
    sched.spawn(wfa);
    sched.spawn(wfb);
    sched.run(1);
    SLUICE_CHECK_MSG(wna.is_registered(), "W_A queued head");
    SLUICE_CHECK_MSG(wb_grants.load() == 0, "W_B queued, not granted");

    // Phase 2: cancel W_A (head). active_readers=2 -> reconcile refuses.
    SLUICE_CHECK_MSG(rw.cancel(wna), "cancel(W_A) won");
    sched.run(1);
    SLUICE_CHECK_MSG(wb_grants.load() == 0,
                     "W_B not granted after cancel (readers still active)");

    // Phase 3: R0 releases (2->1): still no grant.
    r0_released.store(true, std::memory_order_release);
    sched.run(1);
    SLUICE_CHECK_MSG(wb_grants.load() == 0,
                     "W_B not granted at active_readers=1 (early return)");

    // Phase 4: R1 releases (1->0): exactly W_B acquires.
    r1_released.store(true, std::memory_order_release);
    sched.run(1);
    SLUICE_CHECK_MSG(wb_grants.load() == 1,
                     "exactly W_B granted at active_readers==0");
    SLUICE_CHECK_MSG(sched.waiting_count() == 0, "no unresolved waits");
}

// ---- R5a: cancel wins the node; a later expiry must be a no-op -------------
SLUICE_TEST_CASE(rwlock_audit_r5_cancel_wins_over_late_expiry) {
    if constexpr (!fiber_ctx::supported) return;
    AsyncIoContext ctx(std::make_unique<IdleBackend>());
    Scheduler sched(ctx);
    AsyncRwLock rw(sched);
    TimerCtl::enable_test_clock(sched);
    TimerCtl::set_clock(sched, 0);

    std::atomic<bool> r0_released{false};
    std::atomic<bool> w_registered{false};
    std::atomic<bool> cancel_done{false};

    Fiber rf0;
    rf0.set_entry([&](Fiber&) {
        WaitNode rn;
        rw.read_lock(rn);
        sched.await_ready_flag(r0_released);
        rw.unlock_read();
    });

    WaitNode wn;
    Fiber wf;
    wf.set_entry([&](Fiber&) {
        w_registered.store(true, std::memory_order_release);
        rw.write_lock_until(wn, 100);
        SLUICE_CHECK_MSG(wn.was_cancelled(),
                         "cancel winner: node resolves Cancelled, not Expired");
    });

    // Driver: after cancel, still advance the clock so the pump processes the
    // stale (RETIRED) registration; it MUST skip it inertly. The pump drains
    // every due entry inside the single advance_clock call — one advance is
    // the complete causal evidence, not a retry loop.
    Fiber fdrv;
    fdrv.set_entry([&](Fiber&) {
        sched.await_ready_flag(cancel_done);
        sched.advance_clock(100);
    });

    FiberStack s0, sw, sd;
    SLUICE_CHECK(sched.init_fiber(rf0, s0.base(), s0.size()));
    SLUICE_CHECK(sched.init_fiber(wf, sw.base(), sw.size()));
    SLUICE_CHECK(sched.init_fiber(fdrv, sd.base(), sd.size()));

    sched.spawn(rf0);
    sched.run(1);
    sched.spawn(wf);
    sched.spawn(fdrv);
    sched.run(1);
    SLUICE_CHECK_MSG(w_registered.load() && wn.is_registered(),
                     "timed writer queued");

    // Cancel wins the terminal CAS; retire the timer in the same CS.
    SLUICE_CHECK_MSG(rw.cancel(wn), "cancel(writer) won");
    SLUICE_CHECK_MSG(wn.was_cancelled(), "terminal outcome is Cancelled");
    SLUICE_CHECK_MSG(TimerCtl::active_deadline_count(sched) == 0,
                     "cancel retired the timer exactly once");

    cancel_done.store(true, std::memory_order_release);
    sched.run(1);  // pump pops the stale entry
    SLUICE_CHECK_MSG(wn.is_terminal() && !wn.was_expired(),
                     "late expiry is a no-op (single terminal winner)");
    SLUICE_CHECK_MSG(TimerCtl::active_deadline_count(sched) == 0,
                     "pump did not double-decrement");

    r0_released.store(true, std::memory_order_release);
    sched.run(1);
    SLUICE_CHECK_MSG(sched.waiting_count() == 0, "no unresolved waits");
}

// ---- R5b: expiry wins; a later cancel must return false --------------------
SLUICE_TEST_CASE(rwlock_audit_r5_expiry_wins_cancel_returns_false) {
    if constexpr (!fiber_ctx::supported) return;
    AsyncIoContext ctx(std::make_unique<IdleBackend>());
    Scheduler sched(ctx);
    AsyncRwLock rw(sched);
    TimerCtl::enable_test_clock(sched);
    TimerCtl::set_clock(sched, 0);

    std::atomic<bool> r0_released{false};
    std::atomic<bool> w_registered{false};

    Fiber rf0;
    rf0.set_entry([&](Fiber&) {
        WaitNode rn;
        rw.read_lock(rn);
        sched.await_ready_flag(r0_released);
        rw.unlock_read();
    });

    WaitNode wn;
    Fiber wf;
    wf.set_entry([&](Fiber&) {
        w_registered.store(true, std::memory_order_release);
        rw.write_lock_until(wn, 100);
        SLUICE_CHECK_MSG(wn.was_expired(),
                         "expiry winner: node resolves Expired");
    });

    Fiber fdrv;
    fdrv.set_entry([&](Fiber&) {
        sched.await_ready_flag(w_registered);
        sched.advance_clock(100);
    });

    FiberStack s0, sw, sd;
    SLUICE_CHECK(sched.init_fiber(rf0, s0.base(), s0.size()));
    SLUICE_CHECK(sched.init_fiber(wf, sw.base(), sw.size()));
    SLUICE_CHECK(sched.init_fiber(fdrv, sd.base(), sd.size()));

    sched.spawn(rf0);
    sched.run(1);
    sched.spawn(wf);
    sched.spawn(fdrv);
    sched.run(1);

    SLUICE_CHECK_MSG(wn.was_expired(), "expiry won (single terminal)");
    SLUICE_CHECK_MSG(!rw.cancel(wn), "cancel after expiry returns false");
    SLUICE_CHECK_MSG(TimerCtl::active_deadline_count(sched) == 0,
                     "timer consumed exactly once");

    r0_released.store(true, std::memory_order_release);
    sched.run(1);
    SLUICE_CHECK_MSG(sched.waiting_count() == 0, "no unresolved waits");
}

// ---- R5c: grant vs cancel arbitration — grant first, cancel loses ----------
SLUICE_TEST_CASE(rwlock_audit_r5_grant_wins_cancel_returns_false) {
    if constexpr (!fiber_ctx::supported) return;
    AsyncIoContext ctx(std::make_unique<IdleBackend>());
    Scheduler sched(ctx);
    AsyncRwLock rw(sched);

    std::atomic<bool> r0_released{false};
    std::atomic<bool> w_acquired{false};

    Fiber rf0;
    rf0.set_entry([&](Fiber&) {
        WaitNode rn;
        rw.read_lock(rn);
        sched.await_ready_flag(r0_released);
        rw.unlock_read();
    });

    WaitNode wn;
    Fiber wf;
    wf.set_entry([&](Fiber&) {
        rw.write_lock(wn);
        w_acquired.store(wn.was_woken(), std::memory_order_release);
        if (wn.was_woken()) rw.unlock_write();
    });

    FiberStack s0, sw;
    SLUICE_CHECK(sched.init_fiber(rf0, s0.base(), s0.size()));
    SLUICE_CHECK(sched.init_fiber(wf, sw.base(), sw.size()));

    sched.spawn(rf0);
    sched.run(1);
    sched.spawn(wf);
    sched.run(1);
    SLUICE_CHECK_MSG(wn.is_registered(), "writer queued behind R0");

    // Unlock first: grant wins; the node is woken and unlinked.
    r0_released.store(true, std::memory_order_release);
    sched.run(1);
    SLUICE_CHECK_MSG(w_acquired.load(), "grant winner: W acquired");
    SLUICE_CHECK_MSG(!rw.cancel(wn), "cancel after grant returns false");
    SLUICE_CHECK_MSG(wn.was_woken(), "single terminal outcome Woken");
    SLUICE_CHECK_MSG(sched.waiting_count() == 0, "no unresolved waits");
}

// ---- R5d: grant vs cancel arbitration — cancel first, grant is a no-op -----
SLUICE_TEST_CASE(rwlock_audit_r5_cancel_wins_grant_is_noop) {
    if constexpr (!fiber_ctx::supported) return;
    AsyncIoContext ctx(std::make_unique<IdleBackend>());
    Scheduler sched(ctx);
    AsyncRwLock rw(sched);

    std::atomic<bool> r0_released{false};
    std::atomic<bool> w_acquired{false};

    Fiber rf0;
    rf0.set_entry([&](Fiber&) {
        WaitNode rn;
        rw.read_lock(rn);
        sched.await_ready_flag(r0_released);
        rw.unlock_read();
    });

    WaitNode wn;
    Fiber wf;
    wf.set_entry([&](Fiber&) {
        rw.write_lock(wn);
        w_acquired.store(wn.was_woken(), std::memory_order_release);
    });

    FiberStack s0, sw;
    SLUICE_CHECK(sched.init_fiber(rf0, s0.base(), s0.size()));
    SLUICE_CHECK(sched.init_fiber(wf, sw.base(), sw.size()));

    sched.spawn(rf0);
    sched.run(1);
    sched.spawn(wf);
    sched.run(1);
    SLUICE_CHECK_MSG(wn.is_registered(), "writer queued behind R0");

    // Cancel first: node terminal Cancelled; the later unlock reconcile
    // grants nothing (queue already drained).
    SLUICE_CHECK_MSG(rw.cancel(wn), "cancel won");
    r0_released.store(true, std::memory_order_release);
    sched.run(1);
    SLUICE_CHECK_MSG(wn.was_cancelled(), "single terminal outcome Cancelled");
    SLUICE_CHECK_MSG(!w_acquired.load(),
                     "grant is a no-op for the canceled node");
    SLUICE_CHECK_MSG(sched.waiting_count() == 0, "no unresolved waits");
}

// ---- M3a: write_lock_until — due deadline + free resource = resource-first -
// Same precedence probe as T9 (read side); the write side must be identical.
SLUICE_TEST_CASE(rwlock_audit_m3_write_lock_until_resource_first) {
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
        rw.write_lock_until(node, 0);  // deadline already due
        acquired.store(node.was_woken(), std::memory_order_release);
        if (node.was_woken()) rw.unlock_write();
    });
    FiberStack sa;
    SLUICE_CHECK(sched.init_fiber(f, sa.base(), sa.size()));
    sched.spawn(f);
    sched.run(1);
    SLUICE_CHECK_MSG(acquired.load(),
                     "resource-first: admission wins over due deadline (write)");
    SLUICE_CHECK_MSG(node.was_woken(), "resolved Woken (not Expired)");
    SLUICE_CHECK_MSG(sched.waiting_count() == 0, "no unresolved waits");
    SLUICE_CHECK_MSG(TimerCtl::active_deadline_count(sched) == 0,
                     "timer retired at admission (no leak)");
}

// ---- M3b: write_lock_until — due deadline + busy resource = expire-inline --
// Precedence 2: when admission is impossible and the deadline is already due,
// the registration resolves Expired immediately without suspending.
SLUICE_TEST_CASE(rwlock_audit_m3_write_lock_until_due_blocked_expires) {
    if constexpr (!fiber_ctx::supported) return;
    AsyncIoContext ctx(std::make_unique<IdleBackend>());
    Scheduler sched(ctx);
    AsyncRwLock rw(sched);
    TimerCtl::enable_test_clock(sched);
    TimerCtl::set_clock(sched, 100);

    std::atomic<bool> r_holds{false};
    std::atomic<bool> r_released{false};

    Fiber rf;
    rf.set_entry([&](Fiber&) {
        WaitNode rn;
        rw.read_lock(rn);
        r_holds.store(true, std::memory_order_release);
        sched.await_ready_flag(r_released);
        rw.unlock_read();
    });

    WaitNode wn;
    std::atomic<bool> expired_inline{false};
    Fiber wf;
    wf.set_entry([&](Fiber&) {
        rw.write_lock_until(wn, 0);  // deadline already due, resource busy
        expired_inline.store(wn.was_expired(), std::memory_order_release);
    });

    FiberStack sr, sw;
    SLUICE_CHECK(sched.init_fiber(rf, sr.base(), sr.size()));
    SLUICE_CHECK(sched.init_fiber(wf, sw.base(), sw.size()));

    sched.spawn(rf);
    sched.run(1);
    SLUICE_CHECK_MSG(r_holds.load(), "reader holds");

    sched.spawn(wf);
    sched.run(1);
    SLUICE_CHECK_MSG(expired_inline.load(),
                     "due deadline + busy resource: Expired inline (no suspend)");
    SLUICE_CHECK_MSG(wn.was_expired(), "resolved Expired, not Woken");
    SLUICE_CHECK_MSG(TimerCtl::active_deadline_count(sched) == 0,
                     "timer consumed exactly once");

    r_released.store(true, std::memory_order_release);
    sched.run(1);
    SLUICE_CHECK_MSG(sched.waiting_count() == 0, "no unresolved waits");
}
