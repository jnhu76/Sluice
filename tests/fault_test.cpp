// Tests for deterministic fault-injection wrappers. These are the correctness
// harness used by every downstream test (copy/wal/buffer), so their
// determinism and partial-write semantics matter most.
#include "harness.hpp"

#include <cppio/fault.hpp>
#include <cppio/reader.hpp>
#include <cppio/writer.hpp>

#include <cstddef>
#include <cstring>
#include <span>
#include <vector>

namespace {

[[maybe_unused]] std::vector<std::byte> bytes_of(std::string_view s) {
    auto* p = reinterpret_cast<const std::byte*>(s.data());
    return {p, p + s.size()};
}

bool same(std::string_view s, const std::vector<std::byte>& b) {
    return b.size() == s.size() &&
           std::memcmp(s.data(), b.data(), s.size()) == 0;
}

std::span<const std::byte> span_of(const std::vector<std::byte>& v) {
    return std::span<const std::byte>(v.data(), v.size());
}

}  // namespace

// ---------- FaultReader ----------

CPPIO_TEST_CASE(fault_reader_passes_through_when_no_plan) {
    auto mem = cppio::MemoryReader::from_string("abcdef");
    cppio::FaultPlan plan;  // empty: no failures, no caps
    cppio::FaultReader r(mem, plan);
    std::vector<std::byte> out(6);
    auto res = r.read_exact(std::span<std::byte>(out));
    CPPIO_CHECK(res.has_value());
    CPPIO_CHECK(same("abcdef", out));
}

CPPIO_TEST_CASE(fault_reader_fails_after_n_read_calls) {
    auto mem = cppio::MemoryReader::from_string("abcdef");
    cppio::FaultPlan plan;
    plan.fail_after_read_calls = 2;
    plan.max_read_size = 1;  // force one-byte reads so call count advances
    plan.error = cppio::IoError{cppio::IoError::Code::canceled};
    cppio::FaultReader r(mem, plan);
    std::vector<std::byte> out(6);
    auto res = r.read_exact(std::span<std::byte>(out));
    CPPIO_CHECK(!res.has_value());
    CPPIO_CHECK(res.error().code == cppio::IoError::Code::canceled);
}

CPPIO_TEST_CASE(fault_reader_forces_short_reads_via_max_read_size) {
    auto mem = cppio::MemoryReader::from_string("abcdef");
    cppio::FaultPlan plan;
    plan.max_read_size = 2;
    cppio::FaultReader r(mem, plan);
    std::array<std::byte, 6> out{};
    auto n1 = r.read_some(std::span<std::byte>(out));
    CPPIO_CHECK(n1.has_value());
    CPPIO_CHECK(n1.value() <= 2);  // never more than max_read_size
}

CPPIO_TEST_CASE(fault_reader_never_mutates_data) {
    auto mem = cppio::MemoryReader::from_string("abcdef");
    cppio::FaultPlan plan;
    plan.max_read_size = 2;
    cppio::FaultReader r(mem, plan);
    std::vector<std::byte> out(6);
    auto res = r.read_exact(std::span<std::byte>(out));
    CPPIO_CHECK(res.has_value());
    CPPIO_CHECK(same("abcdef", out));
}

CPPIO_TEST_CASE(fault_reader_fails_after_n_bytes) {
    auto mem = cppio::MemoryReader::from_string("abcdef");
    cppio::FaultPlan plan;
    plan.max_read_size = 1;
    plan.fail_after_bytes = 3;
    plan.error = cppio::IoError{cppio::IoError::Code::no_space};
    cppio::FaultReader r(mem, plan);
    std::vector<std::byte> out(6);
    auto res = r.read_exact(std::span<std::byte>(out));
    CPPIO_CHECK(!res.has_value());
    CPPIO_CHECK(res.error().code == cppio::IoError::Code::no_space);
}

// ---------- FaultWriter ----------

CPPIO_TEST_CASE(fault_writer_passes_through_when_no_plan) {
    cppio::MemoryWriter sink;
    cppio::FaultPlan plan;
    cppio::FaultWriter w(sink, plan);
    auto res = w.write_all(std::as_bytes(std::span(std::string_view("hello"))));
    CPPIO_CHECK(res.has_value());
    CPPIO_CHECK(same("hello", sink.bytes()));
}

CPPIO_TEST_CASE(fault_writer_fails_after_n_write_calls) {
    cppio::MemoryWriter sink;
    cppio::FaultPlan plan;
    plan.max_write_size = 2;  // force multiple calls for 6 bytes
    plan.fail_after_write_calls = 2;
    plan.error = cppio::IoError{cppio::IoError::Code::no_space};
    cppio::FaultWriter w(sink, plan);
    auto res = w.write_all(std::as_bytes(std::span(std::string_view("abcdef"))));
    CPPIO_CHECK(!res.has_value());
    CPPIO_CHECK(res.error().code == cppio::IoError::Code::no_space);
}

CPPIO_TEST_CASE(fault_writer_forces_short_writes_via_max_write_size) {
    cppio::MemoryWriter sink;
    cppio::FaultPlan plan;
    plan.max_write_size = 2;
    cppio::FaultWriter w(sink, plan);
    std::array<std::byte, 6> in{};
    auto n = w.write_some(std::span<const std::byte>(in));
    CPPIO_CHECK(n.has_value());
    CPPIO_CHECK(n.value() <= 2);
}

