// Tests for Writer::write_vec / write_all_vec default fallback behavior, through
// the public Writer interface using a scripted test double. These verify the
// *behavior* of the default (non-POSIX) vector path: in-order writes, empty
// skipping, short-write handling, error propagation, and write_all_vec's
// retry/zero-progress rules.
#include "harness.hpp"

#include <cppio/writer.hpp>
#include <cppio/iovec.hpp>
#include <cppio/result.hpp>
#include <cppio/error.hpp>

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <optional>
#include <span>
#include <vector>

namespace {

// Records each write_some payload verbatim into `sink`; returns a canned
// response per call from `steps` (bytes accepted / optional error). This is the
// same seam style as writer_test.cpp's ScriptedWriter, exposing the default
// fallback path: write_vec on this writer must drive it through write_some.
class ScriptedWriter final : public cppio::Writer {
public:
    struct Step {
        std::size_t accept;
        std::optional<cppio::IoError> err;
    };
    std::vector<Step> steps;
    std::vector<std::byte> sink;
    // When true and `steps` is exhausted, write_some accepts all remaining
    // bytes instead of erroring. Lets a test seed a few short-write steps and
    // then drain the rest without hand-counting every remaining byte.
    bool accept_all_when_empty = false;

    cppio::Result<std::size_t> write_some(std::span<const std::byte> src) override {
        if (steps.empty()) {
            if (accept_all_when_empty) {
                sink.insert(sink.end(), src.begin(), src.end());
                return src.size();
            }
            return cppio::make_unexpected<std::size_t>(
                cppio::IoError{cppio::IoError::Code::invalid_state});
        }
        Step s = steps.front();
        steps.erase(steps.begin());
        if (s.err) return cppio::make_unexpected<std::size_t>(*s.err);
        std::size_t n = std::min(s.accept, src.size());
        sink.insert(sink.end(), src.begin(), src.begin() + n);
        return n;
    }
    cppio::Result<void> flush() override { return {}; }
};

cppio::ConstIoSlice slice_of(std::string_view s) {
    return cppio::ConstIoSlice{std::as_bytes(std::span(s.data(), s.size()))};
}

}  // namespace

CPPIO_TEST_CASE(write_vec_writes_slices_in_order) {
    ScriptedWriter w;
    // Two steps accepting everything in one call each.
    w.steps = {{100, std::nullopt}, {100, std::nullopt}};
    std::array<cppio::ConstIoSlice, 2> srcs = {slice_of("hello "), slice_of("world")};
    auto r = w.write_vec(std::span<const cppio::ConstIoSlice>(srcs));
    CPPIO_CHECK(r.has_value());
    CPPIO_CHECK(r.value() == 11);
    CPPIO_CHECK(w.sink.size() == 11);
    CPPIO_CHECK(std::memcmp(w.sink.data(), "hello world", 11) == 0);
}

CPPIO_TEST_CASE(write_vec_skips_empty_slices) {
    ScriptedWriter w;
    // An empty slice in the middle must be skipped without consuming a step,
    // so only two steps are needed for three slices.
    w.steps = {{100, std::nullopt}, {100, std::nullopt}};
    cppio::ConstIoSlice empty{std::span<const std::byte>{}};
    std::array<cppio::ConstIoSlice, 3> srcs = {slice_of("ab"), empty, slice_of("cd")};
    auto r = w.write_vec(std::span<const cppio::ConstIoSlice>(srcs));
    CPPIO_CHECK(r.has_value());
    CPPIO_CHECK(r.value() == 4);
    CPPIO_CHECK(w.sink.size() == 4);
    CPPIO_CHECK(std::memcmp(w.sink.data(), "abcd", 4) == 0);
}

CPPIO_TEST_CASE(write_vec_short_write_returns_partial_count) {
    ScriptedWriter w;
    // First slice only half-accepted -> short write -> stop, report 3.
    w.steps = {{3, std::nullopt}};
    std::array<cppio::ConstIoSlice, 2> srcs = {slice_of("hello"), slice_of("world")};
    auto r = w.write_vec(std::span<const cppio::ConstIoSlice>(srcs));
    CPPIO_CHECK(r.has_value());
    CPPIO_CHECK(r.value() == 3);
    CPPIO_CHECK(w.sink.size() == 3);
    CPPIO_CHECK(std::memcmp(w.sink.data(), "hel", 3) == 0);
}

CPPIO_TEST_CASE(write_vec_propagates_error_before_progress) {
    ScriptedWriter w;
    w.steps = {{0, cppio::IoError{cppio::IoError::Code::no_space}}};
    std::array<cppio::ConstIoSlice, 1> srcs = {slice_of("data")};
    auto r = w.write_vec(std::span<const cppio::ConstIoSlice>(srcs));
    CPPIO_CHECK(!r.has_value());
    CPPIO_CHECK(r.error().code == cppio::IoError::Code::no_space);
    CPPIO_CHECK(w.sink.empty());
}

CPPIO_TEST_CASE(write_vec_propagates_error_after_partial_progress) {
    // Slice 0 fully written, slice 1 errors -> error propagated even though
    // bytes were delivered (errors returned, not swallowed; matches write_all).
    ScriptedWriter w;
    w.steps = {{100, std::nullopt},
               {0, cppio::IoError{cppio::IoError::Code::no_space}}};
    std::array<cppio::ConstIoSlice, 2> srcs = {slice_of("ok"), slice_of("bad")};
    auto r = w.write_vec(std::span<const cppio::ConstIoSlice>(srcs));
    CPPIO_CHECK(!r.has_value());
    CPPIO_CHECK(r.error().code == cppio::IoError::Code::no_space);
    CPPIO_CHECK(w.sink.size() == 2);  // partial write happened before failure
}

