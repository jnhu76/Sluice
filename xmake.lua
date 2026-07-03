-- xmake build for the cppio C++ core (Zig std.Io inspired).
-- Zig source under ./zig is design reference only; never built here.
add_rules("mode.debug", "mode.release")

set_languages("c++20")
set_warnings("all", "error")

-- Core static library: Reader/Writer abstractions + wrappers.
target("cppio_core")
    set_kind("static")
    add_includedirs("include", {public = true})
    add_files("src/*.cpp")

-- Bench helper library (CPPIO-CORE-010B). Linked into bench targets + the CSV test.
target("cppio_bench_common")
    set_kind("static")
    set_default(false)
    set_group("bench")
    add_includedirs("include", "bench")
    add_deps("cppio_core")
    add_files("bench/bench_common.cpp")

-- Declare a test/example target only when its source file exists, so xmake
-- does not warn about missing files for slices not yet written.
function cppio_one_file_target(kind, group, name, subdir, deps_list)
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
    "io_context_api", "blocking_io_context",
}
for _, t in ipairs(tests) do
    cppio_one_file_target("binary", "test", t .. "_test", "tests", "cppio_core")
end

-- uring_write_batch_test links the experimental uring lib (stub or real).
target("uring_write_batch_test")
    set_kind("binary")
    set_default(false)
    set_group("test")
    add_deps("cppio_core", "cppio_experimental_uring")
    add_includedirs("include", "bench")
    add_files("tests/uring_write_batch_test.cpp")
    add_tests("uring_write_batch_test")

target("uring_io_context_test")
    set_kind("binary")
    set_default(false)
    set_group("test")
    add_deps("cppio_core", "cppio_experimental_uring")
    add_includedirs("include", "bench")
    add_files("tests/uring_io_context_test.cpp")
    add_tests("uring_io_context_test")

target("uring_stats_test")
    set_kind("binary")
    set_default(false)
    set_group("test")
    add_deps("cppio_core", "cppio_experimental_uring")
    add_includedirs("include", "bench")
    add_files("tests/uring_stats_test.cpp")
    add_tests("uring_stats_test")

-- Buildable examples. Built/run via `xmake -g examples`.
local examples = { "cat", "copy_file", "small_writes", "fault_write", "wal_records",
                   "mvp_copy_pipeline", "mvp_limited_copy", "mvp_wal_vector",
                   "mvp_copy_strategy", "mvp_wal_durable", "mvp_io_context_copy" }
for _, e in ipairs(examples) do
    cppio_one_file_target("binary", "examples", e, "examples", "cppio_core")
end

-- bench_csv_test needs the bench helper lib + bench include dir.
do
    local p = "tests/bench_csv_test.cpp"
    if os.isfile(p) then
        target("bench_csv_test")
            set_kind("binary")
            set_default(false)
            set_group("test")
            add_deps("cppio_core", "cppio_bench_common")
            add_includedirs("include", "bench")
            add_files(p)
            add_tests("bench_csv_test")
    end
end

-- Core microbench targets (CPPIO-CORE-010C-F). Built/run via `xmake -g bench`.
local benches = { "small_writes_bench", "copy_strategy_bench", "wal_write_bench",
                  "sync_smoke_bench" }
for _, b in ipairs(benches) do
    cppio_one_file_target("binary", "bench", b, "bench", {"cppio_core", "cppio_bench_common"})
end

-- ---------------------------------------------------------------------------
-- CPPIO-CORE-013B: optional liburing build gate for the experimental spike.
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
target("cppio_experimental_uring")
    set_kind("static")
    set_default(false)
    set_group("experimental")
    add_includedirs("include", {public = true})
    add_deps("cppio_core")
    add_files("src/experimental/*.cpp")
    if has_liburing then
        add_defines("CPPIO_HAS_LIBURING", {public = true})
        add_packages("liburing", {public = true})
    end

