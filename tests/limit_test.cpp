// Tests for CopyLimit: a tiny value-like byte-count bound for copy/stream ops,
// modeled after Zig std.Io's Limit concept.
#include "harness.hpp"

#include <sluice/limit.hpp>

#include <concepts>
#include <cstdint>

SLUICE_TEST_CASE(copy_limit_unlimited_is_unlimited) {
    auto l = sluice::CopyLimit::unlimited();
    SLUICE_CHECK(l.kind() == sluice::CopyLimit::Kind::unlimited);
    SLUICE_CHECK(l.is_unlimited());
    SLUICE_CHECK(!l.is_limited());
}

SLUICE_TEST_CASE(copy_limit_bytes_is_limited_with_remaining) {
    auto l = sluice::CopyLimit::bytes(10);
    SLUICE_CHECK(l.kind() == sluice::CopyLimit::Kind::limited);
    SLUICE_CHECK(l.is_limited());
    SLUICE_CHECK(!l.is_unlimited());
    SLUICE_CHECK(l.remaining() == 10);
}

SLUICE_TEST_CASE(copy_limit_nothing_is_limited_with_zero_remaining) {
    auto l = sluice::CopyLimit::nothing();
    SLUICE_CHECK(l.kind() == sluice::CopyLimit::Kind::limited);
    SLUICE_CHECK(l.is_limited());
    SLUICE_CHECK(l.remaining() == 0);
}

SLUICE_TEST_CASE(copy_limit_is_value_like_and_constexpr_constructible) {
    // Compile-time check: the factories are constexpr, so a CopyLimit can be a
    // constant expression. This is part of the "tiny and value-like" contract.
    constexpr sluice::CopyLimit unlim = sluice::CopyLimit::unlimited();
    constexpr sluice::CopyLimit ten = sluice::CopyLimit::bytes(10);
    constexpr sluice::CopyLimit zero = sluice::CopyLimit::nothing();
    static_assert(unlim.is_unlimited());
    static_assert(ten.is_limited() && ten.remaining() == 10);
    static_assert(zero.is_limited() && zero.remaining() == 0);
}

// A default-constructed CopyLimit would silently behave like nothing() (copy
// zero bytes), which is a footgun. Instances must come from the factories only.
static_assert(!std::default_initializable<sluice::CopyLimit>);

SLUICE_MAIN()
