// sluice-CORE-022S W1 — many independent writes bench.
//
// `streams` writers, each writing `blocks_per_stream` blocks of `block_size`.
// Two file_layouts: many_files (one FileWriter per stream) and
// one_file_many_offsets (one shared fd, positional write_at per block —
// exercises pread/pwrite from job 018S, decision Q3).
//
// Three execution modes (sync-bench-methodology.md §2):
//   blocking_sequential        — one caller thread, serial streams.
//   blocking_bounded_pool      — BlockingIoPool(pool_threads).
//   blocking_thread_per_stream — one std::thread per stream.
//
// CSV to stdout via matrix::print_row. Results are environment-sensitive and
// workload/machine-scoped — NO universal performance claim.
#include "bench_common.hpp"
#include "support/temp_path.hpp"
#include "support/blocking_io_pool.hpp"
#include "support/sync_matrix.hpp"

#include <sluice/file.hpp>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <memory>
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

// One stream: open its file (many_files) or use the shared fd (one_file_many_offsets),
// write blocks_per_stream blocks.
void run_stream_many_files(const std::string& path, std::size_t blocks_per_stream,
                           std::size_t block_size, const std::byte* fill) {
    sluice::FileWriter w(path);
    std::span<const std::byte> buf(fill, block_size);
    for (std::size_t i = 0; i < blocks_per_stream; ++i) {
        (void)w.write_all(buf);
    }
    (void)w.flush();
}

void run_stream_offsets(sluice::FileWriter& shared, std::uint64_t base_offset,
                        std::size_t blocks_per_stream, std::size_t block_size,
                        const std::byte* fill) {
    std::span<const std::byte> buf(fill, block_size);
    std::uint64_t off = base_offset;
    for (std::size_t i = 0; i < blocks_per_stream; ++i) {
        (void)shared.write_at_all(off, buf);
        off += block_size;
    }
}

struct Params {
    std::size_t streams;
    std::size_t pool_threads;
    std::size_t block_size;
    std::size_t blocks_per_stream;
    std::string file_layout; // many_files | one_file_many_offsets
};

std::uint64_t run_sequential(const Params& pm, const std::byte* fill,
                             const std::vector<std::string>& paths, sluice::FileWriter* shared) {
    auto t0 = now_ns();
    for (std::size_t s = 0; s < pm.streams; ++s) {
        if (pm.file_layout == "many_files") {
            run_stream_many_files(paths[s], pm.blocks_per_stream, pm.block_size, fill);
        } else {
            run_stream_offsets(*shared, s * pm.blocks_per_stream * pm.block_size,
                               pm.blocks_per_stream, pm.block_size, fill);
        }
    }
    return now_ns() - t0;
}

std::uint64_t run_bounded_pool(const Params& pm, const std::byte* fill,
                               const std::vector<std::string>& paths, sluice::FileWriter* shared) {
    sluice::bench::BlockingIoPool pool(pm.pool_threads,
                                       std::max<std::size_t>(pm.pool_threads * 4, 16));
    auto t0 = now_ns();
    for (std::size_t s = 0; s < pm.streams; ++s) {
        if (pm.file_layout == "many_files") {
            const std::string& path = paths[s];
            pool.submit([path, &pm, fill] {
                run_stream_many_files(path, pm.blocks_per_stream, pm.block_size, fill);
            });
        } else {
            // NOTE: positional writes share one fd across streams — concurrent
            // pwrite on the same fd is allowed (offsets are disjoint); this is
            // the point of the one_file_many_offsets cell (decision Q3).
            std::uint64_t base = s * pm.blocks_per_stream * pm.block_size;
            pool.submit([shared, base, &pm, fill] {
                run_stream_offsets(*shared, base, pm.blocks_per_stream, pm.block_size, fill);
            });
        }
    }
    pool.wait_all();
    return now_ns() - t0;
}

