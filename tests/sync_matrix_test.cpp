// Tests for the W1-W4 matrix CSV support (sluice-CORE-022S). Locks the matrix
// CSV shape so future changes are intentional, mirroring bench_csv_test for the
// single-stream shape.
#include "harness.hpp"

#include "support/sync_matrix.hpp"

#include <sstream>
#include <string>

using namespace sluice::bench::matrix;

SLUICE_TEST_CASE(matrix_header_has_required_columns) {
    std::ostringstream out;
    print_header(out);
    std::string h = out.str();
    // Required columns from sync-bench-methodology.md §4.
    SLUICE_CHECK(h.find("mode,") != std::string::npos);
    SLUICE_CHECK(h.find(",workload,") != std::string::npos);
    SLUICE_CHECK(h.find(",streams,") != std::string::npos);
    SLUICE_CHECK(h.find(",pool_threads,") != std::string::npos);
    SLUICE_CHECK(h.find(",block_size,") != std::string::npos);
    SLUICE_CHECK(h.find(",buffer_size,") != std::string::npos);
    SLUICE_CHECK(h.find(",total_bytes,") != std::string::npos);
    SLUICE_CHECK(h.find(",total_ops,") != std::string::npos);
    SLUICE_CHECK(h.find(",total_ms,") != std::string::npos);
    SLUICE_CHECK(h.find(",mbps,") != std::string::npos);
    SLUICE_CHECK(h.find(",ops_per_sec,") != std::string::npos);
    SLUICE_CHECK(h.find(",threads_used,") != std::string::npos);
    SLUICE_CHECK(h.find(",sync_policy,") != std::string::npos);
    SLUICE_CHECK(h.find(",file_layout") != std::string::npos);
}

SLUICE_TEST_CASE(matrix_row_carries_all_fields) {
    MatrixRow r;
    r.mode = "blocking_bounded_pool";
    r.workload = "W1";
    r.streams = 4;
    r.pool_threads = 4;
    r.block_size = 4096;
    r.buffer_size = 0;
    r.total_bytes = 4 * 100 * 4096;
    r.total_ops = 4 * 100;
    r.total_ms = 12.5;
    r.mbps = 124.0;
    r.ops_per_sec = 32000.0;
    r.threads_used = 4;
    r.sync_policy = "none";
    r.file_layout = "many_files";

    std::ostringstream out;
    print_row(out, r);
    std::string row = out.str();
    SLUICE_CHECK(row.find("blocking_bounded_pool,W1,") != std::string::npos);
    SLUICE_CHECK(row.find(",4,4,4096,0,") != std::string::npos);
    SLUICE_CHECK(row.find(",none,many_files") != std::string::npos);
    SLUICE_CHECK(!row.empty() && row.back() == '\n');
}

SLUICE_TEST_CASE(fill_derived_computes_mbps_and_ops) {
    MatrixRow r;
    r.total_bytes = 10 * 1024 * 1024;   // 10 MiB
    r.total_ops = 1000;
    // 10 ms = 10'000'000 ns.
    fill_derived(r, 10'000'000ULL);
    // 10 MiB in 0.01s -> 1000 MB/s (approx).
    SLUICE_CHECK(r.total_ms > 9.9 && r.total_ms < 10.1);
    SLUICE_CHECK(r.mbps > 990.0 && r.mbps < 1010.0);
    SLUICE_CHECK(r.ops_per_sec > 99000.0 && r.ops_per_sec < 101000.0);
}

SLUICE_TEST_CASE(fill_derived_zero_elapsed_does_not_divide_by_zero) {
    MatrixRow r;
    r.total_bytes = 1024;
    r.total_ops = 10;
    fill_derived(r, 0ULL);
    SLUICE_CHECK(r.total_ms == 0.0);
    SLUICE_CHECK(r.mbps == 0.0);
    SLUICE_CHECK(r.ops_per_sec == 0.0);
}

SLUICE_MAIN()
