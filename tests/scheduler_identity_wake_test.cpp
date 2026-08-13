// Phase F1 (issue #98) — the production Scheduler consumes identity-bearing
// reap.
//
// Proves ADR-explicit-io-request-contract Decisions 9/10 on the PRODUCTION
// Scheduler path: await_completion registers a real arena waiter (token +
// routing lease) plus a Scheduler routing record; the backend reap delivers a
// by-value ReadyEvent to the Scheduler-owned ReadyRoutingSink; the drain
// routes the fiber exactly once under global_mtx_. The Completion*-keyed wait
// maps and the O(N) ready() re-scan are gone from the production path.
//
// Determinism: FakeAsyncBackend is the completion source (no real I/O, no
// timing races); the coordinator completes + polls against a Live run (E9
// external-wake protocol — the wake source is the delivery mechanism, no
// sleeps as proof); the race case uses a barrier plus outcome invariants.
//
// Design: docs/design/phase-f1-scheduler-ready-sink.md
// Gate:   docs/architecture/phase-f1-compliance-gate.md (Gate 4 evidence)
#include "harness.hpp"

#include <sluice/async/async_io_context.hpp>
#include <sluice/async/completion.hpp>
#include <sluice/async/detail/ready_sink.hpp>
#include <sluice/async/fake_backend.hpp>
#include <sluice/async/fiber.hpp>
#include <sluice/async/fiber_ctx.hpp>
#include <sluice/async/scheduler.hpp>
#include <sluice/async/sync_backend.hpp>
#include <sluice/async/threadpool_backend.hpp>
#include <sluice/error.hpp>
#include <sluice/result.hpp>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fcntl.h>
#include <filesystem>
#include <string>
#include <thread>
#include <unistd.h>
#include <vector>

using namespace sluice;
using namespace sluice::async;
using AsyncTestAccess = Scheduler::AsyncTestAccess;
using sluice::async::detail::OptionalWaiterDelivery;
using sluice::async::detail::ReadyEvent;
using sluice::async::detail::RoutingLease;
using sluice::async::detail::WaiterToken;

namespace {
// Temporary file path for the ThreadPool real-syscall case (mirrors the C2c
// waiter-borrow test).
struct TempPath {
    TempPath() : path_((std::filesystem::temp_directory_path() /
                        ("sluice_f1_" + std::to_string(++counter_))).string()) {}
    ~TempPath() {
        std::error_code ec;
        std::filesystem::remove(path_, ec);
    }
    TempPath(const TempPath&) = delete;
    TempPath& operator=(const TempPath&) = delete;
    const std::string& path() const { return path_; }

private:
    std::string path_;
    static inline long counter_ = 0;
};

// 64 KiB scratch stack for an E4 fiber. 16-byte aligned (mirrors
// evented_scheduler_test).
struct FiberStack {
    static constexpr std::size_t kBytes = 64 * 1024;
    alignas(16) std::vector<std::byte> bytes{kBytes};
    std::byte* base() noexcept { return bytes.data(); }
    std::size_t size() const noexcept { return bytes.size(); }
};
}  // namespace

