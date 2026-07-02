// Tests for CopyStats strategy selection counters (CPPIO-CORE-007F). Verifies
// exactly one strategy counter fires per top-level copy_all call, deferred
// rejected/fallback are counted, nothing() still counts the strategy, and path
// byte counters remain correct.
#include "harness.hpp"

#include <cppio/buffer.hpp>
#include <cppio/copy.hpp>
#include <cppio/copy_strategy.hpp>
#include <cppio/fault.hpp>
#include <cppio/limit.hpp>
#include <cppio/measurement.hpp>

#include <cstring>
#include <span>
#include <vector>

namespace {

class CountingReader final : public cppio::Reader {
public:
    cppio::MemoryReader mem;
    explicit CountingReader(std::string_view s) : mem(cppio::MemoryReader::from_string(s)) {}
    cppio::Result<std::size_t> read_some(std::span<std::byte> dst) override {
        return mem.read_some(dst);
    }
};

bool eq(std::string_view s, const std::vector<std::byte>& b) {
    return b.size() == s.size() && std::memcmp(s.data(), b.data(), s.size()) == 0;
}

cppio::CopyOptions opts_with(cppio::CopyStrategy s, cppio::CopyLimit lim = cppio::CopyLimit::unlimited(),
                             cppio::UnsupportedStrategyPolicy p =
                                 cppio::UnsupportedStrategyPolicy::ReturnInvalidState) {
    cppio::CopyOptions o;
    o.strategy = s;
    o.limit = lim;
    o.unsupported_policy = p;
    return o;
}

}  // namespace

CPPIO_TEST_CASE(auto_increments_auto_counter) {
    CountingReader inner("0123456789ABCDEF");
    std::vector<std::byte> rbuf(64);
    cppio::BufferedReader br(inner, rbuf);
    std::vector<std::byte> primed(4);
    (void)br.read_some(std::span<std::byte>(primed));
    cppio::MemoryWriter writer;
    std::vector<std::byte> scratch(8);
    cppio::CopyStats st;
    auto res = cppio::copy_all(br, writer, std::span<std::byte>(scratch),
                               opts_with(cppio::CopyStrategy::Auto), &st);
    CPPIO_CHECK(res.has_value());
    CPPIO_CHECK(st.strategy_auto_calls == 1);
    CPPIO_CHECK(st.strategy_scratch_calls == 0);
    CPPIO_CHECK(st.strategy_buffered_first_calls == 0);
}

CPPIO_TEST_CASE(scratch_increments_scratch_counter) {
    auto reader = cppio::MemoryReader::from_string("hello world");
    cppio::MemoryWriter writer;
    std::vector<std::byte> scratch(4);
    cppio::CopyStats st;
    auto res = cppio::copy_all(reader, writer, std::span<std::byte>(scratch),
                               opts_with(cppio::CopyStrategy::Scratch), &st);
    CPPIO_CHECK(res.has_value());
    CPPIO_CHECK(st.strategy_scratch_calls == 1);
    CPPIO_CHECK(st.strategy_auto_calls == 0);
    CPPIO_CHECK(st.strategy_buffered_first_calls == 0);
}

CPPIO_TEST_CASE(buffered_first_increments_buffered_first_counter) {
    CountingReader inner("0123456789ABCDEF");
    std::vector<std::byte> rbuf(64);
    cppio::BufferedReader br(inner, rbuf);
    std::vector<std::byte> primed(4);
    (void)br.read_some(std::span<std::byte>(primed));
    cppio::MemoryWriter writer;
    std::vector<std::byte> scratch(8);
    cppio::CopyStats st;
    auto res = cppio::copy_all(br, writer, std::span<std::byte>(scratch),
                               opts_with(cppio::CopyStrategy::BufferedFirst), &st);
    CPPIO_CHECK(res.has_value());
    CPPIO_CHECK(st.strategy_buffered_first_calls == 1);
    CPPIO_CHECK(st.strategy_scratch_calls == 0);
    CPPIO_CHECK(st.strategy_auto_calls == 0);
}

