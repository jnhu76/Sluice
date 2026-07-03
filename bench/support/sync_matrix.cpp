// Implementation of the matrix CSV support (sluice-CORE-022S).
#include "sync_matrix.hpp"

#include <ios>
#include <ostream>

namespace sluice::bench::matrix {

void print_header(std::ostream& out) {
    out << "mode,workload,streams,pool_threads,block_size,buffer_size,"
        << "total_bytes,total_ops,total_ms,mbps,ops_per_sec,threads_used,"
        << "sync_policy,file_layout\n";
}

void print_row(std::ostream& out, const MatrixRow& r) {
    // Fixed/setprecision keeps the floats CSV-stable across locales.
    std::ostream::sentry s(out);
    if (!s) return;
    out.setf(std::ios::fixed, std::ios::floatfield);
    out.precision(3);
    out << r.mode << ',' << r.workload << ','
        << r.streams << ',' << r.pool_threads << ','
        << r.block_size << ',' << r.buffer_size << ','
        << r.total_bytes << ',' << r.total_ops << ','
        << r.total_ms << ',' << r.mbps << ',' << r.ops_per_sec << ','
        << r.threads_used << ','
        << r.sync_policy << ',' << r.file_layout << '\n';
}

void fill_derived(MatrixRow& r, std::uint64_t elapsed_ns) {
    r.total_ms = static_cast<double>(elapsed_ns) / 1'000'000.0;
    if (r.total_ms > 0.0) {
        // bytes / (bytes per MB) / seconds = MB/s.
        r.mbps = static_cast<double>(r.total_bytes) / (1024.0 * 1024.0) /
                 (r.total_ms / 1000.0);
        r.ops_per_sec = static_cast<double>(r.total_ops) / (r.total_ms / 1000.0);
    }
}

}  // namespace sluice::bench::matrix
