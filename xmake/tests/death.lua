-- POSIX-only death tests (fork/exec + fail-fast invariant assertions).
-- All gated to linux/macosx. Built/run via `xmake -g test`.

local R = SLUICE_ROOT

-- async_rwlock_death_test — E12-F AsyncRwLock fail-fast boundary tests
-- (Category A caller-contract + Category B internal-invariant corruption).
-- Each case runs in a forked child that re-execs this binary via
-- death_test_runner_posix.hpp; the child installs a deterministic terminate
-- handler and the parent asserts the exact exit code. Category A cases are
-- DEBUG-only (Release compiles out the assertions and trusts the caller per
-- the design contract); Category B cases are deterministic fail-fast in BOTH
-- Debug and Release (assert(false) + std::abort). POSIX-only: gated to
-- linux/macosx.
sluice_internal_async_test("async_rwlock_death_test", {platform_gate = {"linux", "macosx"}})

-- async_mutex_death_test — verifies the Mutex acquisition fail-fast
-- boundary (ASYNC-MUTEX-NOTHROW-PRODUCTION-IMPLEMENTATION-1 §F) via a POSIX
-- fork/exec/waitpid child-process harness. Each case (T1 lock / T2 try_lock /
-- T3 condition_variable_any reacquire / T4 control) re-execs this binary with
-- --death-child=<case>; the child installs a deterministic terminate handler
-- and the parent asserts the exact exit code. The unit under test is the real
-- sluice::async::Mutex entry linked against sluice_async_internal_testing
-- (whose SLUICE_ASYNC_INTERNAL_TESTING define exposes the injection seam).
-- POSIX-only (fork/exec/waitpid): gated to linux/macosx. Windows is NOT RUN
-- in this task (the harness is not implemented there); see
-- tests/death_test_runner_posix.hpp.
sluice_internal_async_test("async_mutex_death_test", {platform_gate = {"linux", "macosx"}})

-- select_event_registry_death_test — E13 Select registry death tests (P2).
-- Verifies identity-check assertions fire for duplicate-link, wrong-Event unlink,
-- cross-Scheduler link/unlink, and live-arm Event destruction. Each case runs in
-- a forked child that re-execs this binary via death_test_runner_posix.hpp.
-- POSIX-only: gated to linux/macosx.
sluice_internal_async_test("select_event_registry_death_test", {platform_gate = {"linux", "macosx"}})

-- select_timer_pump_death_test — E13 Select timer pump ACTIVE-due
-- stage-boundary fail-fast (P3). A due ACTIVE SelectTimerRegistration is
-- unreachable in valid P3 (no admission path); the pump must fail fast rather
-- than claim/mark/retire/consume. Runs in a forked child that re-execs this
-- binary via death_test_runner_posix.hpp. POSIX-only; gated to linux/macosx.
-- This is an invariant GUARD, NOT supported production Select behavior.
sluice_internal_async_test("select_timer_pump_death_test", {platform_gate = {"linux", "macosx"}})

-- select_claim_death_test — E13 P4 Select claim/finalization death tests.
-- Verifies preflight assertions fire BEFORE the winner CAS for: candidate index
-- out of range (CG), cross-Scheduler group (CS), candidate not CandidateReady
-- (CP), arm.group mismatch (CA), Event arm not linked to its port (EH), Timer
-- null stable_reg (TN), Timer registration on another Scheduler (TF), Timer
-- registration not pool-owned (TP), and the post-claim all-authority-closed
-- assertion rejecting an open authority (OA). Runs in a forked child that
-- re-execs this binary via death_test_runner_posix.hpp. POSIX-only.
sluice_internal_async_test("select_claim_death_test", {platform_gate = {"linux", "macosx"}})

-- select_publication_death_test — E13 P6 publication invariant death tests
-- (SN-2 duplicate-publish / SN-10 open-authority / FP caller-not-waiting / MG
-- multi-group-Event P8 stage-boundary / CTL valid-publication control). Drives
-- the real production publication entry + Event resolver through guarded
-- internal-testing drivers. Runs in forked children that re-exec this binary
-- via death_test_runner_posix.hpp. POSIX-only; gated to linux/macosx.
sluice_internal_async_test("select_publication_death_test", {platform_gate = {"linux", "macosx"}})

