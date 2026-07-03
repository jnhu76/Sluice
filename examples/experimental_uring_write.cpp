// experimental_uring_write (CPPIO-CORE-013G). Writes a file via the experimental
// uring path if liburing is available; verifies bytes; prints stats. If
// unavailable, prints a clear unsupported message and exits success (skip-style).
// No performance claim.
#include <sluice/experimental/uring_io_context.hpp>
#include <sluice/measurement.hpp>

#include <cstdio>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <span>
#include <sstream>
#include <string>
#include <vector>

namespace {

struct TempPath {
    std::filesystem::path p;
    TempPath() {
        std::ostringstream oss;
        oss << "sluice_experimental_uring_" << std::hex << reinterpret_cast<std::uintptr_t>(this) << ".tmp";
        p = std::filesystem::temp_directory_path() / oss.str();
    }
    ~TempPath() {
        try { std::filesystem::remove(p); } catch (...) {}
    }
    std::string str() const { return p.string(); }
};

bool file_has(const std::string& path, std::string_view want) {
    std::ifstream in(path, std::ios::binary);
    std::string content{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
    return content == std::string(want);
}

}  // namespace

int main() {
    TempPath tp;
    std::string payload = "experimental io_uring write payload";
    std::vector<std::byte> buf(reinterpret_cast<const std::byte*>(payload.data()),
                               reinterpret_cast<const std::byte*>(payload.data() + payload.size()));

    sluice::experimental::UringIoContext ctx(8);
    sluice::UringStats st{};
    ctx.set_stats(&st);

    auto r = ctx.write_file_all(tp.str(), std::span<const std::byte>(buf));
    if (!r.has_value()) {
        if (r.error().code == sluice::IoError::Code::backend_error) {
            // Clean skip: liburing unavailable.
            std::printf("experimental_uring_write: SKIPPED (liburing unavailable)\n");
            return 0;
        }
        std::fprintf(stderr, "experimental_uring_write: failed: %s\n",
                     sluice::to_string(r.error().code).data());
        return 1;
    }

    if (!file_has(tp.str(), payload)) {
        std::fprintf(stderr, "experimental_uring_write: byte mismatch\n");
        return 1;
    }

    std::printf("experimental_uring_write: wrote %zu bytes via experimental uring path\n",
                payload.size());
    std::printf("  submitted_ops=%llu completed_ops=%llu bytes_completed=%llu errors=%llu\n",
                static_cast<unsigned long long>(st.submitted_ops),
                static_cast<unsigned long long>(st.completed_ops),
                static_cast<unsigned long long>(st.bytes_completed),
                static_cast<unsigned long long>(st.completion_errors));
    std::printf("  (experimental only; not a production backend; no durability claim)\n");
    return 0;
}
