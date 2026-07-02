// Tests for deferred (reserved, not-yet-implemented) copy strategies
// (CPPIO-CORE-007E). VectorDeferred/FileRangeDeferred/SendfileDeferred/
// SpliceDeferred must NOT pretend to work: with the default policy they return
// invalid_state and touch nothing; with FallbackToAuto they mark the fallback
// and run Auto.
#include "harness.hpp"

#include <cppio/buffer.hpp>
#include <cppio/copy.hpp>
#include <cppio/copy_strategy.hpp>
#include <cppio/fault.hpp>
#include <cppio/limit.hpp>

#include <cstring>
#include <span>
#include <vector>

namespace {

class CountingReader final : public cppio::Reader {
public:
    cppio::MemoryReader mem;
    int calls = 0;
    explicit CountingReader(std::string_view s) : mem(cppio::MemoryReader::from_string(s)) {}
    cppio::Result<std::size_t> read_some(std::span<std::byte> dst) override {
        ++calls;
        return mem.read_some(dst);
    }
};

class CountingWriter final : public cppio::Writer {
public:
    std::vector<std::byte> sink;
    int calls = 0;
    cppio::Result<std::size_t> write_some(std::span<const std::byte> src) override {
        ++calls;
        sink.insert(sink.end(), src.begin(), src.end());
        return src.size();
    }
    cppio::Result<void> flush() override { return {}; }
};

bool eq(std::string_view s, const std::vector<std::byte>& b) {
    return b.size() == s.size() && std::memcmp(s.data(), b.data(), s.size()) == 0;
}

cppio::CopyOptions opts_deferred(cppio::CopyStrategy s,
                                 cppio::UnsupportedStrategyPolicy p =
                                     cppio::UnsupportedStrategyPolicy::ReturnInvalidState) {
    cppio::CopyOptions o;
    o.strategy = s;
    o.unsupported_policy = p;
    return o;
}

}  // namespace

CPPIO_TEST_CASE(vector_deferred_returns_invalid_state_by_default) {
    auto reader = cppio::MemoryReader::from_string("abc");
    CountingWriter writer;
    std::vector<std::byte> scratch(8);
    auto res = cppio::copy_all(reader, writer, std::span<std::byte>(scratch),
                               opts_deferred(cppio::CopyStrategy::VectorDeferred));
    CPPIO_CHECK(!res.has_value());
    CPPIO_CHECK(res.error().code == cppio::IoError::Code::invalid_state);
}

CPPIO_TEST_CASE(file_range_deferred_returns_invalid_state_by_default) {
    auto reader = cppio::MemoryReader::from_string("abc");
    CountingWriter writer;
    std::vector<std::byte> scratch(8);
    auto res = cppio::copy_all(reader, writer, std::span<std::byte>(scratch),
                               opts_deferred(cppio::CopyStrategy::FileRangeDeferred));
    CPPIO_CHECK(!res.has_value());
    CPPIO_CHECK(res.error().code == cppio::IoError::Code::invalid_state);
}

CPPIO_TEST_CASE(sendfile_deferred_returns_invalid_state_by_default) {
    auto reader = cppio::MemoryReader::from_string("abc");
    CountingWriter writer;
    std::vector<std::byte> scratch(8);
    auto res = cppio::copy_all(reader, writer, std::span<std::byte>(scratch),
                               opts_deferred(cppio::CopyStrategy::SendfileDeferred));
    CPPIO_CHECK(!res.has_value());
    CPPIO_CHECK(res.error().code == cppio::IoError::Code::invalid_state);
}

CPPIO_TEST_CASE(splice_deferred_returns_invalid_state_by_default) {
    auto reader = cppio::MemoryReader::from_string("abc");
    CountingWriter writer;
    std::vector<std::byte> scratch(8);
    auto res = cppio::copy_all(reader, writer, std::span<std::byte>(scratch),
                               opts_deferred(cppio::CopyStrategy::SpliceDeferred));
    CPPIO_CHECK(!res.has_value());
    CPPIO_CHECK(res.error().code == cppio::IoError::Code::invalid_state);
}