-- select_rollback_invariant_death_test — E13 P7 rollback-domain negative tests
-- (SN-8 rollback after suspension + P7-N1..N9). Exercises the guarded internal
-- rollback authorities on invalid domains / corrupted membership, proving
-- fail-fast. Runs in forked children via death_test_runner_posix.hpp.
-- POSIX-only; gated to linux/macosx.
sluice_internal_async_test("select_rollback_invariant_death_test", {platform_gate = {"linux", "macosx"}})

-- threaded_evented_death_test — Threaded/Evented RT-F2a (destructor fail-fast)
-- and RT-F5b (unsupported-target admission) death tests. POSIX only.
do
    local p = R .. "tests/threaded_evented_death_test.cpp"
    if os.isfile(p) and is_plat("linux", "macosx") then
        target("threaded_evented_death_test")
            set_kind("binary")
            set_default(false)
            set_group("test")
            add_deps("sluice_core", "sluice_async")
            add_includedirs(R .. "include", R .. "tests")
            add_files(p)
            add_tests("threaded_evented_death_test")
    end
end

-- async_io_context_death_test — move-assign over a destination with outstanding
-- Completions must fail-fast, and destroying a context with outstanding
-- Completions must fail-fast in BOTH Debug and Release. The truthful
-- deterministic contract: a destructor / move-assignment has no Result channel
-- for invalid_state, and silent abandonment would strand caller-owned address-
-- stable Completions permanently outstanding. POSIX only.
do
    local p = R .. "tests/async_io_context_death_test.cpp"
    if os.isfile(p) and is_plat("linux", "macosx") then
        target("async_io_context_death_test")
            set_kind("binary")
            set_default(false)
            set_group("test")
            add_deps("sluice_core", "sluice_async")
            add_includedirs(R .. "include", R .. "tests")
            add_files(p)
            add_tests("async_io_context_death_test")
    end
end

-- runtime_wait_death_test — M1-A RuntimeTaskContext::await_completion
-- idle-await contract violation (Debug assertion). POSIX-only.
sluice_internal_async_test("runtime_wait_death_test", {platform_gate = {"linux", "macosx"}})

-- completion_authority_death_test — ADR-explicit-io-completion-authority
-- fail-fast boundary tests: reset-on-outstanding, destroy-outstanding,
-- double-publish. Also includes a double-claim regression (returns false,
-- no fail-fast) and a control case (valid lifecycle, exit 0). POSIX-only.
do
    local p = R .. "tests/completion_authority_death_test.cpp"
    if os.isfile(p) and is_plat("linux", "macosx") then
        target("completion_authority_death_test")
            set_kind("binary")
            set_default(false)
            set_group("test")
            add_deps("sluice_core", "sluice_async")
            add_includedirs(R .. "include", R .. "tests")
            add_files(p)
            add_tests("completion_authority_death_test")
    end
end

-- request_arena_death_test — Phase B RequestArena release fail-fast boundary
-- (ADR-explicit-io-request-contract Decision 15 / AC-13 :566-572). Proves
-- release() while the enqueue-in-flight pin is live OR while a waiter is still
-- registered terminates (exit 86) in BOTH Debug and Release, plus a control
-- (valid release after reap exits 0). POSIX-only.
do
    local p = R .. "tests/request_arena_death_test.cpp"
    if os.isfile(p) and is_plat("linux", "macosx") then
        target("request_arena_death_test")
            set_kind("binary")
            set_default(false)
            set_group("test")
            add_deps("sluice_core", "sluice_async")
            add_includedirs(R .. "include", R .. "tests")
            add_files(p)
            add_tests("request_arena_death_test")
    end
end

-- async_queue_lifecycle_death_test — #86-A QueuePort lifecycle serialization
-- (active_port_calls_). Proves begin_teardown fail-fasts (exit 86) while an
-- ordinary QueuePort call is in flight (a parked consumer Fiber leaves
-- active_port_calls_ == 1), plus a control case (begin_teardown succeeds,
-- exit 0, after an ordinary call has returned). Deterministic via single-
-- worker run(1) FIFO scheduling. Links sluice_async_internal_testing (the
-- test drives the non-template detail::QueuePort authority directly).
-- POSIX-only (fork/exec/waitpid).
sluice_internal_async_test("async_queue_lifecycle_death_test", {platform_gate = {"linux", "macosx"}})

