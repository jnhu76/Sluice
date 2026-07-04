// small_writes_bench (CPPIO-CORE-010C). Measures small-write paths across chunk
// sizes. Modes: raw_file_writer / buffered_writer / observed_buffered_writer /
// vector_writer. CSV to stdout. NOT a performance claim — see
// docs/core-microbench-methodology.md.
#include "bench_common.hpp"
#include "support/temp_path.hpp"

#include <sluice/buffer.hpp>
#include <sluice/file.hpp>
#include <sluice/observed.hpp>

#include <chrono>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <span>
#include <sstream>
#include <string>
#include <vector>

namespace {
using sluice::bench::TempPath;

std::uint64_t now_ns() {
    return static_cast<std::uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count());
}

constexpr std::uint64_t kTotalBytes = 1U << 20; // 1 MiB per cell
constexpr int kWarmupIters = 2;

void bench_raw(const std::string& path, std::size_t chunk, sluice::bench::BenchResult& r) {
    r.mode = "raw_file_writer";
    sluice::SyscallStats& ss = r.syscall_stats;
    std::vector<std::byte> buf(chunk, std::byte{0xAB});
    // warmup
    for (int i = 0; i < kWarmupIters; ++i) {
        sluice::FileWriter w(path, &ss);
        std::uint64_t left = kTotalBytes;
        while (left) {
            auto n = std::min(left, buf.size());
            (void)w.write_all(std::span(buf.data(), n));
            left -= n;
        }
    }
    ss = {};
    auto t0 = now_ns();
    sluice::FileWriter w(path, &ss);
    std::uint64_t left = kTotalBytes;
    std::uint64_t iters = 0;
    while (left) {
        auto n = std::min(left, buf.size());
        (void)w.write_all(std::span(buf.data(), n));
        left -= n;
        ++iters;
    }
    (void)w.flush();
    r.elapsed_ns = now_ns() - t0;
    r.bytes = kTotalBytes;
    r.iterations = iters;
}

void bench_buffered(const std::string& path, std::size_t chunk, sluice::bench::BenchResult& r) {
    r.mode = "buffered_writer";
    sluice::SyscallStats ss;
    sluice::BufferStats bs;
    std::vector<std::byte> buf(chunk, std::byte{0xAB});
    std::vector<std::byte> wbuf(4096);
    for (int i = 0; i < kWarmupIters; ++i) {
        sluice::FileWriter fw(path);
        sluice::BufferedWriter bw(fw, wbuf, &bs);
        std::uint64_t left = kTotalBytes;
        while (left) {
            auto n = std::min(left, buf.size());
            (void)bw.write_all(std::span(buf.data(), n));
            left -= n;
        }
        (void)bw.flush();
    }
    bs = {};
    ss = {};
    auto t0 = now_ns();
    sluice::FileWriter fw(path, &ss);
    sluice::BufferedWriter bw(fw, wbuf, &bs);
    std::uint64_t left = kTotalBytes;
    std::uint64_t iters = 0;
    while (left) {
        auto n = std::min(left, buf.size());
        (void)bw.write_all(std::span(buf.data(), n));
        left -= n;
        ++iters;
    }
    (void)bw.flush();
    r.elapsed_ns = now_ns() - t0;
    r.bytes = kTotalBytes;
    r.iterations = iters;
    r.syscall_stats = ss;
    r.buffer_stats = bs;
}

void bench_observed_buffered(const std::string& path, std::size_t chunk,
                             sluice::bench::BenchResult& r) {
    r.mode = "observed_buffered_writer";
    sluice::SyscallStats ss;
    sluice::BufferStats bs;
    sluice::WriterStats ws;
    std::vector<std::byte> buf(chunk, std::byte{0xAB});
    std::vector<std::byte> wbuf(4096);
    auto t0 = now_ns();
    sluice::FileWriter fw(path, &ss);
    sluice::ObservedWriter ow(fw, ws);
    sluice::BufferedWriter bw(ow, wbuf, &bs);
    std::uint64_t left = kTotalBytes;
    std::uint64_t iters = 0;
    while (left) {
        auto n = std::min(left, buf.size());
        (void)bw.write_all(std::span(buf.data(), n));
        left -= n;
        ++iters;
    }
    (void)bw.flush();
    r.elapsed_ns = now_ns() - t0;
    r.bytes = kTotalBytes;
    r.iterations = iters;
    r.syscall_stats = ss;
    r.buffer_stats = bs;
}

void bench_vector(const std::string& path, std::size_t chunk, sluice::bench::BenchResult& r) {
    r.mode = "vector_writer";
    sluice::SyscallStats ss;
    sluice::VectorStats vs;
    std::vector<std::byte> buf(chunk, std::byte{0xAB});
    auto t0 = now_ns();
    sluice::FileWriter fw(path, &ss, &vs);
    std::uint64_t left = kTotalBytes;
    std::uint64_t iters = 0;
    while (left) {
        sluice::ConstIoSlice sl{std::span<const std::byte>(buf)};
        auto res = fw.write_vec(std::span<const sluice::ConstIoSlice>(&sl, 1));
        if (!res.has_value() || res.value() == 0) {
            break;
        }
        left -= res.value();
        ++iters;
    }
    (void)fw.flush();
    r.elapsed_ns = now_ns() - t0;
    r.bytes = kTotalBytes - left;
    r.iterations = iters;
    r.syscall_stats = ss;
    r.vector_stats = vs;
}

} // namespace

int main() {
    TempPath tp("sluice_bench_sw");
    sluice::bench::print_csv_header(std::cout);
    for (std::size_t chunk : {1U, 8U, 64U, 512U, 4096U}) {
        sluice::bench::BenchResult r;
        r.case_name = "small_writes";
        bench_raw(tp.str(), chunk, r);
        sluice::bench::print_csv_row(std::cout, r);
        bench_buffered(tp.str(), chunk, r);
        sluice::bench::print_csv_row(std::cout, r);
        bench_observed_buffered(tp.str(), chunk, r);
        sluice::bench::print_csv_row(std::cout, r);
        bench_vector(tp.str(), chunk, r);
        sluice::bench::print_csv_row(std::cout, r);
    }
    return 0;
}
