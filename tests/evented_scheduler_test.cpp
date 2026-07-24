// E4 — single-worker Evented scheduler cycle tests (sluice-CORE-E4).
//
// Proves ADR-execution-model §9.1 (scheduler liveness): with ONE scheduler
// worker, Fiber B makes progress while Fiber A awaits a pending async op.
// Uses the existing FakeAsyncBackend held-pending mode as the deterministic
// completion source — no real I/O, no threads, no timing races. Single OS
// thread; fibers run cooperatively.
//
// PROVES: scheduler liveness + completion wake path + resume fidelity +
//         exactly-once runnable transition.
// DOES NOT PROVE: Evented I/O beyond this single-worker model; Future/
//                 WaitPolicy integration; Group-on-Evented; multi-worker.
#include "harness.hpp"

#include <sluice/async/async_io_context.hpp>
#include <sluice/async/completion.hpp>
#include <sluice/async/fake_backend.hpp>
#include <sluice/async/fiber.hpp>
#include <sluice/async/fiber_ctx.hpp>
#include <sluice/async/scheduler.hpp>
#include <sluice/error.hpp>
#include <sluice/result.hpp>

#include <cstddef>
#include <cstdint>
#include <vector>

using namespace sluice::async;

namespace {
// 64 KiB scratch stack for an E4 fiber. 16-byte aligned.
struct FiberStack {
    static constexpr std::size_t kBytes = 64 * 1024;
    alignas(16) std::vector<std::byte> bytes{kBytes};
    std::byte* base() noexcept { return bytes.data(); }
    std::size_t size() const noexcept { return bytes.size(); }
};
}  // namespace

// ---- E4-T1: single-worker scheduler liveness (PRIMARY GATE) ---------------
// Fiber A submits a held-pending op and suspends. Fiber B runs and records
// progress while A's op is STILL PENDING. This proves A returned the worker to
// the scheduler (one worker, B could not have run otherwise).
SLUICE_TEST_CASE(sched_single_worker_scheduler_liveness) {
    if constexpr (!fiber_ctx::supported) return;

    auto backend_up = std::make_unique<FakeAsyncBackend>();
    FakeAsyncBackend* backend = backend_up.get();  // retained for cleanup
    AsyncIoContext ctx(std::move(backend_up));
    Scheduler sched(ctx);
    Completion<std::size_t> a_c;
    std::byte a_buf[4]{};
    int b_progress = 0;
    int a_resumed = 0;
    int a_observed = -1;
    std::uint64_t a_pre_token = 0;
    std::uint64_t a_post_token = 0;
    bool a_op_pending_at_b_time = false;

    // Fiber A: submit the read (fake holds it pending), suspend, observe result.
    Fiber fiber_a;
    fiber_a.set_entry([&](Fiber&) {
        SLUICE_CHECK(ctx.submit_read(ReadOp{-1, a_buf, 4, 0}, a_c).has_value());
        SLUICE_CHECK(a_c.outstanding());
        a_pre_token = 0xABCD1234;  // local that must survive suspension
        sched.await_completion_size(a_c);
        // RESUMED here after the scheduler observed completion.
        a_post_token = a_pre_token;
        a_resumed = 1;
        if (a_c.ready()) a_observed = static_cast<int>(a_c.result().value_or(0));
    });
    FiberStack a_stack;
    SLUICE_CHECK(sched.init_fiber(fiber_a, a_stack.base(), a_stack.size()));

    // Fiber B: records progress while A is pending. The fake has NOT had
    // complete_* called, so A's op is still outstanding when B runs.
    Fiber fiber_b;
    fiber_b.set_entry([&](Fiber&) {
        // Observe: A's op is still pending (not completed).
        a_op_pending_at_b_time = a_c.outstanding();
        b_progress = 1;
    });
    FiberStack b_stack;
    SLUICE_CHECK(sched.init_fiber(fiber_b, b_stack.base(), b_stack.size()));

    sched.spawn(fiber_a);
    sched.spawn(fiber_b);
    sched.run_until_idle();

    // PRIMARY GATE: B ran while A's op was pending.
    SLUICE_CHECK(b_progress == 1);
    SLUICE_CHECK(a_op_pending_at_b_time);  // A was still pending when B ran

    // A has NOT resumed yet (the test never released its op).
    SLUICE_CHECK(a_resumed == 0);
    SLUICE_CHECK(a_c.outstanding());  // still pending

    // Cleanup for the context's L11 teardown invariant: release A's op, run one
    // more cycle so A wakes + resumes + its Completion leaves outstanding. (The
    // resume path itself is asserted in T2; here we only drain for teardown.)
    backend->complete_oldest_with_bytes(4);
    sched.run_until_idle();
    SLUICE_CHECK(!a_c.outstanding());
}

