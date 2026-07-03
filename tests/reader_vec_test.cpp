// Tests for Reader::read_vec default fallback behavior through the public Reader
// interface, using a scripted in-memory test double. Verifies in-order fill,
// empty-slice skipping, EOF handling, error propagation, and short-read behavior.
#include "harness.hpp"

#include <sluice/reader.hpp>
#include <sluice/iovec.hpp>
#include <sluice/result.hpp>
#include <sluice/error.hpp>

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <optional>
#include <span>
#include <vector>

namespace {

// Serves a fixed payload, optionally with a per-read cap and an injected error.
// Mirrors reader_test.cpp's ScriptedReader so the vector tests reuse a known seam.
class ScriptedReader final : public sluice::Reader {
public:
    std::vector<std::byte> payload;
    std::size_t pos = 0;
    std::optional<std::size_t> max_per_read;
    std::optional<sluice::IoError> err;
    std::optional<std::size_t> err_trigger_pos;

    sluice::Result<std::size_t> read_some(std::span<std::byte> dst) override {
        if (err && (!err_trigger_pos.has_value() || pos >= *err_trigger_pos)) {
            return sluice::make_unexpected<std::size_t>(*err);
        }
        if (pos >= payload.size()) return std::size_t{0};  // EOF
        std::size_t avail = payload.size() - pos;
        std::size_t n = std::min(dst.size(), avail);
        if (max_per_read) n = std::min(n, *max_per_read);
        std::memcpy(dst.data(), payload.data() + pos, n);
        pos += n;
        return n;
    }
};

sluice::IoSlice mut_slice(std::span<std::byte> b) { return sluice::IoSlice{b}; }

std::vector<std::byte> bytes_of(std::string_view s) {
    auto* p = reinterpret_cast<const std::byte*>(s.data());
    return {p, p + s.size()};
}

}  // namespace

SLUICE_TEST_CASE(read_vec_fills_slices_in_order) {
    ScriptedReader r;
    r.payload = bytes_of("helloworld");
    std::vector<std::byte> a(5), b(5);
    std::array<sluice::IoSlice, 2> dsts = {mut_slice(a), mut_slice(b)};
    auto res = r.read_vec(std::span<sluice::IoSlice>(dsts));
    SLUICE_CHECK(res.has_value());
    SLUICE_CHECK(res.value() == 10);
    SLUICE_CHECK(std::memcmp(a.data(), "hello", 5) == 0);
    SLUICE_CHECK(std::memcmp(b.data(), "world", 5) == 0);
}

SLUICE_TEST_CASE(read_vec_skips_empty_slices) {
    ScriptedReader r;
    r.payload = bytes_of("abcd");
    std::vector<std::byte> a(2), b(2);
    sluice::IoSlice empty{std::span<std::byte>{}};
    std::array<sluice::IoSlice, 3> dsts = {mut_slice(a), empty, mut_slice(b)};
    auto res = r.read_vec(std::span<sluice::IoSlice>(dsts));
    SLUICE_CHECK(res.has_value());
    SLUICE_CHECK(res.value() == 4);
    SLUICE_CHECK(std::memcmp(a.data(), "ab", 2) == 0);
    SLUICE_CHECK(std::memcmp(b.data(), "cd", 2) == 0);
}

SLUICE_TEST_CASE(read_vec_eof_mid_stream_returns_bytes_read_so_far) {
    // Source runs out partway into the second slice. EOF returns the total read
    // before EOF, not an error.
    ScriptedReader r;
    r.payload = bytes_of("abc");  // only 3 bytes for two size-2 slices
    std::vector<std::byte> a(2), b(2);
    std::array<sluice::IoSlice, 2> dsts = {mut_slice(a), mut_slice(b)};
    auto res = r.read_vec(std::span<sluice::IoSlice>(dsts));
    SLUICE_CHECK(res.has_value());
    SLUICE_CHECK(res.value() == 3);
    SLUICE_CHECK(std::memcmp(a.data(), "ab", 2) == 0);
    SLUICE_CHECK(std::memcmp(b.data(), "c", 1) == 0);
}

