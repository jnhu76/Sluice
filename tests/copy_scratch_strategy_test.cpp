// Tests for CopyStrategy::Scratch routing (CPPIO-CORE-007C). Verifies the
// strategy-aware copy_all overload forces the scratch path (never the buffered
// fast path) even when the reader implements BufferedReadable, that the
// existing overload behavior is unchanged, and that CopyDecision is filled.
#include "harness.hpp"

#include <sluice/buffer.hpp>
#include <sluice/buffered_readable.hpp>
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

}  // namespace

SLUICE_TEST_CASE(scratch_strategy_copies_exact_bytes) {
    auto reader = sluice::MemoryReader::from_string("hello world");
    sluice::MemoryWriter writer;
    std::vector<std::byte> scratch(4);
    sluice::CopyDecision dec;
    auto res = sluice::copy_all(reader, writer, std::span<std::byte>(scratch),
                               sluice::CopyOptions{sluice::CopyLimit::unlimited(),
                                                  sluice::CopyStrategy::Scratch},
                               nullptr, &dec);
    SLUICE_CHECK(res.has_value());
    SLUICE_CHECK(res.value() == 11);
    SLUICE_CHECK(eq("hello world", writer.bytes()));
    SLUICE_CHECK(dec.requested == sluice::CopyStrategy::Scratch);
    SLUICE_CHECK(dec.selected == sluice::CopyStrategy::Scratch);
    SLUICE_CHECK(dec.used_scratch_path);
    SLUICE_CHECK(!dec.used_buffered_fast_path);
}

SLUICE_TEST_CASE(scratch_strategy_respects_byte_limit) {
    auto reader = sluice::MemoryReader::from_string("0123456789");
    sluice::MemoryWriter writer;
    std::vector<std::byte> scratch(8);
    sluice::CopyOptions opts;
    opts.strategy = sluice::CopyStrategy::Scratch;
    opts.limit = sluice::CopyLimit::bytes(4);
    sluice::CopyDecision dec;
    auto res = sluice::copy_all(reader, writer, std::span<std::byte>(scratch), opts, nullptr, &dec);
    SLUICE_CHECK(res.has_value());
    SLUICE_CHECK(res.value() == 4);
    SLUICE_CHECK(eq("0123", writer.bytes()));
    SLUICE_CHECK(dec.selected == sluice::CopyStrategy::Scratch);
}

SLUICE_TEST_CASE(scratch_strategy_nothing_limit_touches_nothing) {
    CountingReader inner("hello");
    std::vector<std::byte> rbuf(16);
    sluice::BufferedReader br(inner, rbuf);
    std::vector<std::byte> primed(2);
    (void)br.read_some(std::span<std::byte>(primed));
    int calls_before = inner.calls;

    sluice::MemoryWriter writer;
    std::vector<std::byte> scratch(8);
    sluice::CopyOptions opts;
    opts.strategy = sluice::CopyStrategy::Scratch;
    opts.limit = sluice::CopyLimit::nothing();
    sluice::CopyDecision dec;
    auto res = sluice::copy_all(br, writer, std::span<std::byte>(scratch), opts, nullptr, &dec);
    SLUICE_CHECK(res.has_value());
    SLUICE_CHECK(res.value() == 0);
    SLUICE_CHECK(writer.bytes().empty());
    SLUICE_CHECK(inner.calls == calls_before);  // nothing touched no reader
    // selected may be Scratch, but no path was actually used.
    SLUICE_CHECK(dec.selected == sluice::CopyStrategy::Scratch);
    SLUICE_CHECK(!dec.used_scratch_path);
    SLUICE_CHECK(!dec.used_buffered_fast_path);
}

SLUICE_TEST_CASE(scratch_strategy_propagates_reader_error) {
    sluice::MemoryReader inner = sluice::MemoryReader::from_string("abcdef");
    sluice::FaultPlan plan;
    plan.fail_after_bytes = 3;
    plan.error = sluice::IoError{sluice::IoError::Code::canceled};
    sluice::FaultReader fr(inner, plan);
    sluice::MemoryWriter writer;
    std::vector<std::byte> scratch(2);
    sluice::CopyOptions opts;
    opts.strategy = sluice::CopyStrategy::Scratch;
    auto res = sluice::copy_all(fr, writer, std::span<std::byte>(scratch), opts);
    SLUICE_CHECK(!res.has_value());
    SLUICE_CHECK(res.error().code == sluice::IoError::Code::canceled);
}

SLUICE_TEST_CASE(scratch_strategy_propagates_writer_error) {
    auto reader = sluice::MemoryReader::from_string("abcdef");
    struct FailWriter final : sluice::Writer {
        int calls = 0;
        sluice::Result<std::size_t> write_some(std::span<const std::byte>) override {
            if (calls++ == 0) return std::size_t{3};
            return sluice::make_unexpected<std::size_t>(
                sluice::IoError{sluice::IoError::Code::no_space});
        }
        sluice::Result<void> flush() override { return {}; }
    } writer;
    std::vector<std::byte> scratch(8);
    sluice::CopyOptions opts;
    opts.strategy = sluice::CopyStrategy::Scratch;
    auto res = sluice::copy_all(reader, writer, std::span<std::byte>(scratch), opts);
    SLUICE_CHECK(!res.has_value());
    SLUICE_CHECK(res.error().code == sluice::IoError::Code::no_space);
}

SLUICE_TEST_CASE(scratch_strategy_avoids_buffered_fast_path_even_when_buffered_readable) {
    // BufferedReader implements BufferedReadable; Scratch must NOT touch it and
    // instead read through the wrapper's read_some (which serves from buffer but
    // does NOT use peek_buffered/consume_buffered — i.e. no fast path).
    CountingReader inner("0123456789ABCDEF");
    std::vector<std::byte> rbuf(64);
    sluice::BufferedReader br(inner, rbuf);
    std::vector<std::byte> primed(4);
    (void)br.read_some(std::span<std::byte>(primed));  // 12 buffered remain
    // With Scratch, copy_all must not drain via peek/consume: the buffer's
    // unread region stays intact (only read_some advances it).
    sluice::MemoryWriter writer;
    std::vector<std::byte> scratch(8);
    sluice::CopyOptions opts;
    opts.strategy = sluice::CopyStrategy::Scratch;
    sluice::CopyDecision dec;
    auto res = sluice::copy_all(br, writer, std::span<std::byte>(scratch), opts, nullptr, &dec);
    SLUICE_CHECK(res.has_value());
    SLUICE_CHECK(res.value() == 12);
    SLUICE_CHECK(eq("456789ABCDEF", writer.bytes()));
    SLUICE_CHECK(!dec.used_buffered_fast_path);
    SLUICE_CHECK(dec.used_scratch_path);
    SLUICE_CHECK(br.peek_buffered().empty());  // drained through read_some
}

SLUICE_TEST_CASE(existing_copy_all_overload_preserves_behavior) {
    // The old overload (CopyLimit) must still behave identically: it delegates
    // to CopyOptions{limit, Auto}, and Auto currently == BufferedFirst, so a
    // BufferedReader still gets the fast path.
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
    SLUICE_CHECK(eq("456789ABCDEF", writer.bytes()));
    // Auto == BufferedFirst this stage: fast path engaged.
    SLUICE_CHECK(st.buffered_fast_path_bytes == 12);
}

SLUICE_MAIN()
