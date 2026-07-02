// Tests for Writer::write_all behavior: short writes, zero-progress, error propagation.
// The seam is a scripted test double (no mocking framework) that records calls
// and returns canned Result values. These verify behavior through the public
// Writer interface, not any particular Writer implementation.
#include "harness.hpp"

#include <cppio/writer.hpp>
#include <cppio/result.hpp>
#include <cppio/error.hpp>

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <optional>
#include <span>
#include <vector>

namespace {

// Records each write_some payload; returns scripted response per call.
class ScriptedWriter final : public cppio::Writer {
public:
    // Sequence of (bytes_to_accept, optional error). Consumed in order.
    struct Step {
        std::size_t accept;
        std::optional<cppio::IoError> err;
    };
    std::vector<Step> steps;
    std::vector<std::byte> sink;
    std::size_t flush_calls = 0;
    std::optional<cppio::IoError> flush_err;

    cppio::Result<std::size_t> write_some(std::span<const std::byte> src) override {
        if (steps.empty()) {
            return cppio::make_unexpected<std::size_t>(cppio::IoError{cppio::IoError::Code::invalid_state});
        }
        Step s = steps.front();
        steps.erase(steps.begin());
        if (s.err) return cppio::make_unexpected<std::size_t>(*s.err);
        std::size_t n = std::min(s.accept, src.size());
        sink.insert(sink.end(), src.begin(), src.begin() + n);
        return n;
    }
    cppio::Result<void> flush() override {
        ++flush_calls;
        if (flush_err) return cppio::make_unexpected(*flush_err);
        return {};
    }
};

std::span<const std::byte> as_bytes(std::string_view s) {
    return std::as_bytes(std::span(s.data(), s.size()));
}

}  // namespace

CPPIO_TEST_CASE(write_all_writes_everything_in_one_call) {
    ScriptedWriter w;
    w.steps = {{100, std::nullopt}};
    auto r = w.write_all(as_bytes("hello"));
    CPPIO_CHECK(r.has_value());
    CPPIO_CHECK(w.sink.size() == 5);
    CPPIO_CHECK(std::memcmp(w.sink.data(), "hello", 5) == 0);
}

CPPIO_TEST_CASE(write_all_retries_across_short_writes) {
    ScriptedWriter w;
    w.steps = {{2, std::nullopt}, {2, std::nullopt}, {2, std::nullopt}};
    auto r = w.write_all(as_bytes("abcdef"));
    CPPIO_CHECK(r.has_value());
    CPPIO_CHECK(w.sink.size() == 6);
    CPPIO_CHECK(std::memcmp(w.sink.data(), "abcdef", 6) == 0);
}

CPPIO_TEST_CASE(write_all_rejects_zero_progress_on_non_empty_input) {
    ScriptedWriter w;
    w.steps = {{0, std::nullopt}};  // accepts nothing but no error
    auto r = w.write_all(as_bytes("data"));
    CPPIO_CHECK(!r.has_value());
    CPPIO_CHECK(r.error().code == cppio::IoError::Code::invalid_state);
}

CPPIO_TEST_CASE(write_all_rejects_zero_progress_after_partial_write) {
    // A partial write (2 bytes) followed by a zero-progress call must still be
    // rejected as invalid_state, not loop forever. The 2 partial bytes remain
    // delivered (write_all does not roll back on failure).
    ScriptedWriter w;
    w.steps = {{2, std::nullopt}, {0, std::nullopt}};
    auto r = w.write_all(as_bytes("data"));
    CPPIO_CHECK(!r.has_value());
    CPPIO_CHECK(r.error().code == cppio::IoError::Code::invalid_state);
    CPPIO_CHECK(w.sink.size() == 2);  // partial write happened before failure
}

CPPIO_TEST_CASE(write_all_empty_input_is_success_without_calls) {
    ScriptedWriter w;
    // No steps queued: if write_all wrongly called write_some it would error.
    auto r = w.write_all({});
    CPPIO_CHECK(r.has_value());
    CPPIO_CHECK(w.sink.empty());
}

CPPIO_TEST_CASE(write_all_propagates_write_some_error) {
    ScriptedWriter w;
    w.steps = {{2, std::nullopt}, {0, cppio::IoError{cppio::IoError::Code::no_space}}};
    auto r = w.write_all(as_bytes("abcdef"));
    CPPIO_CHECK(!r.has_value());
    CPPIO_CHECK(r.error().code == cppio::IoError::Code::no_space);
    CPPIO_CHECK(w.sink.size() == 2);  // partial write happened before failure
}

CPPIO_TEST_CASE(flush_propagates_error) {
    ScriptedWriter w;
    w.flush_err = cppio::IoError{cppio::IoError::Code::backend_error};
    auto r = w.flush();
    CPPIO_CHECK(!r.has_value());
    CPPIO_CHECK(r.error().code == cppio::IoError::Code::backend_error);
    CPPIO_CHECK(w.flush_calls == 1);
}

CPPIO_TEST_CASE(flush_succeeds_when_clean) {
    ScriptedWriter w;
    auto r = w.flush();
    CPPIO_CHECK(r.has_value());
    CPPIO_CHECK(w.flush_calls == 1);
}

CPPIO_MAIN()
