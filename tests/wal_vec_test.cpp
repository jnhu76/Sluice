// Tests for wal::write_record_vec — the vector write path. It must produce
// byte-identical output to write_record and round-trip through the unchanged
// read_record. Covers single/multi record, byte-equivalence with the scalar
// path, writer-fault propagation, and the payload-overflow guard.
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

} // namespace

SLUICE_TEST_CASE(wal_vec_round_trips_one_record) {
    sluice::MemoryWriter out;
    auto payload = bytes_of("hello wal");
    SLUICE_CHECK(sluice::wal::write_record_vec(out, span_of(payload)).has_value());

    sluice::MemoryReader in(out.bytes());
    auto res = sluice::wal::read_record(in);
    SLUICE_CHECK(res.has_value());
    SLUICE_CHECK(eq("hello wal", res.value()));
}

SLUICE_TEST_CASE(wal_vec_round_trips_multiple_records_in_order) {
    sluice::MemoryWriter out;
    auto a = bytes_of("first");
    auto b = bytes_of("second record");
    auto c = bytes_of("3");
    SLUICE_CHECK(sluice::wal::write_record_vec(out, span_of(a)).has_value());
    SLUICE_CHECK(sluice::wal::write_record_vec(out, span_of(b)).has_value());
    SLUICE_CHECK(sluice::wal::write_record_vec(out, span_of(c)).has_value());

    sluice::MemoryReader in(out.bytes());
    auto r1 = sluice::wal::read_record(in);
    auto r2 = sluice::wal::read_record(in);
    auto r3 = sluice::wal::read_record(in);
    SLUICE_CHECK(r1.has_value() && eq("first", r1.value()));
    SLUICE_CHECK(r2.has_value() && eq("second record", r2.value()));
    SLUICE_CHECK(r3.has_value() && eq("3", r3.value()));
}

SLUICE_TEST_CASE(wal_vec_produces_same_bytes_as_scalar_path) {
    // The vector path and the scalar path must serialize identically.
    sluice::MemoryWriter vec_out;
    sluice::MemoryWriter scalar_out;
    auto payload = bytes_of("identical bytes");
    SLUICE_CHECK(sluice::wal::write_record_vec(vec_out, span_of(payload)).has_value());
    SLUICE_CHECK(sluice::wal::write_record(scalar_out, span_of(payload)).has_value());
    SLUICE_CHECK(vec_out.bytes().size() == scalar_out.bytes().size());
    SLUICE_CHECK(std::memcmp(vec_out.bytes().data(), scalar_out.bytes().data(),
                             vec_out.bytes().size()) == 0);
}

SLUICE_TEST_CASE(wal_vec_writer_fault_propagates) {
    sluice::MemoryWriter sink;
    sluice::FaultPlan plan;
    plan.fail_after_write_calls = 1;
    plan.error = sluice::IoError{sluice::IoError::Code::no_space};
    sluice::FaultWriter fw(sink, plan);

    auto payload = bytes_of("will not fit");
    auto res = sluice::wal::write_record_vec(fw, span_of(payload));
    SLUICE_CHECK(!res.has_value());
    SLUICE_CHECK(res.error().code == sluice::IoError::Code::no_space);
}

SLUICE_TEST_CASE(wal_vec_survives_short_writes) {
    // write_all_vec inside write_record_vec must retry across short writes and
    // still produce a valid, readable record.
    sluice::MemoryWriter sink;
    sluice::FaultPlan plan;
    plan.max_write_size = 2;
    sluice::FaultWriter fw(sink, plan);

    auto payload = bytes_of("resilient payload");
    SLUICE_CHECK(sluice::wal::write_record_vec(fw, span_of(payload)).has_value());

    sluice::MemoryReader in(sink.bytes());
    auto res = sluice::wal::read_record(in);
    SLUICE_CHECK(res.has_value());
    SLUICE_CHECK(eq("resilient payload", res.value()));
}

SLUICE_TEST_CASE(wal_vec_empty_payload_round_trips) {
    sluice::MemoryWriter out;
    std::vector<std::byte> empty;
    SLUICE_CHECK(sluice::wal::write_record_vec(out, span_of(empty)).has_value());

    sluice::MemoryReader in(out.bytes());
    auto res = sluice::wal::read_record(in);
    SLUICE_CHECK(res.has_value());
    SLUICE_CHECK(res.value().empty());
}

SLUICE_TEST_CASE(wal_vec_payload_overflow_still_fails) {
    // Same overflow guard as write_record: a payload > UINT32_MAX is rejected
    // before any framing. Exercises the guard without a 4 GiB allocation.
    sluice::MemoryWriter out;
    std::span<const std::byte> huge{static_cast<const std::byte*>(nullptr),
                                    static_cast<std::size_t>(UINT32_MAX) + 1};
    auto res = sluice::wal::write_record_vec(out, huge);
    SLUICE_CHECK(!res.has_value());
    SLUICE_CHECK(res.error().code == sluice::IoError::Code::invalid_state);
}

SLUICE_MAIN()