std::uint64_t run_thread_per_stream(const Params& pm, const std::byte* fill,
                                    const std::vector<std::string>& paths,
                                    sluice::FileWriter* shared) {
    std::vector<std::thread> threads;
    threads.reserve(pm.streams);
    auto t0 = now_ns();
    for (std::size_t s = 0; s < pm.streams; ++s) {
        if (pm.file_layout == "many_files") {
            const std::string& path = paths[s];
            threads.emplace_back([path, &pm, fill] {
                run_stream_many_files(path, pm.blocks_per_stream, pm.block_size, fill);
            });
        } else {
            std::uint64_t base = s * pm.blocks_per_stream * pm.block_size;
            threads.emplace_back([shared, base, &pm, fill] {
                run_stream_offsets(*shared, base, pm.blocks_per_stream, pm.block_size, fill);
            });
        }
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
    r.workload = "W1";
    r.streams = pm.streams;
    r.pool_threads = (mode == "blocking_bounded_pool") ? pm.pool_threads : 0;
    r.block_size = pm.block_size;
    r.buffer_size = 0;
    r.total_bytes = std::uint64_t{pm.streams} * pm.blocks_per_stream * pm.block_size;
    r.total_ops = std::uint64_t{pm.streams} * pm.blocks_per_stream;
    r.threads_used = threads_used;
    r.sync_policy = "none";
    r.file_layout = pm.file_layout;
    sluice::bench::matrix::fill_derived(r, elapsed_ns);
    sluice::bench::matrix::print_row(std::cout, r);
}

void run_cell(const Params& pm) {
    std::vector<std::byte> fill_buf(pm.block_size, std::byte{0x5A});
    const std::byte* fill = fill_buf.data();

    // Hold the TempPath objects alive across the whole cell so their dtors clean
    // up the files when the cell ends. (A bare TempPath{}.str() would let the
    // dtor run immediately and leak the file the writer later creates.)
    std::vector<TempPath> held;
    std::vector<std::string> paths;
    for (std::size_t s = 0; s < pm.streams; ++s) {
        held.emplace_back("sluice_w1");
        paths.push_back(held.back().str());
    }

    // For one_file_many_offsets, one shared fd; positional write_at per stream
    // writes disjoint regions. O_TRUNC is fine — we write everything ourselves.
    TempPath shared_path("sluice_w1");
    std::unique_ptr<sluice::FileWriter> shared;
    if (pm.file_layout == "one_file_many_offsets") {
        shared = std::make_unique<sluice::FileWriter>(shared_path.str());
    }

    emit(pm, "blocking_sequential", run_sequential(pm, fill, paths, shared.get()), 1);
    emit(pm, "blocking_bounded_pool", run_bounded_pool(pm, fill, paths, shared.get()),
         pm.pool_threads);
    emit(pm, "blocking_thread_per_stream", run_thread_per_stream(pm, fill, paths, shared.get()),
         pm.streams);
}

} // namespace

int main() {
    sluice::bench::matrix::print_header(std::cout);

    // Minimal coverage per sync-bench-matrix.md §6: a small + a large block
    // sweep, both file_layouts. Keep iterations modest so the bench is quick.
    constexpr std::size_t small = 512;
    constexpr std::size_t large = std::size_t{64} * 1024;
    constexpr std::size_t blocks = 64;
    const std::size_t hw = std::max<std::size_t>(2U, std::thread::hardware_concurrency());

    for (std::size_t streams : {1U, 2U, 4U}) {
        Params pm{.streams = streams,
                  .pool_threads = std::min(streams, hw),
                  .block_size = small,
                  .blocks_per_stream = blocks,
                  .file_layout = "many_files"};
        run_cell(pm);
        pm.file_layout = "one_file_many_offsets";
        run_cell(pm);
    }
    {
        Params pm{.streams = 4,
                  .pool_threads = std::min<std::size_t>(4, hw),
                  .block_size = large,
                  .blocks_per_stream = blocks,
                  .file_layout = "many_files"};
        run_cell(pm);
        pm.file_layout = "one_file_many_offsets";
        run_cell(pm);
    }
    std::cerr << "note: W1 results are environment-sensitive; scoped per "
                 "workload/machine/parameter.\n";
    return 0;
}