// ---- T1: the production Scheduler routes via the Scheduler-owned ReadySink
// (identity path), with no legacy Completion*-keyed scan involvement --------
SLUICE_TEST_CASE(f1_scheduler_routes_via_ready_sink) {
    if constexpr (!fiber_ctx::supported) return;

    auto backend_up = std::make_unique<FakeAsyncBackend>();
    FakeAsyncBackend* backend = backend_up.get();
    AsyncIoContext ctx(std::move(backend_up));
    Scheduler sched(ctx);

    Completion<std::size_t> c;
    std::byte buf[8]{};
    int resumed = 0;
    std::size_t observed = 0;

    Fiber fa;
    fa.set_entry([&](Fiber&) {
        SLUICE_CHECK(ctx.submit_read(ReadOp{-1, buf, 8, 0}, c).has_value());
        auto r = sched.await_completion_size(c);
        SLUICE_CHECK_MSG(r.has_value(), "identity await must succeed");
        resumed = 1;
        observed = c.result().value_or(0);
    });
    FiberStack sa;
    SLUICE_CHECK(sched.init_fiber(fa, sa.base(), sa.size()));
    sched.spawn(fa);

    // The fiber suspends on the identity path: the legacy fallback maps must
    // NOT hold the registration.
    sched.run_until_idle();
    SLUICE_CHECK(resumed == 0);
    SLUICE_CHECK_MSG(AsyncTestAccess::legacy_completion_wait_count(sched) == 0,
                     "registration must be identity, not a Completion*-keyed entry");
    SLUICE_CHECK(AsyncTestAccess::wait_registry_live_count(sched) == 1);

    backend->complete_oldest_with_bytes(8);
    sched.run_until_idle();

    SLUICE_CHECK(resumed == 1);
    SLUICE_CHECK(observed == 8);
    // The identity route fired exactly once: the Scheduler-owned sink
    // delivered the ReadyEvent and marked the record routed.
    SLUICE_CHECK(AsyncTestAccess::ready_sink_deliveries(sched) == 1);
    SLUICE_CHECK(AsyncTestAccess::ready_sink_routed(sched) == 1);
    SLUICE_CHECK(AsyncTestAccess::ready_sink_stale_dropped(sched) == 0);
    SLUICE_CHECK(AsyncTestAccess::ready_sink_cancel_lost(sched) == 0);
    // The registry drained; the Completion published normally.
    SLUICE_CHECK(AsyncTestAccess::wait_registry_live_count(sched) == 0);
    SLUICE_CHECK(c.ready());
    c.reset();
}

// ---- T2: completion/reap before waiter registration -> inline return, no
// lost wake (Race A). --------------------------------------------------------
SLUICE_TEST_CASE(f1_completion_before_waiter_registration_no_lost_wake) {
    if constexpr (!fiber_ctx::supported) return;

    auto backend_up = std::make_unique<FakeAsyncBackend>();
    FakeAsyncBackend* backend = backend_up.get();
    AsyncIoContext ctx(std::move(backend_up));
    Scheduler sched(ctx);

    Completion<std::size_t> c;
    std::byte buf[8]{};
    std::atomic<bool> submitted{false};
    std::atomic<bool> completed{false};
    int resumed = 0;
    std::size_t observed = 0;

    Fiber fa;
    fa.set_entry([&](Fiber&) {
        SLUICE_CHECK(ctx.submit_read(ReadOp{-1, buf, 8, 0}, c).has_value());
        submitted.store(true, std::memory_order::release);
        // Suspend until the coordinator has completed AND reaped the op, so
        // the completion strictly precedes the waiter registration. (The flag
        // wait parks the run on the wake source; the coordinator's
        // SchedulerWakeHandle::notify is the deterministic external wake —
        // E9 protocol, not a sleep.)
        sched.await_ready_flag(completed);
        auto r = sched.await_completion_size(c);
        SLUICE_CHECK_MSG(r.has_value(), "await on an already-ready Completion must return inline");
        resumed = 1;
        observed = c.result().value_or(0);
    });
    FiberStack sa;
    SLUICE_CHECK(sched.init_fiber(fa, sa.base(), sa.size()));
    sched.spawn(fa);

    auto wake_handle = sched.make_wake_handle();
    std::thread coordinator([&] {
        // Wait for the fiber's submit (the handshake — no sleeps), then
        // complete + reap the op while the fiber is suspended on the flag:
        // the completion is READY before the waiter registration.
        while (!submitted.load(std::memory_order::acquire)) {
            std::this_thread::yield();
        }
        backend->complete_oldest_with_bytes(8);
        (void)ctx.poll();
        completed.store(true, std::memory_order::release);
        (void)wake_handle.notify();  // deterministic wake of the parked run
    });
    // LIVE run: the flag wait is an external-wake-capable registration, so a
    // Drain-mode run may return STALLED at the MW-S3 boundary while the fiber
    // still waits (losing the notify). Live keeps the run resident on the wake
    // source (E9 external-wake protocol, cf. external_wake_test) — the notify
    // is always observed and the run returns only when the fiber is done.
    sched.run_live(1);
    coordinator.join();

    SLUICE_CHECK(resumed == 1);
    SLUICE_CHECK(observed == 8);
    // No waiter was ever delivered: the registration lost to reap, and the
    // fiber returned inline (no suspend, no wake needed — no lost wake).
    SLUICE_CHECK(AsyncTestAccess::ready_sink_deliveries(sched) == 0);
    SLUICE_CHECK(AsyncTestAccess::wait_registry_live_count(sched) == 0);
    c.reset();
}

