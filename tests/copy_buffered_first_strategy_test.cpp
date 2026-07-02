// Tests for CopyStrategy::BufferedFirst and Auto (CPPIO-CORE-007D). BufferedFirst
// is the explicit form of the 006 fast path; Auto currently resolves to it. Both
// must drain buffered bytes first then fall back to scratch, and CopyDecision
// must reflect what ran. Behavior was implemented in 007C; this adds coverage.
#include "harness.hpp"

#include <cppio/buffer.hpp>
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

cppio::CopyOptions opts_with(cppio::CopyStrategy s, cppio::CopyLimit lim = cppio::CopyLimit::unlimited()) {
    cppio::CopyOptions o;
    o.strategy = s;
    o.limit = lim;
    return o;
}

}  // namespace

CPPIO_TEST_CASE(buffered_first_drains_buffered_bytes_before_scratch) {
    CountingReader inner("0123456789ABCDEF");
    std::vector<std::byte> rbuf(64);
    cppio::BufferedReader br(inner, rbuf);
    std::vector<std::byte> primed(4);
    (void)br.read_some(std::span<std::byte>(primed));  // 12 buffered remain

    cppio::MemoryWriter writer;
    std::vector<std::byte> scratch(8);
    cppio::CopyStats st;
    cppio::CopyDecision dec;
    auto res = cppio::copy_all(br, writer, std::span<std::byte>(scratch),
                               opts_with(cppio::CopyStrategy::BufferedFirst), &st, &dec);
    CPPIO_CHECK(res.has_value());
    CPPIO_CHECK(res.value() == 12);
    CPPIO_CHECK(eq("456789ABCDEF", writer.bytes()));
    CPPIO_CHECK(st.buffered_fast_path_bytes == 12);
    CPPIO_CHECK(dec.requested == cppio::CopyStrategy::BufferedFirst);
    CPPIO_CHECK(dec.selected == cppio::CopyStrategy::BufferedFirst);
    CPPIO_CHECK(dec.used_buffered_fast_path);
}

CPPIO_TEST_CASE(buffered_first_respects_limit_smaller_than_buffered) {
    CountingReader inner("0123456789ABCDEF");
    std::vector<std::byte> rbuf(64);
    cppio::BufferedReader br(inner, rbuf);
    std::vector<std::byte> primed(4);
    (void)br.read_some(std::span<std::byte>(primed));

    cppio::MemoryWriter writer;
    std::vector<std::byte> scratch(8);
    cppio::CopyDecision dec;
    auto res = cppio::copy_all(br, writer, std::span<std::byte>(scratch),
                               opts_with(cppio::CopyStrategy::BufferedFirst, cppio::CopyLimit::bytes(5)),
                               nullptr, &dec);
    CPPIO_CHECK(res.has_value());
    CPPIO_CHECK(res.value() == 5);
    CPPIO_CHECK(eq("45678", writer.bytes()));
    CPPIO_CHECK(br.peek_buffered().size() == 12 - 5);
}

CPPIO_TEST_CASE(buffered_first_nothing_limit_touches_nothing) {
    CountingReader inner("0123456789ABCDEF");
    std::vector<std::byte> rbuf(64);
    cppio::BufferedReader br(inner, rbuf);
    std::vector<std::byte> primed(4);
    (void)br.read_some(std::span<std::byte>(primed));
    int calls_before = inner.calls;

    cppio::MemoryWriter writer;
    std::vector<std::byte> scratch(8);
    cppio::CopyDecision dec;
    auto res = cppio::copy_all(br, writer, std::span<std::byte>(scratch),
                               opts_with(cppio::CopyStrategy::BufferedFirst, cppio::CopyLimit::nothing()),
                               nullptr, &dec);
    CPPIO_CHECK(res.has_value());
    CPPIO_CHECK(res.value() == 0);
    CPPIO_CHECK(writer.bytes().empty());
    CPPIO_CHECK(inner.calls == calls_before);
    CPPIO_CHECK(!dec.used_buffered_fast_path);
    CPPIO_CHECK(!dec.used_scratch_path);
}

CPPIO_TEST_CASE(buffered_first_writer_error_does_not_consume_buffered_bytes) {
    CountingReader inner("0123456789ABCDEF");
    std::vector<std::byte> rbuf(64);
    cppio::BufferedReader br(inner, rbuf);
    std::vector<std::byte> primed(4);
    (void)br.read_some(std::span<std::byte>(primed));
    std::size_t buffered_before = br.peek_buffered().size();

    struct FailWriter final : cppio::Writer {
        cppio::Result<std::size_t> write_some(std::span<const std::byte>) override {
            return cppio::make_unexpected<std::size_t>(
                cppio::IoError{cppio::IoError::Code::no_space});
        }
        cppio::Result<void> flush() override { return {}; }
    } writer;

    std::vector<std::byte> scratch(8);
    auto res = cppio::copy_all(br, writer, std::span<std::byte>(scratch),
                               opts_with(cppio::CopyStrategy::BufferedFirst));
    CPPIO_CHECK(!res.has_value());
    CPPIO_CHECK(res.error().code == cppio::IoError::Code::no_space);
    CPPIO_CHECK(br.peek_buffered().size() == buffered_before);  // not consumed
}

