// Tests for CopyStrategy::Scratch routing (CPPIO-CORE-007C). Verifies the
// strategy-aware copy_all overload forces the scratch path (never the buffered
// fast path) even when the reader implements BufferedReadable, that the
// existing overload behavior is unchanged, and that CopyDecision is filled.
#include "harness.hpp"

#include <cppio/buffer.hpp>
#include <cppio/buffered_readable.hpp>
#include <cppio/copy.hpp>
#include <cppio/copy_strategy.hpp>
#include <cppio/fault.hpp>
#include <cppio/limit.hpp>
#include <cppio/measurement.hpp>

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

bool eq(std::string_view s, const std::vector<std::byte>& b) {
    return b.size() == s.size() && std::memcmp(s.data(), b.data(), s.size()) == 0;
}

}  // namespace

CPPIO_TEST_CASE(scratch_strategy_copies_exact_bytes) {
    auto reader = cppio::MemoryReader::from_string("hello world");
    cppio::MemoryWriter writer;
    std::vector<std::byte> scratch(4);
    cppio::CopyDecision dec;
    auto res = cppio::copy_all(reader, writer, std::span<std::byte>(scratch),
                               cppio::CopyOptions{cppio::CopyLimit::unlimited(),
                                                  cppio::CopyStrategy::Scratch},
                               nullptr, &dec);
    CPPIO_CHECK(res.has_value());
    CPPIO_CHECK(res.value() == 11);
    CPPIO_CHECK(eq("hello world", writer.bytes()));
    CPPIO_CHECK(dec.requested == cppio::CopyStrategy::Scratch);
    CPPIO_CHECK(dec.selected == cppio::CopyStrategy::Scratch);
    CPPIO_CHECK(dec.used_scratch_path);
    CPPIO_CHECK(!dec.used_buffered_fast_path);
}

CPPIO_TEST_CASE(scratch_strategy_respects_byte_limit) {
    auto reader = cppio::MemoryReader::from_string("0123456789");
    cppio::MemoryWriter writer;
    std::vector<std::byte> scratch(8);
    cppio::CopyOptions opts;
    opts.strategy = cppio::CopyStrategy::Scratch;
    opts.limit = cppio::CopyLimit::bytes(4);
    cppio::CopyDecision dec;
    auto res = cppio::copy_all(reader, writer, std::span<std::byte>(scratch), opts, nullptr, &dec);
    CPPIO_CHECK(res.has_value());
    CPPIO_CHECK(res.value() == 4);
    CPPIO_CHECK(eq("0123", writer.bytes()));
    CPPIO_CHECK(dec.selected == cppio::CopyStrategy::Scratch);
}

CPPIO_TEST_CASE(scratch_strategy_nothing_limit_touches_nothing) {
    CountingReader inner("hello");
    std::vector<std::byte> rbuf(16);
    cppio::BufferedReader br(inner, rbuf);
    std::vector<std::byte> primed(2);
    (void)br.read_some(std::span<std::byte>(primed));
    int calls_before = inner.calls;

    cppio::MemoryWriter writer;
    std::vector<std::byte> scratch(8);
    cppio::CopyOptions opts;
    opts.strategy = cppio::CopyStrategy::Scratch;
    opts.limit = cppio::CopyLimit::nothing();
    cppio::CopyDecision dec;
    auto res = cppio::copy_all(br, writer, std::span<std::byte>(scratch), opts, nullptr, &dec);
    CPPIO_CHECK(res.has_value());
    CPPIO_CHECK(res.value() == 0);
    CPPIO_CHECK(writer.bytes().empty());
    CPPIO_CHECK(inner.calls == calls_before);  // nothing touched no reader
    // selected may be Scratch, but no path was actually used.
    CPPIO_CHECK(dec.selected == cppio::CopyStrategy::Scratch);
    CPPIO_CHECK(!dec.used_scratch_path);
    CPPIO_CHECK(!dec.used_buffered_fast_path);
}

