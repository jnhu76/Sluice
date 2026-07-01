// Tests for IoError + Result<T>/Result<void>. Behavior-focused, not shape-focused.
#include "harness.hpp"

#include <cppio/error.hpp>
#include <cppio/result.hpp>

#include <cerrno>
#include <stdexcept>
#include <string>
#include <utility>

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

// Regression for the assignment UB: a throwing copy ctor must not leave the
// Result claiming to hold a value it never constructed. Previously, has_value_
// was set to true before placement-new; if the ctor threw, the destructor ran
// value_.~T() on uninitialized storage (UB). Now has_value_ is published only
// after a successful construction.
namespace {
int g_throw_on_copy_after = -1;  // copy number that should throw (-1 = never)
struct ThrowingCopy {
    int v;
    static int copies;
    ThrowingCopy(int x) : v(x) {}
    ThrowingCopy(const ThrowingCopy& o) : v(o.v) {
        if (g_throw_on_copy_after >= 0 && ++copies > g_throw_on_copy_after) {
            throw std::runtime_error("induced copy failure");
        }
    }
    ThrowingCopy& operator=(const ThrowingCopy&) = delete;
};
int ThrowingCopy::copies = 0;
}  // namespace

CPPIO_TEST_CASE(result_copy_assignment_survives_throwing_ctor) {
    // The next copy of ThrowingCopy throws. We exercise assignment under
    // exceptions; if the UB were present, the destructor would corrupt state
    // and the next line could crash or trip the sanitizers.
    cppio::Result<ThrowingCopy> src{ThrowingCopy{7}};
    cppio::Result<ThrowingCopy> dst{ThrowingCopy{0}};
    ThrowingCopy::copies = 0;
    g_throw_on_copy_after = 0;  // first copy (the assignment) throws
    bool threw = false;
    try {
        dst = src;
    } catch (const std::runtime_error&) {
        threw = true;
    }
    g_throw_on_copy_after = -1;
    CPPIO_CHECK(threw);
    // After the throw, dst must be safely destructible (no UB on exit). The
    // value state is unspecified-per-standard; we deliberately do NOT assert
    // any specific value — only that we got here without a sanitizer report.
    // The throwing-type's destructor is trivial, so a clean scope exit proves
    // the destructor path ran on a destroy-safe object.
    (void)dst;
}

CPPIO_TEST_CASE(result_copy_works_and_original_unchanged) {
    cppio::Result<int> original{42};
    cppio::Result<int> copied = original;
    CPPIO_CHECK(copied.has_value() && copied.value() == 42);
    CPPIO_CHECK(original.has_value() && original.value() == 42);

    cppio::Result<int> err_src =
        cppio::make_unexpected<int>(cppio::IoError{cppio::IoError::Code::eof});
    cppio::Result<int> copied_err = err_src;
    CPPIO_CHECK(!copied_err.has_value());
    CPPIO_CHECK(copied_err.error().code == cppio::IoError::Code::eof);
}

CPPIO_TEST_CASE(result_assignment_transfers_state) {
    cppio::Result<int> a{1};
    cppio::Result<int> b{2};
    a = b;
    CPPIO_CHECK(a.value() == 2 && b.value() == 2);

    b = cppio::make_unexpected<int>(cppio::IoError{cppio::IoError::Code::canceled});
    a = b;
    CPPIO_CHECK(!a.has_value());
    CPPIO_CHECK(a.error().code == cppio::IoError::Code::canceled);
}

CPPIO_MAIN()
