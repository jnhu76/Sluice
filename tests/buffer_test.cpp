// Tests for BufferedReader / BufferedWriter: order preservation, EOF handling,
// dirty-byte flushing, flush-error propagation, and reduction of inner calls.
// Backed by MemoryReader/MemoryWriter and Fault wrappers as the seams.
#include "harness.hpp"

#include <cppio/buffer.hpp>
#include <cppio/fault.hpp>
#include <cppio/measurement.hpp>
#include <cppio/reader.hpp>
#include <cppio/writer.hpp>

#include <cstddef>
#include <cstring>
#include <span>
#include <vector>

namespace {

bool eq(std::string_view s, const std::vector<std::byte>& b) {
    return b.size() == s.size() && std::memcmp(s.data(), b.data(), s.size()) == 0;
}
std::span<const std::byte> sb(std::string_view s) {
    return std::as_bytes(std::span(s.data(), s.size()));
}

// Reader that counts read_some calls — verifies buffering reduces them.
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
    cppio::MemoryWriter mem;
    int write_calls = 0;
    int flush_calls = 0;
    cppio::Result<std::size_t> write_some(std::span<const std::byte> src) override {
        ++write_calls;
        return mem.write_some(src);
    }
    cppio::Result<void> flush() override { ++flush_calls; return {}; }
};

}  // namespace

// ---------- BufferedReader ----------

CPPIO_TEST_CASE(buffered_reader_preserves_byte_order_across_small_reads) {
    CountingReader inner("abcdefghijklmnopqrstuvwxyz");
    std::vector<std::byte> backing(8);
    cppio::BufferedReader r(inner, std::span<std::byte>(backing));

    char out[26];
    for (int i = 0; i < 26; ++i) {
        auto res = r.read_some(std::as_writable_bytes(std::span(&out[i], 1)));
        CPPIO_CHECK(res.has_value());
        CPPIO_CHECK(res.value() == 1);
    }
    CPPIO_CHECK(std::string_view(out, 26) == "abcdefghijklmnopqrstuvwxyz");
}

CPPIO_TEST_CASE(buffered_reader_reduces_inner_read_calls) {
    CountingReader inner("abcdefghijklmnopqrstuvwxyz");
    std::vector<std::byte> backing(16);
    cppio::BufferedReader r(inner, std::span<std::byte>(backing));

    char out[26];
    for (int i = 0; i < 26; ++i) {
        (void)r.read_some(std::as_writable_bytes(std::span(&out[i], 1)));
    }
    // 26 bytes through a 16-byte buffer => at most 2 inner reads, not 26.
    CPPIO_CHECK(inner.calls <= 2);
}

CPPIO_TEST_CASE(buffered_reader_handles_eof_without_extra_bytes) {
    CountingReader inner("abc");
    std::vector<std::byte> backing(16);
    cppio::BufferedReader r(inner, std::span<std::byte>(backing));

    std::vector<std::byte> out(3);
    auto res = r.read_exact(std::span<std::byte>(out));
    CPPIO_CHECK(res.has_value());
    CPPIO_CHECK(eq("abc", out));

    // Reading past EOF returns 0 cleanly.
    std::array<std::byte, 4> more{};
    auto e = r.read_some(std::span<std::byte>(more));
    CPPIO_CHECK(e.has_value());
    CPPIO_CHECK(e.value() == 0);
}

CPPIO_TEST_CASE(buffered_reader_does_not_overwrite_unread_buffered_bytes) {
    // Read one byte, leaving bytes buffered; a second small read must still
    // return the next byte in order (no overread from inner).
    CountingReader inner("abcdef");
    std::vector<std::byte> backing(8);
    cppio::BufferedReader r(inner, std::span<std::byte>(backing));

    std::byte b0{};
    (void)r.read_some(std::span<std::byte>(&b0, 1));
    CPPIO_CHECK(std::to_integer<char>(b0) == 'a');

    std::byte b1{};
    (void)r.read_some(std::span<std::byte>(&b1, 1));
    CPPIO_CHECK(std::to_integer<char>(b1) == 'b');
    CPPIO_CHECK(inner.calls == 1);  // second byte came from the buffer
}

CPPIO_TEST_CASE(buffered_reader_can_read_more_than_buffer_size) {
    CountingReader inner("0123456789");
    std::vector<std::byte> backing(3);  // smaller than request
    cppio::BufferedReader r(inner, std::span<std::byte>(backing));

    std::vector<std::byte> out(10);
    auto res = r.read_exact(std::span<std::byte>(out));
    CPPIO_CHECK(res.has_value());
    CPPIO_CHECK(eq("0123456789", out));
}

// ---------- BufferedWriter ----------