CPPIO_TEST_CASE(write_vec_all_empty_slices_returns_zero) {
    ScriptedWriter w;
    cppio::ConstIoSlice empty{std::span<const std::byte>{}};
    std::array<cppio::ConstIoSlice, 2> srcs = {empty, empty};
    auto r = w.write_vec(std::span<const cppio::ConstIoSlice>(srcs));
    CPPIO_CHECK(r.has_value());
    CPPIO_CHECK(r.value() == 0);
    CPPIO_CHECK(w.sink.empty());
}

// ---------- write_all_vec ----------

CPPIO_TEST_CASE(write_all_vec_completes_through_short_writes_across_slices) {
    ScriptedWriter w;
    // Each slice gets short-written, then resumed. Enough steps to drain all
    // bytes across two slices without error.
    w.steps = {{3, std::nullopt}, {3, std::nullopt}, {3, std::nullopt}, {3, std::nullopt}};
    std::array<cppio::ConstIoSlice, 2> srcs = {slice_of("hello"), slice_of("world")};
    auto r = w.write_all_vec(std::span<const cppio::ConstIoSlice>(srcs));
    CPPIO_CHECK(r.has_value());
    CPPIO_CHECK(w.sink.size() == 10);
    CPPIO_CHECK(std::memcmp(w.sink.data(), "helloworld", 10) == 0);
}

CPPIO_TEST_CASE(write_all_vec_resumes_inside_a_partially_written_slice) {
    // write_vec reports a short write partway through a slice; write_all_vec
    // must re-issue the tail of that slice (not the whole slice, not skip it).
    ScriptedWriter w;
    // slice "abcdef" (6): accept 2, then 1, then 3. slice "xy" (2): accept 2.
    w.steps = {{2, std::nullopt}, {1, std::nullopt}, {3, std::nullopt}, {2, std::nullopt}};
    std::array<cppio::ConstIoSlice, 2> srcs = {slice_of("abcdef"), slice_of("xy")};
    auto r = w.write_all_vec(std::span<const cppio::ConstIoSlice>(srcs));
    CPPIO_CHECK(r.has_value());
    CPPIO_CHECK(w.sink.size() == 8);
    CPPIO_CHECK(std::memcmp(w.sink.data(), "abcdefxy", 8) == 0);
}

CPPIO_TEST_CASE(write_all_vec_rejects_zero_progress_on_non_empty_input) {
    ScriptedWriter w;
    // First write_vec call accepts nothing and reports no error -> 0 progress
    // while non-empty data remains -> invalid_state (no infinite loop).
    w.steps = {{0, std::nullopt}};
    std::array<cppio::ConstIoSlice, 1> srcs = {slice_of("data")};
    auto r = w.write_all_vec(std::span<const cppio::ConstIoSlice>(srcs));
    CPPIO_CHECK(!r.has_value());
    CPPIO_CHECK(r.error().code == cppio::IoError::Code::invalid_state);
}

CPPIO_TEST_CASE(write_all_vec_propagates_error) {
    ScriptedWriter w;
    // First slice fully written, second slice's first call errors.
    w.steps = {{100, std::nullopt},
               {0, cppio::IoError{cppio::IoError::Code::no_space}}};
    std::array<cppio::ConstIoSlice, 2> srcs = {slice_of("ok"), slice_of("bad")};
    auto r = w.write_all_vec(std::span<const cppio::ConstIoSlice>(srcs));
    CPPIO_CHECK(!r.has_value());
    CPPIO_CHECK(r.error().code == cppio::IoError::Code::no_space);
    CPPIO_CHECK(w.sink.size() == 2);  // partial write happened before failure
}

CPPIO_TEST_CASE(write_all_vec_empty_slices_is_success) {
    ScriptedWriter w;
    cppio::ConstIoSlice empty{std::span<const std::byte>{}};
    std::array<cppio::ConstIoSlice, 2> srcs = {empty, empty};
    auto r = w.write_all_vec(std::span<const cppio::ConstIoSlice>(srcs));
    CPPIO_CHECK(r.has_value());
    CPPIO_CHECK(w.sink.empty());
}

CPPIO_TEST_CASE(write_all_vec_never_skips_or_duplicates_bytes) {
    // Drive write_all_vec across many slices with several short writes landing
    // mid-slice and at slice boundaries (1,1,2,3 byte accepts), then let the
    // writer drain the rest. The sink must be the exact in-order concatenation:
    // no skipped byte, no duplicated byte.
    ScriptedWriter w;
    w.accept_all_when_empty = true;
    w.steps = {{1, std::nullopt}, {1, std::nullopt}, {2, std::nullopt},
               {3, std::nullopt}};
    std::array<cppio::ConstIoSlice, 5> srcs = {
        slice_of("AAAAA"), slice_of("BBBBB"), slice_of("CCCCC"),
        slice_of("DDDDD"), slice_of("EEEEE")};
    auto r = w.write_all_vec(std::span<const cppio::ConstIoSlice>(srcs));
    CPPIO_CHECK(r.has_value());
    CPPIO_CHECK(w.sink.size() == 25);
    CPPIO_CHECK(std::memcmp(w.sink.data(),
                            "AAAAABBBBBCCCCCDDDDDEEEEE", 25) == 0);
}

CPPIO_MAIN()
