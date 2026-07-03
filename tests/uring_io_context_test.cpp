// Tests for experimental::UringIoContext (CPPIO-CORE-013D). Skip-cleanly when
// liburing is unavailable; exercise a real write_file_all when it is.
#include "harness.hpp"

#include <cppio/experimental/uring_io_context.hpp>

#if defined(CPPIO_HAS_LIBURING)
#include <filesystem>
#include <fstream>
#endif

#include <cstddef>
#include <span>
#include <string>
#include <vector>

#if !defined(CPPIO_HAS_LIBURING)
CPPIO_TEST_CASE(uring_io_context_stub_returns_backend_error_without_liburing) {
    cppio::experimental::UringIoContext ctx(8);
    std::vector<std::byte> buf(4, std::byte{0xAB});
    auto r = ctx.write_file_all("/tmp/cppio_uring_stub.tmp", std::span<const std::byte>(buf));
    CPPIO_CHECK(!r.has_value());
    CPPIO_CHECK(r.error().code == cppio::IoError::Code::backend_error);
}
#else
namespace {
struct TempPath {
    std::filesystem::path p;
    TempPath() {
        p = std::filesystem::temp_directory_path() /
            ("cppio_uring_ioctx_" + std::to_string(reinterpret_cast<std::uintptr_t>(this)) + ".tmp");
    }
    ~TempPath() {
        try { std::filesystem::remove(p); } catch (...) {}
    }
    std::string str() const { return p.string(); }
};
bool file_has(const std::string& path, std::string_view want) {
    std::ifstream in(path, std::ios::binary);
    std::string content((std::istreambuf_iterator<char>(in), {}));
    return content == std::string(want);
}
}  // namespace

CPPIO_TEST_CASE(uring_io_context_write_file_all_writes_bytes) {
    TempPath tp;
    std::string payload = "uring io context payload";
    std::vector<std::byte> buf(payload.begin(), payload.end());
    cppio::experimental::UringIoContext ctx(8);
    auto r = ctx.write_file_all(tp.str(), std::span<const std::byte>(buf));
    CPPIO_CHECK(r.has_value());
    if (r.has_value()) {
        CPPIO_CHECK(r.value().bytes_written == payload.size());
        CPPIO_CHECK(file_has(tp.str(), payload));
    }
}

CPPIO_TEST_CASE(uring_io_context_invalid_path_errors) {
    cppio::experimental::UringIoContext ctx(8);
    std::vector<std::byte> buf(4, std::byte{0});
    auto r = ctx.write_file_all("/no/such/cppio/uring/dir/f", std::span<const std::byte>(buf));
    CPPIO_CHECK(!r.has_value());  // open fails -> propagated
}
#endif

CPPIO_MAIN()
