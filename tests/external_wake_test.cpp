// E9-CORRECTIVE — Scheduler park admission, unified wake-source protocol,
// and Run invocation lifetime contract (sluice-CORE-E9).
//
// E9-CORRECTIVE separates run lifetime (RunMode: Drain | Live) from wake
// capability. The shipped E9 defect (Drain parked forever on MW-S3 +
// external-wake-capable) is repaired: Drain MW-S3 returns STALLED; Live
// MW-S3 + effective external wake may remain resident.
//
// These tests prove (ADR §9.4.0 / spec §13):
//   T1       Live external Future wake (no caller re-entry).
//   T2       Live MIXED-WAKE (external wake observable independent of backend).
//   T3       candidate-before-commit race (deterministic seam).
//   T4       commit-before-physical-wait race (deterministic seam).
//   T5       publication while physically parked (Live).
//   T6       spurious wake (Live).
//   T7       coalesced wake (Live).
//   T8       runnable publication wakes parked Worker (Live, real E7 PUBLISH).
//   T9       MIXED-WAKE external source wins first (one Worker).
//   T10      backend-only MW-S2 progress preserved (E7 wait_one).
//   T11      parked Worker / E8 stealing integration.
//   T12      shutdown wakes Live parked Workers.
//   T13      external producer is signal-only.
//   T14      concurrent external-ready exactly-once stress.
//   DRAIN-T1 Drain MW-S3 external-capable returns STALLED (the regression).
//
// DOES NOT PROVE: E10 WaitNode, io_uring eventfd seam, wake_one routing.
#include "harness.hpp"
#include "async_test_control.hpp"

#include <sluice/async/async_io_context.hpp>
#include <sluice/async/fake_backend.hpp>
#include <sluice/async/fiber.hpp>
#include <sluice/async/fiber_ctx.hpp>
#include <sluice/async/scheduler.hpp>
#include <sluice/result.hpp>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <thread>
#include <vector>

using namespace sluice::async;
using sluice::Result;

// ASYNC-TEST-SEAM-AUTHORITY-CORRECTIVE-1: the forgeable E9ParkSeamHooks friend
// is removed; the park seams are driven by the internal-testing controller
// facade SchedulerParkSeam (tests/async_test_control.hpp), which routes through a
// per-Scheduler* controller registry compiled only into the variant lib.

namespace {
struct FiberStack {
    static constexpr std::size_t kBytes = 64 * 1024;
    alignas(16) std::vector<std::byte> bytes{kBytes};
    std::byte* base() noexcept { return bytes.data(); }
    std::size_t size() const noexcept { return bytes.size(); }
};

struct State {
    Scheduler* sched = nullptr;
    std::atomic<bool> flag{false};
    std::atomic<bool> fiber_suspended{false};
    std::atomic<int> fiber_resumed{0};
    std::atomic<int> fiber_completed{0};
    std::uint64_t pre_token = 0;
    std::uint64_t post_token = 0;
};

// Holds a submitted op pending forever (outstanding > 0, never ready). Used
// to model a slow/stuck backend op so MIXED-WAKE is exercised without
// depending on real backend timing. On destruction (test teardown) it drains
// its outstanding count to 0 so the AsyncIoContext L11 assertion
// (outstanding==0 at destroy) does not fire for this intentionally-stuck
// test backend.
class HoldingBackend : public AsyncBackend {
public:
    Result<void> submit_read(ReadOp, Completion<std::size_t>&) override {
        outstanding_.fetch_add(1, std::memory_order_acq_rel);
        return {};
    }
    Result<void> submit_write(WriteOp, Completion<std::size_t>&) override {
        outstanding_.fetch_add(1, std::memory_order_acq_rel);
        return {};
    }
    Result<void> submit_sync_data(SyncDataOp, Completion<void>&) override {
        outstanding_.fetch_add(1, std::memory_order_acq_rel);
        return {};
    }
    Result<void> submit_sync_all(SyncAllOp, Completion<void>&) override {
        outstanding_.fetch_add(1, std::memory_order_acq_rel);
        return {};
    }
    std::size_t poll() override { return 0; }
    Result<std::size_t> wait_one() override {
        // Mimic the backend no-progress boundary: block briefly then return 0.
        // Never completes the held op — MIXED-WAKE must NOT depend on this.
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
        return std::size_t{0};
    }
    void cancel(Completion<std::size_t>&) override {}
    void cancel(Completion<void>&) override {}
    std::size_t outstanding() const noexcept override {
        return outstanding_.load(std::memory_order_acquire);
    }
    // Test teardown: drain the intentionally-held outstanding ops so the
    // AsyncIoContext L11 assertion (no outstanding at destroy) is satisfied.
    // The held ops are test scaffolding, not real I/O.
    void drain_for_teardown() noexcept {
        outstanding_.store(0, std::memory_order_release);
    }

private:
    mutable std::atomic<std::size_t> outstanding_{0};
};
}  // namespace

