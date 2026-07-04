// Tests for CopyStats strategy selection counters (CPPIO-CORE-007F). Verifies
// exactly one strategy counter fires per top-level copy_all call, deferred
// rejected/fallback are counted, nothing() still counts the strategy, and path
// byte counters remain correct.
#include "harness.hpp"

#include <sluice/buffer.hpp>
#include <sluice/copy.hpp>
#include <sluice/copy_strategy.hpp>
#include <sluice/fault.hpp>
#include <sluice/limit.hpp>
#include <sluice/measurement.hpp>

#include <cstring>
#include <span>
#include <vector>

namespace {

class CountingReader final : public sluice::Reader {
  public:
    sluice::MemoryReader mem;
    explicit CountingReader(std::string_view s) : mem(sluice::MemoryReader::from_string(s)) {}
    sluice::Result<std::size_t> read_some(std::span<std::byte> dst) override {
        return mem.read_some(dst);
    }
};

bool eq(std::string_view s, const std::vector<std::byte>& b) {
    return b.size() == s.size() && std::memcmp(s.data(), b.data(), s.size()) == 0;
}

sluice::CopyOptions opts_with(
    sluice::CopyStrategy s, sluice::CopyLimit lim = sluice::CopyLimit::unlimited(),
    sluice::UnsupportedStrategyPolicy p = sluice::UnsupportedStrategyPolicy::ReturnInvalidState) {
    sluice::CopyOptions o;
    o.strategy = s;
    o.limit = lim;
    o.unsupported_policy = p;
    return o;
}

} // namespace

SLUICE_TEST_CASE(auto_increments_auto_counter) {
    CountingReader inner("0123456789ABCDEF");
    std::vector<std::byte> rbuf(64);
    sluice::BufferedReader br(inner, rbuf);
    std::vector<std::byte> primed(4);
    (void)br.read_some(std::span<std::byte>(primed));
    sluice::MemoryWriter writer;
    std::vector<std::byte> scratch(8);
    sluice::CopyStats st;
    auto res = sluice::copy_all(br, writer, std::span<std::byte>(scratch),
                                opts_with(sluice::CopyStrategy::Auto), &st);
    SLUICE_CHECK(res.has_value());
    SLUICE_CHECK(st.strategy_auto_calls == 1);
    SLUICE_CHECK(st.strategy_scratch_calls == 0);
    SLUICE_CHECK(st.strategy_buffered_first_calls == 0);
}

SLUICE_TEST_CASE(scratch_increments_scratch_counter) {
    auto reader = sluice::MemoryReader::from_string("hello world");
    sluice::MemoryWriter writer;
    std::vector<std::byte> scratch(4);
    sluice::CopyStats st;
    auto res = sluice::copy_all(reader, writer, std::span<std::byte>(scratch),
                                opts_with(sluice::CopyStrategy::Scratch), &st);
    SLUICE_CHECK(res.has_value());
    SLUICE_CHECK(st.strategy_scratch_calls == 1);
    SLUICE_CHECK(st.strategy_auto_calls == 0);
    SLUICE_CHECK(st.strategy_buffered_first_calls == 0);
}

SLUICE_TEST_CASE(buffered_first_increments_buffered_first_counter) {
    CountingReader inner("0123456789ABCDEF");
    std::vector<std::byte> rbuf(64);
    sluice::BufferedReader br(inner, rbuf);
    std::vector<std::byte> primed(4);
    (void)br.read_some(std::span<std::byte>(primed));
    sluice::MemoryWriter writer;
    std::vector<std::byte> scratch(8);
    sluice::CopyStats st;
    auto res = sluice::copy_all(br, writer, std::span<std::byte>(scratch),
                                opts_with(sluice::CopyStrategy::BufferedFirst), &st);
    SLUICE_CHECK(res.has_value());
    SLUICE_CHECK(st.strategy_buffered_first_calls == 1);
    SLUICE_CHECK(st.strategy_scratch_calls == 0);
    SLUICE_CHECK(st.strategy_auto_calls == 0);
}

