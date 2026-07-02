// Tests for copy_all: exact byte copy, total returned, error propagation from
// both sides, EOF handling, and the no-internal-alloc scratch variant.
#include "harness.hpp"

#include <cppio/copy.hpp>
#include <cppio/fault.hpp>
#include <cppio/limit.hpp>
#include <cppio/measurement.hpp>
#include <cppio/reader.hpp>
#include <cppio/writer.hpp>

#include <algorithm>
#include <cstring>
#include <cstdint>
#include <span>
#include <vector>

namespace {

bool eq(std::string_view s, const std::vector<std::byte>& b) {
    return b.size() == s.size() && std::memcmp(s.data(), b.data(), s.size()) == 0;
}
[[maybe_unused]] std::span<const std::byte> sb(std::string_view s) {
    return std::as_bytes(std::span(s.data(), s.size()));
}

}  // namespace

CPPIO_TEST_CASE(copy_all_copies_exact_bytes_with_scratch) {
    auto reader = cppio::MemoryReader::from_string("the quick brown fox");
    cppio::MemoryWriter writer;
    std::vector<std::byte> scratch(4);

    auto res = cppio::copy_all(reader, writer, std::span<std::byte>(scratch));
    CPPIO_CHECK(res.has_value());
    CPPIO_CHECK(res.value() == 19);
    CPPIO_CHECK(eq("the quick brown fox", writer.bytes()));
}

CPPIO_TEST_CASE(copy_all_overload_without_scratch_works) {
    auto reader = cppio::MemoryReader::from_string("abcdef");
    cppio::MemoryWriter writer;

    auto res = cppio::copy_all(reader, writer);
    CPPIO_CHECK(res.has_value());
    CPPIO_CHECK(res.value() == 6);
    CPPIO_CHECK(eq("abcdef", writer.bytes()));
}

CPPIO_TEST_CASE(copy_all_returns_total_bytes_copied) {
    auto reader = cppio::MemoryReader::from_string("0123456789ABCDEF");
    cppio::MemoryWriter writer;
    std::vector<std::byte> scratch(3);

    auto res = cppio::copy_all(reader, writer, std::span<std::byte>(scratch));
    CPPIO_CHECK(res.has_value());
    CPPIO_CHECK(res.value() == 16);
}

CPPIO_TEST_CASE(copy_all_handles_empty_source_as_zero_bytes) {
    auto reader = cppio::MemoryReader::from_string("");
    cppio::MemoryWriter writer;
    std::vector<std::byte> scratch(8);

    auto res = cppio::copy_all(reader, writer, std::span<std::byte>(scratch));
    CPPIO_CHECK(res.has_value());
    CPPIO_CHECK(res.value() == 0);
    CPPIO_CHECK(writer.bytes().empty());
}

CPPIO_TEST_CASE(copy_all_propagates_reader_error) {
    cppio::MemoryReader mem = cppio::MemoryReader::from_string("abcdef");
    cppio::FaultPlan plan;
    plan.max_read_size = 1;
    plan.fail_after_read_calls = 3;
    plan.error = cppio::IoError{cppio::IoError::Code::canceled};
    cppio::FaultReader reader(mem, plan);
    cppio::MemoryWriter writer;
    std::vector<std::byte> scratch(8);

    auto res = cppio::copy_all(reader, writer, std::span<std::byte>(scratch));
    CPPIO_CHECK(!res.has_value());
    CPPIO_CHECK(res.error().code == cppio::IoError::Code::canceled);
}

CPPIO_TEST_CASE(copy_all_propagates_writer_error) {
    auto reader = cppio::MemoryReader::from_string("abcdef");
    cppio::MemoryWriter sink;
    cppio::FaultPlan plan;
    plan.max_write_size = 2;
    plan.fail_after_write_calls = 1;
    plan.error = cppio::IoError{cppio::IoError::Code::no_space};
    cppio::FaultWriter writer(sink, plan);
    std::vector<std::byte> scratch(8);

    auto res = cppio::copy_all(reader, writer, std::span<std::byte>(scratch));
    CPPIO_CHECK(!res.has_value());
    CPPIO_CHECK(res.error().code == cppio::IoError::Code::no_space);
}

