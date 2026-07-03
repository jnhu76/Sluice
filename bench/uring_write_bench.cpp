// uring_write_bench (CPPIO-CORE-013F). Compares a narrow write workload across
// blocking file writer, blocking write_vec, and experimental uring write batch.
// The uring mode is guarded: without SLUICE_HAS_LIBURING it emits a clear skip
// row. NO broad performance claim — see docs/io-uring-spike.md §11.
#include "bench_common.hpp"

#include <sluice/experimental/uring_write_batch.hpp>
#include <sluice/file.hpp>
#include <sluice/iovec.hpp>

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <iostream>
#include <span>
#include <sstream>
#include <string>
#include <vector>

#if defined(SLUICE_HAS_LIBURING)
#include <fcntl.h>
#include <unistd.h>
#endif

namespace {

struct TempPath {
    std::filesystem::path p;
    TempPath(const char* tag) {
        std::ostringstream oss;
        oss << "sluice_bench_uring_" << tag << "_" << std::hex << reinterpret_cast<std::uintptr_t>(this) << ".tmp";
        p = std::filesystem::temp_directory_path() / oss.str();
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

constexpr int kIters = 200;

// Narrow CSV schema per the spike spec (subset of the common header).
void print_spike_header() {
    std::cout << "case,mode,bytes,iterations,elapsed_ns,"
              << "write_syscalls,write_vec_calls,"
              << "uring_submitted_ops,uring_completed_ops,uring_completion_errors\n";
}

void run_blocking_file(const std::string& path, std::size_t payload_size) {
    sluice::SyscallStats ss;
    std::vector<std::byte> buf(payload_size, std::byte{0x42});
    auto t0 = now_ns();
    for (int i = 0; i < kIters; ++i) {
        sluice::FileWriter fw(path, &ss);
        (void)fw.write_all(std::span<const std::byte>(buf));
        (void)fw.flush();
    }
    auto elapsed = now_ns() - t0;
    std::cout << "uring_write,blocking_file_writer," << (payload_size * kIters) << ','
              << kIters << ',' << elapsed << ','
              << ss.write_syscalls << ",0,0,0,0\n";
}

void run_blocking_vec(const std::string& path, std::size_t payload_size) {
    sluice::SyscallStats ss; sluice::VectorStats vs;
    std::vector<std::byte> buf(payload_size, std::byte{0x43});
    auto t0 = now_ns();
    for (int i = 0; i < kIters; ++i) {
        sluice::FileWriter fw(path, &ss, &vs);
        sluice::ConstIoSlice sl{std::span<const std::byte>(buf)};
        (void)fw.write_vec(std::span<const sluice::ConstIoSlice>(&sl, 1));
        (void)fw.flush();
    }
    auto elapsed = now_ns() - t0;
    std::cout << "uring_write,blocking_write_vec," << (payload_size * kIters) << ','
              << kIters << ',' << elapsed << ','
              << ss.write_syscalls << ',' << vs.write_vec_calls << ",0,0,0\n";
}

#if defined(SLUICE_HAS_LIBURING)
void run_uring(const std::string& path, std::size_t payload_size) {
    sluice::UringStats us;
    std::vector<std::byte> buf(payload_size, std::byte{0x44});
    auto t0 = now_ns();
    for (int i = 0; i < kIters; ++i) {
        sluice::experimental::UringIoContext ctx(16);
        ctx.set_stats(&us);
        int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
        if (fd < 0) continue;
        sluice::experimental::UringWriteBatch batch(16);
        batch.set_stats(&us);
        (void)batch.write_all(fd, std::span<const std::byte>(buf), 0);
        ::close(fd);
    }
    auto elapsed = now_ns() - t0;
    std::cout << "uring_write,uring_write_batch," << (payload_size * kIters) << ','
              << kIters << ',' << elapsed << ",0,0,"
              << us.submitted_ops << ',' << us.completed_ops << ','
              << us.completion_errors << '\n';
}
#else
void run_uring_skip(std::size_t payload_size) {
    // liburing unavailable: emit a clear skip row (zeros, but labelled so the
    // summarizer/reader can tell it was skipped, not measured).
    std::cout << "uring_write,uring_write_batch_SKIPPED_NO_LIBURING," << payload_size
              << ",0,0,0,0,0,0,0\n";
}
#endif

}  // namespace

int main() {
    print_spike_header();
    TempPath tp("wb");
    for (std::size_t psz : {64u, 4096u, 65536u}) {
        run_blocking_file(tp.str(), psz);
        run_blocking_vec(tp.str(), psz);
#if defined(SLUICE_HAS_LIBURING)
        run_uring(tp.str(), psz);
#else
        run_uring_skip(psz);
#endif
    }
    std::cerr << "note: uring_write_bench results are local observations, NOT claims\n";
    return 0;
}
