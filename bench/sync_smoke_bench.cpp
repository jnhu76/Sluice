// sync_smoke_bench (CPPIO-CORE-010F). Tiny environment-sensitive smoke of
// flush_only / sync_data / sync_all. Low iterations. CSV to stdout.
// This is environment-sensitive — see docs/core-microbench-methodology.md §6-7.
#include "bench_common.hpp"

#include <cppio/file.hpp>

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
    TempPath() {
        p = std::filesystem::temp_directory_path() /
            ("cppio_bench_sync_" + std::to_string(reinterpret_cast<std::uintptr_t>(this)) + ".tmp");
    }
    ~TempPath() { std::filesystem::remove(p); }
    std::string str() const { return p.string(); }
};

std::uint64_t now_ns() {
    return static_cast<std::uint64_t>(
        std::chrono::steady_clock::now().time_since_epoch().count());
}

constexpr int kIters = 50;
constexpr std::size_t kWriteBytes = 4096;

void run(const std::string& path, const char* mode, bool do_sync_data, bool do_sync_all,
         cppio::bench::BenchResult& r) {
    r.mode = mode;
    cppio::SyscallStats ss; cppio::SyncStats sync;
    std::vector<std::byte> buf(kWriteBytes, std::byte{0xAA});
    auto t0 = now_ns();
    for (int i = 0; i < kIters; ++i) {
        cppio::FileWriter fw(path, &ss, nullptr, &sync);
        (void)fw.write_all(std::span<const std::byte>(buf));
        (void)fw.flush();
        if (do_sync_data) (void)fw.sync_data();
        if (do_sync_all) (void)fw.sync_all();
    }
    r.elapsed_ns = now_ns() - t0;
    r.bytes = kWriteBytes * kIters;
    r.iterations = kIters;
    r.syscall_stats = ss; r.sync_stats = sync;
}

}  // namespace

int main() {
    TempPath tp;
    cppio::bench::print_csv_header(std::cout);
    cppio::bench::BenchResult r;
    r.case_name = "sync_smoke";
    run(tp.str(), "flush_only", false, false, r); cppio::bench::print_csv_row(std::cout, r);
    run(tp.str(), "sync_data",  true,  false, r); cppio::bench::print_csv_row(std::cout, r);
    run(tp.str(), "sync_all",   false, true,  r); cppio::bench::print_csv_row(std::cout, r);
    std::cerr << "note: sync_smoke results are environment-sensitive\n";
    return 0;
}
