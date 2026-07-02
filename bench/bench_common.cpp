// Core microbench CSV helpers (CPPIO-CORE-010B).
#include "bench_common.hpp"

namespace cppio::bench {

void print_csv_header(std::ostream& out) {
    out << "case,mode,bytes,iterations,elapsed_ns,"
        << "read_syscalls,write_syscalls,"
        << "read_syscall_bytes,write_syscall_bytes,"
        << "buffered_fast_path_bytes,scratch_path_bytes,"
        << "copy_loop_iterations,"
        << "read_vec_calls,write_vec_calls,"
        << "sync_data_calls,sync_all_calls\n";
}

void print_csv_row(std::ostream& out, const BenchResult& r) {
    out << r.case_name << ',' << r.mode << ','
        << r.bytes << ',' << r.iterations << ',' << r.elapsed_ns << ','
        << r.syscall_stats.read_syscalls << ',' << r.syscall_stats.write_syscalls << ','
        << r.syscall_stats.read_syscall_bytes << ',' << r.syscall_stats.write_syscall_bytes << ','
        << r.copy_stats.buffered_fast_path_bytes << ',' << r.copy_stats.scratch_path_bytes << ','
        << r.copy_stats.copy_loop_iterations << ','
        << r.vector_stats.read_vec_calls << ',' << r.vector_stats.write_vec_calls << ','
        << r.sync_stats.sync_data_calls << ',' << r.sync_stats.sync_all_calls << '\n';
}

}  // namespace cppio::bench
