// sluice-hash deterministic fault tests: the SAME hash engine the CLI uses,
// driven against FakeAsyncBackend (deterministic completion/error injection).
//
// FakeAsyncBackend auto modes apply one shaped result to every outstanding
// op, so the terminating shapes are: auto_error (every read errors) and
// auto_eof (first read returns 0 => hash of empty input). Real short reads
// are covered by the integration tests (real files exercise them naturally).
#include "harness.hpp"

#include "hash_task.hpp"

#include <sluice/async/fake_backend.hpp>
#include <sluice/error.hpp>

#include <fcntl.h>
#include <unistd.h>

#include <vector>

using namespace sluice_hash;
using sluice::IoError;
using sluice::async::FakeAsyncBackend;

namespace {

struct TempFile {
    int fd;
    TempFile() {
        char p[] = "/tmp/sluice_hash_ftest_XXXXXX";
        fd = ::mkstemp(p);
        SLUICE_CHECK(fd >= 0);
        ::unlink(p);
    }
    ~TempFile() { if (fd >= 0) ::close(fd); }
    TempFile(const TempFile&) = delete;
    TempFile& operator=(const TempFile&) = delete;
};

constexpr std::size_t kBuf = 4096;  // >= kMinBufferSize

}  // namespace

SLUICE_TEST_CASE(hash_fault_read_error_propagates_per_file) {
    TempFile a, b;
    auto backend = std::make_unique<FakeAsyncBackend>();
    backend->auto_error(IoError{IoError::Code::backend_error});
    auto rs = hash_files_with_backend(
        {HashInput{"a", a.fd}, HashInput{"b", b.fd}}, kBuf, 1,
        std::move(backend));
    SLUICE_CHECK(rs.size() == 2);
    for (auto& r : rs) {
        SLUICE_CHECK(r.error.has_value());
        SLUICE_CHECK(r.error->code == IoError::Code::backend_error);
        SLUICE_CHECK(r.hex.empty());
        SLUICE_CHECK(r.bytes_hashed == 0);
    }
}

SLUICE_TEST_CASE(hash_fault_eof_hashes_empty_vector) {
    TempFile a;
    auto backend = std::make_unique<FakeAsyncBackend>();
    backend->auto_eof();
    auto rs = hash_files_with_backend({HashInput{"a", a.fd}}, kBuf, 1,
                                      std::move(backend));
    SLUICE_CHECK(rs.size() == 1);
    SLUICE_CHECK(!rs[0].error.has_value());
    SLUICE_CHECK(rs[0].bytes_hashed == 0);
    // SHA-256 of the empty message (NIST vector).
    SLUICE_CHECK(rs[0].hex ==
                 "e3b0c44298fc1c149afbf4c8996fb924"
                 "27ae41e4649b934ca495991b7852b855");
}

SLUICE_TEST_CASE(hash_fault_invalid_buffer_size_rejected_without_running) {
    TempFile a;
    auto backend = std::make_unique<FakeAsyncBackend>();
    // Below kMinBufferSize: synchronous per-file invalid_state; the backend
    // is never used (still owned at scope end proves nothing ran — and the
    // runtime was never built).
    auto rs = hash_files_with_backend({HashInput{"a", a.fd}}, 100, 1,
                                      std::move(backend));
    SLUICE_CHECK(rs.size() == 1);
    SLUICE_CHECK(rs[0].error.has_value());
    SLUICE_CHECK(rs[0].error->code == IoError::Code::invalid_state);

    auto rs2 = hash_files_with_backend({HashInput{"a", a.fd}},
                                       kMaxBufferSize * 2, 1,
                                       std::make_unique<FakeAsyncBackend>());
    SLUICE_CHECK(rs2[0].error.has_value());
    SLUICE_CHECK(rs2[0].error->code == IoError::Code::invalid_state);
}

SLUICE_TEST_CASE(hash_fault_invalid_workers_rejected) {
    TempFile a;
    auto rs = hash_files_with_backend({HashInput{"a", a.fd}}, kBuf, 0,
                                      std::make_unique<FakeAsyncBackend>());
    SLUICE_CHECK(rs[0].error.has_value());
    SLUICE_CHECK(rs[0].error->code == IoError::Code::invalid_state);

    auto rs2 = hash_files_with_backend({HashInput{"a", a.fd}}, kBuf,
                                       kMaxWorkers + 1,
                                       std::make_unique<FakeAsyncBackend>());
    SLUICE_CHECK(rs2[0].error.has_value());
    SLUICE_CHECK(rs2[0].error->code == IoError::Code::invalid_state);
}

SLUICE_MAIN()