CPPIO_TEST_CASE(buffered_writer_coalesces_small_writes_until_flush) {
    CountingWriter inner;
    std::vector<std::byte> backing(16);
    cppio::BufferedWriter w(inner, std::span<std::byte>(backing));

    CPPIO_CHECK(w.write_all(sb("ab")).has_value());
    CPPIO_CHECK(w.write_all(sb("cd")).has_value());
    CPPIO_CHECK(w.write_all(sb("ef")).has_value());
    // Nothing flushed yet — inner untouched.
    CPPIO_CHECK(inner.write_calls == 0);
    CPPIO_CHECK(inner.mem.bytes().empty());

    CPPIO_CHECK(w.flush().has_value());
    CPPIO_CHECK(eq("abcdef", inner.mem.bytes()));
    CPPIO_CHECK(inner.flush_calls == 1);
}

CPPIO_TEST_CASE(buffered_writer_preserves_order_through_short_writes) {
    // Force the inner writer to short-write; buffer must still emit in order.
    cppio::MemoryWriter sink;
    cppio::FaultPlan plan;
    plan.max_write_size = 3;
    cppio::FaultWriter inner(sink, plan);
    std::vector<std::byte> backing(4);
    cppio::BufferedWriter w(inner, std::span<std::byte>(backing));

    CPPIO_CHECK(w.write_all(sb("abcdefgh")).has_value());
    CPPIO_CHECK(w.flush().has_value());
    CPPIO_CHECK(eq("abcdefgh", sink.bytes()));
}

CPPIO_TEST_CASE(buffered_writer_flush_writes_all_dirty_bytes) {
    CountingWriter inner;
    std::vector<std::byte> backing(8);
    cppio::BufferedWriter w(inner, std::span<std::byte>(backing));

    CPPIO_CHECK(w.write_all(sb("hello")).has_value());
    CPPIO_CHECK(inner.mem.bytes().empty());
    CPPIO_CHECK(w.flush().has_value());
    CPPIO_CHECK(eq("hello", inner.mem.bytes()));
}

CPPIO_TEST_CASE(buffered_writer_large_write_bypasses_buffer_in_order) {
    // A write larger than the buffer should still reach the sink in order,
    // with buffered bytes flushed first.
    CountingWriter inner;
    std::vector<std::byte> backing(4);
    cppio::BufferedWriter w(inner, std::span<std::byte>(backing));

    CPPIO_CHECK(w.write_all(sb("AB")).has_value());  // buffered
    CPPIO_CHECK(w.write_all(sb("CDEFGHIJ")).has_value());  // larger than buffer
    CPPIO_CHECK(w.flush().has_value());
    CPPIO_CHECK(eq("ABCDEFGHIJ", inner.mem.bytes()));
}

CPPIO_TEST_CASE(buffered_writer_flush_error_propagates) {
    cppio::MemoryWriter sink;
    cppio::FaultPlan plan;
    plan.max_write_size = 3;          // inner can't drain 5 bytes in one call
    plan.fail_after_write_calls = 1;  // 2nd inner write call fails
    plan.error = cppio::IoError{cppio::IoError::Code::no_space};
    cppio::FaultWriter inner(sink, plan);
    std::vector<std::byte> backing(8);
    cppio::BufferedWriter w(inner, std::span<std::byte>(backing));

    CPPIO_CHECK(w.write_all(sb("hello")).has_value());  // still buffered
    auto res = w.flush();
    CPPIO_CHECK(!res.has_value());
    CPPIO_CHECK(res.error().code == cppio::IoError::Code::no_space);
}

CPPIO_TEST_CASE(buffered_writer_write_all_retries_across_short_inner_writes) {
    cppio::MemoryWriter sink;
    cppio::FaultPlan plan;
    plan.max_write_size = 2;  // every inner write limited to 2
    cppio::FaultWriter inner(sink, plan);
    std::vector<std::byte> backing(3);
    cppio::BufferedWriter w(inner, std::span<std::byte>(backing));

    CPPIO_CHECK(w.write_all(sb("wxyz")).has_value());
    CPPIO_CHECK(w.flush().has_value());
    CPPIO_CHECK(eq("wxyz", sink.bytes()));
}

// Release-safe precondition: an empty backing buffer must produce a clean
// invalid_state error from the operations, NOT undefined behavior. The
// constructor's assert() is debug-only and aborts in dev builds; the runtime
// guard inside read_some/write_some is the safety net that must hold in
// release builds. These tests are only meaningful under NDEBUG, where the
// assert is compiled out.
#ifdef NDEBUG
CPPIO_TEST_CASE(buffered_reader_rejects_empty_buffer_at_runtime) {
    CountingReader inner("abc");
    std::vector<std::byte> empty;
    cppio::BufferedReader r(inner, std::span<std::byte>(empty));
    std::array<std::byte, 4> out{};
    auto res = r.read_some(std::span<std::byte>(out));
    CPPIO_CHECK(!res.has_value());
    CPPIO_CHECK(res.error().code == cppio::IoError::Code::invalid_state);
}

