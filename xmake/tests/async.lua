-- Async runtime tests linking the PRODUCTION sluice_async (hook-free).
-- These never see SLUICE_ASYNC_INTERNAL_TESTING. Built/run via `xmake -g test`.

local R = SLUICE_ROOT

-- Async runtime tests (sluice-CORE-017+). Link both sluice_core (Result/IoError/
-- measurement) and sluice_async (Completion/AsyncIoContext/backends).
sluice_production_async_test("async_completion_test")

-- AsyncIoContext lifecycle / move-semantics tests (E15-P1-03 / E15-P2-06). The
-- SAFE move paths (idle-to-idle, source-with-outstanding transfer, self move,
-- chained moves, moved-from destruction) are exercised here; the FAIL-FAST
-- paths (destination-outstanding move-assign, destroy-with-outstanding) live
-- in async_io_context_death_test.cpp (POSIX fork/exec).
sluice_production_async_test("async_io_context_test")

-- FakeAsyncBackend tests (sluice-CORE-019). The deterministic test vehicle.
sluice_production_async_test("fake_backend_test")

-- Async "all" helpers tests (sluice-CORE-018). read_all/write_all over the fake.
sluice_production_async_test("async_op_helpers_test")

-- Async durability ops tests (sluice-CORE-018B, W4).
sluice_production_async_test("async_durability_test")

-- Async cancellation tests (sluice-CORE-021 spike).
sluice_production_async_test("async_cancel_test")

-- UringAsyncBackend tests (sluice-CORE-020B). Stub-mode contract by default;
-- real io_uring path gated behind SLUICE_HAS_LIBURING.
sluice_production_async_test("uring_backend_test")

-- ThreadPoolBackend tests (sluice-CORE-020A). Real blocking I/O on threads.
sluice_production_async_test("threadpool_backend_test")

-- Shared AsyncBackend conformance suite (sluice-CORE-024, B1). One parameterized
-- harness asserting every genuinely-shared backend semantic against every
-- backend. The suite impl is compiled into the driver target alongside the
-- driver; backend-specific MECHANISM tests stay in their own files.
do
    local driver = R .. "tests/backend_conformance_driver_test.cpp"
    local impl = R .. "tests/backend_conformance_test.cpp"
    if os.isfile(driver) and os.isfile(impl) then
        target("backend_conformance_test")
            set_kind("binary")
            set_default(false)
            set_group("test")
            add_deps("sluice_core", "sluice_async")
            add_includedirs(R .. "include")
            add_files(driver, impl)
            add_tests("backend_conformance_test")
    end
end

-- Cooperative cancellation primitives tests (sluice-CORE-027, T1). Pure-logic;
-- links sluice_core (Result/IoError) + sluice_async (cancel.cpp).
sluice_production_async_test("cancel_token_test")

-- Future<T> tests (sluice-CORE-028, T2). Header-only Future; exercises a
-- thread-driven producer (await blocks until the worker completes) + the
-- cooperative-cancel path. Links sluice_async (for cancel.cpp) + std::thread.
sluice_production_async_test("future_test")

-- Group tests (sluice-CORE-029, T3). Unordered task set; await/cancel whole-
-- group; cancel-propagation boundary. Links sluice_async (group.cpp + cancel).
sluice_production_async_test("group_test")

-- Batch tests (sluice-CORE-030, T4). Grouped completions over AsyncIoContext;
-- uses real I/O (ThreadPoolBackend + temp fds). Links sluice_async (batch.cpp).
sluice_production_async_test("batch_test")

-- Batch reap-order + wait-error regression tests. Uses a deterministic
	-- in-test SequenceBackend that lets each case DIRECT the exact reap order,
	-- so Batch::next()'s "completion (reap) order" contract is asserted exactly
	-- (the ThreadPoolBackend-based batch_test cannot).
	sluice_production_async_test("batch_reap_order_test")

-- Fiber state-model tests (sluice-CORE-E1). Pure C++ state machine; no asm yet
-- (E2). Links sluice_async (fiber.cpp + cancel.cpp).
sluice_production_async_test("fiber_test")

