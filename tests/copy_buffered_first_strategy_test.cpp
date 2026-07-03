// Tests for CopyStrategy::BufferedFirst and Auto (CPPIO-CORE-007D). BufferedFirst
// is the explicit form of the 006 fast path; Auto currently resolves to it. Both
// must drain buffered bytes first then fall back to scratch, and CopyDecision
// must reflect what ran. Behavior was implemented in 007C; this adds coverage.
#include "harness.hpp"

#include <sluice/buffer.hpp>
#include <sluice/copy.hpp>
#include <sluice/copy_strategy.hpp>
#include <sluice/fault.hpp>
#include <sluice/limit.hpp>
#include <sluice/measurement.hpp>

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

bool eq(std::string_view s, const std::vector<std::byte>& b) {
    return b.size() == s.size() && std::memcmp(s.data(), b.data(), s.size()) == 0;
}

sluice::CopyOptions opts_with(sluice::CopyStrategy s, sluice::CopyLimit lim = sluice::CopyLimit::unlimited()) {
    sluice::CopyOptions o;
    o.strategy = s;
    o.limit = lim;
    return o;
}

}  // namespace

SLUICE_TEST_CASE(buffered_first_drains_buffered_bytes_before_scratch) {
    CountingReader inner("0123456789ABCDEF");
    std::vector<std::byte> rbuf(64);
    sluice::BufferedReader br(inner, rbuf);
    std::vector<std::byte> primed(4);
    (void)br.read_some(std::span<std::byte>(primed));  // 12 buffered remain

    sluice::MemoryWriter writer;
    std::vector<std::byte> scratch(8);
    sluice::CopyStats st;
    sluice::CopyDecision dec;
    auto res = sluice::copy_all(br, writer, std::span<std::byte>(scratch),
                               opts_with(sluice::CopyStrategy::BufferedFirst), &st, &dec);
    SLUICE_CHECK(res.has_value());
    SLUICE_CHECK(res.value() == 12);
    SLUICE_CHECK(eq("456789ABCDEF", writer.bytes()));
    SLUICE_CHECK(st.buffered_fast_path_bytes == 12);
    SLUICE_CHECK(dec.requested == sluice::CopyStrategy::BufferedFirst);
    SLUICE_CHECK(dec.selected == sluice::CopyStrategy::BufferedFirst);
    SLUICE_CHECK(dec.used_buffered_fast_path);
}

SLUICE_TEST_CASE(buffered_first_respects_limit_smaller_than_buffered) {
    CountingReader inner("0123456789ABCDEF");
    std::vector<std::byte> rbuf(64);
    sluice::BufferedReader br(inner, rbuf);
    std::vector<std::byte> primed(4);
    (void)br.read_some(std::span<std::byte>(primed));

    sluice::MemoryWriter writer;
    std::vector<std::byte> scratch(8);
    sluice::CopyDecision dec;
    auto res = sluice::copy_all(br, writer, std::span<std::byte>(scratch),
                               opts_with(sluice::CopyStrategy::BufferedFirst, sluice::CopyLimit::bytes(5)),
                               nullptr, &dec);
    SLUICE_CHECK(res.has_value());
    SLUICE_CHECK(res.value() == 5);
    SLUICE_CHECK(eq("45678", writer.bytes()));
    SLUICE_CHECK(br.peek_buffered().size() == 12 - 5);
}

SLUICE_TEST_CASE(buffered_first_nothing_limit_touches_nothing) {
    CountingReader inner("0123456789ABCDEF");
    std::vector<std::byte> rbuf(64);
    sluice::BufferedReader br(inner, rbuf);
    std::vector<std::byte> primed(4);
    (void)br.read_some(std::span<std::byte>(primed));
    int calls_before = inner.calls;

    sluice::MemoryWriter writer;
    std::vector<std::byte> scratch(8);
    sluice::CopyDecision dec;
    auto res = sluice::copy_all(br, writer, std::span<std::byte>(scratch),
                               opts_with(sluice::CopyStrategy::BufferedFirst, sluice::CopyLimit::nothing()),
                               nullptr, &dec);
    SLUICE_CHECK(res.has_value());
    SLUICE_CHECK(res.value() == 0);
    SLUICE_CHECK(writer.bytes().empty());
    SLUICE_CHECK(inner.calls == calls_before);
    SLUICE_CHECK(!dec.used_buffered_fast_path);
    SLUICE_CHECK(!dec.used_scratch_path);
}

