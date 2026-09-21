// ERR-02 partial-effect and progress reporting, as properties of the shared
// outcome representation.
//
// The four cases the root requires to be expressible are asserted directly, and
// the representation's own limitation is asserted too: a terminal shaped only as
// {is_error, error, confirmed_bytes} cannot carry an unaccounted remainder. That
// assertion is what records V15 as a production gap rather than a passing claim.
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

// Case A: no confirmed progress, an error, and no known effect.
bool case_a_zero_confirmed_error_no_effect() {
    const IoOutcome outcome = IoOutcome::failure(kEio);
    return !outcome.succeeded && outcome.effect.confirmed_bytes == 0 &&
           outcome.effect.remaining == EffectCertainty::accounted &&
           outcome.error.code == IoError::Code::backend_error;
}

// Case B: N confirmed bytes, an error, and a remainder proven unaffected.
bool case_b_confirmed_prefix_survives_an_error() {
    const IoOutcome outcome = IoOutcome::failure(kNoSpace, 7);
    return !outcome.succeeded && outcome.effect.confirmed_bytes == 7 &&
           outcome.effect.remaining == EffectCertainty::accounted;
}

// Case C: cancellation keeps the confirmed count, and the remainder follows the
// evidence. A cancel that won before dispatch proves the remainder unaffected;
// a cancel that raced an in-flight attempt proves only the count, so the
// remainder stays unknown. Neither collapses into an unqualified canceled
// result.
bool case_c_canceled_keeps_confirmed_bytes() {
    const IoOutcome before_dispatch = canceled_before_dispatch(9);
    if (!(before_dispatch.is_canceled() && before_dispatch.effect.confirmed_bytes == 9 &&
          before_dispatch.effect.remaining == EffectCertainty::accounted))
        return false;
    const IoOutcome raced = canceled_racing_in_flight_attempt(9);
    return raced.is_canceled() && raced.effect.confirmed_bytes == 9 &&
           raced.effect.remaining == EffectCertainty::unknown;
}

// Case D: an error whose remaining effect cannot be proven, reported as unknown
// rather than as zero bytes.
bool case_d_unknown_remaining_effect() {
    const IoOutcome outcome = IoOutcome::uncertain(kEio, 3);
    return !outcome.succeeded && outcome.effect.confirmed_bytes == 3 &&
           outcome.effect.remaining == EffectCertainty::unknown;
}

// A pre-execution cancel proved no dispatch and no effect, so it reports a
// known zero rather than an unknown remainder.
bool pre_execution_cancel_reports_known_zero() {
    const IoOutcome outcome = canceled_before_dispatch();
    return outcome.is_canceled() && outcome.effect.confirmed_bytes == 0 &&
           outcome.effect.remaining == EffectCertainty::accounted;
}

// A successful primitive count is exact progress with nothing left over.
bool success_is_exact_progress() {
    const IoOutcome data = IoOutcome::success(5);
    const IoOutcome scalar = IoOutcome::success();
    return data.succeeded && data.effect.confirmed_bytes == 5 &&
           data.effect.remaining == EffectCertainty::accounted && scalar.succeeded &&
           scalar.effect.confirmed_bytes == 0;
}

// The conversion rule is evidence-driven: a dispatched attempt that failed
// cannot exclude its effects whatever its direction. A failed read may have
// filled part of the caller's destination buffer (a LIFE-01 borrow), a failed
// write may have reached the file; neither supplies a trustworthy count for the
// possibly-effective portion.
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

// V15 stated against the representation rather than against any one path: the
// {is_error, error, confirmed_bytes} terminal shape cannot report an unaccounted
// remainder, so an unknown-effect failure is not representable in it.
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

// Deterministic physical write failure without waiting for a full disk: /dev/full
// rejects every write with ENOSPC. The library cannot know whether the attempt
// landed, so the outcome must report an unaccounted remainder instead of a
// zero-byte claim. Reported as NOT RUN when the device is absent.
//
// /dev/full is a character device, which PROD-02 excludes from the regular-file
// guarantees. It is used here only as a fault-injection device for the error and
// effect conversion path, not as evidence about regular-file behaviour.
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
    // The native detail survives the conversion.
    return outcome.error.os_errno == ENOSPC && outcome.error.code == IoError::Code::no_space;
}

} // namespace

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
