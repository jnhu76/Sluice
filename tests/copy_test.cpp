// Tests for copy_all: exact byte copy, total returned, error propagation from
// both sides, EOF handling, and the no-internal-alloc scratch variant.
#include "harness.hpp"

#include <sluice/copy.hpp>
#include <sluice/fault.hpp>
#include <sluice/limit.hpp>
#include <sluice/measurement.hpp>
#include <sluice/reader.hpp>
#include <sluice/writer.hpp>

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

} // namespace

SLUICE_TEST_CASE(copy_all_copies_exact_bytes_with_scratch) {
    auto reader = sluice::MemoryReader::from_string("the quick brown fox");
    sluice::MemoryWriter writer;
    std::vector<std::byte> scratch(4);

    auto res = sluice::copy_all(reader, writer, std::span<std::byte>(scratch));
    SLUICE_CHECK(res.has_value());
    SLUICE_CHECK(res.value() == 19);
    SLUICE_CHECK(eq("the quick brown fox", writer.bytes()));
}

SLUICE_TEST_CASE(copy_all_overload_without_scratch_works) {
    auto reader = sluice::MemoryReader::from_string("abcdef");
    sluice::MemoryWriter writer;

    auto res = sluice::copy_all(reader, writer);
    SLUICE_CHECK(res.has_value());
    SLUICE_CHECK(res.value() == 6);
    SLUICE_CHECK(eq("abcdef", writer.bytes()));
}

SLUICE_TEST_CASE(copy_all_returns_total_bytes_copied) {
    auto reader = sluice::MemoryReader::from_string("0123456789ABCDEF");
    sluice::MemoryWriter writer;
    std::vector<std::byte> scratch(3);

    auto res = sluice::copy_all(reader, writer, std::span<std::byte>(scratch));
    SLUICE_CHECK(res.has_value());
    SLUICE_CHECK(res.value() == 16);
}

SLUICE_TEST_CASE(copy_all_handles_empty_source_as_zero_bytes) {
    auto reader = sluice::MemoryReader::from_string("");
    sluice::MemoryWriter writer;
    std::vector<std::byte> scratch(8);

    auto res = sluice::copy_all(reader, writer, std::span<std::byte>(scratch));
    SLUICE_CHECK(res.has_value());
    SLUICE_CHECK(res.value() == 0);
    SLUICE_CHECK(writer.bytes().empty());
}

SLUICE_TEST_CASE(copy_all_propagates_reader_error) {
    sluice::MemoryReader mem = sluice::MemoryReader::from_string("abcdef");
    sluice::FaultPlan plan;
    plan.max_read_size = 1;
    plan.fail_after_read_calls = 3;
    plan.error = sluice::IoError{sluice::IoError::Code::canceled};
    sluice::FaultReader reader(mem, plan);
    sluice::MemoryWriter writer;
    std::vector<std::byte> scratch(8);

    auto res = sluice::copy_all(reader, writer, std::span<std::byte>(scratch));
    SLUICE_CHECK(!res.has_value());
    SLUICE_CHECK(res.error().code == sluice::IoError::Code::canceled);
}

SLUICE_TEST_CASE(copy_all_propagates_writer_error) {
    auto reader = sluice::MemoryReader::from_string("abcdef");
    sluice::MemoryWriter sink;
    sluice::FaultPlan plan;
    plan.max_write_size = 2;
    plan.fail_after_write_calls = 1;
    plan.error = sluice::IoError{sluice::IoError::Code::no_space};
    sluice::FaultWriter writer(sink, plan);
    std::vector<std::byte> scratch(8);

    auto res = sluice::copy_all(reader, writer, std::span<std::byte>(scratch));
    SLUICE_CHECK(!res.has_value());
    SLUICE_CHECK(res.error().code == sluice::IoError::Code::no_space);
}