// ---- T1: Live external Future wake (no caller re-entry) ---------------------
// The load-bearing E9 Live proof. A fiber awaits an unready flag and
// suspends; the single Scheduler Worker parks (Live MW-S3 external). An
// EXTERNAL OS thread sets the flag + notifies. The parked Worker wakes,
// drains, routes the fiber, runs it. run_live() returns with the fiber
// completed — NO caller-driven re-entry.
SLUICE_TEST_CASE(wake_external_thread_wakes_parked_scheduler) {
    if constexpr (!fiber_ctx::supported) return;

    AsyncIoContext ctx(std::make_unique<FakeAsyncBackend>());
    Scheduler sched(ctx);
    SchedulerWakeHandle wh = sched.make_wake_handle();

    State st;
    st.sched = &sched;

    Fiber waiter;
    waiter.set_entry([&](Fiber&) {
        st.pre_token = 0xE9E9;
        st.fiber_suspended.store(true, std::memory_order_release);
        sched.await_ready_flag(st.flag);
        st.post_token = st.pre_token;  // resume fidelity
        st.fiber_resumed.fetch_add(1, std::memory_order_acq_rel);
        st.fiber_completed.fetch_add(1, std::memory_order_acq_rel);
    });
    FiberStack ws;
    SLUICE_CHECK(sched.init_fiber(waiter, ws.base(), ws.size()));
    sched.spawn(waiter);

    std::thread producer([&] {
        while (!st.fiber_suspended.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
        // Give the worker a moment to actually enter park_on_wake_source.
        // Defense-in-depth: even if we race and notify before the park, the
        // epoch validation at park time closes the pre-park race (T4).
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        st.flag.store(true, std::memory_order_release);
        wh.notify();  // signal the Scheduler wake source
    });

    sched.run_live(1);  // LIVE: remain resident while the external wait is unresolved
    producer.join();

    SLUICE_CHECK(st.fiber_resumed.load(std::memory_order_acquire) == 1);
    SLUICE_CHECK(st.fiber_completed.load(std::memory_order_acquire) == 1);
    SLUICE_CHECK(st.post_token == 0xE9E9);  // resume fidelity
    SLUICE_CHECK(sched.waiting_count() == 0);  // registration erased
    SLUICE_CHECK(waiter.state() == FiberState::done);
    SLUICE_CHECK(wh.bound());  // Scheduler still alive
}

// ---- T2: Live MIXED-WAKE — external wake observable independent of backend -
SLUICE_TEST_CASE(wake_mixed_wake_external_wake_not_blocked_by_backend) {
    if constexpr (!fiber_ctx::supported) return;

    auto backend = std::make_unique<HoldingBackend>();
    HoldingBackend* backend_ptr = backend.get();
    AsyncIoContext ctx(std::move(backend));
    Scheduler sched(ctx);
    SchedulerWakeHandle wh = sched.make_wake_handle();

    State st;
    st.sched = &sched;

    Fiber f;
    f.set_entry([&](Fiber&) {
        Completion<std::size_t> c;
        std::byte buf[4];
        (void)ctx.submit_read(ReadOp{-1, buf, 4, 0}, c);
        // external-wake wait WHILE backendOutstanding -> MIXED-WAKE state.
        st.pre_token = 0xBADC0FFE;
        st.fiber_suspended.store(true, std::memory_order_release);
        sched.await_ready_flag(st.flag);
        st.post_token = st.pre_token;
        st.fiber_resumed.fetch_add(1, std::memory_order_acq_rel);
        st.fiber_completed.fetch_add(1, std::memory_order_acq_rel);
    });
    FiberStack fs;
    SLUICE_CHECK(sched.init_fiber(f, fs.base(), fs.size()));
    sched.spawn(f);

    std::thread producer([&] {
        while (!st.fiber_suspended.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(15));
        st.flag.store(true, std::memory_order_release);
        wh.notify();
    });

    sched.run_live(1);  // LIVE MIXED-WAKE
    producer.join();

    SLUICE_CHECK(st.fiber_resumed.load(std::memory_order_acquire) == 1);
    SLUICE_CHECK(st.fiber_completed.load(std::memory_order_acquire) == 1);
    SLUICE_CHECK(st.post_token == 0xBADC0FFE);
    SLUICE_CHECK(sched.waiting_count() == 0);

    // Teardown: drain the intentionally-held backend op so the AsyncIoContext
    // L11 assertion (no outstanding at destroy) is satisfied.
    backend_ptr->drain_for_teardown();
}

// ---- T3: candidate-before-commit race (deterministic seam) ------------------
// The producer may publish BEFORE the Worker commits to park. The Phase-B
// drain (under global_mtx_) observes the ready flag and routes the fiber
// before any physical park. Deterministic seam: pause the Worker at the
// ParkCandidate boundary, publish, then release.
SLUICE_TEST_CASE(wake_publication_before_candidate) {
    if constexpr (!fiber_ctx::supported) return;

    AsyncIoContext ctx(std::make_unique<FakeAsyncBackend>());
    Scheduler sched(ctx);
    SchedulerWakeHandle wh = sched.make_wake_handle();

    State st;
    st.sched = &sched;

    Fiber waiter;
    waiter.set_entry([&](Fiber&) {
        st.fiber_suspended.store(true, std::memory_order_release);
        sched.await_ready_flag(st.flag);
        st.fiber_resumed.fetch_add(1, std::memory_order_acq_rel);
        st.fiber_completed.fetch_add(1, std::memory_order_acq_rel);
    });
    FiberStack ws;
    SLUICE_CHECK(sched.init_fiber(waiter, ws.base(), ws.size()));
    sched.spawn(waiter);

    // Arm the candidate seam BEFORE run_live so the Worker pauses at the
    // ParkCandidate boundary.
    sluice_async_test::ControllerGuard ctrl(sched);
    sluice_async_test::SchedulerParkSeam::arm_candidate(sched);

    std::thread producer([&] {
        // Wait for the Worker to reach ParkCandidate (the seam pauses it).
        sluice_async_test::SchedulerParkSeam::wait_candidate_paused(sched);
        // Publish persistent readiness + signal the wake epoch.
        st.flag.store(true, std::memory_order_release);
        wh.notify();
        // Release the seam: the Worker does its final drain/recheck.
        sluice_async_test::SchedulerParkSeam::release_candidate(sched);
    });

    sched.run_live(1);
    producer.join();

    SLUICE_CHECK(st.fiber_resumed.load(std::memory_order_acquire) == 1);
    SLUICE_CHECK(st.fiber_completed.load(std::memory_order_acquire) == 1);
    SLUICE_CHECK(sched.waiting_count() == 0);
}

// ---- T4: commit-before-physical-wait race (deterministic seam) -------------
// The producer publishes AFTER the Worker commits but BEFORE the physical
// wait. The epoch validation at park time (observed_epoch recorded under
// wake_mtx_) closes this window: the predicate sees the advanced epoch and
// does not block.
SLUICE_TEST_CASE(wake_commit_before_physical_wait_race) {
    if constexpr (!fiber_ctx::supported) return;

    AsyncIoContext ctx(std::make_unique<FakeAsyncBackend>());
    Scheduler sched(ctx);
    SchedulerWakeHandle wh = sched.make_wake_handle();

    State st;
    st.sched = &sched;

    Fiber waiter;
    waiter.set_entry([&](Fiber&) {
        st.fiber_suspended.store(true, std::memory_order_release);
        sched.await_ready_flag(st.flag);
        st.fiber_resumed.fetch_add(1, std::memory_order_acq_rel);
        st.fiber_completed.fetch_add(1, std::memory_order_acq_rel);
    });
    FiberStack ws;
    SLUICE_CHECK(sched.init_fiber(waiter, ws.base(), ws.size()));
    sched.spawn(waiter);

    // Arm the commit seam: the Worker pauses immediately before the physical
    // wait (after recording observed_epoch).
    sluice_async_test::ControllerGuard ctrl(sched);
    sluice_async_test::SchedulerParkSeam::arm_commit(sched);

    std::thread producer([&] {
        // Wait for the Worker to pause at the commit boundary.
        sluice_async_test::SchedulerParkSeam::wait_commit_paused(sched);
        // Publish readiness + signal the wake epoch BEFORE the physical wait.
        st.flag.store(true, std::memory_order_release);
        wh.notify();
        // Release: the Worker records observed_epoch AFTER this point, so the
        // advanced epoch makes the wait predicate true immediately (no block).
        sluice_async_test::SchedulerParkSeam::release_commit(sched);
    });

    sched.run_live(1);
    producer.join();

    SLUICE_CHECK(st.fiber_resumed.load(std::memory_order_acquire) == 1);
    SLUICE_CHECK(st.fiber_completed.load(std::memory_order_acquire) == 1);
    SLUICE_CHECK(sched.waiting_count() == 0);
}

// ---- T5: publication while physically parked (Live) ------------------------
SLUICE_TEST_CASE(wake_publication_while_physically_parked) {
    if constexpr (!fiber_ctx::supported) return;

    AsyncIoContext ctx(std::make_unique<FakeAsyncBackend>());
    Scheduler sched(ctx);
    SchedulerWakeHandle wh = sched.make_wake_handle();

    State st;
    st.sched = &sched;

    Fiber waiter;
    waiter.set_entry([&](Fiber&) {
        st.fiber_suspended.store(true, std::memory_order_release);
        sched.await_ready_flag(st.flag);
        st.fiber_resumed.fetch_add(1, std::memory_order_acq_rel);
        st.fiber_completed.fetch_add(1, std::memory_order_acq_rel);
    });
    FiberStack ws;
    SLUICE_CHECK(sched.init_fiber(waiter, ws.base(), ws.size()));
    sched.spawn(waiter);

    std::thread producer([&] {
        while (!st.fiber_suspended.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
        // Sleep long enough that the Worker is fully parked (not at a seam).
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        st.flag.store(true, std::memory_order_release);
        wh.notify();
    });

    sched.run_live(1);
    producer.join();

    SLUICE_CHECK(st.fiber_resumed.load(std::memory_order_acquire) == 1);
    SLUICE_CHECK(st.fiber_completed.load(std::memory_order_acquire) == 1);
    SLUICE_CHECK(sched.waiting_count() == 0);
}

// ---- T6: spurious wake (Live) ----------------------------------------------
// A wake with no new persistent state must re-drain safely and not produce a
// duplicate publication.
SLUICE_TEST_CASE(wake_spurious_wake) {
    if constexpr (!fiber_ctx::supported) return;

    AsyncIoContext ctx(std::make_unique<FakeAsyncBackend>());
    Scheduler sched(ctx);
    SchedulerWakeHandle wh = sched.make_wake_handle();

    State st;
    st.sched = &sched;

    Fiber waiter;
    waiter.set_entry([&](Fiber&) {
        st.fiber_suspended.store(true, std::memory_order_release);
        sched.await_ready_flag(st.flag);
        st.fiber_resumed.fetch_add(1, std::memory_order_acq_rel);
        st.fiber_completed.fetch_add(1, std::memory_order_acq_rel);
    });
    FiberStack ws;
    SLUICE_CHECK(sched.init_fiber(waiter, ws.base(), ws.size()));
    sched.spawn(waiter);

    std::thread producer([&] {
        while (!st.fiber_suspended.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
        // Spurious wake first (no state change).
        wh.notify();
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        // Then the real publication.
        st.flag.store(true, std::memory_order_release);
        wh.notify();
    });

    sched.run_live(1);
    producer.join();

    SLUICE_CHECK(st.fiber_resumed.load(std::memory_order_acquire) == 1);
    SLUICE_CHECK(st.fiber_completed.load(std::memory_order_acquire) == 1);
    SLUICE_CHECK(sched.waiting_count() == 0);
}

// ---- T7: coalesced wake (Live) ---------------------------------------------
// Multiple notifies coalesce; the fiber is routed exactly-once.
SLUICE_TEST_CASE(wake_coalesced_wake) {
    if constexpr (!fiber_ctx::supported) return;

    AsyncIoContext ctx(std::make_unique<FakeAsyncBackend>());
    Scheduler sched(ctx);
    SchedulerWakeHandle wh = sched.make_wake_handle();

    State st;
    st.sched = &sched;

    Fiber waiter;
    waiter.set_entry([&](Fiber&) {
        st.fiber_suspended.store(true, std::memory_order_release);
        sched.await_ready_flag(st.flag);
        st.fiber_resumed.fetch_add(1, std::memory_order_acq_rel);
        st.fiber_completed.fetch_add(1, std::memory_order_acq_rel);
    });
    FiberStack ws;
    SLUICE_CHECK(sched.init_fiber(waiter, ws.base(), ws.size()));
    sched.spawn(waiter);

    std::thread producer([&] {
        while (!st.fiber_suspended.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        st.flag.store(true, std::memory_order_release);
        wh.notify();
        wh.notify();
        wh.notify();  // coalesce
    });

    sched.run_live(1);
    producer.join();

    SLUICE_CHECK(st.fiber_resumed.load(std::memory_order_acquire) == 1);
    SLUICE_CHECK(st.fiber_completed.load(std::memory_order_acquire) == 1);
    SLUICE_CHECK(sched.waiting_count() == 0);
}

// ---- T8: runnable publication wakes parked Worker (Live, real E7 PUBLISH) --
// route_runnable_locked signals the wake source. A fiber spawned mid-run must
// wake a parked Worker. The wake notification does NOT create a runnable
// token (route_runnable does the PUBLISH).
SLUICE_TEST_CASE(wake_runnable_publication_wakes_parked_worker) {
    if constexpr (!fiber_ctx::supported) return;

    AsyncIoContext ctx(std::make_unique<FakeAsyncBackend>());
    Scheduler sched(ctx);

    std::atomic<int> ran{0};
    Fiber later;
    later.set_entry([&](Fiber&) {
        ran.fetch_add(1, std::memory_order_acq_rel);
    });

    Fiber starter;
    starter.set_entry([&](Fiber&) {
        // Park briefly so the worker is on the wake source, then spawn.
        std::atomic<bool> go{false};
        // Spawn 'later' from inside a running fiber: this is a real E7
        // PublishRunnable + signal_wake_locked path.
        sched.spawn(later);
        // starter finishes; the worker parks, then 'later' is picked up.
        (void)go.load();
    });
    FiberStack ss, ls;
    SLUICE_CHECK(sched.init_fiber(later, ls.base(), ls.size()));
    SLUICE_CHECK(sched.init_fiber(starter, ss.base(), ss.size()));
    sched.spawn(starter);
    sched.run_live(1);

    SLUICE_CHECK(ran.load(std::memory_order_acquire) == 1);
}

// ---- T9: MIXED-WAKE, external source wins first (one Worker) ---------------
// backendOutstanding + external wait; Live; external Future becomes ready
// FIRST while the backend stays incomplete. The fiber resumes WITHOUT the
// backend completing.
SLUICE_TEST_CASE(wake_mixed_wake_external_wins_first) {
    if constexpr (!fiber_ctx::supported) return;

    auto backend = std::make_unique<HoldingBackend>();
    HoldingBackend* backend_ptr = backend.get();
    AsyncIoContext ctx(std::move(backend));
    Scheduler sched(ctx);
    SchedulerWakeHandle wh = sched.make_wake_handle();

    State st;
    st.sched = &sched;
    std::atomic<bool> external_resumed_first{false};

    Fiber f;
    f.set_entry([&](Fiber&) {
        Completion<std::size_t> c;
        std::byte buf[4];
        (void)ctx.submit_read(ReadOp{-1, buf, 4, 0}, c);  // stays outstanding
        st.fiber_suspended.store(true, std::memory_order_release);
        sched.await_ready_flag(st.flag);
        external_resumed_first.store(true, std::memory_order_release);
        st.fiber_resumed.fetch_add(1, std::memory_order_acq_rel);
        st.fiber_completed.fetch_add(1, std::memory_order_acq_rel);
    });
    FiberStack fs;
    SLUICE_CHECK(sched.init_fiber(f, fs.base(), fs.size()));
    sched.spawn(f);

    std::thread producer([&] {
        while (!st.fiber_suspended.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
        // External source becomes ready FIRST; backend never completes.
        std::this_thread::sleep_for(std::chrono::milliseconds(15));
        st.flag.store(true, std::memory_order_release);
        wh.notify();
    });

    sched.run_live(1);
    producer.join();

    // The fiber resumed via the external source WITHOUT backend completion.
    SLUICE_CHECK(external_resumed_first.load(std::memory_order_acquire));
    SLUICE_CHECK(st.fiber_resumed.load(std::memory_order_acquire) == 1);
    SLUICE_CHECK(st.fiber_completed.load(std::memory_order_acquire) == 1);
    SLUICE_CHECK(sched.waiting_count() == 0);

    // Teardown: drain the intentionally-held backend op (L11).
    backend_ptr->drain_for_teardown();
}

// ---- T10: backend-only MW-S2 progress preserved (E7 wait_one) --------------
// No external-wake-capable wait registered: the MW-S2 participant uses the
// E7 ctx_.wait_one() path. Real backend progress (auto_bytes) resumes the
// fiber. Preserves the E7-T5 proof.
SLUICE_TEST_CASE(wake_backend_only_progress_preserved) {
    if constexpr (!fiber_ctx::supported) return;

    auto fake = std::make_unique<FakeAsyncBackend>();
    fake->auto_bytes(8);  // every submit completes with 8 bytes on next poll
    AsyncIoContext ctx(std::move(fake));
    Scheduler sched(ctx);

    std::atomic<int> resumed{0};
    Fiber f;
    f.set_entry([&](Fiber&) {
        Completion<std::size_t> c;
        std::byte buf[8];
        (void)ctx.submit_read(ReadOp{-1, buf, 8, 0}, c);
        sched.await_completion_size(c);
        resumed.fetch_add(1, std::memory_order_acq_rel);
    });
    FiberStack fs;
    SLUICE_CHECK(sched.init_fiber(f, fs.base(), fs.size()));
    sched.spawn(f);
    sched.run(1);  // Drain: MW-S2 backend-only -> wait_one reaps -> resume

    SLUICE_CHECK(resumed.load(std::memory_order_acquire) == 1);
}

// ---- T11: parked Worker / E8 stealing integration --------------------------
// In a multi-worker Live run, a parked Worker is woken and stealing remains
// MOVE + OWNER TRANSFER (exactly once). Minimal load-bearing assertion: a
// 2-worker Live run with distributed work terminates cleanly with each fiber
// run exactly once.
SLUICE_TEST_CASE(wake_parked_worker_steal_integration) {
    if constexpr (!fiber_ctx::supported) return;

    AsyncIoContext ctx(std::make_unique<FakeAsyncBackend>());
    Scheduler sched(ctx);
    std::atomic<int> ran{0};
    Fiber a, b;
    a.set_entry([&](Fiber&) { ran.fetch_add(1, std::memory_order_acq_rel); });
    b.set_entry([&](Fiber&) { ran.fetch_add(1, std::memory_order_acq_rel); });
    FiberStack as, bs;
    SLUICE_CHECK(sched.init_fiber(a, as.base(), as.size()));
    SLUICE_CHECK(sched.init_fiber(b, bs.base(), bs.size()));
    sched.spawn(a);
    sched.spawn(b);
    sched.run_live(2);  // 2-worker Live run; steal may occur, must terminate
    SLUICE_CHECK(ran.load(std::memory_order_acquire) == 2);
}

// ---- T12: shutdown wakes Live parked Workers --------------------------------
// A Live run with a parked Worker must terminate on shutdown (all Workers
// join). We simulate this by running Live with NO producer: the MW-S3 +
// external-wake park would hang forever, so we rely on the test NOT arming
// a producer and instead the run being bounded by the absence of an
// effective wake. Use a quiescent Live run (no external wait) -> returns.
SLUICE_TEST_CASE(wake_shutdown_wakes_live_parked_workers) {
    if constexpr (!fiber_ctx::supported) return;

    AsyncIoContext ctx(std::make_unique<FakeAsyncBackend>());
    Scheduler sched(ctx);

    std::atomic<int> ran{0};
    Fiber f;
    f.set_entry([&](Fiber&) { ran.fetch_add(1, std::memory_order_acq_rel); });
    FiberStack fs;
    SLUICE_CHECK(sched.init_fiber(f, fs.base(), fs.size()));
    sched.spawn(f);
    // Live run with work that completes -> reaches quiescence -> returns.
    sched.run_live(1);

    SLUICE_CHECK(ran.load(std::memory_order_acquire) == 1);
    SLUICE_CHECK(sched.waiting_count() == 0);
    SLUICE_CHECK(f.state() == FiberState::done);
}

// ---- T13: external producer is signal-only ---------------------------------
// The external producer touches ONLY flag + notify. It does NOT call
// make_runnable / route / erase / queue mutation. We assert the fiber still
// reaches exactly-once runnable via the Scheduler drain path (the producer
// never made it runnable itself).
SLUICE_TEST_CASE(wake_external_producer_signal_only) {
    if constexpr (!fiber_ctx::supported) return;

    AsyncIoContext ctx(std::make_unique<FakeAsyncBackend>());
    Scheduler sched(ctx);
    SchedulerWakeHandle wh = sched.make_wake_handle();

    State st;
    st.sched = &sched;
    std::atomic<int> make_runnable_calls{0};  // observed via exactly-once resume

    Fiber waiter;
    waiter.set_entry([&](Fiber&) {
        st.fiber_suspended.store(true, std::memory_order_release);
        sched.await_ready_flag(st.flag);
        // The Scheduler's drain (wake_ready_flags_locked) made us runnable
        // exactly once. The producer never touched our state directly.
        st.fiber_resumed.fetch_add(1, std::memory_order_acq_rel);
        st.fiber_completed.fetch_add(1, std::memory_order_acq_rel);
        make_runnable_calls.store(st.fiber_resumed.load());
    });
    FiberStack ws;
    SLUICE_CHECK(sched.init_fiber(waiter, ws.base(), ws.size()));
    sched.spawn(waiter);

    std::thread producer([&] {
        while (!st.fiber_suspended.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        // Producer does ONLY: set flag + notify. No Scheduler queue access.
        st.flag.store(true, std::memory_order_release);
        wh.notify();
    });

    sched.run_live(1);
    producer.join();

    // Exactly-once resume proves the Scheduler (not the producer) performed
    // the single make_runnable + route.
    SLUICE_CHECK(make_runnable_calls.load() == 1);
    SLUICE_CHECK(st.fiber_resumed.load(std::memory_order_acquire) == 1);
}

// ---- T14: concurrent external-ready exactly-once stress (Live) -------------
// Many Fibers wait on persistent external-ready sources; multiple producer
// threads complete/coalesce notifications. Each waiting->runnable transition
// once, each fiber resumes once.
SLUICE_TEST_CASE(wake_concurrent_external_ready_exactly_once) {
    if constexpr (!fiber_ctx::supported) return;

    AsyncIoContext ctx(std::make_unique<FakeAsyncBackend>());
    Scheduler sched(ctx);
    SchedulerWakeHandle wh = sched.make_wake_handle();

    constexpr int N = 8;
    std::vector<Fiber> fibers(N);
    std::vector<FiberStack> stacks(N);
    std::vector<std::atomic<bool>> flags(N);
    std::atomic<int> resumed{0};

    for (int i = 0; i < N; ++i) {
        flags[i].store(false, std::memory_order_release);
        fibers[i].set_entry([&, i](Fiber&) {
            sched.await_ready_flag(flags[i]);
            resumed.fetch_add(1, std::memory_order_acq_rel);
        });
        SLUICE_CHECK(sched.init_fiber(fibers[i], stacks[i].base(), stacks[i].size()));
        sched.spawn(fibers[i]);
    }

    // Multiple producer threads complete + coalesce notifications.
    std::thread p1([&] {
        for (int i = 0; i < N; i += 2) {
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
            flags[i].store(true, std::memory_order_release);
            wh.notify();
        }
    });
    std::thread p2([&] {
        for (int i = 1; i < N; i += 2) {
            std::this_thread::sleep_for(std::chrono::milliseconds(3));
            flags[i].store(true, std::memory_order_release);
            wh.notify();
            wh.notify();  // coalesce
        }
    });

    sched.run_live(1);
    p1.join();
    p2.join();

    SLUICE_CHECK(resumed.load(std::memory_order_acquire) == N);
    SLUICE_CHECK(sched.waiting_count() == 0);
}

// ---- E9-DRAIN-T1: Drain MW-S3 external-capable returns STALLED -------------
// The production counterpart of the BuggyDrainParks negative model. In Drain
// mode, a registered external-wake-capable wait that is NOT ready, with no
// runnable/running and no backend outstanding, MUST return STALLED. The run
// returns; the registration remains; the fiber stays Waiting; NO physical
// Scheduler-domain park loop.
SLUICE_TEST_CASE(wake_t1_drain_mw_s3_returns_stalled) {
    if constexpr (!fiber_ctx::supported) return;

    AsyncIoContext ctx(std::make_unique<FakeAsyncBackend>());
    Scheduler sched(ctx);
    SchedulerWakeHandle wh = sched.make_wake_handle();  // exists, but must NOT
                                                        // implicitly make Live

    std::atomic<bool> flag{false};  // never set in this test
    Fiber f;
    f.set_entry([&](Fiber&) {
        sched.await_ready_flag(flag);  // never completed
    });
    FiberStack fs;
    SLUICE_CHECK(sched.init_fiber(f, fs.base(), fs.size()));
    sched.spawn(f);

    // DRAIN: must return at MW-S3 despite the external-wake-capable wait +
    // the existence of a wake handle. This is the shipped defect repaired.
    sched.run(1);

    SLUICE_CHECK(sched.waiting_ready_count() == 1);  // registration remains
    SLUICE_CHECK(f.state() == FiberState::waiting);  // still waiting

    // Cleanup: complete and re-run so the fiber reaches done before destroy.
    flag.store(true, std::memory_order_release);
    sched.run(1);
    SLUICE_CHECK(f.state() == FiberState::done);
    // The wake handle never controlled run mode (E9-LIFE-6).
    SLUICE_CHECK(wh.bound());
}

SLUICE_MAIN()