CPPIO_TEST_CASE(buffered_writer_rejects_empty_buffer_at_runtime) {
    CountingWriter inner;
    std::vector<std::byte> empty;
    cppio::BufferedWriter w(inner, std::span<std::byte>(empty));
    auto res = w.write_some(sb("data"));
    CPPIO_CHECK(!res.has_value());
    CPPIO_CHECK(res.error().code == cppio::IoError::Code::invalid_state);
    CPPIO_CHECK(inner.write_calls == 0);  // never reached the inner writer
}
#endif

// BufferedWriter's destructor asserts (debug-only) that no dirty bytes remain,
// catching the common "forgot to flush" misuse. This test proves the positive
// contract: a flushed writer destroys cleanly. The negative contract (unflushed
// dirty bytes abort in debug builds) is intentionally not asserted here —
// triggering it would abort the test process, and it's debug-only so it can't
// be observed under NDEBUG. Manual verification: construct a BufferedWriter,
// write_all without flush, let it go out of scope in a debug build -> SIGABRT.
CPPIO_TEST_CASE(buffered_writer_destroys_cleanly_after_flush) {
    CountingWriter inner;
    std::vector<std::byte> backing(8);
    {
        cppio::BufferedWriter w(inner, std::span<std::byte>(backing));
        CPPIO_CHECK(w.write_all(sb("hello")).has_value());
        CPPIO_CHECK(w.flush().has_value());  // end_ back to 0 before scope exit
    }  // ~BufferedWriter asserts end_ == 0; reaching here proves the contract
    CPPIO_CHECK(eq("hello", inner.mem.bytes()));
}

// ---------- BufferStats attachment (CPPIO-CORE-004) ----------

CPPIO_TEST_CASE(buffered_reader_records_hit_then_miss_then_refill) {
    // Small buffer forces a refill once buffered bytes are exhausted.
    CountingReader inner("abcdefgh");
    cppio::BufferStats stats{};
    std::vector<std::byte> backing(4);
    cppio::BufferedReader r(inner, std::span<std::byte>(backing), &stats);

    // First read: 1 byte served from a fresh refill (miss -> refill).
    std::byte b{};
    CPPIO_CHECK(r.read_some(std::span<std::byte>(&b, 1)).has_value());
    CPPIO_CHECK(stats.read_requests >= 1);
    CPPIO_CHECK(stats.read_refill_calls >= 1);
    CPPIO_CHECK(stats.read_refill_bytes == 4);  // filled the 4-byte buffer

    // Second byte: served from the buffer (hit), no new refill.
    CPPIO_CHECK(r.read_some(std::span<std::byte>(&b, 1)).has_value());
    CPPIO_CHECK(stats.read_buffer_hits >= 1);
    CPPIO_CHECK(stats.read_refill_calls == 1);  // unchanged
}

CPPIO_TEST_CASE(buffered_writer_records_buffered_then_flush) {
    CountingWriter inner;
    cppio::BufferStats stats{};
    std::vector<std::byte> backing(16);
    cppio::BufferedWriter w(inner, std::span<std::byte>(backing), &stats);

    CPPIO_CHECK(w.write_all(sb("hello")).has_value());  // fits in buffer
    CPPIO_CHECK(stats.write_buffered_calls == 1);
    CPPIO_CHECK(stats.write_buffered_bytes == 5);
    CPPIO_CHECK(stats.write_flush_calls == 0);

    CPPIO_CHECK(w.flush().has_value());
    CPPIO_CHECK(stats.write_flush_calls == 1);
    CPPIO_CHECK(stats.write_flush_bytes == 5);
}

CPPIO_TEST_CASE(buffered_writer_large_write_goes_direct) {
    CountingWriter inner;
    cppio::BufferStats stats{};
    std::vector<std::byte> backing(4);
    cppio::BufferedWriter w(inner, std::span<std::byte>(backing), &stats);

    // A write larger than the buffer bypasses into a direct inner write.
    CPPIO_CHECK(w.write_all(sb("CDEFGHIJ")).has_value());
    CPPIO_CHECK(stats.write_direct_calls >= 1);
    CPPIO_CHECK(stats.write_direct_bytes >= 1);
}

CPPIO_TEST_CASE(buffer_stats_null_is_zero_overhead) {
    CountingReader inner("abc");
    std::vector<std::byte> backing(4);
    cppio::BufferedReader r(inner, std::span<std::byte>(backing), nullptr);
    std::array<std::byte, 3> out{};
    CPPIO_CHECK(r.read_exact(std::span<std::byte>(out)).has_value());  // no crash
}

CPPIO_MAIN()
