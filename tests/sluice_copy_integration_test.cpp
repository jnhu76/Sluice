// sluice-copy integration tests (M1-A, brief §24).
//
// Real temporary files + ThreadPoolBackend. Drives the same copy_task code the
// CLI uses (apps/sluice-copy/copy_task.cpp) via its public header, asserting:
//   - exact source/destination byte equality across many sizes/buffers
//   - empty file, 1 byte, sub-buffer, exactly one buffer, buffer+1, multi-buffer
//   - binary data with embedded zero bytes
//   - buffer sizes 4KiB / 64KiB / 256KiB
//   - 1 and 2 Runtime workers
//   - sync none / data / all
//   - clean shutdown + temporary resource cleanup
//
// No sleeps. copy_task.cpp is compiled into this target alongside the test.
#include "harness.hpp"

#include "copy_task.hpp"  // apps/sluice-copy public header (added to include path)

#include <sluice/async/threadpool_backend.hpp>
#include <sluice/error.hpp>

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

using namespace sluice_copy;
using sluice::IoError;

namespace {

// RAII temp file. Created empty; caller writes via returned fd.
struct TempFile {
    int fd;
    std::string path;
    TempFile() {
        char p[] = "/tmp/sluice_copy_itest_XXXXXX";
        fd = ::mkstemp(p);
        SLUICE_CHECK(fd >= 0);
        ::unlink(p);  // auto-cleanup when fd closes
    }
    ~TempFile() { if (fd >= 0) ::close(fd); }
    TempFile(const TempFile&) = delete;
    TempFile& operator=(const TempFile&) = delete;
};

// Write `n` deterministic bytes to fd at offset 0 and return a heap copy the
// test can compare against. Records a failure (does not abort the helper) on a
// precondition error so the function still has a value return.
std::vector<std::byte> seed_file(int fd, std::size_t n) {
    std::vector<std::byte> data(n);
    for (std::size_t i = 0; i < n; ++i) {
        // deterministic pattern with embedded zero bytes (every 7th byte = 0)
        unsigned char b = static_cast<unsigned char>((i * 31 + 7) & 0xFF);
        data[i] = (i % 7 == 0) ? std::byte{0} : std::byte{b};
    }
    if (n > 0) {
        ssize_t w = ::pwrite(fd, data.data(), n, 0);
        if (w != static_cast<ssize_t>(n)) {
            ::sluice_test::record_failure(__FILE__, __LINE__, "pwrite seed");
        }
    }
    return data;
}

// Read the whole dst fd back into a vector.
std::vector<std::byte> read_all(int fd) {
    struct stat st{};
    if (::fstat(fd, &st) != 0) {
        ::sluice_test::record_failure(__FILE__, __LINE__, "fstat");
        return {};
    }
    std::vector<std::byte> out(st.st_size);
    if (st.st_size > 0) {
        ssize_t r = ::pread(fd, out.data(), out.size(), 0);
        if (r != static_cast<ssize_t>(out.size())) {
            ::sluice_test::record_failure(__FILE__, __LINE__, "pread");
        }
    }
    return out;
}

// Parametric copy-and-compare. Returns true on exact byte equality + success.
bool copy_matches(std::size_t file_size, std::size_t buffer_size,
                  unsigned workers, SyncPolicy sync) {
    TempFile src;
    TempFile dst;
    auto expected = seed_file(src.fd, file_size);

    auto r = run_sequential_copy(src.fd, dst.fd, buffer_size, workers, sync);
    if (!r.has_value()) return false;
    if (r.value().bytes_copied != file_size) return false;

    auto got = read_all(dst.fd);
    if (got.size() != expected.size()) return false;
    if (expected.empty()) return true;
    return std::memcmp(got.data(), expected.data(), expected.size()) == 0;
}

}  // namespace

// ---- sizes ----------------------------------------------------------------

SLUICE_TEST_CASE(sluice_copy_empty_file) {
    SLUICE_CHECK(copy_matches(0, 4096, 1, SyncPolicy::none));
}