// ---- E4-T2: completion resumes the waiting fiber --------------------------
// After T1's pending state, release A's op; the scheduler observes the
// completion and resumes A at the exact suspension point.
SLUICE_TEST_CASE(sched_completion_resumes_waiting_fiber) {
    if constexpr (!fiber_ctx::supported) return;

    auto backend_up = std::make_unique<FakeAsyncBackend>();
    FakeAsyncBackend* backend = backend_up.get();
    AsyncIoContext ctx(std::move(backend_up));
    Scheduler sched(ctx);

    Completion<std::size_t> a_c;
    std::byte a_buf[4]{};
    int a_resumed = 0;
    int a_observed = -1;
    std::uint64_t a_pre = 0, a_post = 0;
    bool b_ran_before_a_resume = false;
    int b_progress = 0;

    Fiber fiber_a;
    fiber_a.set_entry([&](Fiber&) {
        SLUICE_CHECK(ctx.submit_read(ReadOp{-1, a_buf, 4, 0}, a_c).has_value());
        a_pre = 0xBEEF;
        sched.await_completion_size(a_c);
        a_post = a_pre;       // RESUME FIDELITY: local survived
        a_resumed = 1;
        if (a_c.ready()) a_observed = static_cast<int>(a_c.result().value_or(0));
    });
    Fiber fiber_b;
    fiber_b.set_entry([&](Fiber&) {
        b_ran_before_a_resume = (a_resumed == 0);  // B runs before A resumes
        b_progress = 1;
        // Release A's op: stage a 4-byte completion. The scheduler's next
        // wake_ready_completions() polls ctx, observes the ready Completion,
        // and wakes A.
        backend->complete_oldest_with_bytes(4);
    });
    FiberStack a_stack, b_stack;
    SLUICE_CHECK(sched.init_fiber(fiber_a, a_stack.base(), a_stack.size()));
    SLUICE_CHECK(sched.init_fiber(fiber_b, b_stack.base(), b_stack.size()));
    sched.spawn(fiber_a);
    sched.spawn(fiber_b);
    sched.run_until_idle();

    SLUICE_CHECK(b_progress == 1);
    SLUICE_CHECK(b_ran_before_a_resume);
    SLUICE_CHECK(a_resumed == 1);
    SLUICE_CHECK(a_observed == 4);          // A received the released result
    SLUICE_CHECK(a_post == 0xBEEF);          // resume fidelity
    SLUICE_CHECK(a_c.ready());
    SLUICE_CHECK(fiber_a.state() == FiberState::done);
}

