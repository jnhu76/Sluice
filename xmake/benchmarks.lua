-- Bench targets. Built/run via `xmake -g bench`.

-- Core microbench targets (SLUICE-CORE-010C-F).
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

-- M1-A Runtime I/O wait comparison benchmark (brief §13). Measures the public
-- RuntimeTaskContext::await_completion overhead against the low-level
-- Scheduler::await_completion_* baseline (NOT exposed to apps), using
-- FakeAsyncBackend to isolate framework overhead. Links sluice_async.
sluice_one_file_target("binary", "bench", "bench_runtime_io_wait", "bench",
                      {"sluice_core", "sluice_async"})

-- idle_erase_ab_bench (issue #161 repair cost check): trivial short fibers
-- through Scheduler::run at 1/2/4/8 workers, measuring the route/pop/dance
-- hot path that carries the idle-count erase (store(0) baseline vs the
-- repair's exchange(0) + conditional generation bump). A/B protocol in the
-- bench header; cost-check evidence only, no optimization claim (16.7).
sluice_one_file_target("binary", "bench", "idle_erase_ab_bench", "bench",
                      {"sluice_core", "sluice_async"})

-- Grep performance-attribution ladder (docs/verification/
-- performance-attribution.md): L0..L4 over identical deterministic
-- workloads. Links the real sluice-grep engine (grep_task + matcher) for
-- the L4 stage, so the ladder measures the exact production pipeline.
do
    local R = SLUICE_ROOT
    local app_dir = R .. "apps/sluice-grep"
    if os.isfile(R .. "bench/grep_attribution_bench.cpp") and
       os.isfile(app_dir .. "/grep_task.cpp") and
       os.isfile(app_dir .. "/matcher.cpp") then
        target("grep_attribution_bench")
            set_kind("binary")
            set_default(false)
            set_group("bench")
            add_deps("sluice_core", "sluice_async")
            add_includedirs(R .. "include", R .. "bench", R .. "bench/support",
                            app_dir)
            add_files(R .. "bench/grep_attribution_bench.cpp",
                      app_dir .. "/grep_task.cpp", app_dir .. "/matcher.cpp")
        target_end()
    end
    -- Workload file writer for CLI / competitor comparisons (runner helper).
    if os.isfile(R .. "bench/grep_workload_gen.cpp") then
        target("grep_workload_gen")
            set_kind("binary")
            set_default(false)
            set_group("bench")
            add_includedirs(R .. "bench", R .. "bench/support")
            add_files(R .. "bench/grep_workload_gen.cpp")
        target_end()
    end
end

-- overload_backpressure_bench (#199 / V6 guarantee-cost): sustained overload
-- of the bounded RequestArena admission capacity on FakeAsyncBackend —
-- refusal-path latency, accepted-path latency under overload, in-flight
-- high-water, RSS series, drain recovery, and static sizeof probes. Release
-- evidence only; driven/wrapped by scripts/bench/perf-attribution.py
-- `overload` (schema 2, kind "overload").
sluice_one_file_target("binary", "bench", "overload_backpressure_bench", "bench",
                      {"sluice_core", "sluice_async"})

-- e1_abstraction_tax_bench (#221 G0 / E1 Core Cost baseline): the
-- explicit-I/O abstraction-tax ladder (L0 raw pread/pwrite, L1 minimal
-- std::thread pool, L2 ApplicationRuntime + ThreadPoolBackend) over one
-- deterministic positional op stream with fail-closed same-work accounting.
-- Links the PRODUCTION sluice_async only (no internal-testing seams).
-- Driven by scripts/bench/perf-attribution.py `e1` (schema 2, kind
-- "e1tax"); methodology docs/verification/explicit-io-abstraction-tax.md.
sluice_one_file_target("binary", "bench", "e1_abstraction_tax_bench", "bench",
                      {"sluice_core", "sluice_async"})

-- tax0_capacity_bench (#250 TAX-0B/EXP-0 — capacity invariance): the
-- production-path capacity experiment. Independently configures
-- request_capacity C vs active depth D on the REAL ThreadPoolBackend or
-- UringAsyncBackend (request_capacity == queue_depth stays independent),
-- because e1_abstraction_tax_bench intentionally couples capacity == depth
-- for its ladder semantics and stays unchanged. Links the PRODUCTION
-- sluice_async only (no internal-testing seams); the uring arm requires a
-- --with-liburing build and fails closed without a real ring. Driven by
-- scripts/bench/perf-attribution.py `tax0` (schema 2, kind "tax0capacity").
sluice_one_file_target("binary", "bench", "tax0_capacity_bench", "bench",
                      {"sluice_core", "sluice_async"})

-- tax0u0_router_bench (#250 TAX-0 EXP-U0 — router-scan causal ablation):
-- RESEARCH instrument, deliberately different wiring: it links
-- sluice_async_internal_testing (never the production sluice_async) so the
-- guarded EXP-U0 scan seam (scan-direction ablation + exact iteration
-- witness) is visible; without liburing it compiles to a fail-closed stub
-- main. The production build's find_live_router_cookie_ stays untouched.
-- Driven by scripts/bench/perf-attribution.py `tax0u0`.
sluice_one_file_target("binary", "bench", "tax0u0_router_bench", "bench",
                      {"sluice_core", "sluice_async_internal_testing"})

-- rx1_workload_bench (#234 RX-1 — controlled attribution falsification gate):
-- single-shape ApplicationRuntime + ThreadPoolBackend pipeline driver with
-- would_block-aware submission (intervention I2), an AC-1a pull-based
-- observation thread, and process-level OS accounting over the measured
-- window. Links the PRODUCTION sluice_async only. Driven by
-- research/rx1/scripts/rx1.py; methodology research/rx1/RX1_METHOD.md.
-- Lives under research/ (experiment harness, not an E1 ladder stage).
do
    local R = SLUICE_ROOT
    if os.isfile(R .. "research/rx1/bench/rx1_workload_bench.cpp") then
        target("rx1_workload_bench")
            set_kind("binary")
            set_default(false)
            set_group("bench")
            add_deps("sluice_core", "sluice_async")
            add_includedirs(R .. "include")
            add_files(R .. "research/rx1/bench/rx1_workload_bench.cpp")
        target_end()
    end
end
