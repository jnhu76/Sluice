// sluice-CORE-022 async bench harness: async (ThreadPoolBackend) vs engineered
// blocking baseline, W1-shape (many positional writes).
//
// Compares two paths for the same workload (N positional writes to a file):
//   - blocking_sequential: one thread, FileWriter::write_at_all per block.
//   - async_threadpool:    submit N write ops to ThreadPoolBackend, reap via
//                           wait_one. Overlaps in-flight writes on worker threads.
//
// The PURPOSE is to give async a defined blocking baseline to compare against
// (the async-runtime analogue of the sync-runtime W1 bench). CSV via BenchResult.
// Results are environment-sensitive — NO universal performance claim. Debug
// builds cap absolute MB/s; re-run in release for tuning-grade numbers.
#include "bench_common.hpp"

#include <sluice/async/async_io_context.hpp>
#include <sluice/async/completion.hpp>
#include <sluice/async/op_helpers.hpp>
#include <sluice/async/threadpool_backend.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <fcntl.h>
#include <filesystem>
#include <iostream>
#include <sstream>
#include <string>
#include <unistd.h>
#include <vector>

namespace {

struct TempPath {
    std::filesystem::path p;
    TempPath() {
        std::ostringstream oss;
        oss << "sluice_async_bench_" << std::hex
            << reinterpret_cast<std::uintptr_t>(this) << "_" << (counter_++) << ".tmp";
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

struct Params {
    std::size_t ops;
    std::size_t block_size;
};

// Blocking baseline via raw fd + pwrite (positional, matching the async path's
// pwrite semantics for a fair apples-to-apples comparison). Returns elapsed ns.
std::uint64_t run_blocking(const Params& pm, int fd, const std::byte* fill) {
    auto t0 = now_ns();
    for (std::size_t i = 0; i < pm.ops; ++i) {
        std::size_t off = 0;
        while (off < pm.block_size) {
            ssize_t n = ::pwrite(fd, fill + off, pm.block_size - off,
                                 static_cast<off_t>(i * pm.block_size + off));
            if (n <= 0) break;
            off += static_cast<std::size_t>(n);
        }
    }
    (void)::fdatasync(fd);
    return now_ns() - t0;
}

// Async path: submit N positional writes, reap via wait_one.
std::uint64_t run_async_threadpool(const Params& pm, int fd, const std::byte* fill) {
    sluice::async::AsyncIoContext ctx(std::make_unique<sluice::async::ThreadPoolBackend>());
    std::vector<sluice::async::Completion<std::size_t>> cs(pm.ops);
    auto t0 = now_ns();
    for (std::size_t i = 0; i < pm.ops; ++i) {
        (void)ctx.submit_write(sluice::async::WriteOp{
            fd, fill, pm.block_size, static_cast<std::uint64_t>(i) * pm.block_size}, cs[i]);
    }
    std::size_t reaped = 0;
    while (reaped < pm.ops) {
        auto r = ctx.wait_one();
        if (!r.has_value()) break;
        reaped += r.value();
    }
    return now_ns() - t0;
}

void emit(const std::string& mode, const Params& pm, std::uint64_t elapsed_ns) {
    sluice::bench::BenchResult r;
    r.case_name = "async_w1_writes";
    r.mode = mode;
    r.bytes = pm.ops * pm.block_size;
    r.iterations = pm.ops;
    r.elapsed_ns = elapsed_ns;
    sluice::bench::print_csv_row(std::cout, r);
}

}  // namespace

int main() {
    sluice::bench::print_csv_header(std::cout);

    for (std::size_t block_size : {std::size_t(512), std::size_t(4) * 1024}) {
        for (std::size_t ops : {std::size_t(64), std::size_t(256)}) {
            Params pm{ops, block_size};
            std::vector<std::byte> fill(block_size, std::byte{0xA5});

            TempPath tp_blocking;
            int fd_blocking = ::open(tp_blocking.str().c_str(),
                                     O_RDWR | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
            if (fd_blocking < 0) { std::cerr << "blocking open failed\n"; return 1; }
            emit("blocking_sequential", pm, run_blocking(pm, fd_blocking, fill.data()));
            ::close(fd_blocking);

            // Async writes need a raw fd (ThreadPoolBackend uses pwrite on fd).
            TempPath tp_async;
            int fd = ::open(tp_async.str().c_str(), O_RDWR | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
            if (fd < 0) {
                std::cerr << "async bench: open failed\n";
                return 1;
            }
            emit("async_threadpool", pm, run_async_threadpool(pm, fd, fill.data()));
            ::close(fd);
        }
    }
    std::cerr << "note: async bench results are environment-sensitive (debug build, "
                 "single machine); scoped per workload/parameter. NO universal claim.\n";
    return 0;
}
