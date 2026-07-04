// sluice observed wrappers — transparent stats-collecting Reader/Writer.
// They record counters on the way through but never alter data semantics.
#pragma once

#include <sluice/reader.hpp>
#include <sluice/writer.hpp>
#include <sluice/measurement.hpp>

#include <cstdint>

namespace sluice {

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
    // `vec_stats` is optional: when null, read_vec simply delegates without
    // counting. When set, read_vec on a non-overriding inner reader counts as a
    // fallback call (the default read_some loop runs).
    ObservedReader(Reader& inner, ReaderStats& stats, VectorStats* vec_stats = nullptr)
        : inner_(inner), stats_(stats), vec_stats_(vec_stats) {}

    // Not copyable or movable: holds references to the inner reader and the
    // caller-owned stats struct.
    ObservedReader(const ObservedReader&) = delete;
    ObservedReader& operator=(const ObservedReader&) = delete;
    ObservedReader(ObservedReader&&) = delete;
    ObservedReader& operator=(ObservedReader&&) = delete;

    Result<std::size_t> read_some(std::span<std::byte> dst) override;
    Result<std::size_t> read_vec(std::span<IoSlice> dsts) override;

  private:
    Reader& inner_;
    ReaderStats& stats_;
    VectorStats* vec_stats_;
};

class ObservedWriter final : public Writer {
  public:
    // `vec_stats` is optional: when null, write_vec simply delegates without
    // counting. When set, write_vec on a non-overriding inner writer counts as a
    // fallback call (the default write_some loop runs).
    ObservedWriter(Writer& inner, WriterStats& stats, VectorStats* vec_stats = nullptr)
        : inner_(inner), stats_(stats), vec_stats_(vec_stats) {}

    // Not copyable or movable: holds a reference to the inner writer and stats.
    ObservedWriter(const ObservedWriter&) = delete;
    ObservedWriter& operator=(const ObservedWriter&) = delete;
    ObservedWriter(ObservedWriter&&) = delete;
    ObservedWriter& operator=(ObservedWriter&&) = delete;

    Result<std::size_t> write_some(std::span<const std::byte> src) override;
    Result<std::size_t> write_vec(std::span<const ConstIoSlice> srcs) override;
    Result<void> flush() override;

  private:
    Writer& inner_;
    WriterStats& stats_;
    VectorStats* vec_stats_;
};

} // namespace sluice
