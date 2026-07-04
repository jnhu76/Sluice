// Core microbench common helpers (CPPIO-CORE-010B). Shared by all bench targets.
// Emits CSV rows per the methodology in docs/core-microbench-methodology.md.
#pragma once

#include <sluice/measurement.hpp>

#include <cstdint>
#include <iostream>
#include <string>

namespace sluice::bench {

// One measured cell: a (case, mode) run with its stats. `case_name` is the
// bench family (e.g. "small_writes"), `mode` the variant (e.g. "buffered").
struct BenchResult {
    std::string case_name;
    std::string mode;
    std::uint64_t bytes = 0;
    std::uint64_t iterations = 0;
    std::uint64_t elapsed_ns = 0;

    SyscallStats syscall_stats;
    BufferStats buffer_stats;
    CopyStats copy_stats;
    VectorStats vector_stats;
    SyncStats sync_stats;
};

// Print the CSV header (one line) to `out`.
void print_csv_header(std::ostream& out);

// Print one CSV row for `r` to `out`. case/mode are emitted as-is; if they
// contain commas the caller must sanitize, but bench names are fixed literals.
void print_csv_row(std::ostream& out, const BenchResult& r);

} // namespace sluice::bench