CPPIO_TEST_CASE(fault_writer_preserves_partial_write_data_on_failure) {
    cppio::MemoryWriter sink;
    cppio::FaultPlan plan;
    plan.max_write_size = 2;
    plan.fail_after_write_calls = 1;  // fail on the 2nd call
    plan.error = cppio::IoError{cppio::IoError::Code::no_space};
    cppio::FaultWriter w(sink, plan);
    auto res = w.write_all(std::as_bytes(std::span(std::string_view("abcdef"))));
    CPPIO_CHECK(!res.has_value());
    // first 2 bytes were written before the failure
    CPPIO_CHECK(sink.bytes().size() == 2);
    CPPIO_CHECK(same("ab", sink.bytes()));
}

CPPIO_TEST_CASE(fault_writer_flush_failure_propagates) {
    cppio::MemoryWriter sink;
    cppio::FaultPlan plan;
    plan.fail_flush = true;
    plan.error = cppio::IoError{cppio::IoError::Code::backend_error};
    cppio::FaultWriter w(sink, plan);
    auto res = w.flush();
    CPPIO_CHECK(!res.has_value());
    CPPIO_CHECK(res.error().code == cppio::IoError::Code::backend_error);
}

CPPIO_TEST_CASE(fault_writer_is_deterministic_across_instances) {
    // Same plan + same input => same outcome and same partial bytes.
    auto run = []() {
        cppio::MemoryWriter sink;
        cppio::FaultPlan plan;
        plan.max_write_size = 2;
        plan.fail_after_write_calls = 2;
        plan.error = cppio::IoError{cppio::IoError::Code::no_space};
        cppio::FaultWriter w(sink, plan);
        (void)w.write_all(std::as_bytes(std::span(std::string_view("abcdef"))));
        return sink.bytes();
    };
    auto a = run();
    auto b = run();
    CPPIO_CHECK(a.size() == b.size());
    CPPIO_CHECK(std::memcmp(a.data(), b.data(), a.size()) == 0);
}

CPPIO_TEST_CASE(fault_writer_clamps_single_write_to_fail_after_bytes) {
    // Regression: a single write_some larger than fail_after_bytes must not
    // overshoot the byte budget. Previously returned the full src.size().
    cppio::MemoryWriter sink;
    cppio::FaultPlan plan;
    plan.fail_after_bytes = 5;
    cppio::FaultWriter w(sink, plan);
    auto payload = bytes_of("0123456789");  // 10 bytes
    auto res = w.write_some(span_of(payload));
    CPPIO_CHECK(res.has_value());
    CPPIO_CHECK(res.value() == 5);              // clamped to the budget
    CPPIO_CHECK(same("01234", sink.bytes()));   // first 5 only
}

CPPIO_TEST_CASE(fault_writer_fails_exactly_at_byte_limit) {
    // fail_after_bytes=5 with one-byte-per-call: the first 5 calls succeed, the
    // 6th must fail (budget exhausted). Total delivered == 5, deterministically.
    cppio::MemoryWriter sink;
    cppio::FaultPlan plan;
    plan.max_write_size = 1;
    plan.fail_after_bytes = 5;
    plan.error = cppio::IoError{cppio::IoError::Code::no_space};
    cppio::FaultWriter w(sink, plan);
    auto payload = bytes_of("0123456789");
    auto res = w.write_all(span_of(payload));
    CPPIO_CHECK(!res.has_value());
    CPPIO_CHECK(res.error().code == cppio::IoError::Code::no_space);
    CPPIO_CHECK(same("01234", sink.bytes()));
    CPPIO_CHECK(sink.bytes().size() == 5);
}

CPPIO_TEST_CASE(fault_reader_clamps_single_read_to_fail_after_bytes) {
    auto mem = cppio::MemoryReader::from_string("0123456789");
    cppio::FaultPlan plan;
    plan.fail_after_bytes = 4;
    cppio::FaultReader r(mem, plan);
    std::array<std::byte, 10> out{};
    auto res = r.read_some(std::span<std::byte>(out));
    CPPIO_CHECK(res.has_value());
    CPPIO_CHECK(res.value() == 4);  // clamped, not 10
}

CPPIO_TEST_CASE(fault_reader_fails_after_bytes_combined_with_call_limit) {
    // Both conditions active simultaneously: byte limit (3) binds first here.
    auto mem = cppio::MemoryReader::from_string("abcdefgh");
    cppio::FaultPlan plan;
    plan.max_read_size = 1;
    plan.fail_after_bytes = 3;
    plan.fail_after_read_calls = 99;  // never reached
    plan.error = cppio::IoError{cppio::IoError::Code::canceled};
    cppio::FaultReader r(mem, plan);
    std::vector<std::byte> out(8);
    auto res = r.read_exact(std::span<std::byte>(out));
    CPPIO_CHECK(!res.has_value());
    CPPIO_CHECK(res.error().code == cppio::IoError::Code::canceled);
}

CPPIO_MAIN()