-- Isolated x86_64 fiber context-switch tests (sluice-CORE-E2/E3). NO I/O, no
-- Future/WaitPolicy/AsyncBackend/Group integration. Proves only the asm +
-- trampoline. Links sluice_async (fiber_ctx.cpp). Gated to x86_64 via the
-- `supported` constant in the header; non-x86_64 skips cleanly.
sluice_production_async_test("fiber_ctx_test")

-- E4 single-worker Evented scheduler tests (sluice-CORE-E4). Proves scheduler
-- liveness (B progresses while A awaits a pending op), completion wake path,
-- resume fidelity, exactly-once. Uses FakeAsyncBackend held-pending mode.
-- Gated to x86_64 (depends on fiber_ctx::context_switch).
sluice_production_async_test("evented_scheduler_test")

-- E5-A1 level-triggered scheduler ready-flag wait tests (sluice-CORE-E5-A1).
-- Tests Scheduler::await_ready_flag in isolation (no Future). Proves R1-R5.
-- Gated to x86_64.
sluice_production_async_test("scheduler_ready_flag_test")

-- E5-A2 Evented Future await tests (sluice-CORE-E5-A2). Proves F1-F6: an
-- EventedWaitPolicy Future suspends the current Fiber; another Fiber progresses
-- (liveness); completion resumes the awaiter; resume fidelity; idempotent
-- repeat; Threaded regression. Gated to x86_64.
sluice_production_async_test("evented_future_test")

-- E5-B Evented Group tests (sluice-CORE-E5-B). Proves G1-G6: Evented Group
-- tasks run on Fibers (not std::thread), can suspend inside Future::await,
-- resume, and complete; Threaded regression. Gated to x86_64.
sluice_production_async_test("evented_group_test")

-- E6 scheduler progress tests (sluice-CORE-E6). Proves the hybrid poll/wait
-- progress policy: a Fiber awaiting a real-backend Completion that completes
-- after the runnable queue drains is resumed via wait_one. ThreadPoolBackend is
-- the real completion source (cv-wait). Gated to x86_64.
sluice_production_async_test("scheduler_progress_test")

-- E7 multi-worker scheduler tests (sluice-CORE-E7). Proves worker-local
-- execution state, pinned routing, serialized backend access, MW coordination.
-- Gated to x86_64.
sluice_production_async_test("multi_worker_test")

-- runnable_dup_publication_test — focused regression for the E7-T2 root cause
-- (exactly-once runnable publication). Unit-level make_runnable contract +
-- integration wake-while-runnable scenario. Fails on pre-fix code.
sluice_production_async_test("runnable_dup_publication_test")

-- runnable_steal_test — runnable ownership transfer / work stealing (sluice-CORE-E8).
-- Proves steal = MOVE + OWNER TRANSFER (never PUBLISH); stolen Fiber
-- wake-routes to the thief. Gated to x86_64 (fiber_ctx::supported).
sluice_production_async_test("runnable_steal_test")

-- wait_queue_test — WaitNode/WaitQueue cancellation-safe protocol
-- (sluice-CORE-E10). Pure-protocol tests (no scheduler): wake-vs-cancel single
-- winner, repeated wake/cancel, wake-after-cancel/cancel-after-wake, unlink
-- exactly-once, multiple waiters, node-reuse rejection, destruction invariant,
-- and a high-iteration wake||cancel stress. Header-only WaitNode/WaitQueue, so
-- this links sluice_async only to stay consistent with the async test family.
sluice_production_async_test("wait_queue_test")

-- scheduler_wait_test — Scheduler integration of WaitNode/WaitQueue
-- (sluice-CORE-E10). Integration tests with real fibers: C10 exactly-one winner
-- makes the fiber runnable via the canonical wake seam (wake + cancel + race),
-- C11 Drain interaction (MW-S3 wait returns STALLED, no revival of E9 hang).
-- Gated to x86_64 (fiber_ctx::supported).
sluice_production_async_test("scheduler_wait_test")

-- wait_queue_external_wake_test — E10-CORRECTIVE C1 external wake-domain classification
-- regression (sluice-CORE-E10). Proves a Live run with an externally-resolvable
-- WaitQueue wait parks (not STALLED) so an external wake_wait_one/cancel_wait
-- resumes the waiter. Fails on uncorrected 0debd21.
sluice_production_async_test("wait_queue_external_wake_test")

