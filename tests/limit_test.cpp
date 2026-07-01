// Tests for CopyLimit: a tiny value-like byte-count bound for copy/stream ops,
// modeled after Zig std.Io's Limit concept.
#include "harness.hpp"

#include <cppio/limit.hpp>

#include <cstdint>

CPPIO_TEST_CASE(copy_limit_unlimited_is_unlimited) {
    auto l = cppio::CopyLimit::unlimited();
    CPPIO_CHECK(l.kind() == cppio::CopyLimit::Kind::unlimited);
    CPPIO_CHECK(l.is_unlimited());
    CPPIO_CHECK(!l.is_limited());
}

CPPIO_TEST_CASE(copy_limit_bytes_is_limited_with_remaining) {
    auto l = cppio::CopyLimit::bytes(10);
    CPPIO_CHECK(l.kind() == cppio::CopyLimit::Kind::limited);
    CPPIO_CHECK(l.is_limited());
    CPPIO_CHECK(!l.is_unlimited());
    CPPIO_CHECK(l.remaining() == 10);
}

CPPIO_TEST_CASE(copy_limit_nothing_is_limited_with_zero_remaining) {
    auto l = cppio::CopyLimit::nothing();
    CPPIO_CHECK(l.kind() == cppio::CopyLimit::Kind::limited);
    CPPIO_CHECK(l.is_limited());
    CPPIO_CHECK(l.remaining() == 0);
}

CPPIO_TEST_CASE(copy_limit_is_value_like_and_constexpr_constructible) {
    // Compile-time check: the factories are constexpr, so a CopyLimit can be a
    // constant expression. This is part of the "tiny and value-like" contract.
    constexpr cppio::CopyLimit unlim = cppio::CopyLimit::unlimited();
    constexpr cppio::CopyLimit ten = cppio::CopyLimit::bytes(10);
    constexpr cppio::CopyLimit zero = cppio::CopyLimit::nothing();
    static_assert(unlim.is_unlimited());
    static_assert(ten.is_limited() && ten.remaining() == 10);
    static_assert(zero.is_limited() && zero.remaining() == 0);
}

CPPIO_MAIN()
