// Tests for experimental::UringWriteBatch (CPPIO-CORE-013C). When liburing is
// unavailable (SLUICE_HAS_LIBURING not defined), the test asserts the clean
// unsupported-stub behavior. When liburing IS available, it exercises a real
// small write to a temp file. Either way the suite stays green.
#include "harness.hpp"

#include <sluice/experimental/uring_write_batch.hpp>

#if defined(SLUICE_HAS_LIBURING)
#include <sluice/file.hpp>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <fcntl.h>
#include <unistd.h>
#endif

#include <cstddef>
#include <cstdio>
#include <span>
#include <vector>

#if !defined(SLUICE_HAS_LIBURING)
SLUICE_TEST_CASE(uring_write_batch_stub_returns_backend_error_without_liburing) {
    sluice::experimental::UringWriteBatch batch(8);
    std::vector<std::byte> buf(4, std::byte{0xAB});
    auto r = batch.write_all(/*fd=*/1, std::span<const std::byte>(buf), 0);
    // Without liburing the stub reports backend_error; this is a clean skip.
    SLUICE_CHECK(!r.has_value());
    SLUICE_CHECK(r.error().code == sluice::IoError::Code::backend_error);
}
#else
namespace {
struct TempPath {
    std::filesystem::path p;
    TempPath() {
        std::ostringstream oss;
        oss << "sluice_uring_" << std::hex << reinterpret_cast<std::uintptr_t>(this) << ".tmp";
        p = std::filesystem::temp_directory_path() / oss.str();
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

SLUICE_TEST_CASE(uring_write_batch_writes_small_file) {
    TempPath tp;
    // The batch takes a raw fd; open directly via POSIX for the spike.
    int fd = ::open(tp.str().c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    SLUICE_CHECK(fd >= 0);
    if (fd < 0) return;
    std::string payload = "hello uring";
    std::vector<std::byte> buf(payload.begin(), payload.end());
    sluice::experimental::UringWriteBatch batch(8);
    auto r = batch.write_all(fd, std::span<const std::byte>(buf), 0);
    ::close(fd);
    SLUICE_CHECK(r.has_value());
    if (r.has_value()) {
        SLUICE_CHECK(r.value().bytes_written == payload.size());
        SLUICE_CHECK(file_has(tp.str(), payload));
    }
}

SLUICE_TEST_CASE(uring_write_batch_invalid_fd_errors) {
    sluice::experimental::UringWriteBatch batch(8);
    std::vector<std::byte> buf(4, std::byte{0});
    auto r = batch.write_all(/*fd=*/-1, std::span<const std::byte>(buf), 0);
    SLUICE_CHECK(!r.has_value());
}
#endif

SLUICE_MAIN()
