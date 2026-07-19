-- xmake build for the sluice C++ core (Zig std.Io inspired).
-- Zig source under ./zig is design reference only; never built here.
add_rules("mode.debug", "mode.release", "mode.asan", "mode.tsan", "mode.ubsan", "mode.valgrind")
add_rules("plugin.compile_commands.autoupdate")

-- Combined ASan + UBSan: no built-in mode, set policies directly.
if is_mode("asanubsan") then
    set_policy("build.sanitizer.address", true)
    set_policy("build.sanitizer.undefined", true)
end

set_languages("c++20")
set_warnings("all", "error")

-- CPP-STATIC-1: Clang Thread Safety Analysis gate.
-- The TSA flags are added only for the sluice_async target (pilot scope),
-- via unconditional add_cxxflags on that target below.
-- GCC does not recognize -Wthread-safety and would error on it, so xmake
-- filters the flag out of the GCC compile command (the gate is Clang-only).
-- For non-Clang compilers the annotation macros additionally erase to no-ops.

-- Core static library: Reader/Writer abstractions + wrappers.
target("sluice_core")
    set_kind("static")
    add_includedirs("include", {public = true})
    add_files("src/*.cpp")

-- Async runtime library (sluice-CORE-017+). OPT-IN, namespace sluice::async.
-- Built alongside the core but kept a separate static lib so the blocking
-- default (sluice_core) carries no async surface. ADR §A6: async is opt-in and
-- BlockingIoContext/Reader/Writer are untouched.
target("sluice_async")
    set_kind("static")
    set_default(false)
    set_group("async")
    add_includedirs("include", {public = true})
    add_deps("sluice_core")
    add_files("src/async/*.cpp")
    -- CPP-STATIC-1: Clang TSA gate.
    -- ASYNC-GCC-TSA-FLAG-ROUTING-CORRECTIVE-1 (W3): the flags are scoped to
    -- the Clang frontends via the `tools` option. {force=true} previously
    -- bypassed xmake's per-compiler flag filtering, which caused GCC to
    -- receive the Clang-only -Wthread-safety and fail. Dropping force and
    -- using {tools={"clang","clang_cl"}} scopes the flags to BOTH Clang
    -- frontends (the Linux/Mac clang driver AND the Windows clang-cl driver),
    -- so Windows/clang-cl builds keep TSA coverage; GCC never receives them.
    -- Verified against the official xmake docs (add_cxxflags {tools=...}).
    add_cxxflags("-Wthread-safety", "-Werror=thread-safety",
                 {tools = {"clang", "clang_cl"}})

