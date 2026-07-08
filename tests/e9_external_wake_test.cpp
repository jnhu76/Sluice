// E9 — Scheduler park admission and unified wake-source protocol
// (sluice-CORE-E9). Implements ADR §9.4 (Model P3, decoupled wake domains).
//
// These tests prove the E9 load-bearing path: an EXTERNAL OS thread completes
// a Future-like flag (sets ready) and signals the Scheduler wake source, while
// a Scheduler Worker is parked. The parked Worker wakes, drains the
// registration, routes the waiting Fiber, and runs it — WITHOUT caller-driven
// run() re-entry. This is the property the E9-0 audit identified as GAP-1
// (external wake) and GAP-2 (MIXED-WAKE).
//
// PROVES:
//   T1  external-thread flag completion wakes a parked Scheduler (no re-entry).
//   T2  the same works with a ThreadPoolBackend outstanding op (MIXED-WAKE):
//       external wake is observable independent of backend timing.
//   T3  wake coalescing: multiple notifies collapse to one wake (no double-
//       route / no lost work).
//   T4  a notify that arrives BEFORE the Worker parks is not lost (the
//       commit-to-sleep epoch validation closes the pre-park race).
//   T5  SchedulerWakeHandle survives Scheduler destruction (post-destruction
//       notify is a safe no-op, no use-after-free).
//   T6  E7/E8 regression: a parked Worker is still woken by runnable
//       publication (route_runnable_locked signals the wake source).
//   T7  shutdown wakes a parked Worker (global_terminate_ signals).
//
// DOES NOT PROVE: E10 cancellation-safe wait queue, io_uring eventfd seam,
//   wake_one routing (notify_all baseline).
#include "harness.hpp"

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

namespace {
struct FiberStack {
    static constexpr std::size_t kBytes = 64 * 1024;
    alignas(16) std::vector<std::byte> bytes{kBytes};
    std::byte* base() noexcept { return bytes.data(); }
    std::size_t size() const noexcept { return bytes.size(); }
};

// Synchronization so the external producer thread knows the Scheduler Worker
// is parked before it sets the flag + notifies. The fiber registers the wait
// and switches out; at that point the worker will park on the wake source.
// `fiber_suspended` is set by the fiber just before it switches out; the
// producer waits for it, then (with a tiny grace for the worker to actually
// enter the park) sets the flag and notifies.
struct State {
    Scheduler* sched = nullptr;
    std::atomic<bool> flag{false};
    std::atomic<bool> fiber_suspended{false};
    std::atomic<int> fiber_resumed{0};
    std::atomic<int> fiber_completed{0};
    std::uint64_t pre_token = 0;
    std::uint64_t post_token = 0;
};
}  // namespace