SLUICE_TEST_CASE(copy_all_does_not_treat_short_writes_as_success) {
    // Inner writer always writes exactly 1 byte per call regardless of input.
    struct OneByteWriter final : sluice::Writer {
        sluice::MemoryWriter mem;
        sluice::Result<std::size_t> write_some(std::span<const std::byte> src) override {
            if (src.empty())
                return std::size_t{0};
            auto r = mem.write_some(src.first(1));
            if (!r.has_value())
                return make_unexpected<std::size_t>(r.error());
            return std::size_t{1};
        }
        sluice::Result<void> flush() override { return {}; }
    };
    auto reader = sluice::MemoryReader::from_string("hello");
    OneByteWriter writer;
    std::vector<std::byte> scratch(8);

    auto res = sluice::copy_all(reader, writer, std::span<std::byte>(scratch));
    SLUICE_CHECK(res.has_value());
    SLUICE_CHECK(res.value() == 5);
    SLUICE_CHECK(eq("hello", writer.mem.bytes()));
}

SLUICE_TEST_CASE(copy_all_handles_eof_cleanly) {
    // Larger than scratch: forces multiple read loops terminating in EOF.
    std::string big(1000, 'x');
    auto reader = sluice::MemoryReader::from_string(big);
    sluice::MemoryWriter writer;
    std::vector<std::byte> scratch(7);

    auto res = sluice::copy_all(reader, writer, std::span<std::byte>(scratch));
    SLUICE_CHECK(res.has_value());
    SLUICE_CHECK(res.value() == 1000);
    SLUICE_CHECK(writer.bytes().size() == 1000);
}

// ---------- limited copy_all (CopyLimit) ----------

namespace {

// Reader that records the largest buffer it was ever asked to fill, so tests can
// assert the limit clamps the requested read size.
class ObservingReader final : public sluice::Reader {
  public:
    sluice::MemoryReader mem;
    std::size_t max_requested = 0;
    explicit ObservingReader(std::string_view s) : mem(sluice::MemoryReader::from_string(s)) {}
    sluice::Result<std::size_t> read_some(std::span<std::byte> dst) override {
        max_requested = std::max(max_requested, dst.size());
        return mem.read_some(dst);
    }
};

} // namespace

SLUICE_TEST_CASE(limited_copy_unlimited_preserves_old_behavior) {
    auto reader = sluice::MemoryReader::from_string("abcdef");
    sluice::MemoryWriter writer;
    std::vector<std::byte> scratch(64);
    auto res = sluice::copy_all(reader, writer, std::span<std::byte>(scratch),
                                sluice::CopyLimit::unlimited());
    SLUICE_CHECK(res.has_value());
    SLUICE_CHECK(res.value() == 6);
    SLUICE_CHECK(eq("abcdef", writer.bytes()));
}

SLUICE_TEST_CASE(limited_copy_nothing_copies_zero_and_does_not_touch_endpoints) {
    // Reader is configured to fail on the very first read; if nothing() touched
    // the reader at all, this would surface the error instead of success.
    sluice::MemoryReader mem = sluice::MemoryReader::from_string("abcdef");
    sluice::FaultPlan plan;
    plan.fail_after_read_calls = 0;
    plan.error = sluice::IoError{sluice::IoError::Code::canceled};
    sluice::FaultReader reader(mem, plan);

    sluice::MemoryWriter sink;
    sluice::FaultPlan wplan;
    wplan.fail_after_write_calls = 0;
    wplan.error = sluice::IoError{sluice::IoError::Code::no_space};
    sluice::FaultWriter writer(sink, wplan);

    std::vector<std::byte> scratch(8);
    auto res = sluice::copy_all(reader, writer, std::span<std::byte>(scratch),
                                sluice::CopyLimit::nothing());
    SLUICE_CHECK(res.has_value());
    SLUICE_CHECK(res.value() == 0);
    SLUICE_CHECK(sink.bytes().empty()); // the underlying sink, not the FaultWriter
}

SLUICE_TEST_CASE(limited_copy_less_than_source) {
    auto reader = sluice::MemoryReader::from_string("abcdef");
    sluice::MemoryWriter writer;
    std::vector<std::byte> scratch(64);
    auto res = sluice::copy_all(reader, writer, std::span<std::byte>(scratch),
                                sluice::CopyLimit::bytes(3));
    SLUICE_CHECK(res.has_value());
    SLUICE_CHECK(res.value() == 3);
    SLUICE_CHECK(eq("abc", writer.bytes()));
}

