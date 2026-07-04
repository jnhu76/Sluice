// Tests for the bench CSV helpers (CPPIO-CORE-010B). Verifies the header has the
// expected columns, a row carries the fields, and elapsed_ns is present.
#include "harness.hpp"

#include "../bench/bench_common.hpp"

#include <sstream>
#include <string>

SLUICE_TEST_CASE(bench_csv_header_has_expected_columns) {
    std::ostringstream out;
    sluice::bench::print_csv_header(out);
    std::string h = out.str();
    for (const auto* col :
         {"case", "mode", "bytes", "iterations", "elapsed_ns", "read_syscalls", "write_syscalls",
          "buffered_fast_path_bytes", "scratch_path_bytes", "read_vec_calls", "write_vec_calls",
          "sync_data_calls", "sync_all_calls"}) {
        SLUICE_CHECK(h.find(col) != std::string::npos);
    }
}

SLUICE_TEST_CASE(bench_csv_row_carries_case_mode_and_fields) {
    sluice::bench::BenchResult r;
    r.case_name = "small_writes";
    r.mode = "buffered";
    r.bytes = 1024;
    r.iterations = 100;
    r.elapsed_ns = 12345;
    r.syscall_stats.write_syscalls = 7;
    r.copy_stats.scratch_path_bytes = 999;
    r.sync_stats.sync_all_calls = 2;
    std::ostringstream out;
    sluice::bench::print_csv_row(out, r);
    std::string row = out.str();
    SLUICE_CHECK(row.find("small_writes,buffered,1024,100,12345,") != std::string::npos);
    SLUICE_CHECK(row.find(",7,") != std::string::npos);   // write_syscalls
    SLUICE_CHECK(row.find(",999,") != std::string::npos); // scratch_path_bytes
    SLUICE_CHECK(row.find(",2\n") != std::string::npos);  // sync_all_calls last
}

SLUICE_TEST_CASE(bench_csv_row_has_elapsed_ns_field) {
    sluice::bench::BenchResult r;
    r.elapsed_ns = 42;
    std::ostringstream out;
    sluice::bench::print_csv_row(out, r);
    SLUICE_CHECK(out.str().find(",42,") != std::string::npos);
}

SLUICE_MAIN()