CPPIO_TEST_CASE(buffered_first_no_skipped_bytes) {
    CountingReader inner("ABCDEFGHIJKLMNOP");
    std::vector<std::byte> rbuf(64);
    cppio::BufferedReader br(inner, rbuf);
    std::vector<std::byte> primed(3);
    (void)br.read_some(std::span<std::byte>(primed));  // 13 buffered remain

    cppio::MemoryWriter writer;
    std::vector<std::byte> scratch(4);
    auto res = cppio::copy_all(br, writer, std::span<std::byte>(scratch),
                               opts_with(cppio::CopyStrategy::BufferedFirst));
    CPPIO_CHECK(res.has_value());
    CPPIO_CHECK(res.value() == 13);
    CPPIO_CHECK(eq("DEFGHIJKLMNOP", writer.bytes()));  // contiguous, no gap
}

CPPIO_TEST_CASE(buffered_first_no_duplicated_bytes) {
    // Buffer 64 > payload 16: all bytes drained from buffer in one fast-path
    // round; nothing should be duplicated.
    CountingReader inner("0123456789ABCDEF");
    std::vector<std::byte> rbuf(64);
    cppio::BufferedReader br(inner, rbuf);
    std::vector<std::byte> primed(1);
    (void)br.read_some(std::span<std::byte>(primed));  // 15 buffered remain

    cppio::MemoryWriter writer;
    std::vector<std::byte> scratch(8);
    auto res = cppio::copy_all(br, writer, std::span<std::byte>(scratch),
                               opts_with(cppio::CopyStrategy::BufferedFirst));
    CPPIO_CHECK(res.has_value());
    CPPIO_CHECK(res.value() == 15);
    CPPIO_CHECK(eq("123456789ABCDEF", writer.bytes()));  // each byte once
}

CPPIO_TEST_CASE(buffered_first_falls_back_to_scratch_when_no_buffered_bytes) {
    // Plain MemoryReader is not BufferedReadable: BufferedFirst must degrade to
    // scratch, not error.
    auto reader = cppio::MemoryReader::from_string("hello world");
    cppio::MemoryWriter writer;
    std::vector<std::byte> scratch(4);
    cppio::CopyDecision dec;
    auto res = cppio::copy_all(reader, writer, std::span<std::byte>(scratch),
                               opts_with(cppio::CopyStrategy::BufferedFirst), nullptr, &dec);
    CPPIO_CHECK(res.has_value());
    CPPIO_CHECK(res.value() == 11);
    CPPIO_CHECK(eq("hello world", writer.bytes()));
    CPPIO_CHECK(!dec.used_buffered_fast_path);
    CPPIO_CHECK(dec.used_scratch_path);
}

CPPIO_TEST_CASE(auto_strategy_behaves_like_buffered_first) {
    CountingReader inner("0123456789ABCDEF");
    std::vector<std::byte> rbuf(64);
    cppio::BufferedReader br(inner, rbuf);
    std::vector<std::byte> primed(4);
    (void)br.read_some(std::span<std::byte>(primed));

    cppio::MemoryWriter writer;
    std::vector<std::byte> scratch(8);
    cppio::CopyStats st;
    cppio::CopyDecision dec;
    auto res = cppio::copy_all(br, writer, std::span<std::byte>(scratch),
                               opts_with(cppio::CopyStrategy::Auto), &st, &dec);
    CPPIO_CHECK(res.has_value());
    CPPIO_CHECK(res.value() == 12);
    CPPIO_CHECK(eq("456789ABCDEF", writer.bytes()));
    CPPIO_CHECK(st.buffered_fast_path_bytes == 12);  // Auto engaged fast path
    CPPIO_CHECK(dec.requested == cppio::CopyStrategy::Auto);
    CPPIO_CHECK(dec.selected == cppio::CopyStrategy::BufferedFirst);  // Auto resolves to it
}

CPPIO_TEST_CASE(buffered_first_decision_records_both_paths_when_mixed) {
    // 16-byte source split across buffered and unbuffered: buffer holds 8 bytes
    // (rbuf=8), prime 2 leaves 6 buffered. BufferedFirst drains 6 from buffer
    // (fast path), then scratch reads the remaining 8 unbuffered bytes
    // (used_scratch_path=true because bytes actually moved through scratch).
    CountingReader inner("0123456789ABCDEF");
    std::vector<std::byte> rbuf(8);  // small: only first 8 buffered
    cppio::BufferedReader br(inner, rbuf);
    std::vector<std::byte> primed(2);
    (void)br.read_some(std::span<std::byte>(primed));  // 6 buffered remain

    cppio::MemoryWriter writer;
    std::vector<std::byte> scratch(4);
    cppio::CopyDecision dec;
    auto res = cppio::copy_all(br, writer, std::span<std::byte>(scratch),
                               opts_with(cppio::CopyStrategy::BufferedFirst), nullptr, &dec);
    CPPIO_CHECK(res.has_value());
    CPPIO_CHECK(res.value() == 14);  // 6 (buffered) + 8 (scratch refill)
    CPPIO_CHECK(eq("23456789ABCDEF", writer.bytes()));
    CPPIO_CHECK(dec.used_buffered_fast_path);  // drained the 6 buffered bytes
    CPPIO_CHECK(dec.used_scratch_path);        // 8 bytes moved through scratch
}

CPPIO_TEST_CASE(existing_006_fast_path_tests_still_pass) {
    // The CopyLimit overload delegates with Auto; a BufferedReader still gets
    // the fast path (Auto == BufferedFirst), preserving 006 behavior.
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
    CPPIO_CHECK(st.buffered_fast_path_bytes == 12);
}

CPPIO_MAIN()
