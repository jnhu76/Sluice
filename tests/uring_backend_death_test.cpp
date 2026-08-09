// Phase D1 UringAsyncBackend quiescent-destruction fail-fast death tests.
//
// POSIX-only (fork/exec/waitpid). Each child case re-execs this binary via
// death_test_runner_posix.hpp, installs a deterministic terminate handler, and
// either terminates with exit 86 (the destructor fail-fast fired) or exits 87
// (the destructor returned without terminating — the regression). The control
// case exits 0.
//
// Linkage: this target links the PRODUCTION sluice_async (the real
// UringAsyncBackend destructor). The destruction contract needs no injection
// hook, so this is a stronger proof than recompiling the backend source under
// SLUICE_URING_INTERNAL_TESTING: the fail-fast under test is the exact
// production destructor preflight that runs before io_uring_queue_exit().
//
// Fail-fast authority: the Completion is declared BEFORE the backend (and
// outside the backend's scope) so the backend destructor is the FIRST authority
// to run. If the Completion were declared inside the same scope, its destructor
// (reverse declaration order) would hit the outstanding-state
// completion_authority_fail_fast first and the backend destructor would never
// run — a false positive. The intended fail-fast authority for the non-quiescent
// case is uring_non_quiescent_destruction_fail_fast.
//
// Direct-backend destroy (per review): the child destroys the backend directly,
// NOT through AsyncIoContext. AsyncIoContext has its own destruction authority
// (it fail-fasts on non-zero outstanding); destroying the backend directly
// proves the exact contract under repair:
//
//     completion_ready, slot_in_use == 1, accepted_outstanding == 0
//         -> ~UringAsyncBackend
//         -> preflight detects non-quiescence (slot_in_use != 0)
//         -> terminate
//         -> io_uring_queue_exit MUST NOT run first
//
// Bounded waits: all child wait loops are bounded poll/yield loops with a
// deadline (never a blocking wait_one(), which has no timeout and could hang a
// child — and thus the blocking parent waitpid — forever if a terminal or wake
// were lost).
#include "harness.hpp"
#include "death_test_runner_posix.hpp"

#if defined(__unix__)

#include <sluice/async/completion.hpp>
#include <sluice/async/uring_backend.hpp>
#include <sluice/error.hpp>
#include <sluice/result.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <fcntl.h>
#include <filesystem>
#include <string>
#include <thread>
#include <unistd.h>

using namespace sluice::async;
using sluice::IoError;
using sluice::Result;

namespace {

constexpr auto kWaitTimeout = std::chrono::seconds(5);

class TempPath {
public:
    TempPath(const char* tag) {
        path_ = (std::filesystem::temp_directory_path() /
                 ("sluice_uring_death_" + std::string(tag) + "_" +
                  std::to_string(::getpid()) + "_" +
                  std::to_string(counter_++) + ".tmp"))
                    .string();
    }
    ~TempPath() {
        std::error_code ec;
        std::filesystem::remove(path_, ec);
    }
    TempPath(const TempPath&) = delete;
    TempPath& operator=(const TempPath&) = delete;
    const std::string& path() const { return path_; }

private:
    std::string path_;
    static inline unsigned counter_ = 0;
};

int open_temp(const std::string& path, bool truncate = true) {
    int flags = O_RDWR | O_CREAT;
    if (truncate)
        flags |= O_TRUNC;
    int fd = ::open(path.c_str(), flags, 0600);
    if (fd < 0) {
        std::perror("open");
        std::_Exit(sluice_death_test::kChildTestFailExit);
    }
    return fd;
}

// Seed a one-byte file so a 1-byte read completes with a real CQE.
void seed_one_byte(const std::string& path, std::byte seed) {
    int fd = open_temp(path);
    const unsigned char b = static_cast<unsigned char>(seed);
    if (::pwrite(fd, &b, 1, 0) != 1) {
        std::_Exit(sluice_death_test::kChildTestFailExit);
    }
    ::close(fd);
}

} // namespace

