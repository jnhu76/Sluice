// Tests for Reader::read_vec_all (CPPIO-CORE-015A) — the exact-read-over-slices
// helper, symmetric to Writer::write_all_vec. It fills every non-empty slice
// completely (retrying the same slice on a short read, unlike read_vec which
// stops) or returns an error.
#include "harness.hpp"

#include <sluice/fault.hpp>
#include <sluice/reader.hpp>
#include <sluice/iovec.hpp>

#include <cstring>
#include <span>
#include <vector>

namespace {

sluice::IoSlice mslice(std::span<std::byte> b) { return sluice::IoSlice{b}; }

// A reader that caps each read_some to k per call, forcing multiple short reads.
class ShortReader final : public sluice::Reader {
public:
    sluice::MemoryReader mem;
    std::size_t cap;
    int calls = 0;
    ShortReader(std::string_view s, std::size_t per_call)
        : mem(sluice::MemoryReader::from_string(s)), cap(per_call) {}
    sluice::Result<std::size_t> read_some(std::span<std::byte> dst) override {
        ++calls;
        std::size_t n = std::min(dst.size(), cap);
        // read into a sub-view so MemoryReader honors the cap.
        auto r = mem.read_some(dst.first(n));
        return r;
    }
};

// A reader that errors after a configured number of bytes have been read.
class ErrorAfterReader final : public sluice::Reader {
public:
    sluice::MemoryReader mem;
    std::size_t consumed = 0;
    std::size_t trigger;
    sluice::IoError err;
    ErrorAfterReader(std::string_view s, std::size_t t, sluice::IoError e)
        : mem(sluice::MemoryReader::from_string(s)), trigger(t), err(e) {}
    sluice::Result<std::size_t> read_some(std::span<std::byte> dst) override {
        if (consumed >= trigger) return sluice::make_unexpected<std::size_t>(err);
        auto r = mem.read_some(dst);
        if (r.has_value()) consumed += r.value();
        return r;
    }
};

}  // namespace

SLUICE_TEST_CASE(read_vec_all_fills_all_buffers_in_one_call) {
    auto rd = sluice::MemoryReader::from_string("hello world!!");  // 13 bytes
    std::vector<std::byte> a(5), b(5), c(3);
    std::array<sluice::IoSlice, 3> dsts = {mslice(a), mslice(b), mslice(c)};
    auto r = rd.read_vec_all(std::span<sluice::IoSlice>(dsts));
    SLUICE_CHECK(r.has_value());
    SLUICE_CHECK(std::memcmp(a.data(), "hello", 5) == 0);
    SLUICE_CHECK(std::memcmp(b.data(), " worl", 5) == 0);
    SLUICE_CHECK(std::memcmp(c.data(), "d!!", 3) == 0);
}

SLUICE_TEST_CASE(read_vec_all_fills_across_multiple_short_reads) {
    // cap=2 forces many short reads; read_vec_all must keep filling the SAME
    // slice until complete, unlike read_vec which stops on a short read.
    ShortReader rd("ABCDEFGHIJ", 2);  // 10 bytes, 2 per read
    std::vector<std::byte> a(4), b(6);
    std::array<sluice::IoSlice, 2> dsts = {mslice(a), mslice(b)};
    auto r = rd.read_vec_all(std::span<sluice::IoSlice>(dsts));
    SLUICE_CHECK(r.has_value());
    SLUICE_CHECK(std::memcmp(a.data(), "ABCD", 4) == 0);
    SLUICE_CHECK(std::memcmp(b.data(), "EFGHIJ", 6) == 0);
    SLUICE_CHECK(rd.calls >= 5);  // at least 5 reads of 2
}

SLUICE_TEST_CASE(read_vec_all_eof_before_completion_returns_eof) {
    // Only 3 bytes available for two 4-byte slices: EOF mid-way -> eof error.
    auto rd = sluice::MemoryReader::from_string("abc");
    std::vector<std::byte> a(4), b(4);
    std::array<sluice::IoSlice, 2> dsts = {mslice(a), mslice(b)};
    auto r = rd.read_vec_all(std::span<sluice::IoSlice>(dsts));
    SLUICE_CHECK(!r.has_value());
    SLUICE_CHECK(r.error().code == sluice::IoError::Code::eof);
    // First slice got the 3 available bytes (partial progress is observable).
    SLUICE_CHECK(std::memcmp(a.data(), "abc", 3) == 0);
}

SLUICE_TEST_CASE(read_vec_all_zero_length_vector_is_success) {
    auto rd = sluice::MemoryReader::from_string("data");
    std::array<sluice::IoSlice, 0> dsts{};
    auto r = rd.read_vec_all(std::span<sluice::IoSlice>(dsts));
    SLUICE_CHECK(r.has_value());
}

SLUICE_TEST_CASE(read_vec_all_all_empty_slices_is_success) {
    auto rd = sluice::MemoryReader::from_string("data");
    sluice::IoSlice empty{std::span<std::byte>{}};
    std::array<sluice::IoSlice, 2> dsts = {empty, empty};
    auto r = rd.read_vec_all(std::span<sluice::IoSlice>(dsts));
    SLUICE_CHECK(r.has_value());
    // No bytes consumed from the reader.
    SLUICE_CHECK(rd.remaining() == 4);
}

SLUICE_TEST_CASE(read_vec_all_propagates_error_after_partial_progress) {
    // Error after 3 bytes consumed, but we ask for 8: error fires mid-fill.
    ErrorAfterReader rd("ABCDEFGH", /*trigger=*/3,
                        sluice::IoError{sluice::IoError::Code::canceled});
    std::vector<std::byte> a(4), b(4);
    std::array<sluice::IoSlice, 2> dsts = {mslice(a), mslice(b)};
    auto r = rd.read_vec_all(std::span<sluice::IoSlice>(dsts));
    SLUICE_CHECK(!r.has_value());
    SLUICE_CHECK(r.error().code == sluice::IoError::Code::canceled);
    // Partial progress was made before the error (observable in buffer a).
    SLUICE_CHECK(std::memcmp(a.data(), "ABC", 3) == 0);
}

SLUICE_TEST_CASE(read_vec_all_defensive_check_reader_returning_more_than_asked) {
    // A broken reader that returns more bytes than the buffer has room for
    // must trigger the defensive invalid_state check rather than corrupting.
    struct OverReader final : sluice::Reader {
        sluice::Result<std::size_t> read_some(std::span<std::byte> dst) override {
            return dst.size() + 1;  // Return more than asked (broken)
        }
    };
    OverReader rd;
    std::vector<std::byte> a(4);
    std::array<sluice::IoSlice, 1> dsts = {mslice(a)};
    auto r = rd.read_vec_all(std::span<sluice::IoSlice>(dsts));
    SLUICE_CHECK(!r.has_value());
    SLUICE_CHECK(r.error().code == sluice::IoError::Code::invalid_state);
}

SLUICE_MAIN()
