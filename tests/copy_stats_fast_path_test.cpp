// Tests for CopyStats fast/scratch counters (CPPIO-CORE-006E). Verifies the
// buffered fast path and scratch path are counted separately and that
// bytes_read/bytes_written stay correct in mixed scenarios.
#include "harness.hpp"

#include <cppio/buffer.hpp>
#include <cppio/copy.hpp>
#include <cppio/fault.hpp>
#include <cppio/limit.hpp>
#include <cppio/measurement.hpp>
#include <cppio/reader.hpp>
#include <cppio/writer.hpp>

#include <cstring>
#include <span>
#include <vector>

namespace {

class CountingReader final : public cppio::Reader {
public:
    cppio::MemoryReader mem;
    int calls = 0;
    explicit CountingReader(std::string_view s) : mem(cppio::MemoryReader::from_string(s)) {}
    cppio::Result<std::size_t> read_some(std::span<std::byte> dst) override {
        ++calls;
        return mem.read_some(dst);
    }
};

bool eq(std::string_view s, const std::vector<std::byte>& b) {
    return b.size() == s.size() && std::memcmp(s.data(), b.data(), s.size()) == 0;
}

}  // namespace

CPPIO_TEST_CASE(copy_stats_buffered_fast_path_increments_fast_counters) {
    CountingReader inner("0123456789ABCDEF");
    std::vector<std::byte> rbuf(64);
    cppio::BufferedReader br(inner, rbuf);
    std::vector<std::byte> primed(4);
    (void)br.read_some(std::span<std::byte>(primed));  // 12 buffered remain

    cppio::MemoryWriter writer;
    std::vector<std::byte> scratch(8);
    cppio::CopyStats st{};
    auto res = cppio::copy_all(br, writer, std::span<std::byte>(scratch),
                               cppio::CopyLimit::bytes(5), &st);  // stop at limit, no scratch
    CPPIO_CHECK(res.has_value());
    CPPIO_CHECK(res.value() == 5);
    CPPIO_CHECK(st.buffered_fast_path_calls >= 1);
    CPPIO_CHECK(st.buffered_fast_path_bytes == 5);
    CPPIO_CHECK(st.scratch_path_calls == 0);
    CPPIO_CHECK(st.scratch_path_bytes == 0);
}

CPPIO_TEST_CASE(copy_stats_scratch_path_increments_scratch_counters) {
    // Plain MemoryReader is not BufferedReadable: pure scratch path.
    auto reader = cppio::MemoryReader::from_string("hello world");
    cppio::MemoryWriter writer;
    std::vector<std::byte> scratch(4);
    cppio::CopyStats st{};
    auto res = cppio::copy_all(reader, writer, std::span<std::byte>(scratch),
                               cppio::CopyLimit::unlimited(), &st);
    CPPIO_CHECK(res.has_value());
    CPPIO_CHECK(res.value() == 11);
    CPPIO_CHECK(eq("hello world", writer.bytes()));
    CPPIO_CHECK(st.buffered_fast_path_calls == 0);
    CPPIO_CHECK(st.buffered_fast_path_bytes == 0);
    CPPIO_CHECK(st.scratch_path_calls >= 1);
    CPPIO_CHECK(st.scratch_path_bytes == 11);
}

CPPIO_TEST_CASE(copy_stats_mixed_buffered_and_scratch_counts_both) {
    // Prime 4 bytes out of 16, leaving 12 buffered. copy_all drains 12 via the
    // fast path, then the scratch path probes EOF (one scratch call, 0 bytes).
    CountingReader inner("0123456789ABCDEF");
    std::vector<std::byte> rbuf(64);
    cppio::BufferedReader br(inner, rbuf);
    std::vector<std::byte> primed(4);
    (void)br.read_some(std::span<std::byte>(primed));

    cppio::MemoryWriter writer;
    std::vector<std::byte> scratch(8);
    cppio::CopyStats st{};
    auto res = cppio::copy_all(br, writer, std::span<std::byte>(scratch),
                               cppio::CopyLimit::unlimited(), &st);
    CPPIO_CHECK(res.has_value());
    CPPIO_CHECK(res.value() == 12);
    CPPIO_CHECK(eq("456789ABCDEF", writer.bytes()));
    CPPIO_CHECK(st.buffered_fast_path_bytes == 12);
    CPPIO_CHECK(st.buffered_fast_path_calls >= 1);
    CPPIO_CHECK(st.scratch_path_calls >= 1);  // the EOF probe
    CPPIO_CHECK(st.scratch_path_bytes == 0);  // nothing served from scratch
}

CPPIO_TEST_CASE(copy_stats_nothing_limit_counts_neither_path) {
    CountingReader inner("0123456789ABCDEF");
    std::vector<std::byte> rbuf(64);
    cppio::BufferedReader br(inner, rbuf);
    std::vector<std::byte> primed(4);
    (void)br.read_some(std::span<std::byte>(primed));

    cppio::MemoryWriter writer;
    std::vector<std::byte> scratch(8);
    cppio::CopyStats st{};
    auto res = cppio::copy_all(br, writer, std::span<std::byte>(scratch),
                               cppio::CopyLimit::nothing(), &st);
    CPPIO_CHECK(res.has_value());
    CPPIO_CHECK(res.value() == 0);
    CPPIO_CHECK(st.buffered_fast_path_calls == 0);
    CPPIO_CHECK(st.scratch_path_calls == 0);
    CPPIO_CHECK(st.limit_stops == 1);  // nothing() still counts the limit stop
}

CPPIO_TEST_CASE(copy_stats_bytes_read_and_written_remain_correct) {
    CountingReader inner("0123456789ABCDEF");
    std::vector<std::byte> rbuf(64);
    cppio::BufferedReader br(inner, rbuf);
    std::vector<std::byte> primed(4);
    (void)br.read_some(std::span<std::byte>(primed));

    cppio::MemoryWriter writer;
    std::vector<std::byte> scratch(8);
    cppio::CopyStats st{};
    auto res = cppio::copy_all(br, writer, std::span<std::byte>(scratch),
                               cppio::CopyLimit::unlimited(), &st);
    CPPIO_CHECK(res.has_value());
    CPPIO_CHECK(res.value() == 12);
    CPPIO_CHECK(st.bytes_read == 12);
    CPPIO_CHECK(st.bytes_written == 12);
    // fast + scratch bytes sum to the total written.
    CPPIO_CHECK(st.buffered_fast_path_bytes + st.scratch_path_bytes == st.bytes_written);
}

CPPIO_MAIN()