SLUICE_TEST_CASE(read_vec_eof_before_any_bytes_returns_zero) {
    ScriptedReader r;  // empty payload => immediate EOF
    std::vector<std::byte> a(2);
    std::array<sluice::IoSlice, 1> dsts = {mut_slice(a)};
    auto res = r.read_vec(std::span<sluice::IoSlice>(dsts));
    SLUICE_CHECK(res.has_value());
    SLUICE_CHECK(res.value() == 0);
}

SLUICE_TEST_CASE(read_vec_propagates_error_before_progress) {
    ScriptedReader r;
    r.payload = bytes_of("abcdef");
    r.err = sluice::IoError{sluice::IoError::Code::backend_error};
    r.err_trigger_pos = 0;  // fire immediately, no bytes read
    std::vector<std::byte> a(4);
    std::array<sluice::IoSlice, 1> dsts = {mut_slice(a)};
    auto res = r.read_vec(std::span<sluice::IoSlice>(dsts));
    SLUICE_CHECK(!res.has_value());
    SLUICE_CHECK(res.error().code == sluice::IoError::Code::backend_error);
}

SLUICE_TEST_CASE(read_vec_propagates_error_after_partial_progress) {
    // Read 2 bytes into slice 0, then error on slice 1 -> error propagated even
    // though progress was made (errors returned, not swallowed; matches read_exact).
    ScriptedReader r;
    r.payload = bytes_of("abcdef");
    r.max_per_read = 2;  // force pos to advance so the trigger can fire
    r.err = sluice::IoError{sluice::IoError::Code::canceled};
    r.err_trigger_pos = 2;
    std::vector<std::byte> a(2), b(2);
    std::array<sluice::IoSlice, 2> dsts = {mut_slice(a), mut_slice(b)};
    auto res = r.read_vec(std::span<sluice::IoSlice>(dsts));
    SLUICE_CHECK(!res.has_value());
    SLUICE_CHECK(res.error().code == sluice::IoError::Code::canceled);
    SLUICE_CHECK(std::memcmp(a.data(), "ab", 2) == 0);  // first slice was filled
}

SLUICE_TEST_CASE(read_vec_stops_on_first_short_read_and_leaves_later_slices_untouched) {
    // max_per_read=2 means slice 0 (size 4) gets a short read of 2. A
    // conservative readv-style primitive must STOP there: it returns 2 and does
    // NOT touch slice 1 at all.
    ScriptedReader r;
    r.payload = bytes_of("abcdefgh");
    r.max_per_read = 2;
    std::vector<std::byte> a(4, std::byte{0xFF}), b(4, std::byte{0xFF});
    std::array<sluice::IoSlice, 2> dsts = {mut_slice(a), mut_slice(b)};
    auto res = r.read_vec(std::span<sluice::IoSlice>(dsts));
    SLUICE_CHECK(res.has_value());
    SLUICE_CHECK(res.value() == 2);  // stopped at the short read of slice 0
    SLUICE_CHECK(std::memcmp(a.data(), "ab", 2) == 0);
    // slice 1 untouched: still all 0xFF (never handed to read_some).
    for (auto x : b) SLUICE_CHECK(std::to_integer<int>(x) == 0xFF);
}

SLUICE_TEST_CASE(read_vec_all_empty_slices_returns_zero) {
    ScriptedReader r;
    r.payload = bytes_of("data");  // never touched
    sluice::IoSlice empty{std::span<std::byte>{}};
    std::array<sluice::IoSlice, 2> dsts = {empty, empty};
    auto res = r.read_vec(std::span<sluice::IoSlice>(dsts));
    SLUICE_CHECK(res.has_value());
    SLUICE_CHECK(res.value() == 0);
}

SLUICE_MAIN()
