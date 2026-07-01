// Tests for ObservedReader / ObservedWriter: stats accounting + transparent
// pass-through of data semantics. Backed by MemoryReader/MemoryWriter + Fault.
#include "harness.hpp"

#include <cppio/observed.hpp>
#include <cppio/fault.hpp>
#include <cppio/reader.hpp>
#include <cppio/writer.hpp>

#include <cstring>
#include <span>
#include <vector>

namespace {

std::span<const std::byte> sb(std::string_view s) {
    return std::as_bytes(std::span(s.data(), s.size()));
}
bool eq(std::string_view s, const std::vector<std::byte>& b) {
    return b.size() == s.size() && std::memcmp(s.data(), b.data(), s.size()) == 0;
}

}  // namespace

// ---------- ObservedReader ----------

CPPIO_TEST_CASE(observed_reader_counts_read_calls_and_bytes) {
    cppio::MemoryReader inner = cppio::MemoryReader::from_string("hello");
    cppio::ReaderStats stats{};
    cppio::ObservedReader r(inner, stats);

    std::vector<std::byte> out(5);
    CPPIO_CHECK(r.read_exact(std::span<std::byte>(out)).has_value());

    CPPIO_CHECK(stats.read_calls == 1);
    CPPIO_CHECK(stats.read_bytes == 5);
    CPPIO_CHECK(stats.eof_count == 0);
    CPPIO_CHECK(stats.read_errors == 0);
}

CPPIO_TEST_CASE(observed_reader_counts_eof_when_zero_returned) {
    cppio::MemoryReader inner = cppio::MemoryReader::from_string("ab");
    cppio::ReaderStats stats{};
    cppio::ObservedReader r(inner, stats);

    std::array<std::byte, 2> a{};
    (void)r.read_some(std::span<std::byte>(a));       // consumes both bytes
    std::array<std::byte, 2> b{};
    auto e = r.read_some(std::span<std::byte>(b));    // EOF
    CPPIO_CHECK(e.has_value());
    CPPIO_CHECK(e.value() == 0);
    CPPIO_CHECK(stats.eof_count == 1);
}

CPPIO_TEST_CASE(observed_reader_counts_errors_and_does_not_count_as_eof) {
    cppio::MemoryReader mem = cppio::MemoryReader::from_string("abcdef");
    cppio::FaultPlan plan;
    plan.max_read_size = 1;
    plan.fail_after_read_calls = 2;
    cppio::FaultReader faulted(mem, plan);
    cppio::ReaderStats stats{};
    cppio::ObservedReader r(faulted, stats);

    std::vector<std::byte> out(6);
    (void)r.read_exact(std::span<std::byte>(out));
    CPPIO_CHECK(stats.read_errors >= 1);
    CPPIO_CHECK(stats.eof_count == 0);
}

CPPIO_TEST_CASE(observed_reader_is_data_transparent) {
    cppio::MemoryReader inner = cppio::MemoryReader::from_string("payload");
    cppio::ReaderStats stats{};
    cppio::ObservedReader r(inner, stats);

    std::vector<std::byte> out(7);
    CPPIO_CHECK(r.read_exact(std::span<std::byte>(out)).has_value());
    CPPIO_CHECK(eq("payload", out));
}

// ---------- ObservedWriter ----------

CPPIO_TEST_CASE(observed_writer_counts_write_calls_and_bytes) {
    cppio::MemoryWriter inner;
    cppio::WriterStats stats{};
    cppio::ObservedWriter w(inner, stats);

    CPPIO_CHECK(w.write_all(sb("hello")).has_value());
    CPPIO_CHECK(stats.write_calls == 1);
    CPPIO_CHECK(stats.write_bytes == 5);
    CPPIO_CHECK(stats.short_writes == 0);
}

CPPIO_TEST_CASE(observed_writer_counts_short_writes) {
    cppio::MemoryWriter sink;
    cppio::FaultPlan plan;
    plan.max_write_size = 2;
    cppio::FaultWriter inner(sink, plan);
    cppio::WriterStats stats{};
    cppio::ObservedWriter w(inner, stats);

    // A single write_all triggers multiple short writes internally.
    CPPIO_CHECK(w.write_all(sb("abcdef")).has_value());
    CPPIO_CHECK(stats.short_writes >= 1);  // at least one call wrote fewer than asked
    CPPIO_CHECK(stats.write_bytes == 6);
    CPPIO_CHECK(eq("abcdef", sink.bytes()));
}

CPPIO_TEST_CASE(observed_writer_counts_flush_calls_and_errors) {
    cppio::MemoryWriter sink;
    cppio::FaultPlan plan;
    plan.fail_flush = true;
    cppio::FaultWriter inner(sink, plan);
    cppio::WriterStats stats{};
    cppio::ObservedWriter w(inner, stats);

    auto res = w.flush();
    CPPIO_CHECK(!res.has_value());
    CPPIO_CHECK(stats.flush_calls == 1);
    CPPIO_CHECK(stats.flush_errors == 1);
}

CPPIO_TEST_CASE(observed_writer_flush_success_increments_only_flush_calls) {
    cppio::MemoryWriter inner;
    cppio::WriterStats stats{};
    cppio::ObservedWriter w(inner, stats);

    CPPIO_CHECK(w.flush().has_value());
    CPPIO_CHECK(stats.flush_calls == 1);
    CPPIO_CHECK(stats.flush_errors == 0);
    CPPIO_CHECK(stats.write_calls == 0);
}

CPPIO_TEST_CASE(observed_writer_is_data_transparent) {
    cppio::MemoryWriter inner;
    cppio::WriterStats stats{};
    cppio::ObservedWriter w(inner, stats);
    CPPIO_CHECK(w.write_all(sb("data")).has_value());
    CPPIO_CHECK(eq("data", inner.bytes()));
}

CPPIO_MAIN()