CPPIO_TEST_CASE(deferred_strategy_touches_neither_reader_nor_writer) {
    CountingReader inner("hello");
    std::vector<std::byte> rbuf(16);
    cppio::BufferedReader br(inner, rbuf);
    // Prime the buffer so a fallback would otherwise move bytes.
    std::vector<std::byte> primed(2);
    (void)br.read_some(std::span<std::byte>(primed));
    int reader_calls_before = inner.calls;
    CountingWriter writer;
    int writer_calls_before = 0;
    std::vector<std::byte> scratch(8);
    auto res = cppio::copy_all(br, writer, std::span<std::byte>(scratch),
                               opts_deferred(cppio::CopyStrategy::VectorDeferred));
    CPPIO_CHECK(!res.has_value());
    CPPIO_CHECK(inner.calls == reader_calls_before);  // reader untouched
    CPPIO_CHECK(writer.calls == writer_calls_before);  // writer untouched
    CPPIO_CHECK(writer.sink.empty());
    // Buffered bytes left intact (not consumed).
    CPPIO_CHECK(br.peek_buffered().size() == 3);
}

CPPIO_TEST_CASE(deferred_strategy_fills_decision) {
    auto reader = cppio::MemoryReader::from_string("abc");
    CountingWriter writer;
    std::vector<std::byte> scratch(8);
    cppio::CopyDecision dec;
    auto res = cppio::copy_all(reader, writer, std::span<std::byte>(scratch),
                               opts_deferred(cppio::CopyStrategy::SendfileDeferred),
                               nullptr, &dec);
    CPPIO_CHECK(!res.has_value());
    CPPIO_CHECK(dec.requested == cppio::CopyStrategy::SendfileDeferred);
    CPPIO_CHECK(dec.unsupported_requested);
}

CPPIO_TEST_CASE(deferred_fallback_to_auto_executes_auto_behavior) {
    CountingReader inner("0123456789ABCDEF");
    std::vector<std::byte> rbuf(64);
    cppio::BufferedReader br(inner, rbuf);
    std::vector<std::byte> primed(4);
    (void)br.read_some(std::span<std::byte>(primed));  // 12 buffered

    CountingWriter writer;
    std::vector<std::byte> scratch(8);
    cppio::CopyDecision dec;
    auto res = cppio::copy_all(br, writer, std::span<std::byte>(scratch),
                               opts_deferred(cppio::CopyStrategy::VectorDeferred,
                                             cppio::UnsupportedStrategyPolicy::FallbackToAuto),
                               nullptr, &dec);
    CPPIO_CHECK(res.has_value());
    CPPIO_CHECK(res.value() == 12);  // Auto ran and copied
    CPPIO_CHECK(eq("456789ABCDEF", writer.sink));
}

CPPIO_TEST_CASE(deferred_fallback_to_auto_records_fallback_in_decision) {
    CountingReader inner("0123456789ABCDEF");
    std::vector<std::byte> rbuf(64);
    cppio::BufferedReader br(inner, rbuf);
    std::vector<std::byte> primed(4);
    (void)br.read_some(std::span<std::byte>(primed));

    CountingWriter writer;
    std::vector<std::byte> scratch(8);
    cppio::CopyDecision dec;
    auto res = cppio::copy_all(br, writer, std::span<std::byte>(scratch),
                               opts_deferred(cppio::CopyStrategy::SpliceDeferred,
                                             cppio::UnsupportedStrategyPolicy::FallbackToAuto),
                               nullptr, &dec);
    CPPIO_CHECK(res.has_value());
    CPPIO_CHECK(dec.requested == cppio::CopyStrategy::SpliceDeferred);
    CPPIO_CHECK(dec.unsupported_requested);
    CPPIO_CHECK(dec.selected == cppio::CopyStrategy::BufferedFirst);  // Auto resolves to it
    CPPIO_CHECK(dec.used_buffered_fast_path);
}

CPPIO_MAIN()