// 1) Destroy with a ready-but-not-reset Completion. Submit a 1-byte read, poll
// it to completion (the original operation CQE fires and reap publishes
// Completion-ready), then destroy the backend WITHOUT resetting the Completion.
// The slot stays bound (slot_in_use == 1), so the destructor preflight MUST
// fail-fast BEFORE io_uring_queue_exit(). This is the exact bug under repair:
// the pre-fix destructor ran io_uring_queue_exit() first and only the later
// arena member destructor caught slot_in_use != 0.
void child_destroy_with_completion_ready() {
    sluice_death_test::install_deterministic_terminate_handler();

    Completion<std::size_t> c; // declared BEFORE the backend -> backend dtor runs first
    {
        UringAsyncBackend backend(UringConfig{1, 4});
        if (!backend.available())
            std::_Exit(sluice_death_test::kChildTestFailExit);

        TempPath tp("completion_ready");
        seed_one_byte(tp.path(), std::byte{0x44});

        std::byte buf[1]{};
        int fd = open_temp(tp.path(), false);
        if (!backend.submit_read(ReadOp{fd, buf, 1, 0}, c).has_value())
            std::_Exit(sluice_death_test::kChildTestFailExit);

        // Bounded poll to ready (wait_one() has no timeout and could hang a
        // child forever if a terminal or wake were lost).
        const auto deadline = std::chrono::steady_clock::now() + kWaitTimeout;
        while (!c.ready()) {
            if (std::chrono::steady_clock::now() >= deadline)
                std::_Exit(sluice_death_test::kChildTestFailExit);
            if (backend.poll() == 0)
                std::this_thread::yield();
        }
        if (!c.ready())
            std::_Exit(sluice_death_test::kChildTestFailExit);
        if (buf[0] != std::byte{0x44})
            std::_Exit(sluice_death_test::kChildTestFailExit);
        // slot is bound until the caller resets/releases the ready Completion.
        if (backend.arena_slot_in_use() != 1)
            std::_Exit(sluice_death_test::kChildTestFailExit);

        // backend destroyed here while slot_in_use != 0 -> fail-fast.
        ::close(fd);
    }

    std::_Exit(sluice_death_test::kUnexpectedReturnExit);
}

// 2) Control: quiescent destroy. Submit, drive to ready, reset the Completion
// (releases the slot), then destroy the backend. The preflight must observe a
// quiescent arena and proceed to io_uring_queue_exit() cleanly -> exit 0.
void child_control_quiescent_destroy() {
    {
        UringAsyncBackend backend(UringConfig{1, 4});
        if (!backend.available())
            std::_Exit(sluice_death_test::kChildTestFailExit);

        TempPath tp("control");
        seed_one_byte(tp.path(), std::byte{0x55});

        std::byte buf[1]{};
        Completion<std::size_t> c;
        int fd = open_temp(tp.path(), false);
        if (!backend.submit_read(ReadOp{fd, buf, 1, 0}, c).has_value())
            std::_Exit(sluice_death_test::kChildTestFailExit);

        const auto deadline = std::chrono::steady_clock::now() + kWaitTimeout;
        while (backend.outstanding() > 0) {
            if (std::chrono::steady_clock::now() >= deadline)
                std::_Exit(sluice_death_test::kChildTestFailExit);
            if (backend.poll() == 0)
                std::this_thread::yield();
        }
        if (!c.ready())
            std::_Exit(sluice_death_test::kChildTestFailExit);
        if (buf[0] != std::byte{0x55})
            std::_Exit(sluice_death_test::kChildTestFailExit);
        c.reset(); // release the slot before destroy
        if (backend.arena_slot_in_use() != 0)
            std::_Exit(sluice_death_test::kChildTestFailExit);

        // backend destroyed here cleanly.
        ::close(fd);
    }

    std::_Exit(0);
}

SLUICE_TEST_CASE(uring_death_destroy_with_completion_ready) {
    auto r = sluice_death_test::run_death_case("completion-ready");
    SLUICE_CHECK_MSG(sluice_death_test::expect_terminated_via_fail_fast(r),
                     "destroying with completion-ready unreset must fail-fast before "
                     "io_uring_queue_exit (exit 86)");
}

SLUICE_TEST_CASE(uring_death_control_quiescent_destroy) {
    auto r = sluice_death_test::run_death_case("control");
    SLUICE_CHECK_MSG(sluice_death_test::expect_normal_exit_zero(r),
                     "quiescent destroy after drain + reset must exit 0");
}

int main(int argc, char** argv) {
    std::string child_case = sluice_death_test::parse_child_case(argc, argv);
    if (!child_case.empty()) {
        if (child_case == "completion-ready") {
            child_destroy_with_completion_ready();
        } else if (child_case == "control") {
            child_control_quiescent_destroy();
        } else {
            std::_Exit(sluice_death_test::kChildTestFailExit);
        }
        std::_Exit(sluice_death_test::kChildTestFailExit);
    }
    return sluice_test::run_all();
}

#else // !defined(__unix__)

SLUICE_TEST_CASE(uring_death_skip_non_posix) {
    // Death tests require POSIX fork/exec.
}

SLUICE_MAIN()

#endif // defined(__unix__)
