// Tests for the minimal WAL record format: round-trip, multi-record, truncated
// record, checksum mismatch, and writer-fault propagation. Backed by the
// in-memory Reader/Writer + Fault wrappers.
#include "harness.hpp"

#include <sluice/wal.hpp>
#include <sluice/fault.hpp>
#include <sluice/reader.hpp>
#include <sluice/writer.hpp>

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

SLUICE_TEST_CASE(wal_round_trips_one_record) {
    sluice::MemoryWriter out;
    auto payload = bytes_of("hello wal");
    SLUICE_CHECK(sluice::wal::write_record(out, span_of(payload)).has_value());

    sluice::MemoryReader in(out.bytes());
    auto res = sluice::wal::read_record(in);
    SLUICE_CHECK(res.has_value());
    SLUICE_CHECK(res.value().size() == payload.size());
    SLUICE_CHECK(eq("hello wal", res.value()));
}

SLUICE_TEST_CASE(wal_round_trips_multiple_records_in_order) {
    sluice::MemoryWriter out;
    auto a = bytes_of("first");
    auto b = bytes_of("second record");
    auto c = bytes_of("3");
    SLUICE_CHECK(sluice::wal::write_record(out, span_of(a)).has_value());
    SLUICE_CHECK(sluice::wal::write_record(out, span_of(b)).has_value());
    SLUICE_CHECK(sluice::wal::write_record(out, span_of(c)).has_value());

    sluice::MemoryReader in(out.bytes());
    auto r1 = sluice::wal::read_record(in);
    auto r2 = sluice::wal::read_record(in);
    auto r3 = sluice::wal::read_record(in);
    SLUICE_CHECK(r1.has_value() && eq("first", r1.value()));
    SLUICE_CHECK(r2.has_value() && eq("second record", r2.value()));
    SLUICE_CHECK(r3.has_value() && eq("3", r3.value()));
}

SLUICE_TEST_CASE(wal_read_record_at_clean_eof_returns_eof) {
    sluice::MemoryWriter out;
    auto payload = bytes_of("only");
    SLUICE_CHECK(sluice::wal::write_record(out, span_of(payload)).has_value());

    sluice::MemoryReader in(out.bytes());
    (void)sluice::wal::read_record(in);
    // Next read at exactly EOF: no header bytes available.
    auto res = sluice::wal::read_record(in);
    SLUICE_CHECK(!res.has_value());
    SLUICE_CHECK(res.error().code == sluice::IoError::Code::eof);
}

SLUICE_TEST_CASE(wal_truncated_record_returns_error) {
    sluice::MemoryWriter out;
    auto payload = bytes_of("payload-data");
    SLUICE_CHECK(sluice::wal::write_record(out, span_of(payload)).has_value());

    // Truncate the encoded stream so the payload is incomplete.
    auto full = out.bytes();
    SLUICE_CHECK(full.size() > 8);  // header is 8 bytes
    std::vector<std::byte> trunc(full.begin(), full.begin() + (full.size() - 3));
    sluice::MemoryReader in(trunc);
    auto res = sluice::wal::read_record(in);
    SLUICE_CHECK(!res.has_value());
    SLUICE_CHECK(res.error().code == sluice::IoError::Code::eof);
}

SLUICE_TEST_CASE(wal_truncated_header_returns_error) {
    sluice::MemoryWriter out;
    auto payload = bytes_of("data");
    SLUICE_CHECK(sluice::wal::write_record(out, span_of(payload)).has_value());

    // Keep only 3 header bytes (< the 8-byte header) so read_exact on the
    // header fails with EOF.
    auto full = out.bytes();
    std::vector<std::byte> trunc(full.begin(), full.begin() + 3);
    sluice::MemoryReader in(trunc);
    auto res = sluice::wal::read_record(in);
    SLUICE_CHECK(!res.has_value());
    SLUICE_CHECK(res.error().code == sluice::IoError::Code::eof);
}

SLUICE_TEST_CASE(wal_checksum_mismatch_returns_error) {
    sluice::MemoryWriter out;
    auto payload = bytes_of("checksum me");
    SLUICE_CHECK(sluice::wal::write_record(out, span_of(payload)).has_value());

    auto bytes = out.take();
    // Corrupt one payload byte (offset 8 = first payload byte).
    bytes[8] = std::byte{static_cast<unsigned char>(std::to_integer<int>(bytes[8]) ^ 0xFF)};
    sluice::MemoryReader in(bytes);
    auto res = sluice::wal::read_record(in);
    SLUICE_CHECK(!res.has_value());
    SLUICE_CHECK(res.error().code == sluice::IoError::Code::invalid_state);
}

SLUICE_TEST_CASE(wal_writer_fault_propagates) {
    sluice::MemoryWriter sink;
    sluice::FaultPlan plan;
    plan.fail_after_write_calls = 1;
    plan.error = sluice::IoError{sluice::IoError::Code::no_space};
    sluice::FaultWriter fw(sink, plan);

    auto payload = bytes_of("will not fit");
    auto res = sluice::wal::write_record(fw, span_of(payload));
    SLUICE_CHECK(!res.has_value());
    SLUICE_CHECK(res.error().code == sluice::IoError::Code::no_space);
}

SLUICE_TEST_CASE(wal_writer_short_write_does_not_corrupt_record) {
    // Force short writes; write_all inside write_record must still produce a
    // valid, readable record.
    sluice::MemoryWriter sink;
    sluice::FaultPlan plan;
    plan.max_write_size = 2;
    sluice::FaultWriter fw(sink, plan);

    auto payload = bytes_of("resilient payload");
    SLUICE_CHECK(sluice::wal::write_record(fw, span_of(payload)).has_value());

    sluice::MemoryReader in(sink.bytes());
    auto res = sluice::wal::read_record(in);
    SLUICE_CHECK(res.has_value());
    SLUICE_CHECK(eq("resilient payload", res.value()));
}

SLUICE_TEST_CASE(wal_empty_payload_round_trips) {
    sluice::MemoryWriter out;
    std::vector<std::byte> empty;
    SLUICE_CHECK(sluice::wal::write_record(out, span_of(empty)).has_value());

    sluice::MemoryReader in(out.bytes());
    auto res = sluice::wal::read_record(in);
    SLUICE_CHECK(res.has_value());
    SLUICE_CHECK(res.value().empty());
}

SLUICE_TEST_CASE(wal_checked_u32_len_accepts_exact_max) {
    auto r = sluice::wal::detail::checked_u32_len(static_cast<std::size_t>(UINT32_MAX));
    SLUICE_CHECK(r.has_value());
    SLUICE_CHECK(r.value() == UINT32_MAX);
}

SLUICE_TEST_CASE(wal_checked_u32_len_rejects_overflow) {
    // One past UINT32_MAX must fail; this is the value the old cast truncated.
    // This directly exercises the guard without constructing a 4 GiB payload
    // (or the UB-span workaround an earlier version of this test used).
    auto r = sluice::wal::detail::checked_u32_len(static_cast<std::size_t>(UINT32_MAX) + 1);
    SLUICE_CHECK(!r.has_value());
    SLUICE_CHECK(r.error().code == sluice::IoError::Code::invalid_state);
}

SLUICE_MAIN()