SLUICE_TEST_CASE(buffered_first_writer_error_does_not_consume_buffered_bytes) {
    CountingReader inner("0123456789ABCDEF");
    std::vector<std::byte> rbuf(64);
    sluice::BufferedReader br(inner, rbuf);
    std::vector<std::byte> primed(4);
    (void)br.read_some(std::span<std::byte>(primed));
    std::size_t buffered_before = br.peek_buffered().size();

    struct FailWriter final : sluice::Writer {
        sluice::Result<std::size_t> write_some(std::span<const std::byte>) override {
            return sluice::make_unexpected<std::size_t>(
                sluice::IoError{sluice::IoError::Code::no_space});
        }
        sluice::Result<void> flush() override { return {}; }
    } writer;

    std::vector<std::byte> scratch(8);
    auto res = sluice::copy_all(br, writer, std::span<std::byte>(scratch),
                               opts_with(sluice::CopyStrategy::BufferedFirst));
    SLUICE_CHECK(!res.has_value());
    SLUICE_CHECK(res.error().code == sluice::IoError::Code::no_space);
    SLUICE_CHECK(br.peek_buffered().size() == buffered_before);  // not consumed
}

SLUICE_TEST_CASE(buffered_first_no_skipped_bytes) {
    CountingReader inner("ABCDEFGHIJKLMNOP");
    std::vector<std::byte> rbuf(64);
    sluice::BufferedReader br(inner, rbuf);
    std::vector<std::byte> primed(3);
    (void)br.read_some(std::span<std::byte>(primed));  // 13 buffered remain

    sluice::MemoryWriter writer;
    std::vector<std::byte> scratch(4);
    auto res = sluice::copy_all(br, writer, std::span<std::byte>(scratch),
                               opts_with(sluice::CopyStrategy::BufferedFirst));
    SLUICE_CHECK(res.has_value());
    SLUICE_CHECK(res.value() == 13);
    SLUICE_CHECK(eq("DEFGHIJKLMNOP", writer.bytes()));  // contiguous, no gap
}

SLUICE_TEST_CASE(buffered_first_no_duplicated_bytes) {
    // Buffer 64 > payload 16: all bytes drained from buffer in one fast-path
    // round; nothing should be duplicated.
    CountingReader inner("0123456789ABCDEF");
    std::vector<std::byte> rbuf(64);
    sluice::BufferedReader br(inner, rbuf);
    std::vector<std::byte> primed(1);
    (void)br.read_some(std::span<std::byte>(primed));  // 15 buffered remain

    sluice::MemoryWriter writer;
    std::vector<std::byte> scratch(8);
    auto res = sluice::copy_all(br, writer, std::span<std::byte>(scratch),
                               opts_with(sluice::CopyStrategy::BufferedFirst));
    SLUICE_CHECK(res.has_value());
    SLUICE_CHECK(res.value() == 15);
    SLUICE_CHECK(eq("123456789ABCDEF", writer.bytes()));  // each byte once
}

SLUICE_TEST_CASE(buffered_first_falls_back_to_scratch_when_no_buffered_bytes) {
    // Plain MemoryReader is not BufferedReadable: BufferedFirst must degrade to
    // scratch, not error.
    auto reader = sluice::MemoryReader::from_string("hello world");
    sluice::MemoryWriter writer;
    std::vector<std::byte> scratch(4);
    sluice::CopyDecision dec;
    auto res = sluice::copy_all(reader, writer, std::span<std::byte>(scratch),
                               opts_with(sluice::CopyStrategy::BufferedFirst), nullptr, &dec);
    SLUICE_CHECK(res.has_value());
    SLUICE_CHECK(res.value() == 11);
    SLUICE_CHECK(eq("hello world", writer.bytes()));
    SLUICE_CHECK(!dec.used_buffered_fast_path);
    SLUICE_CHECK(dec.used_scratch_path);
}

