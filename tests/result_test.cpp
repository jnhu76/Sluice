// Tests for IoError + Result<T>/Result<void>. Behavior-focused, not shape-focused.
#include "harness.hpp"

#include <cppio/error.hpp>
#include <cppio/result.hpp>

#include <cerrno>
#include <string>

// ---- IoError behavior ----

CPPIO_TEST_CASE(to_string_round_trips_every_code) {
    CPPIO_CHECK(cppio::to_string(cppio::IoError::Code::eof) == std::string_view("eof"));
    CPPIO_CHECK(cppio::to_string(cppio::IoError::Code::no_space) == std::string_view("no_space"));
    CPPIO_CHECK(cppio::to_string(cppio::IoError::Code::canceled) == std::string_view("canceled"));
    CPPIO_CHECK(cppio::to_string(cppio::IoError::Code::interrupted) == std::string_view("interrupted"));
    CPPIO_CHECK(cppio::to_string(cppio::IoError::Code::would_block) == std::string_view("would_block"));
    CPPIO_CHECK(cppio::to_string(cppio::IoError::Code::permission_denied) ==
                std::string_view("permission_denied"));
    CPPIO_CHECK(cppio::to_string(cppio::IoError::Code::invalid_state) ==
                std::string_view("invalid_state"));
    CPPIO_CHECK(cppio::to_string(cppio::IoError::Code::backend_error) ==
                std::string_view("backend_error"));
}

// from_errno_value must use portable <cerrno> macros (not hardcoded ints) and
// always preserve the original errno in os_errno regardless of the Code mapping.
CPPIO_TEST_CASE(from_errno_maps_known_macros_and_preserves_os_errno) {
    auto check = [](int errc, cppio::IoError::Code expected) {
        cppio::IoError e = cppio::from_errno_value(errc);
        CPPIO_CHECK(e.os_errno == errc);  // always preserved
        CPPIO_CHECK(e.code == expected);
    };
    check(EACCES, cppio::IoError::Code::permission_denied);
    check(EPERM, cppio::IoError::Code::permission_denied);
    check(ENOSPC, cppio::IoError::Code::no_space);
    check(EINTR, cppio::IoError::Code::interrupted);
    check(EAGAIN, cppio::IoError::Code::would_block);
    check(ENOENT, cppio::IoError::Code::permission_denied);
    check(ENOTDIR, cppio::IoError::Code::permission_denied);
#ifdef ECANCELED
    check(ECANCELED, cppio::IoError::Code::canceled);
#endif
#if EWOULDBLOCK != EAGAIN
    check(EWOULDBLOCK, cppio::IoError::Code::would_block);
#endif
}

CPPIO_TEST_CASE(from_errno_unknown_value_maps_to_backend_error_preserving_os_errno) {
    cppio::IoError e = cppio::from_errno_value(9999);
    CPPIO_CHECK(e.os_errno == 9999);
    CPPIO_CHECK(e.code == cppio::IoError::Code::backend_error);
}

CPPIO_TEST_CASE(from_errno_zero_is_benign) {
    cppio::IoError e = cppio::from_errno_value(0);
    CPPIO_CHECK(e.os_errno == 0);
    CPPIO_CHECK(e.code == cppio::IoError::Code::backend_error);
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
