// cppio observed wrappers — transparent stats-collecting Reader/Writer.
// They record counters on the way through but never alter data semantics.
#pragma once

#include <cppio/reader.hpp>
#include <cppio/writer.hpp>

#include <cstdint>

namespace cppio {

struct ReaderStats {
    std::uint64_t read_calls = 0;
    std::uint64_t read_bytes = 0;
    std::uint64_t eof_count = 0;
    std::uint64_t read_errors = 0;
};

struct WriterStats {
    std::uint64_t write_calls = 0;
    std::uint64_t write_bytes = 0;
    std::uint64_t short_writes = 0;
    std::uint64_t write_errors = 0;
    std::uint64_t flush_calls = 0;
    std::uint64_t flush_errors = 0;
};

class ObservedReader final : public Reader {
public:
    ObservedReader(Reader& inner, ReaderStats& stats) : inner_(inner), stats_(stats) {}

    // Not copyable or movable: holds references to the inner reader and the
    // caller-owned stats struct.
    ObservedReader(const ObservedReader&) = delete;
    ObservedReader& operator=(const ObservedReader&) = delete;
    ObservedReader(ObservedReader&&) = delete;
    ObservedReader& operator=(ObservedReader&&) = delete;

    Result<std::size_t> read_some(std::span<std::byte> dst) override;

private:
    Reader& inner_;
    ReaderStats& stats_;
};

class ObservedWriter final : public Writer {
public:
    ObservedWriter(Writer& inner, WriterStats& stats) : inner_(inner), stats_(stats) {}

    // Not copyable or movable: holds references to the inner writer and stats.
    ObservedWriter(const ObservedWriter&) = delete;
    ObservedWriter& operator=(const ObservedWriter&) = delete;
    ObservedWriter(ObservedWriter&&) = delete;
    ObservedWriter& operator=(ObservedWriter&&) = delete;

    Result<std::size_t> write_some(std::span<const std::byte> src) override;
    Result<void> flush() override;

private:
    Writer& inner_;
    WriterStats& stats_;
};

}  // namespace cppio