SLUICE_TEST_CASE(limited_copy_equal_to_source) {
    auto reader = sluice::MemoryReader::from_string("abcdef");
    sluice::MemoryWriter writer;
    std::vector<std::byte> scratch(64);
    auto res = sluice::copy_all(reader, writer, std::span<std::byte>(scratch),
                                sluice::CopyLimit::bytes(6));
    SLUICE_CHECK(res.has_value());
    SLUICE_CHECK(res.value() == 6);
    SLUICE_CHECK(eq("abcdef", writer.bytes()));
}

SLUICE_TEST_CASE(limited_copy_greater_than_source_eof_is_success) {
    auto reader = sluice::MemoryReader::from_string("abc");
    sluice::MemoryWriter writer;
    std::vector<std::byte> scratch(64);
    auto res = sluice::copy_all(reader, writer, std::span<std::byte>(scratch),
                                sluice::CopyLimit::bytes(10));
    SLUICE_CHECK(res.has_value());
    SLUICE_CHECK(res.value() == 3); // EOF before the limit
    SLUICE_CHECK(eq("abc", writer.bytes()));
}

SLUICE_TEST_CASE(limited_copy_never_asks_reader_for_more_than_remaining) {
    ObservingReader reader("abcdef");
    sluice::MemoryWriter writer;
    std::vector<std::byte> scratch(64);
    auto res = sluice::copy_all(reader, writer, std::span<std::byte>(scratch),
                                sluice::CopyLimit::bytes(3));
    SLUICE_CHECK(res.has_value() && res.value() == 3);
    // Each read request must be <= the remaining limit (3), never the full 64.
    SLUICE_CHECK(reader.max_requested <= 3);
}

SLUICE_TEST_CASE(limited_copy_scratch_larger_than_limit_clamps_request) {
    ObservingReader reader("abcdef");
    sluice::MemoryWriter writer;
    std::vector<std::byte> scratch(64);
    auto res = sluice::copy_all(reader, writer, std::span<std::byte>(scratch),
                                sluice::CopyLimit::bytes(3));
    SLUICE_CHECK(res.has_value() && res.value() == 3);
    SLUICE_CHECK(reader.max_requested <= 3); // limit wins over scratch.size
}

SLUICE_TEST_CASE(limited_copy_scratch_smaller_than_limit) {
    auto reader = sluice::MemoryReader::from_string("abcdef");
    sluice::MemoryWriter writer;
    std::vector<std::byte> scratch(2);
    auto res = sluice::copy_all(reader, writer, std::span<std::byte>(scratch),
                                sluice::CopyLimit::bytes(5));
    SLUICE_CHECK(res.has_value());
    SLUICE_CHECK(res.value() == 5);
    SLUICE_CHECK(eq("abcde", writer.bytes()));
}

SLUICE_TEST_CASE(limited_copy_empty_scratch_nonzero_limit_is_invalid_state) {
    auto reader = sluice::MemoryReader::from_string("abcdef");
    sluice::MemoryWriter writer;
    std::vector<std::byte> empty;
    auto res =
        sluice::copy_all(reader, writer, std::span<std::byte>(empty), sluice::CopyLimit::bytes(1));
    SLUICE_CHECK(!res.has_value());
    SLUICE_CHECK(res.error().code == sluice::IoError::Code::invalid_state);
}

SLUICE_TEST_CASE(limited_copy_empty_scratch_nothing_is_success_zero) {
    auto reader = sluice::MemoryReader::from_string("abcdef");
    sluice::MemoryWriter writer;
    std::vector<std::byte> empty;
    auto res =
        sluice::copy_all(reader, writer, std::span<std::byte>(empty), sluice::CopyLimit::nothing());
    SLUICE_CHECK(res.has_value());
    SLUICE_CHECK(res.value() == 0);
}

