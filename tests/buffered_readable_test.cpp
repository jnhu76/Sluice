// Tests for the BufferedReadable capability interface and BufferedReader's
// implementation of peek_buffered / consume_buffered. These verify the seam
// that copy_all's buffered fast path (006D) relies on.
#include "harness.hpp"

#include <sluice/buffer.hpp>
#include <sluice/buffered_readable.hpp>
#include <sluice/fault.hpp>
#include <sluice/reader.hpp>

#include <cstddef>
#include <cstring>
#include <span>
#include <vector>

namespace {

// Inner reader that counts read_some calls, so we can prove peek/consume never
// touch the inner reader.
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

} // namespace

SLUICE_TEST_CASE(buffered_readable_newly_constructed_has_empty_peek) {
    CountingReader inner("hello");
    std::vector<std::byte> buf(16);
    sluice::BufferedReader br(inner, buf);
    SLUICE_CHECK(br.peek_buffered().empty());
}

SLUICE_TEST_CASE(buffered_readable_exposes_leftover_after_partial_read) {
    // Read 2 bytes through the BufferedReader; the refill pulls more than 2
    // into the buffer, leaving unread bytes visible via peek_buffered().
    CountingReader inner("hello world");
    std::vector<std::byte> buf(16);
    sluice::BufferedReader br(inner, buf);

    std::vector<std::byte> out(2);
    auto r = br.read_some(std::span<std::byte>(out));
    SLUICE_CHECK(r.has_value());
    SLUICE_CHECK(r.value() == 2);
    SLUICE_CHECK(std::memcmp(out.data(), "he", 2) == 0);

    auto peeked = br.peek_buffered();
    SLUICE_CHECK(peeked.size() == 9); // "llo world"
    SLUICE_CHECK(std::memcmp(peeked.data(), "llo world", 9) == 0);
}

SLUICE_TEST_CASE(buffered_readable_consume_advances_cursor) {
    CountingReader inner("hello world");
    std::vector<std::byte> buf(16);
    sluice::BufferedReader br(inner, buf);

    std::vector<std::byte> out(2);
    (void)br.read_some(std::span<std::byte>(out)); // refill pulls all 11

    SLUICE_CHECK(br.peek_buffered().size() == 9);
    auto c = br.consume_buffered(3); // consume "llo"
    SLUICE_CHECK(c.has_value());
    auto peeked = br.peek_buffered();
    SLUICE_CHECK(peeked.size() == 6); // " world"
    SLUICE_CHECK(std::memcmp(peeked.data(), " world", 6) == 0);

    // The next read_some serves from the (now advanced) buffer, in order.
    std::vector<std::byte> out2(6);
    auto r2 = br.read_some(std::span<std::byte>(out2));
    SLUICE_CHECK(r2.has_value());
    SLUICE_CHECK(r2.value() == 6);
    SLUICE_CHECK(std::memcmp(out2.data(), " world", 6) == 0);
}

SLUICE_TEST_CASE(buffered_readable_consume_too_many_returns_invalid_state) {
    CountingReader inner("hello");
    std::vector<std::byte> buf(16);
    sluice::BufferedReader br(inner, buf);
    std::vector<std::byte> out(1);
    (void)br.read_some(std::span<std::byte>(out)); // refill pulls all 5
    std::size_t buffered = br.peek_buffered().size();
    SLUICE_CHECK(buffered > 0);

    auto c = br.consume_buffered(buffered + 1);
    SLUICE_CHECK(!c.has_value());
    SLUICE_CHECK(c.error().code == sluice::IoError::Code::invalid_state);
    // State unchanged on the rejected consume.
    SLUICE_CHECK(br.peek_buffered().size() == buffered);
}

SLUICE_TEST_CASE(buffered_readable_peek_does_not_call_inner_reader) {
    CountingReader inner("hello world");
    std::vector<std::byte> buf(16);
    sluice::BufferedReader br(inner, buf);
    std::vector<std::byte> out(2);
    (void)br.read_some(std::span<std::byte>(out));
    int calls_before = inner.calls;

    (void)br.peek_buffered();
    (void)br.peek_buffered();
    (void)br.peek_buffered();
    SLUICE_CHECK(inner.calls == calls_before); // peek never touches inner
}

