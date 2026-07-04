// sluice-CORE-022S W3 — many copy streams bench.
//
// `streams` independent reader->writer copies, each copying `bytes_per_stream`
// using a `buffer_size` scratch. The source files are pre-written; the sinks
// are distinct temp files per stream. (No file_layout axis here — copy is
// inherently reader+writer pairs.)
//
// Three execution modes (sync-bench-methodology.md §2). CSV to stdout. Results
// are environment-sensitive — NO universal performance claim.
#include "bench_common.hpp"
#include "support/temp_path.hpp"
#include "support/blocking_io_pool.hpp"
#include "support/sync_matrix.hpp"

#include <sluice/copy.hpp>
#include <sluice/file.hpp>
#include <sluice/limit.hpp>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <exception>
#include <cstdint>
#include <filesystem>
#include <fstream>
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

void seed_file(const std::string& path, std::size_t total) {
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    std::vector<char> buf(std::min<std::size_t>(total, 1 << 20), 'C');
    std::size_t w = 0;
    while (w < total) {
        std::size_t chunk = std::min(buf.size(), total - w);
        f.write(buf.data(), static_cast<std::streamsize>(chunk));
        w += chunk;
    }
}

// One copy stream: open src + dst, copy bytes_per_stream via scratch.
std::uint64_t run_one_copy(const std::string& src, const std::string& dst,
                           std::size_t bytes_per_stream, std::size_t buffer_size,
                           std::byte* scratch) {
    sluice::FileReader r(src);
    sluice::FileWriter w(dst);
    std::span<std::byte> buf(scratch, buffer_size);
    auto res = sluice::copy_all(r, w, buf, sluice::CopyLimit::bytes(bytes_per_stream));
    (void)w.flush();
    return res.has_value() ? res.value() : 0;
}

struct Params {
    std::size_t streams;
    std::size_t pool_threads;
    std::size_t buffer_size;
    std::size_t bytes_per_stream;
};

std::uint64_t run_sequential(const Params& pm, const std::vector<std::string>& srcs,
                             const std::vector<std::string>& dsts, std::byte* scratch) {
    auto t0 = now_ns();
    for (std::size_t s = 0; s < pm.streams; ++s) {
        (void)run_one_copy(srcs[s], dsts[s], pm.bytes_per_stream, pm.buffer_size, scratch);
    }
    return now_ns() - t0;
}

std::uint64_t run_bounded_pool(const Params& pm, const std::vector<std::string>& srcs,
                               const std::vector<std::string>& dsts) {
    std::vector<std::vector<std::byte>> scratches(pm.streams,
                                                  std::vector<std::byte>(pm.buffer_size));
    sluice::bench::BlockingIoPool pool(pm.pool_threads,
                                       std::max<std::size_t>(pm.pool_threads * 4, 16));
    auto t0 = now_ns();
    for (std::size_t s = 0; s < pm.streams; ++s) {
        std::byte* sc = scratches[s].data();
        std::string src = srcs[s];
        std::string dst = dsts[s];
        pool.submit([src, dst, &pm, sc] {
            (void)run_one_copy(src, dst, pm.bytes_per_stream, pm.buffer_size, sc);
        });
    }
    pool.wait_all();
    return now_ns() - t0;
}

std::uint64_t run_thread_per_stream(const Params& pm, const std::vector<std::string>& srcs,
                                    const std::vector<std::string>& dsts) {
    std::vector<std::vector<std::byte>> scratches(pm.streams,
                                                  std::vector<std::byte>(pm.buffer_size));
    // Capture any thread exception so it does not std::terminate the bench; it
    // is rethrown on the caller thread after join. exception_ptr is NOT
    // trivially copyable, so guard with a mutex (portable; std::atomic<
    // exception_ptr> is not guaranteed pre-C++23).
    std::mutex ex_mtx;
    std::exception_ptr captured;
    std::vector<std::thread> threads;
    threads.reserve(pm.streams);
    auto t0 = now_ns();
    for (std::size_t s = 0; s < pm.streams; ++s) {
        std::byte* sc = scratches[s].data();
        std::string src = srcs[s];
        std::string dst = dsts[s];
        threads.emplace_back([src, dst, &pm, sc, &captured, &ex_mtx] {
            try {
                (void)run_one_copy(src, dst, pm.bytes_per_stream, pm.buffer_size, sc);
            } catch (...) {
                std::scoped_lock lk(ex_mtx);
                if (!captured) {
                    captured = std::current_exception();
                }
            }
        });
    }
    for (auto& t : threads) {
        t.join();
    }
    if (captured) {
        std::rethrow_exception(captured);
    }
    return now_ns() - t0;
}

void emit(const Params& pm, const std::string& mode, std::uint64_t elapsed_ns,
          std::uint64_t threads_used) {
    sluice::bench::matrix::MatrixRow r;
    r.mode = mode;
    r.workload = "W3";
    r.streams = pm.streams;
    r.pool_threads = (mode == "blocking_bounded_pool") ? pm.pool_threads : 0;
    r.block_size = 0;
    r.buffer_size = pm.buffer_size;
    r.total_bytes = pm.streams * pm.bytes_per_stream;
    r.total_ops = pm.streams;
    r.threads_used = threads_used;
    r.sync_policy = "none";
    r.file_layout = "many_files";
    sluice::bench::matrix::fill_derived(r, elapsed_ns);
    sluice::bench::matrix::print_row(std::cout, r);
}

void run_cell(const Params& pm) {
    std::vector<TempPath> held;
    std::vector<std::string> srcs;
    std::vector<std::string> dsts;
    for (std::size_t s = 0; s < pm.streams; ++s) {
        held.emplace_back("sluice_w3");
        srcs.push_back(held.back().str());
        seed_file(srcs.back(), pm.bytes_per_stream);
        held.emplace_back("sluice_w3");
        dsts.push_back(held.back().str());
    }
    std::vector<std::byte> scratch(pm.buffer_size);

    emit(pm, "blocking_sequential", run_sequential(pm, srcs, dsts, scratch.data()), 1);
    emit(pm, "blocking_bounded_pool", run_bounded_pool(pm, srcs, dsts), pm.pool_threads);
    emit(pm, "blocking_thread_per_stream", run_thread_per_stream(pm, srcs, dsts), pm.streams);
}

} // namespace

int main() {
    sluice::bench::matrix::print_header(std::cout);
    constexpr std::size_t bytes_per_stream = std::size_t{256} * 1024; // 256 KiB per stream
    const std::size_t hw = std::max<std::size_t>(2U, std::thread::hardware_concurrency());

    for (std::size_t buf : {std::size_t(4) * 1024, std::size_t(64) * 1024}) {
        for (std::size_t streams : {1U, 2U, 4U}) {
            Params pm{streams, std::min(streams, hw), buf, bytes_per_stream};
            run_cell(pm);
        }
    }
    std::cerr << "note: W3 results are environment-sensitive; scoped per "
                 "workload/machine/parameter.\n";
    return 0;
}