SLUICE_TEST_CASE(limited_copy_unlimited_empty_scratch_is_invalid_state) {
    auto reader = sluice::MemoryReader::from_string("abcdef");
    sluice::MemoryWriter writer;
    std::vector<std::byte> empty;
    auto res = sluice::copy_all(reader, writer, std::span<std::byte>(empty),
                                sluice::CopyLimit::unlimited());
    SLUICE_CHECK(!res.has_value());
    SLUICE_CHECK(res.error().code == sluice::IoError::Code::invalid_state);
}

SLUICE_TEST_CASE(limited_copy_reader_error_propagates) {
    sluice::MemoryReader mem = sluice::MemoryReader::from_string("abcdef");
    sluice::FaultPlan plan;
    plan.max_read_size = 1;
    plan.fail_after_read_calls = 2;
    plan.error = sluice::IoError{sluice::IoError::Code::canceled};
    sluice::FaultReader reader(mem, plan);
    sluice::MemoryWriter writer;
    std::vector<std::byte> scratch(8);
    auto res = sluice::copy_all(reader, writer, std::span<std::byte>(scratch),
                                sluice::CopyLimit::bytes(6));
    SLUICE_CHECK(!res.has_value());
    SLUICE_CHECK(res.error().code == sluice::IoError::Code::canceled);
}

SLUICE_TEST_CASE(limited_copy_writer_error_propagates) {
    auto reader = sluice::MemoryReader::from_string("abcdef");
    sluice::MemoryWriter sink;
    sluice::FaultPlan plan;
    plan.max_write_size = 2;
    plan.fail_after_write_calls = 1;
    plan.error = sluice::IoError{sluice::IoError::Code::no_space};
    sluice::FaultWriter writer(sink, plan);
    std::vector<std::byte> scratch(8);
    auto res = sluice::copy_all(reader, writer, std::span<std::byte>(scratch),
                                sluice::CopyLimit::bytes(6));
    SLUICE_CHECK(!res.has_value());
    SLUICE_CHECK(res.error().code == sluice::IoError::Code::no_space);
}

SLUICE_TEST_CASE(limited_copy_convenience_overload_without_scratch) {
    auto reader = sluice::MemoryReader::from_string("abcdef");
    sluice::MemoryWriter writer;
    auto res = sluice::copy_all(reader, writer, sluice::CopyLimit::bytes(3));
    SLUICE_CHECK(res.has_value());
    SLUICE_CHECK(res.value() == 3);
    SLUICE_CHECK(eq("abc", writer.bytes()));
}

// ---------- CopyStats attachment + stop reasons (CPPIO-CORE-004) ----------

SLUICE_TEST_CASE(copy_stats_eof_stop_and_byte_counts) {
    auto reader = sluice::MemoryReader::from_string("abcdef");
    sluice::MemoryWriter writer;
    std::vector<std::byte> scratch(64);
    sluice::CopyStats stats{};
    auto res = sluice::copy_all(reader, writer, std::span<std::byte>(scratch),
                                sluice::CopyLimit::unlimited(), &stats);
    SLUICE_CHECK(res.has_value() && res.value() == 6);
    SLUICE_CHECK(stats.copy_calls == 1);
    SLUICE_CHECK(stats.bytes_read == 6);
    SLUICE_CHECK(stats.bytes_written == 6);
    SLUICE_CHECK(stats.eof_stops == 1);
    SLUICE_CHECK(stats.limit_stops == 0);
    SLUICE_CHECK(stats.reader_error_stops == 0);
    SLUICE_CHECK(stats.writer_error_stops == 0);
}

