// Tests for deferred (reserved, not-yet-implemented) copy strategies
// (CPPIO-CORE-007E). VectorDeferred/FileRangeDeferred/SendfileDeferred/
// SpliceDeferred must NOT pretend to work: with the default policy they return
// invalid_state and touch nothing; with FallbackToAuto they mark the fallback
// and run Auto.
#include "harness.hpp"

#include <sluice/buffer.hpp>
#include <sluice/copy.hpp>
#include <sluice/copy_strategy.hpp>
#include <sluice/fault.hpp>
#include <sluice/limit.hpp>

#include <cstring>
#include <span>
#include <vector>

namespace {

class CountingReader final : public sluice::Reader {
public:
    sluice::MemoryReader mem;
    int calls = 0;
    explicit CountingReader(std::string_view s) : mem(sluice::MemoryReader::from_string(s)) {}
    sluice::Result<std::size_t> read_some(std::span<std::byte> dst) override {
        ++calls;
        return mem.read_some(dst);
    }
};

class CountingWriter final : public sluice::Writer {
public:
    std::vector<std::byte> sink;
    int calls = 0;
    sluice::Result<std::size_t> write_some(std::span<const std::byte> src) override {
        ++calls;
        sink.insert(sink.end(), src.begin(), src.end());
        return src.size();
    }
    sluice::Result<void> flush() override { return {}; }
};

bool eq(std::string_view s, const std::vector<std::byte>& b) {
    return b.size() == s.size() && std::memcmp(s.data(), b.data(), s.size()) == 0;
}

sluice::CopyOptions opts_deferred(sluice::CopyStrategy s,
                                 sluice::UnsupportedStrategyPolicy p =
                                     sluice::UnsupportedStrategyPolicy::ReturnInvalidState) {
    sluice::CopyOptions o;
    o.strategy = s;
    o.unsupported_policy = p;
    return o;
}

}  // namespace

SLUICE_TEST_CASE(vector_deferred_returns_invalid_state_by_default) {
    auto reader = sluice::MemoryReader::from_string("abc");
    CountingWriter writer;
    std::vector<std::byte> scratch(8);
    auto res = sluice::copy_all(reader, writer, std::span<std::byte>(scratch),
                               opts_deferred(sluice::CopyStrategy::VectorDeferred));
    SLUICE_CHECK(!res.has_value());
    SLUICE_CHECK(res.error().code == sluice::IoError::Code::invalid_state);
}

SLUICE_TEST_CASE(file_range_deferred_returns_invalid_state_by_default) {
    auto reader = sluice::MemoryReader::from_string("abc");
    CountingWriter writer;
    std::vector<std::byte> scratch(8);
    auto res = sluice::copy_all(reader, writer, std::span<std::byte>(scratch),
                               opts_deferred(sluice::CopyStrategy::FileRangeDeferred));
    SLUICE_CHECK(!res.has_value());
    SLUICE_CHECK(res.error().code == sluice::IoError::Code::invalid_state);
}

SLUICE_TEST_CASE(sendfile_deferred_returns_invalid_state_by_default) {
    auto reader = sluice::MemoryReader::from_string("abc");
    CountingWriter writer;
    std::vector<std::byte> scratch(8);
    auto res = sluice::copy_all(reader, writer, std::span<std::byte>(scratch),
                               opts_deferred(sluice::CopyStrategy::SendfileDeferred));
    SLUICE_CHECK(!res.has_value());
    SLUICE_CHECK(res.error().code == sluice::IoError::Code::invalid_state);
}

SLUICE_TEST_CASE(splice_deferred_returns_invalid_state_by_default) {
    auto reader = sluice::MemoryReader::from_string("abc");
    CountingWriter writer;
    std::vector<std::byte> scratch(8);
    auto res = sluice::copy_all(reader, writer, std::span<std::byte>(scratch),
                               opts_deferred(sluice::CopyStrategy::SpliceDeferred));
    SLUICE_CHECK(!res.has_value());
    SLUICE_CHECK(res.error().code == sluice::IoError::Code::invalid_state);
}