// ---- T3/T8: waiter before completion -> wake exactly once; a repeat reap
// delivers nothing and never double-wakes. ----------------------------------
SLUICE_TEST_CASE(f1_waiter_before_completion_wake_exactly_once) {
    if constexpr (!fiber_ctx::supported) return;

    auto backend_up = std::make_unique<FakeAsyncBackend>();
    FakeAsyncBackend* backend = backend_up.get();
    AsyncIoContext ctx(std::move(backend_up));
    Scheduler sched(ctx);

    Completion<std::size_t> c;
    std::byte buf[8]{};
    int resumes = 0;
    std::size_t observed = 0;

    Fiber fa;
    fa.set_entry([&](Fiber&) {
        SLUICE_CHECK(ctx.submit_read(ReadOp{-1, buf, 8, 0}, c).has_value());
        auto r = sched.await_completion_size(c);
        SLUICE_CHECK(r.has_value());
        ++resumes;
        observed = c.result().value_or(0);
    });
    FiberStack sa;
    SLUICE_CHECK(sched.init_fiber(fa, sa.base(), sa.size()));
    sched.spawn(fa);
    sched.run_until_idle();
    SLUICE_CHECK(resumes == 0);

    backend->complete_oldest_with_bytes(8);
    sched.run_until_idle();
    SLUICE_CHECK(resumes == 1);
    SLUICE_CHECK(observed == 8);
    SLUICE_CHECK(AsyncTestAccess::ready_sink_deliveries(sched) == 1);
    SLUICE_CHECK(AsyncTestAccess::ready_sink_routed(sched) == 1);

    // Duplicate reap / repeat poll: nothing more is delivered, the fiber is
    // not woken a second time (single delivery per registration, I16/I4).
    (void)ctx.poll();
    sched.run_until_idle();
    SLUICE_CHECK(resumes == 1);
    SLUICE_CHECK(AsyncTestAccess::ready_sink_deliveries(sched) == 1);
    SLUICE_CHECK(AsyncTestAccess::wait_registry_live_count(sched) == 0);
    c.reset();
}

// ---- T4: cancel_waiter before completion removes ONLY the waiter (I5); the
// I/O still terminals + reaps; the Completion publishes normally. -----------
SLUICE_TEST_CASE(f1_cancel_waiter_keeps_io) {
    if constexpr (!fiber_ctx::supported) return;

    auto backend_up = std::make_unique<FakeAsyncBackend>();
    FakeAsyncBackend* backend = backend_up.get();
    AsyncIoContext ctx(std::move(backend_up));
    Scheduler sched(ctx);

    Completion<std::size_t> c;
    std::byte buf[8]{};
    int outcome = 0;  // 1 = success, 2 = wait-cancelled
    std::size_t observed = 0;

    Fiber fa;
    fa.set_entry([&](Fiber&) {
        SLUICE_CHECK(ctx.submit_read(ReadOp{-1, buf, 8, 0}, c).has_value());
        auto r = sched.await_completion_size(c);
        if (r.has_value()) {
            outcome = 1;
            observed = c.result().value_or(0);
        } else if (r.error().code == IoError::Code::canceled) {
            outcome = 2;
        } else {
            outcome = 3;
        }
    });
    FiberStack sa;
    SLUICE_CHECK(sched.init_fiber(fa, sa.base(), sa.size()));
    sched.spawn(fa);
    sched.run_until_idle();
    SLUICE_CHECK(outcome == 0);  // fiber suspended

    // Waiter cancellation (production caller shape): removes the waiter only.
    auto cw = sched.cancel_waiter(c);
    SLUICE_CHECK_MSG(cw.has_value(), "cancel_waiter must succeed while registered");
    SLUICE_CHECK_MSG(cw.value() == true,
                     "this call must win the waiter (the reap has not run)");
    sched.run_until_idle();

    // The fiber resumed with the wait-cancelled outcome — NOT via the
    // identity route (routed == 0) and NOT via the completion (not ready).
    SLUICE_CHECK(outcome == 2);
    SLUICE_CHECK(!c.ready());
    SLUICE_CHECK(AsyncTestAccess::ready_sink_routed(sched) == 0);
    SLUICE_CHECK(AsyncTestAccess::wait_registry_live_count(sched) == 0);

    // The I/O is untouched: it still completes + reaps; the Completion
    // publishes normally; the sink sees no waiter delivery.
    SLUICE_CHECK(c.outstanding());
    backend->complete_oldest_with_bytes(8);
    (void)ctx.poll();
    SLUICE_CHECK(c.ready());
    SLUICE_CHECK(c.result().value_or(0) == 8);
    SLUICE_CHECK(AsyncTestAccess::ready_sink_deliveries(sched) == 0);
    c.reset();
}

