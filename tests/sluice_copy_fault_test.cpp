// sluice-copy deterministic fault tests (M1-A, brief §25).
//
// Drives the SAME copy_task code as the CLI/integration tests against a
// FakeAsyncBackend (deterministic completion/error injection).
//
// Determinism + no-sleep note: FakeAsyncBackend auto modes apply ONE shaped
// result to every outstanding op. The copy loop terminates on a zero-byte
// read (EOF); auto_bytes(n>0) never returns 0, so a success-path test against
// the fake would run forever. Therefore the fault tests here use shapes that
// terminate deterministically:
//   - auto_error(...)        -> every op completes with that error; the read
//                               path surfaces the error and stops immediately.
//   - auto_eof() (bytes 0)   -> the first read returns 0 => immediate EOF;
//                               the copy reports 0 bytes copied and runs the
//                               optional sync.
// The partial-read / partial-write / multiple-short-write / zero-write-progress
// LOOP paths are covered by the real-file integration tests (ThreadPoolBackend
// exercises short reads on real files and the write loop consumes them), so
// they are not duplicated here under a shape that would not terminate.
//
// copy_task.cpp is compiled into this target alongside the test.
#include "harness.hpp"

#include "copy_task.hpp"

#include <sluice/async/fake_backend.hpp>
#include <sluice/error.hpp>

#include <cstddef>
#include <fcntl.h>
#include <unistd.h>

using namespace sluice_copy;
using sluice::async::FakeAsyncBackend;
using sluice::IoError;

namespace {

struct TempFile {
    int fd;
    TempFile() {
        char p[] = "/tmp/sluice_copy_ftest_XXXXXX";
        fd = ::mkstemp(p);
        SLUICE_CHECK(fd >= 0);
        ::unlink(p);
    }
    ~TempFile() { if (fd >= 0) ::close(fd); }
    TempFile(const TempFile&) = delete;
    TempFile& operator=(const TempFile&) = delete;
};

}  // namespace

// Read completion error propagates as an error result and the copy stops
// immediately (no infinite retry).
SLUICE_TEST_CASE(sluice_copy_read_completion_error_propagates) {
    TempFile src; TempFile dst;
    auto backend = std::make_unique<FakeAsyncBackend>();
    backend->auto_error(IoError{IoError::Code::backend_error});
    auto r = run_sequential_copy_with_backend(src.fd, dst.fd, 16, 1,
                                              SyncPolicy::none,
                                              std::move(backend));
    SLUICE_CHECK(!r.has_value());
    SLUICE_CHECK(r.error().code == IoError::Code::backend_error);
}

// no_space (a write-side completion error class) propagates.
SLUICE_TEST_CASE(sluice_copy_no_space_completion_error_propagates) {
    TempFile src; TempFile dst;
    auto backend = std::make_unique<FakeAsyncBackend>();
    backend->auto_error(IoError{IoError::Code::no_space});
    auto r = run_sequential_copy_with_backend(src.fd, dst.fd, 16, 1,
                                              SyncPolicy::none,
                                              std::move(backend));
    SLUICE_CHECK(!r.has_value());
    SLUICE_CHECK(r.error().code == IoError::Code::no_space);
}

// Immediate EOF (zero-byte read) terminates the copy cleanly with 0 bytes and
// still runs the optional sync op. Proves the EOF guard works under the fake.
SLUICE_TEST_CASE(sluice_copy_eof_under_fake_terminates_clean) {
    TempFile src; TempFile dst;
    auto backend = std::make_unique<FakeAsyncBackend>();
    backend->auto_eof();  // every read returns 0 => immediate EOF
    auto r = run_sequential_copy_with_backend(src.fd, dst.fd, 16, 1,
                                              SyncPolicy::data,
                                              std::move(backend));
    SLUICE_CHECK(r.has_value());
    SLUICE_CHECK(r.value().bytes_copied == 0);
    SLUICE_CHECK(r.value().read_ops == 1);  // the single EOF read
    SLUICE_CHECK(r.value().write_ops == 0);
    SLUICE_CHECK(r.value().sync == SyncPolicy::data);
}

// Same EOF-terminates probe for sync=none and sync=all.
SLUICE_TEST_CASE(sluice_copy_eof_sync_none) {
    TempFile src; TempFile dst;
    auto backend = std::make_unique<FakeAsyncBackend>();
    backend->auto_eof();
    auto r = run_sequential_copy_with_backend(src.fd, dst.fd, 16, 1,
                                              SyncPolicy::none,
                                              std::move(backend));
    SLUICE_CHECK(r.has_value());
    SLUICE_CHECK(r.value().sync == SyncPolicy::none);
}

SLUICE_TEST_CASE(sluice_copy_eof_sync_all) {
    TempFile src; TempFile dst;
    auto backend = std::make_unique<FakeAsyncBackend>();
    backend->auto_eof();
    auto r = run_sequential_copy_with_backend(src.fd, dst.fd, 16, 1,
                                              SyncPolicy::all,
                                              std::move(backend));
    SLUICE_CHECK(r.has_value());
    SLUICE_CHECK(r.value().sync == SyncPolicy::all);
}

// Invalid arguments are rejected before any Runtime is built.
SLUICE_TEST_CASE(sluice_copy_rejects_zero_buffer) {
    TempFile src; TempFile dst;
    auto backend = std::make_unique<FakeAsyncBackend>();
    auto r = run_sequential_copy_with_backend(src.fd, dst.fd, 0, 1,
                                              SyncPolicy::none,
                                              std::move(backend));
    SLUICE_CHECK(!r.has_value());
    SLUICE_CHECK(r.error().code == IoError::Code::invalid_state);
}

SLUICE_TEST_CASE(sluice_copy_rejects_zero_workers) {
    TempFile src; TempFile dst;
    auto backend = std::make_unique<FakeAsyncBackend>();
    auto r = run_sequential_copy_with_backend(src.fd, dst.fd, 16, 0,
                                              SyncPolicy::none,
                                              std::move(backend));
    SLUICE_CHECK(!r.has_value());
    SLUICE_CHECK(r.error().code == IoError::Code::invalid_state);
}

SLUICE_TEST_CASE(sluice_copy_rejects_null_backend) {
    TempFile src; TempFile dst;
    auto r = run_sequential_copy_with_backend(src.fd, dst.fd, 16, 1,
                                              SyncPolicy::none, nullptr);
    SLUICE_CHECK(!r.has_value());
    SLUICE_CHECK(r.error().code == IoError::Code::invalid_state);
}

SLUICE_MAIN()
