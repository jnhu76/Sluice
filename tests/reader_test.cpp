// Tests for Reader::read_exact and Reader::stream_to through the public Reader
// interface, using a scripted in-memory test double as the seam.
#include "harness.hpp"

#include <cppio/reader.hpp>
#include <cppio/writer.hpp>
#include <cppio/result.hpp>
#include <cppio/error.hpp>
#include <cppio/limit.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <span>
#include <vector>

namespace {

// Hand-rolled scripted reader: serves a fixed payload, optionally with a cap
// per read and an injected error after some bytes are consumed.
class ScriptedReader final : public cppio::Reader {
public:
    std::vector<std::byte> payload;
    std::size_t pos = 0;
    std::optional<std::size_t> max_per_read;
    std::optional<cppio::IoError> err;  // returned instead of next data once set

    cppio::Result<std::size_t> read_some(std::span<std::byte> dst) override {
        if (err && pos >= err_trigger_pos) return cppio::make_unexpected<std::size_t>(*err);
        if (pos >= payload.size()) return std::size_t{0};  // EOF
        std::size_t avail = payload.size() - pos;
        std::size_t n = std::min(dst.size(), avail);
        if (max_per_read) n = std::min(n, *max_per_read);
        std::memcpy(dst.data(), payload.data() + pos, n);
        pos += n;
        return n;
    }
    std::size_t err_trigger_pos = std::size_t(-1);
};

class ScriptedWriter final : public cppio::Writer {
public:
    std::vector<std::byte> sink;
    cppio::Result<std::size_t> write_some(std::span<const std::byte> src) override {
        sink.insert(sink.end(), src.begin(), src.end());
        return src.size();
    }
    cppio::Result<void> flush() override { return {}; }
};

std::vector<std::byte> bytes_of(std::string_view s) {
    auto* p = reinterpret_cast<const std::byte*>(s.data());
    return {p, p + s.size()};
}

}  // namespace

CPPIO_TEST_CASE(read_exact_fills_buffer_when_enough_data) {
    ScriptedReader r;
    r.payload = bytes_of("hello world");
    std::vector<std::byte> out(5);
    auto res = r.read_exact(std::span<std::byte>(out));
    CPPIO_CHECK(res.has_value());
    CPPIO_CHECK(std::memcmp(out.data(), "hello", 5) == 0);
}

CPPIO_TEST_CASE(read_exact_returns_eof_when_source_too_short) {
    ScriptedReader r;
    r.payload = bytes_of("hi");  // only 2 bytes
    std::vector<std::byte> out(5);
    auto res = r.read_exact(std::span<std::byte>(out));
    CPPIO_CHECK(!res.has_value());
    CPPIO_CHECK(res.error().code == cppio::IoError::Code::eof);
}

CPPIO_TEST_CASE(read_exact_zero_length_is_success) {
    ScriptedReader r;
    std::vector<std::byte> out;
    auto res = r.read_exact(std::span<std::byte>(out));
    CPPIO_CHECK(res.has_value());
}

CPPIO_TEST_CASE(read_exact_assembles_short_reads) {
    ScriptedReader r;
    r.payload = bytes_of("abcdef");
    r.max_per_read = 2;
    std::vector<std::byte> out(6);
    auto res = r.read_exact(std::span<std::byte>(out));
    CPPIO_CHECK(res.has_value());
    CPPIO_CHECK(std::memcmp(out.data(), "abcdef", 6) == 0);
}

CPPIO_TEST_CASE(read_exact_propagates_read_error) {
    ScriptedReader r;
    r.payload = bytes_of("abcdef");
    r.max_per_read = 1;  // force partial reads so pos walks through the trigger
    r.err = cppio::IoError{cppio::IoError::Code::backend_error};
    r.err_trigger_pos = 2;
    std::vector<std::byte> out(6);
    auto res = r.read_exact(std::span<std::byte>(out));
    CPPIO_CHECK(!res.has_value());
    CPPIO_CHECK(res.error().code == cppio::IoError::Code::backend_error);
}

