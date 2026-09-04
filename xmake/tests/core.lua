-- Core-only correctness tests. Each links sluice_core (or sluice_bench_common
-- when noted). Built/run via `xmake -g test`.

local R = SLUICE_ROOT

-- Correctness tests, one binary per slice.
local tests = {
    "result", "writer", "reader", "fault", "buffer",
    "observed", "copy", "wal", "file", "posix_retry",
    "wrapper_noncopyable", "limit", "measurement",
    "writer_vec", "reader_vec", "file_vec", "wal_vec", "vector_stats",
    "buffered_readable", "copy_fast_path", "copy_stats_fast_path", "copy_strategy",
    "copy_scratch_strategy", "copy_buffered_first_strategy",
    "copy_strategy_stats", "syncable_writer", "file_sync", "wal_writer",
    "io_context_api", "blocking_io_context", "read_vec_all",
    "memory_reader_convenience", "memory_io_context",
    "file_positional", "sync_contract_negative", "io_validation",
}
for _, t in ipairs(tests) do
    sluice_one_file_target("binary", "test", t .. "_test", "tests", "sluice_core")
end

-- bench_csv_test needs the bench helper lib + bench include dir.
do
    local p = R .. "tests/bench_csv_test.cpp"
    if os.isfile(p) then
        target("bench_csv_test")
            set_kind("binary")
            set_default(false)
            set_group("test")
            add_deps("sluice_core", "sluice_bench_common")
            add_includedirs(R .. "include", R .. "bench")
            add_files(p)
            add_tests("bench_csv_test")
    end
end

-- file_close_test (ERR-001, issue #143): compiles src/file.cpp directly WITH
-- SLUICE_FILE_INTERNAL_TESTING so the CloseScript seam (src/file_test_seams.hpp)
-- can inject deterministic close(2) failures at the real syscall boundary. It
-- deliberately does NOT link sluice_core: the seam-enabled file.cpp object
-- would redefine the library's symbols. The sibling TUs are the minimal
-- self-contained closure around file.cpp (base-class key functions in
-- reader/writer, whose read_exact/stream_to pull in copy) — header-only
-- dependencies elsewhere.
do
    local p = R .. "tests/file_close_test.cpp"
    if os.isfile(p) then
        target("file_close_test")
            set_kind("binary")
            set_default(false)
            set_group("test")
            add_files(p, R .. "src/file.cpp", R .. "src/reader.cpp",
                      R .. "src/writer.cpp", R .. "src/copy.cpp",
                      R .. "src/copy_strategy.cpp")
            add_defines("SLUICE_FILE_INTERNAL_TESTING")
            add_includedirs(R .. "include", R .. "src", R .. "tests")
            add_tests("file_close_test")
    end
end

-- blocking_io_pool_test (021S) needs the bench helper lib (it links
-- BlockingIoPool from bench/support/) in addition to the core.
do
    local p = R .. "tests/blocking_io_pool_test.cpp"
    if os.isfile(p) then
        target("blocking_io_pool_test")
            set_kind("binary")
            set_default(false)
            set_group("test")
            add_deps("sluice_core", "sluice_bench_common")
            add_includedirs(R .. "include", R .. "bench")
            add_files(p)
            add_tests("blocking_io_pool_test")
    end
end

-- Production BlockingIoPool tests (sluice-CORE-024S). Core-only: the production
-- pool lives in include/sluice/blocking_io_pool.hpp + src/blocking_io_pool.cpp.
do
    local p = R .. "tests/blocking_io_pool_prod_test.cpp"
    if os.isfile(p) then
        target("blocking_io_pool_prod_test")
            set_kind("binary")
            set_default(false)
            set_group("test")
            add_deps("sluice_core")
            add_includedirs(R .. "include")
            add_files(p)
            add_tests("blocking_io_pool_prod_test")
    end
end

-- Production BlockingIoPool CONCURRENCY stress tests (sluice-CORE-024S).
-- Catches data races / deadlocks the functional tests miss. Run under TSan.
do
    local p = R .. "tests/blocking_io_pool_stress_test.cpp"
    if os.isfile(p) then
        target("blocking_io_pool_stress_test")
            set_kind("binary")
            set_default(false)
            set_group("test")
            add_deps("sluice_core")
            add_includedirs(R .. "include")
            add_files(p)
            add_tests("blocking_io_pool_stress_test")
    end
end

-- Production BlockingIoPool INVARIANT tests (sluice-CORE-024S, category B):
-- exactly-once / no-lost-task / no-double-get / FIFO order.
do
    local p = R .. "tests/blocking_io_pool_invariants_test.cpp"
    if os.isfile(p) then
        target("blocking_io_pool_invariants_test")
            set_kind("binary")
            set_default(false)
            set_group("test")
            add_deps("sluice_core")
            add_includedirs(R .. "include")
            add_files(p)
            add_tests("blocking_io_pool_invariants_test")
    end
end

-- sync_matrix_test (022S) locks the matrix CSV shape.
do
    local p = R .. "tests/sync_matrix_test.cpp"
    if os.isfile(p) then
        target("sync_matrix_test")
            set_kind("binary")
            set_default(false)
            set_group("test")
            add_deps("sluice_core", "sluice_bench_common")
            add_includedirs(R .. "include", R .. "bench")
            add_files(p)
            add_tests("sync_matrix_test")
    end
end
