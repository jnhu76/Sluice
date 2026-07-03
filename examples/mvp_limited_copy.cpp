// mvp_limited_copy: copy only the first N bytes using CopyLimit::bytes(N).
// Verifies output size and contents; prints the stop reason from CopyStats.
#include <sluice/buffer.hpp>
#include <sluice/copy.hpp>
#include <sluice/file.hpp>
#include <sluice/limit.hpp>
#include <sluice/measurement.hpp>

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <span>
#include <sstream>
#include <string>
#include <vector>

namespace {

struct TempPath {
    std::filesystem::path p;
    TempPath(const char* tag) {
        std::ostringstream oss;
        oss << "sluice_mvp_lim_" << tag << "_" << std::hex << reinterpret_cast<std::uintptr_t>(this) << ".tmp";
        p = std::filesystem::temp_directory_path() / oss.str();
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

const char* stop_reason(const sluice::CopyStats& s) {
    if (s.eof_stops) return "eof";
    if (s.limit_stops) return "limit";
    if (s.reader_error_stops) return "reader_error";
    if (s.writer_error_stops) return "writer_error";
    return "none";
}

}  // namespace

int main() {
    constexpr std::size_t kLimit = 100;
    TempPath in_tp("in"), out_tp("out");
    // Source is larger than the limit so the stop reason must be "limit".
    const std::string payload(2048, 'Q');

    {
        std::ofstream o(in_tp.str(), std::ios::binary);
        o.write(payload.data(), static_cast<std::streamsize>(payload.size()));
    }

    sluice::BufferStats buffer_stats{};
    sluice::CopyStats copy_stats{};
    std::vector<std::byte> read_buf(64);
    std::vector<std::byte> write_buf(64);
    std::vector<std::byte> scratch(256);

    {
        sluice::FileReader file_in(in_tp.str());
        sluice::BufferedReader buffered_in(file_in, read_buf, &buffer_stats);
        sluice::FileWriter file_out(out_tp.str());
        sluice::BufferedWriter buffered_out(file_out, write_buf, &buffer_stats);

        auto res = sluice::copy_all(buffered_in, buffered_out,
                                   std::span<std::byte>(scratch),
                                   sluice::CopyLimit::bytes(kLimit), &copy_stats);
        if (!res.has_value()) {
            std::fprintf(stderr, "copy failed: %s\n",
                         sluice::to_string(res.error().code).data());
            return 1;
        }
        if (res.value() != kLimit) {
            std::fprintf(stderr, "copied %llu, want %zu\n",
                         static_cast<unsigned long long>(res.value()), kLimit);
            return 1;
        }
        auto flush_res = buffered_out.flush();
        if (!flush_res.has_value()) {
            std::fprintf(stderr, "flush failed\n");
            return 1;
        }
    }

    std::string got;
    if (!file_read_all(out_tp.str(), got) || got.size() != kLimit ||
        std::memcmp(got.data(), payload.data(), kLimit) != 0) {
        std::fprintf(stderr, "output content/size mismatch\n");
        return 1;
    }

    std::printf("mvp_limited_copy: copied %zu bytes (limit=%zu), stop=%s\n",
                got.size(), kLimit, stop_reason(copy_stats));
    std::printf("  copy_loop_iterations=%llu limit_stops=%llu eof_stops=%llu\n",
                static_cast<unsigned long long>(copy_stats.copy_loop_iterations),
                static_cast<unsigned long long>(copy_stats.limit_stops),
                static_cast<unsigned long long>(copy_stats.eof_stops));
    return 0;
}
