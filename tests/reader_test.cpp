// Tests for Reader::read_exact and Reader::stream_to through the public Reader
// interface, using a scripted in-memory test double as the seam.
#include "harness.hpp"

#include <sluice/reader.hpp>
#include <sluice/writer.hpp>
#include <sluice/result.hpp>
#include <sluice/error.hpp>
#include <sluice/limit.hpp>

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
class ScriptedReader final : public sluice::Reader {
public:
    std::vector<std::byte> payload;
    std::size_t pos = 0;
    std::optional<std::size_t> max_per_read;
    std::optional<sluice::IoError> err;  // returned instead of next data once set

    sluice::Result<std::size_t> read_some(std::span<std::byte> dst) override {
        // If an error is armed, fire when the trigger position is reached. If
        // the author armed err but forgot to set a position, fire immediately
        // rather than silently never triggering (which would be a false pass).
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
    std::optional<std::size_t> err_trigger_pos;
};

class ScriptedWriter final : public sluice::Writer {
public:
    std::vector<std::byte> sink;
    sluice::Result<std::size_t> write_some(std::span<const std::byte> src) override {
        sink.insert(sink.end(), src.begin(), src.end());
        return src.size();
    }
    sluice::Result<void> flush() override { return {}; }
};

std::vector<std::byte> bytes_of(std::string_view s) {
    auto* p = reinterpret_cast<const std::byte*>(s.data());
    return {p, p + s.size()};
}

}  // namespace

SLUICE_TEST_CASE(read_exact_fills_buffer_when_enough_data) {
    ScriptedReader r;
    r.payload = bytes_of("hello world");
    std::vector<std::byte> out(5);
    auto res = r.read_exact(std::span<std::byte>(out));
    SLUICE_CHECK(res.has_value());
    SLUICE_CHECK(std::memcmp(out.data(), "hello", 5) == 0);
}

SLUICE_TEST_CASE(read_exact_returns_eof_when_source_too_short) {
    ScriptedReader r;
    r.payload = bytes_of("hi");  // only 2 bytes
    std::vector<std::byte> out(5);
    auto res = r.read_exact(std::span<std::byte>(out));
    SLUICE_CHECK(!res.has_value());
    SLUICE_CHECK(res.error().code == sluice::IoError::Code::eof);
}

SLUICE_TEST_CASE(read_exact_zero_length_is_success) {
    ScriptedReader r;
    std::vector<std::byte> out;
    auto res = r.read_exact(std::span<std::byte>(out));
    SLUICE_CHECK(res.has_value());
}

SLUICE_TEST_CASE(read_exact_assembles_short_reads) {
    ScriptedReader r;
    r.payload = bytes_of("abcdef");
    r.max_per_read = 2;
    std::vector<std::byte> out(6);
    auto res = r.read_exact(std::span<std::byte>(out));
    SLUICE_CHECK(res.has_value());
    SLUICE_CHECK(std::memcmp(out.data(), "abcdef", 6) == 0);
}

SLUICE_TEST_CASE(read_exact_propagates_read_error) {
    ScriptedReader r;
    r.payload = bytes_of("abcdef");
    r.max_per_read = 1;  // force partial reads so pos walks through the trigger
    r.err = sluice::IoError{sluice::IoError::Code::backend_error};
    r.err_trigger_pos = 2;
    std::vector<std::byte> out(6);
    auto res = r.read_exact(std::span<std::byte>(out));
    SLUICE_CHECK(!res.has_value());
    SLUICE_CHECK(res.error().code == sluice::IoError::Code::backend_error);
}

SLUICE_TEST_CASE(stream_to_copies_all_bytes_until_eof) {
    ScriptedReader r;
    r.payload = bytes_of("stream me end to end");
    ScriptedWriter w;
    auto res = r.stream_to(w);
    SLUICE_CHECK(res.has_value());
    SLUICE_CHECK(res.value() == r.payload.size());
    SLUICE_CHECK(w.sink.size() == r.payload.size());
    SLUICE_CHECK(std::memcmp(w.sink.data(), r.payload.data(), r.payload.size()) == 0);
}

SLUICE_TEST_CASE(stream_to_propagates_reader_error) {
    ScriptedReader r;
    r.payload = bytes_of("abcdef");
    r.max_per_read = 1;  // force pos to reach the trigger across reads
    r.err = sluice::IoError{sluice::IoError::Code::canceled};
    r.err_trigger_pos = 2;
    ScriptedWriter w;
    auto res = r.stream_to(w);
    SLUICE_CHECK(!res.has_value());
    SLUICE_CHECK(res.error().code == sluice::IoError::Code::canceled);
}

SLUICE_TEST_CASE(stream_to_propagates_writer_error) {
    ScriptedReader r;
    r.payload = bytes_of("abcdef");
    // Writer that fails after first write.
    struct FailW final : sluice::Writer {
        int calls = 0;
        sluice::Result<std::size_t> write_some(std::span<const std::byte>) override {
            if (calls++ == 0) return std::size_t{3};
            return sluice::make_unexpected<std::size_t>(sluice::IoError{sluice::IoError::Code::no_space});
        }
        sluice::Result<void> flush() override { return {}; }
    } w;
    auto res = r.stream_to(w);
    SLUICE_CHECK(!res.has_value());
    SLUICE_CHECK(res.error().code == sluice::IoError::Code::no_space);
}

// ---------- limited Reader::stream_to (delegates to copy_all) ----------

SLUICE_TEST_CASE(stream_to_limited_bytes_copies_only_n) {
    ScriptedReader r;
    r.payload = bytes_of("abcdef");
    ScriptedWriter w;
    std::vector<std::byte> scratch(16);
    auto res = r.stream_to(w, std::span<std::byte>(scratch), sluice::CopyLimit::bytes(3));
    SLUICE_CHECK(res.has_value());
    SLUICE_CHECK(res.value() == 3);
    SLUICE_CHECK(w.sink.size() == 3);
    SLUICE_CHECK(std::memcmp(w.sink.data(), "abc", 3) == 0);
}

SLUICE_TEST_CASE(stream_to_nothing_copies_zero) {
    ScriptedReader r;
    r.payload = bytes_of("abcdef");
    ScriptedWriter w;
    std::vector<std::byte> scratch(16);
    auto res = r.stream_to(w, std::span<std::byte>(scratch), sluice::CopyLimit::nothing());
    SLUICE_CHECK(res.has_value());
    SLUICE_CHECK(res.value() == 0);
    SLUICE_CHECK(w.sink.empty());
}

SLUICE_TEST_CASE(stream_to_unlimited_matches_unbounded_behavior) {
    ScriptedReader r;
    r.payload = bytes_of("abcdef");
    ScriptedWriter w;
    std::vector<std::byte> scratch(16);
    auto res = r.stream_to(w, std::span<std::byte>(scratch), sluice::CopyLimit::unlimited());
    SLUICE_CHECK(res.has_value());
    SLUICE_CHECK(res.value() == 6);
    SLUICE_CHECK(w.sink.size() == 6);
    SLUICE_CHECK(std::memcmp(w.sink.data(), "abcdef", 6) == 0);
}

SLUICE_TEST_CASE(stream_to_convenience_overload_without_scratch) {
    ScriptedReader r;
    r.payload = bytes_of("abcdef");
    ScriptedWriter w;
    auto res = r.stream_to(w, sluice::CopyLimit::bytes(3));
    SLUICE_CHECK(res.has_value());
    SLUICE_CHECK(res.value() == 3);
}

SLUICE_MAIN()
