// Tests for the copy strategy API types (CPPIO-CORE-007B). No behavior change
// yet — these verify the enum/string mappings and default values only.
#include "harness.hpp"

#include <cppio/copy_strategy.hpp>
#include <cppio/limit.hpp>

#include <string_view>

CPPIO_TEST_CASE(to_string_copy_strategy_values) {
    CPPIO_CHECK(cppio::to_string(cppio::CopyStrategy::Auto) == std::string_view("auto"));
    CPPIO_CHECK(cppio::to_string(cppio::CopyStrategy::Scratch) == std::string_view("scratch"));
    CPPIO_CHECK(cppio::to_string(cppio::CopyStrategy::BufferedFirst) ==
                std::string_view("buffered_first"));
    CPPIO_CHECK(cppio::to_string(cppio::CopyStrategy::VectorDeferred) ==
                std::string_view("vector_deferred"));
    CPPIO_CHECK(cppio::to_string(cppio::CopyStrategy::FileRangeDeferred) ==
                std::string_view("file_range_deferred"));
    CPPIO_CHECK(cppio::to_string(cppio::CopyStrategy::SendfileDeferred) ==
                std::string_view("sendfile_deferred"));
    CPPIO_CHECK(cppio::to_string(cppio::CopyStrategy::SpliceDeferred) ==
                std::string_view("splice_deferred"));
}

CPPIO_TEST_CASE(to_string_unsupported_strategy_policy_values) {
    CPPIO_CHECK(cppio::to_string(cppio::UnsupportedStrategyPolicy::ReturnInvalidState) ==
                std::string_view("return_invalid_state"));
    CPPIO_CHECK(cppio::to_string(cppio::UnsupportedStrategyPolicy::FallbackToAuto) ==
                std::string_view("fallback_to_auto"));
}

CPPIO_TEST_CASE(copy_options_default_values) {
    cppio::CopyOptions opts;
    CPPIO_CHECK(opts.strategy == cppio::CopyStrategy::Auto);
    CPPIO_CHECK(opts.unsupported_policy ==
                cppio::UnsupportedStrategyPolicy::ReturnInvalidState);
    // Default limit is unlimited.
    CPPIO_CHECK(opts.limit.is_unlimited());
}

CPPIO_TEST_CASE(copy_decision_default_values) {
    cppio::CopyDecision d;
    CPPIO_CHECK(d.requested == cppio::CopyStrategy::Auto);
    CPPIO_CHECK(d.selected == cppio::CopyStrategy::Auto);
    CPPIO_CHECK(d.reason == std::string_view("auto"));
    CPPIO_CHECK(!d.used_buffered_fast_path);
    CPPIO_CHECK(!d.used_scratch_path);
    CPPIO_CHECK(!d.unsupported_requested);
}

CPPIO_MAIN()
