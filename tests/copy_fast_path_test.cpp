// Tests for copy_all's buffered fast path (CPPIO-CORE-006D). The fast path drains
// already-buffered unread bytes via BufferedReadable before falling back to the
// scratch read path. These verify behavior through the public copy_all API,
// composing BufferedReader (which implements BufferedReadable) with inner
// counting readers/writers.
#include "harness.hpp"

#include <cppio/buffer.hpp>
#include <cppio/copy.hpp>
#include <cppio/fault.hpp>
#include <cppio/limit.hpp>
#include <cppio/measurement.hpp>
#include <cppio/reader.hpp>
#include <cppio/writer.hpp>

#include <algorithm>
#include <cstring>
#include <cstddef>
#include <span>
#include <vector>

namespace {

// Reader that counts read_some calls, to prove the fast path avoids inner reads
// while buffered bytes are available.
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

CPPIO_TEST_CASE(copy_fast_path_drains_buffered_bytes_before_scratch_read) {
    // Source has 16 bytes; BufferedReader refill pulls more into its buffer than
    // a tiny read consumes, leaving buffered unread bytes. copy_all must drain
    // those via peek/consume. The drain itself does not call the inner reader;
    // the only extra inner call after draining is the single EOF probe once the
    // buffer is empty (unavoidable for a blocking reader to learn the stream is
    // done).
    CountingReader inner("0123456789ABCDEF");
    std::vector<std::byte> rbuf(64);
    cppio::BufferedReader br(inner, rbuf);

    // Prime the buffer: a small read forces a refill that pulls all 16 bytes in.
    std::vector<std::byte> primed(4);
    auto pr = br.read_some(std::span<std::byte>(primed));
    CPPIO_CHECK(pr.has_value() && pr.value() == 4);
    CPPIO_CHECK(br.peek_buffered().size() == 12);  // 12 buffered unread bytes remain
    int inner_calls_after_prime = inner.calls;

    cppio::MemoryWriter writer;
    std::vector<std::byte> scratch(8);
    auto res = cppio::copy_all(br, writer, std::span<std::byte>(scratch),
                               cppio::CopyLimit::unlimited(), nullptr);
    CPPIO_CHECK(res.has_value());
    CPPIO_CHECK(res.value() == 12);  // copied the buffered remainder only
    CPPIO_CHECK(eq("456789ABCDEF", writer.bytes()));
    // The drain performed zero inner reads; exactly one EOF probe happens once
    // the buffered region empties. Anything more would mean the fast path fell
    // back to the scratch loop for bytes it should have served from the buffer.
    CPPIO_CHECK(inner.calls == inner_calls_after_prime + 1);
}

CPPIO_TEST_CASE(copy_fast_path_limit_smaller_than_buffered_copies_only_limit) {
    CountingReader inner("0123456789ABCDEF");
    std::vector<std::byte> rbuf(64);
    cppio::BufferedReader br(inner, rbuf);
    std::vector<std::byte> primed(4);
    (void)br.read_some(std::span<std::byte>(primed));  // 12 buffered remain

    cppio::MemoryWriter writer;
    std::vector<std::byte> scratch(8);
    auto res = cppio::copy_all(br, writer, std::span<std::byte>(scratch),
                               cppio::CopyLimit::bytes(5), nullptr);
    CPPIO_CHECK(res.has_value());
    CPPIO_CHECK(res.value() == 5);
    CPPIO_CHECK(eq("45678", writer.bytes()));
    // Remaining buffered bytes are unconsumed past the limit.
    CPPIO_CHECK(br.peek_buffered().size() == 12 - 5);
}

CPPIO_TEST_CASE(copy_fast_path_nothing_limit_touches_nothing_and_does_not_consume) {
    CountingReader inner("0123456789ABCDEF");
    std::vector<std::byte> rbuf(64);
    cppio::BufferedReader br(inner, rbuf);
    std::vector<std::byte> primed(4);
    (void)br.read_some(std::span<std::byte>(primed));
    CPPIO_CHECK(br.peek_buffered().size() == 12);

    cppio::MemoryWriter writer;
    std::vector<std::byte> scratch(8);
    auto res = cppio::copy_all(br, writer, std::span<std::byte>(scratch),
                               cppio::CopyLimit::nothing(), nullptr);
    CPPIO_CHECK(res.has_value());
    CPPIO_CHECK(res.value() == 0);
    CPPIO_CHECK(writer.bytes().empty());
    CPPIO_CHECK(br.peek_buffered().size() == 12);  // nothing consumed
}

CPPIO_TEST_CASE(copy_fast_path_writer_error_does_not_consume_buffered_bytes) {
    // A writer that fails on the first write. Per the writer-error rule, the
    // fast path must NOT consume any buffered bytes (write_all does not expose
    // partial completion), and must return the writer error.
    CountingReader inner("0123456789ABCDEF");
    std::vector<std::byte> rbuf(64);
    cppio::BufferedReader br(inner, rbuf);
    std::vector<std::byte> primed(4);
    (void)br.read_some(std::span<std::byte>(primed));
    std::size_t buffered_before = br.peek_buffered().size();
    CPPIO_CHECK(buffered_before > 0);

    struct FailWriter final : cppio::Writer {
        cppio::Result<std::size_t> write_some(std::span<const std::byte>) override {
            return cppio::make_unexpected<std::size_t>(
                cppio::IoError{cppio::IoError::Code::no_space});
        }
        cppio::Result<void> flush() override { return {}; }
    } writer;

    std::vector<std::byte> scratch(8);
    auto res = cppio::copy_all(br, writer, std::span<std::byte>(scratch),
                               cppio::CopyLimit::unlimited(), nullptr);
    CPPIO_CHECK(!res.has_value());
    CPPIO_CHECK(res.error().code == cppio::IoError::Code::no_space);
    // Buffered bytes untouched by the failed write.
    CPPIO_CHECK(br.peek_buffered().size() == buffered_before);
}

CPPIO_TEST_CASE(copy_fast_path_does_not_duplicate_or_skip_bytes) {
    // Buffered head + scratch tail must concatenate in order with no gap/repeat.
    CountingReader inner("ABCDEFGHIJKLMNOP");  // 16 bytes
    std::vector<std::byte> rbuf(64);
    cppio::BufferedReader br(inner, rbuf);
    std::vector<std::byte> primed(3);
    (void)br.read_some(std::span<std::byte>(primed));  // 13 buffered remain

    cppio::MemoryWriter writer;
    std::vector<std::byte> scratch(4);  // small, forces multiple scratch rounds
    auto res = cppio::copy_all(br, writer, std::span<std::byte>(scratch),
                               cppio::CopyLimit::unlimited(), nullptr);
    CPPIO_CHECK(res.has_value());
    CPPIO_CHECK(res.value() == 13);
    CPPIO_CHECK(eq("DEFGHIJKLMNOP", writer.bytes()));
}

CPPIO_TEST_CASE(copy_scratch_path_still_works_when_no_buffered_bytes) {
    // A plain MemoryReader is NOT BufferedReadable: no fast path, classic scratch
    // loop. Verifies the fallback path is untouched.
    auto reader = cppio::MemoryReader::from_string("the quick brown fox");
    cppio::MemoryWriter writer;
    std::vector<std::byte> scratch(5);
    auto res = cppio::copy_all(reader, writer, std::span<std::byte>(scratch),
                               cppio::CopyLimit::unlimited(), nullptr);
    CPPIO_CHECK(res.has_value());
    CPPIO_CHECK(res.value() == 19);
    CPPIO_CHECK(eq("the quick brown fox", writer.bytes()));
}

CPPIO_TEST_CASE(copy_scratch_path_reader_error_propagates) {
    cppio::MemoryReader inner = cppio::MemoryReader::from_string("abcdef");
    cppio::FaultPlan plan;
    plan.fail_after_bytes = 3;
    plan.error = cppio::IoError{cppio::IoError::Code::canceled};
    cppio::FaultReader fr(inner, plan);
    cppio::MemoryWriter writer;
    std::vector<std::byte> scratch(2);
    auto res = cppio::copy_all(fr, writer, std::span<std::byte>(scratch),
                               cppio::CopyLimit::unlimited(), nullptr);
    CPPIO_CHECK(!res.has_value());
    CPPIO_CHECK(res.error().code == cppio::IoError::Code::canceled);
}

CPPIO_TEST_CASE(copy_scratch_path_writer_error_propagates) {
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
    auto res = cppio::copy_all(reader, writer, std::span<std::byte>(scratch),
                               cppio::CopyLimit::unlimited(), nullptr);
    CPPIO_CHECK(!res.has_value());
    CPPIO_CHECK(res.error().code == cppio::IoError::Code::no_space);
}

CPPIO_MAIN()
