-- xmake build for the sluice C++ core (Zig std.Io inspired).
-- Zig source under ./zig is design reference only; never built here.
add_rules("mode.debug", "mode.release", "mode.asan", "mode.tsan", "mode.ubsan", "mode.valgrind")

-- Combined ASan + UBSan: no built-in mode, set policies directly.
if is_mode("asanubsan") then
    set_policy("build.sanitizer.address", true)
    set_policy("build.sanitizer.undefined", true)
end

set_languages("c++20")
set_warnings("all", "error")

-- Core static library: Reader/Writer abstractions + wrappers.
target("sluice_core")
    set_kind("static")
    add_includedirs("include", {public = true})
    add_files("src/*.cpp")

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

-- TEMP btest.
do
    local p = "tests/_btest.cpp"
    if os.isfile(p) then
        target("_btest")
            set_kind("binary")
            set_default(false)
            set_group("test")
            add_deps("sluice_core")
            add_includedirs("include", "tests")
            add_files(p)
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

local has_liburing = false
if has_config("with-liburing") then
    -- add_requires with optional=true lets xmake try to fetch liburing; if the
    -- user passed --with-liburing=true but it's unavailable, fail loudly here
    -- rather than silently building stubs.
    add_requires("liburing", {alias = "liburing"})
    has_liburing = true
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