SLUICE_TEST_CASE(buffered_readable_consume_does_not_call_inner_reader) {
    CountingReader inner("hello world");
    std::vector<std::byte> buf(16);
    sluice::BufferedReader br(inner, buf);
    std::vector<std::byte> out(2);
    (void)br.read_some(std::span<std::byte>(out));
    int calls_before = inner.calls;

    SLUICE_CHECK(br.consume_buffered(3).has_value());
    SLUICE_CHECK(br.consume_buffered(2).has_value());
    SLUICE_CHECK(inner.calls == calls_before); // consume never touches inner
}

SLUICE_TEST_CASE(buffered_readable_is_detectable_via_dynamic_cast) {
    // copy_all (006D) detects the fast path via dynamic_cast<BufferedReadable*>.
    // A BufferedReader is one; a plain Reader (MemoryReader) is not. The casts
    // go through a Reader& base so the compiler cannot statically elide them.
    CountingReader inner("hello");
    std::vector<std::byte> buf(16);
    sluice::BufferedReader br(inner, buf);
    sluice::MemoryReader mr;
    sluice::Reader& br_as_reader = br;
    sluice::Reader& mr_as_reader = mr;

    auto* br_cap = dynamic_cast<sluice::BufferedReadable*>(&br_as_reader);
    auto* mr_cap = dynamic_cast<sluice::BufferedReadable*>(&mr_as_reader);
    SLUICE_CHECK(br_cap != nullptr);
    SLUICE_CHECK(mr_cap == nullptr); // capability not present on plain reader
}

SLUICE_TEST_CASE(buffered_readable_consume_zero_on_non_empty_buffer_succeeds) {
    CountingReader inner("hello");
    std::vector<std::byte> buf(16);
    sluice::BufferedReader br(inner, buf);
    std::vector<std::byte> out(2);
    (void)br.read_some(std::span<std::byte>(out));
    std::size_t before = br.peek_buffered().size();
    // consume_buffered(0) is a no-op: nothing consumed, no error.
    auto c = br.consume_buffered(0);
    SLUICE_CHECK(c.has_value());
    SLUICE_CHECK(br.peek_buffered().size() == before);
}

SLUICE_TEST_CASE(buffered_readable_consume_exactly_available_empties_peek) {
    CountingReader inner("hello");
    std::vector<std::byte> buf(16);
    sluice::BufferedReader br(inner, buf);
    std::vector<std::byte> out(1);
    (void)br.read_some(std::span<std::byte>(out));
    std::size_t n = br.peek_buffered().size();
    SLUICE_CHECK(n > 0);
    auto c = br.consume_buffered(n);
    SLUICE_CHECK(c.has_value());
    SLUICE_CHECK(br.peek_buffered().empty());
}

SLUICE_TEST_CASE(buffered_readable_peek_on_already_empty_buffer_returns_empty) {
    CountingReader inner("a");
    std::vector<std::byte> buf(16);
    sluice::BufferedReader br(inner, buf);
    std::vector<std::byte> out(16); // read all
    (void)br.read_some(std::span<std::byte>(out));
    // Buffer is now empty: no unread bytes.
    auto p = br.peek_buffered();
    SLUICE_CHECK(p.empty());
}

SLUICE_TEST_CASE(buffered_readable_read_after_partial_consume_serves_remaining_bytes) {
    // After consuming part of the buffer, a read_some serves from the unread
    // portion (not from the start of the physical buffer, not a new refill).
    CountingReader inner("hello world");
    std::vector<std::byte> buf(16);
    sluice::BufferedReader br(inner, buf);
    std::vector<std::byte> out(1);
    (void)br.read_some(std::span<std::byte>(out)); // refill pulls all 11
    // peek shows 10, consume 3 ("ell"), then read_some serves "o worl" (6).
    SLUICE_CHECK(br.peek_buffered().size() == 10);
    (void)br.consume_buffered(3);
    std::vector<std::byte> out2(6);
    auto r2 = br.read_some(std::span<std::byte>(out2));
    SLUICE_CHECK(r2.has_value() && r2.value() == 6);
    SLUICE_CHECK(std::memcmp(out2.data(), "o worl", 6) == 0);
}

SLUICE_MAIN()
