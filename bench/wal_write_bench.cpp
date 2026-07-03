// wal_write_bench (CPPIO-CORE-010E). Measures scalar vs vector WAL write paths
// and buffered variants, across payload sizes and durability modes. CSV to
// stdout. NOT a performance claim; durability modes are flush-only, not fsync.
#include "bench_common.hpp"

#include <cppio/buffer.hpp>
#include <cppio/file.hpp>
#include <cppio/wal.hpp>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <span>
#include <string>
#include <vector>

namespace {

struct TempPath {
    std::filesystem::path p;
    TempPath(const char* tag) {
        p = std::filesystem::temp_directory_path() /
            ("cppio_bench_wal_" + std::string(tag) + "_" +
             std::to_string(reinterpret_cast<std::uintptr_t>(this)) + ".tmp");
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

constexpr int kRecords = 256;

void run_scalar(const std::string& path, std::size_t payload_size, bool flush_each,
                cppio::bench::BenchResult& r) {
    r.mode = flush_each ? "buffered_scalar/flush_each_batch" : "buffered_scalar/no_sync";
    cppio::VectorStats vs; cppio::SyscallStats ss;
    std::vector<std::byte> payload(payload_size, std::byte{0x77});
    std::vector<std::byte> wbuf(64 * 1024);
    auto t0 = now_ns();
    cppio::FileWriter fw(path, &ss, &vs);
    cppio::BufferedWriter bw(fw, wbuf);
    for (int i = 0; i < kRecords; ++i) {
        auto res = cppio::wal::write_record(bw, std::span<const std::byte>(payload));
        if (!res.has_value()) break;
        if (flush_each) (void)bw.flush();
    }
    (void)bw.flush();
    r.elapsed_ns = now_ns() - t0;
    r.bytes = payload_size * kRecords;
    r.iterations = kRecords;
    r.syscall_stats = ss; r.vector_stats = vs;
}

void run_vector(const std::string& path, std::size_t payload_size, bool flush_each,
                cppio::bench::BenchResult& r) {
    r.mode = flush_each ? "buffered_vector/flush_each_batch" : "buffered_vector/no_sync";
    cppio::VectorStats vs; cppio::SyscallStats ss;
    std::vector<std::byte> payload(payload_size, std::byte{0x76});
    std::vector<std::byte> wbuf(64 * 1024);
    auto t0 = now_ns();
    cppio::FileWriter fw(path, &ss, &vs);
    cppio::BufferedWriter bw(fw, wbuf);
    for (int i = 0; i < kRecords; ++i) {
        auto res = cppio::wal::write_record_vec(bw, std::span<const std::byte>(payload));
        if (!res.has_value()) break;
        if (flush_each) (void)bw.flush();
    }
    (void)bw.flush();
    r.elapsed_ns = now_ns() - t0;
    r.bytes = payload_size * kRecords;
    r.iterations = kRecords;
    r.syscall_stats = ss; r.vector_stats = vs;
}

void run_raw_scalar(const std::string& path, std::size_t payload_size,
                    cppio::bench::BenchResult& r) {
    r.mode = "wal_scalar_write_record/no_sync";
    cppio::VectorStats vs; cppio::SyscallStats ss;
    std::vector<std::byte> payload(payload_size, std::byte{0x73});
    auto t0 = now_ns();
    cppio::FileWriter fw(path, &ss, &vs);
    for (int i = 0; i < kRecords; ++i) {
        auto res = cppio::wal::write_record(fw, std::span<const std::byte>(payload));
        if (!res.has_value()) break;
    }
    (void)fw.flush();
    r.elapsed_ns = now_ns() - t0;
    r.bytes = payload_size * kRecords;
    r.iterations = kRecords;
    r.syscall_stats = ss; r.vector_stats = vs;
}

void run_raw_vector(const std::string& path, std::size_t payload_size,
                    cppio::bench::BenchResult& r) {
    r.mode = "wal_vector_write_record_vec/no_sync";
    cppio::VectorStats vs; cppio::SyscallStats ss;
    std::vector<std::byte> payload(payload_size, std::byte{0x76});
    auto t0 = now_ns();
    cppio::FileWriter fw(path, &ss, &vs);
    for (int i = 0; i < kRecords; ++i) {
        auto res = cppio::wal::write_record_vec(fw, std::span<const std::byte>(payload));
        if (!res.has_value()) break;
    }
    (void)fw.flush();
    r.elapsed_ns = now_ns() - t0;
    r.bytes = payload_size * kRecords;
    r.iterations = kRecords;
    r.syscall_stats = ss; r.vector_stats = vs;
}

}  // namespace

int main() {
    TempPath tp("wal");
    cppio::bench::print_csv_header(std::cout);
    for (std::size_t psz : {16u, 128u, 1024u, 16u * 1024u}) {
        cppio::bench::BenchResult r;
        r.case_name = "wal_write";
        run_raw_scalar(tp.str(), psz, r);    cppio::bench::print_csv_row(std::cout, r);
        run_raw_vector(tp.str(), psz, r);    cppio::bench::print_csv_row(std::cout, r);
        run_scalar(tp.str(), psz, false, r); cppio::bench::print_csv_row(std::cout, r);
        run_vector(tp.str(), psz, false, r); cppio::bench::print_csv_row(std::cout, r);
        run_scalar(tp.str(), psz, true, r);  cppio::bench::print_csv_row(std::cout, r);
        run_vector(tp.str(), psz, true, r);  cppio::bench::print_csv_row(std::cout, r);
    }
    return 0;
}
