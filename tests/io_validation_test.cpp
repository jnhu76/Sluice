// Boundary tests for the small internal conversions shared by POSIX and
// io_uring backends. The tests operate on integer values only: they never
// allocate UINT_MAX-sized buffers or issue huge I/O requests.
#include "harness.hpp"

#include <sluice/detail/io_validation.hpp>

#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <sys/types.h>

namespace {

template <std::size_t N>
class ResultSequence {
  public:
    explicit ResultSequence(std::array<int, N> values) : values_(values) {}

    int operator()() {
        const std::size_t index = calls_ < N ? calls_ : N - 1;
        ++calls_;
        return values_[index];
    }

    std::size_t calls() const noexcept { return calls_; }

  private:
    std::array<int, N> values_;
    std::size_t calls_ = 0;
};

} // namespace

SLUICE_TEST_CASE(uring_length_accepts_representable_boundaries) {
    auto zero = sluice::detail::checked_uring_length(0);
    auto ordinary = sluice::detail::checked_uring_length(4096);
    auto maximum = sluice::detail::checked_uring_length(
        static_cast<std::size_t>(std::numeric_limits<unsigned>::max()));

    SLUICE_CHECK(zero.has_value() && zero.value() == 0);
    SLUICE_CHECK(ordinary.has_value() && ordinary.value() == 4096);
    SLUICE_CHECK(maximum.has_value());
    SLUICE_CHECK(maximum.value() == std::numeric_limits<unsigned>::max());
}

SLUICE_TEST_CASE(uring_length_rejects_one_past_unsigned_max_when_representable) {
    if constexpr (std::numeric_limits<std::size_t>::max() >
                  std::numeric_limits<unsigned>::max()) {
        const std::size_t too_large =
            static_cast<std::size_t>(std::numeric_limits<unsigned>::max()) + 1;
        auto result = sluice::detail::checked_uring_length(too_large);
        SLUICE_CHECK(!result.has_value());
        SLUICE_CHECK(result.error().code == sluice::IoError::Code::invalid_state);
    }
}

SLUICE_TEST_CASE(uring_chunk_length_caps_without_truncating) {
    SLUICE_CHECK(sluice::detail::uring_chunk_length(0) == 0);
    SLUICE_CHECK(sluice::detail::uring_chunk_length(4096) == 4096);
    SLUICE_CHECK(sluice::detail::uring_chunk_length(
                     static_cast<std::size_t>(std::numeric_limits<unsigned>::max())) ==
                 std::numeric_limits<unsigned>::max());
    if constexpr (std::numeric_limits<std::size_t>::max() >
                  std::numeric_limits<unsigned>::max()) {
        const std::size_t too_large =
            static_cast<std::size_t>(std::numeric_limits<unsigned>::max()) + 1;
        SLUICE_CHECK(sluice::detail::uring_chunk_length(too_large) ==
                     std::numeric_limits<unsigned>::max());
    }
}

SLUICE_TEST_CASE(posix_offset_accepts_zero_ordinary_and_native_max) {
    constexpr auto native_max = std::numeric_limits<off_t>::max();
    auto zero = sluice::detail::checked_posix_offset(0);
    auto ordinary = sluice::detail::checked_posix_offset(4096);
    auto maximum =
        sluice::detail::checked_posix_offset(static_cast<std::uint64_t>(native_max));

    SLUICE_CHECK(zero.has_value() && zero.value() == 0);
    SLUICE_CHECK(ordinary.has_value() && ordinary.value() == 4096);
    SLUICE_CHECK(maximum.has_value() && maximum.value() == native_max);
}

SLUICE_TEST_CASE(posix_offset_rejects_one_past_native_max_when_representable) {
    constexpr std::uint64_t native_max =
        static_cast<std::uint64_t>(std::numeric_limits<off_t>::max());
    if constexpr (native_max < std::numeric_limits<std::uint64_t>::max()) {
        auto result = sluice::detail::checked_posix_offset(native_max + 1);
        SLUICE_CHECK(!result.has_value());
        SLUICE_CHECK(result.error().code == sluice::IoError::Code::invalid_state);
    }
}

SLUICE_TEST_CASE(uring_wait_retries_one_eintr_then_succeeds) {
    ResultSequence sequence{std::array{-EINTR, 0}};
    int result = sluice::detail::retry_uring_wait_on_eintr([&] { return sequence(); });
    SLUICE_CHECK(result == 0);
    SLUICE_CHECK(sequence.calls() == 2);
}

SLUICE_TEST_CASE(uring_wait_retries_multiple_eintr_results) {
    ResultSequence sequence{std::array{-EINTR, -EINTR, -EINTR, 0}};
    int result = sluice::detail::retry_uring_wait_on_eintr([&] { return sequence(); });
    SLUICE_CHECK(result == 0);
    SLUICE_CHECK(sequence.calls() == 4);
}

SLUICE_TEST_CASE(uring_wait_does_not_retry_other_negative_errno) {
    ResultSequence sequence{std::array{-EIO, 0}};
    int result = sluice::detail::retry_uring_wait_on_eintr([&] { return sequence(); });
    SLUICE_CHECK(result == -EIO);
    SLUICE_CHECK(sequence.calls() == 1);
}

SLUICE_TEST_CASE(uring_submit_result_classifies_all_progress_states) {
    using sluice::detail::UringSubmitProgress;
    SLUICE_CHECK(sluice::detail::classify_uring_submit(-EIO, 4) ==
                 UringSubmitProgress::error);
    SLUICE_CHECK(sluice::detail::classify_uring_submit(0, 4) ==
                 UringSubmitProgress::no_progress);
    SLUICE_CHECK(sluice::detail::classify_uring_submit(2, 4) ==
                 UringSubmitProgress::partial);
    SLUICE_CHECK(sluice::detail::classify_uring_submit(4, 4) ==
                 UringSubmitProgress::complete);
    SLUICE_CHECK(sluice::detail::classify_uring_submit(0, 0) ==
                 UringSubmitProgress::complete);
}

SLUICE_MAIN()