CPPIO_TEST_CASE(copy_all_does_not_treat_short_writes_as_success) {
    // Inner writer always writes exactly 1 byte per call regardless of input.
    struct OneByteWriter final : cppio::Writer {
        cppio::MemoryWriter mem;
        cppio::Result<std::size_t> write_some(std::span<const std::byte> src) override {
            if (src.empty()) return std::size_t{0};
            auto r = mem.write_some(src.first(1));
            if (!r.has_value()) return make_unexpected<std::size_t>(r.error());
            return std::size_t{1};
        }
        cppio::Result<void> flush() override { return {}; }
    };
    auto reader = cppio::MemoryReader::from_string("hello");
    OneByteWriter writer;
    std::vector<std::byte> scratch(8);

    auto res = cppio::copy_all(reader, writer, std::span<std::byte>(scratch));
    CPPIO_CHECK(res.has_value());
    CPPIO_CHECK(res.value() == 5);
    CPPIO_CHECK(eq("hello", writer.mem.bytes()));
}

CPPIO_TEST_CASE(copy_all_handles_eof_cleanly) {
    // Larger than scratch: forces multiple read loops terminating in EOF.
    std::string big(1000, 'x');
    auto reader = cppio::MemoryReader::from_string(big);
    cppio::MemoryWriter writer;
    std::vector<std::byte> scratch(7);

    auto res = cppio::copy_all(reader, writer, std::span<std::byte>(scratch));
    CPPIO_CHECK(res.has_value());
    CPPIO_CHECK(res.value() == 1000);
    CPPIO_CHECK(writer.bytes().size() == 1000);
}

// ---------- limited copy_all (CopyLimit) ----------

namespace {

// Reader that records the largest buffer it was ever asked to fill, so tests can
// assert the limit clamps the requested read size.
class ObservingReader final : public cppio::Reader {
public:
    cppio::MemoryReader mem;
    std::size_t max_requested = 0;
    explicit ObservingReader(std::string_view s) : mem(cppio::MemoryReader::from_string(s)) {}
    cppio::Result<std::size_t> read_some(std::span<std::byte> dst) override {
        max_requested = std::max(max_requested, dst.size());
        return mem.read_some(dst);
    }
};

}  // namespace

CPPIO_TEST_CASE(limited_copy_unlimited_preserves_old_behavior) {
    auto reader = cppio::MemoryReader::from_string("abcdef");
    cppio::MemoryWriter writer;
    std::vector<std::byte> scratch(64);
    auto res = cppio::copy_all(reader, writer, std::span<std::byte>(scratch),
                               cppio::CopyLimit::unlimited());
    CPPIO_CHECK(res.has_value());
    CPPIO_CHECK(res.value() == 6);
    CPPIO_CHECK(eq("abcdef", writer.bytes()));
}

CPPIO_TEST_CASE(limited_copy_nothing_copies_zero_and_does_not_touch_endpoints) {
    // Reader is configured to fail on the very first read; if nothing() touched
    // the reader at all, this would surface the error instead of success.
    cppio::MemoryReader mem = cppio::MemoryReader::from_string("abcdef");
    cppio::FaultPlan plan;
    plan.fail_after_read_calls = 0;
    plan.error = cppio::IoError{cppio::IoError::Code::canceled};
    cppio::FaultReader reader(mem, plan);

    cppio::MemoryWriter sink;
    cppio::FaultPlan wplan;
    wplan.fail_after_write_calls = 0;
    wplan.error = cppio::IoError{cppio::IoError::Code::no_space};
    cppio::FaultWriter writer(sink, wplan);

    std::vector<std::byte> scratch(8);
    auto res = cppio::copy_all(reader, writer, std::span<std::byte>(scratch),
                               cppio::CopyLimit::nothing());
    CPPIO_CHECK(res.has_value());
    CPPIO_CHECK(res.value() == 0);
    CPPIO_CHECK(sink.bytes().empty());  // the underlying sink, not the FaultWriter
}

