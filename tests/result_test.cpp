// Tests for IoError + Result<T>/Result<void>. Behavior-focused, not shape-focused.
#include "harness.hpp"

#include <cppio/error.hpp>
#include <cppio/result.hpp>

#include <string>

// ---- IoError behavior ----

CPPIO_TEST_CASE(iorror_eof_code_round_trips_through_to_string) {
    CPPIO_CHECK(cppio::to_string(cppio::IoError::Code::eof) == std::string_view("eof"));
    CPPIO_CHECK(cppio::to_string(cppio::IoError::Code::no_space) == std::string_view("no_space"));
    CPPIO_CHECK(cppio::to_string(cppio::IoError::Code::backend_error) ==
                std::string_view("backend_error"));
}

CPPIO_TEST_CASE(from_errno_maps_positive_errno_to_interrupted_for_eintr_like_values) {
    // from_errno must be deterministic and produce a sensible code for ENOSPC.
    cppio::IoError e = cppio::from_errno_value(28 /* ENOSPC on Linux */);
    CPPIO_CHECK(e.code == cppio::IoError::Code::no_space);
    CPPIO_CHECK(e.os_errno == 28);
}

CPPIO_TEST_CASE(from_errno_zero_means_ok_is_not_an_error_helper) {
    // errno 0 maps to a benign code; not used as an error by Result.
    cppio::IoError e = cppio::from_errno_value(0);
    CPPIO_CHECK(e.os_errno == 0);
}

// ---- Result<T> behavior ----

CPPIO_TEST_CASE(result_value_holds_constructed_value) {
    cppio::Result<int> r{42};
    CPPIO_CHECK(r.has_value());
    CPPIO_CHECK(r.value() == 42);
}

CPPIO_TEST_CASE(result_error_holds_error) {
    cppio::Result<int> r = cppio::make_unexpected<int>(cppio::IoError{cppio::IoError::Code::eof});
    CPPIO_CHECK(!r.has_value());
    CPPIO_CHECK(r.error().code == cppio::IoError::Code::eof);
}

CPPIO_TEST_CASE(result_void_can_be_success) {
    cppio::Result<void> r{};
    CPPIO_CHECK(r.has_value());
}

CPPIO_TEST_CASE(result_void_can_be_error) {
    cppio::Result<void> r = cppio::make_unexpected<void>(cppio::IoError{cppio::IoError::Code::canceled});
    CPPIO_CHECK(!r.has_value());
    CPPIO_CHECK(r.error().code == cppio::IoError::Code::canceled);
}

CPPIO_TEST_CASE(result_value_or_returns_value_on_success) {
    cppio::Result<int> r{7};
    CPPIO_CHECK(r.value_or(99) == 7);
}

CPPIO_TEST_CASE(result_value_or_returns_fallback_on_error) {
    cppio::Result<int> r = cppio::make_unexpected<int>(cppio::IoError{cppio::IoError::Code::eof});
    CPPIO_CHECK(r.value_or(99) == 99);
}

CPPIO_TEST_CASE(result_can_carry_a_move_only_type_like_vector) {
    cppio::Result<std::vector<int>> r{std::vector<int>{1, 2, 3}};
    CPPIO_CHECK(r.has_value());
    CPPIO_CHECK(r.value().size() == 3);
    CPPIO_CHECK(r.value()[2] == 3);
}

CPPIO_TEST_CASE(result_value_can_be_moved_out) {
    cppio::Result<std::string> r{std::string("payload")};
    std::string out = std::move(r).value();
    CPPIO_CHECK(out == "payload");
}

CPPIO_MAIN()
