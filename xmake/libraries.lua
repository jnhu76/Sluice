-- Production libraries: sluice_core, sluice_async, sluice_async_internal_testing,
-- and sluice_bench_common.

local R = SLUICE_ROOT

-- Core static library: Reader/Writer abstractions + wrappers.
target("sluice_core")
    set_kind("static")
    add_includedirs(R .. "include", {public = true})
    add_files(R .. "src/*.cpp")

-- Async runtime library (sluice-CORE-017+). OPT-IN, namespace sluice::async.
-- Built alongside the core but kept a separate static lib so the blocking
-- default (sluice_core) carries no async surface. ADR §A6: async is opt-in and
-- BlockingIoContext/Reader/Writer are untouched.
target("sluice_async")
    set_kind("static")
    set_default(false)
    set_group("async")
    add_includedirs(R .. "include", {public = true})
    add_deps("sluice_core")
    add_files(R .. "src/async/*.cpp")
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
    return { R .. "src/async/*.cpp" }
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
    add_includedirs(R .. "include", R .. "tests", {public = true})
    add_deps("sluice_core")
    add_files(async_sources())
    -- The non-installed test controller (defines test_phase + the registry).
    -- Lives in tests/ so the production src/async/*.cpp glob never sees it.
    add_files(R .. "tests/async_test_control.cpp")
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
    add_includedirs(R .. "include", R .. "bench")
    add_deps("sluice_core")
    add_files(R .. "bench/bench_common.cpp", R .. "bench/support/blocking_io_pool.cpp",
              R .. "bench/support/sync_matrix.cpp")
