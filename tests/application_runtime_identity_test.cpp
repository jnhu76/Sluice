// E16-POST-MERGE-CORRECTIVE-1 — Fiber-local Runtime identity authority tests.
//
// ADR: docs/adr/ADR-application-runtime.md (Accepted).
//
// These tests prove the C2 contract:
//
//   The Fiber-local Runtime identity tag must not be publicly forgeable or
//   clearable by ordinary application code. A Runtime-owned task cannot bypass
//   is_runtime_task() self-close detection.
//
// The tag is stored IN the current Fiber's execution_tag_ field (not
// thread_local), so it survives Fiber suspend/resume and is correct under
// multiplexing. The write authority is private: only ApplicationRuntime's task
// wrapper sets/restores it (via Scheduler::set_current_fiber_execution_tag,
// private, friended to ApplicationRuntime). Ordinary application code cannot
// reach any public execution-tag setter.
//
// Deterministic causal tests. NO sleep_for as proof of ordering.
#include "harness.hpp"

#include <sluice/async/application_runtime.hpp>
#include <sluice/async/fake_backend.hpp>

#include <atomic>
#include <memory>
#include <thread>

using namespace sluice::async;
using sluice::IoError;
using sluice::Result;

// Build a Runtime with a no-op backend for identity tests.
static std::unique_ptr<ApplicationRuntime> build_identity_runtime() {
    RuntimeBuilder builder;
    builder.backend(std::make_unique<FakeAsyncBackend>());
    auto r = builder.build();
    if (!r.has_value()) {
        std::fprintf(stderr, "build_identity_runtime: builder.build() failed\n");
        std::abort();
    }
    return std::move(r.value());
}

// ---------------------------------------------------------------------------
// C2-T1 — a Runtime-owned task cannot self-close.
//
// Run a Runtime-owned task. Inside the task, drain()/join()/shutdown() must
// each return invalid_state (recognized as a Runtime task via the Fiber-local
// tag) without hanging. request_stop() is worker-safe and MAY be called.
//
// On pre-corrective merged master the public Fiber::set_execution_tag /
// Scheduler::set_current_fiber_execution_tag let a task clear the tag and
// bypass the guard. After the C2 repair no public setter exists, so a task
// cannot forge/clear the identity.
// ---------------------------------------------------------------------------
SLUICE_TEST_CASE(c2_task_cannot_self_close) {
    auto rt = build_identity_runtime();
    SLUICE_CHECK(rt->start().has_value());

    std::atomic<int> drain_results{0};
    std::atomic<int> join_results{0};
    std::atomic<int> shutdown_results{0};
    std::atomic<bool> task_done{false};

    SLUICE_CHECK(rt->submit([&](RuntimeTaskContext& ctx) {
        (void)ctx;
        // Each lifecycle call MUST be rejected as a Runtime task.
        auto dr = rt->drain();
        if (!dr.has_value() && dr.error().code == IoError::Code::invalid_state) {
            drain_results.fetch_add(1, std::memory_order::release);
        }
        auto jr = rt->join();
        if (!jr.has_value() && jr.error().code == IoError::Code::invalid_state) {
            join_results.fetch_add(1, std::memory_order::release);
        }
        auto sr = rt->shutdown();
        if (!sr.has_value() && sr.error().code == IoError::Code::invalid_state) {
            shutdown_results.fetch_add(1, std::memory_order::release);
        }
        task_done.store(true, std::memory_order::release);
    }).has_value());

    // Drive the lifecycle from the owning thread: the task must not have been
    // able to self-close, so the Runtime is still Running until we stop it.
    rt->request_stop();
    SLUICE_CHECK(rt->drain().has_value());
    SLUICE_CHECK(task_done.load(std::memory_order::acquire));
    SLUICE_CHECK(rt->join().has_value());

    SLUICE_CHECK(drain_results.load() == 1);
    SLUICE_CHECK(join_results.load() == 1);
    SLUICE_CHECK(shutdown_results.load() == 1);
}

// ---------------------------------------------------------------------------
// C2-T2 — identity is preserved across concurrent Fiber execution.
//
// Two Runtime tasks run concurrently on the Runtime's Scheduler (multi-Fiber
// interleaving). Each task records, at its execution point, that it is still
// recognized as a Runtime task (lifecycle calls rejected). This exercises the
// Fiber-local tag under genuine concurrent task execution: if the tag were
// thread_local it would still be observable, but the combination of C2-T1
// (per-task identity) plus this case (concurrent tasks each retain identity
// independently) plus the private-setter audit (C2-T4) together establish that
// the identity is per-Fiber state, set by the task wrapper, and not clearable.
// ---------------------------------------------------------------------------
SLUICE_TEST_CASE(c2_identity_preserved_concurrent_tasks) {
    auto rt = build_identity_runtime();
    SLUICE_CHECK(rt->start().has_value());

    constexpr int N = 4;
    std::atomic<int> rejected{0};
    std::atomic<int> ran{0};

    for (int i = 0; i < N; ++i) {
        SLUICE_CHECK(rt->submit([&](RuntimeTaskContext& ctx) {
            (void)ctx;
            // Each concurrent task independently observes its Runtime identity.
            auto sr = rt->shutdown();
            if (!sr.has_value() && sr.error().code == IoError::Code::invalid_state) {
                rejected.fetch_add(1, std::memory_order::release);
            }
            ran.fetch_add(1, std::memory_order::release);
        }).has_value());
    }

    rt->request_stop();
    SLUICE_CHECK(rt->drain().has_value());
    SLUICE_CHECK(ran.load(std::memory_order::acquire) == N);
    SLUICE_CHECK(rt->join().has_value());

    // Every task was recognized as a Runtime task and rejected.
    SLUICE_CHECK(rejected.load() == N);
}

