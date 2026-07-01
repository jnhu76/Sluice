// Tests for copy_all: exact byte copy, total returned, error propagation from
// both sides, EOF handling, and the no-internal-alloc scratch variant.
#include "harness.hpp"

#include <cppio/copy.hpp>
#include <cppio/fault.hpp>
#include <cppio/reader.hpp>
#include <cppio/writer.hpp>

#include <cstring>
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

CPPIO_MAIN()
