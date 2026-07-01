// Tests for the minimal WAL record format: round-trip, multi-record, truncated
// record, checksum mismatch, and writer-fault propagation. Backed by the
// in-memory Reader/Writer + Fault wrappers.
#include "harness.hpp"

#include <cppio/wal.hpp>
#include <cppio/fault.hpp>
#include <cppio/reader.hpp>
#include <cppio/writer.hpp>

#include <cstring>
#include <span>
#include <string>
#include <vector>

namespace {

bool eq(std::string_view s, const std::vector<std::byte>& b) {
    return b.size() == s.size() && std::memcmp(s.data(), b.data(), s.size()) == 0;
}
std::vector<std::byte> bytes_of(std::string_view s) {
    auto* p = reinterpret_cast<const std::byte*>(s.data());
    return {p, p + s.size()};
}
std::span<const std::byte> span_of(const std::vector<std::byte>& v) {
    return std::span<const std::byte>(v.data(), v.size());
}

}  // namespace

CPPIO_TEST_CASE(wal_round_trips_one_record) {
    cppio::MemoryWriter out;
    auto payload = bytes_of("hello wal");
    CPPIO_CHECK(cppio::wal::write_record(out, span_of(payload)).has_value());

    cppio::MemoryReader in(out.bytes());
    auto res = cppio::wal::read_record(in);
    CPPIO_CHECK(res.has_value());
    CPPIO_CHECK(res.value().size() == payload.size());
    CPPIO_CHECK(eq("hello wal", res.value()));
}

CPPIO_TEST_CASE(wal_round_trips_multiple_records_in_order) {
    cppio::MemoryWriter out;
    auto a = bytes_of("first");
    auto b = bytes_of("second record");
    auto c = bytes_of("3");
    CPPIO_CHECK(cppio::wal::write_record(out, span_of(a)).has_value());
    CPPIO_CHECK(cppio::wal::write_record(out, span_of(b)).has_value());
    CPPIO_CHECK(cppio::wal::write_record(out, span_of(c)).has_value());

    cppio::MemoryReader in(out.bytes());
    auto r1 = cppio::wal::read_record(in);
    auto r2 = cppio::wal::read_record(in);
    auto r3 = cppio::wal::read_record(in);
    CPPIO_CHECK(r1.has_value() && eq("first", r1.value()));
    CPPIO_CHECK(r2.has_value() && eq("second record", r2.value()));
    CPPIO_CHECK(r3.has_value() && eq("3", r3.value()));
}

CPPIO_TEST_CASE(wal_read_record_at_clean_eof_returns_eof) {
    cppio::MemoryWriter out;
    auto payload = bytes_of("only");
    CPPIO_CHECK(cppio::wal::write_record(out, span_of(payload)).has_value());

    cppio::MemoryReader in(out.bytes());
    (void)cppio::wal::read_record(in);
    // Next read at exactly EOF: no header bytes available.
    auto res = cppio::wal::read_record(in);
    CPPIO_CHECK(!res.has_value());
    CPPIO_CHECK(res.error().code == cppio::IoError::Code::eof);
}

CPPIO_TEST_CASE(wal_truncated_record_returns_error) {
    cppio::MemoryWriter out;
    auto payload = bytes_of("payload-data");
    CPPIO_CHECK(cppio::wal::write_record(out, span_of(payload)).has_value());

    // Truncate the encoded stream so the payload is incomplete.
    auto full = out.bytes();
    CPPIO_CHECK(full.size() > 8);  // header is 8 bytes
    std::vector<std::byte> trunc(full.begin(), full.begin() + (full.size() - 3));
    cppio::MemoryReader in(trunc);
    auto res = cppio::wal::read_record(in);
    CPPIO_CHECK(!res.has_value());
    CPPIO_CHECK(res.error().code == cppio::IoError::Code::eof);
}

CPPIO_TEST_CASE(wal_truncated_header_returns_error) {
    cppio::MemoryWriter out;
    auto payload = bytes_of("data");
    CPPIO_CHECK(cppio::wal::write_record(out, span_of(payload)).has_value());

    // Keep only 3 header bytes (< the 8-byte header) so read_exact on the
    // header fails with EOF.
    auto full = out.bytes();
    std::vector<std::byte> trunc(full.begin(), full.begin() + 3);
    cppio::MemoryReader in(trunc);
    auto res = cppio::wal::read_record(in);
    CPPIO_CHECK(!res.has_value());
    CPPIO_CHECK(res.error().code == cppio::IoError::Code::eof);
}

