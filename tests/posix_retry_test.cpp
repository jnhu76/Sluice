// Tests for detail::retry_on_eintr: EINTR is retried, real errors are not.
#include "harness.hpp"

#include <sluice/detail/posix_retry.hpp>

#include <cerrno>
#include <functional>

namespace {

// Callable that returns scripted results in sequence. Each entry is
// {return_value, errno_to_set}. errno is set BEFORE returning so the helper
// observes it.
class ScriptedSyscall {
public:
    struct Step { long ret; int err; };
    std::vector<Step> steps;
    mutable std::size_t calls = 0;

    long operator()() {
        if (calls >= steps.size()) {
            errno = EBADF;
            return -1;
        }
        Step s = steps[calls++];
        errno = s.err;
        return s.ret;
    }
};

}  // namespace

SLUICE_TEST_CASE(retry_on_eintr_retries_then_succeeds) {
    ScriptedSyscall sc;
    sc.steps = {{-1, EINTR}, {-1, EINTR}, {7, 0}};
    auto n = sluice::detail::retry_on_eintr(std::ref(sc));
    SLUICE_CHECK(n == 7);
    SLUICE_CHECK(sc.calls == 3);  // retried exactly twice
}

SLUICE_TEST_CASE(retry_on_eintr_returns_first_success) {
    ScriptedSyscall sc;
    sc.steps = {{42, 0}};
    auto n = sluice::detail::retry_on_eintr(std::ref(sc));
    SLUICE_CHECK(n == 42);
    SLUICE_CHECK(sc.calls == 1);
}

SLUICE_TEST_CASE(retry_on_eintr_does_not_retry_non_eintr_error) {
    ScriptedSyscall sc;
    sc.steps = {{-1, EBADF}};
    auto n = sluice::detail::retry_on_eintr(std::ref(sc));
    SLUICE_CHECK(n == -1);
    SLUICE_CHECK(sc.calls == 1);
    SLUICE_CHECK(errno == EBADF);
}

SLUICE_TEST_CASE(retry_on_eintr_returns_zero_unchanged) {
    // A legitimate 0-byte read (EOF) must not be mistaken for an error.
    ScriptedSyscall sc;
    sc.steps = {{0, 0}};
    auto n = sluice::detail::retry_on_eintr(std::ref(sc));
    SLUICE_CHECK(n == 0);
    SLUICE_CHECK(sc.calls == 1);
}

SLUICE_TEST_CASE(retry_on_eintr_stops_after_eintr_then_permanent_error) {
    // Multiple EINTR retries, then a permanent (non-EINTR) error: the helper
    // must stop at the permanent error and preserve its errno, not keep retrying.
    ScriptedSyscall sc;
    sc.steps = {{-1, EINTR}, {-1, EINTR}, {-1, EBADF}};
    auto n = sluice::detail::retry_on_eintr(std::ref(sc));
    SLUICE_CHECK(n == -1);
    SLUICE_CHECK(sc.calls == 3);
    SLUICE_CHECK(errno == EBADF);
}

SLUICE_MAIN()