-- threadpool_backend_death_test — Phase E ThreadPoolBackend non-quiescent
-- destruction fail-fast. Verifies that destroying a backend with enqueued ops,
-- running workers, backend-ready unreaped terminals, or completion-ready
-- unreset Completions terminates (exit 86) in BOTH Debug and Release, while a
-- quiescent close_admission + drain + reset path exits 0. Uses
-- SLUICE_ASYNC_INTERNAL_TESTING pause gates for the enqueued/running/pending
-- cases (the pending case is Phase C2e — destroying while a committed request
-- sits between commit and enqueue); links sluice_async_internal_testing.
-- POSIX-only.
sluice_internal_async_test("threadpool_backend_death_test", {platform_gate = {"linux", "macosx"}})

-- fake_backend_death_test — Phase C2e FakeAsyncBackend non-quiescent
-- destruction fail-fast (Issue #68 row 16; ADR Decision 15). Proves the
-- reference path through the CONCRETE FakeAsyncBackend type: destroying the
-- backend with a bound unreaped request or with a ready-but-unreset Completion
-- terminates (exit 86) in BOTH Debug and Release (the arena destructor is the
-- fail-fast authority), while close_admission + drain + reset + destroy exits
-- 0. POSIX-only.
sluice_internal_async_test("fake_backend_death_test", {platform_gate = {"linux", "macosx"}})

-- uring_backend_death_test — Phase D1 UringAsyncBackend non-quiescent
-- destruction fail-fast. The destruction contract needs NO injection hook, so
-- this links the PRODUCTION sluice_async (the real UringAsyncBackend
-- destructor), not the internal-testing variant. Proves that destroying a
-- backend whose destructor preflight finds non-quiescence (a ready-but-unreset
-- Completion with slot_in_use != 0) terminates (exit 86) in BOTH Debug and
-- Release BEFORE io_uring_queue_exit() runs, while a quiescent destroy (drain +
-- reset + destroy) exits 0. Real-liburing only (the production destructor
-- preflights the live io_uring ring); POSIX-only (fork/exec/waitpid).
if has_config("with-liburing") then
    local p = R .. "tests/uring_backend_death_test.cpp"
    if os.isfile(p) and is_plat("linux", "macosx") then
        target("uring_backend_death_test")
            set_kind("binary")
            set_default(false)
            set_group("test")
            add_deps("sluice_core", "sluice_async")
            add_includedirs(R .. "include", R .. "tests")
            add_files(p)
            add_tests("uring_backend_death_test")
    end
end

-- uring_backend_c2e_death_test — Phase D4 C2e non-quiescent destruction death
-- matrix (Issue #68 row 16; ADR Decision 15). Real mode compiles the
-- authoritative production uring_backend.cpp + fail_fast.cpp under SLUICE_ASYNC_
-- INTERNAL_TESTING (the deterministic pause gates / CQE injection are needed to
-- reach the pending / enqueued / backend-ready / live-control windows). Proves
-- destroying the backend with pending / enqueued / running (ring-owned) /
-- transport-ledger residue / backend-ready unreaped / completion-ready
-- unreset / live-control-reference state terminates (exit 86) in BOTH Debug
-- and Release BEFORE io_uring_queue_exit(), while close_admission + drain +
-- reset + destroy exits 0. POSIX-only.
--
-- The target exists in BOTH modes (P0-C): the stub build registers the SAME
-- pinned case names as empty build/API-only bodies and emits [evidence-meta]
-- mode=stub, so the manifest's exact case-set holds in every mode and the
-- aggregate gate can attribute the mandatory uring_c2e_quiescent_destruction
-- record honestly (stub -> INCOMPLETE by required_modes=("real",), never
-- PASS).
do
local p = R .. "tests/uring_backend_c2e_death_test.cpp"
if os.isfile(p) and is_plat("linux", "macosx") then
    target("uring_backend_c2e_death_test")
        set_kind("binary")
        set_default(false)
        set_group("test")
        add_deps("sluice_core")
        add_includedirs(R .. "include", R .. "tests")
        add_files(p, R .. "src/async/fail_fast.cpp")
        if has_config("with-liburing") then
            add_files(R .. "src/async/uring_backend.cpp")
            add_defines("SLUICE_HAS_LIBURING", "SLUICE_ASYNC_INTERNAL_TESTING")
            add_packages("liburing")
        end
        add_tests("uring_backend_c2e_death_test")
end
end