-- ---------------------------------------------------------------------------
-- ASYNC-TEST-SEAM-AUTHORITY-CORRECTIVE-1: internal-testing runtime variant.
--
-- The production `sluice_async` (above) is hook-free: it declares no test
-- friends, no test seam state, and exports no test phase/controller symbol.
-- The `sluice_async_internal_testing` variant is compiled from the SAME
-- authoritative async sources (src/async/*.cpp) PLUS the non-installed test
-- controller (tests/async_test_control.cpp), with the private macro
-- SLUICE_ASYNC_INTERNAL_TESTING defined. Only this variant links the
-- controller; only test binaries that need deterministic causal seams depend
-- on it. No binary links both variants.
--
-- Both targets share the same source manifest + TSA configuration via the
-- helper below (one source list, two targets).
-- ---------------------------------------------------------------------------
local async_sources = function()
    return { "src/async/*.cpp" }
end

-- TSA flags scoped to the Clang frontends only (W3 corrective). See the note
-- on sluice_async above. Used by sluice_async_internal_testing.
local async_tsa_flags = function()
    add_cxxflags("-Wthread-safety", "-Werror=thread-safety",
                 {tools = {"clang", "clang_cl"}})
end

target("sluice_async_internal_testing")
    set_kind("static")
    set_default(false)
    set_group("test")
    add_includedirs("include", "tests", {public = true})
    add_deps("sluice_core")
    add_files(async_sources())
    -- The non-installed test controller (defines test_phase + the registry).
    -- Lives in tests/ so the production src/async/*.cpp glob never sees it.
    add_files("tests/async_test_control.cpp")
    -- PUBLIC: the define must also reach the test TUs that include the
    -- non-installed async_test_control.hpp (which references Scheduler::
    -- AsyncTestAccess, a guarded nested struct). Dependents of this variant
    -- see the macro; the production `sluice_async` target does NOT depend on
    -- this variant, so production TUs never see it.
    add_defines("SLUICE_ASYNC_INTERNAL_TESTING", {public = true})
    async_tsa_flags()

-- Bench helper library (SLUICE-CORE-010B). Linked into bench targets + the CSV test.
-- Also contains BlockingIoPool (021S), the bounded execution model for the
-- W1-W4 blocking bench matrix (job 022S). Pool source is here so both bench and
-- test targets link it without per-target duplication.
target("sluice_bench_common")
    set_kind("static")
    set_default(false)
    set_group("bench")
    add_includedirs("include", "bench")
    add_deps("sluice_core")
    add_files("bench/bench_common.cpp", "bench/support/blocking_io_pool.cpp",
              "bench/support/sync_matrix.cpp")

-- Declare a test/example target only when its source file exists, so xmake
-- does not warn about missing files for slices not yet written.
function sluice_one_file_target(kind, group, name, subdir, deps_list)
    local path = subdir .. "/" .. name .. ".cpp"
    if not os.isfile(path) then return end
    target(name)
        set_kind(kind)
        set_default(false)
        set_group(group)
        if deps_list then add_deps(deps_list) end
        add_includedirs("include", "bench")
        add_files(path)
        if group == "test" then add_tests(name) end
end

-- Correctness tests, one binary per slice. Built/run via `xmake -g test`.
local tests = {
    "result", "writer", "reader", "fault", "buffer",
    "observed", "copy", "wal", "file", "posix_retry",
    "wrapper_noncopyable", "limit", "measurement",
    "writer_vec", "reader_vec", "file_vec", "wal_vec", "vector_stats",
    "buffered_readable", "copy_fast_path", "copy_stats_fast_path", "copy_strategy",
    "copy_scratch_strategy", "copy_buffered_first_strategy", "copy_deferred_strategy",
    "copy_strategy_stats", "syncable_writer", "file_sync", "wal_writer",
    "io_context_api", "blocking_io_context", "read_vec_all",
    "memory_reader_convenience", "memory_io_context",
    "file_positional", "sync_contract_negative",
}
for _, t in ipairs(tests) do
    sluice_one_file_target("binary", "test", t .. "_test", "tests", "sluice_core")
end

-- uring_write_batch_test links the experimental uring lib (stub or real).
target("uring_write_batch_test")
    set_kind("binary")
    set_default(false)
    set_group("test")
    add_deps("sluice_core", "sluice_experimental_uring")
    add_includedirs("include", "bench")
    add_files("tests/uring_write_batch_test.cpp")
    add_tests("uring_write_batch_test")

target("uring_io_context_test")
    set_kind("binary")
    set_default(false)
    set_group("test")
    add_deps("sluice_core", "sluice_experimental_uring")
    add_includedirs("include", "bench")
    add_files("tests/uring_io_context_test.cpp")
    add_tests("uring_io_context_test")

target("uring_stats_test")
    set_kind("binary")
    set_default(false)
    set_group("test")
    add_deps("sluice_core", "sluice_experimental_uring")
    add_includedirs("include", "bench")
    add_files("tests/uring_stats_test.cpp")
    add_tests("uring_stats_test")

-- Buildable examples. Built/run via `xmake -g examples`.
local examples = { "cat", "copy_file", "small_writes", "fault_write", "wal_records",
                   "mvp_copy_pipeline", "mvp_limited_copy", "mvp_wal_vector",
                   "mvp_copy_strategy", "mvp_wal_durable", "mvp_io_context_copy",
                   "mvp_memory_io_context", "sync_random_read", "blocking_io_pool" }
for _, e in ipairs(examples) do
    sluice_one_file_target("binary", "examples", e, "examples", "sluice_core")
end

-- experimental_uring_write links the experimental uring lib (stub or real).
sluice_one_file_target("binary", "examples", "experimental_uring_write", "examples",
                      {"sluice_core", "sluice_experimental_uring"})

-- bench_csv_test needs the bench helper lib + bench include dir.
do
    local p = "tests/bench_csv_test.cpp"
    if os.isfile(p) then
        target("bench_csv_test")
            set_kind("binary")
            set_default(false)
            set_group("test")
            add_deps("sluice_core", "sluice_bench_common")
            add_includedirs("include", "bench")
            add_files(p)
            add_tests("bench_csv_test")
    end
end

-- blocking_io_pool_test (021S) needs the bench helper lib (it links
-- BlockingIoPool from bench/support/) in addition to the core.
do
    local p = "tests/blocking_io_pool_test.cpp"
    if os.isfile(p) then
        target("blocking_io_pool_test")
            set_kind("binary")
            set_default(false)
            set_group("test")
            add_deps("sluice_core", "sluice_bench_common")
            add_includedirs("include", "bench")
            add_files(p)
            add_tests("blocking_io_pool_test")
    end
end

-- Production BlockingIoPool tests (sluice-CORE-024S). Core-only: the production
-- pool lives in include/sluice/blocking_io_pool.hpp + src/blocking_io_pool.cpp.
do
    local p = "tests/blocking_io_pool_prod_test.cpp"
    if os.isfile(p) then
        target("blocking_io_pool_prod_test")
            set_kind("binary")
            set_default(false)
            set_group("test")
            add_deps("sluice_core")
            add_includedirs("include")
            add_files(p)
            add_tests("blocking_io_pool_prod_test")
    end
end

-- Production BlockingIoPool CONCURRENCY stress tests (sluice-CORE-024S).
-- Catches data races / deadlocks the functional tests miss. Run under TSan.
do
    local p = "tests/blocking_io_pool_stress_test.cpp"
    if os.isfile(p) then
        target("blocking_io_pool_stress_test")
            set_kind("binary")
            set_default(false)
            set_group("test")
            add_deps("sluice_core")
            add_includedirs("include")
            add_files(p)
            add_tests("blocking_io_pool_stress_test")
    end
end

-- Production BlockingIoPool INVARIANT tests (sluice-CORE-024S, category B):
-- exactly-once / no-lost-task / no-double-get / FIFO order.
do
    local p = "tests/blocking_io_pool_invariants_test.cpp"
    if os.isfile(p) then
        target("blocking_io_pool_invariants_test")
            set_kind("binary")
            set_default(false)
            set_group("test")
            add_deps("sluice_core")
            add_includedirs("include")
            add_files(p)
            add_tests("blocking_io_pool_invariants_test")
    end
end

-- sync_matrix_test (022S) locks the matrix CSV shape.
do
    local p = "tests/sync_matrix_test.cpp"
    if os.isfile(p) then
        target("sync_matrix_test")
            set_kind("binary")
            set_default(false)
            set_group("test")
            add_deps("sluice_core", "sluice_bench_common")
            add_includedirs("include", "bench")
            add_files(p)
            add_tests("sync_matrix_test")
    end
end

-- Async runtime tests (sluice-CORE-017+). Link both sluice_core (Result/IoError/
-- measurement) and sluice_async (Completion/AsyncIoContext/backends).
do
    local p = "tests/async_completion_test.cpp"
    if os.isfile(p) then
        target("async_completion_test")
            set_kind("binary")
            set_default(false)
            set_group("test")
            add_deps("sluice_core", "sluice_async")
            add_includedirs("include")
            add_files(p)
            add_tests("async_completion_test")
    end
end

-- FakeAsyncBackend tests (sluice-CORE-019). The deterministic test vehicle.
do
    local p = "tests/fake_backend_test.cpp"
    if os.isfile(p) then
        target("fake_backend_test")
            set_kind("binary")
            set_default(false)
            set_group("test")
            add_deps("sluice_core", "sluice_async")
            add_includedirs("include")
            add_files(p)
            add_tests("fake_backend_test")
    end
end

-- Async "all" helpers tests (sluice-CORE-018). read_all/write_all over the fake.
do
    local p = "tests/async_op_helpers_test.cpp"
    if os.isfile(p) then
        target("async_op_helpers_test")
            set_kind("binary")
            set_default(false)
            set_group("test")
            add_deps("sluice_core", "sluice_async")
            add_includedirs("include")
            add_files(p)
            add_tests("async_op_helpers_test")
    end
end

-- Async durability ops tests (sluice-CORE-018B, W4).
do
    local p = "tests/async_durability_test.cpp"
    if os.isfile(p) then
        target("async_durability_test")
            set_kind("binary")
            set_default(false)
            set_group("test")
            add_deps("sluice_core", "sluice_async")
            add_includedirs("include")
            add_files(p)
            add_tests("async_durability_test")
    end
end

-- Async cancellation tests (sluice-CORE-021 spike).
do
    local p = "tests/async_cancel_test.cpp"
    if os.isfile(p) then
        target("async_cancel_test")
            set_kind("binary")
            set_default(false)
            set_group("test")
            add_deps("sluice_core", "sluice_async")
            add_includedirs("include")
            add_files(p)
            add_tests("async_cancel_test")
    end
end

-- UringAsyncBackend tests (sluice-CORE-020B). Stub-mode contract by default;
-- real io_uring path gated behind SLUICE_HAS_LIBURING.
do
    local p = "tests/uring_backend_test.cpp"
    if os.isfile(p) then
        target("uring_backend_test")
            set_kind("binary")
            set_default(false)
            set_group("test")
            add_deps("sluice_core", "sluice_async")
            add_includedirs("include")
            add_files(p)
            add_tests("uring_backend_test")
    end
end

-- ThreadPoolBackend tests (sluice-CORE-020A). Real blocking I/O on threads.
do
    local p = "tests/threadpool_backend_test.cpp"
    if os.isfile(p) then
        target("threadpool_backend_test")
            set_kind("binary")
            set_default(false)
            set_group("test")
            add_deps("sluice_core", "sluice_async")
            add_includedirs("include")
            add_files(p)
            add_tests("threadpool_backend_test")
    end
end

-- Shared AsyncBackend conformance suite (sluice-CORE-024, B1). One parameterized
-- harness asserting every genuinely-shared backend semantic against every
-- backend. The suite impl is compiled into the driver target alongside the
-- driver; backend-specific MECHANISM tests stay in their own files.
do
    local driver = "tests/backend_conformance_driver_test.cpp"
    local impl = "tests/backend_conformance_test.cpp"
    if os.isfile(driver) and os.isfile(impl) then
        target("backend_conformance_test")
            set_kind("binary")
            set_default(false)
            set_group("test")
            add_deps("sluice_core", "sluice_async")
            add_includedirs("include")
            add_files(driver, impl)
            add_tests("backend_conformance_test")
    end
end

-- Cooperative cancellation primitives tests (sluice-CORE-027, T1). Pure-logic;
-- links sluice_core (Result/IoError) + sluice_async (cancel.cpp).
do
    local p = "tests/cancel_token_test.cpp"
    if os.isfile(p) then
        target("cancel_token_test")
            set_kind("binary")
            set_default(false)
            set_group("test")
            add_deps("sluice_core", "sluice_async")
            add_includedirs("include")
            add_files(p)
            add_tests("cancel_token_test")
    end
end

-- Future<T> tests (sluice-CORE-028, T2). Header-only Future; exercises a
-- thread-driven producer (await blocks until the worker completes) + the
-- cooperative-cancel path. Links sluice_async (for cancel.cpp) + std::thread.
do
    local p = "tests/future_test.cpp"
    if os.isfile(p) then
        target("future_test")
            set_kind("binary")
            set_default(false)
            set_group("test")
            add_deps("sluice_core", "sluice_async")
            add_includedirs("include")
            add_files(p)
            add_tests("future_test")
    end
end

-- Group tests (sluice-CORE-029, T3). Unordered task set; await/cancel whole-
-- group; cancel-propagation boundary. Links sluice_async (group.cpp + cancel).
do
    local p = "tests/group_test.cpp"
    if os.isfile(p) then
        target("group_test")
            set_kind("binary")
            set_default(false)
            set_group("test")
            add_deps("sluice_core", "sluice_async")
            add_includedirs("include")
            add_files(p)
            add_tests("group_test")
    end
end

-- Batch tests (sluice-CORE-030, T4). Grouped completions over AsyncIoContext;
-- uses real I/O (ThreadPoolBackend + temp fds). Links sluice_async (batch.cpp).
do
    local p = "tests/batch_test.cpp"
    if os.isfile(p) then
        target("batch_test")
            set_kind("binary")
            set_default(false)
            set_group("test")
            add_deps("sluice_core", "sluice_async")
            add_includedirs("include")
            add_files(p)
            add_tests("batch_test")
    end
end

-- Fiber state-model tests (sluice-CORE-E1). Pure C++ state machine; no asm yet
-- (E2). Links sluice_async (fiber.cpp + cancel.cpp).
do
    local p = "tests/fiber_test.cpp"
    if os.isfile(p) then
        target("fiber_test")
            set_kind("binary")
            set_default(false)
            set_group("test")
            add_deps("sluice_core", "sluice_async")
            add_includedirs("include")
            add_files(p)
            add_tests("fiber_test")
    end
end

-- Isolated x86_64 fiber context-switch tests (sluice-CORE-E2/E3). NO I/O, no
-- Future/WaitPolicy/AsyncBackend/Group integration. Proves only the asm +
-- trampoline. Links sluice_async (fiber_ctx.cpp). Gated to x86_64 via the
-- `supported` constant in the header; non-x86_64 skips cleanly.
do
    local p = "tests/fiber_ctx_test.cpp"
    if os.isfile(p) then
        target("fiber_ctx_test")
            set_kind("binary")
            set_default(false)
            set_group("test")
            add_deps("sluice_core", "sluice_async")
            add_includedirs("include")
            add_files(p)
            add_tests("fiber_ctx_test")
    end
end

-- E4 single-worker Evented scheduler tests (sluice-CORE-E4). Proves scheduler
-- liveness (B progresses while A awaits a pending op), completion wake path,
-- resume fidelity, exactly-once. Uses FakeAsyncBackend held-pending mode.
-- Gated to x86_64 (depends on fiber_ctx::context_switch).
do
    local p = "tests/e4_scheduler_test.cpp"
    if os.isfile(p) then
        target("e4_scheduler_test")
            set_kind("binary")
            set_default(false)
            set_group("test")
            add_deps("sluice_core", "sluice_async")
            add_includedirs("include")
            add_files(p)
            add_tests("e4_scheduler_test")
    end
end

-- E5-A1 level-triggered scheduler ready-flag wait tests (sluice-CORE-E5-A1).
-- Tests Scheduler::await_ready_flag in isolation (no Future). Proves R1-R5.
-- Gated to x86_64.
do
    local p = "tests/e5_a1_ready_flag_test.cpp"
    if os.isfile(p) then
        target("e5_a1_ready_flag_test")
            set_kind("binary")
            set_default(false)
            set_group("test")
            add_deps("sluice_core", "sluice_async")
            add_includedirs("include")
            add_files(p)
            add_tests("e5_a1_ready_flag_test")
    end
end

-- E5-A2 Evented Future await tests (sluice-CORE-E5-A2). Proves F1-F6: an
-- EventedWaitPolicy Future suspends the current Fiber; another Fiber progresses
-- (liveness); completion resumes the awaiter; resume fidelity; idempotent
-- repeat; Threaded regression. Gated to x86_64.
do
    local p = "tests/e5_a2_evented_future_test.cpp"
    if os.isfile(p) then
        target("e5_a2_evented_future_test")
            set_kind("binary")
            set_default(false)
            set_group("test")
            add_deps("sluice_core", "sluice_async")
            add_includedirs("include")
            add_files(p)
            add_tests("e5_a2_evented_future_test")
    end
end

-- E5-B Evented Group tests (sluice-CORE-E5-B). Proves G1-G6: Evented Group
-- tasks run on Fibers (not std::thread), can suspend inside Future::await,
-- resume, and complete; Threaded regression. Gated to x86_64.
do
    local p = "tests/e5_b_evented_group_test.cpp"
    if os.isfile(p) then
        target("e5_b_evented_group_test")
            set_kind("binary")
            set_default(false)
            set_group("test")
            add_deps("sluice_core", "sluice_async")
            add_includedirs("include")
            add_files(p)
            add_tests("e5_b_evented_group_test")
    end
end

-- E6 scheduler progress tests (sluice-CORE-E6). Proves the hybrid poll/wait
-- progress policy: a Fiber awaiting a real-backend Completion that completes
-- after the runnable queue drains is resumed via wait_one. ThreadPoolBackend is
-- the real completion source (cv-wait). Gated to x86_64.
do
    local p = "tests/e6_scheduler_progress_test.cpp"
    if os.isfile(p) then
        target("e6_scheduler_progress_test")
            set_kind("binary")
            set_default(false)
            set_group("test")
            add_deps("sluice_core", "sluice_async")
            add_includedirs("include")
            add_files(p)
            add_tests("e6_scheduler_progress_test")
    end
end

-- E7 multi-worker scheduler tests (sluice-CORE-E7). Proves worker-local
-- execution state, pinned routing, serialized backend access, MW coordination.
-- Gated to x86_64.
do
    local p = "tests/e7_worker_test.cpp"
    if os.isfile(p) then
        target("e7_worker_test")
            set_kind("binary")
            set_default(false)
            set_group("test")
            add_deps("sluice_core", "sluice_async")
            add_includedirs("include")
            add_files(p)
            add_tests("e7_worker_test")
    end
end



-- E7-C coordination tests (sluice-CORE-E7-C). Serialized backend access probe,
-- quiescence, MW-S3. Gated to x86_64.
do
    local p = "tests/e7_coord_test.cpp"
    if os.isfile(p) then
        target("e7_coord_test")
            set_kind("binary")
            set_default(false)
            set_group("test")
            add_deps("sluice_core", "sluice_async_internal_testing")
            add_includedirs("include", "tests")
            add_files(p)
            add_tests("e7_coord_test")
    end
end

-- Core microbench targets (SLUICE-CORE-010C-F). Built/run via `xmake -g bench`.
local benches = { "small_writes_bench", "copy_strategy_bench", "wal_write_bench",
                  "sync_smoke_bench" }
for _, b in ipairs(benches) do
    sluice_one_file_target("binary", "bench", b, "bench", {"sluice_core", "sluice_bench_common"})
end

-- W1-W4 blocking bench matrix (sluice-CORE-022S). Linked against
-- sluice_bench_common (which carries BlockingIoPool + the matrix CSV helpers).
local sync_benches = { "w1_write_bench", "w2_read_bench", "w3_copy_bench",
                       "w4_durability_bench" }
for _, b in ipairs(sync_benches) do
    sluice_one_file_target("binary", "bench", b, "bench",
                          {"sluice_core", "sluice_bench_common"})
end

-- uring_write_bench needs the experimental uring lib too (stub or real).
sluice_one_file_target("binary", "bench", "uring_write_bench", "bench",
                      {"sluice_core", "sluice_bench_common", "sluice_experimental_uring"})

-- pool_throughput_bench (sluice-CORE-024S): production pool scalability sweep.
sluice_one_file_target("binary", "bench", "pool_throughput_bench", "bench",
                      {"sluice_core", "sluice_bench_common"})

-- async_writes_bench (sluice-CORE-022) needs the async runtime lib too.
sluice_one_file_target("binary", "bench", "async_writes_bench", "bench",
                      {"sluice_core", "sluice_bench_common", "sluice_async"})

-- ---------------------------------------------------------------------------
-- SLUICE-CORE-013B: optional liburing build gate for the experimental spike.
--
-- Normal builds have NO liburing dependency. Pass `--with-liburing=true` (and
-- have liburing available via xrepo/system) to enable the experimental uring
-- targets and define CPPIO_HAS_LIBURING. Without it, the experimental sources
-- compile as unsupported-stubs so the rest of the project is unaffected.
-- ---------------------------------------------------------------------------
option("with-liburing")
    set_default(false)
    set_description("Enable the experimental io_uring spike (requires liburing).")
option_end()

-- SLUICE-CORE-026 (B3): feature gates for io_uring registered buffers/files.
-- Both OFF by default — matching Zig upstream (Io/Uring.zig uses neither
-- registered buffers nor registered files). A future job may implement them
-- under a documented lifetime contract; until then the gates exist so the
-- build can advertise the capability without the implementation. The defines
-- SLUICE_URING_REGISTERED_BUFFERS / SLUICE_URING_REGISTERED_FILES are threaded
-- onto sluice_async only when liburing is also enabled (they are meaningless
-- without a real ring).
option("with-uring-registered-buffers")
    set_default(false)
    set_description("Enable io_uring registered buffers (lifetime contract WIP; off by default).")
option_end()
option("with-uring-registered-files")
    set_default(false)
    set_description("Enable io_uring registered files descriptors (lifetime contract WIP; off by default).")
option_end()

local has_liburing = false
if has_config("with-liburing") then
    -- add_requires with optional=true lets xmake try to fetch liburing; if the
    -- user passed --with-liburing=true but it's unavailable, fail loudly here
    -- rather than silently building stubs.
    add_requires("liburing", {alias = "liburing"})
    has_liburing = true
end

-- When liburing is enabled, the async runtime's UringAsyncBackend compiles its
-- REAL io_uring path (otherwise it is an unsupported stub, sluice-CORE-020B).
-- Thread the same define + package onto sluice_async that sluice_experimental_uring
-- gets, so src/async/uring_backend.cpp sees SLUICE_HAS_LIBURING and links liburing.
if has_liburing then
    target("sluice_async")
        add_defines("SLUICE_HAS_LIBURING", {public = true})
        add_packages("liburing", {public = true})
        if has_config("with-uring-registered-buffers") then
            add_defines("SLUICE_URING_REGISTERED_BUFFERS")
        end
        if has_config("with-uring-registered-files") then
            add_defines("SLUICE_URING_REGISTERED_FILES")
        end
end

-- Experimental uring library. Always defined so the headers/sources exist; the
-- implementation compiles either the real uring path or the unsupported stub
-- based on CPPIO_HAS_LIBURING.
target("sluice_experimental_uring")
    set_kind("static")
    set_default(false)
    set_group("experimental")
    add_includedirs("include", {public = true})
    add_deps("sluice_core")
    add_files("src/experimental/*.cpp")
    if has_liburing then
        add_defines("SLUICE_HAS_LIBURING", {public = true})
        add_packages("liburing", {public = true})
    end

-- ---------------------------------------------------------------------------
-- Quick reference — sanitizer / test commands:
--
--   Default build:       xmake build
--   Debug build:         xmake f -m debug && xmake build
--   Release build:       xmake f -m release && xmake build
--
--   Run all tests:       xmake run -g test
--   Run one test:        xmake run <test_name>_test
--
--   ASan:                xmake f -m asan && xmake build -g test && xmake run -g test
--   TSan:                xmake f -m tsan && xmake build -g test && xmake run -g test
--   UBSan:               xmake f -m ubsan && xmake build -g test && xmake run -g test
--   ASan+UBSan:          xmake f -m asanubsan && xmake build -g test && xmake run -g test
--   Valgrind:            xmake f -m valgrind && xmake build -g test
--                         valgrind --leak-check=full <binary>
--
--   Switch to Clang:     xmake f --toolchain=clang -c && xmake build
--   Clang + ASan:        xmake f --toolchain=clang -m asan -c && xmake build
--
--   Run all examples:    xmake run -g examples
--   Run all benches:     xmake run -g bench
-- ---------------------------------------------------------------------------

-- e7_dup_publication_test — focused regression for the E7-T2 root cause
-- (exactly-once runnable publication). Unit-level make_runnable contract +
-- integration wake-while-runnable scenario. Fails on pre-fix code.
do
    local p = "tests/e7_dup_publication_test.cpp"
    if os.isfile(p) then
        target("e7_dup_publication_test")
            set_kind("binary")
            set_default(false)
            set_group("test")
            add_deps("sluice_core", "sluice_async")
            add_includedirs("include")
            add_files(p)
            add_tests("e7_dup_publication_test")
    end
end

-- e8_steal_test — runnable ownership transfer / work stealing (sluice-CORE-E8).
-- Proves steal = MOVE + OWNER TRANSFER (never PUBLISH); stolen Fiber
-- wake-routes to the thief. Gated to x86_64 (fiber_ctx::supported).
do
    local p = "tests/e8_steal_test.cpp"
    if os.isfile(p) then
        target("e8_steal_test")
            set_kind("binary")
            set_default(false)
            set_group("test")
            add_deps("sluice_core", "sluice_async")
            add_includedirs("include")
            add_files(p)
            add_tests("e8_steal_test")
    end
end

-- e9_external_wake_test — Scheduler park admission + unified wake-source
-- protocol (sluice-CORE-E9). Proves external-thread flag completion wakes a
-- parked Scheduler (no caller re-entry), MIXED-WAKE closure, wake coalescing,
-- the pre-park race, wake-handle lifetime, and E7/E8 runnable/shutdown wake.
-- Gated to x86_64 (fiber_ctx::supported).
do
    local p = "tests/e9_external_wake_test.cpp"
    if os.isfile(p) then
        target("e9_external_wake_test")
            set_kind("binary")
            set_default(false)
            set_group("test")
            add_deps("sluice_core", "sluice_async_internal_testing")
            add_includedirs("include", "tests")
            add_files(p)
            add_tests("e9_external_wake_test")
    end
end

-- e9_wake_handle_lifetime_test — SchedulerWakeHandle callback-lifetime lease
-- (sluice-CORE-E9 LIFETIME-CORRECTIVE). Proves notify() holds Control::mtx
-- (the callback lease) through the Scheduler wake callback, so destruction
-- cannot interleave with an in-flight callback. Deterministic T1 (notifier
-- wins) / T2 (destructor wins) / T3 (stale handle) + concurrent T4 stress.
-- Gated to x86_64 (fiber_ctx::supported).
do
    local p = "tests/e9_wake_handle_lifetime_test.cpp"
    if os.isfile(p) then
        target("e9_wake_handle_lifetime_test")
            set_kind("binary")
            set_default(false)
            set_group("test")
            add_deps("sluice_core", "sluice_async_internal_testing")
            add_includedirs("include", "tests")
            add_files(p)
            add_tests("e9_wake_handle_lifetime_test")
    end
end

-- e10_wait_queue_test — WaitNode/WaitQueue cancellation-safe protocol
-- (sluice-CORE-E10). Pure-protocol tests (no scheduler): wake-vs-cancel single
-- winner, repeated wake/cancel, wake-after-cancel/cancel-after-wake, unlink
-- exactly-once, multiple waiters, node-reuse rejection, destruction invariant,
-- and a high-iteration wake||cancel stress. Header-only WaitNode/WaitQueue, so
-- this links sluice_async only to stay consistent with the async test family.
do
    local p = "tests/e10_wait_queue_test.cpp"
    if os.isfile(p) then
        target("e10_wait_queue_test")
            set_kind("binary")
            set_default(false)
            set_group("test")
            add_deps("sluice_core", "sluice_async")
            add_includedirs("include")
            add_files(p)
            add_tests("e10_wait_queue_test")
    end
end

-- e10_scheduler_wait_test — Scheduler integration of WaitNode/WaitQueue
-- (sluice-CORE-E10). Integration tests with real fibers: C10 exactly-one winner
-- makes the fiber runnable via the canonical wake seam (wake + cancel + race),
-- C11 Drain interaction (MW-S3 wait returns STALLED, no revival of E9 hang).
-- Gated to x86_64 (fiber_ctx::supported).
do
    local p = "tests/e10_scheduler_wait_test.cpp"
    if os.isfile(p) then
        target("e10_scheduler_wait_test")
            set_kind("binary")
            set_default(false)
            set_group("test")
            add_deps("sluice_core", "sluice_async")
            add_includedirs("include")
            add_files(p)
            add_tests("e10_scheduler_wait_test")
    end
end

-- e10_corrective_c1_test — E10-CORRECTIVE C1 external wake-domain classification
-- regression (sluice-CORE-E10). Proves a Live run with an externally-resolvable
-- WaitQueue wait parks (not STALLED) so an external wake_wait_one/cancel_wait
-- resumes the waiter. Fails on uncorrected 0debd21.
do
    local p = "tests/e10_corrective_c1_test.cpp"
    if os.isfile(p) then
        target("e10_corrective_c1_test")
            set_kind("binary")
            set_default(false)
            set_group("test")
            add_deps("sluice_core", "sluice_async")
            add_includedirs("include")
            add_files(p)
            add_tests("e10_corrective_c1_test")
    end
end

-- e10_corrective_c2_c3_test — E10-CORRECTIVE C2 resolution-authority bypass
-- (structural: public wake_one/cancel/cancel_all are not expressible) + C3
-- cancel_all surface (REMOVED) + T4 non-bypass count consistency. Compile-time
-- static_assert + runtime mirror + fiber integration.
do
    local p = "tests/e10_corrective_c2_c3_test.cpp"
    if os.isfile(p) then
        target("e10_corrective_c2_c3_test")
            set_kind("binary")
            set_default(false)
            set_group("test")
            add_deps("sluice_core", "sluice_async")
            add_includedirs("include")
            add_files(p)
            add_tests("e10_corrective_c2_c3_test")
    end
end

-- e10_corrective_c5_test — E10-CORRECTIVE C5 middle-node concurrent unlink
-- topology stress (A<->B<->C; concurrent wake-head-A || cancel-middle-B). Locks
-- in the doubly-linked list topology invariants at a meaningful stress count.
do
    local p = "tests/e10_corrective_c5_test.cpp"
    if os.isfile(p) then
        target("e10_corrective_c5_test")
            set_kind("binary")
            set_default(false)
            set_group("test")
            add_deps("sluice_core", "sluice_async")
            add_includedirs("include")
            add_files(p)
            add_tests("e10_corrective_c5_test")
    end
end

-- e11_timer_wait_test — Deadline / Timer Wait Integration (sluice-CORE-E11).
-- Deterministic production tests: already-due deadline, resource-wins/timer-
-- wins/cancel-wins races at the resolve_ seam, losing-timer cannot publish,
-- stale timer cannot resolve a later wait epoch, storage-reuse epoch isolation,
-- timer retirement closes WaitNode dereference, deadline park liveness,
-- RunMode classification. Uses a controllable monotonic clock + explicit timer
-- driver (NO sleep_for causal proof). Gated to x86_64 (fiber_ctx::supported).
do
    local p = "tests/e11_timer_wait_test.cpp"
    if os.isfile(p) then
        target("e11_timer_wait_test")
            set_kind("binary")
            set_default(false)
            set_group("test")
            add_deps("sluice_core", "sluice_async_internal_testing")
            add_includedirs("include", "tests")
            add_files(p)
            add_tests("e11_timer_wait_test")
    end
end

-- e12_event_test — Async Event synchronization primitive (sluice-CORE-E12-A).
-- Persistent manual-reset Event on the E10/E11 substrate: basic semantics,
-- lost-set admission closure, set-all broadcast, deadline/cancel composition,
-- set/reset epoch isolation, external-thread set, E8 steal, Drain STALLED,
-- destruction contract. Deterministic causal tests (NO sleep_for proof).
-- Gated to x86_64 (fiber_ctx::supported).
do
    local p = "tests/e12_event_test.cpp"
    if os.isfile(p) then
        target("e12_event_test")
            set_kind("binary")
            set_default(false)
            set_group("test")
            add_deps("sluice_core", "sluice_async_internal_testing")
            add_includedirs("include", "tests")
            add_files(p)
            add_tests("e12_event_test")
    end
end

-- e12_semaphore_test — Async counting Semaphore (sluice-CORE-E12-B).
-- Counting Semaphore on the E10/E11/E12-A substrate: construction/available,
-- try_acquire (no barging), immediate + queued acquire, FIFO release transfer
-- /store/overflow, deadline precedence (permit-first), queue-identity-safe
-- cancel, external-thread release, Drain STALLED, destruction contract.
-- Deterministic causal tests (NO sleep_for proof). Gated to x86_64
-- (fiber_ctx::supported).
do
    local p = "tests/e12_semaphore_test.cpp"
    if os.isfile(p) then
        target("e12_semaphore_test")
            set_kind("binary")
            set_default(false)
            set_group("test")
            add_deps("sluice_core", "sluice_async_internal_testing")
            add_includedirs("include", "tests")
            add_files(p)
            add_tests("e12_semaphore_test")
    end
end

-- e12_async_mutex_test — Fiber-suspending Async Mutex (sluice-CORE-E12-C).
-- AsyncMutex on the E10/E11/E12-A/E12-B substrate: construction/try_lock,
-- immediate + queued lock, FIFO direct handoff, deadline precedence
-- (resource-first), queue-identity-safe cancel, external-thread cancel,
-- migration, destruction contract, 500/500 coordination.
-- Deterministic causal tests (NO sleep_for proof). Gated to x86_64
-- (fiber_ctx::supported).
do
    local p = "tests/e12_async_mutex_test.cpp"
    if os.isfile(p) then
        target("e12_async_mutex_test")
            set_kind("binary")
            set_default(false)
            set_group("test")
            add_deps("sluice_core", "sluice_async_internal_testing")
            add_includedirs("include", "tests")
            add_files(p)
            add_tests("e12_async_mutex_test")
    end
end

-- e12_async_condition_test — Fiber-suspending async condition variable
-- (sluice-CORE-E12-D). AsyncCondition bound to one AsyncMutex: two-epoch
-- (Condition wait + mandatory reacquire) protocol, register-before-release
-- lost-notify closure, notify_one FIFO, notify_all snapshot/drain,
-- notify/cancel/expire winner matrix, Ordinary<->Reacquire FIFO mixing,
-- owner-before-publication, inline-Expired retains ownership, destruction
-- contract. Deterministic causal tests via E12ConditionSeam phase seams
-- (NO sleep_for proof). Gated to x86_64 (fiber_ctx::supported). The authority
-- probe (e12_async_condition_authority_probe.cpp) is NOT a target: it is a
-- negative-compile probe driven by the verify script's compile-probe gate.
do
    local p = "tests/e12_async_condition_test.cpp"
    if os.isfile(p) then
        target("e12_async_condition_test")
            set_kind("binary")
            set_default(false)
            set_group("test")
            add_deps("sluice_core", "sluice_async_internal_testing")
            add_includedirs("include", "tests")
            add_files(p)
            add_tests("e12_async_condition_test")
    end
end

-- e12_async_queue_test — AsyncQueue (sluice-CORE-E12-E).
-- P2+P3 scope: QueuePort fast paths (try_push / try_pop / close / snapshot),
-- capacity/FIFO, failed-payload identity, one-shot lease, close idempotency,
-- closed+empty terminal. Exercised via the non-template QueuePort authority +
-- QueueItemFactory (the public AsyncQueue<T> wrapper lands in P8). The
-- blocking/timed wait-admission paths (P4-P6) and Scheduler reconciliation
-- land later; this target covers only the no-Scheduler fast paths. Links
-- sluice_async_internal_testing (the authority lives in the non-template
-- QueuePort, which is in sluice_async; the internal-testing variant keeps
-- the option open for the deterministic phase seams added in P5/P6).
do
    local p = "tests/e12_async_queue_test.cpp"
    if os.isfile(p) then
        target("e12_async_queue_test")
            set_kind("binary")
            set_default(false)
            set_group("test")
            add_deps("sluice_core", "sluice_async_internal_testing")
            add_includedirs("include", "tests")
            add_files(p)
            add_tests("e12_async_queue_test")
    end
end

-- e12_async_mutex_death_test — verifies the Mutex acquisition fail-fast
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
do
    local p = "tests/e12_async_mutex_death_test.cpp"
    if os.isfile(p) and is_plat("linux", "macosx") then
        target("e12_async_mutex_death_test")
            set_kind("binary")
            set_default(false)
            set_group("test")
            add_deps("sluice_core", "sluice_async_internal_testing")
            add_includedirs("include", "tests")
            add_files(p)
            add_tests("e12_async_mutex_death_test")
    end
end

-- e12_async_mutex_nothrow_authority_probe — positive-compile + run probe for
-- the Mutex noexcept contract (ASYNC-MUTEX-NOTHROW-PRODUCTION-IMPLEMENTATION-1
-- §I1). Holds the static_asserts over noexcept(...) and
-- std::is_nothrow_invocable_v<...> for lock/try_lock/unlock so a regression of
-- the noexcept function-type is caught at compile time. NOT a substitute for
-- the death tests (those verify runtime fail-fast behavior). Depends on the
-- internal_testing variant so the seam header resolves, though the probe
-- itself exercises the production Mutex entries.
do
    local p = "tests/e12_async_mutex_nothrow_authority_probe.cpp"
    if os.isfile(p) then
        target("e12_async_mutex_nothrow_authority_probe")
            set_kind("binary")
            set_default(false)
            set_group("test")
            add_deps("sluice_core", "sluice_async_internal_testing")
            add_includedirs("include", "tests")
            add_files(p)
            add_tests("e12_async_mutex_nothrow_authority_probe")
    end
end

-- e12_api_contract_probes — cross-primitive compile-time contract probe
-- (E10-E12-ASYNC-SYNC-API-SEMANTIC-CLOSURE-1).
-- Verifies that every public async synchronization primitive is non-copyable
-- AND non-movable (D5), that WaitOutcome is the four-value vocabulary enum,
-- and that the typed Queue result types remain move-assignable even when
-- T is NOT move-assignable (PR #12 corrective). PURE compile-time probe: all
-- verification is static_assert; main() is trivial. Does NOT replace the
-- per-primitive authority probes — this gates the cross-primitive parity
-- contract only. This normal xmake target is the POSITIVE compile/run probe;
-- scripts/verify-e12-api-contract-negative-compile.sh separately defines each
-- NEG_* macro and requires that compilation fail for the intended deleted
-- special member. Depends on sluice_async_internal_testing so the seam header
-- resolves (the positive probe itself exercises the public production surface).
do
    local p = "tests/e12_api_contract_probes.cpp"
    if os.isfile(p) then
        target("e12_api_contract_probes")
            set_kind("binary")
            set_default(false)
            set_group("test")
            add_deps("sluice_core", "sluice_async_internal_testing")
            add_includedirs("include", "tests")
            add_files(p)
            add_tests("e12_api_contract_probes")
    end
end

-- e12_cross_primitive_parity_test — cross-primitive semantic parity tests
-- (E10-E12-ASYNC-SYNC-API-SEMANTIC-CLOSURE-1). Directly verifies D3
-- resource-first deadline precedence and D4 queue-identity cancellation for
-- Event / Semaphore / AsyncMutex, plus pairwise WaitOutcome enum distinctness
-- and a fresh unresolved WaitNode. AsyncCondition/Queue and dynamic terminal
-- publication remain per-primitive evidence; this TU does not claim them.
-- Gated to x86_64 (fiber_ctx::supported).
do
    local p = "tests/e12_cross_primitive_parity_test.cpp"
    if os.isfile(p) then
        target("e12_cross_primitive_parity_test")
            set_kind("binary")
            set_default(false)
            set_group("test")
            add_deps("sluice_core", "sluice_async_internal_testing")
            add_includedirs("include", "tests")
            add_files(p)
            add_tests("e12_cross_primitive_parity_test")
    end
end
