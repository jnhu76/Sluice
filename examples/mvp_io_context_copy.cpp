// mvp_io_context_copy: opens reader/writer through a BlockingIoContext, copies,
// flushes, syncs (if SyncableWriter), prints stats, verifies bytes. No
// performance claim.
#include <cppio/buffer.hpp>
#include <cppio/copy.hpp>
#include <cppio/io_context.hpp>
#include <cppio/limit.hpp>
#include <cppio/measurement.hpp>
#include <cppio/sync.hpp>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <span>
#include <string>
#include <vector>

namespace {

struct TempPath {
    std::filesystem::path p;
    TempPath(const char* tag) {
        p = std::filesystem::temp_directory_path() /
            ("cppio_ioctx_copy_" + std::string(tag) + "_" +
             std::to_string(reinterpret_cast<std::uintptr_t>(this)) + ".tmp");
    }
    ~TempPath() {
        try { std::filesystem::remove(p); } catch (...) {}
    }
    std::string str() const { return p.string(); }
};

bool file_read_all(const std::string& path, std::string& out) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    out.assign(std::istreambuf_iterator<char>(in), {});
    return true;
}

}  // namespace

int main() {
    TempPath in_tp("in"), out_tp("out");
    const std::string payload(2048, 'Z');
    {
        std::ofstream o(in_tp.str(), std::ios::binary);
        o.write(payload.data(), static_cast<std::streamsize>(payload.size()));
    }

    cppio::BlockingIoContext ctx;
    cppio::SyscallStats syscall_stats{};
    cppio::SyncStats sync_stats{};
    cppio::CopyStats copy_stats{};

    std::vector<std::byte> scratch(8192);

    auto r = ctx.open_reader(in_tp.str(), cppio::OpenReaderOptions{&syscall_stats});
    if (!r.has_value()) {
        std::fprintf(stderr, "open_reader failed\n");
        return 1;
    }
    auto w = ctx.open_writer(out_tp.str(),
                             cppio::OpenWriterOptions{&syscall_stats, nullptr, &sync_stats});
    if (!w.has_value()) {
        std::fprintf(stderr, "open_writer failed\n");
        return 1;
    }

    auto res = cppio::copy_all(*r.value(), *w.value(), std::span<std::byte>(scratch),
                               cppio::CopyLimit::unlimited(), &copy_stats);
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
    auto* sw = dynamic_cast<cppio::SyncableWriter*>(w.value().get());
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

    std::printf("mvp_io_context_copy: copied %zu bytes via BlockingIoContext\n",
                payload.size());
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