SLUICE_TEST_CASE(deferred_rejected_increments_deferred_rejected_counter) {
    auto reader = sluice::MemoryReader::from_string("abc");
    sluice::MemoryWriter writer;
    std::vector<std::byte> scratch(8);
    sluice::CopyStats st;
    auto res = sluice::copy_all(reader, writer, std::span<std::byte>(scratch),
                                opts_with(sluice::CopyStrategy::VectorDeferred), &st);
    SLUICE_CHECK(!res.has_value());
    SLUICE_CHECK(st.strategy_deferred_rejected_calls == 1);
    SLUICE_CHECK(st.strategy_deferred_fallback_calls == 0);
    // Rejected strategies do not count as a selected strategy.
    SLUICE_CHECK(st.strategy_auto_calls == 0);
    SLUICE_CHECK(st.strategy_scratch_calls == 0);
    SLUICE_CHECK(st.strategy_buffered_first_calls == 0);
}

SLUICE_TEST_CASE(deferred_fallback_increments_fallback_and_selected_counters) {
    CountingReader inner("0123456789ABCDEF");
    std::vector<std::byte> rbuf(64);
    sluice::BufferedReader br(inner, rbuf);
    std::vector<std::byte> primed(4);
    (void)br.read_some(std::span<std::byte>(primed));
    sluice::MemoryWriter writer;
    std::vector<std::byte> scratch(8);
    sluice::CopyStats st;
    auto res = sluice::copy_all(br, writer, std::span<std::byte>(scratch),
                                opts_with(sluice::CopyStrategy::SendfileDeferred,
                                          sluice::CopyLimit::unlimited(),
                                          sluice::UnsupportedStrategyPolicy::FallbackToAuto),
                                &st);
    SLUICE_CHECK(res.has_value());
    SLUICE_CHECK(st.strategy_deferred_fallback_calls == 1);
    // The fallback normalized to Auto, so Auto is also counted as selected.
    SLUICE_CHECK(st.strategy_auto_calls == 1);
    SLUICE_CHECK(st.strategy_deferred_rejected_calls == 0);
}

SLUICE_TEST_CASE(strategy_counters_fire_even_with_nothing_limit) {
    // nothing() short-circuits before any path runs, but the strategy was still
    // selected at the top of copy_all.
    auto reader = sluice::MemoryReader::from_string("abc");
    sluice::MemoryWriter writer;
    std::vector<std::byte> scratch(8);
    sluice::CopyStats st;
    auto res = sluice::copy_all(
        reader, writer, std::span<std::byte>(scratch),
        opts_with(sluice::CopyStrategy::BufferedFirst, sluice::CopyLimit::nothing()), &st);
    SLUICE_CHECK(res.has_value());
    SLUICE_CHECK(res.value() == 0);
    SLUICE_CHECK(st.strategy_buffered_first_calls == 1);
    // No bytes moved.
    SLUICE_CHECK(st.buffered_fast_path_bytes == 0);
    SLUICE_CHECK(st.scratch_path_bytes == 0);
}

SLUICE_TEST_CASE(path_byte_counters_remain_correct) {
    // BufferedFirst with a small read buffer forces mixed buffered/scratch turns
    // (a scratch read into the BufferedReader leaves leftover buffered bytes the
    // next iteration drains via fast path). The path byte counters must sum to
    // the total bytes copied; the exact split depends on buffer/scratch sizes,
    // so we assert the invariant rather than a specific split.
    CountingReader inner("0123456789ABCDEF");
    std::vector<std::byte> rbuf(8); // small: forces interleaving
    sluice::BufferedReader br(inner, rbuf);
    std::vector<std::byte> primed(2);
    (void)br.read_some(std::span<std::byte>(primed));
    sluice::MemoryWriter writer;
    std::vector<std::byte> scratch(4);
    sluice::CopyStats st;
    auto res = sluice::copy_all(br, writer, std::span<std::byte>(scratch),
                                opts_with(sluice::CopyStrategy::BufferedFirst), &st);
    SLUICE_CHECK(res.has_value());
    SLUICE_CHECK(res.value() == 14); // 16 total - 2 primed
    SLUICE_CHECK(eq("23456789ABCDEF", writer.bytes()));
    SLUICE_CHECK(st.strategy_buffered_first_calls == 1);
    // Path counters partition the copied bytes with no skip/dup.
    SLUICE_CHECK(st.buffered_fast_path_bytes + st.scratch_path_bytes == 14);
    SLUICE_CHECK(st.bytes_read == 14);
    SLUICE_CHECK(st.bytes_written == 14);
}

SLUICE_MAIN()