// ---- T5: cancel_waiter racing reap (high iteration). Exactly one legal
// outcome per iteration; no double route, no lost protocol state, no UAF. ---
SLUICE_TEST_CASE(f1_cancel_waiter_vs_reap_race) {
    if constexpr (!fiber_ctx::supported) return;
    constexpr int kIters = 200;
    for (int i = 0; i < kIters; ++i) {
        auto backend_up = std::make_unique<FakeAsyncBackend>();
        FakeAsyncBackend* backend = backend_up.get();
        AsyncIoContext ctx(std::move(backend_up));
        Scheduler sched(ctx);

        Completion<std::size_t> c;
        std::byte buf[8]{};
        int outcome = 0;  // 1 = success (reap won), 2 = canceled (cancel won)
        Fiber fa;
        fa.set_entry([&](Fiber&) {
            SLUICE_CHECK(ctx.submit_read(ReadOp{-1, buf, 8, 0}, c).has_value());
            auto r = sched.await_completion_size(c);
            if (r.has_value()) {
                outcome = 1;
            } else if (r.error().code == IoError::Code::canceled) {
                outcome = 2;
            } else {
                outcome = 3;
            }
        });
        FiberStack sa;
        SLUICE_CHECK(sched.init_fiber(fa, sa.base(), sa.size()));
        sched.spawn(fa);
        sched.run_until_idle();
        SLUICE_CHECK(outcome == 0);

        // Race: the cancel path (arena leaf via G) vs the reap path (arena
        // leaf via access_mtx_). The arena leaf arbitrates exactly-once.
        std::atomic<bool> go{false};
        int cancel_result = 0;  // 1 = won, 2 = lost (delivery already won)
        std::thread canceler([&] {
            while (!go.load(std::memory_order_acquire)) std::this_thread::yield();
            auto cw = sched.cancel_waiter(c);
            cancel_result = !cw.has_value() ? 3
                           : cw.value()     ? 1
                                            : 2;
        });
        std::thread completer([&] {
            while (!go.load(std::memory_order_acquire)) std::this_thread::yield();
            backend->complete_oldest_with_bytes(8);
            (void)ctx.poll();
        });
        go.store(true, std::memory_order::release);
        canceler.join();
        completer.join();

        sched.run_until_idle();  // route whichever path won
        SLUICE_CHECK_MSG(outcome == 1 || outcome == 2,
                         "exactly one legal outcome (reap-won or cancel-won)");
        // Protocol consistency across the arena winner (P1-1 strict):
        //   - reap won (cancel_result == 2): the sink delivered exactly
        //     once, the frozen outcome is completed, and the fiber
        //     observed success.
        //   - cancel won (cancel_result == 1): the sink never delivered,
        //     the frozen outcome is canceled, and the fiber observed
        //     canceled — regardless of whether c.ready() happened to be
        //     true at resume time.
        SLUICE_CHECK(AsyncTestAccess::ready_sink_routed(sched) <= 1);
        if (cancel_result == 2) {  // reap won the arena extraction
            SLUICE_CHECK(AsyncTestAccess::ready_sink_routed(sched) == 1);
            SLUICE_CHECK(outcome == 1);
            SLUICE_CHECK(c.ready());
            SLUICE_CHECK(c.result().value_or(0) == 8);
        } else if (cancel_result == 1) {  // cancel won the arena extraction
            SLUICE_CHECK(AsyncTestAccess::ready_sink_routed(sched) == 0);
            // P1-1 strict: cancel won => outcome is ALWAYS canceled.
            // The frozen CompletionWaitOutcome eliminates the race where
            // c.ready() could be true at resume time.
            SLUICE_CHECK(outcome == 2);
        } else {
            SLUICE_CHECK_MSG(false, "cancel_waiter must return true or false");
        }
        // The registry drained; the completion always terminals + reaps
        // exactly once (wait-cancel never touches the I/O).
        SLUICE_CHECK(AsyncTestAccess::wait_registry_live_count(sched) == 0);
        if (!c.ready()) {
            backend->complete_oldest_with_bytes(8);
            (void)ctx.poll();
            SLUICE_CHECK(c.ready());
            SLUICE_CHECK(c.result().value_or(0) == 8);
        }
        c.reset();
    }
    // Note: no "both race outcomes exercised" statistical assertion here. The
    // fake backend's terminal record lands in microseconds, so the completer
    // systematically beats the canceler's G->access_mtx_->arena-leaf path —
    // which side wins a given iteration is scheduling, not semantics (AGENTS.md
    // §18). Each winner is deterministically covered elsewhere: reap-won by
    // T1/T3, cancel-won by T4; this soak proves the race itself is exactly-once
    // and protocol-consistent on either winner.
}

