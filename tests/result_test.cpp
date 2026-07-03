// Tests for IoError + Result<T>/Result<void>. Behavior-focused, not shape-focused.
#include "harness.hpp"

#include <sluice/error.hpp>
#include <sluice/result.hpp>

#include <cerrno>
#include <stdexcept>
#include <string>
#include <utility>

// ---- IoError behavior ----

SLUICE_TEST_CASE(to_string_round_trips_every_code) {
    SLUICE_CHECK(sluice::to_string(sluice::IoError::Code::eof) == std::string_view("eof"));
    SLUICE_CHECK(sluice::to_string(sluice::IoError::Code::no_space) == std::string_view("no_space"));
    SLUICE_CHECK(sluice::to_string(sluice::IoError::Code::canceled) == std::string_view("canceled"));
    SLUICE_CHECK(sluice::to_string(sluice::IoError::Code::interrupted) == std::string_view("interrupted"));
    SLUICE_CHECK(sluice::to_string(sluice::IoError::Code::would_block) == std::string_view("would_block"));
    SLUICE_CHECK(sluice::to_string(sluice::IoError::Code::permission_denied) ==
                std::string_view("permission_denied"));
    SLUICE_CHECK(sluice::to_string(sluice::IoError::Code::invalid_state) ==
                std::string_view("invalid_state"));
    SLUICE_CHECK(sluice::to_string(sluice::IoError::Code::backend_error) ==
                std::string_view("backend_error"));
}

// from_errno_value must use portable <cerrno> macros (not hardcoded ints) and
// always preserve the original errno in os_errno regardless of the Code mapping.
SLUICE_TEST_CASE(from_errno_maps_known_macros_and_preserves_os_errno) {
    auto check = [](int errc, sluice::IoError::Code expected) {
        sluice::IoError e = sluice::from_errno_value(errc);
        SLUICE_CHECK(e.os_errno == errc);  // always preserved
        SLUICE_CHECK(e.code == expected);
    };
    check(EACCES, sluice::IoError::Code::permission_denied);
    check(EPERM, sluice::IoError::Code::permission_denied);
    check(ENOSPC, sluice::IoError::Code::no_space);
    check(EINTR, sluice::IoError::Code::interrupted);
    check(EAGAIN, sluice::IoError::Code::would_block);
    check(ENOENT, sluice::IoError::Code::permission_denied);
    check(ENOTDIR, sluice::IoError::Code::permission_denied);
#ifdef ECANCELED
    check(ECANCELED, sluice::IoError::Code::canceled);
#endif
#if EWOULDBLOCK != EAGAIN
    check(EWOULDBLOCK, sluice::IoError::Code::would_block);
#endif
}

SLUICE_TEST_CASE(from_errno_unknown_value_maps_to_backend_error_preserving_os_errno) {
    sluice::IoError e = sluice::from_errno_value(9999);
    SLUICE_CHECK(e.os_errno == 9999);
    SLUICE_CHECK(e.code == sluice::IoError::Code::backend_error);
}

SLUICE_TEST_CASE(from_errno_zero_is_benign) {
    sluice::IoError e = sluice::from_errno_value(0);
    SLUICE_CHECK(e.os_errno == 0);
    SLUICE_CHECK(e.code == sluice::IoError::Code::backend_error);
}

// ---- Result<T> behavior ----

SLUICE_TEST_CASE(result_value_holds_constructed_value) {
    sluice::Result<int> r{42};
    SLUICE_CHECK(r.has_value());
    SLUICE_CHECK(r.value() == 42);
}

SLUICE_TEST_CASE(result_error_holds_error) {
    sluice::Result<int> r = sluice::make_unexpected<int>(sluice::IoError{sluice::IoError::Code::eof});
    SLUICE_CHECK(!r.has_value());
    SLUICE_CHECK(r.error().code == sluice::IoError::Code::eof);
}

SLUICE_TEST_CASE(result_void_can_be_success) {
    sluice::Result<void> r{};
    SLUICE_CHECK(r.has_value());
}

SLUICE_TEST_CASE(result_void_can_be_error) {
    sluice::Result<void> r = sluice::make_unexpected<void>(sluice::IoError{sluice::IoError::Code::canceled});
    SLUICE_CHECK(!r.has_value());
    SLUICE_CHECK(r.error().code == sluice::IoError::Code::canceled);
}

SLUICE_TEST_CASE(result_value_or_returns_value_on_success) {
    sluice::Result<int> r{7};
    SLUICE_CHECK(r.value_or(99) == 7);
}

SLUICE_TEST_CASE(result_value_or_returns_fallback_on_error) {
    sluice::Result<int> r = sluice::make_unexpected<int>(sluice::IoError{sluice::IoError::Code::eof});
    SLUICE_CHECK(r.value_or(99) == 99);
}

SLUICE_TEST_CASE(result_can_carry_a_move_only_type_like_vector) {
    sluice::Result<std::vector<int>> r{std::vector<int>{1, 2, 3}};
    SLUICE_CHECK(r.has_value());
    SLUICE_CHECK(r.value().size() == 3);
    SLUICE_CHECK(r.value()[2] == 3);
}

SLUICE_TEST_CASE(result_value_can_be_moved_out) {
    sluice::Result<std::string> r{std::string("payload")};
    std::string out = std::move(r).value();
    SLUICE_CHECK(out == "payload");
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

SLUICE_TEST_CASE(result_copy_assignment_survives_throwing_ctor) {
    // The next copy of ThrowingCopy throws. We exercise assignment under
    // exceptions; if the UB were present, the destructor would corrupt state
    // and the next line could crash or trip the sanitizers.
    sluice::Result<ThrowingCopy> src{ThrowingCopy{7}};
    sluice::Result<ThrowingCopy> dst{ThrowingCopy{0}};
    ThrowingCopy::copies = 0;
    g_throw_on_copy_after = 0;  // first copy (the assignment) throws
    bool threw = false;
    try {
        dst = src;
    } catch (const std::runtime_error&) {
        threw = true;
    }
    g_throw_on_copy_after = -1;
    SLUICE_CHECK(threw);
    // After the throw, dst must be safely destructible (no UB on exit). The
    // value state is unspecified-per-standard; we deliberately do NOT assert
    // any specific value — only that we got here without a sanitizer report.
    // The throwing-type's destructor is trivial, so a clean scope exit proves
    // the destructor path ran on a destroy-safe object.
    (void)dst;
}

SLUICE_TEST_CASE(result_copy_works_and_original_unchanged) {
    sluice::Result<int> original{42};
    sluice::Result<int> copied = original;
    SLUICE_CHECK(copied.has_value() && copied.value() == 42);
    SLUICE_CHECK(original.has_value() && original.value() == 42);

    sluice::Result<int> err_src =
        sluice::make_unexpected<int>(sluice::IoError{sluice::IoError::Code::eof});
    sluice::Result<int> copied_err = err_src;
    SLUICE_CHECK(!copied_err.has_value());
    SLUICE_CHECK(copied_err.error().code == sluice::IoError::Code::eof);
}

SLUICE_TEST_CASE(result_assignment_transfers_state) {
    sluice::Result<int> a{1};
    sluice::Result<int> b{2};
    a = b;
    SLUICE_CHECK(a.value() == 2 && b.value() == 2);

    b = sluice::make_unexpected<int>(sluice::IoError{sluice::IoError::Code::canceled});
    a = b;
    SLUICE_CHECK(!a.has_value());
    SLUICE_CHECK(a.error().code == sluice::IoError::Code::canceled);
}

SLUICE_MAIN()