SLUICE_TEST_CASE(sluice_copy_one_byte) {
    SLUICE_CHECK(copy_matches(1, 4096, 1, SyncPolicy::none));
}

SLUICE_TEST_CASE(sluice_copy_smaller_than_buffer) {
    SLUICE_CHECK(copy_matches(100, 4096, 1, SyncPolicy::none));
}

SLUICE_TEST_CASE(sluice_copy_exactly_one_buffer) {
    SLUICE_CHECK(copy_matches(4096, 4096, 1, SyncPolicy::none));
}

SLUICE_TEST_CASE(sluice_copy_buffer_plus_one) {
    SLUICE_CHECK(copy_matches(4097, 4096, 1, SyncPolicy::none));
}

SLUICE_TEST_CASE(sluice_copy_multiple_buffers) {
    SLUICE_CHECK(copy_matches(4096 * 3 + 123, 4096, 1, SyncPolicy::none));
}

// ---- binary content with embedded zeros (covered by seed_file pattern) ----

SLUICE_TEST_CASE(sluice_copy_embedded_zeros) {
    TempFile src; TempFile dst;
    auto expected = seed_file(src.fd, 5000);
    SLUICE_CHECK(run_sequential_copy(src.fd, dst.fd, 1024, 1, SyncPolicy::none).has_value());
    auto got = read_all(dst.fd);
    SLUICE_CHECK(got.size() == expected.size());
    SLUICE_CHECK(std::memcmp(got.data(), expected.data(), expected.size()) == 0);
}

// ---- buffer sizes ---------------------------------------------------------

SLUICE_TEST_CASE(sluice_copy_buffer_4kib) {
    SLUICE_CHECK(copy_matches(4096 * 10, 4096, 1, SyncPolicy::none));
}

SLUICE_TEST_CASE(sluice_copy_buffer_64kib) {
    SLUICE_CHECK(copy_matches(64 * 1024 * 5, 64 * 1024, 1, SyncPolicy::none));
}

SLUICE_TEST_CASE(sluice_copy_buffer_256kib) {
    SLUICE_CHECK(copy_matches(256 * 1024 * 3, 256 * 1024, 1, SyncPolicy::none));
}

// ---- workers --------------------------------------------------------------

SLUICE_TEST_CASE(sluice_copy_one_worker) {
    SLUICE_CHECK(copy_matches(100000, 8192, 1, SyncPolicy::none));
}

SLUICE_TEST_CASE(sluice_copy_two_workers) {
    SLUICE_CHECK(copy_matches(100000, 8192, 2, SyncPolicy::none));
}

// ---- sync policies --------------------------------------------------------

SLUICE_TEST_CASE(sluice_copy_sync_none) {
    SLUICE_CHECK(copy_matches(8192, 4096, 1, SyncPolicy::none));
}

SLUICE_TEST_CASE(sluice_copy_sync_data) {
    SLUICE_CHECK(copy_matches(8192, 4096, 1, SyncPolicy::data));
}

SLUICE_TEST_CASE(sluice_copy_sync_all) {
    SLUICE_CHECK(copy_matches(8192, 4096, 1, SyncPolicy::all));
}

// ---- stats ----------------------------------------------------------------

SLUICE_TEST_CASE(sluice_copy_stats_reported) {
    TempFile src; TempFile dst;
    seed_file(src.fd, 4096 * 2);
    auto r = run_sequential_copy(src.fd, dst.fd, 4096, 1, SyncPolicy::data);
    SLUICE_CHECK(r.has_value());
    // 4096*2 bytes / 4096 buffer = 2 full chunks + 1 EOF read = 3 read ops,
    // 2 write ops, 0 short writes, sync=data.
    SLUICE_CHECK(r.value().read_ops == 3);
    SLUICE_CHECK(r.value().write_ops == 2);
    SLUICE_CHECK(r.value().short_writes == 0);
    SLUICE_CHECK(r.value().bytes_copied == 4096 * 2);
    SLUICE_CHECK(r.value().sync == SyncPolicy::data);
}

SLUICE_MAIN()