CPPIO_TEST_CASE(deferred_rejected_increments_deferred_rejected_counter) {
    auto reader = cppio::MemoryReader::from_string("abc");
    cppio::MemoryWriter writer;
    std::vector<std::byte> scratch(8);
    cppio::CopyStats st;
    auto res = cppio::copy_all(reader, writer, std::span<std::byte>(scratch),
                               opts_with(cppio::CopyStrategy::VectorDeferred), &st);
    CPPIO_CHECK(!res.has_value());
    CPPIO_CHECK(st.strategy_deferred_rejected_calls == 1);
    CPPIO_CHECK(st.strategy_deferred_fallback_calls == 0);
    // Rejected strategies do not count as a selected strategy.
    CPPIO_CHECK(st.strategy_auto_calls == 0);
    CPPIO_CHECK(st.strategy_scratch_calls == 0);
    CPPIO_CHECK(st.strategy_buffered_first_calls == 0);
}

CPPIO_TEST_CASE(deferred_fallback_increments_fallback_and_selected_counters) {
    CountingReader inner("0123456789ABCDEF");
    std::vector<std::byte> rbuf(64);
    cppio::BufferedReader br(inner, rbuf);
    std::vector<std::byte> primed(4);
    (void)br.read_some(std::span<std::byte>(primed));
    cppio::MemoryWriter writer;
    std::vector<std::byte> scratch(8);
    cppio::CopyStats st;
    auto res = cppio::copy_all(br, writer, std::span<std::byte>(scratch),
                               opts_with(cppio::CopyStrategy::SendfileDeferred,
                                         cppio::CopyLimit::unlimited(),
                                         cppio::UnsupportedStrategyPolicy::FallbackToAuto),
                               &st);
    CPPIO_CHECK(res.has_value());
    CPPIO_CHECK(st.strategy_deferred_fallback_calls == 1);
    // The fallback normalized to Auto, so Auto is also counted as selected.
    CPPIO_CHECK(st.strategy_auto_calls == 1);
    CPPIO_CHECK(st.strategy_deferred_rejected_calls == 0);
}

CPPIO_TEST_CASE(strategy_counters_fire_even_with_nothing_limit) {
    // nothing() short-circuits before any path runs, but the strategy was still
    // selected at the top of copy_all.
    auto reader = cppio::MemoryReader::from_string("abc");
    cppio::MemoryWriter writer;
    std::vector<std::byte> scratch(8);
    cppio::CopyStats st;
    auto res = cppio::copy_all(reader, writer, std::span<std::byte>(scratch),
                               opts_with(cppio::CopyStrategy::BufferedFirst, cppio::CopyLimit::nothing()),
                               &st);
    CPPIO_CHECK(res.has_value());
    CPPIO_CHECK(res.value() == 0);
    CPPIO_CHECK(st.strategy_buffered_first_calls == 1);
    // No bytes moved.
    CPPIO_CHECK(st.buffered_fast_path_bytes == 0);
    CPPIO_CHECK(st.scratch_path_bytes == 0);
}

CPPIO_TEST_CASE(path_byte_counters_remain_correct) {
    // BufferedFirst with a small read buffer forces mixed buffered/scratch turns
    // (a scratch read into the BufferedReader leaves leftover buffered bytes the
    // next iteration drains via fast path). The path byte counters must sum to
    // the total bytes copied; the exact split depends on buffer/scratch sizes,
    // so we assert the invariant rather than a specific split.
    CountingReader inner("0123456789ABCDEF");
    std::vector<std::byte> rbuf(8);  // small: forces interleaving
    cppio::BufferedReader br(inner, rbuf);
    std::vector<std::byte> primed(2);
    (void)br.read_some(std::span<std::byte>(primed));
    cppio::MemoryWriter writer;
    std::vector<std::byte> scratch(4);
    cppio::CopyStats st;
    auto res = cppio::copy_all(br, writer, std::span<std::byte>(scratch),
                               opts_with(cppio::CopyStrategy::BufferedFirst), &st);
    CPPIO_CHECK(res.has_value());
    CPPIO_CHECK(res.value() == 14);  // 16 total - 2 primed
    CPPIO_CHECK(eq("23456789ABCDEF", writer.bytes()));
    CPPIO_CHECK(st.strategy_buffered_first_calls == 1);
    // Path counters partition the copied bytes with no skip/dup.
    CPPIO_CHECK(st.buffered_fast_path_bytes + st.scratch_path_bytes == 14);
    CPPIO_CHECK(st.bytes_read == 14);
    CPPIO_CHECK(st.bytes_written == 14);
}

CPPIO_MAIN()
