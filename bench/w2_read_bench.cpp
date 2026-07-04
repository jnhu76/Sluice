// sluice-CORE-022S W2 — many independent reads bench.
//
// `streams` readers each read `blocks_per_stream` blocks of `block_size` from
// pre-written files. Two file_layouts: many_files (one FileReader per stream)
// and one_file_many_offsets (one shared fd, positional read_at_exact per block).
//
// Three execution modes (sync-bench-methodology.md §2). CSV to stdout. Results
// are environment-sensitive — NO universal performance claim.
#include "bench_common.hpp"
#include "support/blocking_io_pool.hpp"
#include "support/sync_matrix.hpp"

#include <sluice/file.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <span>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace {

struct TempPath {
    std::filesystem::path p;
    TempPath() {
        std::ostringstream oss;
        oss << "sluice_w2_" << std::hex << reinterpret_cast<std::uintptr_t>(this)
            << "_" << (counter_++) << ".tmp";
        p = std::filesystem::temp_directory_path() / oss.str();
    }
    ~TempPath() { try { std::filesystem::remove(p); } catch (...) {} }
    std::string str() const { return p.string(); }
    static inline long counter_ = 0;
};

std::uint64_t now_ns() {
    return static_cast<std::uint64_t>(
        std::chrono::steady_clock::now().time_since_epoch().count());
}

// Helper: write `total` bytes of 'R' filler into the file at `path`.
void seed_file(const std::string& path, std::size_t total) {
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    std::vector<char> buf(std::min<std::size_t>(total, 1 << 20), 'R');
    std::size_t w = 0;
    while (w < total) {
        std::size_t chunk = std::min(buf.size(), total - w);
        f.write(buf.data(), static_cast<std::streamsize>(chunk));
        w += chunk;
    }
}

void run_stream_many_files(const std::string& path, std::size_t blocks_per_stream,
                           std::size_t block_size, std::byte* scratch) {
    sluice::FileReader r(path);
    std::span<std::byte> buf(scratch, block_size);
    for (std::size_t i = 0; i < blocks_per_stream; ++i) {
        (void)r.read_exact(buf);
    }
}

void run_stream_offsets(sluice::FileReader& shared, std::uint64_t base_offset,
                        std::size_t blocks_per_stream, std::size_t block_size,
                        std::byte* scratch) {
    std::span<std::byte> buf(scratch, block_size);
    std::uint64_t off = base_offset;
    for (std::size_t i = 0; i < blocks_per_stream; ++i) {
        (void)shared.read_at_exact(off, buf);
        off += block_size;
    }
}

struct Params {
    std::size_t streams;
    std::size_t pool_threads;
    std::size_t block_size;
    std::size_t blocks_per_stream;
    std::string file_layout;
};

std::uint64_t run_sequential(const Params& pm, std::byte* scratch,
                             const std::vector<std::string>& paths,
                             sluice::FileReader* shared) {
    auto t0 = now_ns();
    for (std::size_t s = 0; s < pm.streams; ++s) {
        if (pm.file_layout == "many_files") {
            run_stream_many_files(paths[s], pm.blocks_per_stream, pm.block_size, scratch);
        } else {
            run_stream_offsets(*shared, s * pm.blocks_per_stream * pm.block_size,
                               pm.blocks_per_stream, pm.block_size, scratch);
        }
    }
    return now_ns() - t0;
}

std::uint64_t run_bounded_pool(const Params& pm,
                               const std::vector<std::string>& paths,
                               sluice::FileReader* shared) {
    // Each worker needs its own scratch (concurrent). We allocate one per stream.
    std::vector<std::vector<std::byte>> scratches(pm.streams,
                                                  std::vector<std::byte>(pm.block_size));
    sluice::bench::BlockingIoPool pool(pm.pool_threads,
                                       std::max<std::size_t>(pm.pool_threads * 4, 16));
    auto t0 = now_ns();
    for (std::size_t s = 0; s < pm.streams; ++s) {
        std::byte* sc = scratches[s].data();
        if (pm.file_layout == "many_files") {
            std::string path = paths[s];
            pool.submit([path, &pm, sc] {
                run_stream_many_files(path, pm.blocks_per_stream, pm.block_size, sc);
            });
        } else {
            std::uint64_t base = s * pm.blocks_per_stream * pm.block_size;
            pool.submit([shared, base, &pm, sc] {
                run_stream_offsets(*shared, base, pm.blocks_per_stream,
                                   pm.block_size, sc);
            });
        }
    }
    pool.wait_all();
    return now_ns() - t0;
}