CPPIO_TEST_CASE(scratch_strategy_propagates_reader_error) {
    cppio::MemoryReader inner = cppio::MemoryReader::from_string("abcdef");
    cppio::FaultPlan plan;
    plan.fail_after_bytes = 3;
    plan.error = cppio::IoError{cppio::IoError::Code::canceled};
    cppio::FaultReader fr(inner, plan);
    cppio::MemoryWriter writer;
    std::vector<std::byte> scratch(2);
    cppio::CopyOptions opts;
    opts.strategy = cppio::CopyStrategy::Scratch;
    auto res = cppio::copy_all(fr, writer, std::span<std::byte>(scratch), opts);
    CPPIO_CHECK(!res.has_value());
    CPPIO_CHECK(res.error().code == cppio::IoError::Code::canceled);
}

CPPIO_TEST_CASE(scratch_strategy_propagates_writer_error) {
    auto reader = cppio::MemoryReader::from_string("abcdef");
    struct FailWriter final : cppio::Writer {
        int calls = 0;
        cppio::Result<std::size_t> write_some(std::span<const std::byte>) override {
            if (calls++ == 0) return std::size_t{3};
            return cppio::make_unexpected<std::size_t>(
                cppio::IoError{cppio::IoError::Code::no_space});
        }
        cppio::Result<void> flush() override { return {}; }
    } writer;
    std::vector<std::byte> scratch(8);
    cppio::CopyOptions opts;
    opts.strategy = cppio::CopyStrategy::Scratch;
    auto res = cppio::copy_all(reader, writer, std::span<std::byte>(scratch), opts);
    CPPIO_CHECK(!res.has_value());
    CPPIO_CHECK(res.error().code == cppio::IoError::Code::no_space);
}

CPPIO_TEST_CASE(scratch_strategy_avoids_buffered_fast_path_even_when_buffered_readable) {
    // BufferedReader implements BufferedReadable; Scratch must NOT touch it and
    // instead read through the wrapper's read_some (which serves from buffer but
    // does NOT use peek_buffered/consume_buffered — i.e. no fast path).
    CountingReader inner("0123456789ABCDEF");
    std::vector<std::byte> rbuf(64);
    cppio::BufferedReader br(inner, rbuf);
    std::vector<std::byte> primed(4);
    (void)br.read_some(std::span<std::byte>(primed));  // 12 buffered remain
    // With Scratch, copy_all must not drain via peek/consume: the buffer's
    // unread region stays intact (only read_some advances it).
    cppio::MemoryWriter writer;
    std::vector<std::byte> scratch(8);
    cppio::CopyOptions opts;
    opts.strategy = cppio::CopyStrategy::Scratch;
    cppio::CopyDecision dec;
    auto res = cppio::copy_all(br, writer, std::span<std::byte>(scratch), opts, nullptr, &dec);
    CPPIO_CHECK(res.has_value());
    CPPIO_CHECK(res.value() == 12);
    CPPIO_CHECK(eq("456789ABCDEF", writer.bytes()));
    CPPIO_CHECK(!dec.used_buffered_fast_path);
    CPPIO_CHECK(dec.used_scratch_path);
    CPPIO_CHECK(br.peek_buffered().empty());  // drained through read_some
}

CPPIO_TEST_CASE(existing_copy_all_overload_preserves_behavior) {
    // The old overload (CopyLimit) must still behave identically: it delegates
    // to CopyOptions{limit, Auto}, and Auto currently == BufferedFirst, so a
    // BufferedReader still gets the fast path.
    CountingReader inner("0123456789ABCDEF");
    std::vector<std::byte> rbuf(64);
    cppio::BufferedReader br(inner, rbuf);
    std::vector<std::byte> primed(4);
    (void)br.read_some(std::span<std::byte>(primed));
    cppio::MemoryWriter writer;
    std::vector<std::byte> scratch(8);
    cppio::CopyStats st;
    auto res = cppio::copy_all(br, writer, std::span<std::byte>(scratch),
                               cppio::CopyLimit::unlimited(), &st);
    CPPIO_CHECK(res.has_value());
    CPPIO_CHECK(res.value() == 12);
    CPPIO_CHECK(eq("456789ABCDEF", writer.bytes()));
    // Auto == BufferedFirst this stage: fast path engaged.
    CPPIO_CHECK(st.buffered_fast_path_bytes == 12);
}

CPPIO_MAIN()