// ---- T6: stale record generation cannot wake a new occupant (Race C). ----
SLUICE_TEST_CASE(f1_stale_record_generation_no_wake) {
    if constexpr (!fiber_ctx::supported) return;

    // Capacity 1 forces slot reuse: request B reuses request A's slot.
    auto backend_up = std::make_unique<FakeAsyncBackend>(/*capacity=*/1);
    FakeAsyncBackend* backend = backend_up.get();
    AsyncIoContext ctx(std::move(backend_up));
    Scheduler sched(ctx);

    Completion<std::size_t> cA;
    Completion<std::size_t> cB;
    std::byte buf[8]{};
    std::atomic<bool> a_done{false};
    int b_resumed = 0;

    Fiber fa;
    fa.set_entry([&](Fiber&) {
        SLUICE_CHECK(ctx.submit_read(ReadOp{-1, buf, 8, 0}, cA).has_value());
        auto r = sched.await_completion_size(cA);
        SLUICE_CHECK(r.has_value());
        cA.reset();  // release the slot; generation increments
        a_done.store(true, std::memory_order::release);
    });
    FiberStack sa;
    SLUICE_CHECK(sched.init_fiber(fa, sa.base(), sa.size()));

    Fiber fb;
    fb.set_entry([&](Fiber&) {
        // Reuses the released slot (generation N+1) and a fresh record.
        SLUICE_CHECK(ctx.submit_read(ReadOp{-1, buf, 8, 0}, cB).has_value());
        auto r = sched.await_completion_size(cB);
        SLUICE_CHECK(r.has_value());
        ++b_resumed;
    });
    FiberStack sb;
    SLUICE_CHECK(sched.init_fiber(fb, sb.base(), sb.size()));

    sched.spawn(fa);
    sched.run_until_idle();
    backend->complete_oldest_with_bytes(8);
    sched.run_until_idle();
    SLUICE_CHECK(a_done.load(std::memory_order::acquire));  // A routed + released

    sched.spawn(fb);
    sched.run_until_idle();
    SLUICE_CHECK(b_resumed == 0);  // B suspended

    // Forge a STALE delivery: the token A used (record 0, generation 0) must
    // not route B (record 0, generation 1). The sink drops it (stale
    // generation) and the lease is acknowledged without routing.
    const std::uint64_t ident = AsyncTestAccess::scheduler_identity(sched);
    const WaiterToken stale_token{ident, /*registration_slot=*/0,
                                  /*registration_generation=*/0};
    RoutingLease stale_lease = RoutingLease::pinning(9999, 0, 0);
    AsyncTestAccess::ready_sink(sched).on_ready(ReadyEvent{
        /*key=*/{}, /*kind=*/sluice::async::detail::OperationKind::read,
        OptionalWaiterDelivery::of(stale_token, std::move(stale_lease))});

    // B must NOT be woken by the stale event...
    sched.run_until_idle();
    SLUICE_CHECK(b_resumed == 0);
    SLUICE_CHECK(AsyncTestAccess::ready_sink_stale_dropped(sched) == 1);
    // ...and its real completion still routes it normally.
    backend->complete_oldest_with_bytes(8);
    sched.run_until_idle();
    SLUICE_CHECK(b_resumed == 1);
    SLUICE_CHECK(AsyncTestAccess::wait_registry_live_count(sched) == 0);
    cB.reset();
}

