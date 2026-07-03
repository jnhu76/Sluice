// sluice-CORE-022S: shared support for the blocking W1-W4 bench matrix.
//
// This is SEPARATE from bench/bench_common.hpp (the single-stream CSV shape),
// because the matrix requires concurrency columns (streams, pool_threads,
// threads_used) and derived columns (mbps, ops_per_sec, total_ms) per
// docs/sync-bench-methodology.md §4. The single-stream BenchResult is locked by
// tests/bench_csv_test.cpp and must not change.
//
// All state here is caller-owned (no globals): each bench target constructs its
// own MatrixRow and writes its own CSV.
#pragma once

#include <cstdint>
#include <ostream>
#include <string>

namespace sluice::bench::matrix {

// One measured matrix cell: a (workload, mode, parameter-set) run.
struct MatrixRow {
    // Required columns (sync-bench-methodology.md §4):
    std::string mode;          // blocking_sequential | blocking_bounded_pool | blocking_thread_per_stream
    std::string workload;      // W1 | W2 | W3 | W4
    std::uint64_t streams = 0;
    std::uint64_t pool_threads = 0;
    std::uint64_t block_size = 0;
    std::uint64_t buffer_size = 0;
    std::uint64_t total_bytes = 0;
    std::uint64_t total_ops = 0;
    double total_ms = 0.0;
    double mbps = 0.0;
    double ops_per_sec = 0.0;
    std::uint64_t threads_used = 0;
    std::string sync_policy;   // none | sync_data_every_file | ... per durability model
    std::string file_layout;   // many_files | one_file_many_offsets
};

// Print the matrix CSV header (one line) to `out`.
void print_header(std::ostream& out);

// Print one matrix CSV row for `r` to `out`.
void print_row(std::ostream& out, const MatrixRow& r);

// Fill the derived fields (total_ms, mbps, ops_per_sec) given an elapsed time in
// nanoseconds. Helper so bench targets can compute once and reuse.
void fill_derived(MatrixRow& r, std::uint64_t elapsed_ns);

}  // namespace sluice::bench::matrix
