-- Async runtime tests linking the PRODUCTION sluice_async (hook-free).
-- These never see SLUICE_ASYNC_INTERNAL_TESTING. Built/run via `xmake -g test`.

local R = SLUICE_ROOT

-- Async runtime tests (sluice-CORE-017+). Link both sluice_core (Result/IoError/
-- measurement) and sluice_async (Completion/AsyncIoContext/backends).
sluice_production_async_test("async_completion_test")

-- Phase B reference lifecycle — bounded RequestSlot arena unit tests. The arena
-- is an internal detail:: type but its capacity/reserve/release/generation
-- contract is a deliberate test seam (design: docs/design/phase-b-request-slot-
-- reference.md). Links sluice_async for the detail/ headers (header-only so far).
sluice_production_async_test("request_arena_test")

-- Phase B reference lifecycle — Completion binding transient tests (idle ->
-- binding -> outstanding). Drives the two-stage claim directly via ProbeBackend.
sluice_production_async_test("completion_binding_test")

-- Phase B reference lifecycle — Scheme B proof (pending cancel wins before
-- enqueue; enqueue observes backend_ready -> successful no-op; reap-ineligible
-- while the enqueue pin is live; exactly-one terminal winner; generation reuse).
sluice_production_async_test("request_lifecycle_scheme_b_test")

-- Phase B round-4 review regression — ADR Decision 11 best-effort cancel. A
-- running blocking syscall records cancel INTENT only; record_terminal later
-- records the REAL result VERBATIM (an ordinary success is NOT rewritten to
-- canceled). Confirmed cancellation records TerminalResult::err(canceled)
-- explicitly. Drives the arena dispatch seam (mark_running) directly.
sluice_production_async_test("request_arena_cancel_intent_test")

-- Phase B reference-backend allocation-freedom + transactional-rejection proof
-- (review test-gap 3 / review C1 fault-injection matrix). A counting +
-- always-throw operator new drives the accepted submit -> poll -> reset path
-- (and the would_block / binding-CAS-loss rejection paths) under a total
-- allocation fault: the path must still succeed with zero allocations, and a
-- lost binding CAS must leave Completion/slot/FIFO/counters untouched with no
-- future result contamination. This is the structural zero-allocation proof
-- the review asked for (ASan alone cannot prove "no allocation").
sluice_production_async_test("reference_backend_no_alloc_test")

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

-- ThreadPoolBackend Phase E contract tests: capacity/would_block, descriptor
-- validation, closed-fd EBADF terminal, Scheme-B cancel exactly-once,
-- running-cancel real-result-wins, no-lost-wake, and high-frequency small-I/O
-- bounded regression. Uses the public AsyncIoContext + ThreadPoolConfig API;
-- the SLUICE_ASYNC_INTERNAL_TESTING-only count seams are no-ops in production.
sluice_production_async_test("threadpool_backend_phase_e_test")

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

