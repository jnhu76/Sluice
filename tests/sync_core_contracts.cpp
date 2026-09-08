#include <sluice/buffer.hpp>
#include <sluice/copy.hpp>
#include <sluice/limit.hpp>
#include <sluice/reader.hpp>
#include <sluice/result.hpp>
#include <sluice/writer.hpp>

#include "test_harness.hpp"

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <string>
#include <vector>

namespace {

using sluice::CopyLimit;
using sluice::IoSlice;
using sluice::Reader;
using sluice::Result;
using sluice::Writer;

std::string sync_pattern_bytes(std::size_t count) {
    std::string bytes(count, '\0');
    for (std::size_t i = 0; i < count; ++i) {
        bytes[i] = static_cast<char>((i * 13 + 11) & 0xFF);
    }
    return bytes;
}

class SequenceReader final : public Reader {
  public:
    explicit SequenceReader(std::string data) : data_(std::move(data)) {}

    Result<std::size_t> read_some(std::span<std::byte> destination) override {
        if (position_ >= data_.size()) {
            return static_cast<std::size_t>(0);
        }
        const std::size_t chunk = std::min(destination.size(), data_.size() - position_);
        std::memcpy(destination.data(), data_.data() + position_, chunk);
        position_ += chunk;
        return chunk;
    }

  private:
    std::string data_;
    std::size_t position_ = 0;
};

class CollectingWriter final : public Writer {
  public:
    explicit CollectingWriter(std::size_t max_bytes_per_write)
        : max_bytes_per_write_(max_bytes_per_write) {}

    Result<std::size_t> write_some(std::span<const std::byte> source) override {
        const std::size_t chunk = std::min(source.size(), max_bytes_per_write_);
        const std::byte* begin = source.data();
        collected_.insert(collected_.end(), begin, begin + chunk);
        return chunk;
    }

    Result<void> flush() override { return {}; }

    const std::vector<std::byte>& collected() const noexcept { return collected_; }

  private:
    std::size_t max_bytes_per_write_;
    std::vector<std::byte> collected_;
};

std::vector<std::byte> to_bytes(const std::string& text) {
    const std::byte* begin = reinterpret_cast<const std::byte*>(text.data());
    return {begin, begin + text.size()};
}

SLUICE_TEST(reader_read_exact_reports_eof_at_stream_end) {
    const std::string content = sync_pattern_bytes(100);
    SequenceReader reader(content);

    std::vector<std::byte> exact(content.size());
    auto exact_result = reader.read_exact(exact);
    SLUICE_CHECK(exact_result.has_value());
    SLUICE_CHECK(std::memcmp(exact.data(), content.data(), content.size()) == 0);

    std::vector<std::byte> beyond(8);
    auto beyond_result = reader.read_exact(beyond);
    SLUICE_CHECK(!beyond_result.has_value());
    SLUICE_CHECK(beyond_result.error().code == sluice::IoError::Code::eof);
}

SLUICE_TEST(writer_write_all_splits_short_writes) {
    const std::string content = sync_pattern_bytes(1000);
    CollectingWriter writer(7);

    const std::vector<std::byte> payload = to_bytes(content);
    auto written = writer.write_all(payload);
    SLUICE_CHECK(written.has_value());
    SLUICE_CHECK(writer.collected().size() == content.size());
    SLUICE_CHECK(std::memcmp(writer.collected().data(), content.data(), content.size()) == 0);
}

SLUICE_TEST(copy_all_transfers_all_bytes_via_scratch_path) {
    const std::string content = sync_pattern_bytes(4096);
    SequenceReader reader(content);
    CollectingWriter writer(content.size());

    std::vector<std::byte> scratch(256);
    auto copied = sluice::copy_all(reader, writer, scratch);
    SLUICE_CHECK(copied.has_value());
    SLUICE_CHECK(copied.value() == content.size());
    SLUICE_CHECK(std::memcmp(writer.collected().data(), content.data(), content.size()) == 0);
}

SLUICE_TEST(copy_all_respects_byte_limit) {
    const std::string content = sync_pattern_bytes(4096);
    SequenceReader reader(content);
    CollectingWriter writer(content.size());

    std::vector<std::byte> scratch(256);
    auto copied = sluice::copy_all(reader, writer, scratch, CopyLimit::bytes(1000));
    SLUICE_CHECK(copied.has_value());
    SLUICE_CHECK(copied.value() == 1000);
    SLUICE_CHECK(writer.collected().size() == 1000);
}

SLUICE_TEST(copy_all_transfers_all_bytes_via_buffered_reader) {
    const std::string content = sync_pattern_bytes(8192);
    SequenceReader inner(content);
    std::vector<std::byte> buffer(512);
    sluice::BufferedReader reader(inner, buffer);
    CollectingWriter writer(content.size());

    std::vector<std::byte> scratch(256);
    auto copied = sluice::copy_all(reader, writer, scratch);
    SLUICE_CHECK(copied.has_value());
    SLUICE_CHECK(copied.value() == content.size());
    SLUICE_CHECK(std::memcmp(writer.collected().data(), content.data(), content.size()) == 0);
}

SLUICE_TEST(buffered_reader_serves_and_consumes_buffered_bytes) {
    const std::string content = sync_pattern_bytes(64);
    SequenceReader inner(content);
    std::vector<std::byte> buffer(16);
    sluice::BufferedReader reader(inner, buffer);

    std::vector<std::byte> first(4);
    auto first_read = reader.read_some(first);
    SLUICE_CHECK(first_read.has_value());
    SLUICE_CHECK(first_read.value() == 4);

    auto peeked = reader.peek_buffered();
    SLUICE_CHECK(peeked.size() >= 1);

    std::vector<std::byte> compare(peeked.size());
    auto next_read = reader.read_some(compare);
    SLUICE_CHECK(next_read.has_value());
    SLUICE_CHECK(next_read.value() == peeked.size());
    SLUICE_CHECK(std::memcmp(compare.data(), peeked.data(), peeked.size()) == 0);
}

SLUICE_TEST(buffered_writer_defers_writes_until_flush_or_full_buffer) {
    const std::string first_part = sync_pattern_bytes(16);
    const std::string tail_part = sync_pattern_bytes(1);

    CollectingWriter inner(first_part.size() + tail_part.size());
    std::vector<std::byte> buffer(16);
    sluice::BufferedWriter writer(inner, buffer);

    auto first_written = writer.write_some(to_bytes(first_part));
    SLUICE_CHECK(first_written.has_value());
    SLUICE_CHECK(first_written.value() == first_part.size());
    SLUICE_CHECK(inner.collected().empty());

    auto tail_written = writer.write_some(to_bytes(tail_part));
    SLUICE_CHECK(tail_written.has_value());
    SLUICE_CHECK(tail_written.value() == tail_part.size());
    SLUICE_CHECK(inner.collected().size() == first_part.size());
    SLUICE_CHECK(std::memcmp(inner.collected().data(), first_part.data(), first_part.size()) == 0);

    auto flushed = writer.flush();
    SLUICE_CHECK(flushed.has_value());

    const std::string expected = first_part + tail_part;
    SLUICE_CHECK(inner.collected().size() == expected.size());
    SLUICE_CHECK(std::memcmp(inner.collected().data(), expected.data(), expected.size()) == 0);
}

}

SLUICE_TEST_MAIN()
