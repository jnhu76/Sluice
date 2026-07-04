// sluice-CORE-022S W4 — durability / sync policy bench.
//
// W1 shape (streams writers), but the variable of interest is `sync_policy`:
//   none                  — no fsync/fdatasync (only flush, which is a no-op).
//   sync_data_every_file  — fdatasync once per stream (per-file cadence).
//   sync_all_every_file   — fsync once per stream (per-file cadence).
//
// (Per-batch cadence reduces to per-file when each stream is one batch; we keep
// the per-file axis which is the dominant cost, decision Q4.) Three execution
// modes for the none policy; sync policies are run on blocking_bounded_pool only
// since the sync cost is the variable of interest, not the scheduling mode.
//
// CSV to stdout. Results are environment-sensitive — NO universal performance
// claim; sync timings depend heavily on the underlying filesystem/disk.
#include "bench_common.hpp"
#include "support/temp_path.hpp"
#include "support/blocking_io_pool.hpp"
#include "support/sync_matrix.hpp"

#include <sluice/file.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <span>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace {
using sluice::bench::TempPath;

std::uint64_t now_ns() {
    return static_cast<std::uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count());
}

enum class SyncPolicy { none, sync_data_per_file, sync_all_per_file };

const char* policy_name(SyncPolicy p) {
    switch (p) {
    case SyncPolicy::none:
        return "none";
    case SyncPolicy::sync_data_per_file:
        return "sync_data_every_file";
    case SyncPolicy::sync_all_per_file:
        return "sync_all_every_file";
    }
    return "none";
}

void apply_policy(sluice::FileWriter& w, SyncPolicy p) {
    switch (p) {
    case SyncPolicy::none:
        break;
    case SyncPolicy::sync_data_per_file:
        (void)w.sync_data();
        break;
    case SyncPolicy::sync_all_per_file:
        (void)w.sync_all();
        break;
    }
}

void run_stream(const std::string& path, std::size_t blocks_per_stream, std::size_t block_size,
                const std::byte* fill, SyncPolicy pol) {
    sluice::FileWriter w(path);
    std::span<const std::byte> buf(fill, block_size);
    for (std::size_t i = 0; i < blocks_per_stream; ++i) {
        (void)w.write_all(buf);
    }
    (void)w.flush();
    apply_policy(w, pol); // durability applied per-file at end of stream
}

struct Params {
    std::size_t streams;
    std::size_t pool_threads;
    std::size_t block_size;
    std::size_t blocks_per_stream;
    SyncPolicy policy;
};

std::uint64_t run_sequential(const Params& pm, const std::byte* fill,
                             const std::vector<std::string>& paths) {
    auto t0 = now_ns();
    for (std::size_t s = 0; s < pm.streams; ++s) {
        run_stream(paths[s], pm.blocks_per_stream, pm.block_size, fill, pm.policy);
    }
    return now_ns() - t0;
}

std::uint64_t run_bounded_pool(const Params& pm, const std::byte* fill,
                               const std::vector<std::string>& paths) {
    sluice::bench::BlockingIoPool pool(pm.pool_threads,
                                       std::max<std::size_t>(pm.pool_threads * 4, 16));
    auto t0 = now_ns();
    for (std::size_t s = 0; s < pm.streams; ++s) {
        const std::string& path = paths[s];
        pool.submit([path, &pm, fill] {
            run_stream(path, pm.blocks_per_stream, pm.block_size, fill, pm.policy);
        });
    }
    pool.wait_all();
    return now_ns() - t0;
}

std::uint64_t run_thread_per_stream(const Params& pm, const std::byte* fill,
                                    const std::vector<std::string>& paths) {
    std::vector<std::thread> threads;
    threads.reserve(pm.streams);
    auto t0 = now_ns();
    for (std::size_t s = 0; s < pm.streams; ++s) {
        const std::string& path = paths[s];
        threads.emplace_back([path, &pm, fill] {
            run_stream(path, pm.blocks_per_stream, pm.block_size, fill, pm.policy);
        });
    }
    for (auto& t : threads) {
        t.join();
    }
    return now_ns() - t0;
}

void emit(const Params& pm, const std::string& mode, std::uint64_t elapsed_ns,
          std::uint64_t threads_used) {
    sluice::bench::matrix::MatrixRow r;
    r.mode = mode;
    r.workload = "W4";
    r.streams = pm.streams;
    r.pool_threads = (mode == "blocking_bounded_pool") ? pm.pool_threads : 0;
    r.block_size = pm.block_size;
    r.buffer_size = 0;
    r.total_bytes = std::uint64_t{pm.streams} * pm.blocks_per_stream * pm.block_size;
    r.total_ops = std::uint64_t{pm.streams} * pm.blocks_per_stream;
    r.threads_used = threads_used;
    r.sync_policy = policy_name(pm.policy);
    r.file_layout = "many_files";
    sluice::bench::matrix::fill_derived(r, elapsed_ns);
    sluice::bench::matrix::print_row(std::cout, r);
}

void run_cell(const Params& pm) {
    std::vector<std::byte> fill_buf(pm.block_size, std::byte{0x44});
    const std::byte* fill = fill_buf.data();
    // Hold TempPath objects alive so their dtors clean up at cell end.
    std::vector<TempPath> held;
    std::vector<std::string> paths;
    for (std::size_t s = 0; s < pm.streams; ++s) {
        held.emplace_back("sluice_w4");
        paths.push_back(held.back().str());
    }

    if (pm.policy == SyncPolicy::none) {
        emit(pm, "blocking_sequential", run_sequential(pm, fill, paths), 1);
        emit(pm, "blocking_bounded_pool", run_bounded_pool(pm, fill, paths), pm.pool_threads);
        emit(pm, "blocking_thread_per_stream", run_thread_per_stream(pm, fill, paths), pm.streams);
    } else {
        // Sync policies: scheduling is not the variable, run on bounded_pool only.
        emit(pm, "blocking_bounded_pool", run_bounded_pool(pm, fill, paths), pm.pool_threads);
    }
}

} // namespace

int main() {
    sluice::bench::matrix::print_header(std::cout);
    constexpr std::size_t block = 4096;
    constexpr std::size_t blocks = 32;
    const std::size_t hw = std::max<std::size_t>(2U, std::thread::hardware_concurrency());

    for (SyncPolicy pol :
         {SyncPolicy::none, SyncPolicy::sync_data_per_file, SyncPolicy::sync_all_per_file}) {
        for (std::size_t streams : {1U, 2U, 4U}) {
            Params pm{.streams = streams,
                      .pool_threads = std::min(streams, hw),
                      .block_size = block,
                      .blocks_per_stream = blocks,
                      .policy = pol};
            run_cell(pm);
        }
    }
    std::cerr << "note: W4 results are environment-sensitive (fsync cost depends "
                 "on filesystem/disk); scoped per workload/machine/parameter.\n";
    return 0;
}
