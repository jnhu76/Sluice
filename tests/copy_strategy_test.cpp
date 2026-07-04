// Tests for the copy strategy API types (CPPIO-CORE-007B). No behavior change
// yet — these verify the enum/string mappings and default values only.
#include "harness.hpp"

#include <sluice/copy_strategy.hpp>
#include <sluice/limit.hpp>

#include <string_view>

SLUICE_TEST_CASE(to_string_copy_strategy_values) {
    SLUICE_CHECK(sluice::to_string(sluice::CopyStrategy::Auto) == std::string_view("auto"));
    SLUICE_CHECK(sluice::to_string(sluice::CopyStrategy::Scratch) == std::string_view("scratch"));
    SLUICE_CHECK(sluice::to_string(sluice::CopyStrategy::BufferedFirst) ==
                 std::string_view("buffered_first"));
    SLUICE_CHECK(sluice::to_string(sluice::CopyStrategy::VectorDeferred) ==
                 std::string_view("vector_deferred"));
    SLUICE_CHECK(sluice::to_string(sluice::CopyStrategy::FileRangeDeferred) ==
                 std::string_view("file_range_deferred"));
    SLUICE_CHECK(sluice::to_string(sluice::CopyStrategy::SendfileDeferred) ==
                 std::string_view("sendfile_deferred"));
    SLUICE_CHECK(sluice::to_string(sluice::CopyStrategy::SpliceDeferred) ==
                 std::string_view("splice_deferred"));
}

SLUICE_TEST_CASE(to_string_unsupported_strategy_policy_values) {
    SLUICE_CHECK(sluice::to_string(sluice::UnsupportedStrategyPolicy::ReturnInvalidState) ==
                 std::string_view("return_invalid_state"));
    SLUICE_CHECK(sluice::to_string(sluice::UnsupportedStrategyPolicy::FallbackToAuto) ==
                 std::string_view("fallback_to_auto"));
}

SLUICE_TEST_CASE(copy_options_default_values) {
    sluice::CopyOptions opts;
    SLUICE_CHECK(opts.strategy == sluice::CopyStrategy::Auto);
    SLUICE_CHECK(opts.unsupported_policy == sluice::UnsupportedStrategyPolicy::ReturnInvalidState);
    // Default limit is unlimited.
    SLUICE_CHECK(opts.limit.is_unlimited());
}

SLUICE_TEST_CASE(copy_decision_default_values) {
    sluice::CopyDecision d;
    SLUICE_CHECK(d.requested == sluice::CopyStrategy::Auto);
    SLUICE_CHECK(d.selected == sluice::CopyStrategy::Auto);
    SLUICE_CHECK(d.reason == std::string_view("auto"));
    SLUICE_CHECK(!d.used_buffered_fast_path);
    SLUICE_CHECK(!d.used_scratch_path);
    SLUICE_CHECK(!d.unsupported_requested);
}

SLUICE_MAIN()