CPPIO_TEST_CASE(stream_to_copies_all_bytes_until_eof) {
    ScriptedReader r;
    r.payload = bytes_of("stream me end to end");
    ScriptedWriter w;
    auto res = r.stream_to(w);
    CPPIO_CHECK(res.has_value());
    CPPIO_CHECK(res.value() == r.payload.size());
    CPPIO_CHECK(w.sink.size() == r.payload.size());
    CPPIO_CHECK(std::memcmp(w.sink.data(), r.payload.data(), r.payload.size()) == 0);
}

CPPIO_TEST_CASE(stream_to_propagates_reader_error) {
    ScriptedReader r;
    r.payload = bytes_of("abcdef");
    r.max_per_read = 1;  // force pos to reach the trigger across reads
    r.err = cppio::IoError{cppio::IoError::Code::canceled};
    r.err_trigger_pos = 2;
    ScriptedWriter w;
    auto res = r.stream_to(w);
    CPPIO_CHECK(!res.has_value());
    CPPIO_CHECK(res.error().code == cppio::IoError::Code::canceled);
}

CPPIO_TEST_CASE(stream_to_propagates_writer_error) {
    ScriptedReader r;
    r.payload = bytes_of("abcdef");
    // Writer that fails after first write.
    struct FailW final : cppio::Writer {
        int calls = 0;
        cppio::Result<std::size_t> write_some(std::span<const std::byte>) override {
            if (calls++ == 0) return std::size_t{3};
            return cppio::make_unexpected<std::size_t>(cppio::IoError{cppio::IoError::Code::no_space});
        }
        cppio::Result<void> flush() override { return {}; }
    } w;
    auto res = r.stream_to(w);
    CPPIO_CHECK(!res.has_value());
    CPPIO_CHECK(res.error().code == cppio::IoError::Code::no_space);
}

// ---------- limited Reader::stream_to (delegates to copy_all) ----------

CPPIO_TEST_CASE(stream_to_limited_bytes_copies_only_n) {
    ScriptedReader r;
    r.payload = bytes_of("abcdef");
    ScriptedWriter w;
    std::vector<std::byte> scratch(16);
    auto res = r.stream_to(w, std::span<std::byte>(scratch), cppio::CopyLimit::bytes(3));
    CPPIO_CHECK(res.has_value());
    CPPIO_CHECK(res.value() == 3);
    CPPIO_CHECK(w.sink.size() == 3);
    CPPIO_CHECK(std::memcmp(w.sink.data(), "abc", 3) == 0);
}

CPPIO_TEST_CASE(stream_to_nothing_copies_zero) {
    ScriptedReader r;
    r.payload = bytes_of("abcdef");
    ScriptedWriter w;
    std::vector<std::byte> scratch(16);
    auto res = r.stream_to(w, std::span<std::byte>(scratch), cppio::CopyLimit::nothing());
    CPPIO_CHECK(res.has_value());
    CPPIO_CHECK(res.value() == 0);
    CPPIO_CHECK(w.sink.empty());
}

CPPIO_TEST_CASE(stream_to_unlimited_matches_unbounded_behavior) {
    ScriptedReader r;
    r.payload = bytes_of("abcdef");
    ScriptedWriter w;
    std::vector<std::byte> scratch(16);
    auto res = r.stream_to(w, std::span<std::byte>(scratch), cppio::CopyLimit::unlimited());
    CPPIO_CHECK(res.has_value());
    CPPIO_CHECK(res.value() == 6);
    CPPIO_CHECK(w.sink.size() == 6);
    CPPIO_CHECK(std::memcmp(w.sink.data(), "abcdef", 6) == 0);
}

CPPIO_TEST_CASE(stream_to_convenience_overload_without_scratch) {
    ScriptedReader r;
    r.payload = bytes_of("abcdef");
    ScriptedWriter w;
    auto res = r.stream_to(w, cppio::CopyLimit::bytes(3));
    CPPIO_CHECK(res.has_value());
    CPPIO_CHECK(res.value() == 3);
}

CPPIO_MAIN()
