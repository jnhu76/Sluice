// small_writes_bench (CPPIO-CORE-010C). Measures small-write paths across chunk
// sizes. Modes: raw_file_writer / buffered_writer / observed_buffered_writer /
// vector_writer. CSV to stdout. NOT a performance claim — see
// docs/core-microbench-methodology.md.
#include "bench_common.hpp"

#include <cppio/buffer.hpp>
#include <cppio/file.hpp>
#include <cppio/observed.hpp>

#include <chrono>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <span>
#include <string>
#include <vector>

namespace {

struct TempPath {
    std::filesystem::path p;
    TempPath() {
        p = std::filesystem::temp_directory_path() /
            ("cppio_bench_sw_" + std::to_string(reinterpret_cast<std::uintptr_t>(this)) + ".tmp");
    }
    ~TempPath() {
        try { std::filesystem::remove(p); } catch (...) {}
    }
    std::string str() const { return p.string(); }
};

std::uint64_t now_ns() {
    return static_cast<std::uint64_t>(
        std::chrono::steady_clock::now().time_since_epoch().count());
}

constexpr std::uint64_t kTotalBytes = 1u << 20;  // 1 MiB per cell
constexpr int kWarmupIters = 2;

void bench_raw(const std::string& path, std::size_t chunk, cppio::bench::BenchResult& r) {
    r.mode = "raw_file_writer";
    cppio::SyscallStats& ss = r.syscall_stats;
    std::vector<std::byte> buf(chunk, std::byte{0xAB});
    // warmup
    for (int i = 0; i < kWarmupIters; ++i) {
        cppio::FileWriter w(path, &ss);
        std::uint64_t left = kTotalBytes;
        while (left) { auto n = std::min(left, buf.size()); (void)w.write_all(std::span(buf.data(), n)); left -= n; }
    }
    ss = {};
    auto t0 = now_ns();
    cppio::FileWriter w(path, &ss);
    std::uint64_t left = kTotalBytes, iters = 0;
    while (left) { auto n = std::min(left, buf.size()); (void)w.write_all(std::span(buf.data(), n)); left -= n; ++iters; }
    (void)w.flush();
    r.elapsed_ns = now_ns() - t0;
    r.bytes = kTotalBytes;
    r.iterations = iters;
}

void bench_buffered(const std::string& path, std::size_t chunk, cppio::bench::BenchResult& r) {
    r.mode = "buffered_writer";
    cppio::SyscallStats ss; cppio::BufferStats bs;
    std::vector<std::byte> buf(chunk, std::byte{0xAB});
    std::vector<std::byte> wbuf(4096);
    for (int i = 0; i < kWarmupIters; ++i) {
        cppio::FileWriter fw(path); cppio::BufferedWriter bw(fw, wbuf, &bs);
        std::uint64_t left = kTotalBytes;
        while (left) { auto n = std::min(left, buf.size()); (void)bw.write_all(std::span(buf.data(), n)); left -= n; }
        (void)bw.flush();
    }
    bs = {}; ss = {};
    auto t0 = now_ns();
    cppio::FileWriter fw(path, &ss); cppio::BufferedWriter bw(fw, wbuf, &bs);
    std::uint64_t left = kTotalBytes, iters = 0;
    while (left) { auto n = std::min(left, buf.size()); (void)bw.write_all(std::span(buf.data(), n)); left -= n; ++iters; }
    (void)bw.flush();
    r.elapsed_ns = now_ns() - t0;
    r.bytes = kTotalBytes; r.iterations = iters;
    r.syscall_stats = ss; r.buffer_stats = bs;
}

void bench_observed_buffered(const std::string& path, std::size_t chunk, cppio::bench::BenchResult& r) {
    r.mode = "observed_buffered_writer";
    cppio::SyscallStats ss; cppio::BufferStats bs; cppio::WriterStats ws;
    std::vector<std::byte> buf(chunk, std::byte{0xAB});
    std::vector<std::byte> wbuf(4096);
    auto t0 = now_ns();
    cppio::FileWriter fw(path, &ss);
    cppio::ObservedWriter ow(fw, ws);
    cppio::BufferedWriter bw(ow, wbuf, &bs);
    std::uint64_t left = kTotalBytes, iters = 0;
    while (left) { auto n = std::min(left, buf.size()); (void)bw.write_all(std::span(buf.data(), n)); left -= n; ++iters; }
    (void)bw.flush();
    r.elapsed_ns = now_ns() - t0;
    r.bytes = kTotalBytes; r.iterations = iters;
    r.syscall_stats = ss; r.buffer_stats = bs;
}

void bench_vector(const std::string& path, std::size_t chunk, cppio::bench::BenchResult& r) {
    r.mode = "vector_writer";
    cppio::SyscallStats ss; cppio::VectorStats vs;
    std::vector<std::byte> buf(chunk, std::byte{0xAB});
    auto t0 = now_ns();
    cppio::FileWriter fw(path, &ss, &vs);
    std::uint64_t left = kTotalBytes, iters = 0;
    while (left) {
        cppio::ConstIoSlice sl{std::span<const std::byte>(buf)};
        auto res = fw.write_vec(std::span<const cppio::ConstIoSlice>(&sl, 1));
        if (!res.has_value() || res.value() == 0) break;
        left -= res.value(); ++iters;
    }
    (void)fw.flush();
    r.elapsed_ns = now_ns() - t0;
    r.bytes = kTotalBytes - left; r.iterations = iters;
    r.syscall_stats = ss; r.vector_stats = vs;
}

}  // namespace

int main() {
    TempPath tp;
    cppio::bench::print_csv_header(std::cout);
    for (std::size_t chunk : {1u, 8u, 64u, 512u, 4096u}) {
        cppio::bench::BenchResult r;
        r.case_name = "small_writes";
        bench_raw(tp.str(), chunk, r);              cppio::bench::print_csv_row(std::cout, r);
        bench_buffered(tp.str(), chunk, r);         cppio::bench::print_csv_row(std::cout, r);
        bench_observed_buffered(tp.str(), chunk, r);cppio::bench::print_csv_row(std::cout, r);
        bench_vector(tp.str(), chunk, r);           cppio::bench::print_csv_row(std::cout, r);
    }
    return 0;
}
