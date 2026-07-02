// Tests for wal::write_record_vec — the vector write path. It must produce
// byte-identical output to write_record and round-trip through the unchanged
// read_record. Covers single/multi record, byte-equivalence with the scalar
// path, writer-fault propagation, and the payload-overflow guard.
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

CPPIO_TEST_CASE(wal_vec_round_trips_one_record) {
    cppio::MemoryWriter out;
    auto payload = bytes_of("hello wal");
    CPPIO_CHECK(cppio::wal::write_record_vec(out, span_of(payload)).has_value());

    cppio::MemoryReader in(out.bytes());
    auto res = cppio::wal::read_record(in);
    CPPIO_CHECK(res.has_value());
    CPPIO_CHECK(eq("hello wal", res.value()));
}

CPPIO_TEST_CASE(wal_vec_round_trips_multiple_records_in_order) {
    cppio::MemoryWriter out;
    auto a = bytes_of("first");
    auto b = bytes_of("second record");
    auto c = bytes_of("3");
    CPPIO_CHECK(cppio::wal::write_record_vec(out, span_of(a)).has_value());
    CPPIO_CHECK(cppio::wal::write_record_vec(out, span_of(b)).has_value());
    CPPIO_CHECK(cppio::wal::write_record_vec(out, span_of(c)).has_value());

    cppio::MemoryReader in(out.bytes());
    auto r1 = cppio::wal::read_record(in);
    auto r2 = cppio::wal::read_record(in);
    auto r3 = cppio::wal::read_record(in);
    CPPIO_CHECK(r1.has_value() && eq("first", r1.value()));
    CPPIO_CHECK(r2.has_value() && eq("second record", r2.value()));
    CPPIO_CHECK(r3.has_value() && eq("3", r3.value()));
}

CPPIO_TEST_CASE(wal_vec_produces_same_bytes_as_scalar_path) {
    // The vector path and the scalar path must serialize identically.
    cppio::MemoryWriter vec_out;
    cppio::MemoryWriter scalar_out;
    auto payload = bytes_of("identical bytes");
    CPPIO_CHECK(cppio::wal::write_record_vec(vec_out, span_of(payload)).has_value());
    CPPIO_CHECK(cppio::wal::write_record(scalar_out, span_of(payload)).has_value());
    CPPIO_CHECK(vec_out.bytes().size() == scalar_out.bytes().size());
    CPPIO_CHECK(std::memcmp(vec_out.bytes().data(), scalar_out.bytes().data(),
                            vec_out.bytes().size()) == 0);
}

CPPIO_TEST_CASE(wal_vec_writer_fault_propagates) {
    cppio::MemoryWriter sink;
    cppio::FaultPlan plan;
    plan.fail_after_write_calls = 1;
    plan.error = cppio::IoError{cppio::IoError::Code::no_space};
    cppio::FaultWriter fw(sink, plan);

    auto payload = bytes_of("will not fit");
    auto res = cppio::wal::write_record_vec(fw, span_of(payload));
    CPPIO_CHECK(!res.has_value());
    CPPIO_CHECK(res.error().code == cppio::IoError::Code::no_space);
}

CPPIO_TEST_CASE(wal_vec_survives_short_writes) {
    // write_all_vec inside write_record_vec must retry across short writes and
    // still produce a valid, readable record.
    cppio::MemoryWriter sink;
    cppio::FaultPlan plan;
    plan.max_write_size = 2;
    cppio::FaultWriter fw(sink, plan);

    auto payload = bytes_of("resilient payload");
    CPPIO_CHECK(cppio::wal::write_record_vec(fw, span_of(payload)).has_value());

    cppio::MemoryReader in(sink.bytes());
    auto res = cppio::wal::read_record(in);
    CPPIO_CHECK(res.has_value());
    CPPIO_CHECK(eq("resilient payload", res.value()));
}

CPPIO_TEST_CASE(wal_vec_empty_payload_round_trips) {
    cppio::MemoryWriter out;
    std::vector<std::byte> empty;
    CPPIO_CHECK(cppio::wal::write_record_vec(out, span_of(empty)).has_value());

    cppio::MemoryReader in(out.bytes());
    auto res = cppio::wal::read_record(in);
    CPPIO_CHECK(res.has_value());
    CPPIO_CHECK(res.value().empty());
}

CPPIO_TEST_CASE(wal_vec_payload_overflow_still_fails) {
    // Same overflow guard as write_record: a payload > UINT32_MAX is rejected
    // before any framing. Exercises the guard without a 4 GiB allocation.
    cppio::MemoryWriter out;
    std::span<const std::byte> huge{static_cast<const std::byte*>(nullptr),
                                    static_cast<std::size_t>(UINT32_MAX) + 1};
    auto res = cppio::wal::write_record_vec(out, huge);
    CPPIO_CHECK(!res.has_value());
    CPPIO_CHECK(res.error().code == cppio::IoError::Code::invalid_state);
}

CPPIO_MAIN()