// ---- E4-T3: exactly-once runnable transition ------------------------------
// One completion must enqueue the waiting fiber exactly once. Repeated polls
// after the wake must NOT re-enqueue. We observe via the runnable queue being
// drained and the fiber reaching `done` exactly once (entry runs once).
SLUICE_TEST_CASE(sched_exactly_once_runnable_transition) {
    if constexpr (!fiber_ctx::supported) return;

    auto backend_up = std::make_unique<FakeAsyncBackend>();
    FakeAsyncBackend* backend = backend_up.get();
    AsyncIoContext ctx(std::move(backend_up));
    Scheduler sched(ctx);

    Completion<std::size_t> a_c;
    std::byte a_buf[4]{};
    int a_entry_runs = 0;       // counts how many times the body resumes past await
    int a_suspension_resumes = 0;

    Fiber fiber_a;
    fiber_a.set_entry([&](Fiber&) {
        ++a_entry_runs;
        SLUICE_CHECK(ctx.submit_read(ReadOp{-1, a_buf, 4, 0}, a_c).has_value());
        sched.await_completion_size(a_c);
        ++a_suspension_resumes;  // should fire exactly once
    });
    // Release A's op from the scheduler's perspective: we arrange it so that
    // when A suspends, the very next wake_ready_completions sees a ready op.
    // We use a second fiber that releases A before A even suspends is racy;
    // instead, release via the fake's auto mode is cleaner. Switch the fake
    // to auto_bytes(4) AFTER A submits — but A submits inside its body. So:
    // we run A once via a tiny driver fiber that submits A's op, releases it,
    // then A's await sees it ready. Simpler: spawn a releaser fiber that runs
    // first, but A must submit before release. Use TWO-stage: releaser fiber
    // waits one yield, then releases. For E4-T3 simplicity, use auto_bytes.
    backend->auto_bytes(4);  // every outstanding op completes with 4 bytes

    FiberStack a_stack;
    SLUICE_CHECK(sched.init_fiber(fiber_a, a_stack.base(), a_stack.size()));
    sched.spawn(fiber_a);
    sched.run_until_idle();

    // The body's await saw the completion exactly once.
    SLUICE_CHECK(a_suspension_resumes == 1);
    SLUICE_CHECK(a_entry_runs == 1);       // entry did not restart
    SLUICE_CHECK(fiber_a.state() == FiberState::done);
    SLUICE_CHECK(sched.waiting_count() == 0);  // association cleared on wake
    SLUICE_CHECK(sched.runnable_count() == 0); // queue drained
}

// ---- E4-T4: a runnable task is not starved by a pending operation ---------
// With A pending and B runnable, the single worker runs B without waiting for
// A. (Structurally overlaps T1, but the contract is made explicit here.)
SLUICE_TEST_CASE(sched_runnable_not_starved_by_pending) {
    if constexpr (!fiber_ctx::supported) return;

    auto backend_up = std::make_unique<FakeAsyncBackend>();
    FakeAsyncBackend* backend = backend_up.get();  // retained for cleanup
    AsyncIoContext ctx(std::move(backend_up));
    Scheduler sched(ctx);

    Completion<std::size_t> a_c;
    std::byte a_buf[4]{};
    int b_progress = 0;
    bool a_pending_when_b_ran = false;

    Fiber fiber_a;
    fiber_a.set_entry([&](Fiber&) {
        SLUICE_CHECK(ctx.submit_read(ReadOp{-1, a_buf, 4, 0}, a_c).has_value());
        sched.await_completion_size(a_c);  // never released in this test
    });
    Fiber fiber_b;
    fiber_b.set_entry([&](Fiber&) {
        a_pending_when_b_ran = a_c.outstanding();
        b_progress = 1;
    });
    FiberStack a_stack, b_stack;
    SLUICE_CHECK(sched.init_fiber(fiber_a, a_stack.base(), a_stack.size()));
    SLUICE_CHECK(sched.init_fiber(fiber_b, b_stack.base(), b_stack.size()));
    sched.spawn(fiber_a);
    sched.spawn(fiber_b);
    sched.run_until_idle();

    // B ran; A is still pending (never released) — not starved.
    SLUICE_CHECK(b_progress == 1);
    SLUICE_CHECK(a_pending_when_b_ran);
    SLUICE_CHECK(a_c.outstanding());

    // Cleanup for the L11 teardown invariant (A's op is still outstanding).
    backend->complete_oldest_with_bytes(4);
    sched.run_until_idle();
    SLUICE_CHECK(!a_c.outstanding());
}

SLUICE_MAIN()
