// mvp_copy_pipeline: the canonical MVP composition.
//   FileReader -> BufferedReader -> copy_all -> BufferedWriter -> ObservedWriter -> FileWriter
// Verifies output bytes match input; prints composition stats. Not a benchmark.
#include <cppio/buffer.hpp>
#include <cppio/copy.hpp>
#include <cppio/file.hpp>
#include <cppio/limit.hpp>
#include <cppio/measurement.hpp>
#include <cppio/observed.hpp>

#include <cstdio>
#include <cstdlib>
#include <cstring>
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
            ("cppio_mvp_" + std::string(tag) + "_" +
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

const char* stop_reason(const cppio::CopyStats& s) {
    if (s.eof_stops) return "eof";
    if (s.limit_stops) return "limit";
    if (s.reader_error_stops) return "reader_error";
    if (s.writer_error_stops) return "writer_error";
    return "none";
}

}  // namespace

int main() {
    // 1. Create a small input file.
    TempPath in_tp("in"), out_tp("out");
    const std::string payload(2048, 'Z');  // larger than the 1KiB read buffer
    {
        std::ofstream o(in_tp.str(), std::ios::binary);
        o.write(payload.data(), static_cast<std::streamsize>(payload.size()));
    }

    // 2. Run the composite pipeline.
    cppio::BufferStats buffer_stats{};
    cppio::WriterStats writer_stats{};
    cppio::SyscallStats syscall_stats{};
    cppio::CopyStats copy_stats{};

    std::vector<std::byte> read_buf(1024);
    std::vector<std::byte> write_buf(1024);
    std::vector<std::byte> scratch(8192);

    // The total copied across the prime read + the copy_all (both feed the
    // output). The prime read pulls more into the read buffer than it consumes,
    // leaving unread buffered bytes for copy_all's fast path to drain.
    std::size_t primed_bytes = 0;

    {
        cppio::FileReader file_in(in_tp.str(), &syscall_stats);
        if (!file_in.opened()) {
            std::fprintf(stderr, "cannot open input\n");
            return 1;
        }
        cppio::BufferedReader buffered_in(file_in, read_buf, &buffer_stats);

        cppio::FileWriter file_out(out_tp.str(), &syscall_stats);
        if (!file_out.opened()) {
            std::fprintf(stderr, "cannot open output\n");
            return 1;
        }
        cppio::ObservedWriter observed_out(file_out, writer_stats);
        cppio::BufferedWriter buffered_out(observed_out, write_buf, &buffer_stats);

        // Prime: read a few bytes out of the BufferedReader and write them
        // directly, leaving the rest buffered for copy_all's fast path.
        std::vector<std::byte> primed(64);
        auto pr = buffered_in.read_some(std::span<std::byte>(primed));
        if (!pr.has_value() || pr.value() != primed.size()) {
            std::fprintf(stderr, "prime read failed\n");
            return 1;
        }
        primed_bytes = primed.size();
        auto pw = buffered_out.write_all(
            std::span<const std::byte>(primed.data(), primed.size()));
        if (!pw.has_value()) {
            std::fprintf(stderr, "prime write failed\n");
            return 1;
        }

        // copy_all drains the remaining buffered bytes via the fast path first,
        // then falls back to scratch reads.
        auto res = cppio::copy_all(buffered_in, buffered_out,
                                   std::span<std::byte>(scratch),
                                   cppio::CopyLimit::unlimited(), &copy_stats);
        if (!res.has_value()) {
            std::fprintf(stderr, "copy failed: %s\n",
                         cppio::to_string(res.error().code).data());
            return 1;
        }
        auto flush_res = buffered_out.flush();  // outermost flush, propagates inward
        if (!flush_res.has_value()) {
            std::fprintf(stderr, "flush failed: %s\n",
                         cppio::to_string(flush_res.error().code).data());
            return 1;
        }
        if (res.value() + primed_bytes != payload.size()) {
            std::fprintf(stderr, "byte count mismatch: copied %llu + primed %zu, want %zu\n",
                         static_cast<unsigned long long>(res.value()), primed_bytes, payload.size());
            return 1;
        }
        // The buffered fast path must have served bytes: the prime left unread
        // buffered data that copy_all drained before any scratch read.
        if (copy_stats.buffered_fast_path_bytes == 0) {
            std::fprintf(stderr, "buffered fast path did not engage\n");
            return 1;
        }
    }

    // 3. Verify output bytes match input bytes.
    std::string got;
    if (!file_read_all(out_tp.str(), got) || got != payload) {
        std::fprintf(stderr, "output content mismatch\n");
        return 1;
    }

    // 4. Print composition stats, including the buffered fast path counters.
    std::printf("mvp_copy_pipeline: copied %zu bytes (primed %zu + copy %llu), stop=%s\n",
                payload.size(), primed_bytes,
                static_cast<unsigned long long>(copy_stats.bytes_read),
                stop_reason(copy_stats));
    std::printf("  copied_bytes=%zu\n", payload.size());
    std::printf("  buffered_fast_path_bytes=%llu\n",
                static_cast<unsigned long long>(copy_stats.buffered_fast_path_bytes));
    std::printf("  scratch_path_bytes=%llu\n",
                static_cast<unsigned long long>(copy_stats.scratch_path_bytes));
    std::printf("  buffer_hits=%llu\n",
                static_cast<unsigned long long>(buffer_stats.read_buffer_hits));
    std::printf("  buffer_misses=%llu\n",
                static_cast<unsigned long long>(buffer_stats.read_buffer_misses));
    std::printf("  copy_loop_iterations=%llu bytes_read=%llu bytes_written=%llu\n",
                static_cast<unsigned long long>(copy_stats.copy_loop_iterations),
                static_cast<unsigned long long>(copy_stats.bytes_read),
                static_cast<unsigned long long>(copy_stats.bytes_written));
    std::printf("  syscalls: read=%llu write=%llu (errors read=%llu write=%llu)\n",
                static_cast<unsigned long long>(syscall_stats.read_syscalls),
                static_cast<unsigned long long>(syscall_stats.write_syscalls),
                static_cast<unsigned long long>(syscall_stats.read_syscall_errors),
                static_cast<unsigned long long>(syscall_stats.write_syscall_errors));
    return 0;
}