// ---- T7: duplicate waiter on the Scheduler path -> synchronous
// invalid_state; the first waiter is untouched (I13). ------------------------
SLUICE_TEST_CASE(f1_duplicate_waiter_invalid_state) {
    if constexpr (!fiber_ctx::supported) return;

    auto backend_up = std::make_unique<FakeAsyncBackend>();
    FakeAsyncBackend* backend = backend_up.get();
    AsyncIoContext ctx(std::move(backend_up));
    Scheduler sched(ctx);

    Completion<std::size_t> c;
    std::byte buf[8]{};
    int a_outcome = 0;
    int b_outcome = 0;  // 1 = invalid_state observed synchronously
    std::size_t a_observed = 0;

    Fiber fa;
    fa.set_entry([&](Fiber&) {
        SLUICE_CHECK(ctx.submit_read(ReadOp{-1, buf, 8, 0}, c).has_value());
        auto r = sched.await_completion_size(c);
        if (r.has_value()) {
            a_outcome = 1;
            a_observed = c.result().value_or(0);
        } else {
            a_outcome = 2;
        }
    });
    FiberStack sa;
    SLUICE_CHECK(sched.init_fiber(fa, sa.base(), sa.size()));

    Fiber fb;
    fb.set_entry([&](Fiber&) {
        // Second waiter on the SAME Completion: the arena rejects it with
        // invalid_state without overwriting the first; the Scheduler surfaces
        // it synchronously (the fiber is NOT suspended).
        auto r = sched.await_completion_size(c);
        if (!r.has_value() && r.error().code == IoError::Code::invalid_state) {
            b_outcome = 1;
        } else {
            b_outcome = 2;
        }
    });
    FiberStack sb;
    SLUICE_CHECK(sched.init_fiber(fb, sb.base(), sb.size()));

    sched.spawn(fa);
    sched.spawn(fb);
    sched.run_until_idle();

    // B observed the synchronous invalid_state without suspending.
    SLUICE_CHECK(b_outcome == 1);
    // A is still registered (single-waiter, no overwrite).
    SLUICE_CHECK(a_outcome == 0);
    SLUICE_CHECK(AsyncTestAccess::wait_registry_live_count(sched) == 1);

    // The first waiter still gets its delivery.
    backend->complete_oldest_with_bytes(8);
    sched.run_until_idle();
    SLUICE_CHECK(a_outcome == 1);
    SLUICE_CHECK(a_observed == 8);
    SLUICE_CHECK(AsyncTestAccess::ready_sink_routed(sched) == 1);
    SLUICE_CHECK(AsyncTestAccess::wait_registry_live_count(sched) == 0);
    c.reset();
}

// ---- T9: shutdown convergence — a pending delivery is drained before
// destruction; the registry is empty when the Scheduler dies. ---------------
SLUICE_TEST_CASE(f1_shutdown_convergence_registry_empty) {
    if constexpr (!fiber_ctx::supported) return;

    auto backend_up = std::make_unique<FakeAsyncBackend>();
    FakeAsyncBackend* backend = backend_up.get();
    AsyncIoContext ctx(std::move(backend_up));
    Scheduler sched(ctx);

    Completion<std::size_t> c;
    std::byte buf[8]{};
    int resumed = 0;
    Fiber fa;
    fa.set_entry([&](Fiber&) {
        SLUICE_CHECK(ctx.submit_read(ReadOp{-1, buf, 8, 0}, c).has_value());
        auto r = sched.await_completion_size(c);
        SLUICE_CHECK(r.has_value());
        ++resumed;
    });
    FiberStack sa;
    SLUICE_CHECK(sched.init_fiber(fa, sa.base(), sa.size()));
    sched.spawn(fa);
    sched.run_until_idle();

    // The stop-equivalent sequence: the backend completes; the last drain
    // routes the delivery before quiescence. (The runtime-level control-wake
    // path is covered by the existing D4-RM13/14 + runtime drain tests.)
    backend->complete_oldest_with_bytes(8);
    sched.run_until_idle();
    SLUICE_CHECK(resumed == 1);
    SLUICE_CHECK(AsyncTestAccess::wait_registry_live_count(sched) == 0);
    c.reset();
    // ~Scheduler asserts the registry is empty — reaching here proves it.
}