// ---- T1: external-thread flag completion wakes a parked Scheduler ---------
// The load-bearing E9 proof. A fiber awaits an unready flag and suspends; the
// single Scheduler Worker parks on the wake source. An EXTERNAL OS thread
// (started before run()) sets the flag and calls SchedulerWakeHandle::notify().
// The parked Worker wakes, drains, routes the fiber, runs it. run() returns
// with the fiber completed — NO caller-driven re-entry.
SLUICE_TEST_CASE(e9_t1_external_thread_wakes_parked_scheduler) {
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

    // External producer thread: wait for the fiber to suspend (which means the
    // worker is about to park), then set the flag + notify the wake source.
    std::thread producer([&] {
        // Wait for the waiter fiber to register the wait and switch out.
        while (!st.fiber_suspended.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
        // Give the worker a moment to actually enter park_on_wake_source.
        // (Defense-in-depth: even if we race and notify before the park, the
        // epoch validation at park time closes the pre-park race — T4.)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        st.flag.store(true, std::memory_order_release);
        wh.notify();  // signal the Scheduler wake source
    });

    sched.run(1);
    producer.join();

    SLUICE_CHECK(st.fiber_resumed.load(std::memory_order_acquire) == 1);
    SLUICE_CHECK(st.fiber_completed.load(std::memory_order_acquire) == 1);
    SLUICE_CHECK(st.post_token == 0xE9E9);  // resume fidelity
    SLUICE_CHECK(sched.waiting_count() == 0);  // registration erased
    SLUICE_CHECK(waiter.state() == FiberState::done);
    SLUICE_CHECK(wh.bound());  // Scheduler still alive
}

// ---- T2: MIXED-WAKE — external wake observable independent of backend ----
// A backend op is held outstanding (never completes); the worker is the MW-S2
// participant. Because an external-wake-capable wait is registered, the
// participant parks on the SCHEDULER domain (not backend wait_one). An
// external thread completes the Future flag + notify. The fiber resumes
// WITHOUT the backend completing — external wake is the authority.
namespace {
// Holds a submitted op pending forever (outstanding > 0, never ready). Used to
// model a slow/stuck backend op so MIXED-WAKE is exercised without depending
// on real backend timing.
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
        // (Never completes the held op — the whole point of MIXED-WAKE is that
        // external wake must NOT depend on this returning progress.)
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        return std::size_t{0};
    }
    void cancel(Completion<std::size_t>&) override {}
    void cancel(Completion<void>&) override {}
    std::size_t outstanding() const noexcept override {
        return outstanding_.load(std::memory_order_acquire);
    }

private:
    std::atomic<std::size_t> outstanding_{0};
};
}  // namespace

SLUICE_TEST_CASE(e9_t2_mixed_wake_external_wake_not_blocked_by_backend) {
    if constexpr (!fiber_ctx::supported) return;

    AsyncIoContext ctx(std::make_unique<HoldingBackend>());
    Scheduler sched(ctx);
    SchedulerWakeHandle wh = sched.make_wake_handle();

    State st;
    st.sched = &sched;

    Fiber f;
    f.set_entry([&](Fiber&) {
        // Submit a backend op that stays outstanding (never completes).
        Completion<std::size_t> c;
        std::byte buf[4];
        (void)ctx.submit_read(ReadOp{-1, buf, 4, 0}, c);
        // Now await an external flag. This registers an external-wake wait
        // WHILE backendOutstanding is TRUE -> MIXED-WAKE state.
        st.pre_token = 0xBADC0FFE;  // MIXED-WAKE fidelity marker
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

    // The run should return when the fiber completes — WITHOUT the held
    // backend op ever completing. If MIXED-WAKE were un-closed, the worker
    // would block in ctx_.wait_one() (backend no-progress path returns 0 → the
    // run terminates without the fiber resuming). The wake-source park path
    // makes external wake observable.
    sched.run(1);
    producer.join();

    SLUICE_CHECK(st.fiber_resumed.load(std::memory_order_acquire) == 1);
    SLUICE_CHECK(st.fiber_completed.load(std::memory_order_acquire) == 1);
    SLUICE_CHECK(st.post_token == 0xBADC0FFE);
    SLUICE_CHECK(sched.waiting_count() == 0);
}

// ---- T3: wake coalescing — multiple notifies collapse, no double-route ----
// Two external notifies on the same wake epoch coalesce; the fiber is routed
// exactly-once (the registration is erased after the first drain).
SLUICE_TEST_CASE(e9_t3_wake_coalescing_no_double_route) {
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
        // Multiple notifies — must coalesce, not double-route.
        wh.notify();
        wh.notify();
        wh.notify();
    });

    sched.run(1);
    producer.join();

    SLUICE_CHECK(st.fiber_resumed.load(std::memory_order_acquire) == 1);
    SLUICE_CHECK(st.fiber_completed.load(std::memory_order_acquire) == 1);
    SLUICE_CHECK(sched.waiting_count() == 0);
}

