// Close-result contract tests for FileReader/FileWriter (ERR-001, issue
// #143): close(2) results must be observable, the fd consumed exactly once,
// EINTR never retried, and the destructor/move-assign stay best-effort.
//
// This binary compiles src/file.cpp directly WITH SLUICE_FILE_INTERNAL_
// TESTING so the CloseScript seam (src/file_test_seams.hpp) can inject
// deterministic close failures (EIO/EINTR) at the real syscall boundary —
// close(2) writeback errors are not otherwise reproducible on a healthy
// filesystem. It deliberately does NOT link sluice_core: the seam-enabled
// file.cpp would redefine the library's file.cpp symbols. file.cpp is
// self-contained (header-only core dependencies only).
#include "harness.hpp"

#include "file_test_seams.hpp" // src/ — on the SLUICE_FILE_INTERNAL_TESTING include path

#include <sluice/file.hpp>

#include <cerrno>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <utility>

namespace {

struct TempPath {
    std::filesystem::path p;
    TempPath() {
        std::ostringstream oss;
        oss << "sluice_close_" << std::hex << reinterpret_cast<std::uintptr_t>(this) << ".tmp";
        p = std::filesystem::temp_directory_path() / oss.str();
    }
    ~TempPath() {
        // Swallow cleanup errors: a failing remove must not throw during stack
        // unwinding and terminate the test process.
        try {
            std::filesystem::remove(p);
        } catch (...) {}
    }
    std::string str() const { return p.string(); }
};

bool file_has(const std::string& path, std::string_view want) {
    std::ifstream in(path, std::ios::binary);
    std::string content((std::istreambuf_iterator<char>(in)), {});
    return content == std::string(want);
}

} // namespace

// --- success path ------------------------------------------------------------

SLUICE_TEST_CASE(file_writer_close_success_consumes_fd_once) {
    TempPath tp;
    sluice::FileWriter w(tp.str());
    SLUICE_CHECK(w.opened());
    auto wr = w.write_some(std::as_bytes(std::span("persisted", 9)));
    SLUICE_CHECK(wr.has_value() && wr.value() == 9);

    auto r = w.close();
    SLUICE_CHECK(r.has_value());
    SLUICE_CHECK(!w.opened());

    // Idempotent: a second close and the destructor perform no further syscall.
    auto r2 = w.close();
    SLUICE_CHECK(r2.has_value());
    SLUICE_CHECK(file_has(tp.str(), "persisted"));
} // destructor runs with fd_ == -1: best-effort no-op

SLUICE_TEST_CASE(file_writer_destructor_best_effort_keeps_happy_path) {
    TempPath tp;
    {
        sluice::FileWriter w(tp.str());
        SLUICE_CHECK(w.opened());
        auto wr = w.write_some(std::as_bytes(std::span("dtor", 4)));
        SLUICE_CHECK(wr.has_value());
    } // destructor closes without caller involvement (unchanged behavior)
    SLUICE_CHECK(file_has(tp.str(), "dtor"));
}

SLUICE_TEST_CASE(file_writer_destructor_close_failure_discarded_survives) {
    // The destructor's best-effort path must route through the SAME close()
    // (exactly one syscall, fd consumed) and must neither throw nor fail-fast
    // when the close fails. Guards against a later "optimization" that
    // bypasses close() in the destructor or turns the discarded error into a
    // contract violation.
    TempPath tp;
    sluice::file_testing::CloseScript script({{.ret = -1, .err = EIO}});
    {
        sluice::FileWriter w(tp.str()); // destructor runs while seam armed
        SLUICE_CHECK(w.opened());
    }
    SLUICE_CHECK(script.calls() == 1); // exactly one close, result discarded
    // Process survived to here: the destructor did not throw or terminate.
}

SLUICE_TEST_CASE(file_reader_close_success_consumes_fd_once) {
    TempPath tp;
    {
        sluice::FileWriter w(tp.str());
        (void)w.write_some(std::as_bytes(std::span("r", 1)));
    }
    sluice::FileReader r(tp.str());
    SLUICE_CHECK(r.opened());
    auto c = r.close();
    SLUICE_CHECK(c.has_value());
    SLUICE_CHECK(!r.opened());
    auto c2 = r.close();
    SLUICE_CHECK(c2.has_value());
} // destructor no-op after explicit close

// --- injected close failures become observable --------------------------------

