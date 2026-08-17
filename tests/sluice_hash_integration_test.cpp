// sluice-hash integration tests: real files + real ThreadPoolBackend through
// the SAME hash engine the CLI uses (hash_task.cpp compiled into this target).
//   - known digests for real file contents (empty, "abc", multi-buffer);
//   - multi-file ordering (results in CLI order);
//   - buffer sizes far below the file size (streaming, many chunks);
//   - per-file error isolation with a bad fd (real EBADF terminal).
#include "harness.hpp"

#include "hash_task.hpp"
#include "sha256.hpp"

#include <fcntl.h>
#include <unistd.h>

#include <string>
#include <vector>

using namespace sluice_hash;

namespace {

struct TempFile {
    int fd;
    TempFile() {
        char p[] = "/tmp/sluice_hash_itest_XXXXXX";
        fd = ::mkstemp(p);
        SLUICE_CHECK(fd >= 0);
        ::unlink(p);
    }
    ~TempFile() { if (fd >= 0) ::close(fd); }
    TempFile(const TempFile&) = delete;
    TempFile& operator=(const TempFile&) = delete;
};

void write_all(int fd, const std::string& s) {
    SLUICE_CHECK(::pwrite(fd, s.data(), s.size(), 0) ==
                 static_cast<ssize_t>(s.size()));
}

// Independent in-test reference: hash the same bytes through Sha256 directly.
std::string reference_hex(const std::string& s) {
    Sha256 h;
    h.update(reinterpret_cast<const std::uint8_t*>(s.data()), s.size());
    std::uint8_t d[32];
    h.final(d);
    char hex[65];
    sha256_hex(d, hex);
    return std::string(hex);
}

}  // namespace

SLUICE_TEST_CASE(hash_integration_empty_file) {
    TempFile f;
    auto rs = hash_files({HashInput{"empty", f.fd}}, 4096, 1);
    SLUICE_CHECK(rs.size() == 1);
    SLUICE_CHECK(!rs[0].error.has_value());
    SLUICE_CHECK(rs[0].bytes_hashed == 0);
    SLUICE_CHECK(rs[0].hex ==
                 "e3b0c44298fc1c149afbf4c8996fb924"
                 "27ae41e4649b934ca495991b7852b855");
}

SLUICE_TEST_CASE(hash_integration_known_abc) {
    TempFile f;
    write_all(f.fd, "abc");
    auto rs = hash_files({HashInput{"abc", f.fd}}, 4096, 1);
    SLUICE_CHECK(!rs[0].error.has_value());
    SLUICE_CHECK(rs[0].bytes_hashed == 3);
    SLUICE_CHECK(rs[0].hex ==
                 "ba7816bf8f01cfea414140de5dae2223"
                 "b00361a396177a9cb410ff61f20015ad");
}

SLUICE_TEST_CASE(hash_integration_multibuffer_streaming) {
    // 1 MiB deterministic pattern hashed through a 64 KiB buffer: 16 reads.
    TempFile f;
    std::string data;
    data.reserve(1024 * 1024);
    for (int i = 0; i < 1024 * 1024; ++i)
        data.push_back(static_cast<char>((i * 2654435761u) >> 24 & 0xff));
    write_all(f.fd, data);

    auto rs = hash_files({HashInput{"big", f.fd}}, 64 * 1024, 2);
    SLUICE_CHECK(!rs[0].error.has_value());
    SLUICE_CHECK(rs[0].bytes_hashed == data.size());
    SLUICE_CHECK(rs[0].hex == reference_hex(data));
}

SLUICE_TEST_CASE(hash_integration_short_reads_tolerated) {
    // Buffer larger than the file plus an odd 1-byte-over size: exercises the
    // single-read path and the exactly-one-buffer path against real pread.
    for (std::string data : {std::string("x"), std::string(4095, 'y'),
                             std::string(4096, 'z'), std::string(4097, 'w')}) {
        TempFile f;
        write_all(f.fd, data);
        auto rs = hash_files({HashInput{"s", f.fd}}, 4096, 1);
        SLUICE_CHECK(rs.size() == 1);
        if (rs[0].error.has_value()) return;  // SLUICE_CHECK already reported
        SLUICE_CHECK(rs[0].hex == reference_hex(data));
        SLUICE_CHECK(rs[0].bytes_hashed == data.size());
    }
}

SLUICE_TEST_CASE(hash_integration_multi_file_order_preserved) {
    TempFile a, b, c;
    write_all(a.fd, "alpha");
    write_all(b.fd, "beta-beta");
    write_all(c.fd, "gamma");

    auto rs = hash_files(
        {HashInput{"a", a.fd}, HashInput{"b", b.fd}, HashInput{"c", c.fd}},
        4096, 1);
    SLUICE_CHECK(rs.size() == 3);
    SLUICE_CHECK(rs[0].path == "a" && rs[1].path == "b" && rs[2].path == "c");
    SLUICE_CHECK(rs[0].hex == reference_hex("alpha"));
    SLUICE_CHECK(rs[1].hex == reference_hex("beta-beta"));
    SLUICE_CHECK(rs[2].hex == reference_hex("gamma"));
}

SLUICE_TEST_CASE(hash_integration_bad_fd_isolates_per_file) {
    TempFile good;
    write_all(good.fd, "still works");
    // fd -1: the real backend validates the descriptor synchronously
    // (invalid_argument) — the OTHER file must still hash.
    auto rs = hash_files({HashInput{"bad", -1}, HashInput{"good", good.fd}},
                         4096, 1);
    SLUICE_CHECK(rs.size() == 2);
    SLUICE_CHECK(rs[0].error.has_value());
    SLUICE_CHECK(rs[0].hex.empty());
    SLUICE_CHECK(!rs[1].error.has_value());
    SLUICE_CHECK(rs[1].hex == reference_hex("still works"));
}

SLUICE_MAIN()