CPPIO_TEST_CASE(limited_copy_less_than_source) {
    auto reader = cppio::MemoryReader::from_string("abcdef");
    cppio::MemoryWriter writer;
    std::vector<std::byte> scratch(64);
    auto res = cppio::copy_all(reader, writer, std::span<std::byte>(scratch),
                               cppio::CopyLimit::bytes(3));
    CPPIO_CHECK(res.has_value());
    CPPIO_CHECK(res.value() == 3);
    CPPIO_CHECK(eq("abc", writer.bytes()));
}

CPPIO_TEST_CASE(limited_copy_equal_to_source) {
    auto reader = cppio::MemoryReader::from_string("abcdef");
    cppio::MemoryWriter writer;
    std::vector<std::byte> scratch(64);
    auto res = cppio::copy_all(reader, writer, std::span<std::byte>(scratch),
                               cppio::CopyLimit::bytes(6));
    CPPIO_CHECK(res.has_value());
    CPPIO_CHECK(res.value() == 6);
    CPPIO_CHECK(eq("abcdef", writer.bytes()));
}

CPPIO_TEST_CASE(limited_copy_greater_than_source_eof_is_success) {
    auto reader = cppio::MemoryReader::from_string("abc");
    cppio::MemoryWriter writer;
    std::vector<std::byte> scratch(64);
    auto res = cppio::copy_all(reader, writer, std::span<std::byte>(scratch),
                               cppio::CopyLimit::bytes(10));
    CPPIO_CHECK(res.has_value());
    CPPIO_CHECK(res.value() == 3);  // EOF before the limit
    CPPIO_CHECK(eq("abc", writer.bytes()));
}

CPPIO_TEST_CASE(limited_copy_never_asks_reader_for_more_than_remaining) {
    ObservingReader reader("abcdef");
    cppio::MemoryWriter writer;
    std::vector<std::byte> scratch(64);
    auto res = cppio::copy_all(reader, writer, std::span<std::byte>(scratch),
                               cppio::CopyLimit::bytes(3));
    CPPIO_CHECK(res.has_value() && res.value() == 3);
    // Each read request must be <= the remaining limit (3), never the full 64.
    CPPIO_CHECK(reader.max_requested <= 3);
}

CPPIO_TEST_CASE(limited_copy_scratch_larger_than_limit_clamps_request) {
    ObservingReader reader("abcdef");
    cppio::MemoryWriter writer;
    std::vector<std::byte> scratch(64);
    auto res = cppio::copy_all(reader, writer, std::span<std::byte>(scratch),
                               cppio::CopyLimit::bytes(3));
    CPPIO_CHECK(res.has_value() && res.value() == 3);
    CPPIO_CHECK(reader.max_requested <= 3);  // limit wins over scratch.size
}

CPPIO_TEST_CASE(limited_copy_scratch_smaller_than_limit) {
    auto reader = cppio::MemoryReader::from_string("abcdef");
    cppio::MemoryWriter writer;
    std::vector<std::byte> scratch(2);
    auto res = cppio::copy_all(reader, writer, std::span<std::byte>(scratch),
                               cppio::CopyLimit::bytes(5));
    CPPIO_CHECK(res.has_value());
    CPPIO_CHECK(res.value() == 5);
    CPPIO_CHECK(eq("abcde", writer.bytes()));
}

CPPIO_TEST_CASE(limited_copy_empty_scratch_nonzero_limit_is_invalid_state) {
    auto reader = cppio::MemoryReader::from_string("abcdef");
    cppio::MemoryWriter writer;
    std::vector<std::byte> empty;
    auto res = cppio::copy_all(reader, writer, std::span<std::byte>(empty),
                               cppio::CopyLimit::bytes(1));
    CPPIO_CHECK(!res.has_value());
    CPPIO_CHECK(res.error().code == cppio::IoError::Code::invalid_state);
}

CPPIO_TEST_CASE(limited_copy_empty_scratch_nothing_is_success_zero) {
    auto reader = cppio::MemoryReader::from_string("abcdef");
    cppio::MemoryWriter writer;
    std::vector<std::byte> empty;
    auto res = cppio::copy_all(reader, writer, std::span<std::byte>(empty),
                               cppio::CopyLimit::nothing());
    CPPIO_CHECK(res.has_value());
    CPPIO_CHECK(res.value() == 0);
}