CPPIO_TEST_CASE(wal_checksum_mismatch_returns_error) {
    cppio::MemoryWriter out;
    auto payload = bytes_of("checksum me");
    CPPIO_CHECK(cppio::wal::write_record(out, span_of(payload)).has_value());

    auto bytes = out.take();
    // Corrupt one payload byte (offset 8 = first payload byte).
    bytes[8] = std::byte{static_cast<unsigned char>(std::to_integer<int>(bytes[8]) ^ 0xFF)};
    cppio::MemoryReader in(bytes);
    auto res = cppio::wal::read_record(in);
    CPPIO_CHECK(!res.has_value());
    CPPIO_CHECK(res.error().code == cppio::IoError::Code::invalid_state);
}

CPPIO_TEST_CASE(wal_writer_fault_propagates) {
    cppio::MemoryWriter sink;
    cppio::FaultPlan plan;
    plan.fail_after_write_calls = 1;
    plan.error = cppio::IoError{cppio::IoError::Code::no_space};
    cppio::FaultWriter fw(sink, plan);

    auto payload = bytes_of("will not fit");
    auto res = cppio::wal::write_record(fw, span_of(payload));
    CPPIO_CHECK(!res.has_value());
    CPPIO_CHECK(res.error().code == cppio::IoError::Code::no_space);
}

CPPIO_TEST_CASE(wal_writer_short_write_does_not_corrupt_record) {
    // Force short writes; write_all inside write_record must still produce a
    // valid, readable record.
    cppio::MemoryWriter sink;
    cppio::FaultPlan plan;
    plan.max_write_size = 2;
    cppio::FaultWriter fw(sink, plan);

    auto payload = bytes_of("resilient payload");
    CPPIO_CHECK(cppio::wal::write_record(fw, span_of(payload)).has_value());

    cppio::MemoryReader in(sink.bytes());
    auto res = cppio::wal::read_record(in);
    CPPIO_CHECK(res.has_value());
    CPPIO_CHECK(eq("resilient payload", res.value()));
}

CPPIO_TEST_CASE(wal_empty_payload_round_trips) {
    cppio::MemoryWriter out;
    std::vector<std::byte> empty;
    CPPIO_CHECK(cppio::wal::write_record(out, span_of(empty)).has_value());

    cppio::MemoryReader in(out.bytes());
    auto res = cppio::wal::read_record(in);
    CPPIO_CHECK(res.has_value());
    CPPIO_CHECK(res.value().empty());
}

CPPIO_TEST_CASE(wal_checked_u32_len_accepts_exact_max) {
    auto r = cppio::wal::detail::checked_u32_len(static_cast<std::size_t>(UINT32_MAX));
    CPPIO_CHECK(r.has_value());
    CPPIO_CHECK(r.value() == UINT32_MAX);
}

CPPIO_TEST_CASE(wal_checked_u32_len_rejects_overflow) {
    // One past UINT32_MAX must fail; this is the value the old cast truncated.
    auto r = cppio::wal::detail::checked_u32_len(static_cast<std::size_t>(UINT32_MAX) + 1);
    CPPIO_CHECK(!r.has_value());
    CPPIO_CHECK(r.error().code == cppio::IoError::Code::invalid_state);
}

CPPIO_TEST_CASE(wal_write_record_rejects_payload_larger_than_u32) {
    // Force checked_u32_len to fail without allocating 4 GiB: wrap the writer
    // so we can also assert nothing was written before the rejection.
    cppio::MemoryWriter out;
    // Construct a span whose length overflows u32 but points at valid memory.
    // We can't hold 4 GiB, so point at a 1-byte buffer and claim a huge size;
    // write_record only inspects payload.size() before writing (which is the
    // exact bug being guarded), so no actual read happens.
    std::byte one{};
    std::span<const std::byte> huge(&one, static_cast<std::size_t>(UINT32_MAX) + 1);
    auto res = cppio::wal::write_record(out, huge);
    CPPIO_CHECK(!res.has_value());
    CPPIO_CHECK(res.error().code == cppio::IoError::Code::invalid_state);
    CPPIO_CHECK(out.bytes().empty());  // rejected before any write
}

CPPIO_MAIN()