SLUICE_TEST_CASE(deferred_strategy_touches_neither_reader_nor_writer) {
    CountingReader inner("hello");
    std::vector<std::byte> rbuf(16);
    sluice::BufferedReader br(inner, rbuf);
    // Prime the buffer so a fallback would otherwise move bytes.
    std::vector<std::byte> primed(2);
    (void)br.read_some(std::span<std::byte>(primed));
    int reader_calls_before = inner.calls;
    CountingWriter writer;
    int writer_calls_before = 0;
    std::vector<std::byte> scratch(8);
    auto res = sluice::copy_all(br, writer, std::span<std::byte>(scratch),
                               opts_deferred(sluice::CopyStrategy::VectorDeferred));
    SLUICE_CHECK(!res.has_value());
    SLUICE_CHECK(inner.calls == reader_calls_before);  // reader untouched
    SLUICE_CHECK(writer.calls == writer_calls_before);  // writer untouched
    SLUICE_CHECK(writer.sink.empty());
    // Buffered bytes left intact (not consumed).
    SLUICE_CHECK(br.peek_buffered().size() == 3);
}

SLUICE_TEST_CASE(deferred_strategy_fills_decision) {
    auto reader = sluice::MemoryReader::from_string("abc");
    CountingWriter writer;
    std::vector<std::byte> scratch(8);
    sluice::CopyDecision dec;
    auto res = sluice::copy_all(reader, writer, std::span<std::byte>(scratch),
                               opts_deferred(sluice::CopyStrategy::SendfileDeferred),
                               nullptr, &dec);
    SLUICE_CHECK(!res.has_value());
    SLUICE_CHECK(dec.requested == sluice::CopyStrategy::SendfileDeferred);
    SLUICE_CHECK(dec.unsupported_requested);
}

SLUICE_TEST_CASE(deferred_fallback_to_auto_executes_auto_behavior) {
    CountingReader inner("0123456789ABCDEF");
    std::vector<std::byte> rbuf(64);
    sluice::BufferedReader br(inner, rbuf);
    std::vector<std::byte> primed(4);
    (void)br.read_some(std::span<std::byte>(primed));  // 12 buffered

    CountingWriter writer;
    std::vector<std::byte> scratch(8);
    sluice::CopyDecision dec;
    auto res = sluice::copy_all(br, writer, std::span<std::byte>(scratch),
                               opts_deferred(sluice::CopyStrategy::VectorDeferred,
                                             sluice::UnsupportedStrategyPolicy::FallbackToAuto),
                               nullptr, &dec);
    SLUICE_CHECK(res.has_value());
    SLUICE_CHECK(res.value() == 12);  // Auto ran and copied
    SLUICE_CHECK(eq("456789ABCDEF", writer.sink));
}

SLUICE_TEST_CASE(deferred_fallback_to_auto_records_fallback_in_decision) {
    CountingReader inner("0123456789ABCDEF");
    std::vector<std::byte> rbuf(64);
    sluice::BufferedReader br(inner, rbuf);
    std::vector<std::byte> primed(4);
    (void)br.read_some(std::span<std::byte>(primed));

    CountingWriter writer;
    std::vector<std::byte> scratch(8);
    sluice::CopyDecision dec;
    auto res = sluice::copy_all(br, writer, std::span<std::byte>(scratch),
                               opts_deferred(sluice::CopyStrategy::SpliceDeferred,
                                             sluice::UnsupportedStrategyPolicy::FallbackToAuto),
                               nullptr, &dec);
    SLUICE_CHECK(res.has_value());
    SLUICE_CHECK(dec.requested == sluice::CopyStrategy::SpliceDeferred);
    SLUICE_CHECK(dec.unsupported_requested);
    SLUICE_CHECK(dec.selected == sluice::CopyStrategy::BufferedFirst);  // Auto resolves to it
    SLUICE_CHECK(dec.used_buffered_fast_path);
}

SLUICE_MAIN()