std::uint64_t run_thread_per_stream(const Params& pm,
                                    const std::vector<std::string>& paths,
                                    sluice::FileReader* shared) {
    std::vector<std::vector<std::byte>> scratches(pm.streams,
                                                  std::vector<std::byte>(pm.block_size));
    std::vector<std::thread> threads;
    threads.reserve(pm.streams);
    auto t0 = now_ns();
    for (std::size_t s = 0; s < pm.streams; ++s) {
        std::byte* sc = scratches[s].data();
        if (pm.file_layout == "many_files") {
            std::string path = paths[s];
            threads.emplace_back([path, &pm, sc] {
                run_stream_many_files(path, pm.blocks_per_stream, pm.block_size, sc);
            });
        } else {
            std::uint64_t base = s * pm.blocks_per_stream * pm.block_size;
            threads.emplace_back([shared, base, &pm, sc] {
                run_stream_offsets(*shared, base, pm.blocks_per_stream,
                                   pm.block_size, sc);
            });
        }
    }
    for (auto& t : threads) t.join();
    return now_ns() - t0;
}

void emit(const Params& pm, const std::string& mode, std::uint64_t elapsed_ns,
          std::uint64_t threads_used) {
    sluice::bench::matrix::MatrixRow r;
    r.mode = mode;
    r.workload = "W2";
    r.streams = pm.streams;
    r.pool_threads = (mode == "blocking_bounded_pool") ? pm.pool_threads : 0;
    r.block_size = pm.block_size;
    r.buffer_size = 0;
    r.total_bytes = pm.streams * pm.blocks_per_stream * pm.block_size;
    r.total_ops = pm.streams * pm.blocks_per_stream;
    r.threads_used = threads_used;
    r.sync_policy = "none";
    r.file_layout = pm.file_layout;
    sluice::bench::matrix::fill_derived(r, elapsed_ns);
    sluice::bench::matrix::print_row(std::cout, r);
}

void run_cell(const Params& pm) {
    std::vector<std::byte> scratch(pm.block_size);
    std::byte* sc = scratch.data();

    std::vector<std::string> paths;
    std::vector<TempPath> held;   // keep temp files alive across the cell
    for (std::size_t s = 0; s < pm.streams; ++s) {
        held.emplace_back();
        paths.push_back(held.back().str());
        seed_file(paths.back(), pm.blocks_per_stream * pm.block_size);
    }

    TempPath shared_path;
    std::unique_ptr<sluice::FileReader> shared;
    if (pm.file_layout == "one_file_many_offsets") {
        seed_file(shared_path.str(), pm.streams * pm.blocks_per_stream * pm.block_size);
        shared = std::make_unique<sluice::FileReader>(shared_path.str());
    }

    emit(pm, "blocking_sequential",
         run_sequential(pm, sc, paths, shared.get()), 1);
    emit(pm, "blocking_bounded_pool",
         run_bounded_pool(pm, paths, shared.get()), pm.pool_threads);
    emit(pm, "blocking_thread_per_stream",
         run_thread_per_stream(pm, paths, shared.get()), pm.streams);
}

}  // namespace

int main() {
    sluice::bench::matrix::print_header(std::cout);
    constexpr std::size_t small = 512;
    constexpr std::size_t large = 64 * 1024;
    constexpr std::size_t blocks = 64;
    const std::size_t hw = std::max<std::size_t>(2u, std::thread::hardware_concurrency());

    for (std::size_t streams : {1u, 2u, 4u}) {
        Params pm{streams, std::min(streams, hw), small, blocks, "many_files"};
        run_cell(pm);
        pm.file_layout = "one_file_many_offsets";
        run_cell(pm);
    }
    {
        Params pm{4, std::min<std::size_t>(4, hw), large, blocks, "many_files"};
        run_cell(pm);
        pm.file_layout = "one_file_many_offsets";
        run_cell(pm);
    }
    std::cerr << "note: W2 results are environment-sensitive; scoped per "
                 "workload/machine/parameter.\n";
    return 0;
}