// ---- T10a: SyncBackend routes through the same identity contract. --------
SLUICE_TEST_CASE(f1_routing_sync_backend) {
    if constexpr (!fiber_ctx::supported) return;

    auto backend_up = std::make_unique<SyncBackend>();
    AsyncIoContext ctx(std::move(backend_up));
    Scheduler sched(ctx);

    Completion<std::size_t> c;
    std::byte buf[8]{};
    int resumed = 0;
    std::size_t observed = 0;
    Fiber fa;
    fa.set_entry([&](Fiber&) {
        SLUICE_CHECK(ctx.submit_read(ReadOp{-1, buf, 8, 0}, c).has_value());
        auto r = sched.await_completion_size(c);
        SLUICE_CHECK(r.has_value());
        resumed = 1;
        observed = c.result().value_or(0);
    });
    FiberStack sa;
    SLUICE_CHECK(sched.init_fiber(fa, sa.base(), sa.size()));
    sched.spawn(fa);
    sched.run_until_idle();
    SLUICE_CHECK(resumed == 1);
    SLUICE_CHECK(observed == 8);
    SLUICE_CHECK(AsyncTestAccess::ready_sink_routed(sched) == 1);
    SLUICE_CHECK(AsyncTestAccess::legacy_completion_wait_count(sched) == 0);
    c.reset();
}

// ---- T10b: ThreadPoolBackend routes through the same identity contract
// (the real syscall completes on a worker; the terminal is an ordinary error
// for fd -1 — routing is what is proven). ------------------------------------
SLUICE_TEST_CASE(f1_routing_threadpool_backend) {
    if constexpr (!fiber_ctx::supported) return;

    // A real fd: the blocking syscall executes on a ThreadPool worker and
    // completes with a real result; the identity route wakes the fiber.
    TempPath tmp;
    int fd = ::open(tmp.path().c_str(), O_RDWR | O_CREAT | O_TRUNC, 0644);
    SLUICE_CHECK(fd >= 0);
    std::byte seed[8] = {std::byte{1}, std::byte{2}, std::byte{3}, std::byte{4},
                       std::byte{5}, std::byte{6}, std::byte{7}, std::byte{8}};
    SLUICE_CHECK(::write(fd, seed, sizeof(seed)) == static_cast<ssize_t>(sizeof(seed)));

    auto backend_up = std::make_unique<ThreadPoolBackend>();
    AsyncIoContext ctx(std::move(backend_up));
    Scheduler sched(ctx);

    Completion<std::size_t> c;
    std::byte buf[8]{};
    int resumed = 0;
    std::size_t observed = 0;
    Fiber fa;
    fa.set_entry([&](Fiber&) {
        SLUICE_CHECK(ctx.submit_read(ReadOp{fd, buf, 8, 0}, c).has_value());
        auto r = sched.await_completion_size(c);
        SLUICE_CHECK(r.has_value());
        resumed = 1;
        observed = c.result().value_or(0);
    });
    FiberStack sa;
    SLUICE_CHECK(sched.init_fiber(fa, sa.base(), sa.size()));
    sched.spawn(fa);
    sched.run_until_idle();
    SLUICE_CHECK(resumed == 1);
    SLUICE_CHECK(observed == 8);
    SLUICE_CHECK(AsyncTestAccess::ready_sink_routed(sched) == 1);
    SLUICE_CHECK(AsyncTestAccess::legacy_completion_wait_count(sched) == 0);
    c.reset();
    ::close(fd);
}

SLUICE_MAIN()