CPPIO_TEST_CASE(limited_copy_unlimited_empty_scratch_is_invalid_state) {
    auto reader = cppio::MemoryReader::from_string("abcdef");
    cppio::MemoryWriter writer;
    std::vector<std::byte> empty;
    auto res = cppio::copy_all(reader, writer, std::span<std::byte>(empty),
                               cppio::CopyLimit::unlimited());
    CPPIO_CHECK(!res.has_value());
    CPPIO_CHECK(res.error().code == cppio::IoError::Code::invalid_state);
}

CPPIO_TEST_CASE(limited_copy_reader_error_propagates) {
    cppio::MemoryReader mem = cppio::MemoryReader::from_string("abcdef");
    cppio::FaultPlan plan;
    plan.max_read_size = 1;
    plan.fail_after_read_calls = 2;
    plan.error = cppio::IoError{cppio::IoError::Code::canceled};
    cppio::FaultReader reader(mem, plan);
    cppio::MemoryWriter writer;
    std::vector<std::byte> scratch(8);
    auto res = cppio::copy_all(reader, writer, std::span<std::byte>(scratch),
                               cppio::CopyLimit::bytes(6));
    CPPIO_CHECK(!res.has_value());
    CPPIO_CHECK(res.error().code == cppio::IoError::Code::canceled);
}

CPPIO_TEST_CASE(limited_copy_writer_error_propagates) {
    auto reader = cppio::MemoryReader::from_string("abcdef");
    cppio::MemoryWriter sink;
    cppio::FaultPlan plan;
    plan.max_write_size = 2;
    plan.fail_after_write_calls = 1;
    plan.error = cppio::IoError{cppio::IoError::Code::no_space};
    cppio::FaultWriter writer(sink, plan);
    std::vector<std::byte> scratch(8);
    auto res = cppio::copy_all(reader, writer, std::span<std::byte>(scratch),
                               cppio::CopyLimit::bytes(6));
    CPPIO_CHECK(!res.has_value());
    CPPIO_CHECK(res.error().code == cppio::IoError::Code::no_space);
}

CPPIO_TEST_CASE(limited_copy_convenience_overload_without_scratch) {
    auto reader = cppio::MemoryReader::from_string("abcdef");
    cppio::MemoryWriter writer;
    auto res = cppio::copy_all(reader, writer, cppio::CopyLimit::bytes(3));
    CPPIO_CHECK(res.has_value());
    CPPIO_CHECK(res.value() == 3);
    CPPIO_CHECK(eq("abc", writer.bytes()));
}

// ---------- CopyStats attachment + stop reasons (CPPIO-CORE-004) ----------

CPPIO_TEST_CASE(copy_stats_eof_stop_and_byte_counts) {
    auto reader = cppio::MemoryReader::from_string("abcdef");
    cppio::MemoryWriter writer;
    std::vector<std::byte> scratch(64);
    cppio::CopyStats stats{};
    auto res = cppio::copy_all(reader, writer, std::span<std::byte>(scratch),
                               cppio::CopyLimit::unlimited(), &stats);
    CPPIO_CHECK(res.has_value() && res.value() == 6);
    CPPIO_CHECK(stats.copy_calls == 1);
    CPPIO_CHECK(stats.bytes_read == 6);
    CPPIO_CHECK(stats.bytes_written == 6);
    CPPIO_CHECK(stats.eof_stops == 1);
    CPPIO_CHECK(stats.limit_stops == 0);
    CPPIO_CHECK(stats.reader_error_stops == 0);
    CPPIO_CHECK(stats.writer_error_stops == 0);
}

