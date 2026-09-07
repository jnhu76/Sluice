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
    ObservedReader(Reader& inner, ReaderStats& stats, VectorStats* vec_stats = nullptr)
        : inner_(inner), stats_(stats), vec_stats_(vec_stats) {}

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
    ObservedWriter(Writer& inner, WriterStats& stats, VectorStats* vec_stats = nullptr)
        : inner_(inner), stats_(stats), vec_stats_(vec_stats) {}

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
