// mvp_io_context_copy: opens reader/writer through a BlockingIoContext, copies,
// flushes, syncs (if SyncableWriter), prints stats, verifies bytes. No
// performance claim.
#include <sluice/buffer.hpp>
#include "support/example_helpers.hpp"
#include "support/temp_path.hpp"
#include <sluice/copy.hpp>
#include <sluice/io_context.hpp>
#include <sluice/limit.hpp>
#include <sluice/measurement.hpp>
#include <sluice/sync.hpp>

using sluice::bench::TempPath;
using sluice::bench::file_read_all;

#include <cstdio>
#include <fstream>
#include <span>
#include <string>
#include <vector>

int main() {
    TempPath in_tp("in"), out_tp("out");
    const std::string payload(2048, 'Z');
    {
        std::ofstream o(in_tp.str(), std::ios::binary);
        o.write(payload.data(), static_cast<std::streamsize>(payload.size()));
    }

    sluice::BlockingIoContext ctx;
    sluice::SyscallStats syscall_stats{};
    sluice::SyncStats sync_stats{};
    sluice::CopyStats copy_stats{};

    std::vector<std::byte> scratch(8192);

    auto r = ctx.open_reader(in_tp.str(), sluice::OpenReaderOptions{&syscall_stats});
    if (!r.has_value()) {
        std::fprintf(stderr, "open_reader failed\n");
        return 1;
    }
    auto w = ctx.open_writer(out_tp.str(),
                             sluice::OpenWriterOptions{&syscall_stats, nullptr, &sync_stats});
    if (!w.has_value()) {
        std::fprintf(stderr, "open_writer failed\n");
        return 1;
    }

    auto res = sluice::copy_all(*r.value(), *w.value(), std::span<std::byte>(scratch),
                                sluice::CopyLimit::unlimited(), &copy_stats);
    if (!res.has_value() || res.value() != payload.size()) {
        std::fprintf(stderr, "copy failed or short\n");
        return 1;
    }
    auto flush_res = w.value()->flush();
    if (!flush_res.has_value()) {
        std::fprintf(stderr, "flush failed\n");
        return 1;
    }
    // Sync if the writer exposes the capability.
    auto* sw = dynamic_cast<sluice::SyncableWriter*>(w.value().get());
    if (sw) {
        auto sync_res = sw->sync_data();
        if (!sync_res.has_value()) {
            std::fprintf(stderr, "sync_data failed\n");
            return 1;
        }
    }

    // Verify bytes.
    std::string got;
    if (!file_read_all(out_tp.str(), got) || got != payload) {
        std::fprintf(stderr, "output content mismatch\n");
        return 1;
    }

    std::printf("mvp_io_context_copy: copied %zu bytes via BlockingIoContext\n", payload.size());
    std::printf("  copy_bytes_read=%llu copy_loop_iterations=%llu\n",
                static_cast<unsigned long long>(copy_stats.bytes_read),
                static_cast<unsigned long long>(copy_stats.copy_loop_iterations));
    std::printf("  syscalls: read=%llu write=%llu\n",
                static_cast<unsigned long long>(syscall_stats.read_syscalls),
                static_cast<unsigned long long>(syscall_stats.write_syscalls));
    std::printf("  sync_data_calls=%llu\n",
                static_cast<unsigned long long>(sync_stats.sync_data_calls));
    return 0;
}