-- Phase C1 — external/custom backend ADMISSION probe (NOT conformance).
-- MinimalExternalBackend subclasses AsyncBackend from the PUBLIC extension
-- surface only (no <sluice/async/detail/*> include) and proves a legitimate
-- subclass can claim/publish + be owned by AsyncIoContext. It is deliberately
-- NOT driven through the shared observable-semantics suite; see
-- scripts/backend_conformance_manifest.py (external_admission layer).
sluice_production_async_test("external_backend_admission_test")

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

-- sluice-copy Version B pipeline integration tests (real files +
-- ThreadPoolBackend). Proves byte-for-byte correctness across sizes/depths/
-- buffers and that depth>1 yields >= 2 concurrent reads against the real
-- backend (content checks the scripted contract test cannot).
do
    local R = SLUICE_ROOT
    local app_dir = R .. "apps/sluice-copy"
    local test_src = R .. "tests/sluice_copy_pipeline_integration_test.cpp"
    if os.isfile(test_src) and os.isfile(app_dir .. "/copy_task.cpp") then
        target("sluice_copy_pipeline_integration_test")
            set_kind("binary")
            set_default(false)
            set_group("test")
            add_deps("sluice_core", "sluice_async")
            add_includedirs(R .. "include", app_dir)
            add_files(test_src, app_dir .. "/copy_task.cpp")
            add_tests("sluice_copy_pipeline_integration_test")
    end
end

-- sluice-copy Version B deterministic pipeline stress test (real files +
-- ThreadPoolBackend). Randomized-but-deterministic matrix driven by
-- --seed/--iterations; a failure reproduces exactly with the same seed.
-- Defaults tuned for the default test group and sanitizer gates; the nightly
-- hardening run passes larger --iterations values.
do
    local R = SLUICE_ROOT
    local app_dir = R .. "apps/sluice-copy"
    local test_src = R .. "tests/sluice_copy_pipeline_stress_test.cpp"
    if os.isfile(test_src) and os.isfile(app_dir .. "/copy_task.cpp") then
        target("sluice_copy_pipeline_stress_test")
            set_kind("binary")
            set_default(false)
            set_group("test")
            add_deps("sluice_core", "sluice_async")
            add_includedirs(R .. "include", app_dir)
            add_files(test_src, app_dir .. "/copy_task.cpp")
            add_tests("sluice_copy_pipeline_stress_test")
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

-- ScriptedAsyncBackend self-tests (sluice-copy Version B test infrastructure).
-- Proves the deterministic, scriptable backend itself is correct before using
-- it to test higher-level components. Links sluice_async (production).
do
    local R = SLUICE_ROOT
    local test_src = R .. "tests/scripted_backend_test.cpp"
    local support_src = R .. "tests/support/scripted_async_backend.cpp"
    if os.isfile(test_src) and os.isfile(support_src) then
        target("scripted_backend_test")
            set_kind("binary")
            set_default(false)
            set_group("test")
            add_deps("sluice_core", "sluice_async")
            add_includedirs(R .. "include", R .. "tests")
            add_files(test_src, support_src)
            add_tests("scripted_backend_test")
    end
end

-- sluice-copy Version B pipeline contract tests.
-- These tests describe the expected behavior of the pipelined copy (Version B).
-- They drive the copy task via the public run_*_copy* entry points against the
-- ScriptedAsyncBackend + controller test infrastructure.
--
-- SLUICE_HAS_PIPELINED_COPY turns the guarded Version-B contract bodies on.
-- The target IS part of the default test group (add_tests below); the old
-- "NOT in the default group / expected FAIL" note was stale once Version B
-- landed. The harness also carries the bounded-failure watchdog: a scenario
-- whose copy thread never publishes terminates the binary with diagnostics
-- instead of hanging the suite forever.
do
    local R = SLUICE_ROOT
    local app_dir = R .. "apps/sluice-copy"
    local test_src = R .. "tests/sluice_copy_pipeline_contract_test.cpp"
    local support_src = R .. "tests/support/scripted_async_backend.cpp"
    if os.isfile(test_src) and os.isfile(support_src) and
       os.isfile(app_dir .. "/copy_task.cpp") then
        target("sluice_copy_pipeline_contract_test")
            set_kind("binary")
            set_default(false)
            set_group("test")
            -- Version B is implemented; SLUICE_HAS_PIPELINED_COPY turns the
            -- guarded contract bodies on. Now part of the default test group.
            add_defines("SLUICE_HAS_PIPELINED_COPY=1")
            add_deps("sluice_core", "sluice_async")
            add_includedirs(R .. "include", R .. "tests", app_dir)
            add_files(test_src, support_src, app_dir .. "/copy_task.cpp")
            add_tests("sluice_copy_pipeline_contract_test")
    end
end

-- sluice-copy CLI argument parsing tests. Strict unsigned decimal parsing,
-- worker caps (no silent narrowing, app-level kMaxWorkers), and argument
-- validation — the same app code the CLI binary runs, compiled into the test
-- target (same pattern as copy_task.cpp in the other sluice-copy tests).
do
    local R = SLUICE_ROOT
    local app_dir = R .. "apps/sluice-copy"
    local test_src = R .. "tests/sluice_copy_cli_parse_test.cpp"
    if os.isfile(test_src) and os.isfile(app_dir .. "/cli_parse.cpp") then
        target("sluice_copy_cli_parse_test")
            set_kind("binary")
            set_default(false)
            set_group("test")
            add_deps("sluice_core", "sluice_async")
            add_includedirs(R .. "include", app_dir)
            add_files(test_src, app_dir .. "/cli_parse.cpp")
            add_tests("sluice_copy_cli_parse_test")
    end
end

-- sluice-copy file-domain tests. The Version B regular-file input domain:
-- invalid sources are rejected BEFORE the destination is created or touched;
-- destinations must be regular files; same-file identity is rejected.
do
    local R = SLUICE_ROOT
    local app_dir = R .. "apps/sluice-copy"
    local test_src = R .. "tests/sluice_copy_file_domain_test.cpp"
    if os.isfile(test_src) and os.isfile(app_dir .. "/file_domain.cpp") then
        target("sluice_copy_file_domain_test")
            set_kind("binary")
            set_default(false)
            set_group("test")
            add_deps("sluice_core", "sluice_async")
            add_includedirs(R .. "include", app_dir)
            add_files(test_src, app_dir .. "/file_domain.cpp")
            add_tests("sluice_copy_file_domain_test")
    end
end