SLUICE_TEST_CASE(copy_stats_limit_stop_includes_nothing) {
    auto reader = sluice::MemoryReader::from_string("abcdef");
    sluice::MemoryWriter writer;
    std::vector<std::byte> scratch(64);
    sluice::CopyStats stats{};

    // nothing() must stop with a limit stop and zero reader/writer traffic.
    auto res = sluice::copy_all(reader, writer, std::span<std::byte>(scratch),
                                sluice::CopyLimit::nothing(), &stats);
    SLUICE_CHECK(res.has_value() && res.value() == 0);
    SLUICE_CHECK(stats.limit_stops == 1);
    SLUICE_CHECK(stats.bytes_read == 0);
    SLUICE_CHECK(stats.bytes_written == 0);
    SLUICE_CHECK(stats.eof_stops == 0);

    // A real byte limit that is hit (not EOF-first) also counts as a limit stop.
    sluice::CopyStats stats2{};
    auto reader2 = sluice::MemoryReader::from_string("abcdef");
    sluice::MemoryWriter writer2;
    auto res2 = sluice::copy_all(reader2, writer2, std::span<std::byte>(scratch),
                                 sluice::CopyLimit::bytes(3), &stats2);
    SLUICE_CHECK(res2.has_value() && res2.value() == 3);
    SLUICE_CHECK(stats2.limit_stops == 1);
    SLUICE_CHECK(stats2.eof_stops == 0);
    SLUICE_CHECK(stats2.bytes_read == 3);
}

SLUICE_TEST_CASE(copy_stats_reader_error_stop) {
    sluice::MemoryReader mem = sluice::MemoryReader::from_string("abcdef");
    sluice::FaultPlan plan;
    plan.max_read_size = 1;
    plan.fail_after_read_calls = 2;
    plan.error = sluice::IoError{sluice::IoError::Code::canceled};
    sluice::FaultReader reader(mem, plan);
    sluice::MemoryWriter writer;
    std::vector<std::byte> scratch(8);
    sluice::CopyStats stats{};
    auto res = sluice::copy_all(reader, writer, std::span<std::byte>(scratch),
                                sluice::CopyLimit::unlimited(), &stats);
    SLUICE_CHECK(!res.has_value());
    SLUICE_CHECK(stats.reader_error_stops == 1);
    SLUICE_CHECK(stats.writer_error_stops == 0);
    SLUICE_CHECK(stats.eof_stops == 0);
    SLUICE_CHECK(stats.limit_stops == 0);
}

SLUICE_TEST_CASE(copy_stats_writer_error_stop) {
    auto reader = sluice::MemoryReader::from_string("abcdef");
    sluice::MemoryWriter sink;
    sluice::FaultPlan plan;
    plan.max_write_size = 2;
    plan.fail_after_write_calls = 1;
    plan.error = sluice::IoError{sluice::IoError::Code::no_space};
    sluice::FaultWriter writer(sink, plan);
    std::vector<std::byte> scratch(8);
    sluice::CopyStats stats{};
    auto res = sluice::copy_all(reader, writer, std::span<std::byte>(scratch),
                                sluice::CopyLimit::unlimited(), &stats);
    SLUICE_CHECK(!res.has_value());
    SLUICE_CHECK(stats.writer_error_stops == 1);
    SLUICE_CHECK(stats.reader_error_stops == 0);
    SLUICE_CHECK(stats.eof_stops == 0);
}

SLUICE_TEST_CASE(copy_stats_null_is_zero_overhead) {
    auto reader = sluice::MemoryReader::from_string("abc");
    sluice::MemoryWriter writer;
    std::vector<std::byte> scratch(8);
    auto res = sluice::copy_all(reader, writer, std::span<std::byte>(scratch),
                                sluice::CopyLimit::unlimited(), nullptr);
    SLUICE_CHECK(res.has_value() && res.value() == 3);
}

SLUICE_TEST_CASE(stream_to_delegates_copy_stats) {
    // Reader::stream_to(writer, scratch, limit) must forward CopyStats to
    // copy_all (no stats drift between the two paths).
    sluice::MemoryReader reader = sluice::MemoryReader::from_string("abcdef");
    sluice::MemoryWriter writer;
    std::vector<std::byte> scratch(64);
    sluice::CopyStats stats{};
    auto res = reader.stream_to(writer, std::span<std::byte>(scratch), sluice::CopyLimit::bytes(3),
                                &stats);
    SLUICE_CHECK(res.has_value() && res.value() == 3);
    SLUICE_CHECK(stats.copy_calls == 1);
    SLUICE_CHECK(stats.limit_stops == 1);
    SLUICE_CHECK(stats.bytes_read == 3);
}

SLUICE_MAIN()
