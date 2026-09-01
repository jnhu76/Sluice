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

-- tax0router_micro_bench (#255 TAX-0 router-fix candidate shootout — Layer
-- A): deterministic synthetic router-lifecycle microbench (install/lookup/
-- retire over the internal-testing micro-op seams + the EXACT production
-- find_live_router_cookie_ under each candidate). Every candidate consumes
-- the identical logical trace; no kernel I/O, no per-op allocation. Also
-- links sluice_async_internal_testing (research instrument only). Driven
-- by scripts/bench/perf-attribution.py `tax0routermicro`.
sluice_one_file_target("binary", "bench", "tax0router_micro_bench", "bench",
                      {"sluice_core", "sluice_async_internal_testing"})

-- tax0router_shootout_bench (#255 TAX-0 router-fix candidate shootout —
-- Layer B): REAL io_uring end-to-end matrix over the EXP-0/U0 geometry
-- (depth-D submit/await pipeline, ApplicationRuntime, fail-closed same-work
-- accounting, per-rep structural witness) with the router fix candidate as
-- the single selectable variable; READ and WRITE arms (write arm verifies
-- the full file word-sum after the last rep). Links
-- sluice_async_internal_testing — candidates are research modes; the
-- production sluice_async keeps R0 behavior. Driven by scripts/bench/
-- perf-attribution.py `tax0routershootout`.
sluice_one_file_target("binary", "bench", "tax0router_shootout_bench", "bench",
                      {"sluice_core", "sluice_async_internal_testing"})

-- tax0_copy_ab_bench (TAX-0 COPY-AB-1 — application-level copy A/B):
-- drives the REAL sluice-copy engine (apps/sluice-copy/copy_task.cpp,
-- run_pipelined_copy_with_backend — the SAME copy task the CLI uses) with
-- the backend as the single selectable variable: REAL UringAsyncBackend
-- under the #256 router-fix research seam (r0 production_baseline vs r1
-- reverse_scan) or the production ThreadPoolBackend default. Links
-- sluice_async_internal_testing (research instrument only — the router
-- seam exists nowhere else) plus the app's copy_task.cpp; it is NOT part
-- of the installed/public sluice-copy surface. uring modes fail closed
-- without a real ring. Driven by scripts/bench/tax0-copy-ab1-run.py;
-- validated by scripts/bench/tax0-copy-ab1-validate.py.
do
    local R = SLUICE_ROOT
    local path = R .. "bench/tax0_copy_ab_bench.cpp"
    local copy_task = R .. "apps/sluice-copy/copy_task.cpp"
    if os.isfile(path) and os.isfile(copy_task) then
        target("tax0_copy_ab_bench")
            set_kind("binary")
            set_default(false)
            set_group("bench")
            add_deps("sluice_core", "sluice_async_internal_testing")
            add_includedirs(R .. "include", R .. "bench",
                            R .. "apps/sluice-copy")
            add_files(path, copy_task)
    end
end

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

-- tax0_z_ladder_bench (#250 TAX-0B — preregistered semantic-floor ladder):
-- Z1 raw liburing bare floor / Z1b minimal semantic-equivalent uring (the
-- frozen F05 checklist as explicit userspace machinery) / Z1bw Z1b + one
-- lost-wake-safe continuation consumer / Z2 AsyncIoContext manual driver
-- (no Scheduler) / Z3 ApplicationRuntime await_completion. Z2/Z3 link the
-- PRODUCTION sluice_async only (no internal-testing seams). The raw arms
-- include <liburing.h> directly (package/include propagate from the
-- sluice_async dep under --with-liburing); without liburing the target
-- compiles to a fail-closed stub. Lives under research/ (experiment
-- harness, rx1 precedent). Driven by research/tax0/scripts/tax0z.py.
do
    local R = SLUICE_ROOT
    if os.isfile(R .. "research/tax0/bench/tax0_z_ladder_bench.cpp") then
        target("tax0_z_ladder_bench")
            set_kind("binary")
            set_default(false)
            set_group("bench")
            add_deps("sluice_core", "sluice_async")
            add_includedirs(R .. "include")
            add_files(R .. "research/tax0/bench/tax0_z_ladder_bench.cpp")
        target_end()
        -- tax0_ablation_bench: the SAME harness linked against
        -- sluice_async_internal_testing (never production sluice_async) so
        -- the TAX-0D F01/F02 R0/R1 ablation modes are installable via CLI.
        -- Research instrument only. F01 R0 reproduces the pre-#261
        -- unconditional-evaluation baseline (production is stats-gated
        -- since #261); F02 R0 remains production behavior.
        target("tax0_ablation_bench")
            set_kind("binary")
            set_default(false)
            set_group("bench")
            add_deps("sluice_core", "sluice_async_internal_testing")
            add_includedirs(R .. "include")
            add_files(R .. "research/tax0/bench/tax0_z_ladder_bench.cpp")
        target_end()
    end
end

-- buf_e0_bench (#263 Phase 2 BUF-E0 — buffer allocation / initialization /
-- first-touch truth): storage-representation arms B0 vector<byte> / B1
-- uninitialized owned / B2 anonymous mmap / B3 page-aligned over lifecycle
-- phases A (alloc->ready), B (alloc->first useful I/O), C (prefaulted
-- steady-state reuse) + D (memory-only first-touch diagnostic). Self-
-- contained (plain pread; the buffer lifecycle is the object, no backend).
-- Driven by research/buf-e0/scripts/bufe0.py; preregistration
-- research/buf-e0/BUF-E0-PREREGISTRATION.md. Research instrument only.
do
    local R = SLUICE_ROOT
    if os.isfile(R .. "bench/buf_e0_bench.cpp") then
        target("buf_e0_bench")
            set_kind("binary")
            set_default(false)
            set_group("bench")
            add_files(R .. "bench/buf_e0_bench.cpp")
        target_end()
    end
end

-- buf_e0_amp_bench (#263 BUF-E0 application amplifier, prereg §8): the
-- realistic PipelineSlot lifecycle end-to-end with slot storage as the only
-- variable — the REAL production engine (apps/sluice-copy/copy_task.cpp,
-- run_pipelined_copy_with_backend) vs a verbatim research replica of the
-- same copy task with selectable slot storage (vector / uninitialized /
-- page-aligned). Same-work fail-closed per rep; runner hashes src/dst
-- post-exit. Research instrument only — production code untouched.
do
    local R = SLUICE_ROOT
    local amp = R .. "bench/buf_e0_amp_bench.cpp"
    local copy_task = R .. "apps/sluice-copy/copy_task.cpp"
    if os.isfile(amp) and os.isfile(copy_task) then
        target("buf_e0_amp_bench")
            set_kind("binary")
            set_default(false)
            set_group("bench")
            add_deps("sluice_core", "sluice_async")
            add_includedirs(R .. "include", R .. "apps/sluice-copy")
            add_files(amp, copy_task)
        target_end()
    end
end
