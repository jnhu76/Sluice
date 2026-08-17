// sluice-grep deterministic fault tests: the SAME engine the CLI uses
// (grep_task.cpp + matcher.cpp compiled in) against FakeAsyncBackend.
#include "harness.hpp"

#include "grep_task.hpp"

#include <sluice/async/fake_backend.hpp>
#include <sluice/error.hpp>

#include <fcntl.h>
#include <unistd.h>

#include <string>
#include <vector>

using namespace sluice_grep;
using sluice::IoError;
using sluice::async::FakeAsyncBackend;

namespace {

struct TempFile {
    int fd;
    TempFile() {
        char p[] = "/tmp/sluice_grep_ftest_XXXXXX";
        fd = ::mkstemp(p);
        SLUICE_CHECK(fd >= 0);
        ::unlink(p);
    }
    ~TempFile() { if (fd >= 0) ::close(fd); }
    TempFile(const TempFile&) = delete;
    TempFile& operator=(const TempFile&) = delete;
};

constexpr std::size_t kBuf = 4096;

struct Capture {
    std::vector<std::string> lines;  // "path:line_no:line"
    void sink(const std::string& path, std::uint64_t line_no,
              std::string_view line) {
        lines.push_back(path + ":" + std::to_string(line_no) + ":" +
                        std::string(line));
    }
};

}  // namespace

SLUICE_TEST_CASE(grep_fault_read_error_propagates_per_file) {
    TempFile a, b;
    auto backend = std::make_unique<FakeAsyncBackend>();
    backend->auto_error(IoError{IoError::Code::backend_error});
    Capture cap;
    auto rs = grep_files_with_backend(
        "x", {GrepInput{"a", a.fd}, GrepInput{"b", b.fd}}, kBuf,
        kDefaultMaxLineBytes, 1,
        [&](const std::string& p, std::uint64_t n, std::string_view l) {
            cap.sink(p, n, l);
        },
        std::move(backend));
    SLUICE_CHECK(rs.size() == 2);
    for (auto& r : rs) {
        SLUICE_CHECK(r.error.has_value());
        SLUICE_CHECK(r.error->code == IoError::Code::backend_error);
        SLUICE_CHECK(r.match_count == 0);
    }
    SLUICE_CHECK(cap.lines.empty());
}

SLUICE_TEST_CASE(grep_fault_eof_empty_file_no_match) {
    TempFile a;
    auto backend = std::make_unique<FakeAsyncBackend>();
    backend->auto_eof();
    Capture cap;
    auto rs = grep_files_with_backend(
        "needle", {GrepInput{"a", a.fd}}, kBuf, kDefaultMaxLineBytes, 1,
        [&](const std::string& p, std::uint64_t n, std::string_view l) {
            cap.sink(p, n, l);
        },
        std::move(backend));
    SLUICE_CHECK(rs.size() == 1);
    SLUICE_CHECK(!rs[0].error.has_value());
    SLUICE_CHECK(rs[0].match_count == 0);
    SLUICE_CHECK(rs[0].lines_scanned == 0);  // no final line: empty carry
}

SLUICE_TEST_CASE(grep_fault_invalid_config_rejected_without_running) {
    TempFile a;
    auto rs = grep_files_with_backend(
        "x", {GrepInput{"a", a.fd}}, 100 /* < kMinBufferSize */,
        kDefaultMaxLineBytes, 1, nullptr,
        std::make_unique<FakeAsyncBackend>());
    SLUICE_CHECK(rs[0].error.has_value());
    SLUICE_CHECK(rs[0].error->code == IoError::Code::invalid_state);

    auto rs2 = grep_files_with_backend(
        "x", {GrepInput{"a", a.fd}}, kBuf, 0 /* zero line cap */, 1, nullptr,
        std::make_unique<FakeAsyncBackend>());
    SLUICE_CHECK(rs2[0].error->code == IoError::Code::invalid_state);

    auto rs3 = grep_files_with_backend(
        "x", {GrepInput{"a", a.fd}}, kBuf, kMaxMaxLineBytes + 1, 1, nullptr,
        std::make_unique<FakeAsyncBackend>());
    SLUICE_CHECK(rs3[0].error->code == IoError::Code::invalid_state);
}

SLUICE_MAIN()
