// Tests for ObservedReader / ObservedWriter: stats accounting + transparent
// pass-through of data semantics. Backed by MemoryReader/MemoryWriter + Fault.
#include "harness.hpp"

#include <sluice/observed.hpp>
#include <sluice/fault.hpp>
#include <sluice/reader.hpp>
#include <sluice/writer.hpp>

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

SLUICE_TEST_CASE(observed_reader_counts_read_calls_and_bytes) {
    sluice::MemoryReader inner = sluice::MemoryReader::from_string("hello");
    sluice::ReaderStats stats{};
    sluice::ObservedReader r(inner, stats);

    std::vector<std::byte> out(5);
    SLUICE_CHECK(r.read_exact(std::span<std::byte>(out)).has_value());

    SLUICE_CHECK(stats.read_calls == 1);
    SLUICE_CHECK(stats.read_bytes == 5);
    SLUICE_CHECK(stats.eof_count == 0);
    SLUICE_CHECK(stats.read_errors == 0);
}

SLUICE_TEST_CASE(observed_reader_counts_eof_when_zero_returned) {
    sluice::MemoryReader inner = sluice::MemoryReader::from_string("ab");
    sluice::ReaderStats stats{};
    sluice::ObservedReader r(inner, stats);

    std::array<std::byte, 2> a{};
    (void)r.read_some(std::span<std::byte>(a));       // consumes both bytes
    std::array<std::byte, 2> b{};
    auto e = r.read_some(std::span<std::byte>(b));    // EOF
    SLUICE_CHECK(e.has_value());
    SLUICE_CHECK(e.value() == 0);
    SLUICE_CHECK(stats.eof_count == 1);
}

SLUICE_TEST_CASE(observed_reader_counts_errors_and_does_not_count_as_eof) {
    sluice::MemoryReader mem = sluice::MemoryReader::from_string("abcdef");
    sluice::FaultPlan plan;
    plan.max_read_size = 1;
    plan.fail_after_read_calls = 2;
    sluice::FaultReader faulted(mem, plan);
    sluice::ReaderStats stats{};
    sluice::ObservedReader r(faulted, stats);

    std::vector<std::byte> out(6);
    (void)r.read_exact(std::span<std::byte>(out));
    SLUICE_CHECK(stats.read_errors >= 1);
    SLUICE_CHECK(stats.eof_count == 0);
}

SLUICE_TEST_CASE(observed_reader_is_data_transparent) {
    sluice::MemoryReader inner = sluice::MemoryReader::from_string("payload");
    sluice::ReaderStats stats{};
    sluice::ObservedReader r(inner, stats);

    std::vector<std::byte> out(7);
    SLUICE_CHECK(r.read_exact(std::span<std::byte>(out)).has_value());
    SLUICE_CHECK(eq("payload", out));
}

// ---------- ObservedWriter ----------

SLUICE_TEST_CASE(observed_writer_counts_write_calls_and_bytes) {
    sluice::MemoryWriter inner;
    sluice::WriterStats stats{};
    sluice::ObservedWriter w(inner, stats);

    SLUICE_CHECK(w.write_all(sb("hello")).has_value());
    SLUICE_CHECK(stats.write_calls == 1);
    SLUICE_CHECK(stats.write_bytes == 5);
    SLUICE_CHECK(stats.short_writes == 0);
}

SLUICE_TEST_CASE(observed_writer_counts_short_writes) {
    sluice::MemoryWriter sink;
    sluice::FaultPlan plan;
    plan.max_write_size = 2;
    sluice::FaultWriter inner(sink, plan);
    sluice::WriterStats stats{};
    sluice::ObservedWriter w(inner, stats);

    // A single write_all triggers multiple short writes internally.
    SLUICE_CHECK(w.write_all(sb("abcdef")).has_value());
    SLUICE_CHECK(stats.short_writes >= 1);  // at least one call wrote fewer than asked
    SLUICE_CHECK(stats.write_bytes == 6);
    SLUICE_CHECK(eq("abcdef", sink.bytes()));
}

SLUICE_TEST_CASE(observed_writer_counts_flush_calls_and_errors) {
    sluice::MemoryWriter sink;
    sluice::FaultPlan plan;
    plan.fail_flush = true;
    sluice::FaultWriter inner(sink, plan);
    sluice::WriterStats stats{};
    sluice::ObservedWriter w(inner, stats);

    auto res = w.flush();
    SLUICE_CHECK(!res.has_value());
    SLUICE_CHECK(stats.flush_calls == 1);
    SLUICE_CHECK(stats.flush_errors == 1);
}

SLUICE_TEST_CASE(observed_writer_flush_success_increments_only_flush_calls) {
    sluice::MemoryWriter inner;
    sluice::WriterStats stats{};
    sluice::ObservedWriter w(inner, stats);

    SLUICE_CHECK(w.flush().has_value());
    SLUICE_CHECK(stats.flush_calls == 1);
    SLUICE_CHECK(stats.flush_errors == 0);
    SLUICE_CHECK(stats.write_calls == 0);
}

SLUICE_TEST_CASE(observed_writer_is_data_transparent) {
    sluice::MemoryWriter inner;
    sluice::WriterStats stats{};
    sluice::ObservedWriter w(inner, stats);
    SLUICE_CHECK(w.write_all(sb("data")).has_value());
    SLUICE_CHECK(eq("data", inner.bytes()));
}

SLUICE_TEST_CASE(observed_writer_counts_write_errors) {
    // Wrap a FaultWriter that fails after one call; ObservedWriter must both
    // propagate the error and increment write_errors.
    sluice::MemoryWriter sink;
    sluice::FaultPlan plan;
    plan.fail_after_write_calls = 0;  // very first write fails
    plan.error = sluice::IoError{sluice::IoError::Code::no_space};
    sluice::FaultWriter inner(sink, plan);
    sluice::WriterStats stats{};
    sluice::ObservedWriter w(inner, stats);

    auto res = w.write_some(sb("data"));
    SLUICE_CHECK(!res.has_value());
    SLUICE_CHECK(res.error().code == sluice::IoError::Code::no_space);
    SLUICE_CHECK(stats.write_errors == 1);
    SLUICE_CHECK(stats.write_calls == 1);
}

SLUICE_TEST_CASE(observed_writer_does_not_count_write_errors_on_success) {
    sluice::MemoryWriter inner;
    sluice::WriterStats stats{};
    sluice::ObservedWriter w(inner, stats);
    SLUICE_CHECK(w.write_all(sb("ok")).has_value());
    SLUICE_CHECK(stats.write_errors == 0);
}

SLUICE_MAIN()