CPPIO_TEST_CASE(copy_stats_limit_stop_includes_nothing) {
    auto reader = cppio::MemoryReader::from_string("abcdef");
    cppio::MemoryWriter writer;
    std::vector<std::byte> scratch(64);
    cppio::CopyStats stats{};

    // nothing() must stop with a limit stop and zero reader/writer traffic.
    auto res = cppio::copy_all(reader, writer, std::span<std::byte>(scratch),
                               cppio::CopyLimit::nothing(), &stats);
    CPPIO_CHECK(res.has_value() && res.value() == 0);
    CPPIO_CHECK(stats.limit_stops == 1);
    CPPIO_CHECK(stats.bytes_read == 0);
    CPPIO_CHECK(stats.bytes_written == 0);
    CPPIO_CHECK(stats.eof_stops == 0);

    // A real byte limit that is hit (not EOF-first) also counts as a limit stop.
    cppio::CopyStats stats2{};
    auto reader2 = cppio::MemoryReader::from_string("abcdef");
    cppio::MemoryWriter writer2;
    auto res2 = cppio::copy_all(reader2, writer2, std::span<std::byte>(scratch),
                                cppio::CopyLimit::bytes(3), &stats2);
    CPPIO_CHECK(res2.has_value() && res2.value() == 3);
    CPPIO_CHECK(stats2.limit_stops == 1);
    CPPIO_CHECK(stats2.eof_stops == 0);
    CPPIO_CHECK(stats2.bytes_read == 3);
}

CPPIO_TEST_CASE(copy_stats_reader_error_stop) {
    cppio::MemoryReader mem = cppio::MemoryReader::from_string("abcdef");
    cppio::FaultPlan plan;
    plan.max_read_size = 1;
    plan.fail_after_read_calls = 2;
    plan.error = cppio::IoError{cppio::IoError::Code::canceled};
    cppio::FaultReader reader(mem, plan);
    cppio::MemoryWriter writer;
    std::vector<std::byte> scratch(8);
    cppio::CopyStats stats{};
    auto res = cppio::copy_all(reader, writer, std::span<std::byte>(scratch),
                               cppio::CopyLimit::unlimited(), &stats);
    CPPIO_CHECK(!res.has_value());
    CPPIO_CHECK(stats.reader_error_stops == 1);
    CPPIO_CHECK(stats.writer_error_stops == 0);
    CPPIO_CHECK(stats.eof_stops == 0);
    CPPIO_CHECK(stats.limit_stops == 0);
}

CPPIO_TEST_CASE(copy_stats_writer_error_stop) {
    auto reader = cppio::MemoryReader::from_string("abcdef");
    cppio::MemoryWriter sink;
    cppio::FaultPlan plan;
    plan.max_write_size = 2;
    plan.fail_after_write_calls = 1;
    plan.error = cppio::IoError{cppio::IoError::Code::no_space};
    cppio::FaultWriter writer(sink, plan);
    std::vector<std::byte> scratch(8);
    cppio::CopyStats stats{};
    auto res = cppio::copy_all(reader, writer, std::span<std::byte>(scratch),
                               cppio::CopyLimit::unlimited(), &stats);
    CPPIO_CHECK(!res.has_value());
    CPPIO_CHECK(stats.writer_error_stops == 1);
    CPPIO_CHECK(stats.reader_error_stops == 0);
    CPPIO_CHECK(stats.eof_stops == 0);
}

CPPIO_TEST_CASE(copy_stats_null_is_zero_overhead) {
    auto reader = cppio::MemoryReader::from_string("abc");
    cppio::MemoryWriter writer;
    std::vector<std::byte> scratch(8);
    auto res = cppio::copy_all(reader, writer, std::span<std::byte>(scratch),
                               cppio::CopyLimit::unlimited(), nullptr);
    CPPIO_CHECK(res.has_value() && res.value() == 3);
}

CPPIO_TEST_CASE(stream_to_delegates_copy_stats) {
    // Reader::stream_to(writer, scratch, limit) must forward CopyStats to
    // copy_all (no stats drift between the two paths).
    cppio::MemoryReader reader = cppio::MemoryReader::from_string("abcdef");
    cppio::MemoryWriter writer;
    std::vector<std::byte> scratch(64);
    cppio::CopyStats stats{};
    auto res = reader.stream_to(writer, std::span<std::byte>(scratch),
                                cppio::CopyLimit::bytes(3), &stats);
    CPPIO_CHECK(res.has_value() && res.value() == 3);
    CPPIO_CHECK(stats.copy_calls == 1);
    CPPIO_CHECK(stats.limit_stops == 1);
    CPPIO_CHECK(stats.bytes_read == 3);
}

CPPIO_MAIN()