SLUICE_TEST_CASE(auto_strategy_behaves_like_buffered_first) {
    CountingReader inner("0123456789ABCDEF");
    std::vector<std::byte> rbuf(64);
    sluice::BufferedReader br(inner, rbuf);
    std::vector<std::byte> primed(4);
    (void)br.read_some(std::span<std::byte>(primed));

    sluice::MemoryWriter writer;
    std::vector<std::byte> scratch(8);
    sluice::CopyStats st;
    sluice::CopyDecision dec;
    auto res = sluice::copy_all(br, writer, std::span<std::byte>(scratch),
                               opts_with(sluice::CopyStrategy::Auto), &st, &dec);
    SLUICE_CHECK(res.has_value());
    SLUICE_CHECK(res.value() == 12);
    SLUICE_CHECK(eq("456789ABCDEF", writer.bytes()));
    SLUICE_CHECK(st.buffered_fast_path_bytes == 12);  // Auto engaged fast path
    SLUICE_CHECK(dec.requested == sluice::CopyStrategy::Auto);
    SLUICE_CHECK(dec.selected == sluice::CopyStrategy::BufferedFirst);  // Auto resolves to it
}

SLUICE_TEST_CASE(buffered_first_decision_records_both_paths_when_mixed) {
    // 16-byte source split across buffered and unbuffered: buffer holds 8 bytes
    // (rbuf=8), prime 2 leaves 6 buffered. BufferedFirst drains 6 from buffer
    // (fast path), then scratch reads the remaining 8 unbuffered bytes
    // (used_scratch_path=true because bytes actually moved through scratch).
    CountingReader inner("0123456789ABCDEF");
    std::vector<std::byte> rbuf(8);  // small: only first 8 buffered
    sluice::BufferedReader br(inner, rbuf);
    std::vector<std::byte> primed(2);
    (void)br.read_some(std::span<std::byte>(primed));  // 6 buffered remain

    sluice::MemoryWriter writer;
    std::vector<std::byte> scratch(4);
    sluice::CopyDecision dec;
    auto res = sluice::copy_all(br, writer, std::span<std::byte>(scratch),
                               opts_with(sluice::CopyStrategy::BufferedFirst), nullptr, &dec);
    SLUICE_CHECK(res.has_value());
    SLUICE_CHECK(res.value() == 14);  // 6 (buffered) + 8 (scratch refill)
    SLUICE_CHECK(eq("23456789ABCDEF", writer.bytes()));
    SLUICE_CHECK(dec.used_buffered_fast_path);  // drained the 6 buffered bytes
    SLUICE_CHECK(dec.used_scratch_path);        // 8 bytes moved through scratch
}

SLUICE_TEST_CASE(existing_006_fast_path_tests_still_pass) {
    // The CopyLimit overload delegates with Auto; a BufferedReader still gets
    // the fast path (Auto == BufferedFirst), preserving 006 behavior.
    CountingReader inner("0123456789ABCDEF");
    std::vector<std::byte> rbuf(64);
    sluice::BufferedReader br(inner, rbuf);
    std::vector<std::byte> primed(4);
    (void)br.read_some(std::span<std::byte>(primed));
    sluice::MemoryWriter writer;
    std::vector<std::byte> scratch(8);
    sluice::CopyStats st;
    auto res = sluice::copy_all(br, writer, std::span<std::byte>(scratch),
                               sluice::CopyLimit::unlimited(), &st);
    SLUICE_CHECK(res.has_value());
    SLUICE_CHECK(res.value() == 12);
    SLUICE_CHECK(st.buffered_fast_path_bytes == 12);
}

SLUICE_MAIN()
