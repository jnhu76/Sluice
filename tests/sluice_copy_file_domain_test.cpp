// sluice-copy file-domain tests.
//
// The Version B pipeline needs a seekable, finite-length source and a
// truncatable, positional destination — i.e. REGULAR files. This test drives
// the same open_copy_files() module the CLI uses (compiled into this target)
// and proves the damage-ordering contract:
//   - a non-regular source (directory, char device) is rejected BEFORE the
//     destination is created or touched;
//   - an existing destination is left byte-identical when the source is
//     rejected;
//   - a directory destination fails with the OS open error;
//   - source == destination (same path or hard link) is rejected by inode
//     identity, before any truncation.
//
// Note on FIFOs: open(src, O_RDONLY) itself blocks for a FIFO with no writer,
// BEFORE open_copy_files can fstat — the tests therefore use non-blocking
// sources (directories, char devices) and the comment/contract wording
// reflects that real ordering.
#include "harness.hpp"

#include "file_domain.hpp"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cstddef>
#include <cstring>
#include <string>
#include <vector>

using namespace sluice_copy;

namespace {

struct TempPath {
    std::string path;
    TempPath() {
        char p[] = "/tmp/sluice_fdtest_XXXXXX";
        int fd = ::mkstemp(p);
        SLUICE_CHECK(fd >= 0);
        ::close(fd);
        path = p;
    }
    ~TempPath() { ::unlink(path.c_str()); }
    TempPath(const TempPath&) = delete;
    TempPath& operator=(const TempPath&) = delete;
};

// Content of a file as bytes (empty vector when the file does not exist).
std::vector<std::byte> read_file(const std::string& p) {
    int fd = ::open(p.c_str(), O_RDONLY);
    if (fd < 0) return {};
    std::vector<std::byte> out;
    std::byte buf[4096];
    for (;;) {
        ssize_t n = ::read(fd, buf, sizeof(buf));
        if (n <= 0) break;
        out.insert(out.end(), buf, buf + n);
    }
    ::close(fd);
    return out;
}

bool file_exists(const std::string& p) {
    struct stat st{};
    return ::stat(p.c_str(), &st) == 0;
}

void seed_file(const std::string& p, std::size_t n, std::byte fill) {
    int fd = ::open(p.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    SLUICE_CHECK(fd >= 0);
    std::vector<std::byte> data(n, fill);
    if (n > 0) SLUICE_CHECK(::write(fd, data.data(), n) == static_cast<ssize_t>(n));
    ::close(fd);
}

}  // namespace

// ---------------------------------------------------------------------------
// Regular source + regular destination: success, both fds valid, and the
// destination is NOT truncated by open_copy_files (the caller truncates).
// ---------------------------------------------------------------------------
SLUICE_TEST_CASE(file_domain_regular_pair_succeeds) {
    TempPath src, dst;
    seed_file(src.path, 100, std::byte{0x5A});
    seed_file(dst.path, 4096, std::byte{0xFF});  // pre-existing, longer

    auto oc = open_copy_files(src.path, dst.path);
    SLUICE_CHECK(oc.failure == OpenCopyFailure::none);
    SLUICE_CHECK(oc.src_fd >= 0);
    SLUICE_CHECK(oc.dst_fd >= 0);

    // open_copy_files must NOT truncate: the pre-existing content survives
    // until the caller explicitly truncates.
    ::close(oc.src_fd);
    ::close(oc.dst_fd);
    auto content = read_file(dst.path);
    SLUICE_CHECK(content.size() == 4096);
    SLUICE_CHECK(content[0] == std::byte{0xFF});
}

// ---------------------------------------------------------------------------
// Directory source: rejected with src_not_regular, and the destination file
// is NEVER created (invalid source must not damage the destination).
// ---------------------------------------------------------------------------
SLUICE_TEST_CASE(file_domain_directory_source_rejected_without_dst_creation) {
    const char* dir = "/tmp";  // a directory, not a regular file
    // A unique destination path that does NOT exist yet (mkstemp for
    // uniqueness, unlink so we can assert open_copy_files never recreates it).
    char p[] = "/tmp/sluice_fdtest_XXXXXX";
    int fd = ::mkstemp(p);
    SLUICE_CHECK(fd >= 0);
    ::close(fd);
    ::unlink(p);
    std::string dst_path = p;

    auto oc = open_copy_files(dir, dst_path);
    SLUICE_CHECK(oc.failure == OpenCopyFailure::src_not_regular);
    SLUICE_CHECK(oc.src_fd < 0 && oc.dst_fd < 0);
    SLUICE_CHECK(!file_exists(dst_path));  // destination never created
}

// ---------------------------------------------------------------------------
// Directory source with an EXISTING destination: the destination content is
// left byte-identical (never truncated).
// ---------------------------------------------------------------------------
SLUICE_TEST_CASE(file_domain_directory_source_preserves_existing_dst) {
    const char* dir = "/tmp";
    TempPath dst;
    seed_file(dst.path, 4096, std::byte{0xAB});

    auto oc = open_copy_files(dir, dst.path);
    SLUICE_CHECK(oc.failure == OpenCopyFailure::src_not_regular);

    auto content = read_file(dst.path);
    SLUICE_CHECK(content.size() == 4096);
    for (std::byte b : content) SLUICE_CHECK(b == std::byte{0xAB});
}

// ---------------------------------------------------------------------------
// Char-device source (/dev/zero): rejected (platform-gated; skip cleanly when
// the device does not exist or is not a character device).
// ---------------------------------------------------------------------------
SLUICE_TEST_CASE(file_domain_char_device_source_rejected) {
    struct stat st{};
    if (::stat("/dev/zero", &st) != 0) {
        return;  // platform without /dev/zero: nothing to test
    }
    if (!S_ISCHR(st.st_mode)) {
        return;  // not a char device here: nothing to test
    }
    char p[] = "/tmp/sluice_fdtest_XXXXXX";
    int fd = ::mkstemp(p);
    SLUICE_CHECK(fd >= 0);
    ::close(fd);
    ::unlink(p);
    std::string dst_path = p;

    auto oc = open_copy_files("/dev/zero", dst_path);
    SLUICE_CHECK(oc.failure == OpenCopyFailure::src_not_regular);
    SLUICE_CHECK(!file_exists(dst_path));
}

// ---------------------------------------------------------------------------
// Directory destination: the open itself fails with the OS error (EISDIR on
// Linux/macOS) — "normal open failure" is the acceptable contract.
// ---------------------------------------------------------------------------
SLUICE_TEST_CASE(file_domain_directory_destination_fails_open) {
    TempPath src;
    seed_file(src.path, 64, std::byte{0x11});

    auto oc = open_copy_files(src.path, "/tmp");
    // The destination open fails (EISDIR); no truncation of anything occurs.
    SLUICE_CHECK(oc.failure == OpenCopyFailure::dst_open);
    SLUICE_CHECK(oc.error.os_errno != 0);
}

// ---------------------------------------------------------------------------
// Same path for source and destination: rejected by inode identity.
// ---------------------------------------------------------------------------
SLUICE_TEST_CASE(file_domain_same_path_rejected) {
    TempPath f;
    seed_file(f.path, 64, std::byte{0x22});

    auto oc = open_copy_files(f.path, f.path);
    SLUICE_CHECK(oc.failure == OpenCopyFailure::same_file);
    // The destination was never truncated (it IS the source).
    auto content = read_file(f.path);
    SLUICE_CHECK(content.size() == 64);
    SLUICE_CHECK(content[0] == std::byte{0x22});
}

// ---------------------------------------------------------------------------
// Hard link with a different path: still rejected (same device + inode).
// ---------------------------------------------------------------------------
SLUICE_TEST_CASE(file_domain_hard_link_rejected) {
    TempPath src;
    seed_file(src.path, 64, std::byte{0x33});
    TempPath dst;
    ::unlink(dst.path.c_str());
    SLUICE_CHECK(::link(src.path.c_str(), dst.path.c_str()) == 0);

    auto oc = open_copy_files(src.path, dst.path);
    SLUICE_CHECK(oc.failure == OpenCopyFailure::same_file);
    // The (linked) destination was not truncated.
    auto content = read_file(dst.path);
    SLUICE_CHECK(content.size() == 64);
    SLUICE_CHECK(content[0] == std::byte{0x33});
}

// ---------------------------------------------------------------------------
// Failure messages are explicit (CLI diagnostics).
// ---------------------------------------------------------------------------
SLUICE_TEST_CASE(file_domain_failure_messages) {
    SLUICE_CHECK(std::strcmp(open_copy_failure_message(
                                 OpenCopyFailure::src_not_regular),
                             "source is not a regular file") == 0);
    SLUICE_CHECK(std::strcmp(open_copy_failure_message(
                                 OpenCopyFailure::dst_not_regular),
                             "destination is not a regular file") == 0);
    SLUICE_CHECK(std::strcmp(open_copy_failure_message(
                                 OpenCopyFailure::same_file),
                             "source and destination refer to the same file") == 0);
}

SLUICE_MAIN()