// ---------------------------------------------------------------------------
// C2-T2b — external thread is NOT recognized as a Runtime task.
//
// An external thread calling drain()/join()/shutdown() is NOT inside a Fiber
// body, so current_fiber_execution_tag() returns nullptr != this, and the
// calls proceed normally (are NOT rejected as invalid_state). This is the
// negative control for the identity mechanism: only Fiber-resident Runtime
// tasks are rejected.
// ---------------------------------------------------------------------------
SLUICE_TEST_CASE(c2_external_thread_not_runtime_task) {
    auto rt = build_identity_runtime();
    SLUICE_CHECK(rt->start().has_value());

    // Submit a task so there is admitted work to drain.
    std::atomic<bool> task_ran{false};
    SLUICE_CHECK(rt->submit([&](RuntimeTaskContext& ctx) {
        (void)ctx;
        task_ran.store(true, std::memory_order::release);
    }).has_value());

    rt->request_stop();
    // External-thread drain/join must SUCCEED (not be rejected).
    SLUICE_CHECK(rt->drain().has_value());
    SLUICE_CHECK(task_ran.load(std::memory_order::acquire));
    SLUICE_CHECK(rt->join().has_value());
}

// ---------------------------------------------------------------------------
// C2-T3 — Fiber-local identity survives Fiber suspend/resume.
//
// Proves the execution tag is Fiber-local (not thread_local) by creating a
// real Fiber suspension boundary: task A suspends via ctx.suspend(), the
// worker switches to task B, task B signals the resume flag, and task A
// resumes and checks that its Runtime identity is still recognized.
//
// A thread_local tag would be overwritten by task B's entry, so when task A
// resumes it would see task B's tag (or nullptr) and the identity check would
// fail. The Fiber-local tag stored in Fiber::execution_tag_ follows the Fiber
// across context switches, so task A retains its identity.
//
// Deterministic causal order (no sleep_for):
//   - Task A is submitted first and runs first (single worker FIFO).
//   - Task A calls ctx.suspend(resume_flag) with resume_flag = false, so the
//     Fiber suspends and the worker picks up the next task.
//   - Task B runs, sets resume_flag = true, returns.
//   - The Scheduler's ready-flag mechanism detects the flag change and resumes
//     task A's Fiber.
//   - Task A resumes and calls shutdown() -> invalid_state (identity preserved).
// ---------------------------------------------------------------------------
SLUICE_TEST_CASE(c2_identity_preserved_across_suspend_resume) {
    auto rt = build_identity_runtime();
    SLUICE_CHECK(rt->start().has_value());

    // The resume flag: initialized false so task A suspends on it.
    std::atomic<bool> resume_flag{false};
    std::atomic<bool> task_a_resumed{false};
    std::atomic<bool> task_b_ran{false};

    // Submit task A (first). It will suspend and then check identity.
    SLUICE_CHECK(rt->submit([&](RuntimeTaskContext& ctx) {
        // Suspend the Fiber. Since resume_flag is false, the Fiber suspends
        // and the worker switches to the next task.
        ctx.suspend(resume_flag);

        // After resumption: the Runtime identity MUST be preserved.
        // shutdown() called from a Runtime task returns invalid_state.
        auto sr = rt->shutdown();
        if (!sr.has_value() && sr.error().code == IoError::Code::invalid_state) {
            task_a_resumed.store(true, std::memory_order::release);
        }
    }).has_value());

    // Submit task B (second). It will run when task A's Fiber suspends.
    SLUICE_CHECK(rt->submit([&](RuntimeTaskContext& ctx) {
        (void)ctx;
        // Signal task A to resume.
        resume_flag.store(true, std::memory_order::release);
        task_b_ran.store(true, std::memory_order::release);
    }).has_value());

    // Drive the lifecycle from the owning thread.
    rt->request_stop();
    SLUICE_CHECK(rt->drain().has_value());
    SLUICE_CHECK(rt->join().has_value());

    // Both tasks must have run.
    SLUICE_CHECK(task_a_resumed.load(std::memory_order::acquire));
    SLUICE_CHECK(task_b_ran.load(std::memory_order::acquire));
}

SLUICE_MAIN()
