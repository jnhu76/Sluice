// sync_smoke_bench (CPPIO-CORE-010F). Tiny environment-sensitive smoke of
// flush_only / sync_data / sync_all. Low iterations. CSV to stdout.
// This is environment-sensitive — see docs/core-microbench-methodology.md §6-7.
#include "bench_common.hpp"

#include <sluice/file.hpp>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <span>
#include <sstream>
#include <string>
#include <vector>

namespace {

struct TempPath {
    std::filesystem::path p;
    TempPath() {
        std::ostringstream oss;
        oss << "sluice_bench_sync_" << std::hex << reinterpret_cast<std::uintptr_t>(this) << ".tmp";
        p = std::filesystem::temp_directory_path() / oss.str();
    }
    ~TempPath() {
        try {
            std::filesystem::remove(p);
        } catch (...) {}
    }
    std::string str() const { return p.string(); }
};

std::uint64_t now_ns() {
    return static_cast<std::uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count());
}

constexpr int kIters = 50;
constexpr std::size_t kWriteBytes = 4096;

void run(const std::string& path, const char* mode, bool do_sync_data, bool do_sync_all,
         sluice::bench::BenchResult& r) {
    r.mode = mode;
    sluice::SyscallStats ss;
    sluice::SyncStats sync;
    std::vector<std::byte> buf(kWriteBytes, std::byte{0xAA});
    auto t0 = now_ns();
    for (int i = 0; i < kIters; ++i) {
        sluice::FileWriter fw(path, &ss, nullptr, &sync);
        (void)fw.write_all(std::span<const std::byte>(buf));
        (void)fw.flush();
        if (do_sync_data)
            (void)fw.sync_data();
        if (do_sync_all)
            (void)fw.sync_all();
    }
    r.elapsed_ns = now_ns() - t0;
    r.bytes = kWriteBytes * kIters;
    r.iterations = kIters;
    r.syscall_stats = ss;
    r.sync_stats = sync;
}

} // namespace

int main() {
    TempPath tp;
    sluice::bench::print_csv_header(std::cout);
    sluice::bench::BenchResult r;
    r.case_name = "sync_smoke";
    run(tp.str(), "flush_only", false, false, r);
    sluice::bench::print_csv_row(std::cout, r);
    run(tp.str(), "sync_data", true, false, r);
    sluice::bench::print_csv_row(std::cout, r);
    run(tp.str(), "sync_all", false, true, r);
    sluice::bench::print_csv_row(std::cout, r);
    std::cerr << "note: sync_smoke results are environment-sensitive\n";
    return 0;
}
