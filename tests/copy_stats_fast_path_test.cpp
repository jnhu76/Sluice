// Tests for CopyStats fast/scratch counters (CPPIO-CORE-006E). Verifies the
// buffered fast path and scratch path are counted separately and that
// bytes_read/bytes_written stay correct in mixed scenarios.
#include "harness.hpp"

#include <sluice/buffer.hpp>
#include <sluice/copy.hpp>
#include <sluice/fault.hpp>
#include <sluice/limit.hpp>
#include <sluice/measurement.hpp>
#include <sluice/reader.hpp>
#include <sluice/writer.hpp>

#include <cstring>
#include <span>
#include <vector>

namespace {

class CountingReader final : public sluice::Reader {
  public:
    sluice::MemoryReader mem;
    int calls = 0;
    explicit CountingReader(std::string_view s) : mem(sluice::MemoryReader::from_string(s)) {}
    sluice::Result<std::size_t> read_some(std::span<std::byte> dst) override {
        ++calls;
        return mem.read_some(dst);
    }
};

bool eq(std::string_view s, const std::vector<std::byte>& b) {
    return b.size() == s.size() && std::memcmp(s.data(), b.data(), s.size()) == 0;
}

} // namespace

SLUICE_TEST_CASE(copy_stats_buffered_fast_path_increments_fast_counters) {
    CountingReader inner("0123456789ABCDEF");
    std::vector<std::byte> rbuf(64);
    sluice::BufferedReader br(inner, rbuf);
    std::vector<std::byte> primed(4);
    (void)br.read_some(std::span<std::byte>(primed)); // 12 buffered remain

    sluice::MemoryWriter writer;
    std::vector<std::byte> scratch(8);
    sluice::CopyStats st{};
    auto res = sluice::copy_all(br, writer, std::span<std::byte>(scratch),
                                sluice::CopyLimit::bytes(5), &st); // stop at limit, no scratch
    SLUICE_CHECK(res.has_value());
    SLUICE_CHECK(res.value() == 5);
    SLUICE_CHECK(st.buffered_fast_path_calls >= 1);
    SLUICE_CHECK(st.buffered_fast_path_bytes == 5);
    SLUICE_CHECK(st.scratch_path_calls == 0);
    SLUICE_CHECK(st.scratch_path_bytes == 0);
}

SLUICE_TEST_CASE(copy_stats_scratch_path_increments_scratch_counters) {
    // Plain MemoryReader is not BufferedReadable: pure scratch path.
    auto reader = sluice::MemoryReader::from_string("hello world");
    sluice::MemoryWriter writer;
    std::vector<std::byte> scratch(4);
    sluice::CopyStats st{};
    auto res = sluice::copy_all(reader, writer, std::span<std::byte>(scratch),
                                sluice::CopyLimit::unlimited(), &st);
    SLUICE_CHECK(res.has_value());
    SLUICE_CHECK(res.value() == 11);
    SLUICE_CHECK(eq("hello world", writer.bytes()));
    SLUICE_CHECK(st.buffered_fast_path_calls == 0);
    SLUICE_CHECK(st.buffered_fast_path_bytes == 0);
    SLUICE_CHECK(st.scratch_path_calls >= 1);
    SLUICE_CHECK(st.scratch_path_bytes == 11);
}

SLUICE_TEST_CASE(copy_stats_mixed_buffered_and_scratch_counts_both) {
    // Prime 4 bytes out of 16, leaving 12 buffered. copy_all drains 12 via the
    // fast path, then the scratch path probes EOF (one scratch call, 0 bytes).
    CountingReader inner("0123456789ABCDEF");
    std::vector<std::byte> rbuf(64);
    sluice::BufferedReader br(inner, rbuf);
    std::vector<std::byte> primed(4);
    (void)br.read_some(std::span<std::byte>(primed));

    sluice::MemoryWriter writer;
    std::vector<std::byte> scratch(8);
    sluice::CopyStats st{};
    auto res = sluice::copy_all(br, writer, std::span<std::byte>(scratch),
                                sluice::CopyLimit::unlimited(), &st);
    SLUICE_CHECK(res.has_value());
    SLUICE_CHECK(res.value() == 12);
    SLUICE_CHECK(eq("456789ABCDEF", writer.bytes()));
    SLUICE_CHECK(st.buffered_fast_path_bytes == 12);
    SLUICE_CHECK(st.buffered_fast_path_calls >= 1);
    SLUICE_CHECK(st.scratch_path_calls >= 1); // the EOF probe
    SLUICE_CHECK(st.scratch_path_bytes == 0); // nothing served from scratch
}

SLUICE_TEST_CASE(copy_stats_nothing_limit_counts_neither_path) {
    CountingReader inner("0123456789ABCDEF");
    std::vector<std::byte> rbuf(64);
    sluice::BufferedReader br(inner, rbuf);
    std::vector<std::byte> primed(4);
    (void)br.read_some(std::span<std::byte>(primed));

    sluice::MemoryWriter writer;
    std::vector<std::byte> scratch(8);
    sluice::CopyStats st{};
    auto res = sluice::copy_all(br, writer, std::span<std::byte>(scratch),
                                sluice::CopyLimit::nothing(), &st);
    SLUICE_CHECK(res.has_value());
    SLUICE_CHECK(res.value() == 0);
    SLUICE_CHECK(st.buffered_fast_path_calls == 0);
    SLUICE_CHECK(st.scratch_path_calls == 0);
    SLUICE_CHECK(st.limit_stops == 1); // nothing() still counts the limit stop
}

SLUICE_TEST_CASE(copy_stats_bytes_read_and_written_remain_correct) {
    CountingReader inner("0123456789ABCDEF");
    std::vector<std::byte> rbuf(64);
    sluice::BufferedReader br(inner, rbuf);
    std::vector<std::byte> primed(4);
    (void)br.read_some(std::span<std::byte>(primed));

    sluice::MemoryWriter writer;
    std::vector<std::byte> scratch(8);
    sluice::CopyStats st{};
    auto res = sluice::copy_all(br, writer, std::span<std::byte>(scratch),
                                sluice::CopyLimit::unlimited(), &st);
    SLUICE_CHECK(res.has_value());
    SLUICE_CHECK(res.value() == 12);
    SLUICE_CHECK(st.bytes_read == 12);
    SLUICE_CHECK(st.bytes_written == 12);
    // fast + scratch bytes sum to the total written.
    SLUICE_CHECK(st.buffered_fast_path_bytes + st.scratch_path_bytes == st.bytes_written);
}

SLUICE_MAIN()
