#include <sluice/blocking/file.hpp>
#include <sluice/detail/file_semantics.hpp>
#include <sluice/file_resource.hpp>

#include <cerrno>
#include <cstdio>

#include <fcntl.h>
#include <unistd.h>

namespace {

using sluice::File;
using sluice::FileAccess;
using sluice::FileOpen;
using sluice::IoError;
using sluice::detail::canceled_before_dispatch;
using sluice::detail::canceled_racing_in_flight_attempt;
using sluice::detail::EffectCertainty;
using sluice::detail::failed_dispatched_attempt;
using sluice::detail::IoOutcome;
using sluice::detail::prefix_only_terminal_can_carry;

constexpr IoError kEio{.code = IoError::Code::backend_error, .os_errno = EIO};
constexpr IoError kNoSpace{.code = IoError::Code::no_space, .os_errno = 28};

bool case_a_zero_confirmed_error_no_effect() {
    const IoOutcome outcome = IoOutcome::failure(kEio);
    return !outcome.succeeded && outcome.effect.confirmed_bytes == 0 &&
           outcome.effect.remaining == EffectCertainty::accounted &&
           outcome.error.code == IoError::Code::backend_error;
}

bool case_b_confirmed_prefix_survives_an_error() {
    const IoOutcome outcome = IoOutcome::failure(kNoSpace, 7);
    return !outcome.succeeded && outcome.effect.confirmed_bytes == 7 &&
           outcome.effect.remaining == EffectCertainty::accounted;
}

bool case_c_canceled_keeps_confirmed_bytes() {
    const IoOutcome before_dispatch = canceled_before_dispatch(9);
    if (!(before_dispatch.is_canceled() && before_dispatch.effect.confirmed_bytes == 9 &&
          before_dispatch.effect.remaining == EffectCertainty::accounted))
        return false;
    const IoOutcome raced = canceled_racing_in_flight_attempt(9);
    return raced.is_canceled() && raced.effect.confirmed_bytes == 9 &&
           raced.effect.remaining == EffectCertainty::unknown;
}

bool case_d_unknown_remaining_effect() {
    const IoOutcome outcome = IoOutcome::uncertain(kEio, 3);
    return !outcome.succeeded && outcome.effect.confirmed_bytes == 3 &&
           outcome.effect.remaining == EffectCertainty::unknown;
}

bool pre_execution_cancel_reports_known_zero() {
    const IoOutcome outcome = canceled_before_dispatch();
    return outcome.is_canceled() && outcome.effect.confirmed_bytes == 0 &&
           outcome.effect.remaining == EffectCertainty::accounted;
}

bool success_is_exact_progress() {
    const IoOutcome data = IoOutcome::success(5);
    const IoOutcome scalar = IoOutcome::success();
    return data.succeeded && data.effect.confirmed_bytes == 5 &&
           data.effect.remaining == EffectCertainty::accounted && scalar.succeeded &&
           scalar.effect.confirmed_bytes == 0;
}

bool failed_dispatched_attempt_is_unknown_in_both_directions() {
    const IoOutcome write_failure = failed_dispatched_attempt(kEio, 2);
    if (write_failure.effect.remaining != EffectCertainty::unknown)
        return false;
    if (write_failure.effect.confirmed_bytes != 2)
        return false;
    const IoOutcome read_failure = failed_dispatched_attempt(kEio, 2);
    return read_failure.effect.remaining == EffectCertainty::unknown &&
           read_failure.effect.confirmed_bytes == 2;
}

bool prefix_only_terminal_cannot_carry_v15() {
    if (!prefix_only_terminal_can_carry(IoOutcome::success(4)))
        return false;
    if (!prefix_only_terminal_can_carry(IoOutcome::failure(kEio, 4)))
        return false;
    if (!prefix_only_terminal_can_carry(canceled_before_dispatch(4)))
        return false;
    if (prefix_only_terminal_can_carry(canceled_racing_in_flight_attempt(4)))
        return false;
    return !prefix_only_terminal_can_carry(IoOutcome::uncertain(kEio, 0));
}

bool real_write_failure_reports_unknown_effect(int* attempted) {
    *attempted = 0;
    const int fd = ::open("/dev/full", O_WRONLY);
    if (fd < 0)
        return true;
    *attempted = 1;
    ::close(fd);

    FileOpen mode;
    mode.access = FileAccess::write_only;
    mode.existence = sluice::FileExistence::open_existing;
    auto opened = File::open("/dev/full", mode);
    if (!opened.has_value())
        return false;
    File file = std::move(opened.value());

    const char payload[] = "partial effect";
    const auto src = std::span<const std::byte>(
        reinterpret_cast<const std::byte*>(payload), sizeof(payload) - 1);
    auto written = sluice::blocking::write_at(file, 0, src);
    if (written.has_value())
        return false;

    const IoOutcome outcome = failed_dispatched_attempt(written.error());
    if (outcome.effect.remaining != EffectCertainty::unknown)
        return false;
    if (prefix_only_terminal_can_carry(outcome))
        return false;
    return outcome.error.os_errno == ENOSPC && outcome.error.code == IoError::Code::no_space;
}

}

int main() {
    struct NamedTest {
        const char* name;
        bool (*fn)();
    };
    const NamedTest tests[] = {
        {"case_a_zero_confirmed_error_no_effect", case_a_zero_confirmed_error_no_effect},
        {"case_b_confirmed_prefix_survives_an_error", case_b_confirmed_prefix_survives_an_error},
        {"case_c_canceled_keeps_confirmed_bytes", case_c_canceled_keeps_confirmed_bytes},
        {"case_d_unknown_remaining_effect", case_d_unknown_remaining_effect},
        {"pre_execution_cancel_reports_known_zero", pre_execution_cancel_reports_known_zero},
        {"success_is_exact_progress", success_is_exact_progress},
        {"failed_dispatched_attempt_is_unknown_in_both_directions",
         failed_dispatched_attempt_is_unknown_in_both_directions},
        {"prefix_only_terminal_cannot_carry_v15", prefix_only_terminal_cannot_carry_v15},
    };

    for (const NamedTest& t : tests) {
        if (!t.fn()) {
            std::fprintf(stderr, "FAIL: %s\n", t.name);
            return 1;
        }
    }
    int attempted = 0;
    if (!real_write_failure_reports_unknown_effect(&attempted)) {
        std::fprintf(stderr, "FAIL: real_write_failure_reports_unknown_effect\n");
        return 1;
    }
    if (attempted == 0) {
        std::printf("NOT RUN: real_write_failure_reports_unknown_effect (/dev/full absent)\n");
    }

    std::printf("all %zu effect outcome tests passed; %d physical failure case ran\n",
                sizeof(tests) / sizeof(tests[0]), attempted);
    return 0;
}