SLUICE_TEST_CASE(file_writer_close_eio_reported_verbatim) {
    TempPath tp;
    sluice::FileWriter w(tp.str());
    SLUICE_CHECK(w.opened());

    sluice::file_testing::CloseScript script({{.ret = -1, .err = EIO}});
    auto r = w.close();
    SLUICE_CHECK(!r.has_value());
    SLUICE_CHECK(r.error().code == sluice::IoError::Code::backend_error);
    SLUICE_CHECK(r.error().os_errno == EIO); // raw errno preserved verbatim
    SLUICE_CHECK(script.calls() == 1);       // exactly one close syscall

    // The fd is consumed DESPITE the error: no double close, no reuse hazard.
    SLUICE_CHECK(!w.opened());
    auto r2 = w.close();
    SLUICE_CHECK(r2.has_value());
    SLUICE_CHECK(script.calls() == 1); // the failed fd was never re-closed
}

SLUICE_TEST_CASE(file_writer_close_eintr_reported_never_retried) {
    TempPath tp;
    sluice::FileWriter w(tp.str());
    SLUICE_CHECK(w.opened());

    sluice::file_testing::CloseScript script({{.ret = -1, .err = EINTR}});
    auto r = w.close();
    SLUICE_CHECK(!r.has_value());
    SLUICE_CHECK(r.error().code == sluice::IoError::Code::interrupted);
    SLUICE_CHECK(r.error().os_errno == EINTR);
    // close is the retry_on_eintr EXCEPTION: a retried close could hit a
    // since-reused fd number, so exactly one call must have been made.
    SLUICE_CHECK(script.calls() == 1);
    SLUICE_CHECK(!w.opened());
}

SLUICE_TEST_CASE(file_reader_close_eio_observable) {
    TempPath tp;
    {
        sluice::FileWriter w(tp.str());
        (void)w.write_some(std::as_bytes(std::span("x", 1)));
    }
    sluice::FileReader r(tp.str());
    SLUICE_CHECK(r.opened());

    sluice::file_testing::CloseScript script({{.ret = -1, .err = EIO}});
    auto c = r.close();
    SLUICE_CHECK(!c.has_value());
    SLUICE_CHECK(c.error().os_errno == EIO);
    SLUICE_CHECK(script.calls() == 1);
    SLUICE_CHECK(!r.opened());
}

// --- channel independence / precedence ---------------------------------------

SLUICE_TEST_CASE(file_writer_sync_success_and_close_failure_are_independent) {
    TempPath tp;
    sluice::FileWriter w(tp.str());
    SLUICE_CHECK(w.opened());
    auto wr = w.write_some(std::as_bytes(std::span("data", 4)));
    SLUICE_CHECK(wr.has_value());

    // sync_all runs for real and succeeds; only close is scripted to fail.
    sluice::file_testing::CloseScript script({{.ret = -1, .err = EIO}});
    auto s = w.sync_all();
    SLUICE_CHECK(s.has_value()); // sync channel reports its own syscall only
    auto r = w.close();
    SLUICE_CHECK(!r.has_value());
    SLUICE_CHECK(r.error().os_errno == EIO); // close failure NOT swallowed
    // Durability asked for is not durability achieved: both observations are
    // separately visible to the caller, neither overwrites the other.
    SLUICE_CHECK(script.calls() == 1);
}

SLUICE_TEST_CASE(file_close_never_opened_is_noop_not_open_error) {
    sluice::FileWriter w("/no/such/sluice/close/path/xyz");
    SLUICE_CHECK(!w.opened());
    SLUICE_CHECK(w.open_error().has_value());
    // close() reports ONLY the close syscall: a preserved open() failure is
    // not re-reported here (it stays on open_error()/first-I/O).
    auto r = w.close();
    SLUICE_CHECK(r.has_value());

    sluice::FileReader r2("/no/such/sluice/close/path/xyz");
    SLUICE_CHECK(!r2.opened());
    auto c = r2.close();
    SLUICE_CHECK(c.has_value());
}

// --- move semantics ------------------------------------------------------------

SLUICE_TEST_CASE(file_writer_move_assign_closes_old_fd_once_result_discarded) {
    TempPath tp1;
    TempPath tp2;
    sluice::FileWriter w1(tp1.str());
    sluice::FileWriter w2(tp2.str());
    SLUICE_CHECK(w1.opened() && w2.opened());

    {
        // The old fd's close fails; operator= is noexcept with no channel,
        // so the result is discarded — but the fd is still closed exactly once.
        sluice::file_testing::CloseScript script({{.ret = -1, .err = EIO}});
        w1 = std::move(w2);
        SLUICE_CHECK(script.calls() == 1);
    } // script disarmed: subsequent closes are real

    SLUICE_CHECK(w1.opened());  // w1 now owns w2's (real) fd
    SLUICE_CHECK(!w2.opened()); // moved-from
    auto rc = w2.close();       // moved-from close: idempotent no-op
    SLUICE_CHECK(rc.has_value());

    auto r = w1.close(); // real close of the adopted fd
    SLUICE_CHECK(r.has_value());
    SLUICE_CHECK(!w1.opened());
}

SLUICE_MAIN()