-- wait_queue_resolution_authority_test — E10-CORRECTIVE C2 resolution-authority bypass
-- (structural: public wake_one/cancel/cancel_all are not expressible) + C3
-- cancel_all surface (REMOVED) + T4 non-bypass count consistency. Compile-time
-- static_assert + runtime mirror + fiber integration.
sluice_production_async_test("wait_queue_resolution_authority_test")

-- wait_queue_unlink_topology_test — E10-CORRECTIVE C5 middle-node concurrent unlink
-- topology stress (A<->B<->C; concurrent wake-head-A || cancel-middle-B). Locks
-- in the doubly-linked list topology invariants at a meaningful stress count.
sluice_production_async_test("wait_queue_unlink_topology_test")

-- threaded_evented_parity_test — Threaded/Evented semantic parity regression tests.
-- RT-F1 (external-producer wake), RT-F3 (init_fiber failure), RT-F4 (size
-- parity), RT-F5a (supported-target admission no-op).
sluice_production_async_test("threaded_evented_parity_test")

-- E16 ApplicationRuntime lifecycle tests.
-- ADR: docs/adr/ADR-application-runtime.md (Accepted).
sluice_production_async_test("application_runtime_test")

-- M1-A Runtime cooperative Completion-wait tests (Candidate A, winner).
-- Design: docs/design/m1-runtime-io-await-race.md. Public-only: exercises
-- RuntimeTaskContext::await_completion against FakeAsyncBackend (deterministic)
-- and ThreadPoolBackend (real-file suspend/resume). No internal-testing macro.
sluice_production_async_test("runtime_wait_test")

-- M1-A sluice-copy integration tests (brief §24). Real temporary files +
-- ThreadPoolBackend; drives the same copy_task code the CLI uses (compiled
-- into this target) via its public header. Public headers only; no
-- internal-testing macro.
do
    local R = SLUICE_ROOT
    local app_dir = R .. "apps/sluice-copy"
    local test_src = R .. "tests/sluice_copy_integration_test.cpp"
    if os.isfile(test_src) and os.isfile(app_dir .. "/copy_task.cpp") then
        target("sluice_copy_integration_test")
            set_kind("binary")
            set_default(false)
            set_group("test")
            add_deps("sluice_core", "sluice_async", "sluice-copy")
            add_includedirs(R .. "include", app_dir)
            add_files(test_src, app_dir .. "/copy_task.cpp")
            add_tests("sluice_copy_integration_test")
    end
end

-- M1-A sluice-copy deterministic fault tests (brief §25). Drives the SAME
-- copy_task code against FakeAsyncBackend for short/zero/error injection.
do
    local R = SLUICE_ROOT
    local app_dir = R .. "apps/sluice-copy"
    local test_src = R .. "tests/sluice_copy_fault_test.cpp"
    if os.isfile(test_src) and os.isfile(app_dir .. "/copy_task.cpp") then
        target("sluice_copy_fault_test")
            set_kind("binary")
            set_default(false)
            set_group("test")
            add_deps("sluice_core", "sluice_async")
            add_includedirs(R .. "include", app_dir)
            add_files(test_src, app_dir .. "/copy_task.cpp")
            add_tests("sluice_copy_fault_test")
    end
end

-- E16-POST-MERGE-CORRECTIVE-1 — terminal resource destruction regressions (C1).
-- Proves every Stopped transition first destroys Runtime-owned components via a
-- destructor-probe backend: Constructed direct close, startup-abort close,
-- normal-join close (exactly once), and concurrent-Constructed close-owner
-- election. Uses sluice_async_internal_testing for the driver barrier seam.
sluice_internal_async_test("application_runtime_resource_test")

-- E16-POST-MERGE-CORRECTIVE-1 — Fiber-local Runtime identity authority (C2).
-- Proves a Runtime-owned task cannot self-close (drain/join/shutdown return
-- invalid_state via the Fiber-local tag), identity is preserved across
-- concurrent tasks, and an external thread is correctly NOT recognized as a
-- Runtime task. The private-setter authority (no public execution-tag setter)
-- is enforced by scripts/verify-async-identity-negative-compile.sh. Uses
-- sluice_async_internal_testing for the Fiber suspend/resume seam.
sluice_internal_async_test("application_runtime_identity_test")