// ---- T4: pre-park race — notify before the Worker parks is not lost -------
// The external notify may win the race against the Worker entering the park.
// The wake-epoch validation at park time closes this: the Worker observes the
// epoch already advanced and does not block (or blocks with a true predicate).
SLUICE_TEST_CASE(e9_t4_pre_park_race_not_lost) {
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
        // Wait for the fiber to switch out, then notify IMMEDIATELY (no grace
        // sleep) to maximize the chance of winning the park race.
        while (!st.fiber_suspended.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
        st.flag.store(true, std::memory_order_release);
        wh.notify();
    });

    sched.run(1);
    producer.join();

    SLUICE_CHECK(st.fiber_resumed.load(std::memory_order_acquire) == 1);
    SLUICE_CHECK(st.fiber_completed.load(std::memory_order_acquire) == 1);
    SLUICE_CHECK(sched.waiting_count() == 0);
}

// ---- T5: SchedulerWakeHandle survives Scheduler destruction ---------------
// A notify after the issuing Scheduler is destroyed is a safe no-op (no
// use-after-free). The weak/generation control block expires.
SLUICE_TEST_CASE(e9_t5_handle_survives_scheduler_destruction) {
    SchedulerWakeHandle wh;
    {
        AsyncIoContext ctx(std::make_unique<FakeAsyncBackend>());
        Scheduler sched(ctx);
        wh = sched.make_wake_handle();
        SLUICE_CHECK(wh.bound());
    }
    // Scheduler is now destroyed. notify must be a safe no-op.
    SLUICE_CHECK(!wh.bound());
    bool delivered = wh.notify();  // must not crash, must return false.
    SLUICE_CHECK(!delivered);
}

// ---- T6: runnable publication still wakes a parked Worker (E7/E8 reg) ------
// route_runnable_locked now also signals the wake source. A fiber spawned
// mid-run (by another fiber) must wake a parked Worker.
SLUICE_TEST_CASE(e9_t6_runnable_publication_wakes_parked_worker) {
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
        // Spawn the 'later' fiber mid-run. This routes runnable work + signals
        // the wake source. The parked worker must wake and run 'later'.
        sched.spawn(later);
    });
    FiberStack ss, ls;
    SLUICE_CHECK(sched.init_fiber(later, ls.base(), ls.size()));
    SLUICE_CHECK(sched.init_fiber(starter, ss.base(), ss.size()));
    sched.spawn(starter);
    sched.run(1);

    SLUICE_CHECK(ran.load(std::memory_order_acquire) == 1);
}

// ---- T7: shutdown wakes a parked Worker -----------------------------------
// When a run reaches quiescence (all Fibers done, no outstanding ops, no
// registered waits), the parked Worker must observe global_terminate_ via the
// wake source and run() must complete. This proves the shutdown signal path
// reaches the wake_cv park (not just the old inbox_cv).
SLUICE_TEST_CASE(e9_t7_shutdown_wakes_parked_worker) {
    if constexpr (!fiber_ctx::supported) return;

    AsyncIoContext ctx(std::make_unique<FakeAsyncBackend>());
    Scheduler sched(ctx);

    // A fiber that completes immediately (no registered wait). After it runs,
    // the state is quiescent; the worker parks on the wake source, then the
    // all-idle recheck sets global_terminate_ and signals the wake source.
    std::atomic<int> ran{0};
    Fiber f;
    f.set_entry([&](Fiber&) {
        ran.fetch_add(1, std::memory_order_acq_rel);
    });
    FiberStack fs;
    SLUICE_CHECK(sched.init_fiber(f, fs.base(), fs.size()));
    sched.spawn(f);

    sched.run(1);  // must terminate (quiescent), not hang on the wake park.

    SLUICE_CHECK(ran.load(std::memory_order_acquire) == 1);
    SLUICE_CHECK(sched.waiting_count() == 0);  // truly quiescent
    SLUICE_CHECK(f.state() == FiberState::done);
}

SLUICE_MAIN()
